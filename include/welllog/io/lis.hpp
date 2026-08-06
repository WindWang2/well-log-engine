#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

enum class LisDiagnosticCode : std::uint8_t {
  unknown_record,
  unsupported_channel,
  non_finite_axis_value,
  non_finite_curve_value,
  unknown_index_semantics,
  constant_axis,
  non_ascii_text,
  inferred_null,
  alias_normalized,
  unit_normalized,
  unknown_curve_semantics,
  normalization_conflict,
  redundant_format_specification,
  replaced_format_specification,
  malformed_dataset,
  no_importable_curve,
};

struct LisDiagnostic {
  LisDiagnosticCode code{LisDiagnosticCode::unknown_record};
  Severity severity{Severity::warning};
  std::uint64_t byte_offset{};
  std::uint32_t logical_file_index{};
  std::uint32_t data_set_ordinal{};
  std::uint8_t representation{};
  std::string channel_name;
};

struct LisLogicalFileDescriptor {
  std::uint32_t index{};
  std::uint64_t byte_offset{};
  std::string display_name;
};

struct LisCatalog {
  std::vector<LisLogicalFileDescriptor> logical_files;
};

struct LisInspection {
  LisCatalog catalog;
  std::vector<LisDiagnostic> diagnostics;
};

// v1 preserves the original LIS adapter identity preimage for persisted
// references. v2 binds imported entities to the supplied source bytes as well
// as source metadata and is the default for new imports.
enum class LisIdentityScheme : std::uint8_t {
  resform_compatible_v1,
  content_bound_v2,
};

struct LisSelection {
  // When exactly one logical file exists, this sentinel selects it. Multiple
  // files always require the host to choose an explicit catalog index.
  std::uint32_t logical_file_index{UINT32_MAX};
  LisIdentityScheme identity_scheme{LisIdentityScheme::content_bound_v2};
};

struct LisAliasRule {
  std::string source_mnemonic;
  std::string canonical_mnemonic;
};

struct LisUnitRule {
  std::string canonical_mnemonic;
  std::string source_unit;
  std::string canonical_unit;
  double multiplier{1.0};
};

enum class LisTextEncoding : std::uint8_t {
  ascii,
  iso_8859_1,
};

// A value object whose rules influence both normalization and stable imported
// identity. Custom rules precede the built-in ResForm-compatible v1 rules and
// are validated as a complete value before the source stream is read.
struct LisNormalizationProfile {
  std::string name{"resform-compatible-v1"};
  std::string version{"1"};
  std::vector<double> inferred_null_values{-999.25, -999.0, -9999.0, -99999.0};
  std::vector<LisAliasRule> aliases;
  std::vector<LisUnitRule> unit_rules;
  // ASCII keeps non-ASCII bytes untouched and reports them. ISO-8859-1 is the
  // currently supported explicit single-byte decoding choice.
  LisTextEncoding text_encoding{LisTextEncoding::ascii};
};

[[nodiscard]] WELLLOG_IO_API const LisNormalizationProfile &
default_lis_normalization_profile() noexcept;

struct LisImport {
  WellLogDocument document;
  std::vector<LisDiagnostic> diagnostics;
};

struct LisLimits {
  std::uint64_t max_input_bytes{256U * 1024U * 1024U};
  std::uint64_t max_logical_record_bytes{4U * 1024U * 1024U};
  std::uint64_t max_logical_records{1'000'000U};
  std::uint64_t max_samples_per_data_set{10'000'000U};
  std::uint64_t max_curves_per_data_set{4'096U};
};

// A bounded LIS79 source adapter. It exposes logical-file selection at the IO
// seam and emits only normalized Core types, never LIS records or structures.
class WELLLOG_IO_API LisSourceAdapter {
public:
  [[nodiscard]] static Result<LisInspection>
  inspect(std::span<const std::byte> bytes, LisLimits limits = {});

  [[nodiscard]] static Result<LisImport>
  import(std::span<const std::byte> bytes, BufferSourceReference source,
         LisSelection selection = {},
         const LisNormalizationProfile &profile =
             default_lis_normalization_profile(),
         LisLimits limits = {});
};

} // namespace welllog
