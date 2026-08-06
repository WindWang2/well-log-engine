// Headless test for the core Table Projection (ADR 0022, #154 Phase A). Asserts:
// curves sharing a Sampling Axis form one wide Depth|Curve table; curves on
// different axes form separate tables (no implicit alignment); cells are read
// on demand from the RAW Curve buffer (not LOD), zero-copy; null samples (null
// bitmap or non-finite value) are a distinct empty cell; row count is the axis
// length; the document id/revision is carried for invalidation.

#include <welllog/core/document.hpp>
#include <welllog/table/table_projection.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void require_near(double actual, double expected, std::string_view message) {
  if (std::abs(actual - expected) > 1.0e-9) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return parsed.value();
}

const auto document_id = id("11111111-0000-4000-8000-000000000001");
const auto axis_a_id = id("11111111-0000-4000-8000-000000000002");
const auto axis_b_id = id("11111111-0000-4000-8000-000000000003");
const auto curve_gr_id = id("11111111-0000-4000-8000-000000000004");
const auto curve_rt_id = id("11111111-0000-4000-8000-000000000005");
const auto curve_rhob_id = id("11111111-0000-4000-8000-000000000006");

// A document with two curves on axis A (GR, RT) and one curve on axis B (RHOB).
// Axis A is the wide-table case; axis B must form its OWN table (no alignment
// to A).
WellLogDocument make_document() {
  auto depths_a = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.5, 1001.0, 1001.5});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  auto rt = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 4.0, 8.0});
  auto depths_b = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2000.0, 2050.0, 2100.0});
  auto rhob = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2.1, 2.3, 2.5});

  WellLogDocumentBuilder builder(document_id, DocumentRevision{7});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id,
      .coordinates = BufferView::from_vector(depths_a),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_b_id,
      .coordinates = BufferView::from_vector(depths_b),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  builder.add_curve(Curve{
      .id = curve_rt_id, .mnemonic = "RT", .display_name = "Resistivity",
      .unit = "ohm.m", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(rt), .nulls = {}});
  builder.add_curve(Curve{
      .id = curve_rhob_id, .mnemonic = "RHOB", .display_name = "Bulk Density",
      .unit = "g/cm3", .sampling_axis_id = axis_b_id,
      .values = BufferView::from_vector(rhob), .nulls = {}});
  return builder.build();
}

// --- Tests -------------------------------------------------------------------

// Same-axis wide table: GR + RT on axis A form ONE table with 3 columns
// (DEPTH, GR, RT) and 4 rows (axis A length). RHOB is on axis B → a SEPARATE
// table. So the document yields exactly 2 tables.
void same_axis_curves_form_one_wide_table() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  require(tables.size() == 2,
          "two Sampling Axes with curves must yield exactly 2 tables");

  // Find the axis-A table (3 columns: DEPTH + GR + RT).
  const TableProjection *axis_a_table = nullptr;
  for (const auto &t : tables) {
    if (t.sampling_axis_id() == axis_a_id) {
      axis_a_table = &t;
    }
  }
  require(axis_a_table != nullptr, "axis A must have a table");
  require(axis_a_table->kind() == TableKind::curves,
          "the axis-A table must be a curves table");
  require(axis_a_table->column_count() == 3,
          "axis-A table must have 3 columns (DEPTH + GR + RT)");
  require(axis_a_table->row_count() == 4,
          "axis-A table row count must equal the axis coordinate count (4)");

  // Column 0 = DEPTH (axis), columns 1/2 = GR/RT (curves, in document order).
  require(axis_a_table->column(0).curve_id.is_nil(),
          "column 0 must be the axis (depth) column (nil curve id)");
  require(axis_a_table->column(0).unit == "m",
          "depth column unit must be the axis unit");
  require(axis_a_table->column(1).curve_id == curve_gr_id &&
              axis_a_table->column(1).name == "GR",
          "column 1 must be GR");
  require(axis_a_table->column(2).curve_id == curve_rt_id &&
              axis_a_table->column(2).name == "RT",
          "column 2 must be RT");

  // Identity carried for invalidation.
  require(axis_a_table->document_id() == document_id,
          "table must carry the document id");
  require(axis_a_table->document_revision() == DocumentRevision{7},
          "table must carry the document revision");
}

// Different-axis separation: RHOB on axis B is its OWN table, never aligned to
// axis A. Its columns/rows reflect axis B alone (DEPTH + RHOB, 3 rows).
void different_axis_curves_form_separate_tables() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  const TableProjection *axis_b_table = nullptr;
  for (const auto &t : tables) {
    if (t.sampling_axis_id() == axis_b_id) {
      axis_b_table = &t;
    }
  }
  require(axis_b_table != nullptr, "axis B must have its own table");
  require(axis_b_table->column_count() == 2,
          "axis-B table must have 2 columns (DEPTH + RHOB)");
  require(axis_b_table->row_count() == 3,
          "axis-B table row count must equal axis B coordinate count (3)");
  require(axis_b_table->column(1).curve_id == curve_rhob_id,
          "axis-B column 1 must be RHOB");
  // No axis-A values leak into the axis-B table (no implicit alignment).
  for (std::uint64_t row = 0; row < axis_b_table->row_count(); ++row) {
    require_near(axis_b_table->cell(row, 0).value.value_or(-1.0),
                 std::vector<double>{2000.0, 2050.0, 2100.0}[row],
                 "axis-B depth column must read axis B coordinates, not A");
  }
}

// Cells are read on demand from the RAW buffer (not LOD). The first row of the
// axis-A table: DEPTH=1000.0, GR=10.0, RT=1.0 — exact source values.
void cells_read_raw_buffer_on_demand() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  const TableProjection *axis_a_table = nullptr;
  for (const auto &t : tables) {
    if (t.sampling_axis_id() == axis_a_id) {
      axis_a_table = &t;
    }
  }
  require(axis_a_table != nullptr, "axis A must have a table");
  // Row 0: exact source values (zero-copy raw read).
  require_near(axis_a_table->cell(0, 0).value.value_or(-1.0), 1000.0,
               "row 0 depth must be the raw axis coordinate");
  require_near(axis_a_table->cell(0, 1).value.value_or(-1.0), 10.0,
               "row 0 GR must be the raw curve value (not LOD)");
  require_near(axis_a_table->cell(0, 2).value.value_or(-1.0), 1.0,
               "row 0 RT must be the raw curve value");
  // Last row.
  require_near(axis_a_table->cell(3, 1).value.value_or(-1.0), 40.0,
               "row 3 GR must be the raw curve value");
}

// Null handling: a null bitmap bit OR a non-finite value yields a null cell
// (empty), never a sentinel or NaN text.
void null_samples_are_empty_cells() {
  // GR with a null bitmap marking sample 1, and RT with a NaN at sample 2.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  auto rt = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0,
                                    std::numeric_limits<double>::quiet_NaN(),
                                    8.0});
  auto nulls_owner = std::make_shared<std::vector<std::uint8_t>>(1, 0);
  // bit 1 set → sample 1 null.
  (*nulls_owner)[0] = 0b00000010;
  NullBitmapView gr_nulls = NullBitmapView::from_raw(
      nulls_owner->data(), 4, nulls_owner->size(), SharedOwner{nulls_owner});

  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = gr_nulls});
  builder.add_curve(Curve{
      .id = curve_rt_id, .mnemonic = "RT", .display_name = "Resistivity",
      .unit = "ohm.m", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(rt), .nulls = {}});

  const auto tables = TableProjectionBuilder::from_document(builder.build());
  require(tables.size() == 1, "one axis → one table");
  const auto &t = tables.front();
  // GR row 1 = null (bitmap), RT row 2 = null (NaN).
  require(t.cell(1, 1).null(), "GR row 1 must be null (null bitmap)");
  require(!t.cell(0, 1).null() && !t.cell(2, 1).null(),
          "GR rows 0/2 must NOT be null");
  require(t.cell(2, 2).null(), "RT row 2 must be null (non-finite NaN)");
  require(!t.cell(3, 2).null(), "RT row 3 must NOT be null");
}

// Out-of-range row/column yields a null cell (no throw) — virtualization safety
// for a host requesting a row beyond the count.
void out_of_range_cell_is_null_safely() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  const auto &t = tables.front();
  require(t.cell(t.row_count(), 0).null(),
          "row == row_count must be a null cell, not a throw");
  require(t.cell(0, t.column_count()).null(),
          "col == column_count must be a null cell, not a throw");
}

// Virtualization: row_count is O(1) and large-row-count documents do not
// materialize rows. A 1M-sample axis reports row_count 1_000_000 immediately;
// only requested cells are read.
void large_row_count_is_virtualized() {
  constexpr std::uint64_t n = 1'000'000;
  // Build mutable vectors, then share them (BufferView::from_vector takes a
  // shared_ptr<const vector>, so fill first, then move into a const shared ptr).
  auto depths_fill = std::make_shared<std::vector<double>>(n);
  auto gr_fill = std::make_shared<std::vector<double>>(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    (*depths_fill)[i] = 1000.0 + static_cast<double>(i) * 0.5;
    (*gr_fill)[i] = static_cast<double>(i);
  }
  auto depths = std::shared_ptr<const std::vector<double>>(std::move(depths_fill));
  auto gr = std::shared_ptr<const std::vector<double>>(std::move(gr_fill));
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  require(tables.size() == 1, "one axis → one table");
  const auto &t = tables.front();
  require(t.row_count() == n, "row_count must report 1_000_000");
  // Spot-read a few cells without materializing the rest.
  require_near(t.cell(0, 1).value.value_or(-1.0), 0.0, "first GR value");
  require_near(t.cell(n - 1, 1).value.value_or(-1.0),
               static_cast<double>(n - 1), "last GR value");
}

// A curve whose sampling_axis_id is not on the document is dropped — never
// implicitly aligned to another axis (ADR 0022 §2.2: no implicit join).
void unaligned_curve_is_dropped() {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  // GR aligned to axis A.
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_a_id, .values = BufferView::from_vector(gr),
      .nulls = {}});
  // RT references a NON-EXISTENT axis (axis_b_id not added) → dropped.
  auto rt = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  builder.add_curve(Curve{
      .id = curve_rt_id, .mnemonic = "RT", .display_name = "RT", .unit = "ohm",
      .sampling_axis_id = axis_b_id, .values = BufferView::from_vector(rt),
      .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  require(tables.size() == 1, "only the aligned axis produces a table");
  require(tables.front().column_count() == 2,
          "table must be DEPTH + GR only (RT dropped — no implicit alignment)");
}

// An empty document yields no tables.
void empty_document_yields_no_tables() {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  require(tables.empty(), "a document with no curves/axes yields no tables");
}

// Repeat / non-monotonic depths: the projection is a pure index projection —
// it does NOT assume monotonic or unique depths. An axis with repeated or
// decreasing coordinates yields one row per coordinate (no de-dup, no sort).
void repeat_and_non_monotonic_depths_keep_one_row_per_sample() {
  // Depths: [1000, 1000, 1001, 999] — a repeat (1000) and a decrease (999).
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.0, 1001.0, 999.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_a_id, .values = BufferView::from_vector(gr),
      .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  require(tables.size() == 1, "one axis → one table");
  const auto &t = tables.front();
  // No de-dup: 4 samples → 4 rows.
  require(t.row_count() == 4,
          "repeat/non-monotonic depths must keep one row per sample (no de-dup)");
  // Each row reads its own coordinate verbatim (index projection, no depth math).
  require_near(t.cell(0, 0).value.value_or(-1.0), 1000.0, "row 0 depth");
  require_near(t.cell(1, 0).value.value_or(-1.0), 1000.0,
               "row 1 depth (repeat kept)");
  require_near(t.cell(2, 0).value.value_or(-1.0), 1001.0, "row 2 depth");
  require_near(t.cell(3, 0).value.value_or(-1.0), 999.0,
               "row 3 depth (non-monotonic kept)");
}

} // namespace

int main() {
  same_axis_curves_form_one_wide_table();
  different_axis_curves_form_separate_tables();
  cells_read_raw_buffer_on_demand();
  null_samples_are_empty_cells();
  out_of_range_cell_is_null_safely();
  large_row_count_is_virtualized();
  unaligned_curve_is_dropped();
  empty_document_yields_no_tables();
  repeat_and_non_monotonic_depths_keep_one_row_per_sample();
  std::cout << "welllog.table-projection: all cases passed\n";
  return EXIT_SUCCESS;
}
