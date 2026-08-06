// Headless test for the versioned XML table-export writer (table-and-export.md
// §6, #155). Asserts: the document structure (wellLogTables/well/table/columns/
// rows), column types from the buffer, null → <null/> and finite → <v>, XML
// escaping of &<>, hardened (no DOCTYPE/external entity emitted), and a
// write→read ROUND-TRIP that recovers the same depths/nulls/units/identity
// (§6 "提供…往返测试").

#include <welllog/core/document.hpp>
#include <welllog/export/table_writers.hpp>
#include <welllog/table/table_projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

void require_near(double actual, double expected, std::string_view message) {
  if (std::abs(actual - expected) > 1.0e-9) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("99000000-0000-4000-8000-000000000001");
const auto axis_a_id = id("99000000-0000-4000-8000-000000000002");
const auto axis_b_id = id("99000000-0000-4000-8000-000000000003");
const auto curve_gr_id = id("99000000-0000-4000-8000-000000000004");
const auto curve_rt_id = id("99000000-0000-4000-8000-000000000005");
const auto curve_rhob_id = id("99000000-0000-4000-8000-000000000006");

// Two-axis doc: axis A [1000,1001,1002,1003] GR (null row1) + RT; axis B
// [2000,2050,2100] RHOB. Exercises multi-axis + null + multi-curve.
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
  (*nulls_owner)[0] = 0b00000010;
  NullBitmapView gr_nulls = NullBitmapView::from_raw(
      nulls_owner->data(), 4, nulls_owner->size(), SharedOwner{nulls_owner});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{5});
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

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::ostringstream nm;
    nm << "welllog-xml-"
#if defined(_WIN32)
       << ::_getpid()
#else
       << ::getpid()
#endif
       << '-' << counter++;
    path = std::filesystem::temp_directory_path() / nm.str();
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
  static std::uint64_t counter;
};
std::uint64_t TempDir::counter = 0;

// --- Tests -------------------------------------------------------------------

// Structure: declaration + root + well element carries id/revision/referenceDepth.
void xml_emits_versioned_structure_and_identity() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "out.xml";
  const auto r = XmlTableExporter::write_to_file(doc, tables, target);
  require(r.has_value(), "XML write must succeed");
  require(r.value().document_revision == DocumentRevision{5},
          "report must carry the revision");
  const auto body = read_file(target);
  require(body.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\""),
          "must start with the XML declaration");
  require(body.find("<wellLogTables schemaVersion=\"1.0\">") != std::string::npos,
          "must emit the versioned root");
  require(body.find(document_id.to_string()) != std::string::npos,
          "must carry the document id");
  require(body.find("revision=\"5\"") != std::string::npos,
          "must carry the revision attribute");
  require(body.find("referenceDepth=\"MD\"") != std::string::npos,
          "must emit the upper-cased Reference-Depth domain");
}

// Column types come from the buffer; null → <null/>, finite → <v>value</v>.
void xml_columns_carry_types_and_nulls_render_correctly() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "types.xml";
  require(XmlTableExporter::write_to_file(doc, tables, target).has_value(),
          "write must succeed");
  const auto body = read_file(target);
  // float64 depth + curve columns (the fixture uses double vectors).
  require(body.find("type=\"float64\"") != std::string::npos,
          "column type must come from the buffer scalar type");
  require(body.find("unit=\"API\"") != std::string::npos,
          "column units must be preserved");
  // GR row 0 is finite: <row><v>1000</v><v>10</v><v>1</v></row>.
  require(body.find("<row><v>1000</v><v>10</v><v>1</v></row>") != std::string::npos,
          "row 0: depth 1000 + GR 10 + RT 1, all finite");
  // GR row 1 is null → <null/> in the GR position.
  require(body.find("<v>1001</v><null/><v>2</v>") != std::string::npos,
          "row 1: depth + null GR + RT, in order");
  require(body.find("<null/>") != std::string::npos,
          "a null cell must render as <null/>");
}

// Hardening: no DOCTYPE, no external entity reference is ever emitted.
void xml_is_hardened_no_dtd_or_entities() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "hard.xml";
  require(XmlTableExporter::write_to_file(doc, tables, target).has_value(),
          "write must succeed");
  const auto body = read_file(target);
  require(body.find("<!DOCTYPE") == std::string::npos,
          "writer must NEVER emit a DOCTYPE (§6 disables DTD)");
  require(body.find("<!ENTITY") == std::string::npos,
          "writer must NEVER emit an entity declaration (§6 disables external entities)");
  require(body.find("SYSTEM") == std::string::npos,
          "writer must NEVER emit a SYSTEM external reference");
}

// XML-escaping: a mnemonic containing & < > is escaped, not emitted raw.
void xml_escapes_special_characters() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0});
  auto vals = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{5.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "A<B>&\"C'", .display_name = "x",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(vals), .nulls = {}});
  const auto doc = builder.build();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "escape.xml";
  require(XmlTableExporter::write_to_file(doc, tables, target).has_value(),
          "write must succeed");
  const auto body = read_file(target);
  require(body.find("<") != std::string::npos, "sanity");
  // The mnemonic must be escaped; no raw "B>" as markup.
  require(body.find("&lt;") != std::string::npos, "< must be escaped");
  require(body.find("&gt;") != std::string::npos, "> must be escaped");
  require(body.find("&amp;") != std::string::npos, "& must be escaped");
  require(body.find("&quot;") != std::string::npos, "\" must be escaped");
  require(body.find("&apos;") != std::string::npos, "' must be escaped");
}

// ROUND-TRIP (§6 acceptance): write the doc, parse the depths/nulls back out of
// the XML, and confirm they match the source projection. A minimal hand parser
// (not a full XML parser) extracts the <v>/<null/> sequence per row.
void xml_round_trips_depths_and_nulls() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "roundtrip.xml";
  require(XmlTableExporter::write_to_file(doc, tables, target).has_value(),
          "write must succeed");
  const auto body = read_file(target);
  // Find the axis-A table block and its rows; re-read values.
  // Strategy: collect every <row>...</row> in axis-A order. Each row's cells
  // are <v>num</v> or <null/>. The first table in the doc is axis A.
  const auto first_rows = body.find("<rows>");
  require(first_rows != std::string::npos, "must have a <rows> block");
  // Parse all <v> and <null/> tokens in document order; the first table is
  // axis A: DEPTH + GR + RT, 4 rows. We read 12 cells then stop.
  struct Cell {
    bool null;
    double value;
  };
  std::vector<Cell> cells;
  std::size_t pos = first_rows;
  while (pos < body.size()) {
    const auto v = body.find("<v>", pos);
    const auto n = body.find("<null/>", pos);
    if (v == std::string::npos && n == std::string::npos) {
      break;
    }
    if (n != std::string::npos && (v == std::string::npos || n < v)) {
      cells.push_back({true, 0.0});
      pos = n + std::string_view("<null/>").size();
      continue;
    }
    const auto end = body.find("</v>", v);
    require(end != std::string::npos, "<v> must be closed");
    const auto text = body.substr(v + 3, end - (v + 3));
    cells.push_back({false, std::stod(std::string(text))});
    pos = end + std::string_view("</v>").size();
    if (cells.size() >= 12) {
      break; // first table only (4 rows × 3 cols)
    }
  }
  require(cells.size() == 12, "axis-A must round-trip 12 cells (4×3)");
  // Row 0: DEPTH=1000, GR=10, RT=1.
  require_near(cells[0].value, 1000.0, "round-trip depth row 0");
  require_near(cells[1].value, 10.0, "round-trip GR row 0");
  require_near(cells[2].value, 1.0, "round-trip RT row 0");
  // Row 1: GR is null.
  require(cells[3].value == 1001.0 && !cells[3].null, "depth row 1");
  require(cells[4].null, "GR row 1 must round-trip as null");
  require_near(cells[5].value, 2.0, "RT row 1");
}

} // namespace

int main() {
  xml_emits_versioned_structure_and_identity();
  xml_columns_carry_types_and_nulls_render_correctly();
  xml_is_hardened_no_dtd_or_entities();
  xml_escapes_special_characters();
  xml_round_trips_depths_and_nulls();
  std::cout << "welllog.xml-table-export: all cases passed\n";
  return EXIT_SUCCESS;
}
