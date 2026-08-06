// Versioned XML table-export writer (table-and-export.md §6, #155). NOT
// SpreadsheetML — a streaming, hardened writer emitting one
// <wellLogTables schemaVersion="1.0"> document grouping a well's projections.
// Disables DTD, external entities, network by construction (never emits them;
// attribute/text content is XML-escaped, control characters rejected). Streams
// <row> elements from the raw cell() reads (zero copy, not LOD, constant
// memory).

#include <welllog/export/table_writers.hpp>

#include <cctype>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "atomic_write.hpp"
#include "format.hpp"

#include <welllog/core/document.hpp>
#include <welllog/table/table_projection.hpp>

namespace welllog {
namespace {

using export_table::append_integer;
using export_table::append_number;
using export_table::write_file_atomic;

// Appends XML-escaped text (escaping & < > " '). Used for both attribute values
// and element text. Control characters (other than tab/newline, which the data
// never contains) are dropped — a hardened writer never emits them.
void append_xml_escaped(std::string &out, std::string_view s) {
  for (const auto c : s) {
    switch (c) {
    case '&':
      out.append("&amp;");
      break;
    case '<':
      out.append("&lt;");
      break;
    case '>':
      out.append("&gt;");
      break;
    case '"':
      out.append("&quot;");
      break;
    case '\'':
      out.append("&apos;");
      break;
    case '\t':
      out.push_back(c);
      break;
    case '\n':
    case '\r':
      // Newlines in attribute/element text would corrupt structure; drop them
      // (mnemonics/units/labels do not legitimately contain raw newlines).
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        continue; // reject other control chars
      }
      out.push_back(c);
    }
  }
}

// Streams the full XML document into `out`. Returns the total data rows
// written across all tables.
[[nodiscard]] std::uint64_t
stream_xml(std::ostream &out, const WellLogDocument &document,
           const std::vector<TableProjection> &projections,
           const XmlTableExportOptions &options) {
  std::string buf;
  // XML declaration + root. No DOCTYPE (§6 disables DTD). The standalone="yes"
  // signals no external resources.
  buf.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
  buf.append("<wellLogTables schemaVersion=\"");
  append_xml_escaped(buf, options.schema_version);
  buf.append("\"><well id=\"");
  append_xml_escaped(buf, document.id().to_string());
  buf.append("\" revision=\"");
  append_integer(buf, document.revision().value);
  // §6's referenceDepth/unit describe the document's depth frame. Phase A has
  // no explicit document-level depth frame, so derive a CONSENSUS: if every
  // Sampling Axis shares the same domain (and unit), emit it; otherwise OMIT
  // the attributes rather than guessing from axes().front() (an ordering
  // assumption the spec does not grant). When axes disagree or there are none,
  // the attributes are absent — a reader treats the table-level samplingAxisId
  // as authoritative.
  const auto axes = document.sampling_axes();
  std::optional<DepthDomain> consensus_domain;
  std::optional<std::string> consensus_unit;
  if (!axes.empty()) {
    consensus_domain = axes.front().domain;
    consensus_unit = axes.front().unit;
    for (const auto &axis : axes) {
      if (axis.domain != *consensus_domain || axis.unit != *consensus_unit) {
        consensus_domain.reset();
        consensus_unit.reset();
        break;
      }
    }
  }
  if (consensus_domain.has_value()) {
    buf.append("\" referenceDepth=\"");
    const auto domain_token = depth_domain_name(*consensus_domain);
    // Canonical tokens are lowercase (md/tvd/...); the §6 example shows
    // upper-case display, so upper-case the emitted attribute value.
    for (const auto c : domain_token) {
      buf.push_back(static_cast<char>(
          std::toupper(static_cast<unsigned char>(c))));
    }
    buf.append("\" unit=\"");
    append_xml_escaped(buf, *consensus_unit);
  }
  buf.append("\">");
  out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  buf.clear();

  std::uint64_t total_rows = 0;
  for (const auto &p : projections) {
    if (p.kind() != TableKind::curves) {
      continue; // Phase A builds curve tables only
    }
    buf.append("<table id=\"");
    append_xml_escaped(buf, p.sampling_axis_id().to_string());
    buf.append("\" kind=\"curves\" samplingAxisId=\"");
    append_xml_escaped(buf, p.sampling_axis_id().to_string());
    buf.append("\"><columns>");
    for (std::uint64_t c = 0; c < p.column_count(); ++c) {
      const auto col = p.column(c);
      buf.append("<column id=\"");
      append_xml_escaped(buf, col.curve_id.to_string());
      buf.append("\" name=\"");
      append_xml_escaped(buf, col.name);
      buf.append("\" unit=\"");
      append_xml_escaped(buf, col.unit);
      buf.append("\" type=\"");
      buf.append(scalar_type_name(col.scalar_type));
      buf.append("\"/>");
    }
    buf.append("</columns><rows>");
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.clear();
    // Stream rows one at a time so a million-row table stays constant-memory.
    std::string row;
    for (std::uint64_t r = 0; r < p.row_count(); ++r) {
      row.append("<row>");
      for (std::uint64_t c = 0; c < p.column_count(); ++c) {
        const auto cell = p.cell(r, c);
        if (cell.null()) {
          row.append("<null/>");
        } else {
          row.append("<v>");
          append_number(row, *cell.value);
          row.append("</v>");
        }
      }
      row.append("</row>");
      out.write(row.data(), static_cast<std::streamsize>(row.size()));
      row.clear();
      ++total_rows;
    }
    buf.append("</rows></table>");
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.clear();
  }
  buf.append("</well></wellLogTables>");
  out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  return total_rows;
}

} // namespace

Result<TableExportReport>
XmlTableExporter::write_to_file(const WellLogDocument &document,
                                const std::vector<TableProjection> &projections,
                                const std::filesystem::path &path,
                                XmlTableExportOptions options) {
  std::uint64_t rows = 0;
  const auto producer = [&](std::ostream &out) -> bool {
    rows = stream_xml(out, document, projections, options);
    return true;
  };
  const auto result = write_file_atomic(path, producer);
  if (!result.has_value()) {
    return result.error();
  }
  return TableExportReport{
      .path = result.value(),
      .exported_rows = rows,
      .exported_tables = projections.size(),
      .document_revision = document.revision(),
  };
}

} // namespace welllog
