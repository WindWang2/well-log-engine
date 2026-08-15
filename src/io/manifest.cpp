#include <welllog/io/manifest.hpp>

#include <welllog/core/checked_math.hpp>
#include <welllog/io/asset_security.hpp>
#include <welllog/io/container_security.hpp>

#include <cctype>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace welllog {
namespace {

struct JsonNumber {
  std::string text;
};

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
  std::variant<std::nullptr_t, bool, JsonNumber, std::string, JsonArray,
               JsonObject>
      value;
};

class ParseFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class JsonParser {
public:
  explicit JsonParser(std::string_view input,
                      ContainerSecurityLimits limits =
                          default_container_security_limits())
      : input_(input), limits_(limits) {
    if (input_.size() > limits_.max_manifest_bytes) {
      throw ParseFailure{"manifest exceeds maximum size"};
    }
  }

  [[nodiscard]] JsonValue parse() {
    auto value = parse_value(0);
    skip_space();
    if (position_ != input_.size()) {
      fail("unexpected trailing JSON data");
    }
    return value;
  }

private:
  [[noreturn]] void fail(const char *message) const {
    // Never embed raw JSON payload slices (may contain curve samples).
    throw ParseFailure{std::string{message} + " at byte " +
                       std::to_string(position_)};
  }

  void skip_space() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
      ++position_;
    }
  }

  [[nodiscard]] bool consume(char expected) {
    skip_space();
    if (position_ < input_.size() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  void expect(std::string_view expected) {
    skip_space();
    if (input_.substr(position_, expected.size()) != expected) {
      fail("unexpected JSON token");
    }
    position_ += expected.size();
  }

  [[nodiscard]] JsonValue parse_value(std::size_t depth) {
    if (depth > limits_.max_json_depth) {
      fail("JSON nesting limit exceeded");
    }
    skip_space();
    if (position_ >= input_.size()) {
      fail("unexpected end of JSON");
    }
    switch (input_[position_]) {
    case '{':
      return JsonValue{parse_object(depth + 1)};
    case '[':
      return JsonValue{parse_array(depth + 1)};
    case '"':
      return JsonValue{parse_string()};
    case 't':
      expect("true");
      return JsonValue{true};
    case 'f':
      expect("false");
      return JsonValue{false};
    case 'n':
      expect("null");
      return JsonValue{nullptr};
    default:
      if (input_[position_] == '-' ||
          std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        return JsonValue{parse_number()};
      }
      fail("invalid JSON value");
    }
  }

  [[nodiscard]] JsonObject parse_object(std::size_t depth) {
    if (!consume('{')) {
      fail("expected object");
    }
    JsonObject result;
    if (consume('}')) {
      return result;
    }
    while (true) {
      if (result.size() >= limits_.max_json_object_keys) {
        fail("JSON object key limit exceeded");
      }
      skip_space();
      if (position_ >= input_.size() || input_[position_] != '"') {
        fail("expected object key");
      }
      auto key = parse_string();
      if (!consume(':')) {
        fail("expected colon");
      }
      auto [_, inserted] = result.emplace(std::move(key), parse_value(depth));
      if (!inserted) {
        fail("duplicate object key");
      }
      if (consume('}')) {
        return result;
      }
      if (!consume(',')) {
        fail("expected comma");
      }
    }
  }

  [[nodiscard]] JsonArray parse_array(std::size_t depth) {
    if (!consume('[')) {
      fail("expected array");
    }
    JsonArray result;
    if (consume(']')) {
      return result;
    }
    while (true) {
      if (result.size() >= limits_.max_json_array_elements) {
        fail("JSON array element limit exceeded");
      }
      result.push_back(parse_value(depth));
      if (consume(']')) {
        return result;
      }
      if (!consume(',')) {
        fail("expected comma");
      }
    }
  }

  static void append_utf8(std::string &output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
      output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
  }

  [[nodiscard]] std::string parse_string() {
    if (!consume('"')) {
      fail("expected string");
    }
    std::string result;
    while (position_ < input_.size()) {
      if (result.size() >= limits_.max_json_string_bytes) {
        fail("JSON string length limit exceeded");
      }
      const auto character = static_cast<unsigned char>(input_[position_++]);
      if (character == '"') {
        return result;
      }
      if (character < 0x20) {
        fail("unescaped control character");
      }
      if (character != '\\') {
        result.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= input_.size()) {
        fail("unterminated escape");
      }
      switch (input_[position_++]) {
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case '/':
        result.push_back('/');
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        if (position_ + 4 > input_.size()) {
          fail("short unicode escape");
        }
        std::uint32_t codepoint{};
        const auto [end, error] =
            std::from_chars(input_.data() + position_,
                            input_.data() + position_ + 4, codepoint, 16);
        if (error != std::errc{} || end != input_.data() + position_ + 4 ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
          fail("invalid unicode escape");
        }
        position_ += 4;
        append_utf8(result, codepoint);
        break;
      }
      default:
        fail("invalid string escape");
      }
    }
    fail("unterminated string");
  }

  [[nodiscard]] JsonNumber parse_number() {
    skip_space();
    const auto start = position_;
    if (input_[position_] == '-') {
      ++position_;
    }
    if (position_ >= input_.size()) {
      fail("short number");
    }
    if (input_[position_] == '0') {
      ++position_;
    } else {
      if (std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
        fail("invalid number");
      }
      while (position_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        ++position_;
      }
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const auto fractional_start = position_;
      while (position_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        ++position_;
      }
      if (fractional_start == position_) {
        fail("invalid fraction");
      }
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      const auto exponent_start = position_;
      while (position_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
        ++position_;
      }
      if (exponent_start == position_) {
        fail("invalid exponent");
      }
    }
    return JsonNumber{std::string{input_.substr(start, position_ - start)}};
  }

  std::string_view input_;
  std::size_t position_{};
  ContainerSecurityLimits limits_{};
};

[[nodiscard]] const JsonObject &object(const JsonValue &value) {
  if (const auto *result = std::get_if<JsonObject>(&value.value)) {
    return *result;
  }
  throw ParseFailure{"expected JSON object"};
}

[[nodiscard]] const JsonArray &array(const JsonValue &value) {
  if (const auto *result = std::get_if<JsonArray>(&value.value)) {
    return *result;
  }
  throw ParseFailure{"expected JSON array"};
}

[[nodiscard]] const JsonValue &field(const JsonObject &value,
                                     std::string_view name) {
  const auto found = value.find(name);
  if (found == value.end()) {
    throw ParseFailure{"missing field: " + std::string{name}};
  }
  return found->second;
}

void require_exact_fields(const JsonObject &value,
                          std::initializer_list<std::string_view> names) {
  if (value.size() != names.size()) {
    throw ParseFailure{"JSON object does not match manifest schema"};
  }
  for (const auto name : names) {
    static_cast<void>(field(value, name));
  }
}

[[nodiscard]] const std::string &string(const JsonValue &value) {
  if (const auto *result = std::get_if<std::string>(&value.value)) {
    return *result;
  }
  throw ParseFailure{"expected JSON string"};
}

[[nodiscard]] std::uint64_t unsigned_integer(const JsonValue &value) {
  const auto *number = std::get_if<JsonNumber>(&value.value);
  if (number == nullptr || number->text.empty() ||
      number->text.front() == '-') {
    throw ParseFailure{"expected unsigned integer"};
  }
  std::uint64_t result{};
  const auto [end, error] = std::from_chars(
      number->text.data(), number->text.data() + number->text.size(), result);
  if (error != std::errc{} ||
      end != number->text.data() + number->text.size()) {
    throw ParseFailure{"unsigned integer is out of range"};
  }
  return result;
}

[[nodiscard]] std::string number_text(double value) {
  // Locale-independent writer (issue #473): std::to_string(double) emits a
  // locale-dependent decimal separator (",") under comma locales, corrupting
  // the JSON round-trip; from_chars/to_chars are locale-free. Non-finite
  // values degrade to 0 (JSON has no NaN/Infinity).
  if (!std::isfinite(value)) {
    return "0";
  }
  char buffer[48]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::general);
  if (result.ec != std::errc{}) {
    return "0";
  }
  return std::string(buffer, result.ptr);
}

[[nodiscard]] double number(const JsonValue &value) {
  const auto *num = std::get_if<JsonNumber>(&value.value);
  if (num == nullptr) {
    throw ParseFailure{"expected JSON number"};
  }
  // Locale-independent parse (issue #473): std::stod honours the C locale —
  // under a comma-decimal locale "0.5" silently parsed as 0 and stopped at
  // '.', corrupting geometry. from_chars is locale-free and requires the
  // ENTIRE token to be consumed.
  // Strictness tradeoff: from_chars only matches finite patterns, so a
  // manifest written before the issue-#473 NaN-free writer whose JSON held
  // nan/inf can no longer be loaded ("number is out of range"). New
  // manifests never contain them — number_text() degrades non-finite values
  // to 0.
  double parsed = 0.0;
  const auto *begin = num->text.data();
  const auto *end = begin + num->text.size();
  const auto result =
      std::from_chars(begin, end, parsed, std::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw ParseFailure{"number is out of range"};
  }
  return parsed;
}

[[nodiscard]] bool boolean(const JsonValue &value) {
  if (const auto *result = std::get_if<bool>(&value.value)) {
    return *result;
  }
  throw ParseFailure{"expected JSON boolean"};
}

// Optional v2 axis field: absent = irregular/unknown sampling. The schema
// validation above already guarantees a number when the key is present.
[[nodiscard]] std::optional<double> parse_optional_interval(
    const JsonObject &axis) {
  const auto found = axis.find("nominalInterval");
  if (found == axis.end()) {
    return std::nullopt;
  }
  return number(found->second);
}

[[nodiscard]] EntityId entity_id(const JsonValue &value) {
  const auto parsed = EntityId::parse(string(value));
  if (!parsed || parsed->is_nil()) {
    throw ParseFailure{"invalid entity identity"};
  }
  return *parsed;
}

void append_escaped(std::string &output, std::string_view text) {
  output.push_back('"');
  constexpr char hex[] = "0123456789abcdef";
  for (const auto raw : text) {
    const auto character = static_cast<unsigned char>(raw);
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

[[nodiscard]] ScalarType parse_scalar(std::string_view name) {
  if (const auto type = parse_scalar_type(name)) {
    return *type;
  }
  throw ParseFailure{"unknown scalar type"};
}

[[nodiscard]] std::string_view domain_name(DepthDomain domain) {
  return depth_domain_name(domain);
}

[[nodiscard]] DepthDomain parse_domain(std::string_view name) {
  const auto parsed = parse_depth_domain(name);
  if (!parsed.has_value()) {
    throw ParseFailure{"unknown depth domain"};
  }
  return *parsed;
}

[[nodiscard]] std::string_view direction_name(AxisDirection direction) {
  return direction == AxisDirection::increasing ? "increasing" : "decreasing";
}

[[nodiscard]] AxisDirection parse_direction(std::string_view name) {
  if (name == "increasing")
    return AxisDirection::increasing;
  if (name == "decreasing")
    return AxisDirection::decreasing;
  throw ParseFailure{"unknown sampling-axis direction"};
}

// ImageSource pixel-format and CustomSymbolOccurrence symbol-kind name helpers
// (manifest-local; parallel to direction_name/domain_name). A later cleanup can
// lift these to core alongside depth_domain_name.
[[nodiscard]] std::string_view pixel_format_name(PixelFormat format) {
  switch (format) {
  case PixelFormat::rgba8: return "rgba8";
  case PixelFormat::rgb8:  return "rgb8";
  case PixelFormat::r8:    return "r8";
  }
  return "";
}

[[nodiscard]] PixelFormat parse_pixel_format(std::string_view name) {
  if (name == "rgba8") return PixelFormat::rgba8;
  if (name == "rgb8")  return PixelFormat::rgb8;
  if (name == "r8")    return PixelFormat::r8;
  throw ParseFailure{"unknown pixel format"};
}

[[nodiscard]] std::string_view symbol_kind_name(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::circle:      return "circle";
  case SymbolKind::square:      return "square";
  case SymbolKind::triangle_up: return "triangleUp";
  case SymbolKind::triangle_down: return "triangleDown";
  case SymbolKind::diamond:     return "diamond";
  case SymbolKind::cross:       return "cross";
  case SymbolKind::shoe:        return "shoe";
  }
  return "";
}

[[nodiscard]] SymbolKind parse_symbol_kind(std::string_view name) {
  if (name == "circle")      return SymbolKind::circle;
  if (name == "square")      return SymbolKind::square;
  if (name == "triangleUp")  return SymbolKind::triangle_up;
  if (name == "triangleDown") return SymbolKind::triangle_down;
  if (name == "diamond")     return SymbolKind::diamond;
  if (name == "cross")       return SymbolKind::cross;
  if (name == "shoe")        return SymbolKind::shoe;
  throw ParseFailure{"unknown symbol kind"};
}

void write_source(std::string &output, const BufferSourceReference &source) {
  output += "{\"uri\":";
  append_escaped(output, source.uri);
  output += ",\"checksum\":";
  append_escaped(output, source.checksum);
  output += ",\"byteOffset\":" + std::to_string(source.byte_offset) + '}';
}

void write_buffer(std::string &output, const BufferView &buffer) {
  output += "{\"source\":";
  write_source(output, buffer.source());
  output += ",\"length\":" + std::to_string(buffer.length());
  output += ",\"strideBytes\":" + std::to_string(buffer.stride_bytes());
  output += ",\"scalarType\":";
  append_escaped(output, scalar_type_name(buffer.scalar_type()));
  output += ",\"byteCapacity\":" + std::to_string(buffer.byte_capacity()) + '}';
}

void write_nulls(std::string &output, const NullBitmapView &nulls) {
  output += "{\"source\":";
  write_source(output, nulls.source());
  output += ",\"bitLength\":" + std::to_string(nulls.bit_length());
  output += ",\"byteCapacity\":" + std::to_string(nulls.byte_capacity()) + '}';
}

// --- ImageSource / CustomLayerSource serialization helpers (#183) -----------
// ADR 0042 untrusted-input limits, mirrored from src/scene/scene.cpp so the
// manifest layer rejects over-limit input before it reaches the scene. A later
// cleanup can share these between the two layers.
constexpr std::uint32_t manifest_maximum_image_dimension_px = 65536;
constexpr std::uint64_t manifest_maximum_image_pixels =
    512ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t manifest_minimum_image_dpi = 1;
constexpr std::size_t manifest_maximum_custom_primitives = 4096;
constexpr std::size_t manifest_maximum_custom_vertices = 1ULL << 20;
// Per-polyline / per-clip-path point ceiling (mirrors scene.cpp's
// maximum_custom_polyline_points); polylines also need ≥2 points, clips ≥3.
constexpr std::size_t manifest_maximum_custom_polyline_points = 8192;

void write_color(std::string &output, const RgbaColor &color) {
  output += "{\"r\":" + std::to_string(color.red);
  output += ",\"g\":" + std::to_string(color.green);
  output += ",\"b\":" + std::to_string(color.blue);
  output += ",\"a\":" + std::to_string(color.alpha) + '}';
}

[[nodiscard]] std::uint8_t color_component(const JsonObject &color,
                                         const char *name) {
  // A manifest is untrusted input; every other numeric domain in this file
  // rejects out-of-range values, but colours silently truncated through
  // uint8_t (g:300 became 44) — reject instead (issue #470).
  const auto raw = unsigned_integer(field(color, name));
  if (raw > 255U) {
    throw ParseFailure{"colour component out of range"};
  }
  return static_cast<std::uint8_t>(raw);
}

[[nodiscard]] RgbaColor parse_color(const JsonValue &value) {
  const auto &color = object(value);
  return RgbaColor{
      .red = color_component(color, "r"),
      .green = color_component(color, "g"),
      .blue = color_component(color, "b"),
      .alpha = color_component(color, "a"),
  };
}

void write_point(std::string &output, const PhysicalPoint &point) {
  output += "{\"left\":" + number_text(point.left.value);
  output += ",\"top\":" + number_text(point.top.value) + '}';
}

[[nodiscard]] PhysicalPoint parse_point(const JsonValue &value) {
  const auto &point = object(value);
  return PhysicalPoint{
      .left = Millimetres{number(field(point, "left"))},
      .top = Millimetres{number(field(point, "top"))},
  };
}

// Defined below; forward-declared so the ADR 0050 dashPattern/patternId
// readers in parse_primitive can use it (declaration order in this file
// otherwise puts its definition after the first use).
[[nodiscard]] const JsonValue *optional_field(const JsonObject &value,
                                              std::string_view name);

[[nodiscard]] CustomPrimitive parse_primitive(const JsonValue &value) {
  const auto &obj = object(value);
  const auto kind = string(field(obj, "kind"));
  if (kind == "polyline") {
    CustomPolyline p;
    p.closed = boolean(field(obj, "closed"));
    p.color = parse_color(field(obj, "color"));
    p.width = Millimetres{number(field(obj, "width"))};
    for (const auto &pt : array(field(obj, "points"))) {
      p.points.push_back(parse_point(pt));
    }
    if (const auto *dash = optional_field(obj, "dashPattern")) {
      const auto &dash_obj = object(*dash);
      if (const auto *segs = optional_field(dash_obj, "segments")) {
        for (const auto &seg : array(*segs)) {
          p.dash_pattern.segments.push_back(Millimetres{number(seg)});
        }
      }
      p.dash_pattern.offset =
          optional_field(dash_obj, "offset") != nullptr
              ? number(*optional_field(dash_obj, "offset"))
              : 0.0;
    }
    return p;
  }
  if (kind == "triangle") {
    return CustomTriangle{
        .a = parse_point(field(obj, "a")),
        .b = parse_point(field(obj, "b")),
        .c = parse_point(field(obj, "c")),
        .fill_color = parse_color(field(obj, "fillColor")),
    };
  }
  if (kind == "quad") {
    const auto &rect = object(field(obj, "rect"));
    CustomQuad q{
        .rect = PhysicalRect{
            .left = Millimetres{number(field(rect, "left"))},
            .top = Millimetres{number(field(rect, "top"))},
            .width = Millimetres{number(field(rect, "width"))},
            .height = Millimetres{number(field(rect, "height"))},
        },
        .fill_color = parse_color(field(obj, "fillColor")),
    };
    if (const auto *pid = optional_field(obj, "patternId")) {
      q.pattern_id = entity_id(*pid);
    }
    return q;
  }
  if (kind == "symbol") {
    return CustomSymbolOccurrence{
        .center = parse_point(field(obj, "center")),
        .kind = parse_symbol_kind(string(field(obj, "symbol"))),
        .color = parse_color(field(obj, "color")),
        .size = Millimetres{number(field(obj, "size"))},
    };
  }
  throw ParseFailure{"unknown custom primitive kind"};
}

void write_primitive(std::string &output, const CustomPrimitive &primitive) {
  std::visit(
      [&](const auto &p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, CustomPolyline>) {
          output += "{\"kind\":\"polyline\",\"closed\":";
          output += (p.closed ? "true" : "false");
          output += ",\"color\":";
          write_color(output, p.color);
          output += ",\"width\":" + number_text(p.width.value);
          output += ",\"points\":[";
          bool first = true;
          for (const auto &pt : p.points) {
            if (!first) output.push_back(',');
            first = false;
            write_point(output, pt);
          }
          output += "]";
          if (!p.dash_pattern.segments.empty()) {
            output += ",\"dashPattern\":{\"segments\":[";
            bool seg_first = true;
            for (const auto &seg : p.dash_pattern.segments) {
              if (!seg_first) output.push_back(',');
              seg_first = false;
              output += number_text(seg.value);
            }
            output += "],\"offset\":";
            output += number_text(p.dash_pattern.offset);
            output += "}";
          }
          output += "}";
        } else if constexpr (std::is_same_v<T, CustomTriangle>) {
          output += "{\"kind\":\"triangle\",\"a\":";
          write_point(output, p.a);
          output += ",\"b\":";
          write_point(output, p.b);
          output += ",\"c\":";
          write_point(output, p.c);
          output += ",\"fillColor\":";
          write_color(output, p.fill_color);
          output += "}";
        } else if constexpr (std::is_same_v<T, CustomQuad>) {
          output += "{\"kind\":\"quad\",\"rect\":{\"left\":";
          output += number_text(p.rect.left.value);
          output += ",\"top\":" + number_text(p.rect.top.value);
          output += ",\"width\":" + number_text(p.rect.width.value);
          output += ",\"height\":" + number_text(p.rect.height.value);
          output += "},\"fillColor\":";
          write_color(output, p.fill_color);
          if (!p.pattern_id.is_nil()) {
            output += ",\"patternId\":\"";
            output += p.pattern_id.to_string();
            output += "\"";
          }
          output += "}";
        } else if constexpr (std::is_same_v<T, CustomSymbolOccurrence>) {
          output += "{\"kind\":\"symbol\",\"center\":";
          write_point(output, p.center);
          output += ",\"symbol\":";
          append_escaped(output, symbol_kind_name(p.kind));
          output += ",\"color\":";
          write_color(output, p.color);
          output += ",\"size\":" + number_text(p.size.value);
          output += "}";
        }
      },
      primitive);
}

// Counts the tessellated vertices a custom primitive contributes to the scene
// (ADR 0042 total-vertex limit), MIRRORING src/scene/scene.cpp's counting so the
// manifest gate rejects exactly what the scene would: polyline = points.size(),
// triangle = 3, quad = 6 (two triangles), symbol = 24 (built-in symbol tessellation).
// (The manifest's geometric count previously diverged from the scene's, letting
// over-limit input through the manifest gate.)
[[nodiscard]] std::size_t primitive_vertex_count(const CustomPrimitive &p) {
  if (const auto *poly = std::get_if<CustomPolyline>(&p)) {
    return poly->points.size();
  }
  if (std::holds_alternative<CustomTriangle>(p)) {
    return 3;
  }
  if (std::holds_alternative<CustomQuad>(p)) {
    return 6;
  }
  return 24; // CustomSymbolOccurrence
}

// Optional-field accessor: returns nullptr when the key is absent (unlike
// field(), which throws). Lets a v2 reader tolerate v1 manifests missing the
// new imageSources/customSources keys.
[[nodiscard]] const JsonValue *optional_field(const JsonObject &value,
                                              std::string_view name) {
  const auto found = value.find(name);
  return found == value.end() ? nullptr : &found->second;
}

[[nodiscard]] BufferSourceReference parse_source(const JsonValue &value) {
  const auto &source = object(value);
  auto result = BufferSourceReference{
      .uri = string(field(source, "uri")),
      .checksum = string(field(source, "checksum")),
      .byte_offset = unsigned_integer(field(source, "byteOffset")),
  };
  if (result.uri.empty()) {
    throw ParseFailure{"buffer source URI must not be empty"};
  }
  if (!is_safe_untrusted_asset_uri(result.uri)) {
    throw ParseFailure{"buffer source URI scheme or content is not allowed"};
  }
  return result;
}

[[nodiscard]] BufferDescriptor parse_buffer(const JsonValue &value) {
  const auto &buffer = object(value);
  auto result = BufferDescriptor{
      .source = parse_source(field(buffer, "source")),
      .length = unsigned_integer(field(buffer, "length")),
      .stride_bytes = unsigned_integer(field(buffer, "strideBytes")),
      .scalar_type = parse_scalar(string(field(buffer, "scalarType"))),
      .byte_capacity = unsigned_integer(field(buffer, "byteCapacity")),
  };
  if (result.length == 0 || result.stride_bytes == 0 ||
      result.byte_capacity == 0) {
    throw ParseFailure{"buffer dimensions must be positive"};
  }
  const auto element = scalar_size_bytes(result.scalar_type);
  if (const auto err = validate_buffer_extent(
          result.length, result.stride_bytes, element, result.byte_capacity);
      err.has_value()) {
    // Map to parse failure without echoing raw dimensions into diagnostics.
    throw ParseFailure{"buffer extent invalid or exceeds capacity"};
  }
  return result;
}

[[nodiscard]] NullBitmapDescriptor parse_nulls(const JsonValue &value) {
  const auto &nulls = object(value);
  auto result = NullBitmapDescriptor{
      .source = parse_source(field(nulls, "source")),
      .bit_length = unsigned_integer(field(nulls, "bitLength")),
      .byte_capacity = unsigned_integer(field(nulls, "byteCapacity")),
  };
  if (result.bit_length == 0 || result.byte_capacity == 0) {
    throw ParseFailure{"null bitmap dimensions must be positive"};
  }
  return result;
}

void validate_source_schema(const JsonValue &value) {
  require_exact_fields(object(value), {"uri", "checksum", "byteOffset"});
}

void validate_buffer_schema(const JsonValue &value) {
  const auto &buffer = object(value);
  require_exact_fields(buffer, {"source", "length", "strideBytes", "scalarType",
                                "byteCapacity"});
  validate_source_schema(field(buffer, "source"));
}

void validate_manifest_schema(const JsonObject &root) {
  require_exact_fields(root,
                       {"schemaVersion", "requiredSdkVersion", "document"});
  const auto &document = object(field(root, "document"));
  // The document object must carry the 4 mandatory keys, and MAY carry the
  // optional imageSources/customSources keys (#183 schema v2). A v1 manifest
  // omits them; a v2 reader tolerates both. Unknown keys are still rejected.
  for (const auto &mandatory :
       {"id", "revision", "samplingAxes", "curves"}) {
    (void)field(document, mandatory); // throws on miss
  }
  for (const auto &[key, value] : document) {
    if (key != "id" && key != "revision" && key != "samplingAxes" &&
        key != "curves" && key != "imageSources" && key != "customSources") {
      throw ParseFailure{"unknown document field: " + key};
    }
    static_cast<void>(value);
  }
  // The per-image / per-custom-source field schemas are validated in the read
  // path (parse_image_source / parse_custom_source), not here.

  const auto &axes = array(field(document, "samplingAxes"));
  const auto &curves = array(field(document, "curves"));
  if (axes.empty() || curves.empty()) {
    throw ParseFailure{"manifest requires axes and curves"};
  }
  for (const auto &axis_value : axes) {
    const auto &axis = object(axis_value);
    // Mandatory keys + optional nominalInterval (multi-rate metadata, v2
    // addition). Mirrors the document-level optional-key tolerance: v1/v2
    // manifests without the key parse as irregular/unknown sampling.
    for (const auto &mandatory :
         {"id", "domain", "unit", "direction", "coordinates"}) {
      (void)field(axis, mandatory); // throws on miss
    }
    for (const auto &[key, value] : axis) {
      if (key != "id" && key != "domain" && key != "unit" &&
          key != "direction" && key != "coordinates" &&
          key != "nominalInterval") {
        throw ParseFailure{"unknown axis field: " + key};
      }
      if (key == "nominalInterval" &&
          !std::holds_alternative<JsonNumber>(value.value)) {
        throw ParseFailure{"nominalInterval must be a number"};
      }
    }
    validate_buffer_schema(field(axis, "coordinates"));
  }
  for (const auto &curve_value : curves) {
    const auto &curve = object(curve_value);
    require_exact_fields(curve, {"id", "mnemonic", "displayName", "unit",
                                 "samplingAxisId", "values", "nulls"});
    validate_buffer_schema(field(curve, "values"));
    const auto &nulls = field(curve, "nulls");
    if (!std::holds_alternative<std::nullptr_t>(nulls.value)) {
      const auto &null_object = object(nulls);
      require_exact_fields(null_object,
                           {"source", "bitLength", "byteCapacity"});
      validate_source_schema(field(null_object, "source"));
    }
  }
}

[[nodiscard]] bool source_matches(const BufferSourceReference &actual,
                                  const BufferSourceReference &expected) {
  return actual.uri == expected.uri && actual.checksum == expected.checksum &&
         actual.byte_offset == expected.byte_offset;
}

[[nodiscard]] bool
can_write_manifest_buffer(const BufferView &buffer) noexcept {
  return !buffer.source().uri.empty() && buffer.length() > 0 &&
         buffer.stride_bytes() > 0 && buffer.byte_capacity() > 0;
}

// A curve's value buffer (#197): the manifest serializes a single
// BufferSourceReference per curve today (#183 round-trip). A composite
// (multi-block, append) curve has one source per segment — serializing that
// is the job of a later ticket (per-segment source identity). Until then, the
// manifest rejects composite-carrying curves (can_write_manifest_document
// returns false), a clean gate. Single-block curves serialize as before.
[[nodiscard]] bool
can_write_manifest_buffer(const CurveBuffer &buffer) noexcept {
  return !buffer.is_composite() &&
         can_write_manifest_buffer(buffer.as_single());
}

[[nodiscard]] bool
can_write_manifest_nulls(const NullBitmapView &nulls) noexcept {
  return nulls.empty() || (!nulls.source().uri.empty() &&
                           nulls.bit_length() > 0 && nulls.byte_capacity() > 0);
}

[[nodiscard]] bool
can_write_manifest_document(const WellLogDocument &document) noexcept {
  if (document.id().is_nil() || document.revision().value == 0 ||
      document.sampling_axes().empty() || document.curves().empty()) {
    return false;
  }
  for (const auto &axis : document.sampling_axes()) {
    if (!can_write_manifest_buffer(axis.coordinates)) {
      return false;
    }
  }
  for (const auto &curve : document.curves()) {
    if (!can_write_manifest_buffer(curve.values) ||
        !can_write_manifest_nulls(curve.nulls)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Error
manifest_error(MessageKey message = MessageKey::manifest_invalid) {
  return Error{
      .code = ErrorCode::invalid_manifest,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

// Overload carrying a distinct ErrorCode (ADR 0042 image/custom-source limits
// use invalid_image / invalid_custom_source, not invalid_manifest).
[[nodiscard]] Error manifest_error(MessageKey message, ErrorCode code) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

[[nodiscard]] Error boundary_error(ErrorCode code, MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

} // namespace

struct ManifestText::Impl {
  std::string value;
};

ManifestText::ManifestText() = default;
ManifestText::~ManifestText() = default;
ManifestText::ManifestText(const ManifestText &) = default;
ManifestText &ManifestText::operator=(const ManifestText &) = default;
ManifestText::ManifestText(ManifestText &&) noexcept = default;
ManifestText &ManifestText::operator=(ManifestText &&) noexcept = default;

ManifestText::ManifestText(std::string text)
    : impl_(std::make_shared<Impl>(Impl{.value = std::move(text)})) {}

std::string_view ManifestText::text() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->value};
}

Result<ManifestText> ManifestCodec::write(const WellLogDocument &document) {
  try {
    if (!can_write_manifest_document(document)) {
      return manifest_error();
    }

    std::string output;
    output.reserve(1024);
    output += "{\"schemaVersion\":";
    output += std::to_string(manifest_schema_version);
    output += ",\"requiredSdkVersion\":";
    append_escaped(output, manifest_sdk_requirement);
    output += ",\"document\":{\"id\":";
    append_escaped(output, document.id().to_string());
    output += ",\"revision\":" + std::to_string(document.revision().value);
    output += ",\"samplingAxes\":[";
    bool first = true;
    for (const auto &axis : document.sampling_axes()) {
      if (axis.coordinates.is_composite() ||
          axis.coordinates.as_single().source().uri.empty()) {
        return manifest_error();
      }
      if (!first)
        output.push_back(',');
      first = false;
      output += "{\"id\":";
      append_escaped(output, axis.id.to_string());
      output += ",\"domain\":";
      append_escaped(output, domain_name(axis.domain));
      output += ",\"unit\":";
      append_escaped(output, axis.unit);
      output += ",\"direction\":";
      append_escaped(output, direction_name(axis.direction));
      if (axis.nominal_interval.has_value()) {
        output += ",\"nominalInterval\":";
        output += number_text(*axis.nominal_interval);
      }
      output += ",\"coordinates\":";
      write_buffer(output, axis.coordinates.as_single());
      output.push_back('}');
    }
    output += "],\"curves\":[";
    first = true;
    for (const auto &curve : document.curves()) {
      if (curve.values.is_composite() ||
          curve.values.as_single().source().uri.empty() ||
          (!curve.nulls.empty() && curve.nulls.source().uri.empty())) {
        return manifest_error();
      }
      if (!first)
        output.push_back(',');
      first = false;
      output += "{\"id\":";
      append_escaped(output, curve.id.to_string());
      output += ",\"mnemonic\":";
      append_escaped(output, curve.mnemonic);
      output += ",\"displayName\":";
      append_escaped(output, curve.display_name);
      output += ",\"unit\":";
      append_escaped(output, curve.unit);
      output += ",\"samplingAxisId\":";
      append_escaped(output, curve.sampling_axis_id.to_string());
      output += ",\"values\":";
      // can_write_manifest_document gates out composite-carrying curves, so a
      // curve reaching here is single-block.
      write_buffer(output, curve.values.as_single());
      output += ",\"nulls\":";
      if (curve.nulls.empty()) {
        output += "null";
      } else {
        write_nulls(output, curve.nulls);
      }
      output.push_back('}');
    }
    output += "]";
    // Optional imageSources (#183). Emitted only when present so the common
    // no-image document is unchanged.
    if (!document.image_sources().empty()) {
      output += ",\"imageSources\":[";
      bool img_first = true;
      for (const auto &image : document.image_sources()) {
        if (image.source.uri.empty()) {
          return manifest_error();
        }
        if (!img_first) output.push_back(',');
        img_first = false;
        output += "{\"id\":";
        append_escaped(output, image.id.to_string());
        output += ",\"widthPx\":" + std::to_string(image.width_px);
        output += ",\"heightPx\":" + std::to_string(image.height_px);
        output += ",\"pixelFormat\":";
        append_escaped(output, pixel_format_name(image.pixel_format));
        output += ",\"referenceDepthTop\":" +
                  number_text(image.reference_depth_top);
        output += ",\"referenceDepthBottom\":" +
                  number_text(image.reference_depth_bottom);
        output += ",\"dpi\":" + std::to_string(image.dpi);
        output += ",\"source\":";
        write_source(output, image.source);
        output += "}";
      }
      output += "]";
    }
    // Optional customSources (#183).
    if (!document.custom_sources().empty()) {
      output += ",\"customSources\":[";
      bool custom_first = true;
      for (const auto &custom : document.custom_sources()) {
        if (!custom_first) output.push_back(',');
        custom_first = false;
        output += "{\"id\":";
        append_escaped(output, custom.id.to_string());
        output += ",\"contentRevision\":" +
                  std::to_string(custom.content_revision.value);
        output += ",\"primitives\":[";
        bool prim_first = true;
        for (const auto &primitive : custom.primitives) {
          if (!prim_first) output.push_back(',');
          prim_first = false;
          write_primitive(output, primitive);
        }
        output += "]";
        if (custom.clip.has_value()) {
          output += ",\"clip\":{\"points\":[";
          bool clip_first = true;
          for (const auto &pt : custom.clip->points) {
            if (!clip_first) output.push_back(',');
            clip_first = false;
            write_point(output, pt);
          }
          output += "]}";
        }
        output += "}";
      }
      output += "]";
    }
    output += "}}";
    return ManifestText{std::move(output)};
  } catch (const std::bad_alloc &) {
    return boundary_error(ErrorCode::resource_exhausted,
                          MessageKey::resource_exhausted);
  } catch (...) {
    return boundary_error(ErrorCode::internal_error,
                          MessageKey::internal_error);
  }
}

Result<WellLogDocument>
ManifestCodec::read(std::string_view manifest,
                    const ManifestResolvers &resolvers) {
  try {
    const auto root = JsonParser{manifest}.parse();
    const auto &root_object = object(root);
    validate_manifest_schema(root_object);
    // Schema version: a v2 reader accepts BOTH v1 and v2. v2 only ADDED two
    // optional keys (imageSources/customSources); v1 manifests omit them and
    // the optional-field handling reconstructs an empty set, so reading a v1
    // manifest is safe and loss-free (the document had no images/custom sources
    // to lose). A future incompatible change would narrow this set. This makes
    // the optional-key tolerance genuine forward-compat (not dead code).
    const auto schema_version =
        unsigned_integer(field(root_object, "schemaVersion"));
    if (schema_version != 1 && schema_version != manifest_schema_version) {
      return manifest_error(MessageKey::manifest_schema_unsupported);
    }
    if (string(field(root_object, "requiredSdkVersion")) !=
        manifest_sdk_requirement) {
      return manifest_error(MessageKey::manifest_schema_unsupported);
    }
    if (!resolvers.buffer) {
      return manifest_error(MessageKey::manifest_resolver_required);
    }

    const auto &document = object(field(root_object, "document"));
    const auto revision = unsigned_integer(field(document, "revision"));
    if (revision == 0) {
      return manifest_error();
    }
    WellLogDocumentBuilder builder(entity_id(field(document, "id")),
                                   DocumentRevision{revision});

    for (const auto &axis_value : array(field(document, "samplingAxes"))) {
      const auto &axis = object(axis_value);
      const auto descriptor = parse_buffer(field(axis, "coordinates"));
      auto resolved = resolvers.buffer(descriptor);
      if (!resolved) {
        return resolved.error();
      }
      const auto &view = resolved.value();
      if (!source_matches(view.source(), descriptor.source) ||
          view.length() != descriptor.length ||
          view.stride_bytes() != descriptor.stride_bytes ||
          view.scalar_type() != descriptor.scalar_type ||
          view.byte_capacity() < descriptor.byte_capacity) {
        return manifest_error(MessageKey::manifest_buffer_mismatch);
      }
      builder.add_sampling_axis(SamplingAxis{
          .id = entity_id(field(axis, "id")),
          .coordinates = view,
          .domain = parse_domain(string(field(axis, "domain"))),
          .unit = string(field(axis, "unit")),
          .direction = parse_direction(string(field(axis, "direction"))),
          .nominal_interval = parse_optional_interval(axis),
      });
    }

    for (const auto &curve_value : array(field(document, "curves"))) {
      const auto &curve = object(curve_value);
      const auto descriptor = parse_buffer(field(curve, "values"));
      auto resolved = resolvers.buffer(descriptor);
      if (!resolved) {
        return resolved.error();
      }
      const auto &view = resolved.value();
      if (!source_matches(view.source(), descriptor.source) ||
          view.length() != descriptor.length ||
          view.stride_bytes() != descriptor.stride_bytes ||
          view.scalar_type() != descriptor.scalar_type ||
          view.byte_capacity() < descriptor.byte_capacity) {
        return manifest_error(MessageKey::manifest_buffer_mismatch);
      }

      NullBitmapView nulls;
      const auto &null_value = field(curve, "nulls");
      if (!std::holds_alternative<std::nullptr_t>(null_value.value)) {
        if (!resolvers.null_bitmap) {
          return manifest_error(MessageKey::manifest_resolver_required);
        }
        const auto null_descriptor = parse_nulls(null_value);
        auto resolved_nulls = resolvers.null_bitmap(null_descriptor);
        if (!resolved_nulls) {
          return resolved_nulls.error();
        }
        nulls = resolved_nulls.value();
        if (!source_matches(nulls.source(), null_descriptor.source) ||
            nulls.bit_length() != null_descriptor.bit_length ||
            nulls.byte_capacity() < null_descriptor.byte_capacity) {
          return manifest_error(MessageKey::manifest_buffer_mismatch);
        }
      }

      builder.add_curve(Curve{
          .id = entity_id(field(curve, "id")),
          .mnemonic = string(field(curve, "mnemonic")),
          .display_name = string(field(curve, "displayName")),
          .unit = string(field(curve, "unit")),
          .sampling_axis_id = entity_id(field(curve, "samplingAxisId")),
          .values = view,
          .nulls = std::move(nulls),
      });
    }
    // Optional imageSources (#183, schema v2). Absent on v1 manifests.
    if (const auto *images_value = optional_field(document, "imageSources")) {
      for (const auto &image_value : array(*images_value)) {
        const auto &image = object(image_value);
        require_exact_fields(image, {"id", "widthPx", "heightPx", "pixelFormat",
                                     "referenceDepthTop",
                                     "referenceDepthBottom", "dpi", "source"});
        const auto width_px =
            static_cast<std::uint32_t>(unsigned_integer(field(image, "widthPx")));
        const auto height_px = static_cast<std::uint32_t>(
            unsigned_integer(field(image, "heightPx")));
        const auto dpi =
            static_cast<std::uint32_t>(unsigned_integer(field(image, "dpi")));
        const auto depth_top = number(field(image, "referenceDepthTop"));
        const auto depth_bottom = number(field(image, "referenceDepthBottom"));
        // ADR 0042 untrusted-input limits.
        if (width_px == 0 || height_px == 0 ||
            width_px > manifest_maximum_image_dimension_px ||
            height_px > manifest_maximum_image_dimension_px) {
          return manifest_error(MessageKey::image_dimension_exceeds_limit,
                                ErrorCode::invalid_image);
        }
        const auto pixels = static_cast<std::uint64_t>(width_px) *
                            static_cast<std::uint64_t>(height_px);
        if (pixels > manifest_maximum_image_pixels) {
          return manifest_error(MessageKey::image_pixels_exceed_limit,
                                ErrorCode::invalid_image);
        }
        if (dpi < manifest_minimum_image_dpi || !std::isfinite(depth_top) ||
            !std::isfinite(depth_bottom)) {
          return manifest_error(MessageKey::image_metadata_invalid,
                                ErrorCode::invalid_image);
        }
        builder.add_image_source(ImageSource{
            .id = entity_id(field(image, "id")),
            .width_px = width_px,
            .height_px = height_px,
            .pixel_format =
                parse_pixel_format(string(field(image, "pixelFormat"))),
            .reference_depth_top = depth_top,
            .reference_depth_bottom = depth_bottom,
            .dpi = dpi,
            .source = parse_source(field(image, "source")),
        });
      }
    }
    // Optional customSources (#183, schema v2).
    if (const auto *customs_value = optional_field(document, "customSources")) {
      for (const auto &custom_value : array(*customs_value)) {
        const auto &custom = object(custom_value);
        // clip is optional; the rest are mandatory.
        for (const auto &mandatory :
             {"id", "contentRevision", "primitives"}) {
          (void)field(custom, mandatory);
        }
        const auto primitives_array = array(field(custom, "primitives"));
        // ADR 0042: non-empty + bounded primitive/vertex counts.
        if (primitives_array.empty()) {
          return manifest_error(MessageKey::custom_source_empty,
                                ErrorCode::invalid_custom_source);
        }
        if (primitives_array.size() > manifest_maximum_custom_primitives) {
          return manifest_error(MessageKey::custom_source_primitives_exceed_limit,
                                ErrorCode::invalid_custom_source);
        }
        std::vector<CustomPrimitive> primitives;
        primitives.reserve(primitives_array.size());
        std::size_t total_vertices = 0;
        for (const auto &primitive_value : primitives_array) {
          auto primitive = parse_primitive(primitive_value);
          // ADR 0042 per-polyline point cap + minimum (mirrors scene.cpp): a
          // polyline needs ≥2 points and ≤8192.
          if (const auto *poly = std::get_if<CustomPolyline>(&primitive)) {
            if (poly->points.size() < 2 ||
                poly->points.size() >
                    manifest_maximum_custom_polyline_points) {
              return manifest_error(
                  MessageKey::custom_source_points_exceed_limit,
                  ErrorCode::invalid_custom_source);
            }
          }
          total_vertices += primitive_vertex_count(primitive);
          primitives.push_back(std::move(primitive));
        }
        if (total_vertices > manifest_maximum_custom_vertices) {
          return manifest_error(MessageKey::custom_source_points_exceed_limit,
                                ErrorCode::invalid_custom_source);
        }
        std::optional<CustomClipPath> clip;
        if (const auto *clip_value = optional_field(custom, "clip")) {
          const auto &clip_obj = object(*clip_value);
          require_exact_fields(clip_obj, {"points"});
          CustomClipPath clip_path;
          for (const auto &pt : array(field(clip_obj, "points"))) {
            clip_path.points.push_back(parse_point(pt));
          }
          // ADR 0042 clip-path point cap + minimum (mirrors scene.cpp): a clip
          // needs ≥3 points and ≤8192.
          if (clip_path.points.size() < 3 ||
              clip_path.points.size() >
                  manifest_maximum_custom_polyline_points) {
            return manifest_error(MessageKey::custom_source_points_exceed_limit,
                                  ErrorCode::invalid_custom_source);
          }
          clip = std::move(clip_path);
        }
        builder.add_custom_source(CustomLayerSource{
            .id = entity_id(field(custom, "id")),
            .content_revision = DocumentRevision{
                unsigned_integer(field(custom, "contentRevision"))},
            .primitives = std::move(primitives),
            .clip = std::move(clip),
        });
      }
    }
    auto result = builder.build();
    if (result.id().is_nil()) {
      return boundary_error(ErrorCode::resource_exhausted,
                            MessageKey::resource_exhausted);
    }
    return result;
  } catch (const std::bad_alloc &) {
    return boundary_error(ErrorCode::resource_exhausted,
                          MessageKey::resource_exhausted);
  } catch (const std::exception &) {
    return manifest_error();
  } catch (...) {
    return manifest_error();
  }
}

} // namespace welllog
