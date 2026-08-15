// Self-contained XLSX (OOXML SpreadsheetML) table export (table-and-export.md
// §5, #155). Produces a .xlsx via a minimal hand-written zip (PRIVATE ZLIB
// deflate) — no third-party spreadsheet library (ADR 0041). One workbook per
// document: one curve worksheet per Sampling Axis, plus a Metadata worksheet.
// Numeric cells are numeric (not pre-formatted strings); null cells are empty
// (§5.2). A projection exceeding 1,048,576 rows splits into _01/_02...
// continuation sheets (§5.1), each repeating column definitions; the global
// start row is recorded in the metadata sheet.

#include <welllog/export/table_writers.hpp>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "atomic_write.hpp"
#include "format.hpp"
#include "zip_writer.hpp"

#include <welllog/core/document.hpp>
#include <welllog/table/table_projection.hpp>

namespace welllog {
namespace {

using export_table::ZipEntry;
using export_table::ZipWriter;
using export_table::append_integer;
using export_table::append_number;
using export_table::write_file_atomic;

// Excel's per-worksheet row ceiling (including the header row counts against
// it, so the data ceiling per sheet is this minus the header).
constexpr std::uint64_t excel_max_rows_per_sheet = 1'048'576;

// XML-escapes text for OOXML (same rule as the XML writer). Inline strings in
// the worksheet use this.
void append_xml_text(std::string &out, std::string_view s) {
  for (const auto c : s) {
    switch (c) {
    case '&': out.append("&amp;"); break;
    case '<': out.append("&lt;"); break;
    case '>': out.append("&gt;"); break;
    case '"': out.append("&quot;"); break;
    case '\'': out.append("&apos;"); break;
    default:
      if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') {
        out.push_back(c);
      }
    }
  }
}

// Converts a 0-based column index to an Excel column letter (A, B, ... Z, AA).
void append_column_letters(std::string &out, std::uint64_t col) {
  std::string letters;
  ++col; // to 1-based
  while (col > 0) {
    --col;
    letters.push_back(static_cast<char>('A' + (col % 26)));
    col /= 26;
  }
  for (auto it = letters.rbegin(); it != letters.rend(); ++it) {
    out.push_back(*it);
  }
}

// One sheet's content: the worksheet XML built into `body`. `global_start_row`
// is the first row index (0-based, into the projection) this sheet covers —
// recorded in metadata for continuation sheets (§5.1). Returns the row count
// actually written.
[[nodiscard]] std::uint64_t build_worksheet_body(
    std::string &body, const TableProjection &projection,
    std::uint64_t first_row, std::uint64_t last_row,
    std::uint64_t global_start_row) {
  body.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
  body.append("<worksheet xmlns=\"http://schemas.openxmlformats.org/"
              "spreadsheetml/2006/main\">");
  // Column definitions (units carried as a header row; the <cols> widths are
  // optional, omitted for minimalism).
  body.append("<sheetData>");
  // Header row: column names + a marker for the global start row on split
  // sheets. Row 1 = headers.
  body.append("<row r=\"1\">");
  std::string cell_ref;
  for (std::uint64_t c = 0; c < projection.column_count(); ++c) {
    const auto col = projection.column(c);
    cell_ref.clear();
    append_column_letters(cell_ref, c);
    cell_ref += "1";
    body.append("<c r=\"");
    body.append(cell_ref);
    body.append("\" t=\"inlineStr\"><is><t>");
    append_xml_text(body, col.name);
    body.append("</t></is></c>");
  }
  body.append("</row>");
  // Unit row (row 2): the units, so §5.2 "单位写在独立表头行" is satisfied.
  body.append("<row r=\"2\">");
  for (std::uint64_t c = 0; c < projection.column_count(); ++c) {
    const auto col = projection.column(c);
    cell_ref.clear();
    append_column_letters(cell_ref, c);
    cell_ref += "2";
    body.append("<c r=\"");
    body.append(cell_ref);
    body.append("\" t=\"inlineStr\"><is><t>");
    append_xml_text(body, col.unit);
    body.append("</t></is></c>");
  }
  body.append("</row>");
  // Data rows. Excel rows are 1-based; headers occupy 1-2, so data starts at 3.
  std::uint64_t written = 0;
  std::uint64_t excel_row = 3;
  for (std::uint64_t r = first_row; r < last_row; ++r) {
    body.append("<row r=\"");
    append_integer(body, excel_row);
    body.append("\">");
    for (std::uint64_t c = 0; c < projection.column_count(); ++c) {
      const auto cell = projection.cell(r, c);
      cell_ref.clear();
      append_column_letters(cell_ref, c);
      append_integer(cell_ref, excel_row);
      if (cell.null()) {
        // Null → empty cell (no <v>). §5.2: "Null 写为空单元格".
        body.append("<c r=\"");
        body.append(cell_ref);
        body.append("\"/>");
      } else {
        // Numeric cell (default type is number; no t= attribute).
        body.append("<c r=\"");
        body.append(cell_ref);
        body.append("\"><v>");
        append_number(body, *cell.value);
        body.append("</v></c>");
      }
    }
    body.append("</row>");
    ++written;
    ++excel_row;
  }
  body.append("</sheetData></worksheet>");
  (void)global_start_row; // recorded by the caller in the metadata sheet
  return written;
}

// Builds the metadata sheet body (units/axis/revision/null rule + per-sheet
// global start rows for splits, §5.1).
void build_metadata_sheet(std::string &body, const WellLogDocument &document,
                          const std::vector<TableProjection> &projections,
                          const std::vector<std::string> &sheet_names,
                          const std::vector<std::uint64_t> &sheet_starts) {
  body.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
  body.append("<worksheet xmlns=\"http://schemas.openxmlformats.org/"
              "spreadsheetml/2006/main\"><sheetData>");
  auto emit_row = [&](std::uint64_t r, std::string_view key,
                      std::string_view value) {
    body.append("<row r=\"");
    append_integer(body, r);
    body.append("\"><c r=\"A");
    append_integer(body, r);
    body.append("\" t=\"inlineStr\"><is><t>");
    append_xml_text(body, key);
    body.append("</t></is></c><c r=\"B");
    append_integer(body, r);
    body.append("\" t=\"inlineStr\"><is><t>");
    append_xml_text(body, value);
    body.append("</t></is></c></row>");
  };
  std::uint64_t r = 1;
  emit_row(r++, "documentId", document.id().to_string());
  emit_row(r++, "revision", std::to_string(document.revision().value));
  emit_row(r++, "nullRule", "empty cell = null sample");
  for (std::size_t i = 0; i < sheet_names.size(); ++i) {
    std::string label = "sheet ";
    label += sheet_names[i];
    label += " globalStartRow";
    emit_row(r++, label, std::to_string(sheet_starts[i]));
  }
  // Per-axis units summary.
  for (const auto &p : projections) {
    std::string label = "axis ";
    label += p.sampling_axis_id().to_string();
    std::string units;
    for (std::uint64_t c = 0; c < p.column_count(); ++c) {
      if (c != 0) units += ',';
      units += p.column(c).unit;
    }
    emit_row(r++, label, units);
  }
  body.append("</sheetData></worksheet>");
}

// OOXML relationship + content-type parts (minimal).
std::string build_content_types(std::uint64_t sheet_count) {
  std::string out;
  out.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
  out.append("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
             "content-types\">");
  out.append("<Default Extension=\"rels\" ContentType=\"application/vnd.openxml"
             "formats-package.relationships+xml\"/>");
  out.append("<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
  out.append("<Override PartName=\"/xl/workbook.xml\" ContentType=\"application"
             "/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>");
  for (std::uint64_t i = 1; i <= sheet_count; ++i) {
    out.append("<Override PartName=\"/xl/worksheets/sheet");
    append_integer(out, i);
    out.append(".xml\" ContentType=\"application/vnd.openxmlformats-officedocum"
               "ent.spreadsheetml.worksheet+xml\"/>");
  }
  out.append("</Types>");
  return out;
}

std::string build_root_rels() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/"
         "relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.open"
         "xmlformats.org/officeDocument/2006/relationships/officeDocument\" "
         "Target=\"xl/workbook.xml\"/></Relationships>";
}

std::string build_workbook(std::uint64_t sheet_count,
                           const std::vector<std::string> &sheet_names) {
  std::string out;
  out.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
  out.append("<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml"
             "/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDoc"
             "ument/2006/relationships\"><sheets>");
  for (std::uint64_t i = 0; i < sheet_count; ++i) {
    out.append("<sheet name=\"");
    append_xml_text(out, sheet_names[i]);
    out.append("\" sheetId=\"");
    append_integer(out, i + 1);
    out.append("\" r:id=\"rId");
    append_integer(out, i + 1);
    out.append("\"/>");
  }
  out.append("</sheets></workbook>");
  return out;
}

std::string build_workbook_rels(std::uint64_t sheet_count) {
  std::string out;
  out.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
  out.append("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
             "2006/relationships\">");
  for (std::uint64_t i = 1; i <= sheet_count; ++i) {
    out.append("<Relationship Id=\"rId");
    append_integer(out, i);
    out.append("\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006"
               "/relationships/worksheet\" Target=\"worksheets/sheet");
    append_integer(out, i);
    out.append(".xml\"/>");
  }
  out.append("</Relationships>");
  return out;
}

} // namespace

Result<TableExportReport>
XlsxTableExporter::write_to_file(const WellLogDocument &document,
                                 const std::vector<TableProjection> &projections,
                                 const std::filesystem::path &path) {
  // Streaming layout (issue #467): the old path held EVERY worksheet body in
  // memory, then the whole deflated archive in one more string — a 1M-row
  // workbook peaked at 3-4x its XML size. Sheet bodies are now built inside
  // the atomic-write producer and streamed straight into the zip sink, so
  // peak memory holds one sheet body plus its compressed copy.
  struct SheetPlan {
    std::string name;
    std::uint64_t start{};
    std::uint64_t end{};
    const TableProjection *projection{}; // nullptr = metadata sheet
  };
  std::vector<SheetPlan> sheet_plans;
  std::uint64_t total_rows = 0;

  // One or more sheets per curve projection, split at the Excel row ceiling.
  // The header occupies 2 rows (names + units); the data ceiling per sheet is
  // excel_max_rows_per_sheet - 2.
  constexpr std::uint64_t header_rows = 2;
  const std::uint64_t data_ceiling = excel_max_rows_per_sheet - header_rows;
  for (const auto &p : projections) {
    if (p.kind() != TableKind::curves) {
      continue;
    }
    const auto row_count = p.row_count();
    std::uint64_t start = 0;
    std::uint64_t part = 1;
    while (start < row_count || (start == 0 && row_count == 0)) {
      const auto end = std::min(start + data_ceiling, row_count);
      std::string name = "curves_";
      // Disambiguate multi-axis by the leading axis-id hex; suffix _01/_02 on
      // split.
      name += p.sampling_axis_id().to_string().substr(0, 8);
      if (row_count > data_ceiling) {
        name += "_";
        if (part < 10) name += "0";
        append_integer(name, part);
      }
      total_rows += end - start;
      sheet_plans.push_back(SheetPlan{.name = std::move(name),
                                      .start = start,
                                      .end = end,
                                      .projection = &p});
      ++part;
      if (start == end) {
        break; // 0-row projection: one empty sheet, then stop
      }
      start = end;
    }
  }

  // Metadata sheet (always last).
  sheet_plans.push_back(
      SheetPlan{.name = "Metadata", .start = 0, .end = 0, .projection = nullptr});

  const auto total_sheets = sheet_plans.size();
  const auto result = write_file_atomic(
      path,
      [&](std::ostream &out) -> bool {
        export_table::StreamingZipSink zip(out);
        std::vector<std::string> sheet_names;
        std::vector<std::uint64_t> sheet_starts;
        sheet_names.reserve(sheet_plans.size());
        sheet_starts.reserve(sheet_plans.size());
        for (const auto &plan : sheet_plans) {
          sheet_names.push_back(plan.name);
          sheet_starts.push_back(plan.start);
        }
        if (!zip.add_entry("[Content_Types].xml",
                           build_content_types(total_sheets), true)) {
          return false;
        }
        if (!zip.add_entry("_rels/.rels", build_root_rels(), true)) {
          return false;
        }
        if (!zip.add_entry("xl/workbook.xml",
                           build_workbook(total_sheets, sheet_names), true)) {
          return false;
        }
        if (!zip.add_entry("xl/_rels/workbook.xml.rels",
                           build_workbook_rels(total_sheets), true)) {
          return false;
        }
        for (std::size_t i = 0; i < sheet_plans.size(); ++i) {
          std::string part_name = "xl/worksheets/sheet";
          append_integer(part_name, i + 1);
          part_name += ".xml";
          std::string body;
          if (sheet_plans[i].projection != nullptr) {
            const auto rows_written = build_worksheet_body(
                body, *sheet_plans[i].projection, sheet_plans[i].start,
                sheet_plans[i].end, sheet_plans[i].start);
            (void)rows_written;  // start/end are precomputed by the planner
          } else {
            build_metadata_sheet(body, document, projections, sheet_names,
                                 sheet_starts);
          }
          if (!zip.add_entry(part_name, body, true)) {
            return false;
          }
          // Free the body before the next sheet is built.
          std::string().swap(body);
        }
        return zip.finalize();
      });
  if (!result.has_value()) {
    return result.error();
  }
  return TableExportReport{
      .path = result.value(),
      .exported_rows = total_rows,
      .exported_tables = projections.size(),
      .document_revision = document.revision(),
  };
}

} // namespace welllog
