#include <welllog/io/lis.hpp>

#include "adapter_common.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

constexpr std::uint8_t normal_data_record = 0U;
constexpr std::uint8_t data_format_specification_record = 64U;
constexpr std::uint8_t file_header_record = 128U;

constexpr std::uint16_t physical_has_file_number = 1U << 10U;
constexpr std::uint16_t physical_has_record_number = 1U << 9U;
constexpr std::uint16_t physical_has_checksum = (1U << 13U) | (1U << 12U);
constexpr std::uint16_t physical_has_predecessor = 1U << 1U;
constexpr std::uint16_t physical_has_successor = 1U;

constexpr std::uint8_t entry_terminator = 0U;
constexpr std::uint8_t entry_frame_size = 3U;
constexpr std::uint8_t entry_absent_value = 12U;

constexpr std::uint8_t representation_i8 = 56U;
constexpr std::uint8_t representation_i16 = 79U;
constexpr std::uint8_t representation_i32 = 73U;
constexpr std::uint8_t representation_f16 = 49U;
constexpr std::uint8_t representation_f32 = 68U;

enum class ParseFailure : std::uint8_t { malformed, exhausted };

[[noreturn]] void malformed() { throw ParseFailure::malformed; }
[[noreturn]] void exhausted() { throw ParseFailure::exhausted; }

[[nodiscard]] Error invalid_lis() {
  return io_detail::invalid_document_error();
}
[[nodiscard]] Error resource_exhausted() {
  return io_detail::resource_exhausted_error();
}
[[nodiscard]] Error internal_failure() { return io_detail::internal_error(); }

using io_detail::owned_nulls;
using io_detail::owned_values;
using io_detail::stable_id;

class Cursor {
public:
  Cursor(std::span<const std::byte> bytes, std::uint64_t base_offset)
      : bytes_(bytes), base_offset_(base_offset) {}

  [[nodiscard]] bool empty() const noexcept {
    return position_ == bytes_.size();
  }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }
  [[nodiscard]] std::uint64_t offset() const noexcept {
    return base_offset_ + static_cast<std::uint64_t>(position_);
  }

  [[nodiscard]] std::uint8_t read_u8() {
    if (empty()) {
      malformed();
    }
    return std::to_integer<std::uint8_t>(bytes_[position_++]);
  }

  [[nodiscard]] std::uint16_t read_u16() {
    const auto first = read_u8();
    const auto second = read_u8();
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(first) << 8U) |
        static_cast<std::uint16_t>(second));
  }

  [[nodiscard]] std::uint32_t read_u32() {
    std::uint32_t value{};
    for (std::uint32_t index{}; index < 4U; ++index) {
      value = static_cast<std::uint32_t>((value << 8U) | read_u8());
    }
    return value;
  }

  [[nodiscard]] std::span<const std::byte> read_span(std::size_t size) {
    if (size > remaining()) {
      malformed();
    }
    const auto result = bytes_.subspan(position_, size);
    position_ += size;
    return result;
  }

private:
  std::span<const std::byte> bytes_;
  std::uint64_t base_offset_{};
  std::size_t position_{};
};

struct LogicalRecord {
  std::uint8_t type{};
  std::uint64_t byte_offset{};
  std::vector<std::byte> body;
};

[[nodiscard]] bool pad_pair(std::span<const std::byte> bytes,
                            std::size_t position) noexcept {
  if (position + 1U >= bytes.size()) {
    return false;
  }
  const auto first = std::to_integer<std::uint8_t>(bytes[position]);
  const auto second = std::to_integer<std::uint8_t>(bytes[position + 1U]);
  return (first == 0U && second == 0U) || (first == 0x20U && second == 0x20U);
}

[[nodiscard]] std::size_t
physical_trailer_size(std::uint16_t attributes) noexcept {
  std::size_t result{};
  if ((attributes & physical_has_record_number) != 0U) {
    result += 2U;
  }
  if ((attributes & physical_has_file_number) != 0U) {
    result += 2U;
  }
  if ((attributes & physical_has_checksum) != 0U) {
    result += 2U;
  }
  return result;
}

struct PhysicalRecord {
  std::uint16_t attributes{};
  std::uint64_t byte_offset{};
  std::span<const std::byte> payload;
};

[[nodiscard]] std::vector<PhysicalRecord>
read_direct_physical_records(std::span<const std::byte> bytes,
                             const LisLimits &limits) {
  std::vector<PhysicalRecord> records;
  std::size_t position{};
  while (position < bytes.size()) {
    while (pad_pair(bytes, position)) {
      // Advance one byte at a time: a legal short physical-record length can
      // itself begin with a null byte immediately after null padding.
      ++position;
    }
    if (position == bytes.size()) {
      break;
    }
    if (bytes.size() - position < 4U) {
      const auto trailing = std::to_integer<std::uint8_t>(bytes[position]);
      if (trailing == 0U || trailing == 0x20U) {
        break;
      }
      malformed();
    }
    const auto first = std::to_integer<std::uint8_t>(bytes[position]);
    const auto second = std::to_integer<std::uint8_t>(bytes[position + 1U]);
    const auto length =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(first) << 8U) |
                                   static_cast<std::uint16_t>(second));
    const auto attr_first = std::to_integer<std::uint8_t>(bytes[position + 2U]);
    const auto attr_second =
        std::to_integer<std::uint8_t>(bytes[position + 3U]);
    const auto attributes = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(attr_first) << 8U) |
        static_cast<std::uint16_t>(attr_second));
    const auto trailer = physical_trailer_size(attributes);
    const auto minimum =
        4U + trailer +
        (((attributes & physical_has_predecessor) == 0U) ? 2U : 0U);
    if (length < minimum ||
        static_cast<std::size_t>(length) > bytes.size() - position) {
      malformed();
    }
    const auto payload_length = static_cast<std::size_t>(length) - 4U - trailer;
    records.push_back(PhysicalRecord{
        .attributes = attributes,
        .byte_offset = static_cast<std::uint64_t>(position),
        .payload = bytes.subspan(position + 4U, payload_length),
    });
    position += static_cast<std::size_t>(length);
  }
  if (records.empty()) {
    malformed();
  }
  if (records.size() > limits.max_logical_records) {
    exhausted();
  }
  return records;
}

[[nodiscard]] std::vector<LogicalRecord>
assemble_logical_records(const std::vector<PhysicalRecord> &physical,
                         const LisLimits &limits) {
  std::vector<LogicalRecord> logical;
  std::size_t index{};
  while (index < physical.size()) {
    const auto &first = physical[index];
    if ((first.attributes & physical_has_predecessor) != 0U ||
        first.payload.size() < 2U) {
      malformed();
    }
    LogicalRecord record{
        .type = std::to_integer<std::uint8_t>(first.payload[0]),
        .byte_offset = first.byte_offset,
        .body = {},
    };
    record.body.insert(record.body.end(), first.payload.begin() + 2,
                       first.payload.end());
    auto successor = (first.attributes & physical_has_successor) != 0U;
    ++index;
    while (successor) {
      if (index == physical.size() ||
          (physical[index].attributes & physical_has_predecessor) == 0U) {
        malformed();
      }
      const auto &part = physical[index];
      if (record.body.size() >
          limits.max_logical_record_bytes - part.payload.size()) {
        exhausted();
      }
      record.body.insert(record.body.end(), part.payload.begin(),
                         part.payload.end());
      successor = (part.attributes & physical_has_successor) != 0U;
      ++index;
    }
    if (record.body.size() > limits.max_logical_record_bytes) {
      exhausted();
    }
    logical.push_back(std::move(record));
    if (logical.size() > limits.max_logical_records) {
      exhausted();
    }
  }
  return logical;
}

[[nodiscard]] std::vector<LogicalRecord>
parse_direct_records(std::span<const std::byte> bytes,
                     const LisLimits &limits) {
  return assemble_logical_records(read_direct_physical_records(bytes, limits),
                                  limits);
}

[[nodiscard]] bool
rp66_decimal_field(std::span<const std::byte> field) noexcept {
  bool has_digit{};
  for (const auto byte : field) {
    const auto character = std::to_integer<std::uint8_t>(byte);
    if (!has_digit && character == static_cast<std::uint8_t>(' ')) {
      continue;
    }
    if (character < static_cast<std::uint8_t>('0') ||
        character > static_cast<std::uint8_t>('9')) {
      return false;
    }
    has_digit = true;
  }
  return has_digit;
}

// This is deliberately a narrow RP66 V1 family signature, not a detector for
// every historical LIS84 envelope. In addition to the Storage Unit Label, the
// first visible record must be exactly partitioned into structurally valid
// logical-record segments so label-shaped LIS79 payloads remain LIS79.
[[nodiscard]] bool
has_rp66_v1_envelope(std::span<const std::byte> bytes) noexcept {
  constexpr std::size_t storage_unit_label_size = 80U;
  constexpr std::size_t visible_header_size = 4U;
  constexpr std::size_t segment_header_size = 4U;
  constexpr std::uint8_t segment_explicit = 0x80U;
  constexpr std::uint8_t segment_predecessor = 0x40U;
  constexpr std::uint8_t segment_successor = 0x20U;
  constexpr std::uint8_t segment_encrypted = 0x10U;
  constexpr std::uint8_t segment_encryption_packet = 0x08U;
  constexpr std::uint8_t segment_checksum = 0x04U;
  constexpr std::uint8_t segment_trailing_length = 0x02U;
  constexpr std::uint8_t segment_padding = 0x01U;

  if (bytes.size() < storage_unit_label_size + visible_header_size +
                         segment_header_size ||
      !rp66_decimal_field(bytes.subspan(0U, 4U)) ||
      std::to_integer<std::uint8_t>(bytes[4U]) !=
          static_cast<std::uint8_t>('V') ||
      std::to_integer<std::uint8_t>(bytes[5U]) !=
          static_cast<std::uint8_t>('1') ||
      std::to_integer<std::uint8_t>(bytes[6U]) !=
          static_cast<std::uint8_t>('.') ||
      !rp66_decimal_field(bytes.subspan(7U, 2U)) ||
      std::to_integer<std::uint8_t>(bytes[9U]) !=
          static_cast<std::uint8_t>('R') ||
      std::to_integer<std::uint8_t>(bytes[10U]) !=
          static_cast<std::uint8_t>('E') ||
      std::to_integer<std::uint8_t>(bytes[11U]) !=
          static_cast<std::uint8_t>('C') ||
      std::to_integer<std::uint8_t>(bytes[12U]) !=
          static_cast<std::uint8_t>('O') ||
      std::to_integer<std::uint8_t>(bytes[13U]) !=
          static_cast<std::uint8_t>('R') ||
      std::to_integer<std::uint8_t>(bytes[14U]) !=
          static_cast<std::uint8_t>('D') ||
      !rp66_decimal_field(bytes.subspan(15U, 5U))) {
    return false;
  }
  for (const auto byte : bytes.subspan(20U, 60U)) {
    const auto character = std::to_integer<std::uint8_t>(byte);
    if (character < 0x20U || character > 0x7eU) {
      return false;
    }
  }

  const auto visible_length = static_cast<std::size_t>(
      (static_cast<std::uint16_t>(
           std::to_integer<std::uint8_t>(bytes[80U]))
       << 8U) |
      static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(bytes[81U])));
  if (visible_length < visible_header_size + 16U ||
      visible_length > bytes.size() - storage_unit_label_size ||
      std::to_integer<std::uint8_t>(bytes[82U]) != 0xffU ||
      std::to_integer<std::uint8_t>(bytes[83U]) != 0x01U) {
    return false;
  }

  const auto visible_end = storage_unit_label_size + visible_length;
  auto position = storage_unit_label_size + visible_header_size;
  bool pending_successor{};
  std::uint8_t pending_type{};
  bool pending_explicit{};
  while (position < visible_end) {
    if (visible_end - position < segment_header_size) {
      return false;
    }
    const auto segment_length = static_cast<std::size_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[position]))
         << 8U) |
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[position + 1U])));
    if (segment_length < 16U || segment_length % 2U != 0U ||
        segment_length > visible_end - position) {
      return false;
    }
    const auto attributes =
        std::to_integer<std::uint8_t>(bytes[position + 2U]);
    const auto type = std::to_integer<std::uint8_t>(bytes[position + 3U]);
    if (position == storage_unit_label_size + visible_header_size &&
        type != 0U) {
      return false;
    }
    const auto has_predecessor =
        (attributes & segment_predecessor) != 0U;
    if (has_predecessor != pending_successor) {
      return false;
    }
    const auto explicit_formatting = (attributes & segment_explicit) != 0U;
    if (has_predecessor &&
        (type != pending_type || explicit_formatting != pending_explicit)) {
      return false;
    }

    auto payload_begin = position + segment_header_size;
    auto payload_size = segment_length - segment_header_size;
    if ((attributes & segment_encryption_packet) != 0U) {
      if (payload_size == 0U) {
        return false;
      }
      const auto packet_length =
          std::to_integer<std::uint8_t>(bytes[payload_begin]);
      if (packet_length > payload_size) {
        return false;
      }
      payload_begin += packet_length;
      payload_size -= packet_length;
    }
    if ((attributes & segment_encrypted) == 0U) {
      std::size_t trailer_size{};
      if ((attributes & segment_checksum) != 0U) {
        trailer_size += 2U;
      }
      if ((attributes & segment_trailing_length) != 0U) {
        trailer_size += 2U;
      }
      if ((attributes & segment_padding) != 0U) {
        if (trailer_size >= payload_size) {
          return false;
        }
        trailer_size += std::to_integer<std::uint8_t>(
            bytes[payload_begin + payload_size - trailer_size - 1U]);
      }
      if (trailer_size > payload_size) {
        return false;
      }
    }

    pending_successor = (attributes & segment_successor) != 0U;
    pending_type = type;
    pending_explicit = explicit_formatting;
    position += segment_length;
  }
  return position == visible_end && !pending_successor;
}

[[nodiscard]] std::uint32_t
read_little_endian_u32(std::span<const std::byte> bytes, std::size_t offset) {
  if (bytes.size() - offset < 4U) {
    malformed();
  }
  std::uint32_t value{};
  for (std::uint32_t index{}; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

// The common Tape Image Format envelope exposes each file as bytes between a
// type-0 tape mark and the following type-1 tape mark. We validate that compact
// form structurally, rather than relying on a filename extension or magic text.
[[nodiscard]] std::span<const std::byte>
unwrap_tif(std::span<const std::byte> bytes) {
  if (bytes.size() < 36U) {
    malformed();
  }
  const auto leading_type = read_little_endian_u32(bytes, 0U);
  const auto leading_previous = read_little_endian_u32(bytes, 4U);
  const auto following_mark = read_little_endian_u32(bytes, 8U);
  if (leading_type != 0U || leading_previous != 0U || following_mark < 12U ||
      static_cast<std::size_t>(following_mark) > bytes.size() - 12U) {
    malformed();
  }
  const auto terminal = static_cast<std::size_t>(following_mark);
  const auto terminal_type = read_little_endian_u32(bytes, terminal);
  const auto terminal_next = read_little_endian_u32(bytes, terminal + 8U);
  if (terminal_type != 1U || terminal_next <= following_mark ||
      static_cast<std::size_t>(terminal_next) > bytes.size()) {
    malformed();
  }
  return bytes.subspan(12U, terminal - 12U);
}

[[nodiscard]] std::vector<LogicalRecord>
parse_lis_records(std::span<const std::byte> bytes, const LisLimits &limits) {
  const auto rp66_v1 = has_rp66_v1_envelope(bytes);
  std::optional<std::vector<LogicalRecord>> direct;
  std::optional<std::vector<LogicalRecord>> tif;
  try {
    direct = parse_direct_records(bytes, limits);
  } catch (ParseFailure failure) {
    if (failure == ParseFailure::exhausted) {
      throw;
    }
  }
  try {
    const auto payload = unwrap_tif(bytes);
    tif = parse_direct_records(payload, limits);
  } catch (ParseFailure failure) {
    if (failure == ParseFailure::exhausted) {
      throw;
    }
  }
  // The LIS public error vocabulary is intentionally format-agnostic. A sole
  // RP66 match is unsupported here; RP66 plus either LIS79 match is ambiguous.
  if (rp66_v1 || direct.has_value() == tif.has_value()) {
    malformed();
  }
  return direct.has_value() ? std::move(*direct) : std::move(*tif);
}

[[nodiscard]] std::string trim_ascii(std::string_view value) {
  const auto first = value.find_first_not_of(' ');
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(' ');
  return std::string{value.substr(first, last - first + 1U)};
}

[[nodiscard]] std::string upper_ascii(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const auto character : value) {
    result.push_back(character >= 'a' && character <= 'z'
                         ? static_cast<char>(character - 'a' + 'A')
                         : character);
  }
  return result;
}

[[nodiscard]] std::string compact_ascii(std::string_view value) {
  std::string result;
  for (const auto character : upper_ascii(trim_ascii(value))) {
    if (character != '_' && character != '-') {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] std::string_view
text_encoding_name(LisTextEncoding encoding) noexcept {
  switch (encoding) {
  case LisTextEncoding::ascii:
    return "ASCII";
  case LisTextEncoding::iso_8859_1:
    return "ISO-8859-1";
  }
  return {};
}

[[nodiscard]] bool valid_identity_scheme(LisIdentityScheme scheme) noexcept {
  switch (scheme) {
  case LisIdentityScheme::resform_compatible_v1:
  case LisIdentityScheme::content_bound_v2:
    return true;
  }
  return false;
}

struct BuiltinAliasRule {
  std::string_view source;
  std::string_view canonical;
};

constexpr std::array builtin_alias_rules{
    BuiltinAliasRule{"GR", "GR"},      BuiltinAliasRule{"GAM", "GR"},
    BuiltinAliasRule{"GAMMA", "GR"},   BuiltinAliasRule{"SP", "SP"},
    BuiltinAliasRule{"AC", "AC"},      BuiltinAliasRule{"DT", "AC"},
    BuiltinAliasRule{"DTC", "AC"},     BuiltinAliasRule{"DEN", "DEN"},
    BuiltinAliasRule{"RHOB", "DEN"},   BuiltinAliasRule{"CNL", "CNL"},
    BuiltinAliasRule{"NPHI", "CNL"},   BuiltinAliasRule{"TNPH", "CNL"},
    BuiltinAliasRule{"CAL", "CAL"},    BuiltinAliasRule{"CALI", "CAL"},
    BuiltinAliasRule{"RILD", "RDEEP"}, BuiltinAliasRule{"LLD", "RDEEP"},
    BuiltinAliasRule{"RT", "RDEEP"},   BuiltinAliasRule{"RILM", "RMED"},
    BuiltinAliasRule{"LLM", "RMED"},   BuiltinAliasRule{"RLLS", "RSHAL"},
    BuiltinAliasRule{"LLS", "RSHAL"},
};

struct BuiltinUnitRule {
  std::string_view canonical_mnemonic;
  std::string_view source_unit;
  std::string_view canonical_unit;
  double multiplier;
};

constexpr std::array builtin_unit_rules{
    BuiltinUnitRule{"GR", "API", "API", 1.0},
    BuiltinUnitRule{"SP", "MV", "mV", 1.0},
    BuiltinUnitRule{"DEN", "G/CC", "g/cm3", 1.0},
    BuiltinUnitRule{"DEN", "G/CM3", "g/cm3", 1.0},
    BuiltinUnitRule{"DEN", "KG/M3", "g/cm3", 0.001},
    BuiltinUnitRule{"AC", "US/M", "us/m", 1.0},
    BuiltinUnitRule{"AC", "US/FT", "us/m", 3.280839895013123},
    BuiltinUnitRule{"CNL", "%", "%", 1.0},
    BuiltinUnitRule{"CNL", "V/V", "%", 100.0},
    BuiltinUnitRule{"CAL", "MM", "mm", 1.0},
    BuiltinUnitRule{"CAL", "CM", "mm", 10.0},
    BuiltinUnitRule{"CAL", "IN", "mm", 25.4},
    BuiltinUnitRule{"RDEEP", "OHM.M", "ohm.m", 1.0},
    BuiltinUnitRule{"RDEEP", "OHMM", "ohm.m", 1.0},
    BuiltinUnitRule{"RDEEP", "OHM.FT", "ohm.m", 0.3048},
    BuiltinUnitRule{"RMED", "OHM.M", "ohm.m", 1.0},
    BuiltinUnitRule{"RMED", "OHMM", "ohm.m", 1.0},
    BuiltinUnitRule{"RMED", "OHM.FT", "ohm.m", 0.3048},
    BuiltinUnitRule{"RSHAL", "OHM.M", "ohm.m", 1.0},
    BuiltinUnitRule{"RSHAL", "OHMM", "ohm.m", 1.0},
    BuiltinUnitRule{"RSHAL", "OHM.FT", "ohm.m", 0.3048},
};

struct BuiltinDepthUnitRule {
  std::string_view source_unit;
  double metres_per_unit;
};

constexpr std::array builtin_depth_unit_rules{
    BuiltinDepthUnitRule{"M", 1.0},
    BuiltinDepthUnitRule{"FT", 0.3048},
    BuiltinDepthUnitRule{"IN", 0.0254},
};

// Immutable protocol data for the original v1 entity-identity preimage. It
// deliberately stays separate from the executable rule tables: it describes
// an already-persisted wire format, not a regenerated summary of current
// behavior. ADR 0049 requires a new profile version for behavior changes.
constexpr std::string_view resform_compatible_v1_identity_component =
    "GR=GR,GAM,GAMMA;SP=SP;AC=AC,DT,DTC;DEN=DEN,RHOB;"
    "CNL=CNL,NPHI,TNPH;CAL=CAL,CALI;RDEEP=RILD,LLD,RT;"
    "RMED=RILM,LLM;RSHAL=RLLS,LLS;"
    "GR/API;SP/mV;DEN/g/cc,g/cm3,kg/m3;AC/us/m,us/ft;"
    "CNL/%,v/v;CAL/mm,cm,in;R/ohm.m,ohm.ft;DEPTH/m,ft,in";

[[nodiscard]] std::optional<double> read_lis_f32(Cursor &cursor) {
  const auto encoded = cursor.read_u32();
  const auto sign = (encoded & 0x80000000U) != 0U;
  const auto encoded_exponent =
      static_cast<std::uint8_t>((encoded >> 23U) & 0xffU);
  const auto exponent =
      static_cast<int>(sign ? static_cast<std::uint8_t>(~encoded_exponent)
                            : encoded_exponent) -
      128;
  auto fraction = encoded & 0x007fffffU;
  if (sign) {
    fraction = static_cast<std::uint32_t>((~fraction + 1U) & 0x007fffffU);
  }
  return (sign ? -1.0 : 1.0) *
         std::ldexp(static_cast<double>(fraction) / 8388608.0, exponent);
}

[[nodiscard]] std::optional<double> read_number(Cursor &cursor,
                                                std::uint8_t representation) {
  switch (representation) {
  case representation_i8:
    return static_cast<double>(static_cast<std::int8_t>(cursor.read_u8()));
  case representation_i16:
    return static_cast<double>(static_cast<std::int16_t>(cursor.read_u16()));
  case representation_i32:
    return static_cast<double>(static_cast<std::int32_t>(cursor.read_u32()));
  case representation_f16: {
    const auto encoded = cursor.read_u16();
    const auto sign = (encoded & 0x8000U) != 0U;
    const auto exponent = static_cast<int>(encoded & 0x000fU);
    auto fraction = static_cast<std::uint16_t>((encoded & 0x7ff0U) >> 4U);
    if (sign) {
      // Two's-complement of the 11-bit fraction without promoting ~uint16
      // through a signed int (Clang -Wsign-conversion).
      fraction = static_cast<std::uint16_t>(
          (0U - static_cast<unsigned>(fraction)) & 0x07ffU);
    }
    return (sign ? -1.0 : 1.0) *
           std::ldexp(static_cast<double>(fraction) / 2048.0, exponent);
  }
  case representation_f32:
    return read_lis_f32(cursor);
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::size_t>
fixed_representation_size(std::uint8_t representation) noexcept {
  switch (representation) {
  case representation_i8:
    return 1U;
  case representation_i16:
  case representation_f16:
    return 2U;
  case representation_i32:
  case representation_f32:
    return 4U;
  default:
    return std::nullopt;
  }
}

struct SpecBlock {
  std::string mnemonic;
  std::string unit;
  std::int16_t reserved_size{};
  std::uint8_t samples{};
  std::uint8_t representation{};
};

struct DataFormatSpecification {
  std::uint64_t byte_offset{};
  std::uint32_t frame_size{};
  std::optional<double> explicit_null_value;
  bool contains_non_ascii_text{};
  std::vector<SpecBlock> specifications;
};

[[nodiscard]] DataFormatSpecification
parse_format_specification(const LogicalRecord &record, const LisLimits &limits,
                           LisTextEncoding text_encoding) {
  Cursor cursor{record.body, record.byte_offset + 6U};
  DataFormatSpecification result{
      .byte_offset = record.byte_offset,
      .frame_size = 0U,
      .explicit_null_value = std::nullopt,
      .contains_non_ascii_text = false,
      .specifications = {},
  };
  while (true) {
    const auto type = cursor.read_u8();
    const auto size = cursor.read_u8();
    const auto representation = cursor.read_u8();
    const auto value = cursor.read_span(size);
    if (type == entry_terminator) {
      if (size != 0U) {
        malformed();
      }
      break;
    }
    if (type == entry_frame_size) {
      Cursor value_cursor{value, cursor.offset() - size};
      const auto number = read_number(value_cursor, representation);
      if (!number.has_value() || *number <= 0.0 ||
          *number >
              static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        malformed();
      }
      result.frame_size = static_cast<std::uint32_t>(*number);
    } else if (type == entry_absent_value) {
      Cursor value_cursor{value, cursor.offset() - size};
      const auto number = read_number(value_cursor, representation);
      if (!number.has_value() || !value_cursor.empty() ||
          !std::isfinite(*number)) {
        malformed();
      }
      result.explicit_null_value = *number;
    }
  }
  if (cursor.remaining() == 0U || cursor.remaining() % 40U != 0U) {
    malformed();
  }
  while (!cursor.empty()) {
    const auto block = cursor.read_span(40U);
    const auto read_text = [&block, &result, text_encoding](std::size_t offset,
                                                            std::size_t size) {
      std::string value;
      value.reserve(size);
      for (std::size_t index{}; index < size; ++index) {
        const auto byte = std::to_integer<std::uint8_t>(block[offset + index]);
        if (byte <= 0x7fU) {
          value.push_back(static_cast<char>(byte));
          continue;
        }
        result.contains_non_ascii_text = true;
        if (text_encoding == LisTextEncoding::iso_8859_1) {
          value.push_back(static_cast<char>(0xc0U | (byte >> 6U)));
          value.push_back(static_cast<char>(0x80U | (byte & 0x3fU)));
        } else {
          value.push_back(static_cast<char>(byte));
        }
      }
      return trim_ascii(value);
    };
    const auto raw_reserved_size = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(block[28U]))
         << 8U) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(block[29U])));
    const auto reserved_size = static_cast<std::int16_t>(raw_reserved_size);
    result.specifications.push_back(SpecBlock{
        .mnemonic = read_text(0U, 4U),
        .unit = read_text(18U, 4U),
        .reserved_size = reserved_size,
        .samples = std::to_integer<std::uint8_t>(block[33U]),
        .representation = std::to_integer<std::uint8_t>(block[34U]),
    });
  }
  if (result.specifications.empty() ||
      result.specifications.size() > limits.max_curves_per_data_set) {
    exhausted();
  }
  if (result.frame_size == 0U) {
    malformed();
  }
  return result;
}

[[nodiscard]] std::string
content_fingerprint(std::span<const std::byte> bytes) {
  constexpr std::uint64_t offset = 1469598103934665603ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto hash = offset;
  for (const auto byte : bytes) {
    hash ^= std::to_integer<std::uint8_t>(byte);
    hash *= prime;
  }
  constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result(16U, '0');
  for (std::size_t index{}; index < result.size(); ++index) {
    const auto shift = static_cast<unsigned>((result.size() - 1U - index) * 4U);
    result[index] = digits[(hash >> shift) & 0x0fU];
  }
  return result;
}

[[nodiscard]] std::string
profile_fingerprint(const LisNormalizationProfile &profile,
                    LisIdentityScheme scheme) {
  const auto append_component = [](std::string &target,
                                   std::string_view value) {
    target += "/" + std::to_string(value.size()) + ":" + std::string{value};
  };
  std::string result;
  append_component(result, profile.name);
  append_component(result, profile.version);
  append_component(result, text_encoding_name(profile.text_encoding));
  append_component(result, resform_compatible_v1_identity_component);
  const auto append_double = [&append_component, scheme](std::string &target,
                                                           double value) {
    if (scheme == LisIdentityScheme::resform_compatible_v1) {
      // Preserve the historical v1 preimage byte-for-byte, including its
      // locale-dependent std::to_string representation.
      append_component(target, std::to_string(value));
      return;
    }
    constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6',
                                          '7', '8', '9', 'a', 'b', 'c', 'd',
                                          'e', 'f'};
    const auto bits = std::bit_cast<std::uint64_t>(value);
    std::string exact{"f64:"};
    for (std::size_t index{}; index < 16U; ++index) {
      const auto shift = static_cast<unsigned>((15U - index) * 4U);
      exact.push_back(digits[(bits >> shift) & 0x0fU]);
    }
    append_component(target, exact);
  };
  for (const auto value : profile.inferred_null_values) {
    append_double(result, value);
  }
  for (const auto &alias : profile.aliases) {
    append_component(result, alias.source_mnemonic);
    append_component(result, alias.canonical_mnemonic);
  }
  for (const auto &rule : profile.unit_rules) {
    append_component(result, rule.canonical_mnemonic);
    append_component(result, rule.source_unit);
    append_component(result, rule.canonical_unit);
    append_double(result, rule.multiplier);
  }
  return result;
}

[[nodiscard]] bool valid_profile(const LisNormalizationProfile &profile) {
  if (profile.name.empty() || profile.version.empty() ||
      text_encoding_name(profile.text_encoding).empty() ||
      !std::all_of(profile.inferred_null_values.begin(),
                   profile.inferred_null_values.end(),
                   [](double value) { return std::isfinite(value); })) {
    return false;
  }
  for (std::size_t index{}; index < profile.aliases.size(); ++index) {
    const auto &rule = profile.aliases[index];
    if (compact_ascii(rule.source_mnemonic).empty() ||
        compact_ascii(rule.canonical_mnemonic).empty()) {
      return false;
    }
    for (std::size_t previous{}; previous < index; ++previous) {
      if (compact_ascii(profile.aliases[previous].source_mnemonic) ==
              compact_ascii(rule.source_mnemonic) &&
          compact_ascii(profile.aliases[previous].canonical_mnemonic) !=
              compact_ascii(rule.canonical_mnemonic)) {
        return false;
      }
    }
  }
  for (std::size_t index{}; index < profile.unit_rules.size(); ++index) {
    const auto &rule = profile.unit_rules[index];
    if (compact_ascii(rule.canonical_mnemonic).empty() ||
        compact_ascii(rule.source_unit).empty() ||
        rule.canonical_unit.empty() || !std::isfinite(rule.multiplier) ||
        rule.multiplier == 0.0) {
      return false;
    }
    for (std::size_t previous{}; previous < index; ++previous) {
      const auto &other = profile.unit_rules[previous];
      if (compact_ascii(other.canonical_mnemonic) ==
              compact_ascii(rule.canonical_mnemonic) &&
          compact_ascii(other.source_unit) == compact_ascii(rule.source_unit) &&
          (other.canonical_unit != rule.canonical_unit ||
           other.multiplier != rule.multiplier)) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::string>
canonical_mnemonic(std::string_view mnemonic,
                   const LisNormalizationProfile &profile) {
  const auto key = compact_ascii(mnemonic);
  for (const auto &rule : profile.aliases) {
    if (compact_ascii(rule.source_mnemonic) == key) {
      return upper_ascii(trim_ascii(rule.canonical_mnemonic));
    }
  }
  for (const auto &rule : builtin_alias_rules) {
    if (rule.source == key) {
      return std::string{rule.canonical};
    }
  }
  return std::nullopt;
}

struct UnitConversion {
  std::string unit;
  double multiplier{1.0};
};

[[nodiscard]] std::optional<UnitConversion>
curve_unit_conversion(std::string_view mnemonic, std::string_view unit,
                      const LisNormalizationProfile &profile) {
  const auto key = compact_ascii(unit);
  for (const auto &rule : profile.unit_rules) {
    if (compact_ascii(rule.canonical_mnemonic) == compact_ascii(mnemonic) &&
        compact_ascii(rule.source_unit) == key) {
      return UnitConversion{rule.canonical_unit, rule.multiplier};
    }
  }
  const auto canonical_key = compact_ascii(mnemonic);
  for (const auto &rule : builtin_unit_rules) {
    if (rule.canonical_mnemonic == canonical_key && rule.source_unit == key) {
      return UnitConversion{std::string{rule.canonical_unit}, rule.multiplier};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<double> depth_to_metres(std::string_view unit) {
  const auto key = compact_ascii(unit);
  for (const auto &rule : builtin_depth_unit_rules) {
    if (rule.source_unit == key) {
      return rule.metres_per_unit;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool depth_mnemonic(std::string_view mnemonic) {
  const auto key = compact_ascii(mnemonic);
  return key == "DEPT" || key == "DEPTH" || key == "MD";
}

[[nodiscard]] std::optional<DepthDomain> axis_domain(std::string_view mnemonic,
                                                     std::string_view unit) {
  if (!depth_to_metres(unit).has_value()) {
    return std::nullopt;
  }
  const auto key = compact_ascii(mnemonic);
  if (depth_mnemonic(mnemonic)) {
    return DepthDomain::measured_depth;
  }
  if (key == "TVD") {
    return DepthDomain::true_vertical_depth;
  }
  if (key == "TVDSS") {
    return DepthDomain::true_vertical_depth_subsea;
  }
  return std::nullopt;
}

[[nodiscard]] bool is_null(double value, const LisNormalizationProfile &profile,
                           std::optional<double> explicit_null_value) {
  if (!std::isfinite(value)) {
    return true;
  }
  if (explicit_null_value.has_value()) {
    return value == *explicit_null_value;
  }
  return std::any_of(profile.inferred_null_values.begin(),
                     profile.inferred_null_values.end(),
                     [value](double sentinel) { return value == sentinel; });
}

struct DecodedDataSet {
  std::vector<double> index;
  std::vector<std::vector<double>> values;
  std::vector<bool> scalar_values;
};

struct AxisSegment {
  std::size_t begin{};
  std::size_t end{};
  AxisDirection direction{AxisDirection::increasing};
};

[[nodiscard]] std::vector<AxisSegment>
split_axis_segments(const std::vector<double> &coordinates) {
  std::vector<AxisSegment> result;
  std::size_t begin{};
  std::optional<AxisDirection> direction;
  for (std::size_t index = 1U; index < coordinates.size(); ++index) {
    if (coordinates[index] == coordinates[index - 1U]) {
      continue;
    }
    const auto next = coordinates[index] > coordinates[index - 1U]
                          ? AxisDirection::increasing
                          : AxisDirection::decreasing;
    if (direction.has_value() && *direction != next) {
      result.push_back(
          AxisSegment{.begin = begin, .end = index, .direction = *direction});
      begin = index;
      direction.reset();
    }
    if (!direction.has_value()) {
      direction = next;
    }
  }
  result.push_back(
      AxisSegment{.begin = begin,
                  .end = coordinates.size(),
                  .direction = direction.value_or(AxisDirection::increasing)});
  return result;
}

struct LogicalFileRange {
  std::size_t begin{};
  std::size_t end{};
};

[[nodiscard]] DecodedDataSet
decode_normal_data(const DataFormatSpecification &format,
                   const std::vector<LogicalRecord> &records,
                   const LisLimits &limits) {
  if (format.specifications.empty() ||
      format.specifications.front().samples != 1U ||
      format.specifications.front().reserved_size <= 0 ||
      !fixed_representation_size(format.specifications.front().representation)
           .has_value()) {
    malformed();
  }
  DecodedDataSet result;
  result.values.resize(format.specifications.size() - 1U);
  result.scalar_values.resize(format.specifications.size() - 1U, false);
  for (const auto &record : records) {
    if (record.body.size() % format.frame_size != 0U) {
      malformed();
    }
    Cursor cursor{record.body, record.byte_offset + 6U};
    while (!cursor.empty()) {
      if (result.index.size() == limits.max_samples_per_data_set) {
        exhausted();
      }
      const auto frame = cursor.read_span(format.frame_size);
      Cursor sample{frame, cursor.offset() - format.frame_size};
      for (std::size_t spec_index{}; spec_index < format.specifications.size();
           ++spec_index) {
        const auto &spec = format.specifications[spec_index];
        if (spec.reserved_size <= 0) {
          malformed();
        }
        const auto field =
            sample.read_span(static_cast<std::size_t>(spec.reserved_size));
        const auto scalar =
            spec.samples == 1U &&
            static_cast<std::size_t>(spec.reserved_size) ==
                fixed_representation_size(spec.representation).value_or(0U);
        if (spec_index == 0U) {
          if (!scalar) {
            malformed();
          }
          Cursor scalar_field{field, sample.offset() - field.size()};
          const auto value = read_number(scalar_field, spec.representation);
          if (!value.has_value() || !scalar_field.empty()) {
            malformed();
          }
          result.index.push_back(*value);
        } else if (scalar) {
          Cursor scalar_field{field, sample.offset() - field.size()};
          const auto value = read_number(scalar_field, spec.representation);
          if (!value.has_value() || !scalar_field.empty()) {
            malformed();
          }
          result.values[spec_index - 1U].push_back(*value);
          result.scalar_values[spec_index - 1U] = true;
        }
      }
      if (!sample.empty()) {
        malformed();
      }
    }
  }
  return result;
}

[[nodiscard]] std::vector<LisLogicalFileDescriptor>
logical_file_catalog(const std::vector<LogicalRecord> &records) {
  std::vector<LisLogicalFileDescriptor> files;
  for (const auto &record : records) {
    if (record.type == file_header_record) {
      files.push_back(LisLogicalFileDescriptor{
          .index = static_cast<std::uint32_t>(files.size()),
          .byte_offset = record.byte_offset,
          .display_name = {},
      });
    }
  }
  if (files.empty()) {
    files.push_back(LisLogicalFileDescriptor{
        .index = 0U, .byte_offset = 0U, .display_name = {}});
  }
  return files;
}

[[nodiscard]] std::vector<LogicalFileRange>
logical_file_ranges(const std::vector<LogicalRecord> &records) {
  std::vector<LogicalFileRange> ranges;
  for (std::size_t index{}; index < records.size(); ++index) {
    if (records[index].type != file_header_record) {
      continue;
    }
    if (!ranges.empty()) {
      ranges.back().end = index;
    }
    ranges.push_back(LogicalFileRange{.begin = index, .end = records.size()});
  }
  if (ranges.empty()) {
    ranges.push_back(LogicalFileRange{.begin = 0U, .end = records.size()});
  }
  return ranges;
}

[[nodiscard]] std::string import_identity(
    std::span<const std::byte> bytes, const BufferSourceReference &source,
    const LisNormalizationProfile &profile, LisIdentityScheme scheme,
    std::uint32_t logical_file_index) {
  if (scheme == LisIdentityScheme::resform_compatible_v1) {
    const auto content =
        source.checksum.empty() ? content_fingerprint(bytes) : source.checksum;
    return "lis/" + content + "/" +
           profile_fingerprint(profile, LisIdentityScheme::resform_compatible_v1) +
           "/logical-file/" + std::to_string(logical_file_index);
  }
  const auto append_component = [](std::string &target,
                                   std::string_view value) {
    target += "/" + std::to_string(value.size()) + ":" + std::string{value};
  };
  std::string result{"lis/content-bound-v2"};
  append_component(result, source.uri);
  append_component(result, source.checksum);
  append_component(result, std::to_string(source.byte_offset));
  append_component(result, content_fingerprint(bytes));
  append_component(result,
                   profile_fingerprint(profile,
                                       LisIdentityScheme::content_bound_v2));
  append_component(result, std::to_string(logical_file_index));
  return result;
}

[[nodiscard]] EntityId
axis_id_for_import(std::string_view identity, LisIdentityScheme scheme,
                   std::uint32_t data_set_ordinal,
                   std::uint64_t specification_offset,
                   std::size_t segment_begin, std::size_t segment_index) {
  if (scheme == LisIdentityScheme::resform_compatible_v1) {
    return stable_id(identity, "axis",
                     specification_offset +
                         static_cast<std::uint64_t>(segment_begin));
  }
  return stable_id(identity,
                   "axis/data-set/" + std::to_string(data_set_ordinal),
                   static_cast<std::uint64_t>(segment_index));
}

[[nodiscard]] EntityId
curve_id(std::string_view identity, LisIdentityScheme scheme,
         std::uint32_t data_set_ordinal, std::uint64_t specification_offset,
         std::size_t segment_index, std::size_t specification_index) {
  if (scheme == LisIdentityScheme::resform_compatible_v1) {
    return stable_id(identity, "curve",
                     specification_offset +
                         static_cast<std::uint64_t>(segment_index) * 4096U +
                         static_cast<std::uint64_t>(specification_index));
  }
  return stable_id(identity,
                   "curve/data-set/" + std::to_string(data_set_ordinal) +
                       "/segment/" + std::to_string(segment_index),
                   static_cast<std::uint64_t>(specification_index));
}

} // namespace

const LisNormalizationProfile &default_lis_normalization_profile() noexcept {
  static const LisNormalizationProfile profile;
  return profile;
}

Result<LisInspection>
LisSourceAdapter::inspect(std::span<const std::byte> bytes, LisLimits limits) {
  try {
    if (bytes.size() > limits.max_input_bytes) {
      return resource_exhausted();
    }
    const auto records = parse_lis_records(bytes, limits);
    return LisInspection{
        .catalog = LisCatalog{.logical_files = logical_file_catalog(records)},
        .diagnostics = {}};
  } catch (ParseFailure failure) {
    return failure == ParseFailure::exhausted ? resource_exhausted()
                                              : invalid_lis();
  } catch (const std::bad_alloc &) {
    return resource_exhausted();
  } catch (...) {
    return internal_failure();
  }
}

Result<LisImport>
LisSourceAdapter::import(std::span<const std::byte> bytes,
                         BufferSourceReference source, LisSelection selection,
                         const LisNormalizationProfile &profile,
                         LisLimits limits) {
  try {
    if (!valid_profile(profile) ||
        !valid_identity_scheme(selection.identity_scheme)) {
      return invalid_lis();
    }
    if (bytes.size() > limits.max_input_bytes) {
      return resource_exhausted();
    }
    const auto records = parse_lis_records(bytes, limits);
    const auto files = logical_file_catalog(records);
    const auto ranges = logical_file_ranges(records);
    const auto selected =
        selection.logical_file_index == UINT32_MAX && files.size() == 1U
            ? 0U
            : selection.logical_file_index;
    if (selected >= files.size()) {
      return invalid_lis();
    }

    const auto &range = ranges[selected];
    const auto selected_begin =
        records.begin() + static_cast<std::ptrdiff_t>(range.begin);
    const auto selected_end =
        records.begin() + static_cast<std::ptrdiff_t>(range.end);
    const auto identity = import_identity(bytes, source, profile,
                                          selection.identity_scheme, selected);
    const auto document_id = stable_id(identity, "document");
    WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
    std::vector<LisDiagnostic> diagnostics;
    for (auto iterator = selected_begin; iterator != selected_end; ++iterator) {
      const auto &record = *iterator;
      if (record.type == file_header_record ||
          record.type == data_format_specification_record ||
          record.type == normal_data_record) {
        continue;
      }
      diagnostics.push_back(LisDiagnostic{
          .code = LisDiagnosticCode::unknown_record,
          .severity = Severity::warning,
          .byte_offset = record.byte_offset,
          .logical_file_index = selected,
          .data_set_ordinal = 0U,
          .channel_name = {},
      });
    }
    std::uint32_t data_set_ordinal{};
    for (auto format = selected_begin; format != selected_end; ++format) {
      if (format->type != data_format_specification_record) {
        continue;
      }
      const auto ordinal = data_set_ordinal++;
      DataFormatSpecification specification;
      try {
        specification =
            parse_format_specification(*format, limits, profile.text_encoding);
      } catch (ParseFailure failure) {
        if (failure == ParseFailure::exhausted) {
          throw;
        }
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::malformed_dataset,
            .severity = Severity::warning,
            .byte_offset = format->byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = {},
        });
        continue;
      }
      if (specification.contains_non_ascii_text &&
          profile.text_encoding == LisTextEncoding::ascii) {
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::non_ascii_text,
            .severity = Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .representation = 0U,
            .channel_name = {},
        });
      }

      std::vector<LogicalRecord> data_records;
      for (auto iterator = std::next(format); iterator != selected_end;
           ++iterator) {
        if (iterator->type == data_format_specification_record) {
          break;
        }
        if (iterator->type == normal_data_record) {
          data_records.push_back(*iterator);
        }
      }
      if (data_records.empty()) {
        const auto next_format = std::find_if(
            std::next(format), selected_end, [](const LogicalRecord &record) {
              return record.type == data_format_specification_record;
            });
        const auto is_replaced = next_format != selected_end;
        diagnostics.push_back(LisDiagnostic{
            .code = is_replaced && format->body == next_format->body
                        ? LisDiagnosticCode::redundant_format_specification
                    : is_replaced
                        ? LisDiagnosticCode::replaced_format_specification
                        : LisDiagnosticCode::malformed_dataset,
            .severity = is_replaced && format->body == next_format->body
                            ? Severity::info
                            : Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = {},
        });
        continue;
      }

      DecodedDataSet decoded;
      try {
        decoded = decode_normal_data(specification, data_records, limits);
      } catch (ParseFailure failure) {
        if (failure == ParseFailure::exhausted) {
          throw;
        }
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::malformed_dataset,
            .severity = Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = {},
        });
        continue;
      }
      if (decoded.index.empty()) {
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::malformed_dataset,
            .severity = Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = {},
        });
        continue;
      }

      const auto &index_specification = specification.specifications.front();
      const auto recognized_axis_domain =
          axis_domain(index_specification.mnemonic, index_specification.unit);
      const auto depth_factor = depth_to_metres(index_specification.unit);
      const auto domain =
          recognized_axis_domain.value_or(DepthDomain::source_index);
      const std::string axis_unit =
          recognized_axis_domain.has_value() ? "m" : index_specification.unit;
      std::vector<double> coordinates;
      std::vector<std::size_t> source_samples;
      coordinates.reserve(decoded.index.size());
      source_samples.reserve(decoded.index.size());
      for (std::size_t sample_index{}; sample_index < decoded.index.size();
           ++sample_index) {
        const auto value = decoded.index[sample_index];
        if (!std::isfinite(value)) {
          diagnostics.push_back(LisDiagnostic{
              .code = LisDiagnosticCode::non_finite_axis_value,
              .severity = Severity::warning,
              .byte_offset = specification.byte_offset,
              .logical_file_index = selected,
              .data_set_ordinal = ordinal,
              .channel_name = index_specification.mnemonic,
          });
          continue;
        }
        coordinates.push_back(
            recognized_axis_domain.has_value() ? value * *depth_factor : value);
        source_samples.push_back(sample_index);
      }
      if (coordinates.empty()) {
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::malformed_dataset,
            .severity = Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = {},
        });
        continue;
      }
      if (!recognized_axis_domain.has_value()) {
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::unknown_index_semantics,
            .severity = Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = index_specification.mnemonic,
        });
      }
      if (coordinates.size() > 1U &&
          std::adjacent_find(coordinates.begin(), coordinates.end(),
                             std::not_equal_to<>{}) == coordinates.end()) {
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::constant_axis,
            .severity = Severity::info,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = index_specification.mnemonic,
        });
      }

      struct CurvePlan {
        std::size_t specification_index{};
        std::string mnemonic;
        std::string unit;
        double multiplier{1.0};
      };
      std::vector<CurvePlan> curves;
      for (std::size_t spec_index = 1U;
           spec_index < specification.specifications.size(); ++spec_index) {
        const auto &spec = specification.specifications[spec_index];
        if (!decoded.scalar_values[spec_index - 1U]) {
          diagnostics.push_back(LisDiagnostic{
              .code = LisDiagnosticCode::unsupported_channel,
              .severity = Severity::warning,
              .byte_offset = specification.byte_offset,
              .logical_file_index = selected,
              .data_set_ordinal = ordinal,
              .representation = spec.representation,
              .channel_name = spec.mnemonic,
          });
          continue;
        }
        const auto semantic = canonical_mnemonic(spec.mnemonic, profile);
        const auto conversion =
            semantic.has_value()
                ? curve_unit_conversion(*semantic, spec.unit, profile)
                : std::nullopt;
        if (!semantic.has_value() || !conversion.has_value()) {
          diagnostics.push_back(LisDiagnostic{
              .code = !semantic.has_value()
                          ? LisDiagnosticCode::unknown_curve_semantics
                          : LisDiagnosticCode::normalization_conflict,
              .severity = Severity::warning,
              .byte_offset = specification.byte_offset,
              .logical_file_index = selected,
              .data_set_ordinal = ordinal,
              .representation = spec.representation,
              .channel_name = spec.mnemonic,
          });
          curves.push_back(CurvePlan{.specification_index = spec_index,
                                     .mnemonic = spec.mnemonic,
                                     .unit = spec.unit});
          continue;
        }
        if (*semantic != spec.mnemonic) {
          diagnostics.push_back(LisDiagnostic{
              .code = LisDiagnosticCode::alias_normalized,
              .severity = Severity::info,
              .byte_offset = specification.byte_offset,
              .logical_file_index = selected,
              .data_set_ordinal = ordinal,
              .channel_name = spec.mnemonic,
          });
        }
        if (conversion->unit != spec.unit) {
          diagnostics.push_back(LisDiagnostic{
              .code = LisDiagnosticCode::unit_normalized,
              .severity = Severity::info,
              .byte_offset = specification.byte_offset,
              .logical_file_index = selected,
              .data_set_ordinal = ordinal,
              .channel_name = spec.mnemonic,
          });
        }
        curves.push_back(CurvePlan{.specification_index = spec_index,
                                   .mnemonic = *semantic,
                                   .unit = conversion->unit,
                                   .multiplier = conversion->multiplier});
      }
      if (curves.empty()) {
        diagnostics.push_back(LisDiagnostic{
            .code = LisDiagnosticCode::no_importable_curve,
            .severity = Severity::warning,
            .byte_offset = specification.byte_offset,
            .logical_file_index = selected,
            .data_set_ordinal = ordinal,
            .channel_name = {},
        });
        continue;
      }

      const auto segments = split_axis_segments(coordinates);
      for (std::size_t segment_index{}; segment_index < segments.size();
           ++segment_index) {
        const auto &segment = segments[segment_index];
        std::vector<double> segment_coordinates(
            coordinates.begin() + static_cast<std::ptrdiff_t>(segment.begin),
            coordinates.begin() + static_cast<std::ptrdiff_t>(segment.end));
        const auto axis_id =
            axis_id_for_import(identity, selection.identity_scheme, ordinal,
                               specification.byte_offset, segment.begin,
                               segment_index);
        const auto depth_owner = std::make_shared<const std::vector<double>>(
            std::move(segment_coordinates));
        builder.add_sampling_axis(SamplingAxis{
            .id = axis_id,
            .coordinates = owned_values(depth_owner, source),
            .domain = domain,
            .unit = axis_unit,
            .direction = segment.direction,
        });

        for (const auto &curve : curves) {
          const auto &spec =
              specification.specifications[curve.specification_index];
          auto values = std::make_shared<std::vector<double>>();
          values->reserve(segment.end - segment.begin);
          std::vector<std::uint8_t> nulls;
          nulls.reserve(segment.end - segment.begin);
          for (std::size_t coordinate_index = segment.begin;
               coordinate_index < segment.end; ++coordinate_index) {
            const auto value = decoded.values[curve.specification_index - 1U]
                                             [source_samples[coordinate_index]];
            const auto null_value =
                is_null(value, profile, specification.explicit_null_value);
            if (null_value && std::isfinite(value) &&
                !specification.explicit_null_value.has_value()) {
              diagnostics.push_back(LisDiagnostic{
                  .code = LisDiagnosticCode::inferred_null,
                  .severity = Severity::info,
                  .byte_offset = specification.byte_offset,
                  .logical_file_index = selected,
                  .data_set_ordinal = ordinal,
                  .channel_name = spec.mnemonic,
              });
            } else if (!std::isfinite(value)) {
              diagnostics.push_back(LisDiagnostic{
                  .code = LisDiagnosticCode::non_finite_curve_value,
                  .severity = Severity::warning,
                  .byte_offset = specification.byte_offset,
                  .logical_file_index = selected,
                  .data_set_ordinal = ordinal,
                  .channel_name = spec.mnemonic,
              });
            }
            values->push_back(null_value
                                  ? std::numeric_limits<double>::quiet_NaN()
                                  : value * curve.multiplier);
            nulls.push_back(null_value ? std::uint8_t{1U} : std::uint8_t{0U});
          }
          const auto owner =
              std::shared_ptr<const std::vector<double>>{std::move(values)};
          builder.add_curve(Curve{
              .id = curve_id(identity, selection.identity_scheme, ordinal,
                             specification.byte_offset, segment_index,
                             curve.specification_index),
              .mnemonic = curve.mnemonic,
              .display_name = spec.mnemonic,
              .unit = curve.unit,
              .sampling_axis_id = axis_id,
              .values = owned_values(owner, source),
              .nulls = owned_nulls(nulls, source),
          });
        }
      }
    }

    if (data_set_ordinal == 0U) {
      diagnostics.push_back(LisDiagnostic{
          .code = LisDiagnosticCode::no_importable_curve,
          .severity = Severity::warning,
          .byte_offset = records[range.begin].byte_offset,
          .logical_file_index = selected,
          .data_set_ordinal = 0U,
          .representation = 0U,
          .channel_name = {},
      });
    }

    return LisImport{.document = builder.build(),
                     .diagnostics = std::move(diagnostics)};
  } catch (ParseFailure failure) {
    return failure == ParseFailure::exhausted ? resource_exhausted()
                                              : invalid_lis();
  } catch (const std::bad_alloc &) {
    return resource_exhausted();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace welllog
