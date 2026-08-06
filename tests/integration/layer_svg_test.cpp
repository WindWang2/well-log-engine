#include <welllog/export/svg.hpp>
#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>

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

const auto document_id = id("40000000-0000-4000-8000-000000000001");
const auto axis_id = id("40000000-0000-4000-8000-000000000002");
const auto curve_id = id("40000000-0000-4000-8000-000000000003");
const auto track_id = id("40000000-0000-4000-8000-000000000004");
const auto pattern_id = id("40000000-0000-4000-8000-000000000005");
const auto interval_layer_id = id("40000000-0000-4000-8000-000000000006");
const auto marker_layer_id = id("40000000-0000-4000-8000-000000000007");
const auto symbol_layer_id = id("40000000-0000-4000-8000-000000000008");
const auto text_layer_id = id("40000000-0000-4000-8000-000000000009");
const auto sand_id = id("40000000-0000-4000-8000-00000000000a");
const auto shale_id = id("40000000-0000-4000-8000-00000000000b");
const auto marker_id = id("40000000-0000-4000-8000-00000000000c");
const auto symbol_id = id("40000000-0000-4000-8000-00000000000d");
const auto annotation_id = id("40000000-0000-4000-8000-00000000000e");

[[nodiscard]] std::size_t occurrences(std::string_view haystack,
                                      std::string_view needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = haystack.find(needle, position)) !=
         std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void require_contains(std::string_view text, std::string_view needle,
                      std::string_view message) {
  if (text.find(needle) == std::string_view::npos) {
    std::cerr << "missing fragment: " << needle << '\n';
    fail(message);
  }
}

std::shared_ptr<HarfBuzzTextEngine> make_engine() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/NotoSans-Regular.ttf")
              .has_value(),
          "bundled test font must load");
  return engine;
}

std::shared_ptr<const PreparedScene> make_scene(WellLogSession &session) {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1005.0, 1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  WellLogDocumentBuilder document(document_id, DocumentRevision{6});
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
      .id = sand_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1004.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{220, 200, 120, 255},
      .label = "Sand",
  });
  document.add_interval(Interval{
      .id = shale_id,
      .top_reference_depth = 1004.0,
      .bottom_reference_depth = 1008.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{90, 90, 90, 255},
      .label = "Shale",
  });
  document.add_marker(Marker{
      .id = marker_id,
      .reference_depth = 1004.0,
      .semantic = MarkerSemantic::formation_top,
      .label = "Top B",
  });
  document.add_symbol(SymbolOccurrence{
      .id = symbol_id,
      .reference_depth = 1002.0,
      .track_fraction = 0.5,
      .kind = SymbolKind::diamond,
      .label = "fossil",
  });
  document.add_annotation(TextAnnotation{
      .id = annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1006.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "Gas",
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
              PatternLine{PhysicalPoint{Millimetres{-1.0}, Millimetres{-1.0}},
                          PhysicalPoint{Millimetres{5.0}, Millimetres{5.0}}},
          },
  });
  presentation.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
  });
  presentation.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 1,
      .line_color = RgbaColor{200, 0, 0, 255},
      .line_width = Millimetres{0.5},
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
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
          "presentation must be accepted");
  auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");
  return scene;
}

void patterns_export_as_scene_anchored_vector_definitions() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  const auto scene = make_scene(session);
  const auto document = SvgExporter::write(*scene);
  require(document.has_value(), "SVG export must succeed");
  const auto text = document.value().text();

  require_contains(text, "<pattern id=\"pat-", "pattern def must be emitted");
  require_contains(text, "patternUnits=\"userSpaceOnUse\"",
                   "pattern must anchor to scene coordinates");
  require_contains(text, "x=\"0\" y=\"0\" width=\"4\" height=\"4\"",
                   "tile anchor and physical size must round-trip");
  require(occurrences(text, "<pattern ") == 1,
          "two intervals sharing a pattern must share one definition");
  require(occurrences(text, "fill=\"url(#pat-") == 2,
          "both patterned intervals must reference the shared definition");
  // The diagonal is clipped to the tile so adjacent tiles connect cleanly.
  require_contains(text, "<line x1=\"0\" y1=\"0\" x2=\"4\" y2=\"4\"",
                   "pattern primitives must clip to the tile");
  require_contains(text, "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"#fffae6\"",
                   "pattern background must render inside the tile");
}

void intervals_markers_and_symbols_export_with_entity_ids() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  const auto scene = make_scene(session);
  const auto document = SvgExporter::write(*scene);
  require(document.has_value(), "SVG export must succeed");
  const auto text = document.value().text();

  require_contains(text, "id=\"interval-40000000-0000-4000-8000-00000000000a\"",
                   "sand interval must carry its entity identity");
  require_contains(text, "data-top-depth=\"1000\" data-bottom-depth=\"1004\"",
                   "interval depths must be exported");
  require_contains(text, "id=\"interval-40000000-0000-4000-8000-00000000000b\"",
                   "shale interval must carry its entity identity");
  require_contains(text, "y=\"40\" width=\"40\" height=\"40\"",
                   "intervals must be positioned in scene millimetres");
  require_contains(text,
                   "id=\"marker-40000000-0000-4000-8000-00000000000c\"",
                   "marker must carry its entity identity");
  require_contains(text, "x1=\"0\" y1=\"40\" x2=\"40\" y2=\"40\"",
                   "marker must span the track at its display depth");
  require_contains(text, "stroke=\"#c80000\"",
                   "marker must use the layer line color");
  require_contains(text,
                   "id=\"symbol-40000000-0000-4000-8000-00000000000d\"",
                   "symbol must carry its entity identity");
  require_contains(text, "data-reference-depth=\"1004\"",
                   "marker depth must be exported");
}

void text_exports_shared_glyph_outlines() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  const auto scene = make_scene(session);
  const auto document = SvgExporter::write(*scene);
  require(document.has_value(), "SVG export must succeed");
  const auto text = document.value().text();

  require(occurrences(text, "<path id=\"g0-") >= 3,
          "each distinct glyph must be defined once");
  require_contains(text, "id=\"run-40000000-0000-4000-8000-00000000000e\"",
                   "the run must carry the annotation identity");
  require_contains(text, "data-orientation=\"0\"",
                   "horizontal orientation must be exported");
  require(occurrences(text, "<use href=\"#g0-") == 3,
              "Gas must place three glyph instances");
  require_contains(text, "translate(20 60)",
                   "glyphs must anchor at the display depth");
  require_contains(text, ") scale(4 -4)",
                   "glyph instances must scale em space into millimetres");
}

void export_is_byte_for_byte_deterministic() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  const auto scene = make_scene(session);
  const auto first = SvgExporter::write(*scene);
  const auto second = SvgExporter::write(*scene);
  require(first.has_value() && second.has_value(), "exports must succeed");
  require(first.value().text() == second.value().text(),
          "repeated exports must be identical");
}

void vertical_cjk_text_exports_upright_glyphs() {
  WellLogSession session;
  auto engine = make_engine();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/SourceHanSansCN-subset.otf")
              .has_value(),
          "bundled CJK subset font must load");
  session.set_text_engine(std::move(engine));

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  WellLogDocumentBuilder document(document_id, DocumentRevision{7});
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
  document.add_annotation(TextAnnotation{
      .id = annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1005.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "\xE7\xA0\x82\xE5\xB2\xA9", // 砂岩
      .language = "zh-Hans",
      .orientation = TextOrientation::vertical,
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
  presentation.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .color = RgbaColor{10, 10, 10, 255},
  });
  require(session.execute(SetPresentationCommand{presentation.build()})
              .has_value(),
          "vertical presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");
  const auto exported = SvgExporter::write(*scene);
  require(exported.has_value(), "vertical export must succeed");
  const auto text = exported.value().text();
  require_contains(text, "data-orientation=\"2\"",
                   "vertical orientation must be exported");
  require(occurrences(text, "<use href=\"#g") == 2,
          "both CJK glyphs must be placed");
  require_contains(text, "rotate(0)", "upright glyphs must not rotate");
  require(occurrences(text, "translate(18 52)") == 1,
          "the first upright glyph must center on the pen line");
}

} // namespace

int main() {
  patterns_export_as_scene_anchored_vector_definitions();
  intervals_markers_and_symbols_export_with_entity_ids();
  text_exports_shared_glyph_outlines();
  export_is_byte_for_byte_deterministic();
  vertical_cjk_text_exports_upright_glyphs();
  std::cout << "PASS: layered SVG export\n";
  return EXIT_SUCCESS;
}
