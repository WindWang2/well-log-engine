#include <welllog/io/container_security.hpp>

#include <welllog/core/checked_math.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>

#include <zlib.h>

namespace welllog {
namespace {

[[nodiscard]] Error security_error(ErrorCode code, MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

[[nodiscard]] std::uint16_t read_u16_le(const std::byte *p) {
  // Cast the full expression: the `|` of two promoted ints is an `int` under
  // usual arithmetic conversions; Clang -Wimplicit-int-conversion flags that.
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0])) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])) << 8));
}

[[nodiscard]] std::uint32_t read_u32_le(const std::byte *p) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24);
}

[[nodiscard]] bool contains_ci(std::string_view hay, std::string_view needle) {
  if (needle.empty() || hay.size() < needle.size()) {
    return false;
  }
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      const auto a = static_cast<unsigned char>(hay[i + j]);
      const auto b = static_cast<unsigned char>(needle[j]);
      if (std::tolower(a) != std::tolower(b)) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

} // namespace

const ContainerSecurityLimits &default_container_security_limits() noexcept {
  static const ContainerSecurityLimits limits{};
  return limits;
}

bool is_safe_archive_entry_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 512) {
    return false;
  }
  if (name.front() == '/' || name.front() == '\\') {
    return false;
  }
  // Drive-letter absolute path (Windows).
  if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) != 0 &&
      name[1] == ':') {
    return false;
  }
  for (const auto c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\') {
      return false;
    }
  }
  // Segment-wise ".." rejection.
  std::size_t start = 0;
  while (start <= name.size()) {
    const auto slash = name.find('/', start);
    const auto end = slash == std::string_view::npos ? name.size() : slash;
    const auto segment = name.substr(start, end - start);
    if (segment == ".." || segment == ".") {
      // Allow "." only as empty relative? Reject both for safety.
      if (segment == "..") {
        return false;
      }
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  if (name.find("..") != std::string_view::npos) {
    // Catch "foo/../bar" already handled; also "foo/..bar" is ok, but
    // "foo/../bar" has segment .. . Double-check path with simple scan:
    std::size_t pos = 0;
    while ((pos = name.find("..", pos)) != std::string_view::npos) {
      const bool left_ok = pos == 0 || name[pos - 1] == '/';
      const bool right_ok =
          pos + 2 >= name.size() || name[pos + 2] == '/';
      if (left_ok && right_ok) {
        return false;
      }
      pos += 2;
    }
  }
  return true;
}

std::optional<Error> scan_untrusted_xml(std::string_view xml) noexcept {
  try {
    if (xml.size() > default_container_security_limits().max_manifest_bytes) {
      return security_error(ErrorCode::resource_exhausted,
                            MessageKey::resource_exhausted);
    }
    // XXE / DTD / external resource markers (case-insensitive).
    if (contains_ci(xml, "<!DOCTYPE") || contains_ci(xml, "<!ENTITY") ||
        contains_ci(xml, "<!ELEMENT") || contains_ci(xml, "<!ATTLIST") ||
        contains_ci(xml, "xi:include") || contains_ci(xml, "xinclude")) {
      return security_error(ErrorCode::invalid_manifest,
                            MessageKey::manifest_invalid);
    }
    // External ID on entity/doctype lines only (already rejected above for
    // ENTITY/DOCTYPE). Bare SYSTEM/PUBLIC without those are ignored.
    return std::nullopt;
  } catch (...) {
    return security_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

Result<ZipInspectReport>
inspect_untrusted_zip(std::span<const std::byte> archive,
                      const ContainerSecurityLimits &limits) noexcept {
  try {
    if (archive.size() > limits.max_zip_archive_bytes) {
      return security_error(ErrorCode::resource_exhausted,
                            MessageKey::resource_exhausted);
    }
    if (archive.size() < 22) {
      return security_error(ErrorCode::invalid_manifest,
                            MessageKey::manifest_invalid);
    }
    // Find EOCD signature 0x06054b50 scanning backward (comment ≤ 64k).
    const auto *bytes = archive.data();
    const auto n = archive.size();
    std::size_t eocd = std::string::npos;
    const auto scan_from = n > 65557 ? n - 65557 : 0;
    for (std::size_t i = n - 22; i + 1 > scan_from; --i) {
      if (read_u32_le(bytes + i) == 0x06054b50U) {
        eocd = i;
        break;
      }
      if (i == 0) {
        break;
      }
    }
    if (eocd == std::string::npos) {
      return security_error(ErrorCode::invalid_manifest,
                            MessageKey::manifest_invalid);
    }
    const auto total_entries = read_u16_le(bytes + eocd + 10);
    const auto cd_size = read_u32_le(bytes + eocd + 12);
    const auto cd_offset = read_u32_le(bytes + eocd + 16);
    if (total_entries > limits.max_zip_entries) {
      return security_error(ErrorCode::resource_exhausted,
                            MessageKey::resource_exhausted);
    }
    if (static_cast<std::uint64_t>(cd_offset) + cd_size > n) {
      return security_error(ErrorCode::invalid_manifest,
                            MessageKey::manifest_invalid);
    }
    ZipInspectReport report;
    report.entries.reserve(total_entries);
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_compressed = 0;
    std::size_t pos = cd_offset;
    for (std::uint16_t e = 0; e < total_entries; ++e) {
      if (pos + 46 > n || read_u32_le(bytes + pos) != 0x02014b50U) {
        return security_error(ErrorCode::invalid_manifest,
                              MessageKey::manifest_invalid);
      }
      const auto method = read_u16_le(bytes + pos + 10);
      const auto comp = static_cast<std::uint64_t>(read_u32_le(bytes + pos + 20));
      const auto uncomp =
          static_cast<std::uint64_t>(read_u32_le(bytes + pos + 24));
      const auto name_len = read_u16_le(bytes + pos + 28);
      const auto extra_len = read_u16_le(bytes + pos + 30);
      const auto comment_len = read_u16_le(bytes + pos + 32);
      if (pos + 46 + name_len + extra_len + comment_len > n) {
        return security_error(ErrorCode::invalid_manifest,
                              MessageKey::manifest_invalid);
      }
      const auto name = std::string_view{
          reinterpret_cast<const char *>(bytes + pos + 46), name_len};
      if (!is_safe_archive_entry_name(name)) {
        return security_error(ErrorCode::invalid_manifest,
                              MessageKey::manifest_invalid);
      }
      if (uncomp > limits.max_zip_entry_uncompressed_bytes) {
        return security_error(ErrorCode::resource_exhausted,
                              MessageKey::resource_exhausted);
      }
      if (comp > 0 && uncomp > 0) {
        const auto ratio =
            static_cast<double>(uncomp) / static_cast<double>(comp);
        if (ratio > limits.max_zip_compression_ratio) {
          return security_error(ErrorCode::resource_exhausted,
                                MessageKey::resource_exhausted);
        }
      }
      const auto next_total = checked_add_u64(total_uncompressed, uncomp);
      if (!next_total.has_value() ||
          *next_total > limits.max_zip_total_uncompressed_bytes) {
        return security_error(ErrorCode::resource_exhausted,
                              MessageKey::resource_exhausted);
      }
      total_uncompressed = *next_total;
      total_compressed =
          checked_add_u64(total_compressed, comp).value_or(total_compressed);
      report.entries.push_back(ZipEntryInfo{
          .name = std::string{name},
          .compressed_size = comp,
          .uncompressed_size = uncomp,
          .method = method,
      });
      pos += static_cast<std::size_t>(46) + name_len + extra_len + comment_len;
    }
    report.total_uncompressed_bytes = total_uncompressed;
    report.total_compressed_bytes = total_compressed;
    return report;
  } catch (const std::bad_alloc &) {
    return security_error(ErrorCode::resource_exhausted,
                          MessageKey::resource_exhausted);
  } catch (...) {
    return security_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

Result<std::string>
inflate_zip_entry(std::span<const std::byte> archive, std::string_view name,
                  std::uint64_t max_uncompressed_bytes) noexcept {
  try {
    auto report = inspect_untrusted_zip(archive);
    if (!report.has_value()) {
      return report.error();
    }
    // Locate local file header for the named entry.
    const auto *bytes = archive.data();
    const auto n = archive.size();
    std::size_t pos = 0;
    while (pos + 30 <= n && read_u32_le(bytes + pos) == 0x04034b50U) {
      const auto method = read_u16_le(bytes + pos + 8);
      const auto comp = static_cast<std::uint64_t>(read_u32_le(bytes + pos + 18));
      const auto uncomp =
          static_cast<std::uint64_t>(read_u32_le(bytes + pos + 22));
      const auto name_len = read_u16_le(bytes + pos + 26);
      const auto extra_len = read_u16_le(bytes + pos + 28);
      if (pos + 30 + name_len + extra_len + comp > n) {
        return security_error(ErrorCode::invalid_manifest,
                              MessageKey::manifest_invalid);
      }
      const auto entry_name = std::string_view{
          reinterpret_cast<const char *>(bytes + pos + 30), name_len};
      const auto data_off = pos + 30 + name_len + extra_len;
      if (entry_name == name) {
        if (uncomp > max_uncompressed_bytes) {
          return security_error(ErrorCode::resource_exhausted,
                                MessageKey::resource_exhausted);
        }
        const auto *src = reinterpret_cast<const Bytef *>(bytes + data_off);
        if (method == 0) {
          return std::string{reinterpret_cast<const char *>(src),
                             static_cast<std::size_t>(comp)};
        }
        if (method != 8) {
          return security_error(ErrorCode::invalid_manifest,
                                MessageKey::manifest_invalid);
        }
        std::string out;
        out.resize(static_cast<std::size_t>(uncomp));
        z_stream stream{};
        stream.next_in = const_cast<Bytef *>(src);
        stream.avail_in = static_cast<uInt>(comp);
        stream.next_out = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = static_cast<uInt>(uncomp);
        if (inflateInit2(&stream, -15) != Z_OK) {
          return security_error(ErrorCode::invalid_manifest,
                                MessageKey::manifest_invalid);
        }
        const auto rc = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (rc != Z_STREAM_END || stream.total_out != uncomp) {
          return security_error(ErrorCode::invalid_manifest,
                                MessageKey::manifest_invalid);
        }
        return out;
      }
      pos = data_off + static_cast<std::size_t>(comp);
    }
    return security_error(ErrorCode::invalid_manifest,
                          MessageKey::manifest_invalid);
  } catch (const std::bad_alloc &) {
    return security_error(ErrorCode::resource_exhausted,
                          MessageKey::resource_exhausted);
  } catch (...) {
    return security_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

std::optional<Error>
validate_buffer_extent(std::uint64_t length, std::uint64_t stride_bytes,
                       std::uint64_t element_size, std::uint64_t byte_capacity,
                       const ContainerSecurityLimits &limits) noexcept {
  if (element_size == 0 || stride_bytes == 0) {
    return security_error(ErrorCode::invalid_buffer,
                          MessageKey::buffer_data_required);
  }
  if (byte_capacity > limits.max_buffer_byte_capacity) {
    return security_error(ErrorCode::resource_exhausted,
                          MessageKey::resource_exhausted);
  }
  const auto extent =
      checked_strided_extent_u64(length, stride_bytes, element_size);
  if (!extent.has_value()) {
    return security_error(ErrorCode::arithmetic_overflow,
                          MessageKey::buffer_extent_overflow);
  }
  if (*extent > byte_capacity) {
    return security_error(ErrorCode::invalid_buffer,
                          MessageKey::buffer_extent_exceeds_capacity);
  }
  return std::nullopt;
}

} // namespace welllog
