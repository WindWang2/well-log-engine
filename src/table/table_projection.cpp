// Virtualized Table Projection implementation (ADR 0022, #154). See header for
// the design. The projection partitions a WellLogDocument's curves by Sampling
// Axis into one CurveTable per axis, reading raw Curve::values straight from
// the document's BufferViews (zero copy — never LOD points). Interval, marker
// and annotation tables are flat per-row projections of their struct fields.

#include <welllog/table/table_projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>

namespace welllog {
namespace {

// The shared, immutable state of one projected table. Holds only metadata +
// references to the document's spans; cells are read on demand from the
// document buffers the spans point into (the document's SharedOwners keep that
// storage alive — the builder holds the document by value, retaining it).
struct CurveColumn {
  EntityId curve_id{};
  std::string name;
  std::string unit;
  ScalarType scalar_type{ScalarType::float64};
  // For a curve column: pointers into the document's spans (the curve + its
  // axis). For the axis (depth) column, curve is nullptr and the axis is set.
  const SamplingAxis *axis{};
  const Curve *curve{};
  std::optional<DerivedCurveProvenance> derived{};
  DerivedFreshness derived_freshness{DerivedFreshness::current};
};

} // namespace

// The PIMPL state of TableProjection. Defined at namespace scope (not anon) so
// the header's forward-declared `struct Impl` resolves to it.
struct TableProjection::Impl {
  TableKind kind{};
  EntityId sampling_axis_id{};
  EntityId document_id{};
  DocumentRevision document_revision{};
  // The visible row window over the source buffers: row r of THIS projection
  // maps to source row (row_offset + r). A source projection has row_offset 0
  // and row_count = axis length; a slice has row_offset = first_row and
  // row_count = (last_row - first_row).
  std::uint64_t row_offset{};
  std::uint64_t row_count{};
  std::vector<CurveColumn> columns;
  // Retains the source document for the projection's lifetime; the column
  // pointers reference spans inside it.
  std::shared_ptr<const WellLogDocument> document_holder;
  TableQcPolicy qc_policy{};
};

namespace {

#if defined(_MSC_VER)
#define WELLLOG_NOINLINE __declspec(noinline)
#else
#define WELLLOG_NOINLINE __attribute__((noinline))
#endif

// Copies the document so the projections keep it alive, and returns it as a
// shared owner. Kept in a separate, non-inlined function: GCC 16 -Warray-bounds
// misfires at -O3 when this shared_ptr control-block allocation is inlined into
// the same function that destroys TableProjection::Impl (a shared_ptr-holding
// PIMPL), fusing the 24-byte control block with Impl member accesses and
// reporting bogus out-of-bounds reads. Debug/-O2 and Clang are unaffected.
// (Not a real bounds bug — the false positive is the optimizer's, not the
// code's.)
WELLLOG_NOINLINE std::shared_ptr<WellLogDocument>
copy_document(const WellLogDocument &document) {
  return std::shared_ptr<WellLogDocument>(new WellLogDocument(document));
}

#undef WELLLOG_NOINLINE

// Reads a curve/axis cell straight from the raw buffer (zero copy). The curve
// values arrive as a CurveBuffer (single-block OR composite, #197); both
// expose value_as_double(index). Null when the index is out of range, the null
// bitmap is set, or the value is non-finite — matching the scene kernel's
// missing-sample rule (src/scene/scene.cpp ~1505). This is the "reads the raw
// Buffer, not LOD" path. Defined here (before cell()) so the accessor can call
// it.
TableCell read_buffer_cell(const CurveBuffer &values,
                           const NullBitmapView &nulls,
                           std::uint64_t row,
                           QcState qc = QcState::valid) noexcept {
  if (!nulls.empty() && nulls.is_null(row)) {
    return TableCell{.value = std::nullopt, .qc_state = qc};
  }
  const auto v = values.value_as_double(row);
  if (!v.has_value() || !std::isfinite(*v)) {
    return TableCell{.value = std::nullopt, .qc_state = qc};
  }
  return TableCell{.value = *v, .qc_state = qc};
}

} // namespace

TableProjection::TableProjection() = default;
TableProjection::~TableProjection() = default;
TableProjection::TableProjection(const TableProjection &) = default;
TableProjection &TableProjection::operator=(const TableProjection &) = default;
TableProjection::TableProjection(TableProjection &&) noexcept = default;
TableProjection &TableProjection::operator=(TableProjection &&) noexcept = default;

TableProjection::TableProjection(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

TableKind TableProjection::kind() const noexcept {
  return impl_ ? impl_->kind : TableKind::curves;
}
EntityId TableProjection::sampling_axis_id() const noexcept {
  return impl_ ? impl_->sampling_axis_id : EntityId{};
}
EntityId TableProjection::document_id() const noexcept {
  return impl_ ? impl_->document_id : EntityId{};
}
DocumentRevision TableProjection::document_revision() const noexcept {
  return impl_ ? impl_->document_revision : DocumentRevision{};
}
std::uint64_t TableProjection::column_count() const noexcept {
  return impl_ ? impl_->columns.size() : 0;
}
TableColumn TableProjection::column(std::uint64_t index) const noexcept {
  if (!impl_ || index >= impl_->columns.size()) {
    return {};
  }
  const auto &c = impl_->columns[index];
  return TableColumn{.curve_id = c.curve_id,
                     .name = c.name,
                     .unit = c.unit,
                     .scalar_type = c.scalar_type,
                     .derived = c.derived,
                     .derived_freshness = c.derived_freshness};
}
std::uint64_t TableProjection::row_count() const noexcept {
  return impl_ ? impl_->row_count : 0;
}

TableCell TableProjection::cell(std::uint64_t row,
                                std::uint64_t col) const noexcept {
  if (!impl_ || row >= impl_->row_count || col >= impl_->columns.size()) {
    return TableCell{std::nullopt};
  }
  // Map the projection row through the slice offset to the source row.
  const auto source_row = impl_->row_offset + row;
  const auto &column = impl_->columns[col];
  // make_curve_table sets `axis` on every column, so a built curves table never
  // has a null axis; the depth column is distinguished by `curve == nullptr`.
  // The kind guard below keeps this scoped to curves tables (the only kind
  // built in Phase A).
  if (impl_->kind == TableKind::curves) {
    if (column.curve == nullptr) {
      // Depth/axis column — reads the axis coordinates (always a single-block
      // BufferView; append extends curve values, not axis coordinates).
      return read_buffer_cell(CurveBuffer{column.axis->coordinates},
                              NullBitmapView{}, source_row);
    }
    const auto qc =
        impl_->document_holder
            ? qc_state_at(*impl_->document_holder, *column.curve, source_row)
            : QcState::valid;
    if (qc_state_is_suppressed(qc, impl_->qc_policy.nullify_suspect,
                               impl_->qc_policy.nullify_invalid,
                               impl_->qc_policy.nullify_user_excluded)) {
      return TableCell{.value = std::nullopt, .qc_state = qc};
    }
    return read_buffer_cell(column.curve->values, column.curve->nulls,
                            source_row, qc);
  }
  return TableCell{.value = std::nullopt};
}

TableProjection TableProjection::slice(std::uint64_t first_row,
                                       std::uint64_t last_row) const noexcept {
  // An empty/null source yields an empty slice.
  if (!impl_ || impl_->row_count == 0) {
    return TableProjection{};
  }
  // Clamp to [0, row_count]; normalize an inverted/empty span to a 0-row slice.
  const auto clamped_first = first_row >= impl_->row_count ? impl_->row_count : first_row;
  const auto clamped_last = last_row > impl_->row_count ? impl_->row_count : last_row;
  auto sliced = std::make_shared<Impl>(*impl_);
  sliced->row_offset = impl_->row_offset + clamped_first;
  sliced->row_count = clamped_last > clamped_first ? (clamped_last - clamped_first) : 0;
  return TableProjection{std::move(sliced)};
}

// Builds one curve table for an axis + the curves sharing it. A member of the
// builder (not a free function) so it can reach TableProjection's private
// ctor/Impl via this class's friendship. Column 0 is the axis (depth) column;
// columns 1..n are the curves in document order.
TableProjection TableProjectionBuilder::make_curve_table(
    const std::shared_ptr<const WellLogDocument> &document,
    EntityId document_id, DocumentRevision revision, const SamplingAxis &axis,
    const std::vector<const Curve *> &curves, TableQcPolicy qc_policy) {
  auto impl = std::make_shared<TableProjection::Impl>();
  impl->kind = TableKind::curves;
  impl->sampling_axis_id = axis.id;
  impl->document_id = document_id;
  impl->document_revision = revision;
  impl->document_holder = document;
  impl->qc_policy = qc_policy;
  // Row count = axis coordinate count (the alignment unit). Curves whose value
  // buffers differ in length still address by row index; out-of-range reads
  // yield null cells (read_buffer_cell).
  impl->row_count = axis.coordinates.length();
  // Depth/axis column.
  CurveColumn depth_col;
  depth_col.curve_id = EntityId{};
  depth_col.name = "DEPTH";
  depth_col.unit = axis.unit;
  depth_col.scalar_type = axis.coordinates.scalar_type();
  depth_col.axis = &axis;
  depth_col.curve = nullptr;
  impl->columns.push_back(std::move(depth_col));
  // One column per curve sharing this axis.
  for (const Curve *curve : curves) {
    CurveColumn cc;
    cc.curve_id = curve->id;
    cc.name = curve->mnemonic.empty() ? curve->display_name : curve->mnemonic;
    cc.unit = curve->unit;
    cc.scalar_type = curve->values.scalar_type();
    cc.axis = &axis;
    cc.curve = curve;
    if (curve->derived.has_value()) {
      cc.derived = curve->derived;
      cc.derived_freshness = curve->derived->freshness;
    }
    impl->columns.push_back(std::move(cc));
  }
  return TableProjection{std::move(impl)};
}

std::vector<TableProjection>
TableProjectionBuilder::from_document(const WellLogDocument &document,
                                      TableQcPolicy qc_policy) noexcept {
  // Hold the document alive for the projections' lifetime via a shared copy.
  // WellLogDocument is an immutable PIMPL value type (shared_ptr<const Impl>),
  // so copying is cheap and shares state.
  auto holder = copy_document(document);
  const auto doc_id = document.id();
  const auto revision = document.revision();

  // Group curves by sampling_axis_id, preserving per-axis document order, and
  // remember the order axes were first seen (curve tables are emitted in axis
  // order). A curve whose axis is not present on the document is skipped (it
  // cannot be aligned — no implicit join).
  const auto axes = document.sampling_axes();
  const auto curves = document.curves();
  std::unordered_map<EntityId, const SamplingAxis *, EntityIdHash> axis_by_id;
  for (const auto &axis : axes) {
    axis_by_id.emplace(axis.id, &axis);
  }
  std::vector<EntityId> axis_order;
  std::unordered_map<EntityId, std::vector<const Curve *>, EntityIdHash>
      curves_by_axis;
  for (const auto &curve : curves) {
    const auto it = axis_by_id.find(curve.sampling_axis_id);
    if (it == axis_by_id.end()) {
      continue; // unaligned curve — no implicit alignment (ADR 0022 §2.2)
    }
    if (curves_by_axis.find(curve.sampling_axis_id) == curves_by_axis.end()) {
      axis_order.push_back(curve.sampling_axis_id);
    }
    curves_by_axis[curve.sampling_axis_id].push_back(&curve);
  }

  std::vector<TableProjection> tables;
  // Curve tables, in axis order.
  for (const auto &axis_id : axis_order) {
    const auto *axis = axis_by_id.at(axis_id);
    tables.push_back(make_curve_table(holder, doc_id, revision, *axis,
                                      curves_by_axis[axis_id], qc_policy));
  }
  return tables;
}

} // namespace welllog
