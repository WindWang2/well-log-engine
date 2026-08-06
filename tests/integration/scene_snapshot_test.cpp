// Semantic snapshot verification (ADR 0036): a full layered scene is
// prepared once and its prepared content — glyph positions, pattern
// anchors, clipping and entity identities — is compared against a golden
// snapshot. The bundled test font keeps shaping deterministic; the font
// fingerprint is asserted first so a font change fails loudly instead of
// silently shifting glyph coordinates.
#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  // _Exit, not std::exit: avoid CRT/DLL teardown while LOD/frame worker
  // jthreads are still mid-flight (Windows loader-lock deadlock, #241).
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

#ifndef WELLLOG_TEST_FONT_DIR
#define WELLLOG_TEST_FONT_DIR "tests/assets/fonts"
#endif

const auto document_id = id("70000000-0000-4000-8000-000000000001");
const auto axis_id = id("70000000-0000-4000-8000-000000000002");
const auto curve_id = id("70000000-0000-4000-8000-000000000003");
const auto track_id = id("70000000-0000-4000-8000-000000000004");
const auto scale_id = id("70000000-0000-4000-8000-000000000005");
const auto curve_layer_id = id("70000000-0000-4000-8000-000000000006");
const auto pattern_id = id("70000000-0000-4000-8000-000000000007");
const auto interval_layer_id = id("70000000-0000-4000-8000-000000000008");
const auto marker_layer_id = id("70000000-0000-4000-8000-000000000009");
const auto symbol_layer_id = id("70000000-0000-4000-8000-00000000000a");
const auto text_layer_id = id("70000000-0000-4000-8000-00000000000b");
const auto interval_id = id("70000000-0000-4000-8000-00000000000c");
const auto marker_id = id("70000000-0000-4000-8000-00000000000d");
const auto symbol_id = id("70000000-0000-4000-8000-00000000000e");
const auto note_id = id("70000000-0000-4000-8000-00000000000f");

void append_number(std::string &output, double value) {
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + 64, value,
                                    std::chars_format::general, 9);
  output.append(buffer.data(), result.ptr);
}

void append_rect(std::string &output, const PhysicalRect &rect) {
  append_number(output, rect.left.value);
  output.push_back(',');
  append_number(output, rect.top.value);
  output.push_back(',');
  append_number(output, rect.width.value);
  output.push_back(',');
  append_number(output, rect.height.value);
}

std::string snapshot_scene(const PreparedScene &scene) {
  std::string output;
  for (const auto &track : scene.tracks()) {
    output += "track ";
    output += track.id.to_string();
    output += " clip ";
    append_rect(output, track.clip);
    output.push_back('\n');
  }
  for (const auto &pattern : scene.patterns()) {
    output += "pattern ";
    output += pattern.id.to_string();
    output += " tile ";
    append_number(output, pattern.tile_width.value);
    output.push_back(',');
    append_number(output, pattern.tile_height.value);
    output += " anchor ";
    append_number(output, pattern.scene_anchor.left.value);
    output.push_back(',');
    append_number(output, pattern.scene_anchor.top.value);
    output += " rotation ";
    append_number(output, pattern.rotation_degrees);
    output.push_back('\n');
  }
  for (const auto &interval : scene.intervals()) {
    output += "interval ";
    output += interval.interval_id.to_string();
    output += " layer ";
    output += interval.layer_id.to_string();
    output += " rect ";
    append_rect(output, interval.rect);
    output += " pattern ";
    output += interval.pattern_id.is_nil() ? "none"
                                           : interval.pattern_id.to_string();
    output += " depths ";
    append_number(output, interval.top_reference_depth);
    output.push_back(',');
    append_number(output, interval.bottom_reference_depth);
    output += " label_run ";
    output += interval.label_run_index == no_text_run
                  ? "none"
                  : std::to_string(interval.label_run_index);
    output.push_back('\n');
  }
  for (const auto &marker : scene.markers()) {
    output += "marker ";
    output += marker.marker_id.to_string();
    output += " top ";
    append_number(output, marker.display_top.value);
    output += " depth ";
    append_number(output, marker.reference_depth);
    output += " label_run ";
    output += marker.label_run_index == no_text_run
                  ? "none"
                  : std::to_string(marker.label_run_index);
    output.push_back('\n');
  }
  for (const auto &symbol : scene.symbols()) {
    output += "symbol ";
    output += symbol.symbol_id.to_string();
    output += " kind ";
    output += std::to_string(static_cast<unsigned>(symbol.kind));
    output += " center ";
    append_number(output, symbol.center.left.value);
    output.push_back(',');
    append_number(output, symbol.center.top.value);
    output.push_back('\n');
  }
  for (const auto &font : scene.text_fonts()) {
    output += "font ";
    output += std::to_string(font.index);
    output.push_back(' ');
    output += font.fingerprint;
    output += " \"";
    output += font.family_name;
    output += "\"\n";
  }
  for (const auto &run : scene.text_runs()) {
    output += "run ";
    output += run.source_entity_id.to_string();
    output += " layer ";
    output += run.layer_id.to_string();
    output += " anchor ";
    append_number(output, run.anchor.left.value);
    output.push_back(',');
    append_number(output, run.anchor.top.value);
    output += " orientation ";
    output += std::to_string(static_cast<unsigned>(run.orientation));
    output += " rotation ";
    append_number(output, run.rotation_degrees);
    output += " size ";
    append_number(output, run.font_size.value);
    output += " bounds ";
    append_rect(output, run.bounds);
    output += " glyphs ";
    output += std::to_string(run.glyph_count);
    output += " text=\"";
    output += run.text;
    output += "\"\n";
    for (std::uint64_t offset = 0; offset < run.glyph_count; ++offset) {
      const auto &glyph =
          scene.glyphs()[static_cast<std::size_t>(run.first_glyph + offset)];
      output += "  glyph cp=";
      output += std::to_string(static_cast<std::uint32_t>(glyph.code_point));
      output += " font=";
      output += std::to_string(glyph.font_index);
      output += " gid=";
      output += std::to_string(glyph.glyph_id);
      output += " origin=";
      append_number(output, glyph.origin.left.value);
      output.push_back(',');
      append_number(output, glyph.origin.top.value);
      output += " rotation=";
      append_number(output, glyph.rotation_degrees);
      output += " upright=";
      output += glyph.upright ? '1' : '0';
      output.push_back('\n');
    }
  }
  for (const auto &outline : scene.glyph_outlines()) {
    output += "outline font=";
    output += std::to_string(outline.font_index);
    output += " gid=";
    output += std::to_string(outline.glyph_id);
    output += " advance=";
    append_number(output, outline.advance_x);
    output += " box=";
    append_number(output, outline.left);
    output.push_back(',');
    append_number(output, outline.bottom);
    output.push_back(',');
    append_number(output, outline.right);
    output.push_back(',');
    append_number(output, outline.top);
    output += " commands=";
    output += std::to_string(outline.command_count);
    output.push_back('\n');
  }
  return output;
}

std::shared_ptr<const PreparedScene> make_snapshot_scene(
    WellLogSession &session) {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0,
                                    1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 20.0, 40.0, 60.0, 80.0, 100.0});
  WellLogDocumentBuilder document(document_id, DocumentRevision{11});
  document.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  document.add_interval(Interval{
      .id = interval_id,
      .top_reference_depth = 1002.0,
      .bottom_reference_depth = 1006.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{220, 200, 120, 255},
      .label = "Sand",
  });
  document.add_marker(Marker{
      .id = marker_id,
      .reference_depth = 1006.0,
      .semantic = MarkerSemantic::formation_top,
      .label = "Top",
  });
  document.add_symbol(SymbolOccurrence{
      .id = symbol_id,
      .reference_depth = 1004.0,
      .track_fraction = 0.5,
      .kind = SymbolKind::diamond,
      .label = {},
  });
  document.add_annotation(TextAnnotation{
      .id = note_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1008.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "Gas 100 m3",
      .language = "en",
      .orientation = TextOrientation::horizontal,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });
  require(session.execute(SetDocumentCommand{document.build()}).has_value(),
          "snapshot document must be accepted");

  ScenePresentationBuilder presentation(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1010.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
  });
  presentation.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation.add_curve_layer(CurveLayerSpec{
      .id = curve_layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{20, 52, 86, 255},
      .line_width = Millimetres{0.5},
      .z_order = 10,
      .visible = true,
  });
  presentation.add_pattern(PatternDefinition{
      .id = pattern_id,
      .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{60, 60, 60, 255},
      .background = RgbaColor{255, 250, 230, 255},
      .stroke_width = Millimetres{0.2},
      .scene_anchor = PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
      .primitives =
          {
              PatternLine{PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
                          PhysicalPoint{Millimetres{4.0}, Millimetres{4.0}}},
          },
  });
  presentation.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = true,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{40, 40, 40, 255},
  });
  presentation.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 1,
      .line_color = RgbaColor{200, 0, 0, 255},
      .line_width = Millimetres{0.5},
      .draw_labels = true,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{50, 50, 50, 255},
  });
  presentation.add_symbol_layer(SymbolLayerSpec{
      .id = symbol_layer_id,
      .track_id = track_id,
      .z_order = 2,
      .color = RgbaColor{0, 0, 200, 255},
      .symbol_size = Millimetres{4.0},
  });
  presentation.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 3,
      .color = RgbaColor{10, 10, 10, 255},
  });
  require(session.execute(SetPresentationCommand{presentation.build()})
              .has_value(),
          "snapshot presentation must be accepted");
  auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "snapshot scene must be prepared");
  return scene;
}

void missing_glyphs_and_clipping_enter_the_snapshot_scene() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/NotoSans-Regular.ttf")
              .has_value(),
          "bundled test font must load");
  WellLogSession session;
  session.set_text_engine(std::move(engine));

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  WellLogDocumentBuilder document(document_id, DocumentRevision{12});
  document.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  // Crosses the visible range bottom: must clamp to the scene edge.
  document.add_interval(Interval{
      .id = interval_id,
      .top_reference_depth = 1006.0,
      .bottom_reference_depth = 1014.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = {},
      .fill_color = RgbaColor{90, 90, 90, 255},
      .label = {},
  });
  document.add_annotation(TextAnnotation{
      .id = note_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1002.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "A\xF4\x8F\xBF\xBE", // A + U+10FFFE (uncovered)
      .language = "en",
      .orientation = TextOrientation::horizontal,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });
  require(session.execute(SetDocumentCommand{document.build()}).has_value(),
          "document must be accepted");
  ScenePresentationBuilder presentation(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1010.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
  });
  presentation.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
  });
  presentation.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 1,
      .color = RgbaColor{0, 0, 0, 255},
  });
  require(session.execute(SetPresentationCommand{presentation.build()})
              .has_value(),
          "presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");

  // Clipping: the interval crossing the range bottom clamps to 100mm.
  require(scene->intervals().size() == 1, "one interval expected");
  const auto &interval = scene->intervals().front();
  require(interval.rect.top.value == 60.0 &&
              interval.rect.height.value == 40.0,
          "interval crossing the range must clamp to the scene edge");

  // Missing glyphs: an explicit issue names the offending annotation and
  // the replacement glyph is not a silent .notdef box.
  auto found_issue = false;
  for (const auto &issue : scene->text_issues()) {
    if (issue.code == TextIssueCode::missing_glyphs &&
        issue.entity_id == note_id) {
      found_issue = true;
      require(issue.occurrence_count == 1,
              "exactly one missing code point must be counted");
    }
  }
  require(found_issue, "the missing glyph must be issued by the scene");
  const auto snapshot = snapshot_scene(*scene);
  auto found_replacement = false;
  for (const auto &glyph : scene->glyphs()) {
    if (glyph.code_point == 0x10FFFE) {
      found_replacement = true;
      require(glyph.glyph_id != 0,
              "replacement must not be a silent .notdef glyph");
      const auto font = std::find_if(
          scene->text_fonts().begin(), scene->text_fonts().end(),
          [&](const PreparedTextFont &candidate) {
            return candidate.index == glyph.font_index;
          });
      require(font != scene->text_fonts().end() &&
                  font->fingerprint == "builtin:font5x7:v1",
              "replacement must come from the built-in fallback font");
    }
  }
  require(found_replacement, "the replacement glyph must be in the scene");
  require(!scene->glyph_outlines().empty(),
          "the replacement outline must be embedded");
}

} // namespace

int main(int argc, char **argv) {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/NotoSans-Regular.ttf")
              .has_value(),
          "bundled test font must load");
  WellLogSession session;
  session.set_text_engine(std::move(engine));
  const auto scene = make_snapshot_scene(session);
  const auto snapshot = snapshot_scene(*scene);

  if (argc > 1 && std::string_view{argv[1]} == "--dump") {
    std::cout << snapshot;
    return EXIT_SUCCESS;
  }

  require(scene->text_fonts().size() == 1,
          "the snapshot scene must use exactly one font");
  require(scene->text_fonts().front().fingerprint ==
              "bcb9a6e8677ebb4a:0",
          "the snapshot font fingerprint must match the bundled Noto font; "
          "update the golden snapshot if the font is intentionally changed");

  constexpr std::string_view expected = R"SNAPSHOT(track 70000000-0000-4000-8000-000000000004 clip 0,0,40,100
pattern 70000000-0000-4000-8000-000000000007 tile 4,4 anchor 0,0 rotation 0
interval 70000000-0000-4000-8000-00000000000c layer 70000000-0000-4000-8000-000000000008 rect 0,20,40,40 pattern 70000000-0000-4000-8000-000000000007 depths 1002,1006 label_run 1
marker 70000000-0000-4000-8000-00000000000d top 60 depth 1006 label_run 2
symbol 70000000-0000-4000-8000-00000000000e kind 3 center 20,40
font 0 bcb9a6e8677ebb4a:0 "Noto Sans"
run 70000000-0000-4000-8000-00000000000f layer 70000000-0000-4000-8000-00000000000b anchor 20,80 orientation 0 rotation 0 size 4 bounds 20,76,23.756,8 glyphs 10 text="Gas 100 m3"
  glyph cp=71 font=0 gid=42 origin=20,80 rotation=0 upright=1
  glyph cp=97 font=0 gid=68 origin=22.912,80 rotation=0 upright=1
  glyph cp=115 font=0 gid=86 origin=25.156,80 rotation=0 upright=1
  glyph cp=32 font=0 gid=3 origin=27.072,80 rotation=0 upright=1
  glyph cp=49 font=0 gid=20 origin=28.112,80 rotation=0 upright=1
  glyph cp=48 font=0 gid=19 origin=30.4,80 rotation=0 upright=1
  glyph cp=48 font=0 gid=19 origin=32.688,80 rotation=0 upright=1
  glyph cp=32 font=0 gid=3 origin=34.976,80 rotation=0 upright=1
  glyph cp=109 font=0 gid=80 origin=36.016,80 rotation=0 upright=1
  glyph cp=51 font=0 gid=22 origin=39.756,80 rotation=0 upright=1
run 70000000-0000-4000-8000-00000000000c layer 70000000-0000-4000-8000-000000000008 anchor 1,23.05 orientation 0 rotation 0 size 3 bounds 1,20.05,8.184,6 glyphs 4 text="Sand"
  glyph cp=83 font=0 gid=54 origin=1,23.05 rotation=0 upright=1
  glyph cp=97 font=0 gid=68 origin=2.647,23.05 rotation=0 upright=1
  glyph cp=110 font=0 gid=81 origin=4.33,23.05 rotation=0 upright=1
  glyph cp=100 font=0 gid=71 origin=6.184,23.05 rotation=0 upright=1
run 70000000-0000-4000-8000-00000000000d layer 70000000-0000-4000-8000-000000000009 anchor 1,59.4 orientation 0 rotation 0 size 3 bounds 1,56.4,6.273,6 glyphs 3 text="Top"
  glyph cp=84 font=0 gid=55 origin=1,59.4 rotation=0 upright=1
  glyph cp=111 font=0 gid=82 origin=2.458,59.4 rotation=0 upright=1
  glyph cp=112 font=0 gid=83 origin=4.273,59.4 rotation=0 upright=1
outline font=0 gid=3 advance=0.26 box=0,0,0,0 commands=0
outline font=0 gid=19 advance=0.572 box=0.049,-0.01,0.523,0.725 commands=22
outline font=0 gid=20 advance=0.572 box=0.089,0,0.355,0.714 commands=13
outline font=0 gid=22 advance=0.572 box=0.045,-0.01,0.515,0.724 commands=33
outline font=0 gid=42 advance=0.728 box=0.061,-0.01,0.654,0.724 commands=27
outline font=0 gid=54 advance=0.549 box=0.051,-0.01,0.502,0.724 commands=32
outline font=0 gid=55 advance=0.556 box=0.01,0,0.545,0.714 commands=10
outline font=0 gid=68 advance=0.561 box=0.046,-0.01,0.48,0.545 commands=33
outline font=0 gid=71 advance=0.615 box=0.055,-0.01,0.53,0.76 commands=30
outline font=0 gid=80 advance=0.935 box=0.085,0,0.854,0.546 commands=28
outline font=0 gid=81 advance=0.618 box=0.085,0,0.537,0.546 commands=18
outline font=0 gid=82 advance=0.605 box=0.055,-0.01,0.551,0.546 commands=22
outline font=0 gid=83 advance=0.615 box=0.085,-0.24,0.56,0.546 commands=31
outline font=0 gid=86 advance=0.479 box=0.051,-0.01,0.434,0.546 commands=32
)SNAPSHOT";

  if (snapshot != expected) {
    std::cerr << "snapshot mismatch; actual:\n" << snapshot;
    fail("prepared scene snapshot must match the golden snapshot");
  }
  missing_glyphs_and_clipping_enter_the_snapshot_scene();
  std::cout << "PASS: prepared scene semantic snapshot\n";
  return EXIT_SUCCESS;
}
