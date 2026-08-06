#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
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

void require_near(double actual, double expected, std::string_view message) {
  if (std::abs(actual - expected) > 1.0e-9) {
    std::cerr << "actual " << actual << " expected " << expected << '\n';
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

const auto document_id = id("30000000-0000-4000-8000-000000000001");
const auto axis_id = id("30000000-0000-4000-8000-000000000002");
const auto curve_id = id("30000000-0000-4000-8000-000000000003");
const auto track_id = id("30000000-0000-4000-8000-000000000004");
const auto text_layer_id = id("30000000-0000-4000-8000-000000000005");
const auto interval_layer_id = id("30000000-0000-4000-8000-000000000006");
const auto marker_layer_id = id("30000000-0000-4000-8000-000000000007");
const auto interval_id = id("30000000-0000-4000-8000-000000000008");
const auto marker_id = id("30000000-0000-4000-8000-000000000009");
const auto horizontal_id = id("30000000-0000-4000-8000-00000000000a");
const auto rotated_id = id("30000000-0000-4000-8000-00000000000b");
const auto vertical_id = id("30000000-0000-4000-8000-00000000000c");
const auto missing_id = id("30000000-0000-4000-8000-00000000000d");

std::shared_ptr<HarfBuzzTextEngine> make_engine() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  const auto font = engine->add_project_font(
      std::string{WELLLOG_TEST_FONT_DIR} + "/NotoSans-Regular.ttf");
  require(font.has_value(), "bundled test font must load");
  const auto cjk_font = engine->add_project_font(
      std::string{WELLLOG_TEST_FONT_DIR} + "/SourceHanSansCN-subset.otf");
  require(cjk_font.has_value(), "bundled CJK subset font must load");
  return engine;
}

WellLogDocument text_document() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1005.0, 1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{2});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  builder.add_interval(Interval{
      .id = interval_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1004.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = {},
      .fill_color = RgbaColor{220, 200, 120, 255},
      .label = "Sand",
  });
  builder.add_marker(Marker{
      .id = marker_id,
      .reference_depth = 1004.0,
      .semantic = MarkerSemantic::formation_top,
      .label = "Top",
  });
  builder.add_annotation(TextAnnotation{
      .id = horizontal_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1005.0,
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
  builder.add_annotation(TextAnnotation{
      .id = rotated_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1002.0,
      .track_fraction = 0.25,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "AB",
      .language = "en",
      .orientation = TextOrientation::rotated,
      .rotation_degrees = 90.0,
      .font_size = Millimetres{4.0},
  });
  builder.add_annotation(TextAnnotation{
      .id = vertical_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1007.0,
      .track_fraction = 0.75,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "\xE7\xA0\x82", // 砂
      .language = "zh-Hans",
      .orientation = TextOrientation::vertical,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });
  return builder.build();
}

ScenePresentationBuilder base_presentation() {
  ScenePresentationBuilder builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1010.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
  });
  return builder;
}

const PreparedTextRun &find_run(const PreparedScene &scene, EntityId source) {
  for (const auto &run : scene.text_runs()) {
    if (run.source_entity_id == source) {
      return run;
    }
  }
  fail("expected a prepared text run for the source entity");
}

void horizontal_rotated_and_vertical_runs_prepare_positions() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  require(session.execute(SetDocumentCommand{text_document()}).has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  builder.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .color = RgbaColor{10, 10, 10, 255},
  });
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(), "text presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");
  require(scene->text_layers().size() == 1, "one text layer expected");
  require(scene->text_layers().front().run_count == 3,
          "three annotations must produce three runs");

  // Horizontal: baseline anchors at the depth and track fraction.
  const auto &horizontal = find_run(*scene, horizontal_id);
  require(horizontal.layer_id == text_layer_id,
          "run must reference its text layer");
  require(horizontal.orientation == TextOrientation::horizontal,
          "orientation must round-trip");
  require(horizontal.glyph_count == 3, "Gas must shape to three glyphs");
  require_near(horizontal.anchor.left.value, 20.0,
               "anchor must sit at the track fraction");
  require_near(horizontal.anchor.top.value, 50.0,
               "anchor must sit at the display depth");
  const auto &glyphs = scene->glyphs();
  const auto &first = glyphs[static_cast<std::size_t>(horizontal.first_glyph)];
  require(first.code_point == U'G', "first glyph must be G");
  require_near(first.origin.left.value, 20.0,
               "first glyph origin must start at the anchor");
  require_near(first.origin.top.value, 50.0,
               "first glyph baseline must start at the anchor");
  require_near(first.rotation_degrees, 0.0,
               "horizontal glyphs must not rotate");
  const auto &second =
      glyphs[static_cast<std::size_t>(horizontal.first_glyph + 1)];
  require(second.origin.left.value > first.origin.left.value + 2.0,
          "pen must advance by a plausible glyph width");
  require_near(second.origin.top.value, first.origin.top.value,
               "horizontal baselines must stay level");
  require(horizontal.bounds.left.value <= first.origin.left.value,
          "run bounds must cover the glyph origins");
  require(horizontal.bounds.width.value > 0.0 &&
              horizontal.bounds.height.value > 0.0,
          "run bounds must have extent");

  // Rotated 90 degrees: the pen advances downward and glyphs rotate.
  const auto &rotated = find_run(*scene, rotated_id);
  require(rotated.orientation == TextOrientation::rotated,
          "rotated orientation must round-trip");
  require_near(rotated.rotation_degrees, 90.0,
               "run rotation must round-trip");
  const auto &rot_first =
      glyphs[static_cast<std::size_t>(rotated.first_glyph)];
  const auto &rot_second =
      glyphs[static_cast<std::size_t>(rotated.first_glyph + 1)];
  require_near(rot_first.rotation_degrees, 90.0,
               "glyphs must carry the run rotation");
  require_near(rot_first.origin.left.value, 10.0,
               "rotated run must anchor at the track fraction");
  require_near(rot_first.origin.top.value, 20.0,
               "rotated run must anchor at the display depth");
  require(rot_second.origin.top.value >
              rot_first.origin.top.value + 2.0,
          "a 90 degree run must advance downward");
  require_near(rot_second.origin.left.value, rot_first.origin.left.value,
               "a 90 degree run must not advance sideways");

  // True vertical: the CJK glyph stays upright and centers on the pen.
  const auto &vertical = find_run(*scene, vertical_id);
  require(vertical.orientation == TextOrientation::vertical,
          "vertical orientation must round-trip");
  require(vertical.glyph_count == 1, "one CJK glyph expected");
  const auto &cjk = glyphs[static_cast<std::size_t>(vertical.first_glyph)];
  require(cjk.upright, "CJK must stay upright in vertical typesetting");
  require_near(cjk.rotation_degrees, 0.0,
               "upright glyphs must not rotate");
  require_near(cjk.origin.left.value, 30.0 - 2.0,
               "upright glyphs must center on the pen line");
  require_near(cjk.origin.top.value, 70.0 + 2.0,
               "upright glyphs must offset below the pen");

  // Fonts and outlines are embedded for both backends.
  require(!scene->text_fonts().empty(), "used fonts must be recorded");
  require(scene->text_fonts().front().fingerprint.size() >= 18,
          "font fingerprints must be recorded");
  require(!scene->glyph_outlines().empty(),
          "used glyph outlines must be embedded");
  require(!scene->outline_commands().empty(),
          "outline commands must be embedded");
  for (const auto &outline : scene->glyph_outlines()) {
    require(outline.command_count > 0,
            "every used glyph must have outline commands");
  }
}

void interval_and_marker_labels_share_the_text_pipeline() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  require(session.execute(SetDocumentCommand{text_document()}).has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  builder.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = true,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{40, 40, 40, 255},
  });
  builder.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 1,
      .line_color = RgbaColor{200, 0, 0, 255},
      .line_width = Millimetres{0.5},
      .draw_labels = true,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{50, 50, 50, 255},
  });
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(), "label presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");

  require(scene->intervals().size() == 1, "one visible interval expected");
  const auto &interval = scene->intervals().front();
  require(interval.label_run_index != no_text_run,
          "labeled intervals must reference a text run");
  const auto &interval_run =
      scene->text_runs()[static_cast<std::size_t>(interval.label_run_index)];
  require(interval_run.layer_id == interval_layer_id,
          "interval label must belong to the interval layer");
  require(interval_run.source_entity_id == interval_id,
          "interval label must reference the interval entity");
  require(interval_run.text == "Sand", "label text must round-trip");
  require_near(interval_run.anchor.left.value, 1.0,
               "interval labels must pad from the track edge");
  require_near(interval_run.anchor.top.value,
               interval.rect.top.value + 0.5 + 3.0 * 0.85,
               "interval labels must sit inside the interval top");
  require(interval_run.color == RgbaColor(40, 40, 40, 255),
          "interval labels must use the layer label color");

  require(scene->markers().size() == 1, "one visible marker expected");
  const auto &marker = scene->markers().front();
  require(marker.label_run_index != no_text_run,
          "labeled markers must reference a text run");
  const auto &marker_run =
      scene->text_runs()[static_cast<std::size_t>(marker.label_run_index)];
  require(marker_run.layer_id == marker_layer_id,
          "marker label must belong to the marker layer");
  require(marker_run.source_entity_id == marker_id,
          "marker label must reference the marker entity");
  require_near(marker_run.anchor.top.value, marker.display_top.value - 0.6,
               "marker labels must sit above the line");
}

void missing_glyphs_flow_into_session_diagnostics() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  auto builder = WellLogDocumentBuilder(document_id, DocumentRevision{3});
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  builder.add_annotation(TextAnnotation{
      .id = missing_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1005.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "A\xF4\x8F\xBF\xBE", // A + U+10FFFE (no font covers it)
      .language = "en",
      .orientation = TextOrientation::horizontal,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "document must be accepted");
  auto presentation = base_presentation();
  presentation.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .color = RgbaColor{0, 0, 0, 255},
  });
  const auto receipt =
      session.execute(SetPresentationCommand{presentation.build()});
  require(receipt.has_value(),
          "missing glyphs must not fail the presentation");
  require(receipt.value().diagnostic_id.has_value(),
          "missing glyphs must report a diagnostic identity");

  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");
  require(scene->text_issues().size() >= 1, "a text issue must be recorded");
  require(scene->text_issues().front().code == TextIssueCode::missing_glyphs,
          "the issue must identify missing glyphs");
  require(scene->text_issues().front().entity_id == missing_id,
          "the issue must reference the offending annotation");
  const auto &run = find_run(*scene, missing_id);
  require(run.glyph_count == 2,
          "the missing code point must still produce a replacement glyph");
  const auto &replacement =
      scene->glyphs()[static_cast<std::size_t>(run.first_glyph + 1)];
  require(replacement.code_point == 0x10FFFE,
          "the replacement must keep the source code point");

  const auto diagnostics = session.diagnostics();
  require(!diagnostics.empty(), "session diagnostics must be published");
  auto found_missing = false;
  for (const auto &diagnostic : diagnostics) {
    if (diagnostic.code == DiagnosticCode::missing_glyphs &&
        diagnostic.entity_id == missing_id) {
      found_missing = true;
      require(diagnostic.severity == Severity::warning,
              "missing glyphs must be a warning");
      require(diagnostic.occurrence_count == 1,
              "one missing code point must be counted");
    }
  }
  require(found_missing, "missing-glyph diagnostic must be published");
}

void text_layers_without_an_engine_prepare_empty_with_a_diagnostic() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{text_document()}).has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  builder.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .color = RgbaColor{0, 0, 0, 255},
  });
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(),
          "text layers without an engine must still prepare");
  require(receipt.value().diagnostic_id.has_value(),
          "the unavailable engine must be diagnosed");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must be prepared");
  require(scene->text_layers().size() == 1, "the text layer must prepare");
  require(scene->text_layers().front().run_count == 0,
          "no runs can be shaped without an engine");
  require(scene->text_issues().size() == 1,
          "one engine-unavailable issue expected");
  require(scene->text_issues().front().code ==
              TextIssueCode::text_engine_unavailable,
          "the issue must identify the missing engine");
  require(scene->text_issues().front().occurrence_count == 3,
          "all three annotations must be counted as suppressed");
  auto found_diagnostic = false;
  for (const auto &diagnostic : session.diagnostics()) {
    if (diagnostic.code == DiagnosticCode::text_engine_unavailable) {
      found_diagnostic = true;
    }
  }
  require(found_diagnostic,
          "session must publish the engine-unavailable diagnostic");
}

void preparation_is_deterministic_across_identical_presentations() {
  WellLogSession session;
  session.set_text_engine(make_engine());
  require(session.execute(SetDocumentCommand{text_document()}).has_value(),
          "document must be accepted");
  const auto build_scene = [&] {
    auto builder = base_presentation();
    builder.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .z_order = 0,
        .draw_labels = true,
        .label_font_size = Millimetres{3.0},
        .label_color = RgbaColor{40, 40, 40, 255},
    });
    builder.add_text_layer(TextLayerSpec{
        .id = text_layer_id,
        .track_id = track_id,
        .z_order = 1,
        .color = RgbaColor{10, 10, 10, 255},
    });
    require(session.execute(SetPresentationCommand{builder.build()})
                .has_value(),
            "presentation must be accepted");
    return session.prepared_scene(document_id);
  };
  const auto first = build_scene();
  const auto second = build_scene();
  require(first->glyphs().size() == second->glyphs().size(),
          "glyph counts must be reproducible");
  require(first->text_runs().size() == second->text_runs().size(),
          "run counts must be reproducible");
  for (std::size_t index = 0; index < first->glyphs().size(); ++index) {
    const auto &a = first->glyphs()[index];
    const auto &b = second->glyphs()[index];
    require(a.glyph_id == b.glyph_id && a.font_index == b.font_index,
            "glyph identities must be reproducible");
    require_near(a.origin.left.value, b.origin.left.value,
                 "glyph positions must be reproducible");
    require_near(a.origin.top.value, b.origin.top.value,
                 "glyph positions must be reproducible");
  }
  require(first->text_fonts().front().fingerprint ==
              second->text_fonts().front().fingerprint,
          "font fingerprints must be reproducible");
}

} // namespace

int main() {
  horizontal_rotated_and_vertical_runs_prepare_positions();
  interval_and_marker_labels_share_the_text_pipeline();
  missing_glyphs_flow_into_session_diagnostics();
  text_layers_without_an_engine_prepare_empty_with_a_diagnostic();
  preparation_is_deterministic_across_identical_presentations();
  std::cout << "PASS: text preparation in the scene\n";
  return EXIT_SUCCESS;
}
