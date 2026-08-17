#pragma once

// Format716SourceAdapter — bounded importer for Chinese multi-curve "716"
// disk files (Atlas-3700-era workstation interchange).
//
// Supported profile: welllog-716-disk-v1 (layout + field layout documented
// below). Vendor extensions, tape images, and unconfirmed variants are
// rejected rather than guessed (ADR 0005; same policy as LIS/DLIS adapters).
//
// File layout (welllog-716-disk-v1):
//   FileHeader 128 bytes
//     [0..79]    well name, ASCII space/NUL padded
//     [80..83]   curve_count        int32
//     [84..87]   start_depth        float32
//     [88..91]   end_depth          float32
//     [92..95]   sample_interval    float32 (sign encodes axis direction)
//     [96..99]   null_value         float32
//     [100..103] sample_count       int32
//     [104..127] reserved (must be zero)
//   CurveHeader 64 bytes × curve_count
//     [0..15]  mnemonic ASCII
//     [16..31] unit ASCII
//     [32..35] display min float32 (metadata only)
//     [36..39] display max float32 (metadata only)
//     [40..63] reserved (must be zero)
//   Data: IEEE-754 float32, sample-major (depth-major):
//     for sample i in [0, sample_count):
//       for curve c in [0, curve_count): value[i][c]
//
// Depth axis:
//   - If the first curve mnemonic is a depth name (DEPT/DEPTH/MD, case
//     insensitive), those samples become the Sampling Axis and remaining
//     curves become Curve entities (strategy = depth_as_first_curve).
//   - Otherwise depths are synthesized as start + i * sample_interval
//     (strategy = synthetic_depth) and every curve is a measurement curve.
//     The axis unit is "unknown" (the file header has no depth-unit field).
//
// Endianness is never silently guessed when both interpretations look
// valid: the host must choose Format716Endian, or use detect_endian which
// returns a unique match or an error.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

enum class Format716Endian : std::uint8_t {
  little,
  big,
};

// The only layout accepted in phase one. Explicit so hosts cannot rely on
// silent layout guessing when more profiles are added later.
enum class Format716Layout : std::uint8_t {
  multi_curve_disk_v1,
};

enum class Format716DepthStrategy : std::uint8_t {
  synthetic_depth,
  depth_as_first_curve,
};

enum class Format716DiagnosticCode : std::uint8_t {
  non_finite_axis_value,
  non_finite_curve_value,
  reserved_bytes_nonzero,
  truncated_curve_name,
  // Synthetic-depth files have no depth-unit field; the axis unit is
  // "unknown" rather than a guessed metre label.
  synthetic_depth_unit_unknown,
};

struct Format716Diagnostic {
  Format716DiagnosticCode code{Format716DiagnosticCode::non_finite_curve_value};
  Severity severity{Severity::warning};
  std::uint64_t byte_offset{};
  std::uint32_t sample_index{};
  std::uint32_t curve_index{};
  std::string curve_name;
};

struct Format716Limits {
  std::uint64_t max_input_bytes{64U * 1024U * 1024U};
  std::uint32_t max_curves{4'096U};
  std::uint64_t max_samples{10'000'000U};
};

struct Format716Selection {
  Format716Endian endian{Format716Endian::little};
  Format716Layout layout{Format716Layout::multi_curve_disk_v1};
};

struct Format716Inspection {
  std::string well_name;
  std::uint32_t curve_count{};
  std::uint64_t sample_count{};
  double start_depth{};
  double end_depth{};
  double sample_interval{};
  double null_value{};
  Format716Endian endian{Format716Endian::little};
  Format716Layout layout{Format716Layout::multi_curve_disk_v1};
  Format716DepthStrategy depth_strategy{Format716DepthStrategy::synthetic_depth};
  std::vector<std::string> curve_mnemonics;
  std::vector<std::string> curve_units;
  std::vector<Format716Diagnostic> diagnostics;
};

struct Format716Import {
  WellLogDocument document;
  std::vector<Format716Diagnostic> diagnostics;
  // The strategy that actually produced the document — hosts must surface
  // this so reloads never hide a silent reinterpretation.
  Format716Endian endian_used{Format716Endian::little};
  Format716Layout layout_used{Format716Layout::multi_curve_disk_v1};
  Format716DepthStrategy depth_strategy_used{
      Format716DepthStrategy::synthetic_depth};
};

// A bounded 716 source adapter. Format-private types never leave this seam;
// Core and Session only receive WellLogDocument + diagnostics (ADR 0005).
class WELLLOG_IO_API Format716SourceAdapter {
public:
  // Prefer this when the host does not yet know endianness. Returns a unique
  // matching endian or ErrorCode::invalid_document when zero or two matches.
  [[nodiscard]] static Result<Format716Endian>
  detect_endian(std::span<const std::byte> bytes, Format716Limits limits = {});

  [[nodiscard]] static Result<Format716Inspection>
  inspect(std::span<const std::byte> bytes, Format716Selection selection = {},
          Format716Limits limits = {});

  [[nodiscard]] static Result<Format716Import>
  import(std::span<const std::byte> bytes, BufferSourceReference source,
         Format716Selection selection = {}, Format716Limits limits = {});
};

} // namespace welllog
