#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

// Stable DLIS object identity within a Logical File. DLIS terminology remains
// confined to the IO adapter; Core and Session only receive EntityId values.
struct DlisObjectReference {
  std::int32_t origin{};
  std::uint8_t copy_number{};
  std::string identifier;

  friend bool operator==(const DlisObjectReference &,
                         const DlisObjectReference &) = default;
};

struct DlisChannelDescriptor {
  DlisObjectReference reference;
  std::string unit;
  // RP66 `DIMENSION` as supplied by the Channel Object. An absent source
  // attribute remains empty; scalar sources commonly carry `{1}`, while
  // arrays retain their full source shape for explicit host handling.
  std::vector<std::uint32_t> dimensions;
};

struct DlisFrameDescriptor {
  DlisObjectReference reference;
  std::string index_type;
  std::string direction;
  std::vector<DlisChannelDescriptor> channels;
};

struct DlisLogicalFileDescriptor {
  std::uint32_t index{};
  std::vector<DlisFrameDescriptor> frames;
};

struct DlisCatalog {
  std::vector<DlisLogicalFileDescriptor> logical_files;
};

enum class DlisDiagnosticCode : std::uint8_t {
  encrypted_record,
  unsupported_object_type,
  unsupported_channel_representation,
  unsupported_channel_dimension,
  malformed_frame_data,
  non_finite_axis_value,
  non_finite_curve_value,
};

// A recoverable source-format condition. byte_offset identifies the affected
// record in the supplied source bytes; absent frame/channel references are
// represented by their default-empty values. Unsupported object types carry
// empty frame/channel references because they do not participate in an import
// selection.
struct DlisDiagnostic {
  DlisDiagnosticCode code{DlisDiagnosticCode::malformed_frame_data};
  Severity severity{Severity::warning};
  std::uint64_t byte_offset{};
  std::uint32_t logical_file_index{};
  DlisObjectReference frame;
  DlisObjectReference channel;
};

struct DlisInspection {
  DlisCatalog catalog;
  std::vector<DlisDiagnostic> diagnostics;
};

struct DlisSelection {
  // No Logical File is selected by default. Import rejects this sentinel so a
  // host can never silently choose an ambiguous source.
  std::uint32_t logical_file_index{UINT32_MAX};
  DlisObjectReference frame;
  std::vector<DlisObjectReference> channels;
};

struct DlisImport {
  WellLogDocument document;
  std::vector<DlisDiagnostic> diagnostics;
};

// Conservative ceilings for untrusted DLIS data. Limits apply before any
// numeric buffers are materialized; a structural violation returns an Error
// while source-local unsupported fields become DlisDiagnostic entries.
struct DlisLimits {
  std::uint64_t max_input_bytes{64U * 1024U * 1024U};
  std::uint64_t max_logical_record_bytes{1024U * 1024U};
  std::uint64_t max_logical_records{100'000U};
  std::uint64_t max_samples{10'000'000U};
};

// A bounded RP66/DLIS source adapter. `inspect` exposes all selectable
// Logical Files, Frames and Channels. `import` requires a specific selection
// and emits only the normalized WellLogDocument + recoverable diagnostics.
class WELLLOG_IO_API DlisSourceAdapter {
public:
  [[nodiscard]] static Result<DlisInspection>
  inspect(std::span<const std::byte> bytes, DlisLimits limits = {});

  [[nodiscard]] static Result<DlisImport>
  import(std::span<const std::byte> bytes, BufferSourceReference source,
         const DlisSelection &selection, DlisLimits limits = {});
};

} // namespace welllog
