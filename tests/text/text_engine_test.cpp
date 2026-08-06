#include <welllog/text/harfbuzz_text_engine.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

#ifndef WELLLOG_TEST_FONT_DIR
#define WELLLOG_TEST_FONT_DIR "tests/assets/fonts"
#endif

const std::string test_font_path =
    std::string{WELLLOG_TEST_FONT_DIR} + "/NotoSans-Regular.ttf";
const std::string test_cjk_font_path =
    std::string{WELLLOG_TEST_FONT_DIR} + "/SourceHanSansCN-subset.otf";

void register_test_fonts(HarfBuzzTextEngine &engine) {
  require(engine.add_project_font(test_font_path).has_value(),
          "the bundled Latin test font must load");
  require(engine.add_project_font(test_cjk_font_path).has_value(),
          "the bundled CJK subset font must load");
}

[[nodiscard]] bool code_point_missing(const ShapedRun &run, char32_t cp) {
  for (const auto missing : run.missing_code_points) {
    if (missing == cp) {
      return true;
    }
  }
  return false;
}

void project_fonts_resolve_first_and_record_fingerprints() {
  HarfBuzzTextEngine engine;
  const auto font = engine.add_project_font(test_font_path);
  require(font.has_value(), "the bundled test font must load");
  require(font.value() == 0, "the first project font must take index zero");

  const auto fingerprint = engine.font_fingerprint(font.value());
  require(fingerprint.size() == 18,
          "fingerprint must be a 16-digit hash plus face index");
  require(engine.font_family_name(font.value()).find("Noto") !=
              std::string::npos,
          "family name must come from the font tables");

  HarfBuzzTextEngine second_engine;
  require(second_engine.add_project_font(test_font_path).has_value(),
          "reloading the font must succeed");
  require(second_engine.font_fingerprint(0) == fingerprint,
          "fingerprints must be stable across engine instances");
}

void latin_text_shapes_with_the_project_font() {
  HarfBuzzTextEngine engine;
  require(engine.add_project_font(test_font_path).has_value(),
          "project font must load");
  const auto run = engine.shape(TextShapeRequest{
      .text = "Gas 0.5 m3",
      .language = "en",
      .direction = TextDirection::left_to_right,
  });
  require(run.has_value(), "Latin shaping must succeed");
  require(run.value().glyphs.size() == 10,
          "every character must produce one glyph");
  require(run.value().missing_code_points.empty(),
          "Latin text must be fully covered");
  require(!run.value().used_fallback_font,
          "single-font text must not report fallback");
  double advance_sum = 0.0;
  for (const auto &glyph : run.value().glyphs) {
    require(glyph.font_index == 0, "glyphs must use the project font");
    require(glyph.glyph_id != 0, "glyphs must not be .notdef");
    advance_sum += glyph.advance_x;
  }
  require(advance_sum > 3.0 && advance_sum < 12.0,
          "advance sum must stay within plausible em bounds");
  require(run.value().ascender > 0.5, "ascender must be a positive em value");
  require(run.value().descender < 0.0, "descender must be negative");
}

void glyph_outlines_come_from_the_same_font() {
  HarfBuzzTextEngine engine;
  require(engine.add_project_font(test_font_path).has_value(),
          "project font must load");
  const auto run = engine.shape(TextShapeRequest{
      .text = "A",
      .language = "en",
      .direction = TextDirection::left_to_right,
  });
  require(run.has_value() && run.value().glyphs.size() == 1,
          "single-letter shaping must succeed");
  const auto &glyph = run.value().glyphs.front();
  const auto outline =
      engine.glyph_outline(glyph.font_index, glyph.glyph_id);
  require(outline.has_value(), "outline extraction must succeed");
  require(!outline.value().commands.empty(),
          "an uppercase letter must have outline commands");
  require(outline.value().advance_x > 0.3 && outline.value().advance_x < 1.2,
          "outline advance must be a plausible em fraction");
  require(outline.value().top > 0.5,
          "uppercase outline must reach cap height");
  require(std::abs(outline.value().advance_x - glyph.advance_x) < 1.0e-9,
          "shaping and outline advances must agree");
}

void missing_glyphs_are_explicit_and_diagnosed() {
  HarfBuzzTextEngine engine;
  require(engine.add_project_font(test_font_path).has_value(),
          "project font must load");
  // U+10FFFE is a noncharacter: no font covers it.
  const auto run = engine.shape(TextShapeRequest{
      .text = "A\xF4\x8F\xBF\xBE",
      .language = "en",
      .direction = TextDirection::left_to_right,
  });
  require(run.has_value(), "shaping with an unassigned code point must succeed");
  require(run.value().glyphs.size() == 2,
          "missing code points must still produce a replacement glyph");
  require(code_point_missing(run.value(), 0x10FFFE),
          "the missing code point must be reported");
  require(run.value().used_fallback_font,
          "replacement glyphs must mark the run as fallback");
  const auto &replacement = run.value().glyphs.back();
  require(replacement.code_point == 0x10FFFE,
          "replacement glyph must keep the source code point");
  const auto outline =
      engine.glyph_outline(replacement.font_index, replacement.glyph_id);
  require(outline.has_value() && !outline.value().commands.empty(),
          "the replacement glyph must have an explicit outline");
}

void built_in_fallback_covers_ascii_without_font_files() {
  HarfBuzzTextEngine engine;
  engine.add_system_font_directory("/nonexistent-welllog-fonts");
  const auto run = engine.shape(TextShapeRequest{
      .text = "API",
      .language = "en",
      .direction = TextDirection::left_to_right,
  });
  require(run.has_value() && run.value().glyphs.size() == 3,
          "built-in fallback must shape ASCII");
  const auto fingerprint =
      engine.font_fingerprint(run.value().glyphs.front().font_index);
  require(fingerprint == "builtin:font5x7:v1",
          "built-in fallback must report its own fingerprint");
  const auto outline = engine.glyph_outline(
      run.value().glyphs.front().font_index, run.value().glyphs.front().glyph_id);
  require(outline.has_value() && !outline.value().commands.empty(),
          "built-in glyphs must have deterministic outlines");
  require(run.value().ascender > 0.5 && run.value().descender < 0.0,
          "built-in metrics must be stable");
}

void vertical_typesetting_uses_uax50_orientation() {
  HarfBuzzTextEngine engine;
  register_test_fonts(engine);
  const auto run = engine.shape(TextShapeRequest{
      .text = "\xE7\xA0\x82" "A", // 砂 A
      .language = "zh-Hans",
      .direction = TextDirection::top_to_bottom,
  });
  require(run.has_value() && run.value().glyphs.size() == 2,
          "vertical shaping must produce two glyphs");
  require(run.value().missing_code_points.empty(),
          "system CJK fonts must cover the text");
  const auto &cjk = run.value().glyphs.front();
  const auto &latin = run.value().glyphs.back();
  require(cjk.code_point == 0x7802, "first glyph must be the CJK character");
  require(cjk.upright, "CJK must stay upright in vertical typesetting");
  require(!latin.upright, "Latin must rotate in vertical typesetting");
  require(cjk.advance_y < 0.0, "upright glyphs must advance downward");
  require(latin.advance_y < 0.0, "rotated glyphs must advance downward");
}

void rotated_cjk_shaping_uses_harfbuzz_clusters() {
  HarfBuzzTextEngine engine;
  register_test_fonts(engine);
  const auto run = engine.shape(TextShapeRequest{
      .text = "\xE7\xA0\x82\xE5\xB2\xA9 Sand", // 砂岩 Sand
      .language = "zh-Hans",
      .direction = TextDirection::left_to_right,
  });
  require(run.has_value(), "mixed CJK/Latin shaping must succeed");
  require(run.value().missing_code_points.empty(),
          "mixed text must be fully covered by the fallback chain");
  require(run.value().glyphs.size() == 7,
          "each character must produce one glyph");
  require(run.value().used_fallback_font,
          "mixing project and system fonts must report fallback");
  const auto cjk_font = run.value().glyphs.front().font_index;
  require(cjk_font != 0, "CJK glyphs must not use the Latin-only project font");
  require(engine.font_fingerprint(cjk_font) != "builtin:font5x7:v1",
          "CJK glyphs must come from a real font, not the dot-matrix fallback");
  double pen = 0.0;
  for (const auto &glyph : run.value().glyphs) {
    require(glyph.advance_x > 0.0, "horizontal advances must be positive");
    pen += glyph.advance_x;
  }
  require(pen > 4.0, "mixed text must accumulate plausible advances");
}

void invalid_input_is_rejected() {
  HarfBuzzTextEngine engine;
  const auto bad_text = engine.shape(TextShapeRequest{
      .text = "bad \xC0\x80 text",
      .language = "en",
      .direction = TextDirection::left_to_right,
  });
  require(!bad_text.has_value(), "invalid UTF-8 must be rejected");
  require(bad_text.error().message == MessageKey::text_encoding_invalid,
          "invalid text must use the encoding message key");

  const auto missing_file =
      engine.add_project_font("/nonexistent-welllog-font.ttf");
  require(!missing_file.has_value(), "missing font files must be rejected");
  require(missing_file.error().code == ErrorCode::invalid_font,
          "missing font files must use the font error code");

  const auto bogus_outline = engine.glyph_outline(999, 1);
  require(!bogus_outline.has_value(),
          "unknown font indices must be rejected");
}

} // namespace

int main() {
  project_fonts_resolve_first_and_record_fingerprints();
  latin_text_shapes_with_the_project_font();
  glyph_outlines_come_from_the_same_font();
  missing_glyphs_are_explicit_and_diagnosed();
  built_in_fallback_covers_ascii_without_font_files();
  vertical_typesetting_uses_uax50_orientation();
  rotated_cjk_shaping_uses_harfbuzz_clusters();
  invalid_input_is_rejected();
  std::cout << "PASS: Unicode text pipeline\n";
  return EXIT_SUCCESS;
}
