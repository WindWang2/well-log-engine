#pragma once

// Untrusted container guards for Manifest/XML/ZIP/XLSX packages and path
// strings (#171, ADR 0042). Pure validation — no host process abort.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

// --- Limits (shared policy) -------------------------------------------------

struct ContainerSecurityLimits {
  // Manifest JSON
  std::uint64_t max_manifest_bytes{16ULL * 1024ULL * 1024ULL};
  std::uint32_t max_json_depth{64};
  std::uint64_t max_json_array_elements{100'000};
  std::uint64_t max_json_object_keys{10'000};
  std::uint64_t max_json_string_bytes{1ULL * 1024ULL * 1024ULL};
  // ZIP / XLSX packages
  std::uint64_t max_zip_archive_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint32_t max_zip_entries{4'096};
  // Single OOXML worksheet bodies for large projections can exceed 32 MiB;
  // keep a high but finite ceiling for zip-bomb defense.
  std::uint64_t max_zip_entry_uncompressed_bytes{256ULL * 1024ULL * 1024ULL};
  std::uint64_t max_zip_total_uncompressed_bytes{512ULL * 1024ULL * 1024ULL};
  // Reject entries whose uncompressed/compressed ratio exceeds this when
  // compressed_size > 0 (zip bomb heuristic).
  double max_zip_compression_ratio{200.0};
  // mmap / columnar files
  std::uint64_t max_mmap_file_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
  // Buffer descriptor extent
  std::uint64_t max_buffer_byte_capacity{2ULL * 1024ULL * 1024ULL * 1024ULL};
};

[[nodiscard]] WELLLOG_IO_API const ContainerSecurityLimits &
default_container_security_limits() noexcept;

// --- Path / entry name ------------------------------------------------------

// Rejects absolute paths, ".." segments, backslashes, empty names, and
// control characters. ZIP entry names must be relative POSIX-style paths.
[[nodiscard]] WELLLOG_IO_API bool
is_safe_archive_entry_name(std::string_view name) noexcept;

// --- XML threat scan (untrusted inbound text) -------------------------------

// Rejects DOCTYPE, ENTITY declarations, SYSTEM/PUBLIC external IDs, and
// xinclude-style network markers. Safe wellLogTables exports pass.
// Returns nullopt on success, Error on policy violation.
[[nodiscard]] WELLLOG_IO_API std::optional<Error>
scan_untrusted_xml(std::string_view xml) noexcept;

// --- ZIP inspect (untrusted inbound archive) --------------------------------

struct ZipEntryInfo {
  std::string name;
  std::uint64_t compressed_size{};
  std::uint64_t uncompressed_size{};
  std::uint16_t method{}; // 0 store, 8 deflate
};

struct ZipInspectReport {
  std::vector<ZipEntryInfo> entries;
  std::uint64_t total_uncompressed_bytes{};
  std::uint64_t total_compressed_bytes{};
};

// Parses central directory / EOCD without fully inflating payload bodies.
// Enforces entry-count, path, size, and compression-ratio limits. Returns
// resource_exhausted / invalid_manifest on policy violations (never throws).
[[nodiscard]] WELLLOG_IO_API Result<ZipInspectReport>
inspect_untrusted_zip(std::span<const std::byte> archive,
                      const ContainerSecurityLimits &limits =
                          default_container_security_limits()) noexcept;

// Inflates one stored/deflated entry into `out` with a hard uncompressed cap.
// Used by tests and future XLSX import — not for streaming production loads.
[[nodiscard]] WELLLOG_IO_API Result<std::string>
inflate_zip_entry(std::span<const std::byte> archive, std::string_view name,
                  std::uint64_t max_uncompressed_bytes =
                      default_container_security_limits()
                          .max_zip_entry_uncompressed_bytes) noexcept;

// --- Buffer descriptor extent -----------------------------------------------

// Validates length/stride/capacity for a typed buffer description.
// Returns nullopt on success.
[[nodiscard]] WELLLOG_IO_API std::optional<Error>
validate_buffer_extent(std::uint64_t length, std::uint64_t stride_bytes,
                       std::uint64_t element_size,
                       std::uint64_t byte_capacity,
                       const ContainerSecurityLimits &limits =
                           default_container_security_limits()) noexcept;

} // namespace welllog
