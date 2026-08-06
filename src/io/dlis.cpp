#include <welllog/io/dlis.hpp>

#include "adapter_common.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace welllog {
namespace {

constexpr std::uint8_t segment_explicit = 0x80U;
constexpr std::uint8_t segment_predecessor = 0x40U;
constexpr std::uint8_t segment_successor = 0x20U;
constexpr std::uint8_t segment_encrypted = 0x10U;
constexpr std::uint8_t segment_encryption_packet = 0x08U;
constexpr std::uint8_t segment_checksum = 0x04U;
constexpr std::uint8_t segment_trailing_length = 0x02U;
constexpr std::uint8_t segment_padding = 0x01U;

constexpr std::uint8_t explicit_file_header = 0;
constexpr std::uint8_t explicit_origin = 1;
constexpr std::uint8_t explicit_channel = 3;
constexpr std::uint8_t explicit_frame = 4;
constexpr std::uint8_t implicit_frame_data = 0;

constexpr std::uint8_t role_mask = 0xe0U;
constexpr std::uint8_t role_attribute = 0x20U;
constexpr std::uint8_t role_invariant_attribute = 0x40U;
constexpr std::uint8_t role_object = 0x60U;
constexpr std::uint8_t role_absent_attribute = 0x00U;
constexpr std::uint8_t role_set = 0xe0U;

constexpr std::uint8_t has_label = 0x10U;
constexpr std::uint8_t has_count = 0x08U;
constexpr std::uint8_t has_representation = 0x04U;
constexpr std::uint8_t has_units = 0x02U;
constexpr std::uint8_t has_value = 0x01U;

constexpr std::uint8_t representation_fshort = 1;
constexpr std::uint8_t representation_fsingl = 2;
constexpr std::uint8_t representation_fsing1 = 3;
constexpr std::uint8_t representation_fsing2 = 4;
constexpr std::uint8_t representation_isingl = 5;
constexpr std::uint8_t representation_vsingl = 6;
constexpr std::uint8_t representation_fdoubl = 7;
constexpr std::uint8_t representation_fdoub1 = 8;
constexpr std::uint8_t representation_fdoub2 = 9;
constexpr std::uint8_t representation_csingl = 10;
constexpr std::uint8_t representation_cdoubl = 11;
constexpr std::uint8_t representation_sshort = 12;
constexpr std::uint8_t representation_snorm = 13;
constexpr std::uint8_t representation_slong = 14;
constexpr std::uint8_t representation_ushort = 15;
constexpr std::uint8_t representation_unorm = 16;
constexpr std::uint8_t representation_ulong = 17;
constexpr std::uint8_t representation_uvari = 18;
constexpr std::uint8_t representation_ident = 19;
constexpr std::uint8_t representation_ascii = 20;
constexpr std::uint8_t representation_dtime = 21;
constexpr std::uint8_t representation_origin = 22;
constexpr std::uint8_t representation_obname = 23;
constexpr std::uint8_t representation_objref = 24;
constexpr std::uint8_t representation_attref = 25;
constexpr std::uint8_t representation_status = 26;
constexpr std::uint8_t representation_units = 27;

enum class FailureKind : std::uint8_t { malformed, exhausted };

struct ParseFailure final {
  FailureKind kind{FailureKind::malformed};
};

[[noreturn]] void malformed() { throw ParseFailure{FailureKind::malformed}; }
[[noreturn]] void exhausted() { throw ParseFailure{FailureKind::exhausted}; }

using io_detail::axis_direction;
using io_detail::owned_nulls;
using io_detail::owned_values;
using io_detail::stable_id;

[[nodiscard]] Error invalid_dlis() { return io_detail::invalid_document_error(); }
[[nodiscard]] Error resource_exhausted() {
  return io_detail::resource_exhausted_error();
}
[[nodiscard]] Error internal_failure() { return io_detail::internal_error(); }

class Cursor {
public:
  Cursor(std::span<const std::byte> bytes, std::uint64_t base_offset = 0)
      : bytes_(bytes), base_offset_(base_offset) {}

  [[nodiscard]] bool empty() const noexcept { return position_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  [[nodiscard]] std::uint64_t offset() const noexcept {
    return base_offset_ + static_cast<std::uint64_t>(position_);
  }

  [[nodiscard]] std::uint8_t peek_u8() const {
    if (empty()) {
      malformed();
    }
    return std::to_integer<std::uint8_t>(bytes_[position_]);
  }

  [[nodiscard]] std::uint8_t read_u8() {
    const auto value = peek_u8();
    ++position_;
    return value;
  }

  [[nodiscard]] std::uint16_t read_u16() {
    const auto first = read_u8();
    const auto second = read_u8();
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(first) << 8U) |
                                      static_cast<std::uint16_t>(second));
  }

  [[nodiscard]] std::uint32_t read_u32() {
    std::uint32_t value{};
    for (std::uint32_t index{}; index < 4; ++index) {
      value = static_cast<std::uint32_t>((value << 8U) | read_u8());
    }
    return value;
  }

  [[nodiscard]] std::uint64_t read_u64() {
    std::uint64_t value{};
    for (std::uint32_t index{}; index < 8; ++index) {
      value = (value << 8U) | read_u8();
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

[[nodiscard]] std::uint32_t read_uvari(Cursor &cursor) {
  const auto first = cursor.peek_u8();
  if ((first & 0xc0U) == 0x00U) {
    return cursor.read_u8();
  }
  if ((first & 0xc0U) == 0x80U) {
    return static_cast<std::uint32_t>(cursor.read_u16() & 0x3fffU);
  }
  return cursor.read_u32() & 0x3fffffffU;
}

[[nodiscard]] std::string read_ident(Cursor &cursor) {
  const auto length = cursor.read_u8();
  const auto bytes = cursor.read_span(length);
  std::string result;
  result.reserve(length);
  for (const auto byte : bytes) {
    result.push_back(static_cast<char>(std::to_integer<std::uint8_t>(byte)));
  }
  return result;
}

[[nodiscard]] std::string read_ascii(Cursor &cursor) {
  const auto length = read_uvari(cursor);
  const auto bytes = cursor.read_span(length);
  std::string result;
  result.reserve(length);
  for (const auto byte : bytes) {
    result.push_back(static_cast<char>(std::to_integer<std::uint8_t>(byte)));
  }
  return result;
}

[[nodiscard]] DlisObjectReference read_obname(Cursor &cursor) {
  const auto origin = read_uvari(cursor);
  if (origin > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    malformed();
  }
  return DlisObjectReference{.origin = static_cast<std::int32_t>(origin),
                             .copy_number = cursor.read_u8(),
                             .identifier = read_ident(cursor)};
}

[[nodiscard]] float read_float(Cursor &cursor) {
  return std::bit_cast<float>(cursor.read_u32());
}

[[nodiscard]] double read_double(Cursor &cursor) {
  return std::bit_cast<double>(cursor.read_u64());
}

using RawValue = std::variant<std::monostate, double, std::uint32_t, std::string,
                              DlisObjectReference>;

[[nodiscard]] std::optional<double> numeric_value(const RawValue &value) {
  if (const auto number = std::get_if<double>(&value); number != nullptr) {
    return *number;
  }
  if (const auto number = std::get_if<std::uint32_t>(&value); number != nullptr) {
    return static_cast<double>(*number);
  }
  return std::nullopt;
}

[[nodiscard]] RawValue read_value(Cursor &cursor, std::uint8_t representation) {
  switch (representation) {
  case representation_fshort: {
    const auto raw = cursor.read_u16();
    const auto sign = (raw & 0x8000U) == 0U ? 1.0 : -1.0;
    const auto exponent = static_cast<int>(raw & 0x000fU);
    auto fraction = static_cast<double>((raw & 0xfff0U) >> 4U) / 2048.0;
    if ((raw & 0x8000U) != 0U) {
      fraction = static_cast<double>((~((raw & 0xfff0U) >> 4U) & 0x0fffU) + 1U) /
                 2048.0;
    }
    return sign * fraction * std::pow(2.0, exponent);
  }
  case representation_fsingl:
    return static_cast<double>(read_float(cursor));
  default:
    break;
  }
  switch (representation) {
  case representation_fsing1: {
    const auto value = static_cast<double>(read_float(cursor));
    static_cast<void>(read_float(cursor));
    return value;
  }
  case representation_fsing2: {
    const auto value = static_cast<double>(read_float(cursor));
    static_cast<void>(read_float(cursor));
    static_cast<void>(read_float(cursor));
    return value;
  }
  case representation_isingl:
  case representation_vsingl:
    static_cast<void>(cursor.read_span(4));
    return std::monostate{};
  case representation_fdoubl:
    return read_double(cursor);
  case representation_fdoub1: {
    const auto value = read_double(cursor);
    static_cast<void>(read_double(cursor));
    return value;
  }
  case representation_fdoub2: {
    const auto value = read_double(cursor);
    static_cast<void>(read_double(cursor));
    static_cast<void>(read_double(cursor));
    return value;
  }
  case representation_csingl:
    static_cast<void>(cursor.read_span(8));
    return std::monostate{};
  case representation_cdoubl:
    static_cast<void>(cursor.read_span(16));
    return std::monostate{};
  case representation_sshort:
    return static_cast<double>(static_cast<std::int8_t>(cursor.read_u8()));
  case representation_snorm:
    return static_cast<double>(static_cast<std::int16_t>(cursor.read_u16()));
  case representation_slong:
    return static_cast<double>(static_cast<std::int32_t>(cursor.read_u32()));
  case representation_ushort:
    return static_cast<std::uint32_t>(cursor.read_u8());
  case representation_unorm:
    return static_cast<std::uint32_t>(cursor.read_u16());
  case representation_ulong:
    return cursor.read_u32();
  case representation_uvari:
    return read_uvari(cursor);
  case representation_ident:
  case representation_units:
    return read_ident(cursor);
  case representation_ascii:
    return read_ascii(cursor);
  case representation_dtime:
    static_cast<void>(cursor.read_span(8));
    return std::monostate{};
  case representation_origin:
    return read_uvari(cursor);
  case representation_obname:
    return read_obname(cursor);
  case representation_objref:
    static_cast<void>(read_ident(cursor));
    static_cast<void>(read_obname(cursor));
    return std::monostate{};
  case representation_attref:
    static_cast<void>(read_ident(cursor));
    static_cast<void>(read_obname(cursor));
    static_cast<void>(read_ident(cursor));
    return std::monostate{};
  case representation_status:
    return static_cast<std::uint32_t>(cursor.read_u8());
  default:
    malformed();
  }
}

[[nodiscard]] bool is_numeric_representation(std::uint8_t representation) noexcept {
  switch (representation) {
  case representation_fshort:
  case representation_fsingl:
  case representation_fsing1:
  case representation_fsing2:
  case representation_fdoubl:
  case representation_fdoub1:
  case representation_fdoub2:
  case representation_sshort:
  case representation_snorm:
  case representation_slong:
  case representation_ushort:
  case representation_unorm:
  case representation_ulong:
  case representation_uvari:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool equals_ascii(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index{}; index < left.size(); ++index) {
    const auto character = left[index];
    const auto normalized = character >= 'a' && character <= 'z'
                                ? static_cast<char>(character - 'a' + 'A')
                                : character;
    if (normalized != right[index]) {
      return false;
    }
  }
  return true;
}

struct Attribute {
  std::string label;
  std::uint32_t count{1};
  std::uint8_t representation{representation_ident};
  std::string unit;
  std::vector<RawValue> values;
  bool invariant{};
  bool present{true};
};

struct RawObject {
  DlisObjectReference reference;
  std::vector<Attribute> attributes;
};

struct ObjectSet {
  std::string type;
  std::vector<RawObject> objects;
};

void read_attribute_values(Cursor &cursor, Attribute &attribute,
                           const DlisLimits &limits) {
  if (attribute.count > limits.max_samples) {
    exhausted();
  }
  attribute.values.clear();
  attribute.values.reserve(attribute.count);
  for (std::uint32_t index{}; index < attribute.count; ++index) {
    attribute.values.push_back(read_value(cursor, attribute.representation));
  }
}

[[nodiscard]] ObjectSet parse_object_set(Cursor &cursor, const DlisLimits &limits) {
  if (cursor.empty()) {
    malformed();
  }
  const auto set_descriptor = cursor.read_u8();
  if ((set_descriptor & role_mask) != role_set ||
      (set_descriptor & has_label) == 0U) {
    malformed();
  }

  ObjectSet result;
  result.type = read_ident(cursor);
  std::vector<Attribute> template_attributes;
  while (!cursor.empty()) {
    const auto descriptor = cursor.peek_u8();
    const auto role = descriptor & role_mask;
    if (role == role_object) {
      break;
    }
    static_cast<void>(cursor.read_u8());
    if (role == role_absent_attribute) {
      continue;
    }
    if ((role != role_attribute && role != role_invariant_attribute) ||
        (descriptor & has_label) == 0U) {
      malformed();
    }
    Attribute attribute;
    attribute.invariant = role == role_invariant_attribute;
    attribute.label = read_ident(cursor);
    if ((descriptor & has_count) != 0U) {
      attribute.count = read_uvari(cursor);
    }
    if ((descriptor & has_representation) != 0U) {
      attribute.representation = cursor.read_u8();
    }
    if ((descriptor & has_units) != 0U) {
      attribute.unit = read_ident(cursor);
    }
    if ((descriptor & has_value) != 0U) {
      read_attribute_values(cursor, attribute, limits);
    }
    template_attributes.push_back(std::move(attribute));
  }

  while (!cursor.empty()) {
    const auto descriptor = cursor.read_u8();
    if ((descriptor & role_mask) != role_object || (descriptor & has_label) == 0U) {
      malformed();
    }
    RawObject object;
    object.reference = read_obname(cursor);
    object.attributes = template_attributes;
    for (std::size_t index{}; index < template_attributes.size(); ++index) {
      if (template_attributes[index].invariant || cursor.empty()) {
        continue;
      }
      const auto patch_descriptor = cursor.peek_u8();
      if ((patch_descriptor & role_mask) == role_object) {
        break;
      }
      static_cast<void>(cursor.read_u8());
      auto &attribute = object.attributes[index];
      const auto role = patch_descriptor & role_mask;
      if (role == role_absent_attribute) {
        attribute.present = false;
        continue;
      }
      if ((role != role_attribute && role != role_invariant_attribute) ||
          (patch_descriptor & has_label) != 0U) {
        malformed();
      }
      if ((patch_descriptor & has_count) != 0U) {
        attribute.count = read_uvari(cursor);
      }
      if ((patch_descriptor & has_representation) != 0U) {
        attribute.representation = cursor.read_u8();
      }
      if ((patch_descriptor & has_units) != 0U) {
        attribute.unit = read_ident(cursor);
      }
      if ((patch_descriptor & has_value) != 0U) {
        read_attribute_values(cursor, attribute, limits);
      }
    }
    result.objects.push_back(std::move(object));
  }
  return result;
}

[[nodiscard]] const Attribute *attribute_named(const RawObject &object,
                                                std::string_view name) {
  const auto found = std::find_if(
      object.attributes.begin(), object.attributes.end(), [name](const Attribute &attribute) {
        return attribute.present && equals_ascii(attribute.label, name);
      });
  return found == object.attributes.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::uint32_t> unsigned_attribute(
    const RawObject &object, std::string_view name) {
  const auto *attribute = attribute_named(object, name);
  if (attribute == nullptr || attribute->values.empty()) {
    return std::nullopt;
  }
  if (const auto value = std::get_if<std::uint32_t>(&attribute->values.front());
      value != nullptr) {
    return *value;
  }
  if (const auto value = std::get_if<double>(&attribute->values.front());
      value != nullptr && *value >= 0.0 &&
          *value <= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    return static_cast<std::uint32_t>(*value);
  }
  return std::nullopt;
}

[[nodiscard]] std::string string_attribute(const RawObject &object,
                                           std::string_view name) {
  const auto *attribute = attribute_named(object, name);
  if (attribute == nullptr || attribute->values.empty()) {
    return {};
  }
  const auto *value = std::get_if<std::string>(&attribute->values.front());
  return value == nullptr ? std::string{} : *value;
}

[[nodiscard]] std::vector<DlisObjectReference> references_attribute(
    const RawObject &object, std::string_view name) {
  std::vector<DlisObjectReference> result;
  const auto *attribute = attribute_named(object, name);
  if (attribute == nullptr) {
    return result;
  }
  result.reserve(attribute->values.size());
  for (const auto &value : attribute->values) {
    if (const auto *reference = std::get_if<DlisObjectReference>(&value);
        reference != nullptr) {
      result.push_back(*reference);
    }
  }
  return result;
}

[[nodiscard]] std::vector<std::uint32_t> dimensions_attribute(const RawObject &object) {
  std::vector<std::uint32_t> result;
  const auto *attribute = attribute_named(object, "DIMENSION");
  if (attribute == nullptr) {
    return result;
  }
  result.reserve(attribute->values.size());
  for (const auto &value : attribute->values) {
    if (const auto number = std::get_if<std::uint32_t>(&value); number != nullptr) {
      result.push_back(*number);
    }
  }
  return result;
}

[[nodiscard]] std::uint32_t dimension_size(const std::vector<std::uint32_t> &dimensions,
                                           const DlisLimits &limits) {
  if (dimensions.empty()) {
    return 1;
  }
  std::uint64_t result{1};
  for (const auto dimension : dimensions) {
    if (dimension == 0U || result > limits.max_samples / dimension) {
      exhausted();
    }
    result *= dimension;
  }
  return static_cast<std::uint32_t>(result);
}

struct Record {
  bool explicit_formatting{};
  std::uint8_t type{};
  std::uint64_t byte_offset{};
  std::vector<std::byte> body;
};

struct PhysicalFile {
  std::vector<Record> records;
  std::vector<DlisDiagnostic> diagnostics;
};

[[nodiscard]] PhysicalFile parse_physical(std::span<const std::byte> bytes,
                                           const DlisLimits &limits) {
  if (bytes.size() > limits.max_input_bytes) {
    exhausted();
  }
  if (bytes.size() < 80 || std::to_integer<unsigned char>(bytes[4]) != 'V' ||
      std::to_integer<unsigned char>(bytes[5]) != '1' ||
      std::to_integer<unsigned char>(bytes[6]) != '.') {
    malformed();
  }

  PhysicalFile result;
  Cursor file(bytes.subspan(80), 80);
  std::optional<Record> pending;
  while (!file.empty()) {
    const auto visible_offset = file.offset();
    const auto visible_length = file.read_u16();
    if (visible_length < 4U || file.read_u8() != 0xffU || file.read_u8() != 0x01U ||
        visible_length - 4U > file.remaining()) {
      malformed();
    }
    Cursor visible(file.read_span(visible_length - 4U), visible_offset + 4U);
    while (!visible.empty()) {
      const auto segment_offset = visible.offset();
      const auto segment_length = visible.read_u16();
      const auto attributes = visible.read_u8();
      const auto type = visible.read_u8();
      if (segment_length < 4U || segment_length - 4U > visible.remaining()) {
        malformed();
      }
      auto payload = visible.read_span(segment_length - 4U);
      if ((attributes & segment_encryption_packet) != 0U) {
        if (payload.empty()) {
          malformed();
        }
        const auto packet_length = std::to_integer<std::uint8_t>(payload.front());
        if (packet_length > payload.size()) {
          malformed();
        }
        payload = payload.subspan(packet_length);
      }
      std::size_t trim{};
      if ((attributes & segment_encrypted) == 0U) {
        if ((attributes & segment_checksum) != 0U) {
          trim += 2U;
        }
        if ((attributes & segment_trailing_length) != 0U) {
          trim += 2U;
        }
        if ((attributes & segment_padding) != 0U) {
          if (trim >= payload.size()) {
            malformed();
          }
          trim += std::to_integer<std::uint8_t>(payload[payload.size() - trim - 1U]);
        }
      }
      if (trim > payload.size()) {
        malformed();
      }
      payload = payload.first(payload.size() - trim);

      const auto has_predecessor = (attributes & segment_predecessor) != 0U;
      const auto has_successor = (attributes & segment_successor) != 0U;
      if (!has_predecessor) {
        if (pending.has_value()) {
          malformed();
        }
        pending = Record{.explicit_formatting = (attributes & segment_explicit) != 0U,
                         .type = type,
                         .byte_offset = segment_offset,
                         .body = {}};
      }
      if (!pending.has_value() || pending->type != type ||
          pending->explicit_formatting != ((attributes & segment_explicit) != 0U)) {
        malformed();
      }
      if (payload.size() > limits.max_logical_record_bytes - pending->body.size()) {
        exhausted();
      }
      pending->body.insert(pending->body.end(), payload.begin(), payload.end());
      if ((attributes & segment_encrypted) != 0U) {
        result.diagnostics.push_back(DlisDiagnostic{
            .code = DlisDiagnosticCode::encrypted_record,
            .severity = Severity::warning,
            .byte_offset = segment_offset,
            .logical_file_index = 0,
            .frame = {},
            .channel = {},
        });
      }
      if (!has_successor) {
        if (result.records.size() == limits.max_logical_records) {
          exhausted();
        }
        result.records.push_back(std::move(*pending));
        pending.reset();
      }
    }
  }
  if (pending.has_value()) {
    malformed();
  }
  return result;
}

struct InternalChannel {
  DlisChannelDescriptor descriptor;
  std::uint8_t representation{};
  std::uint32_t element_count{};
  std::uint64_t byte_offset{};
};

struct InternalFrame {
  DlisFrameDescriptor descriptor;
  std::vector<DlisObjectReference> channel_references;
  std::uint64_t byte_offset{};
};

struct InternalLogicalFile {
  std::vector<InternalChannel> channels;
  std::vector<InternalFrame> frames;
};

struct Model {
  DlisCatalog catalog;
  std::vector<InternalLogicalFile> logical_files;
  std::vector<DlisDiagnostic> diagnostics;
};

[[nodiscard]] const InternalChannel *find_channel(const InternalLogicalFile &logical_file,
                                                   const DlisObjectReference &reference) {
  const auto found = std::find_if(logical_file.channels.begin(), logical_file.channels.end(),
                                  [&reference](const InternalChannel &channel) {
                                    return channel.descriptor.reference == reference;
                                  });
  return found == logical_file.channels.end() ? nullptr : &*found;
}

[[nodiscard]] const InternalFrame *find_frame(const InternalLogicalFile &logical_file,
                                               const DlisObjectReference &reference) {
  const auto found = std::find_if(logical_file.frames.begin(), logical_file.frames.end(),
                                  [&reference](const InternalFrame &frame) {
                                    return frame.descriptor.reference == reference;
                                  });
  return found == logical_file.frames.end() ? nullptr : &*found;
}

[[nodiscard]] Model build_model(const PhysicalFile &physical, const DlisLimits &limits) {
  Model result;
  std::optional<std::size_t> current_file;
  for (const auto &record : physical.records) {
    if (record.explicit_formatting && record.type == explicit_file_header) {
      const auto index = result.logical_files.size();
      result.logical_files.push_back({});
      result.catalog.logical_files.push_back(
          DlisLogicalFileDescriptor{.index = static_cast<std::uint32_t>(index), .frames = {}});
      current_file = index;
      continue;
    }
    if (!current_file.has_value() || !record.explicit_formatting || record.body.empty()) {
      continue;
    }
    if (record.type != explicit_channel && record.type != explicit_frame) {
      if (record.type != explicit_origin) {
        result.diagnostics.push_back(DlisDiagnostic{
            .code = DlisDiagnosticCode::unsupported_object_type,
            .severity = Severity::warning,
            .byte_offset = record.byte_offset,
            .logical_file_index = static_cast<std::uint32_t>(*current_file),
            .frame = {},
            .channel = {},
        });
      }
      continue;
    }
    Cursor cursor(record.body, record.byte_offset + 4U);
    const auto objects = parse_object_set(cursor, limits);
    auto &logical_file = result.logical_files[*current_file];
    if (record.type == explicit_channel && equals_ascii(objects.type, "CHANNEL")) {
      for (const auto &object : objects.objects) {
        const auto representation = unsigned_attribute(object, "REPRESENTATION-CODE");
        const auto dimensions = dimensions_attribute(object);
        const auto element_count = dimension_size(dimensions, limits);
        const auto code = representation.value_or(0U);
        if (code == 0U || code > representation_units ||
            !is_numeric_representation(static_cast<std::uint8_t>(code))) {
          result.diagnostics.push_back(DlisDiagnostic{
              .code = DlisDiagnosticCode::unsupported_channel_representation,
              .severity = Severity::warning,
              .byte_offset = record.byte_offset,
              .logical_file_index = static_cast<std::uint32_t>(*current_file),
              .frame = {},
              .channel = object.reference,
          });
        }
        logical_file.channels.push_back(InternalChannel{
            .descriptor = DlisChannelDescriptor{.reference = object.reference,
                                                .unit = string_attribute(object, "UNITS"),
                                                .dimensions = dimensions},
            .representation = code <= representation_units
                                  ? static_cast<std::uint8_t>(code)
                                  : std::uint8_t{0},
            .element_count = element_count,
            .byte_offset = record.byte_offset,
        });
      }
    } else if (record.type == explicit_frame && equals_ascii(objects.type, "FRAME")) {
      for (const auto &object : objects.objects) {
        logical_file.frames.push_back(InternalFrame{
            .descriptor = DlisFrameDescriptor{.reference = object.reference,
                                              .index_type = string_attribute(object, "INDEX-TYPE"),
                                              .direction = string_attribute(object, "DIRECTION"),
                                              .channels = {}},
            .channel_references = references_attribute(object, "CHANNELS"),
            .byte_offset = record.byte_offset,
        });
      }
    } else {
      result.diagnostics.push_back(DlisDiagnostic{
          .code = DlisDiagnosticCode::unsupported_object_type,
          .severity = Severity::warning,
          .byte_offset = record.byte_offset,
          .logical_file_index = static_cast<std::uint32_t>(*current_file),
          .frame = {},
          .channel = {},
      });
    }
  }
  if (result.logical_files.empty()) {
    malformed();
  }
  for (std::size_t file_index{}; file_index < result.logical_files.size(); ++file_index) {
    const auto &internal_file = result.logical_files[file_index];
    auto &catalog_file = result.catalog.logical_files[file_index];
    catalog_file.frames.reserve(internal_file.frames.size());
    for (const auto &frame : internal_file.frames) {
      auto descriptor = frame.descriptor;
      descriptor.channels.reserve(frame.channel_references.size());
      for (const auto &reference : frame.channel_references) {
        if (const auto *channel = find_channel(internal_file, reference); channel != nullptr) {
          descriptor.channels.push_back(channel->descriptor);
        }
      }
      catalog_file.frames.push_back(std::move(descriptor));
    }
  }
  return result;
}

[[nodiscard]] std::string reference_identity(const DlisObjectReference &reference) {
  return std::to_string(reference.origin) + ":" + std::to_string(reference.copy_number) + ":" +
         reference.identifier;
}

[[nodiscard]] std::string source_identity(const BufferSourceReference &source,
                                          std::span<const std::byte> bytes) {
  if (!source.uri.empty() || !source.checksum.empty()) {
    return source.uri + "#" + source.checksum;
  }
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto byte : bytes) {
    hash ^= std::to_integer<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return "dlis:" + std::to_string(hash);
}

void append_diagnostic(std::vector<DlisDiagnostic> &diagnostics, DlisDiagnosticCode code,
                       std::uint64_t byte_offset, std::uint32_t logical_file_index,
                       const DlisObjectReference &frame = {},
                       const DlisObjectReference &channel = {}) {
  diagnostics.push_back(DlisDiagnostic{.code = code,
                                       .severity = Severity::warning,
                                       .byte_offset = byte_offset,
                                       .logical_file_index = logical_file_index,
                                       .frame = frame,
                                       .channel = channel});
}

[[nodiscard]] bool contains_reference(const std::vector<DlisObjectReference> &references,
                                      const DlisObjectReference &reference) {
  return std::find(references.begin(), references.end(), reference) != references.end();
}

struct CurveAccumulator {
  const InternalChannel *channel{};
  std::vector<double> values;
  std::vector<std::uint8_t> nulls;
};

struct MonotonicSegment {
  std::size_t begin{};
  std::size_t end{};
  AxisDirection direction{AxisDirection::increasing};
};

[[nodiscard]] std::vector<MonotonicSegment>
monotonic_segments(const std::vector<double> &coordinates) {
  std::vector<MonotonicSegment> result;
  if (coordinates.empty()) {
    return result;
  }

  std::size_t begin{};
  std::optional<AxisDirection> direction;
  for (std::size_t index{1}; index < coordinates.size(); ++index) {
    if (coordinates[index] == coordinates[index - 1U]) {
      continue;
    }
    const auto next = coordinates[index] > coordinates[index - 1U]
                          ? AxisDirection::increasing
                          : AxisDirection::decreasing;
    if (!direction.has_value()) {
      direction = next;
      continue;
    }
    if (*direction != next) {
      result.push_back(MonotonicSegment{.begin = begin,
                                        .end = index,
                                        .direction = *direction});
      begin = index;
      direction = next;
    }
  }
  result.push_back(MonotonicSegment{
      .begin = begin,
      .end = coordinates.size(),
      .direction = direction.value_or(AxisDirection::increasing),
  });
  return result;
}

[[nodiscard]] Result<DlisImport> import_selected(const PhysicalFile &physical,
                                                  const Model &model,
                                                  std::span<const std::byte> bytes,
                                                  BufferSourceReference source,
                                                  const DlisSelection &selection,
                                                  const DlisLimits &limits) {
  if (selection.logical_file_index == UINT32_MAX || selection.channels.empty() ||
      selection.frame.identifier.empty() ||
      selection.logical_file_index >= model.logical_files.size()) {
    return invalid_dlis();
  }
  const auto &logical_file = model.logical_files[selection.logical_file_index];
  const auto *frame = find_frame(logical_file, selection.frame);
  if (frame == nullptr || frame->channel_references.empty()) {
    return invalid_dlis();
  }
  for (std::size_t index{}; index < selection.channels.size(); ++index) {
    if (!contains_reference(frame->channel_references, selection.channels[index]) ||
        std::find(selection.channels.begin(), selection.channels.begin() +
                                                 static_cast<std::ptrdiff_t>(index),
                  selection.channels[index]) != selection.channels.begin() +
                                                   static_cast<std::ptrdiff_t>(index)) {
      return invalid_dlis();
    }
  }
  const auto *axis_channel = find_channel(logical_file, frame->channel_references.front());
  if (axis_channel == nullptr ||
      !contains_reference(selection.channels, axis_channel->descriptor.reference) ||
      axis_channel->element_count != 1U) {
    return invalid_dlis();
  }

  std::vector<DlisDiagnostic> diagnostics = physical.diagnostics;
  diagnostics.insert(diagnostics.end(), model.diagnostics.begin(), model.diagnostics.end());
  const auto identity = source_identity(source, bytes) + "/" +
                        std::to_string(selection.logical_file_index) + "/" +
                        reference_identity(frame->descriptor.reference);
  const auto document_id = stable_id(identity, "document");
  const auto has_undecodable_channel = std::any_of(
      frame->channel_references.begin(), frame->channel_references.end(),
      [&logical_file](const DlisObjectReference &reference) {
        const auto *channel = find_channel(logical_file, reference);
        return channel != nullptr && channel->representation == 0U;
      });
  if (has_undecodable_channel ||
      !is_numeric_representation(axis_channel->representation)) {
    WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
    auto document = builder.build();
    if (document.id().is_nil()) {
      return resource_exhausted();
    }
    return DlisImport{.document = std::move(document),
                      .diagnostics = std::move(diagnostics)};
  }

  std::vector<CurveAccumulator> curves;
  for (const auto &reference : selection.channels) {
    if (reference == axis_channel->descriptor.reference) {
      continue;
    }
    const auto *channel = find_channel(logical_file, reference);
    if (channel == nullptr) {
      return invalid_dlis();
    }
    if (channel->element_count != 1U) {
      append_diagnostic(diagnostics, DlisDiagnosticCode::unsupported_channel_dimension,
                        channel->byte_offset, selection.logical_file_index,
                        frame->descriptor.reference, channel->descriptor.reference);
      continue;
    }
    if (!is_numeric_representation(channel->representation)) {
      continue;
    }
    curves.push_back(CurveAccumulator{.channel = channel, .values = {}, .nulls = {}});
  }

  std::vector<double> coordinates;
  std::uint64_t decoded_rows{};
  for (const auto &record : physical.records) {
    if (record.explicit_formatting || record.type != implicit_frame_data || record.body.empty()) {
      continue;
    }
    try {
      Cursor cursor(record.body, record.byte_offset + 4U);
      const auto data_descriptor = read_obname(cursor);
      if (data_descriptor != frame->descriptor.reference) {
        continue;
      }
      while (!cursor.empty()) {
        if (decoded_rows == limits.max_samples) {
          exhausted();
        }
        ++decoded_rows;
        static_cast<void>(read_uvari(cursor));
        std::optional<double> axis_value;
        std::vector<std::optional<double>> row_values(curves.size());
        for (const auto &channel_reference : frame->channel_references) {
          const auto *channel = find_channel(logical_file, channel_reference);
          if (channel == nullptr) {
            malformed();
          }
          const auto elements = channel->element_count;
          std::optional<double> scalar;
          for (std::uint32_t element{}; element < elements; ++element) {
            const auto value = read_value(cursor, channel->representation);
            if (element == 0U) {
              scalar = numeric_value(value);
            }
          }
          if (channel->descriptor.reference == axis_channel->descriptor.reference) {
            axis_value = scalar;
          }
          for (std::size_t curve_index{}; curve_index < curves.size(); ++curve_index) {
            if (curves[curve_index].channel->descriptor.reference == channel->descriptor.reference) {
              row_values[curve_index] = scalar;
            }
          }
        }
        if (!axis_value.has_value() || !std::isfinite(*axis_value)) {
          append_diagnostic(diagnostics, DlisDiagnosticCode::non_finite_axis_value,
                            record.byte_offset, selection.logical_file_index,
                            frame->descriptor.reference, axis_channel->descriptor.reference);
          continue;
        }
        coordinates.push_back(*axis_value);
        for (std::size_t curve_index{}; curve_index < curves.size(); ++curve_index) {
          const auto value = row_values[curve_index];
          const auto null = !value.has_value() || !std::isfinite(*value);
          curves[curve_index].values.push_back(
              null ? std::numeric_limits<double>::quiet_NaN() : *value);
          curves[curve_index].nulls.push_back(null ? std::uint8_t{1} : std::uint8_t{0});
          if (null) {
            append_diagnostic(diagnostics, DlisDiagnosticCode::non_finite_curve_value,
                              record.byte_offset, selection.logical_file_index,
                              frame->descriptor.reference,
                              curves[curve_index].channel->descriptor.reference);
          }
        }
      }
    } catch (const ParseFailure &failure) {
      if (failure.kind == FailureKind::exhausted) {
        throw;
      }
      append_diagnostic(diagnostics, DlisDiagnosticCode::malformed_frame_data,
                        record.byte_offset, selection.logical_file_index,
                        frame->descriptor.reference);
    }
  }
  if (coordinates.empty()) {
    return invalid_dlis();
  }

  BufferSourceReference buffer_source = std::move(source);
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  const auto segments = monotonic_segments(coordinates);
  for (std::size_t segment_index{}; segment_index < segments.size(); ++segment_index) {
    const auto &segment = segments[segment_index];
    const auto axis_id = stable_id(
        identity, "sampling-axis/" + reference_identity(axis_channel->descriptor.reference),
        static_cast<std::uint64_t>(segment_index));
    auto segment_coordinates = std::vector<double>(
        coordinates.begin() + static_cast<std::ptrdiff_t>(segment.begin),
        coordinates.begin() + static_cast<std::ptrdiff_t>(segment.end));
    const auto coordinate_owner =
        std::make_shared<const std::vector<double>>(std::move(segment_coordinates));
    builder.add_sampling_axis(SamplingAxis{
        .id = axis_id,
        .coordinates = owned_values(coordinate_owner, buffer_source),
        .domain = equals_ascii(frame->descriptor.index_type, "BOREHOLE-DEPTH")
                      ? DepthDomain::measured_depth
                      : DepthDomain::source_index,
        .unit = axis_channel->descriptor.unit,
        .direction = segment.direction,
    });
    for (const auto &curve : curves) {
      auto segment_values = std::vector<double>(
          curve.values.begin() + static_cast<std::ptrdiff_t>(segment.begin),
          curve.values.begin() + static_cast<std::ptrdiff_t>(segment.end));
      auto segment_nulls = std::vector<std::uint8_t>(
          curve.nulls.begin() + static_cast<std::ptrdiff_t>(segment.begin),
          curve.nulls.begin() + static_cast<std::ptrdiff_t>(segment.end));
      const auto curve_owner =
          std::make_shared<const std::vector<double>>(std::move(segment_values));
      const auto &channel = curve.channel->descriptor;
      builder.add_curve(Curve{
          .id = stable_id(identity, "curve/" + reference_identity(channel.reference),
                          static_cast<std::uint64_t>(segment_index)),
          .mnemonic = channel.reference.identifier,
          .display_name = channel.reference.identifier,
          .unit = channel.unit,
          .sampling_axis_id = axis_id,
          .values = owned_values(curve_owner, buffer_source),
          .nulls = owned_nulls(segment_nulls, buffer_source),
      });
    }
  }
  auto document = builder.build();
  if (document.id().is_nil()) {
    return resource_exhausted();
  }
  return DlisImport{.document = std::move(document), .diagnostics = std::move(diagnostics)};
}

} // namespace

Result<DlisInspection> DlisSourceAdapter::inspect(std::span<const std::byte> bytes,
                                                   DlisLimits limits) {
  try {
    auto physical = parse_physical(bytes, limits);
    auto model = build_model(physical, limits);
    physical.diagnostics.insert(physical.diagnostics.end(), model.diagnostics.begin(),
                                model.diagnostics.end());
    return DlisInspection{.catalog = std::move(model.catalog),
                          .diagnostics = std::move(physical.diagnostics)};
  } catch (const ParseFailure &failure) {
    return failure.kind == FailureKind::exhausted ? resource_exhausted() : invalid_dlis();
  } catch (const std::bad_alloc &) {
    return resource_exhausted();
  } catch (...) {
    return internal_failure();
  }
}

Result<DlisImport> DlisSourceAdapter::import(std::span<const std::byte> bytes,
                                             BufferSourceReference source,
                                             const DlisSelection &selection,
                                             DlisLimits limits) {
  try {
    const auto physical = parse_physical(bytes, limits);
    const auto model = build_model(physical, limits);
    return import_selected(physical, model, bytes, std::move(source), selection, limits);
  } catch (const ParseFailure &failure) {
    return failure.kind == FailureKind::exhausted ? resource_exhausted() : invalid_dlis();
  } catch (const std::bad_alloc &) {
    return resource_exhausted();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace welllog
