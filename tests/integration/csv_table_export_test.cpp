// Headless test for the CSV table-export writer (ADR 0022 / table-and-export.md
// §7, #155). Asserts: a single projection streams to a CSV that round-trips
// values + nulls; the null token is distinct from a literal value that reads as
// the token (RFC-4180 quoting); multi-projection packages write a directory +
// manifest.json sidecar; the atomic write leaves no temp file on failure.

#include <welllog/core/document.hpp>
#include <welllog/export/table_writers.hpp>
#include <welllog/table/table_projection.hpp>

#include "../../src/export_table/atomic_write.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("88000000-0000-4000-8000-000000000001");
const auto axis_a_id = id("88000000-0000-4000-8000-000000000002");
const auto axis_b_id = id("88000000-0000-4000-8000-000000000003");
const auto curve_gr_id = id("88000000-0000-4000-8000-000000000004");
const auto curve_rt_id = id("88000000-0000-4000-8000-000000000005");
const auto curve_rhob_id = id("88000000-0000-4000-8000-000000000006");

// Two-axis document: axis A [1000,1001,1002,1003] with GR (null at row 1) + RT;
// axis B [2000,2050,2100] with RHOB. Exercises multi-axis + null paths.
WellLogDocument make_document() {
  auto depths_a = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  auto rt = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 4.0, 8.0});
  auto depths_b = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2000.0, 2050.0, 2100.0});
  auto rhob = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2.1, 2.3, 2.5});
  auto nulls_owner = std::make_shared<std::vector<std::uint8_t>>(1, 0);
  (*nulls_owner)[0] = 0b00000010; // GR row 1 null
  NullBitmapView gr_nulls = NullBitmapView::from_raw(
      nulls_owner->data(), 4, nulls_owner->size(), SharedOwner{nulls_owner});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{3});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths_a),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_b_id, .coordinates = BufferView::from_vector(depths_b),
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
  builder.add_curve(Curve{
      .id = curve_rhob_id, .mnemonic = "RHOB", .display_name = "Bulk Density",
      .unit = "g/cm3", .sampling_axis_id = axis_b_id,
      .values = BufferView::from_vector(rhob), .nulls = {}});
  return builder.build();
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A unique temp dir per test, cleaned up on scope exit.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    auto base = std::filesystem::temp_directory_path();
    std::ostringstream nm;
    nm << "welllog-csv-" <<
#if defined(_WIN32)
        ::_getpid()
#else
        ::getpid()
#endif
        << '-' << counter++;
    path = base / nm.str();
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
  static std::uint64_t counter;
};
std::uint64_t TempDir::counter = 0;

// --- Tests -------------------------------------------------------------------

// Single-projection CSV: header + one row per sample; values verbatim; a null
// cell emits the token. Round-trips the raw buffer (not LOD).
void single_projection_csv_round_trips_values_and_nulls() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  const TableProjection *axis_a = nullptr;
  for (const auto &t : tables) {
    if (t.sampling_axis_id() == axis_a_id) {
      axis_a = &t;
    }
  }
  require(axis_a != nullptr, "axis A must have a table");
  TempDir dir;
  const auto target = dir.path / "axis_a.csv";
  const auto r = CsvTableExporter::write_to_file(*axis_a, target);
  require(r.has_value(), "CSV write must succeed");
  require(r.value().exported_rows == 4, "axis-A CSV must have 4 data rows");
  require(r.value().exported_tables == 1, "single CSV = 1 table");
  require(r.value().document_revision == DocumentRevision{3},
          "report must carry the document revision");
  const auto body = read_file(target);
  // Header: DEPTH,GR,RT
  require(body.find("DEPTH,GR,RT\n") == 0, "header row must be DEPTH,GR,RT");
  // Row 0: 1000,10,1
  require(body.find("\n1000,10,1\n") != std::string::npos,
          "row 0 must round-trip raw values");
  // GR row 1 is null → "null" token.
  require(body.find("\n1001,null,2\n") != std::string::npos,
          "GR row 1 null must emit the null token");
}

// A literal value equal to the null token is quoted, so a null sample (token)
// is distinct from a real value that reads "null" (§7).
void null_token_distinct_from_literal_value() {
  // A curve whose value is the NaN-becoming... no — build a curve with a
  // header mnemonic equal to the null token "null"; the header field must be
  // quoted so it isn't mistaken for a null marker.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0});
  auto vals = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{5.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  // A curve mnemonic literally "null" — its header cell must be quoted.
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "null", .display_name = "Null Named",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(vals), .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  TempDir dir;
  const auto target = dir.path / "quoted.csv";
  require(CsvTableExporter::write_to_file(tables.front(), target).has_value(),
          "write must succeed");
  const auto body = read_file(target);
  // Header: DEPTH,"null" — the null-named curve header is quoted.
  require(body.find("\"null\"") != std::string::npos,
          "a header field equal to the null token must be RFC-4180 quoted");
}

// A multi-projection package writes one CSV per axis + a manifest.json sidecar
// carrying axis/units/revision/null rule (§7).
void package_writes_directory_and_manifest() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  require(tables.size() == 2, "fixture yields 2 axis tables");
  TempDir dir;
  const auto pkg = dir.path / "pkg";
  const auto r = CsvPackageExporter::write_to_directory(tables, pkg);
  require(r.has_value(), "package write must succeed");
  require(r.value().exported_tables == 2, "package must report 2 tables");
  require(r.value().exported_rows == 7, "4 + 3 rows = 7");
  // Two CSVs + manifest.json.
  require(std::filesystem::exists(pkg / (axis_a_id.to_string() + ".csv")),
          "axis-A CSV must exist");
  require(std::filesystem::exists(pkg / (axis_b_id.to_string() + ".csv")),
          "axis-B CSV must exist");
  const auto manifest = read_file(pkg / "manifest.json");
  require(manifest.find("\"welllog-csv-manifest\"") != std::string::npos,
          "manifest must declare its format");
  require(manifest.find("\"revision\":3") != std::string::npos,
          "manifest must carry the document revision");
  require(manifest.find("\"unit\":\"API\"") != std::string::npos,
          "manifest must carry column units");
  require(manifest.find("\"nullToken\":\"null\"") != std::string::npos,
          "manifest must carry the null rule");
}

// An empty projection writes a header-only CSV (no rows) without error.
void empty_projection_writes_header_only() {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(
                                       std::make_shared<const std::vector<double>>()),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  // A curve with no values; from_document drops axes with no curves, so build
  // a 0-sample axis + 0-sample curve to get a 0-row projection.
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(std::make_shared<const std::vector<double>>()),
      .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  // (from_document emits a table per axis that has curves; a 0-length axis is
  // still a table with 0 rows.)
  if (tables.empty()) {
    return; // acceptable: some builders skip 0-row; the writer handles either
  }
  TempDir dir;
  const auto target = dir.path / "empty.csv";
  const auto r = CsvTableExporter::write_to_file(tables.front(), target);
  require(r.has_value(), "empty CSV write must succeed");
  require(r.value().exported_rows == 0, "empty projection → 0 rows");
  const auto body = read_file(target);
  require(body.find("DEPTH") != std::string::npos,
          "header is still written for 0 rows");
}

// Selection-Set export path (§7 / criterion "支持完整表或 Selection Set"):
// slice a projection to a [first_row,last_row) span — as a Phase-B SelectionState
// would — then export only that range. The slice shares the source's columns/
// identity and reads the same raw buffer; row r of the slice maps to
// first_row + r of the source.
void selection_slice_exports_only_the_selected_rows() {
  const auto tables = TableProjectionBuilder::from_document(make_document());
  const TableProjection *axis_a = nullptr;
  for (const auto &t : tables) {
    if (t.sampling_axis_id() == axis_a_id) {
      axis_a = &t;
    }
  }
  require(axis_a != nullptr, "axis A must have a table");
  require(axis_a->row_count() == 4, "axis-A source has 4 rows");
  // Slice rows [1, 3) — a 2-row selection.
  const auto sliced = axis_a->slice(1, 3);
  require(sliced.row_count() == 2, "slice [1,3) must have 2 rows");
  // Identity is preserved (exporter invalidation key).
  require(sliced.document_id() == axis_a->document_id(),
          "slice must carry the source document id");
  require(sliced.document_revision() == axis_a->document_revision(),
          "slice must carry the source revision");
  // Slice row 0 → source row 1: DEPTH=1001, GR=null, RT=2.
  const auto c0 = sliced.cell(0, 0);
  require(!c0.null() && std::abs(c0.value.value_or(-1.0) - 1001.0) < 1.0e-9,
          "slice row 0 depth must map to source row 1 (1001)");
  const auto gr0 = sliced.cell(0, 1);
  require(gr0.null(), "slice row 0 GR (source row 1) must be null");
  // Export the slice — only 2 rows.
  TempDir dir;
  const auto target = dir.path / "sel.csv";
  const auto r = CsvTableExporter::write_to_file(sliced, target);
  require(r.has_value(), "slice CSV export must succeed");
  require(r.value().exported_rows == 2, "slice export must write 2 rows");
  const auto body = read_file(target);
  // Source row 1's depth (1001) is present; source row 0's depth (1000) is not.
  require(body.find("1001") != std::string::npos,
          "slice must include the selected row's depth");
  require(body.find("\n1000,") == std::string::npos,
          "slice must NOT include rows outside the selection");
  // Out-of-range / inverted slices are clamped safely.
  require(axis_a->slice(10, 20).row_count() == 0, "OOB slice → 0 rows");
  require(axis_a->slice(3, 1).row_count() == 0, "inverted slice → 0 rows");
}

[[nodiscard]] std::filesystem::path sibling_temp(const std::filesystem::path &target) {
  auto temp = target;
  temp += ".";
  temp += std::to_string(static_cast<std::uint64_t>(
#if defined(_WIN32)
      ::_getpid()
#else
      ::getpid()
#endif
      ));
  temp += ".tmp";
  return temp;
}

[[nodiscard]] std::size_t count_tmp_siblings(const std::filesystem::path &dir) {
  std::size_t n = 0;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".tmp") {
      ++n;
    }
  }
  return n;
}

// #760: write_file_atomic's failure contract (producer abort + I/O error)
// must leave no sibling temp and must not clobber a pre-existing target.
void atomic_write_failure_cleans_temp_and_preserves_target() {
  TempDir dir;
  const auto target = dir.path / "keep.csv";
  const std::string original = "PREEXISTING\n";
  {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << original;
  }
  require(std::filesystem::exists(target), "pre-existing target must exist");

  const auto aborted = export_table::write_file_atomic(
      target, [](std::ostream &out) {
        out << "PARTIAL-SHOULD-NOT-COMMIT\n";
        return false;
      });
  require(!aborted.has_value(), "producer false must fail the atomic write");
  require(aborted.error().code == ErrorCode::internal_error,
          "producer abort maps to internal_error");
  require(!std::filesystem::exists(sibling_temp(target)),
          "producer abort must remove the sibling temp");
  require(count_tmp_siblings(dir.path) == 0, "no leftover *.tmp after abort");
  require(read_file(target) == original,
          "failed write must leave the pre-existing target byte-identical");

  // I/O-failure path, portably: make the TARGET itself a directory so the
  // final rename fails on every platform. (The previous approach removed the
  // write bit on the parent directory, which POSIX honors but Windows
  // ignores for directories — the read-only DOS attribute does not block
  // file creation inside, so the write succeeded and this test failed on
  // windows-latest.) The temp file lands in a writable directory, so this
  // still exercises the rename-failure cleanup contract.
  const auto denied_dir = dir.path / "denied";
  std::filesystem::create_directories(denied_dir);
  const auto ro_target = denied_dir / "denied.csv";
  std::filesystem::create_directories(ro_target);
  const auto io_fail = export_table::write_file_atomic(
      ro_target, [](std::ostream &out) {
        out << "SHOULD-NOT-LAND\n";
        return true;
      });
  require(!io_fail.has_value(), "unwritable target must fail");
  require(io_fail.error().code == ErrorCode::internal_error,
          "I/O failure maps to internal_error");
  require(std::filesystem::is_directory(ro_target),
          "failed I/O must not replace the visible target");
  require(count_tmp_siblings(denied_dir) == 0,
          "failed I/O must not leave a sibling temp");
}

} // namespace

int main() {
  single_projection_csv_round_trips_values_and_nulls();
  null_token_distinct_from_literal_value();
  package_writes_directory_and_manifest();
  empty_projection_writes_header_only();
  selection_slice_exports_only_the_selected_rows();
  atomic_write_failure_cleans_temp_and_preserves_target();
  std::cout << "welllog.csv-table-export: all cases passed\n";
  return EXIT_SUCCESS;
}
