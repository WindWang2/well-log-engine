// Headless test for the self-contained XLSX (OOXML) table export (table-and-
// export.md §5, #155). Validates by unzipping the produced archive with ZLIB
// inflate (no Excel dependency) and asserting on the OOXML parts: the archive
// is well-formed (EOCD present, entries readable), [Content_Types].xml and
// workbook.xml declare the sheets, worksheet cells carry numeric values /
// empty cells for nulls, and the metadata sheet carries units/identity.

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
#include <zlib.h>

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

const auto document_id = id("aaaaaaaa-0000-4000-8000-000000000001");
const auto axis_a_id = id("aaaaaaaa-0000-4000-8000-000000000002");
const auto axis_b_id = id("aaaaaaaa-0000-4000-8000-000000000003");
const auto curve_gr_id = id("aaaaaaaa-0000-4000-8000-000000000004");
const auto curve_rt_id = id("aaaaaaaa-0000-4000-8000-000000000005");
const auto curve_rhob_id = id("aaaaaaaa-0000-4000-8000-000000000006");

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
  WellLogDocumentBuilder builder(document_id, DocumentRevision{7});
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
    nm << "welllog-xlsx-"
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

// --- Minimal ZIP reader (inflate entries) -----------------------------------

struct ZipReadEntry {
  std::string name;
  std::string data;       // decompressed
  std::uint16_t method{};
};

[[nodiscard]] std::uint32_t read_u32(const std::string &s, std::size_t off) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1]))
          << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2]))
          << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3]))
          << 24);
}
[[nodiscard]] std::uint16_t read_u16(const std::string &s, std::size_t off) {
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(s[off]) |
      (static_cast<unsigned char>(s[off + 1]) << 8));
}

// Inflates a raw deflate stream into `out`.
[[nodiscard]] bool inflate_raw(const std::string &compressed,
                               std::string &out) {
  z_stream stream{};
  if (inflateInit2(&stream, -15) != Z_OK) {
    return false;
  }
  out.resize(compressed.size() * 4 + 1024);
  stream.next_in = reinterpret_cast<Bytef *>(
      const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  for (;;) {
    stream.next_out = reinterpret_cast<Bytef *>(out.data() + stream.total_out);
    stream.avail_out =
        static_cast<uInt>(out.size() - stream.total_out);
    const auto rc = inflate(&stream, Z_NO_FLUSH);
    if (rc == Z_STREAM_END) {
      out.resize(stream.total_out);
      inflateEnd(&stream);
      return true;
    }
    if (rc == Z_OK || rc == Z_BUF_ERROR) {
      if (stream.avail_out == 0) {
        out.resize(out.size() * 2);
        continue;
      }
      inflateEnd(&stream);
      return false;
    }
    inflateEnd(&stream);
    return false;
  }
}

// Parses the local-file-header entries of a ZIP archive. Sufficient for the
// OOXML archives this writer produces (no data descriptors, sequential).
[[nodiscard]] std::vector<ZipReadEntry>
read_zip_entries(const std::string &archive) {
  std::vector<ZipReadEntry> entries;
  std::size_t pos = 0;
  while (pos + 4 <= archive.size()) {
    const auto sig = read_u32(archive, pos);
    if (sig != 0x04034b50) {
      break; // reached central directory or EOCD
    }
    const auto method = read_u16(archive, pos + 8);
    const auto compressed_size = read_u32(archive, pos + 18);
    const auto name_len = read_u16(archive, pos + 26);
    const auto extra_len = read_u16(archive, pos + 28);
    const auto name_off = pos + 30;
    if (name_off + name_len > archive.size()) {
      break;
    }
    ZipReadEntry e;
    e.name.assign(archive, name_off, name_len);
    e.method = method;
    const auto data_off = name_off + name_len + extra_len;
    if (data_off + compressed_size > archive.size()) {
      break;
    }
    std::string raw(archive, data_off, compressed_size);
    if (method == 0) {
      e.data = std::move(raw);
    } else if (method == 8) {
      if (!inflate_raw(raw, e.data)) {
        break;
      }
    } else {
      break;
    }
    entries.push_back(std::move(e));
    pos = data_off + compressed_size;
  }
  return entries;
}

[[nodiscard]] const ZipReadEntry *
find_entry(const std::vector<ZipReadEntry> &entries, std::string_view name) {
  for (const auto &e : entries) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

// --- Tests -------------------------------------------------------------------

// The archive is well-formed and carries the core OOXML parts; content types
// and workbook declare the sheets.
void xlsx_archive_is_well_formed_with_core_parts() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "out.xlsx";
  const auto r = XlsxTableExporter::write_to_file(doc, tables, target);
  require(r.has_value(), "XLSX write must succeed");
  require(r.value().document_revision == DocumentRevision{7},
          "report must carry the revision");
  const auto archive = read_file(target);
  require(archive.size() > 0, "archive must be non-empty");
  // EOCD signature present.
  require(archive.find(std::string({'P', 'K', '\x05', '\x06'})) !=
              std::string::npos,
          "archive must have an end-of-central-directory record");
  const auto entries = read_zip_entries(archive);
  require(find_entry(entries, "[Content_Types].xml") != nullptr,
          "must have [Content_Types].xml");
  require(find_entry(entries, "_rels/.rels") != nullptr,
          "must have _rels/.rels");
  require(find_entry(entries, "xl/workbook.xml") != nullptr,
          "must have xl/workbook.xml");
  const auto *wb = find_entry(entries, "xl/workbook.xml");
  require(wb->data.find("<sheets>") != std::string::npos,
          "workbook must declare sheets");
  // 2 curve sheets + 1 metadata sheet = 3 sheets.
  require(wb->data.find("Metadata") != std::string::npos,
          "workbook must include the Metadata sheet");
}

// Numeric cells are numeric; null cells are empty (§5.2). The first worksheet
// (axis A: DEPTH + GR + RT) carries the values; GR row 1 (null) is an empty c.
void xlsx_numeric_and_null_cells() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "cells.xlsx";
  require(XlsxTableExporter::write_to_file(doc, tables, target).has_value(),
          "write must succeed");
  const auto entries = read_zip_entries(read_file(target));
  const auto *sheet = find_entry(entries, "xl/worksheets/sheet1.xml");
  require(sheet != nullptr, "sheet1 must exist");
  // Row 3 (first data row): DEPTH=1000, GR=10, RT=1.
  require(sheet->data.find("<v>1000</v>") != std::string::npos,
          "depth value must be numeric");
  require(sheet->data.find("<v>10</v>") != std::string::npos,
          "GR row 0 finite value must be numeric");
  // The header row carries column names as inline strings.
  require(sheet->data.find("<t>DEPTH</t>") != std::string::npos,
          "header row must carry the depth column name");
  require(sheet->data.find("<t>API</t>") != std::string::npos,
          "unit row must carry the API unit");
}

// The metadata sheet carries the document id, revision, and units (§5.2).
void xlsx_metadata_sheet_carries_identity_and_units() {
  const auto doc = make_document();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "meta.xlsx";
  require(XlsxTableExporter::write_to_file(doc, tables, target).has_value(),
          "write must succeed");
  const auto entries = read_zip_entries(read_file(target));
  // The last sheet is Metadata (sheet3 for a 2-curve doc).
  const auto *meta = find_entry(entries, "xl/worksheets/sheet3.xml");
  require(meta != nullptr, "metadata sheet must exist");
  require(meta->data.find(document_id.to_string()) != std::string::npos,
          "metadata must carry the document id");
  require(meta->data.find("revision") != std::string::npos,
          "metadata must carry the revision label");
  require(meta->data.find("empty cell = null sample") != std::string::npos,
          "metadata must record the null rule");
  require(meta->data.find("API") != std::string::npos,
          "metadata must carry the units");
}

// A projection exceeding the Excel row ceiling splits into continuation sheets,
// each recording its global start row (§5.1). Excel's ceiling is 1,048,576
// rows (minus the 2-row header).
void xlsx_splits_oversized_projection_into_continuation_sheets() {
  // A 1.1M-row axis: exceeds the (1,048,576 - 2) data ceiling → 2 sheets.
  constexpr std::uint64_t n = 1'100'000;
  auto depths_fill = std::make_shared<std::vector<double>>(n);
  auto gr_fill = std::make_shared<std::vector<double>>(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    (*depths_fill)[i] = 1000.0 + static_cast<double>(i) * 0.125;
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
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_a_id, .values = BufferView::from_vector(gr),
      .nulls = {}});
  const auto doc = builder.build();
  const auto tables = TableProjectionBuilder::from_document(doc);
  TempDir dir;
  const auto target = dir.path / "big.xlsx";
  const auto r = XlsxTableExporter::write_to_file(doc, tables, target);
  require(r.has_value(), "large XLSX write must succeed");
  require(r.value().exported_rows == n,
          "all rows must be exported across the split sheets");
  const auto entries = read_zip_entries(read_file(target));
  // Two curve sheets (split) + one metadata sheet = 3 total worksheets.
  require(find_entry(entries, "xl/worksheets/sheet1.xml") != nullptr &&
              find_entry(entries, "xl/worksheets/sheet2.xml") != nullptr,
          "an oversized projection must split into >= 2 sheets");
  // The workbook names the split sheets with _01/_02 suffixes (§5.1).
  const auto *wb = find_entry(entries, "xl/workbook.xml");
  require(wb != nullptr, "workbook must exist");
  require(wb->data.find("_01") != std::string::npos &&
              wb->data.find("_02") != std::string::npos,
          "split sheet names must use the _01/_02 convention (§5.1)");
  // The metadata records the second sheet's global start row (= first sheet's
  // data ceiling, 1,048,574).
  const auto *meta = find_entry(entries, "xl/worksheets/sheet3.xml");
  require(meta != nullptr && meta->data.find("globalStartRow") != std::string::npos,
          "metadata must record continuation sheets' global start rows");
}

} // namespace

int main() {
  xlsx_archive_is_well_formed_with_core_parts();
  xlsx_numeric_and_null_cells();
  xlsx_metadata_sheet_carries_identity_and_units();
  xlsx_splits_oversized_projection_into_continuation_sheets();
  std::cout << "welllog.xlsx-table-export: all cases passed\n";
  return EXIT_SUCCESS;
}
