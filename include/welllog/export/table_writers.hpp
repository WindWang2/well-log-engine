#pragma once

// Table export backends (ADR 0022 / table-and-export.md §5-§7, §10, #155).
// Three writers over a core welllog::TableProjection — the data projection of
// a Document Revision — each streaming rows from the RAW curve buffer (zero
// copy, never LOD points), writing to a temp file with atomic rename (§10),
// preserving units / null semantics / identity / Reference Depth:
//   - CSV  (§7): one file per Table Projection; multi-table → directory + manifest
//   - XML  (§6): versioned, hardened (no DTD/entities/network), round-trippable
//   - XLSX (§5): self-contained OOXML+zlib; >1,048,576-row sheet splitting
//
// All three are Qt-agnostic (core only); the host drives them off a
// TableProjection produced by TableProjectionBuilder. A selection-restricted
// projection is produced by filtering rows over the Phase-B SelectionState
// (range↔row mapping), so "export the selection" is just exporting a
// sub-range projection.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/export/table_export.hpp>
#include <welllog/table/table_projection.hpp>

namespace welllog {

// Outcome of a table export (table-and-export.md §10: the result reports the
// revision/exported counts; the error Result carries path/format/stage, never
// raw data). Lightweight — a writer emits one per file.
struct TableExportReport {
  // The path actually written (after the atomic rename).
  std::filesystem::path path;
  std::uint64_t exported_rows{};
  std::uint64_t exported_tables{};
  DocumentRevision document_revision;
};

// CSV writer (§7). One CSV expresses exactly one Table Projection. UTF-8;
// deterministic delimiter/decimal-point/LF. Null cells emit the null token
// (never empty, never a value). Metadata for multi-table packages lives in a
// sidecar manifest, never CSV comment lines (§7).
struct CsvExportOptions {
  // Token emitted for a null cell. Must not collide with a legitimate value;
  // the writer RFC-4180-quotes any field that equals the token as data so a
  // literal null value stays distinct from a null sample (§7).
  std::string null_token = "null";
  char delimiter = ',';
};

class WELLLOG_EXPORT_TABLE_API CsvTableExporter {
public:
  // Streams the projection to `path` as one CSV (header row of column names +
  // one row per sample). Writes atomically (temp file + rename, §10). Returns
  // the report or an error (path/stage carried on the Error).
  [[nodiscard]] static Result<TableExportReport>
  write_to_file(const TableProjection &projection,
                const std::filesystem::path &path,
                CsvExportOptions options = {});
};

// Writes a package of projections to a directory: one CSV per projection
// (named `<axis>.csv`) plus a `manifest.json` sidecar listing the tables,
// their Sampling Axis, columns/units, document id/revision, and the null rule
// (§7 "多表导出使用目录或 ZIP 包，并附 manifest"). (ZIP packaging is a follow-up.)
class WELLLOG_EXPORT_TABLE_API CsvPackageExporter {
public:
  [[nodiscard]] static Result<TableExportReport>
  write_to_directory(const std::vector<TableProjection> &projections,
                     const std::filesystem::path &directory,
                     CsvExportOptions options = {});
};

// Versioned XML table exchange format (§6). NOT SpreadsheetML — a streaming,
// hardened writer that emits one <wellLogTables schemaVersion="1.0"> document
// grouping all of a well's projections. Disables DTD, external entities, and
// network resources by construction (the writer never emits them; attribute
// text is XML-escaped and control characters rejected). Round-trippable: a
// small/repeat-depth/multi-axis/null document can be written and read back to
// the same depths/nulls/units/identity.
struct XmlTableExportOptions {
  std::string schema_version = "1.0";
};

class WELLLOG_EXPORT_TABLE_API XmlTableExporter {
public:
  // Streams the document + its projections to `path` as one XML document. Each
  // table's columns carry id/name/unit/type (type from TableColumn.scalar_type);
  // rows stream <v> (finite) / <null/> (null sample) from raw cell() reads.
  [[nodiscard]] static Result<TableExportReport>
  write_to_file(const WellLogDocument &document,
                const std::vector<TableProjection> &projections,
                const std::filesystem::path &path,
                XmlTableExportOptions options = {});
};

// XLSX (OOXML SpreadsheetML) table export (§5). Self-contained — a minimal
// hand-written zip (PRIVATE ZLIB deflate) produces the OOXML parts; no
// third-party spreadsheet library (ADR 0041 reproducible deps). One workbook
// per document; one worksheet per Sampling Axis for curves, plus a Metadata
// worksheet carrying units/axis/revision/null rule. Numeric cells are numeric
// (not pre-formatted strings); null cells are empty (§5.2). A projection
// exceeding 1,048,576 rows splits into _01/_02... continuation sheets, each
// repeating column definitions and recording the global start row in the
// metadata sheet (§5.1). Phase-A scope: curve sheets only (interval/marker/
// annotation sheets land when those tables do).
class WELLLOG_EXPORT_TABLE_API XlsxTableExporter {
public:
  [[nodiscard]] static Result<TableExportReport>
  write_to_file(const WellLogDocument &document,
                const std::vector<TableProjection> &projections,
                const std::filesystem::path &path);
};

} // namespace welllog
