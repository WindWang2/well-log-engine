#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

enum class LasDiagnosticCode : std::uint8_t {
  short_ascii_row,
  long_ascii_row,
  invalid_ascii_value,
  non_finite_curve_value,
  non_finite_depth_value,
  incomplete_wrapped_row,
};

// A recoverable condition in an otherwise usable LAS input. `line` is the
// one-based source line, and `column` is the one-based LAS column when one is
// meaningful (zero for row-level diagnostics).
struct LasDiagnostic {
  LasDiagnosticCode code{LasDiagnosticCode::invalid_ascii_value};
  Severity severity{Severity::warning};
  std::uint64_t line{};
  std::uint64_t column{};
};

// The normalized document and the recoverable input problems observed while
// producing it. Structural LAS failures are returned as Result errors instead.
struct LasImport {
  WellLogDocument document;
  std::vector<LasDiagnostic> diagnostics;
};

// A format adapter for LAS 2.0/3.0 ASCII files. It owns converted numeric
// buffers, which retain the caller-supplied source reference for provenance;
// LAS vocabulary itself never crosses into Core or Session APIs (ADR 0005).
class WELLLOG_IO_API LasSourceAdapter {
public:
  [[nodiscard]] static Result<LasImport>
  parse(std::string_view text, BufferSourceReference source);
};

} // namespace welllog
