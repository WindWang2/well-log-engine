// CSV table-export writer (table-and-export.md §7, #155). One CSV = one Table
// Projection. Streams rows from the projection's raw cell() reads (zero copy,
// constant memory). Null cells emit the null token; a literal value equal to
// the token is RFC-4180-quoted so the two never collide (§7 "Null 令牌不得与合法
// 值混淆"). UTF-8; deterministic delimiter/decimal-point/LF.

#include <welllog/export/table_writers.hpp>

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atomic_write.hpp"
#include "format.hpp"

#include <welllog/core/document.hpp>
#include <welllog/table/table_projection.hpp>

namespace welllog {
namespace {

using export_table::append_integer;
using export_table::append_number;
using export_table::write_file_atomic;

// True if a CSV field needs RFC-4180 quoting: contains the delimiter, a quote,
// or any line break.
[[nodiscard]] bool needs_quoting(std::string_view field, char delimiter) {
  for (const auto c : field) {
    if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
      return true;
    }
  }
  return false;
}

// Appends one RFC-4180-quoted field (doubles embedded quotes, wraps in quotes).
void append_quoted(std::string &out, std::string_view field) {
  out.push_back('"');
  for (const auto c : field) {
    if (c == '"') {
      out.push_back('"');
    }
    out.push_back(c);
  }
  out.push_back('"');
}

// Appends one CSV field, quoting only when needed. A field that exactly equals
// the null token is ALWAYS quoted so a real null sample (token, unquoted) is
// distinct from a legitimate value that happens to read as the token (quoted)
// — §7 "Null 令牌不得与合法值混淆". (Default token "null" never equals a numeric
// form, so this matters for header mnemonics and non-default tokens.)
void append_field(std::string &out, std::string_view field, char delimiter,
                  std::string_view null_token) {
  if (field == null_token || needs_quoting(field, delimiter)) {
    append_quoted(out, field);
    return;
  }
  out.append(field);
}

// Minimal JSON string escaping (for the manifest sidecar).
void escape_json(std::string &out, std::string_view s) {
  for (const auto c : s) {
    switch (c) {
    case '"':
      out.append("\\\"");
      break;
    case '\\':
      out.append("\\\\");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      out.push_back(c);
    }
  }
}

// Streams the full CSV (header + rows) for one projection into `out`. Returns
// the number of data rows written. The CSV body is pure data + a single header
// row of names; units/metadata live only in the sidecar manifest (§7 forbids
// arbitrary comment lines).
[[nodiscard]] std::uint64_t
stream_csv(std::ostream &out, const TableProjection &projection,
           const CsvExportOptions &options) {
  const auto cols = projection.column_count();
  std::string line;
  // Header row: column names.
  for (std::uint64_t c = 0; c < cols; ++c) {
    if (c != 0) {
      line.push_back(options.delimiter);
    }
    const auto col = projection.column(c);
    append_field(line, col.name, options.delimiter, options.null_token);
  }
  line.push_back('\n');
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  line.clear();
  std::uint64_t rows = 0;
  for (std::uint64_t r = 0; r < projection.row_count(); ++r) {
    for (std::uint64_t c = 0; c < cols; ++c) {
      if (c != 0) {
        line.push_back(options.delimiter);
      }
      const auto cell = projection.cell(r, c);
      if (cell.null()) {
        line.append(options.null_token);
      } else {
        // Numeric form never collides with a wordy null_token (default "null").
        append_number(line, *cell.value);
      }
    }
    line.push_back('\n');
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    line.clear();
    ++rows;
  }
  return rows;
}

// Builds the manifest.json sidecar body for a set of projections (§7 "附
// manifest"). Lists each table's file name, kind, sampling axis, columns
// (name/unit), document id/revision, and the null rule.
[[nodiscard]] std::string
build_manifest(const std::vector<std::string> &files,
               const std::vector<TableProjection> &projections,
               const CsvExportOptions &options) {
  std::string out;
  out.append("{\"format\":\"welllog-csv-manifest\",\"version\":\"1.0\",");
  out.append("\"tables\":[");
  for (std::size_t i = 0; i < projections.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    const auto &p = projections[i];
    out.append("{\"file\":\"");
    escape_json(out, files[i]);
    out.append("\",\"kind\":\"curves\",\"samplingAxisId\":\"");
    out.append(p.sampling_axis_id().to_string());
    out.append("\",\"documentId\":\"");
    out.append(p.document_id().to_string());
    out.append("\",\"revision\":");
    append_integer(out, p.document_revision().value);
    out.append(",\"columns\":[");
    for (std::uint64_t c = 0; c < p.column_count(); ++c) {
      if (c != 0) {
        out.push_back(',');
      }
      const auto col = p.column(c);
      out.append("{\"name\":\"");
      escape_json(out, col.name);
      out.append("\",\"unit\":\"");
      escape_json(out, col.unit);
      out.append("\"}");
    }
    out.append("],\"nullToken\":\"");
    escape_json(out, options.null_token);
    out.append("\"}");
  }
  out.append("]}");
  return out;
}

} // namespace

Result<TableExportReport>
CsvTableExporter::write_to_file(const TableProjection &projection,
                                const std::filesystem::path &path,
                                CsvExportOptions options) {
  std::uint64_t rows = 0;
  const auto producer = [&](std::ostream &out) -> bool {
    rows = stream_csv(out, projection, options);
    return true;
  };
  const auto result = write_file_atomic(path, producer);
  if (!result.has_value()) {
    return result.error();
  }
  return TableExportReport{
      .path = result.value(),
      .exported_rows = rows,
      .exported_tables = 1,
      .document_revision = projection.document_revision(),
  };
}

Result<TableExportReport>
CsvPackageExporter::write_to_directory(
    const std::vector<TableProjection> &projections,
    const std::filesystem::path &directory, CsvExportOptions options) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) {
    return Error{
        .code = ErrorCode::internal_error,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::internal_error,
        .arguments = {},
    };
  }
  std::vector<std::string> files;
  std::uint64_t total_rows = 0;
  for (const auto &p : projections) {
    // File name: the axis id (stable) + .csv.
    std::string fname = p.sampling_axis_id().to_string();
    fname += ".csv";
    files.push_back(fname);
    const auto target = directory / fname;
    const auto r = CsvTableExporter::write_to_file(p, target, options);
    if (!r.has_value()) {
      return r.error();
    }
    total_rows += r.value().exported_rows;
  }
  // Sidecar manifest.
  const auto manifest_path = directory / "manifest.json";
  const auto manifest_body = build_manifest(files, projections, options);
  const auto manifest_result = write_file_atomic(
      manifest_path,
      [&](std::ostream &out) -> bool {
        out.write(manifest_body.data(),
                  static_cast<std::streamsize>(manifest_body.size()));
        return static_cast<bool>(out);
      });
  if (!manifest_result.has_value()) {
    return manifest_result.error();
  }
  return TableExportReport{
      .path = directory,
      .exported_rows = total_rows,
      .exported_tables = projections.size(),
      .document_revision = projections.empty()
                                ? DocumentRevision{}
                                : projections.front().document_revision(),
  };
}

} // namespace welllog
