#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <welllog/core/result.hpp>
#include <welllog/scene/export.hpp>

namespace welllog {

// Base direction requested for a shaping pass. Vertical typesetting asks
// the engine to classify every glyph with Unicode UAX #50 so that CJK
// stays upright while rotated scripts turn 90 degrees.
enum class TextDirection : std::uint8_t {
  left_to_right,
  right_to_left,
  top_to_bottom,
};

struct TextShapeRequest {
  std::string_view text;
  std::string_view language;
  TextDirection direction{TextDirection::left_to_right};
};

// One shaped glyph. Advances and offsets are expressed in em fractions
// (1.0 == the requested font size) so the scene can position runs in
// physical units without knowing font internals.
struct ShapedGlyph {
  std::uint32_t glyph_id{};
  std::uint32_t font_index{};
  std::uint32_t cluster{};
  char32_t code_point{};
  double advance_x{};
  double advance_y{};
  double offset_x{};
  double offset_y{};
  bool upright{true};
};

struct ShapedRun {
  std::vector<ShapedGlyph> glyphs;
  std::vector<char32_t> missing_code_points;
  bool used_fallback_font{};
  double ascender{};
  double descender{};
};

enum class OutlineVerb : std::uint8_t {
  move_to,
  line_to,
  quadratic_to,
  cubic_to,
  close,
};

struct OutlineCommand {
  OutlineVerb verb{OutlineVerb::move_to};
  // Em fractions in a y-up, glyph-local coordinate system. Only the
  // leading entries required by the verb are meaningful.
  double coordinates[6]{};
};

struct GlyphOutline {
  std::vector<OutlineCommand> commands;
  double advance_x{};
  double left{};
  double bottom{};
  double right{};
  double top{};
};

// Platform-neutral text shaping boundary (ADR 0029). Implementations
// resolve fonts in project, built-in fallback, then system order, shape
// UTF-8 text and return glyph outlines; the scene and both rendering
// backends consume only this interface. Implementations must be safe to
// call from any single preparation thread at a time.
class WELLLOG_SCENE_API TextEngine {
public:
  TextEngine() = default;
  virtual ~TextEngine() = default;
  TextEngine(const TextEngine &) = delete;
  TextEngine &operator=(const TextEngine &) = delete;

protected:
  TextEngine(TextEngine &&) = default;
  TextEngine &operator=(TextEngine &&) = default;

public:

  [[nodiscard]] virtual Result<ShapedRun>
  shape(const TextShapeRequest &request) noexcept = 0;
  [[nodiscard]] virtual Result<GlyphOutline>
  glyph_outline(std::uint32_t font_index, std::uint32_t glyph_id) noexcept = 0;
  [[nodiscard]] virtual std::string
  font_fingerprint(std::uint32_t font_index) const = 0;
  [[nodiscard]] virtual std::string
  font_family_name(std::uint32_t font_index) const = 0;
};

} // namespace welllog
