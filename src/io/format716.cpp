#include <welllog/io/format716.hpp>

#include "adapter_common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace welllog {
namespace {

using io_detail::axis_direction;
using io_detail::invalid_document_error;
using io_detail::owned_nulls;
using io_detail::owned_values;
using io_detail::resource_exhausted_error;
using io_detail::stable_id;

constexpr std::size_t k_file_header_bytes = 128;
constexpr std::size_t k_curve_header_bytes = 64;
constexpr std::size_t k_well_name_bytes = 80;
constexpr std::size_t k_curve_name_bytes = 16;
constexpr std::size_t k_curve_unit_bytes = 16;
constexpr std::size_t k_reserved_file_begin = 104;
constexpr std::size_t k_reserved_curve_begin = 40;
constexpr float k_end_depth_tolerance = 1.0e-2F;

[[nodiscard]] Error invalid_716() { return invalid_document_error(); }
[[nodiscard]] Error exhausted() { return resource_exhausted_error(); }
[[nodiscard]] Error internal_failure() { return io_detail::internal_error(); }

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset,
                                     Format716Endian endian) {
  std::array<std::uint8_t, 4> raw{};
  for (std::size_t index = 0; index < 4; ++index) {
    raw[index] = static_cast<std::uint8_t>(bytes[offset + index]);
  }
  if (endian == Format716Endian::little) {
    return static_cast<std::uint32_t>(raw[0]) |
           (static_cast<std::uint32_t>(raw[1]) << 8U) |
           (static_cast<std::uint32_t>(raw[2]) << 16U) |
           (static_cast<std::uint32_t>(raw[3]) << 24U);
  }
  return (static_cast<std::uint32_t>(raw[0]) << 24U) |
         (static_cast<std::uint32_t>(raw[1]) << 16U) |
         (static_cast<std::uint32_t>(raw[2]) << 8U) |
         static_cast<std::uint32_t>(raw[3]);
}

[[nodiscard]] float read_f32(std::span<const std::byte> bytes,
                             std::size_t offset, Format716Endian endian) {
  const auto bits = read_u32(bytes, offset, endian);
  float value{};
  static_assert(sizeof(float) == 4);
  std::memcpy(&value, &bits, sizeof(float));
  return value;
}

[[nodiscard]] std::string_view
trim_ascii_field(std::string_view value) noexcept {
  while (!value.empty() &&
         (value.back() == '\0' || value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' || value.front() == '\0')) {
    value.remove_prefix(1);
  }
  return value;
}

[[nodiscard]] bool is_printable_ascii(std::string_view value) noexcept {
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte > 0x7eU) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string uppercase_ascii(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const auto character : value) {
    if (character >= 'a' && character <= 'z') {
      result.push_back(static_cast<char>(character - 'a' + 'A'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] bool is_depth_mnemonic(std::string_view mnemonic) {
  const auto upper = uppercase_ascii(mnemonic);
  return upper == "DEPT" || upper == "DEPTH" || upper == "MD";
}

[[nodiscard]] bool reserved_region_zero(std::span<const std::byte> bytes,
                                        std::size_t begin,
                                        std::size_t end) noexcept {
  for (std::size_t index = begin; index < end; ++index) {
    if (bytes[index] != std::byte{0}) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string source_identity(const BufferSourceReference &source,
                                          std::span<const std::byte> bytes) {
  // Bind identity to host provenance and a content fingerprint so reloads of
  // the same bytes + source metadata stay stable while content edits diverge.
  constexpr std::uint64_t offset = 1469598103934665603ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  std::uint64_t hash = offset;
  for (const auto byte : bytes) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= prime;
  }
  std::string identity;
  identity.reserve(source.uri.size() + source.checksum.size() + 32);
  identity.append(source.uri);
  identity.push_back('|');
  identity.append(source.checksum);
  identity.push_back('|');
  identity.append(std::to_string(hash));
  return identity;
}

struct ParsedHeader {
  std::string well_name;
  std::uint32_t curve_count{};
  float start_depth{};
  float end_depth{};
  float sample_interval{};
  float null_value{};
  std::uint32_t sample_count{};
  std::vector<std::string> mnemonics;
  std::vector<std::string> units;
  Format716DepthStrategy depth_strategy{Format716DepthStrategy::synthetic_depth};
  std::vector<Format716Diagnostic> diagnostics;
};

[[nodiscard]] Result<ParsedHeader>
parse_header(std::span<const std::byte> bytes, Format716Endian endian,
             Format716Limits limits) {
  if (bytes.size() < k_file_header_bytes) {
    return invalid_716();
  }
  if (static_cast<std::uint64_t>(bytes.size()) > limits.max_input_bytes) {
    return exhausted();
  }

  ParsedHeader header;
  const auto well_view = trim_ascii_field(std::string_view{
      reinterpret_cast<const char *>(bytes.data()), k_well_name_bytes});
  if (!is_printable_ascii(well_view)) {
    return invalid_716();
  }
  header.well_name.assign(well_view);

  header.curve_count = read_u32(bytes, 80, endian);
  header.start_depth = read_f32(bytes, 84, endian);
  header.end_depth = read_f32(bytes, 88, endian);
  header.sample_interval = read_f32(bytes, 92, endian);
  header.null_value = read_f32(bytes, 96, endian);
  header.sample_count = read_u32(bytes, 100, endian);

  if (header.curve_count == 0) {
    return invalid_716();
  }
  if (header.curve_count > limits.max_curves) {
    return exhausted();
  }
  if (header.sample_count == 0) {
    return invalid_716();
  }
  if (static_cast<std::uint64_t>(header.sample_count) > limits.max_samples) {
    return exhausted();
  }
  if (!std::isfinite(header.start_depth) || !std::isfinite(header.end_depth) ||
      !std::isfinite(header.sample_interval) ||
      !std::isfinite(header.null_value)) {
    return invalid_716();
  }

  if (!reserved_region_zero(bytes, k_reserved_file_begin, k_file_header_bytes)) {
    header.diagnostics.push_back(Format716Diagnostic{
        .code = Format716DiagnosticCode::reserved_bytes_nonzero,
        .severity = Severity::warning,
        .byte_offset = k_reserved_file_begin,
        .sample_index = 0,
        .curve_index = 0,
        .curve_name = {},
    });
  }

  // Checked size arithmetic (quality-security §6.2 / ADR 0042): reject on
  // overflow instead of wrapping into a length that matches a truncated buffer.
  const auto mul_u64 = [](std::uint64_t left,
                          std::uint64_t right) -> std::optional<std::uint64_t> {
    if (left != 0 && right > (std::numeric_limits<std::uint64_t>::max() / left)) {
      return std::nullopt;
    }
    return left * right;
  };
  const auto add_u64 = [](std::uint64_t left,
                          std::uint64_t right) -> std::optional<std::uint64_t> {
    if (right > (std::numeric_limits<std::uint64_t>::max() - left)) {
      return std::nullopt;
    }
    return left + right;
  };
  const auto headers_bytes =
      mul_u64(header.curve_count, k_curve_header_bytes);
  const auto samples_by_curves =
      mul_u64(header.sample_count, header.curve_count);
  const auto data_bytes =
      samples_by_curves.has_value()
          ? mul_u64(*samples_by_curves, sizeof(float))
          : std::nullopt;
  std::optional<std::uint64_t> expected_size;
  if (headers_bytes.has_value() && data_bytes.has_value()) {
    if (const auto body = add_u64(*headers_bytes, *data_bytes);
        body.has_value()) {
      expected_size = add_u64(k_file_header_bytes, *body);
    }
  }
  if (!expected_size.has_value()) {
    return exhausted();
  }
  if (static_cast<std::uint64_t>(bytes.size()) != *expected_size) {
    return invalid_716();
  }

  header.mnemonics.reserve(header.curve_count);
  header.units.reserve(header.curve_count);
  for (std::uint32_t curve = 0; curve < header.curve_count; ++curve) {
    const auto base =
        k_file_header_bytes + static_cast<std::size_t>(curve) * k_curve_header_bytes;
    const auto name_view = trim_ascii_field(std::string_view{
        reinterpret_cast<const char *>(bytes.data() + base), k_curve_name_bytes});
    const auto unit_view = trim_ascii_field(std::string_view{
        reinterpret_cast<const char *>(bytes.data() + base + k_curve_name_bytes),
        k_curve_unit_bytes});
    if (name_view.empty() || !is_printable_ascii(name_view) ||
        !is_printable_ascii(unit_view)) {
      return invalid_716();
    }
    if (name_view.size() == k_curve_name_bytes) {
      header.diagnostics.push_back(Format716Diagnostic{
          .code = Format716DiagnosticCode::truncated_curve_name,
          .severity = Severity::info,
          .byte_offset = static_cast<std::uint64_t>(base),
          .curve_index = curve,
          .curve_name = std::string{name_view},
      });
    }
    if (!reserved_region_zero(bytes, base + k_reserved_curve_begin,
                              base + k_curve_header_bytes)) {
      header.diagnostics.push_back(Format716Diagnostic{
          .code = Format716DiagnosticCode::reserved_bytes_nonzero,
          .severity = Severity::warning,
          .byte_offset = static_cast<std::uint64_t>(base + k_reserved_curve_begin),
          .curve_index = curve,
          .curve_name = std::string{name_view},
      });
    }
    header.mnemonics.emplace_back(name_view);
    header.units.emplace_back(unit_view);
  }

  if (is_depth_mnemonic(header.mnemonics.front())) {
    if (header.curve_count < 2) {
      return invalid_716();
    }
    header.depth_strategy = Format716DepthStrategy::depth_as_first_curve;
  } else {
    // Synthetic depth requires a non-zero interval that reaches end_depth.
    if (header.sample_interval == 0.0F) {
      return invalid_716();
    }
    const auto expected_end =
        header.start_depth +
        static_cast<float>(header.sample_count - 1U) * header.sample_interval;
    if (std::fabs(expected_end - header.end_depth) > k_end_depth_tolerance) {
      return invalid_716();
    }
    header.depth_strategy = Format716DepthStrategy::synthetic_depth;
  }

  return header;
}

[[nodiscard]] bool header_looks_plausible(const ParsedHeader &header) noexcept {
  if (header.curve_count == 0 || header.sample_count == 0) {
    return false;
  }
  if (!std::isfinite(header.start_depth) || !std::isfinite(header.end_depth)) {
    return false;
  }
  for (const auto &name : header.mnemonics) {
    if (name.empty() || !is_printable_ascii(name)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Result<Format716Import>
import_with_endian(std::span<const std::byte> bytes,
                   BufferSourceReference source, Format716Endian endian,
                   Format716Limits limits) {
  try {
    auto parsed = parse_header(bytes, endian, limits);
    if (!parsed.has_value()) {
      return parsed.error();
    }
    auto header = std::move(parsed.value());

    const auto data_offset =
        k_file_header_bytes +
        static_cast<std::size_t>(header.curve_count) * k_curve_header_bytes;

    std::vector<double> depths;
    depths.reserve(header.sample_count);
    std::vector<std::vector<double>> curve_values;
    std::vector<std::vector<std::uint8_t>> curve_nulls;
    std::vector<Format716Diagnostic> diagnostics = std::move(header.diagnostics);

    const auto read_sample_value = [&](std::uint32_t sample,
                                       std::uint32_t curve) -> float {
      const auto offset =
          data_offset +
          (static_cast<std::size_t>(sample) * header.curve_count + curve) *
              sizeof(float);
      return read_f32(bytes, offset, endian);
    };

    std::uint32_t first_measurement_curve = 0;
    std::string depth_unit = "m";
    if (header.depth_strategy == Format716DepthStrategy::depth_as_first_curve) {
      first_measurement_curve = 1;
      depth_unit = header.units.front().empty() ? "m" : header.units.front();
      for (std::uint32_t sample = 0; sample < header.sample_count; ++sample) {
        const auto depth = read_sample_value(sample, 0);
        if (!std::isfinite(depth)) {
          diagnostics.push_back(Format716Diagnostic{
              .code = Format716DiagnosticCode::non_finite_axis_value,
              .severity = Severity::error,
              .byte_offset = static_cast<std::uint64_t>(
                  data_offset + static_cast<std::size_t>(sample) *
                                    header.curve_count * sizeof(float)),
              .sample_index = sample,
              .curve_index = 0,
              .curve_name = header.mnemonics.front(),
          });
          return invalid_716();
        }
        depths.push_back(static_cast<double>(depth));
      }
    } else {
      for (std::uint32_t sample = 0; sample < header.sample_count; ++sample) {
        depths.push_back(static_cast<double>(header.start_depth) +
                         static_cast<double>(sample) *
                             static_cast<double>(header.sample_interval));
      }
    }

    const auto measurement_count =
        header.curve_count - first_measurement_curve;
    curve_values.resize(measurement_count);
    curve_nulls.resize(measurement_count);
    for (std::uint32_t curve = 0; curve < measurement_count; ++curve) {
      curve_values[curve].reserve(header.sample_count);
      curve_nulls[curve].reserve(header.sample_count);
    }

    for (std::uint32_t sample = 0; sample < header.sample_count; ++sample) {
      for (std::uint32_t curve = 0; curve < measurement_count; ++curve) {
        const auto source_curve = curve + first_measurement_curve;
        const auto raw = read_sample_value(sample, source_curve);
        const auto offset =
            data_offset +
            (static_cast<std::size_t>(sample) * header.curve_count +
             source_curve) *
                sizeof(float);
        // Header null_value is an explicit file sentinel (not an inference).
        // Apply it quietly; only non-finite samples need per-sample diagnostics.
        bool is_null = false;
        if (!std::isfinite(raw)) {
          is_null = true;
          diagnostics.push_back(Format716Diagnostic{
              .code = Format716DiagnosticCode::non_finite_curve_value,
              .severity = Severity::warning,
              .byte_offset = static_cast<std::uint64_t>(offset),
              .sample_index = sample,
              .curve_index = source_curve,
              .curve_name = header.mnemonics[source_curve],
          });
        } else if (raw == header.null_value) {
          is_null = true;
        }
        curve_values[curve].push_back(
            is_null ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(raw));
        curve_nulls[curve].push_back(is_null ? std::uint8_t{1}
                                             : std::uint8_t{0});
      }
    }

    const auto direction = axis_direction(depths);
    if (!direction.has_value()) {
      return invalid_716();
    }

    const auto identity = source_identity(source, bytes);
    const auto document_id = stable_id(identity, "document", 0);
    const auto axis_id = stable_id(identity, "sampling-axis", 0);
    WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
    const auto depth_owner =
        std::make_shared<const std::vector<double>>(std::move(depths));
    builder.add_sampling_axis(SamplingAxis{
        .id = axis_id,
        .coordinates = owned_values(depth_owner, source),
        .domain = DepthDomain::measured_depth,
        .unit = depth_unit,
        .direction = *direction,
    });

    for (std::uint32_t curve = 0; curve < measurement_count; ++curve) {
      const auto source_curve = curve + first_measurement_curve;
      const auto values = std::make_shared<const std::vector<double>>(
          std::move(curve_values[curve]));
      builder.add_curve(Curve{
          .id = stable_id(identity, "curve", source_curve),
          .mnemonic = header.mnemonics[source_curve],
          .display_name = header.mnemonics[source_curve],
          .unit = header.units[source_curve],
          .sampling_axis_id = axis_id,
          .values = owned_values(values, source),
          .nulls = owned_nulls(curve_nulls[curve], source),
      });
    }

    auto document = builder.build();
    if (document.id().is_nil()) {
      return exhausted();
    }
    return Format716Import{
        .document = std::move(document),
        .diagnostics = std::move(diagnostics),
        .endian_used = endian,
        .layout_used = Format716Layout::multi_curve_disk_v1,
        .depth_strategy_used = header.depth_strategy,
    };
  } catch (const std::bad_alloc &) {
    return exhausted();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace

Result<Format716Endian>
Format716SourceAdapter::detect_endian(std::span<const std::byte> bytes,
                                      Format716Limits limits) {
  const auto little = parse_header(bytes, Format716Endian::little, limits);
  const auto big = parse_header(bytes, Format716Endian::big, limits);
  const bool little_ok =
      little.has_value() && header_looks_plausible(little.value());
  const bool big_ok = big.has_value() && header_looks_plausible(big.value());
  if (little_ok && !big_ok) {
    return Format716Endian::little;
  }
  if (big_ok && !little_ok) {
    return Format716Endian::big;
  }
  // Zero matches or an ambiguous dual match must not silently pick a side.
  return invalid_716();
}

Result<Format716Inspection>
Format716SourceAdapter::inspect(std::span<const std::byte> bytes,
                                Format716Selection selection,
                                Format716Limits limits) {
  if (selection.layout != Format716Layout::multi_curve_disk_v1) {
    return invalid_716();
  }
  auto parsed = parse_header(bytes, selection.endian, limits);
  if (!parsed.has_value()) {
    return parsed.error();
  }
  auto header = std::move(parsed.value());
  return Format716Inspection{
      .well_name = std::move(header.well_name),
      .curve_count = header.curve_count,
      .sample_count = header.sample_count,
      .start_depth = header.start_depth,
      .end_depth = header.end_depth,
      .sample_interval = header.sample_interval,
      .null_value = header.null_value,
      .endian = selection.endian,
      .layout = Format716Layout::multi_curve_disk_v1,
      .depth_strategy = header.depth_strategy,
      .curve_mnemonics = std::move(header.mnemonics),
      .curve_units = std::move(header.units),
      .diagnostics = std::move(header.diagnostics),
  };
}

Result<Format716Import>
Format716SourceAdapter::import(std::span<const std::byte> bytes,
                               BufferSourceReference source,
                               Format716Selection selection,
                               Format716Limits limits) {
  if (selection.layout != Format716Layout::multi_curve_disk_v1) {
    return invalid_716();
  }
  return import_with_endian(bytes, std::move(source), selection.endian, limits);
}

} // namespace welllog
