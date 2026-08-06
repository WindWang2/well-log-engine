#pragma once

// Virtualized Table Projection (ADR 0022, #154). A Qt-agnostic, immutable view
// over a WellLogDocument that partitions its curves by Sampling Axis into one
// CurveTable per axis (a `Depth | Curve...` wide table). Curves on DIFFERENT
// axes never align implicitly — there is no join by array index, float-depth
// approximate match, auto-interpolation, or Display Depth substitution
// (ADR 0022 §2.2). Only an explicit user choice produces a labelled Resampled
// Table (a later ticket). Phase A builds curve tables only; interval/marker/
// annotation tables are a tracked follow-up (the TableKind enum reserves their
// values).
//
// Table Projection is the data projection of a Document Revision: graphics use
// PreparedScene (LOD-reduced envelope points); the table reads the RAW curve
// BufferView straight from the document, never LOD points (table-and-export.md
// §4.2 "Copy reads the raw Buffer, not LOD"). Reads are on-demand per cell /
// row-block — no full QVariant/row matrix is materialized — so a million-row
// table is virtualized: the 64-bit row count is known up front but cells are
// fetched only when requested.
//
// A null sample — per the Curve's NullBitmapView, an out-of-range index, or a
// non-finite value — is a distinct empty-cell state (TableCell::null), never a
// sentinel string or NaN text. Identity (document id/revision, axis id, curve
// id) is carried so a host can invalidate on document replacement.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>
#include <welllog/table/export.hpp>

namespace welllog {

// The kind of a projected table (ADR 0022 §2.2). Phase A builds curve tables
// only (curves sharing a Sampling Axis form one Depth|Curve table per axis).
// Interval/marker/annotation tables are tracked follow-ups — the enum reserves
// their values so consumers can branch on kind without an ABI break later.
enum class TableKind : std::uint8_t {
  curves,
  intervals,   // reserved — not built in Phase A
  markers,     // reserved — not built in Phase A
  annotations, // reserved — not built in Phase A
};

// One column of a projected table. For a curve table the first column is the
// axis (depth) and each subsequent column is a curve; `curve_id` is nil for the
// axis column. `unit` is the column's value unit (axis unit for the depth
// column, curve unit otherwise). `scalar_type` is the raw buffer's element type
// (axis coordinate type for the depth column, curve value type otherwise) —
// exported writers use it to label columns (e.g. XML `type="float64"`).
struct TableColumn {
  EntityId curve_id{};      // nil for the axis/depth column of a curve table
  std::string name;         // mnemonic/display name (or axis label)
  std::string unit;         // value unit
  ScalarType scalar_type{ScalarType::float64};
  // Derived-curve provenance when this column is a Derived Curve (#159). Empty
  // for axis columns and raw source curves. Resampled/export paths reuse these
  // fields so table semantics match the document.
  std::optional<DerivedCurveProvenance> derived{};
  DerivedFreshness derived_freshness{DerivedFreshness::current};
};

// How table cells treat QC Mask states (#159). Defaults match graphics:
// invalid and user-excluded become null cells; suspect remains visible.
struct TableQcPolicy {
  bool nullify_suspect{false};
  bool nullify_invalid{true};
  bool nullify_user_excluded{true};
};

// One cell, read on demand from the raw buffer. `value` is nullopt for a null
// sample (null bitmap set, out-of-range index, non-finite value, or suppressed
// QC state) — the cell is empty, not a sentinel.
struct TableCell {
  std::optional<double> value;
  QcState qc_state{QcState::valid};
  [[nodiscard]] bool null() const noexcept { return !value.has_value(); }
};

// One projected table: a rectangular view over rows of columns. Immutable,
// cheap to copy (PIMPL, shared state). Cells are read on demand via
// cell(row, column); row_count() is O(1) and may be in the millions without
// any row materialization. Reads go straight to the document's raw BufferView
// (zero copy); the SharedOwner on each buffer keeps the source alive for the
// projection's lifetime.
class WELLLOG_TABLE_API TableProjection {
public:
  TableProjection();
  ~TableProjection();
  TableProjection(const TableProjection &);
  TableProjection &operator=(const TableProjection &);
  TableProjection(TableProjection &&) noexcept;
  TableProjection &operator=(TableProjection &&) noexcept;

  // The kind of this table (curves / intervals / markers / annotations).
  [[nodiscard]] TableKind kind() const noexcept;
  // The Sampling Axis this table is aligned to (nil for interval/marker/
  // annotation tables, which are not axis-aligned wide tables).
  [[nodiscard]] EntityId sampling_axis_id() const noexcept;
  // The document this projection was built from (identity + revision carried so
  // a host can invalidate on document replacement).
  [[nodiscard]] EntityId document_id() const noexcept;
  [[nodiscard]] DocumentRevision document_revision() const noexcept;

  // Column metadata. For a curve table: column 0 is the axis (depth) column,
  // columns 1..n are curves. Stable for the projection's lifetime.
  [[nodiscard]] std::uint64_t column_count() const noexcept;
  [[nodiscard]] TableColumn column(std::uint64_t index) const noexcept;

  // The number of rows. O(1); may be large (millions) with no materialization —
  // virtualization means rows are read on demand, not stored.
  [[nodiscard]] std::uint64_t row_count() const noexcept;

  // Reads one cell on demand from the raw buffer. Out-of-range row/column or a
  // null/non-finite sample yields a null cell (no throw). This is the
  // zero-copy raw read — never an LOD point.
  [[nodiscard]] TableCell cell(std::uint64_t row,
                               std::uint64_t column) const noexcept;

  // Returns a projection over the half-open [first_row, last_row) row span of
  // THIS projection, sharing its column metadata + identity (document id/
  // revision/axis) and reading the SAME raw buffer (zero copy). A row r in the
  // slice maps to row (first_row + r) in the source. The slice's row_count is
  // (last_row - first_row), clamped to the source's bounds; an empty or
  // out-of-range span yields a valid 0-row slice. Used by the table exporters
  // to export a Phase-B Selection Set range ("支持完整表或 Selection Set") —
  // slice the projection to the selection's [first_row,last_row) then export.
  [[nodiscard]] TableProjection slice(std::uint64_t first_row,
                                      std::uint64_t last_row) const noexcept;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class TableProjectionBuilder;
  explicit TableProjection(std::shared_ptr<const Impl> impl);
};

// Builds the set of Table Projections for a document. Phase A emits one curve
// table per Sampling Axis that has curves (grouped by sampling_axis_id; axes
// with no curves produce no table). Interval/marker/annotation tables are a
// tracked follow-up. Order: curve tables in first-seen axis order.
class WELLLOG_TABLE_API TableProjectionBuilder {
public:
  // Partitions the document into its projected tables. Each table shares the
  // document's id/revision for invalidation. `qc_policy` controls how QC Mask
  // states surface as null cells (#159).
  [[nodiscard]] static std::vector<TableProjection>
  from_document(const WellLogDocument &document,
                TableQcPolicy qc_policy = {}) noexcept;

private:
  // Builds one curve table for an axis + the curves sharing it. Column 0 is the
  // axis (depth) column; columns 1..n are the curves in document order. A member
  // (not a free function) so it can reach TableProjection's private ctor/Impl
  // via this class's friendship.
  [[nodiscard]] static TableProjection
  make_curve_table(const std::shared_ptr<const WellLogDocument> &document,
                   EntityId document_id, DocumentRevision revision,
                   const SamplingAxis &axis,
                   const std::vector<const Curve *> &curves,
                   TableQcPolicy qc_policy);
};

} // namespace welllog
