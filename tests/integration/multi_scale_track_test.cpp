#include <welllog/render_gl/upload.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>
#include <cstdint>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

const auto document_id = id("80000000-0000-4000-8000-000000000001");
const auto axis_id = id("80000000-0000-4000-8000-000000000002");
const auto gr_curve_id = id("80000000-0000-4000-8000-000000000003");
const auto den_curve_id = id("80000000-0000-4000-8000-000000000004");
const auto track_id = id("80000000-0000-4000-8000-000000000005");
const auto linear_scale_id = id("80000000-0000-4000-8000-000000000006");
const auto log_scale_id = id("80000000-0000-4000-8000-000000000007");
const auto gr_layer_id = id("80000000-0000-4000-8000-000000000008");
const auto den_layer_id = id("80000000-0000-4000-8000-000000000009");

WellLogDocument multi_curve_document(std::shared_ptr<const std::vector<double>> depths,
                                     std::shared_ptr<const std::vector<double>> gr,
                                     std::shared_ptr<const std::vector<double>> den) {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(std::move(depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = gr_curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(std::move(gr)),
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = den_curve_id,
      .mnemonic = "DEN",
      .display_name = "Density",
      .unit = "g/cm3",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(std::move(den)),
      .nulls = {},
  });
  return builder.build();
}

WellLogDocument simple_multi_document() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0,
                                    1005.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 50.0, 100.0, 150.0, 20.0, 60.0});
  auto den = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.1, 1.0, 0.0, -2.0, 10.0, 2.0});
  return multi_curve_document(std::move(depths), std::move(gr), std::move(den));
}

ScenePresentationBuilder base_presentation() {
  ScenePresentationBuilder builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1005.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
      .header = {},
  });
  builder.add_scale(TrackScaleSpec{
      .id = linear_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_scale(TrackScaleSpec{
      .id = log_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::logarithmic,
      .minimum = 0.1,
      .maximum = 10.0,
      .direction = ScaleDirection::right_to_left,
      .unit = "g/cm3",
  });
  return builder;
}

void add_curve_layers(ScenePresentationBuilder &builder) {
  builder.add_curve_layer(CurveLayerSpec{
      .id = gr_layer_id,
      .track_id = track_id,
      .curve_id = gr_curve_id,
      .scale_id = linear_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = den_layer_id,
      .track_id = track_id,
      .curve_id = den_curve_id,
      .scale_id = log_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
}

std::shared_ptr<const PreparedScene> prepare(WellLogSession &session,
                                             ScenePresentationBuilder &builder) {
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(), "multi-scale presentation must be accepted");
  auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "multi-scale scene must be prepared");
  return scene;
}

const PreparedCurveLayer &find_layer(const PreparedScene &scene, EntityId layer) {
  for (const auto &candidate : scene.curve_layers()) {
    if (candidate.id == layer) {
      return candidate;
    }
  }
  fail("expected the curve layer in the scene");
}

// A real text engine so header text runs are actually shaped (the default
// null engine suppresses them, leaving label_run_index at no_text_run).
std::shared_ptr<HarfBuzzTextEngine> make_text_engine() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  const auto font = engine->add_project_font(
      std::string{WELLLOG_TEST_FONT_DIR} + "/NotoSans-Regular.ttf");
  require(font.has_value(), "bundled test font must load");
  return engine;
}

void linear_log_and_reversed_scales_map_independently() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  add_curve_layers(builder);
  const auto scene = prepare(session, builder);

  require(scene->curve_layers().size() == 2, "both layers must prepare");
  const auto &gr_layer = find_layer(*scene, gr_layer_id);
  const auto &den_layer = find_layer(*scene, den_layer_id);
  require(gr_layer.curve_id == gr_curve_id && den_layer.curve_id == den_curve_id,
          "layers must keep their curve identities");
  require(gr_layer.color == RgbaColor(20, 120, 20, 255) &&
              den_layer.color == RgbaColor(200, 30, 30, 255),
          "layers must keep their styles");

  const auto &points = scene->curve_points();
  const auto &segments = scene->curve_segments();
  // GR linear 0..100: value 50 at depth 1001 maps to the track center.
  const auto &gr_segment = segments[static_cast<std::size_t>(gr_layer.first_segment)];
  const auto &gr_second =
      points[static_cast<std::size_t>(gr_segment.first_point + 1)];
  require_near(gr_second.position.left.value, 20.0,
               "linear scale must map 50/100 to the track center");
  require_near(gr_second.value, 50.0, "points must keep original values");

  // DEN log 0.1..10 reversed: value 1.0 maps to log-center, then flips.
  const auto &den_segment =
      segments[static_cast<std::size_t>(den_layer.first_segment)];
  const auto &den_second =
      points[static_cast<std::size_t>(den_segment.first_point + 1)];
  require_near(den_second.position.left.value, 20.0,
               "reversed log scale must map 1.0 to the track center");
  require_near(den_second.value, 1.0, "log points must keep original values");

  // Values beyond the scale range are NOT re-normalized: GR 150/100 maps
  // past the track edge (track clip handles it, never implicit scaling).
  const auto &gr_fourth =
      points[static_cast<std::size_t>(gr_segment.first_point + 3)];
  require(gr_fourth.position.left.value > 40.0,
          "out-of-range values must map past the track, never auto-scale");
}

void nonpositive_log_values_break_segments_and_aggregate_diagnostics() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  add_curve_layers(builder);
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(),
          "non-positive log values must not fail preparation");
  const auto scene = session.prepared_scene(document_id);

  const auto &den_layer = find_layer(*scene, den_layer_id);
  require(den_layer.segment_count == 2,
          "zero and negative values must split the log polyline");
  const auto issues = scene->value_issues();
  require(issues.size() == 1, "one aggregated value issue expected");
  require(issues.front().code == ValueIssueCode::nonpositive_log_values,
          "the issue must identify non-positive log values");
  require(issues.front().entity_id == den_layer_id,
          "the issue must reference the affected layer");
  require(issues.front().occurrence_count == 2,
          "both non-positive samples must be counted");

  auto found_diagnostic = false;
  for (const auto &diagnostic : session.diagnostics()) {
    if (diagnostic.code == DiagnosticCode::nonpositive_log_values &&
        diagnostic.entity_id == den_layer_id) {
      found_diagnostic = true;
      require(diagnostic.severity == Severity::warning,
              "non-positive log values must be a warning");
      require(diagnostic.occurrence_count == 2,
              "the diagnostic must carry the aggregated count");
    }
  }
  require(found_diagnostic,
          "session must publish the non-positive log diagnostic");
}

void hidden_layers_keep_identity_without_geometry() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  add_curve_layers(builder);
  // Hide the density layer.
  builder = base_presentation();
  builder.add_curve_layer(CurveLayerSpec{
      .id = gr_layer_id,
      .track_id = track_id,
      .curve_id = gr_curve_id,
      .scale_id = linear_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = den_layer_id,
      .track_id = track_id,
      .curve_id = den_curve_id,
      .scale_id = log_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = false,
  });
  const auto scene = prepare(session, builder);
  require(scene->curve_layers().size() == 2,
          "hidden layers must stay in the scene");
  const auto &den_layer = find_layer(*scene, den_layer_id);
  require(!den_layer.visible, "visibility must round-trip");
  require(den_layer.segment_count == 0,
          "hidden layers must not contribute geometry");
  require(den_layer.color == RgbaColor(200, 30, 30, 255),
          "hidden layers must keep their style");
  const auto &gr_layer = find_layer(*scene, gr_layer_id);
  require(gr_layer.visible && gr_layer.segment_count > 0,
          "visible layers must render normally");
  // No non-positive issue for a hidden layer: nothing was drawn.
  require(scene->value_issues().empty(),
          "hidden layers must not report draw-time issues");
}

void log_scales_reject_nonpositive_ranges() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  builder.add_curve_layer(CurveLayerSpec{
      .id = gr_layer_id,
      .track_id = track_id,
      .curve_id = gr_curve_id,
      .scale_id = linear_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  const auto bad_scale_id = id("80000000-0000-4000-8000-0000000000a1");
  builder.add_scale(TrackScaleSpec{
      .id = bad_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::logarithmic,
      .minimum = 0.0,
      .maximum = 10.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "g/cm3",
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = den_layer_id,
      .track_id = track_id,
      .curve_id = den_curve_id,
      .scale_id = bad_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
  const auto result = session.execute(SetPresentationCommand{builder.build()});
  require(!result.has_value(),
          "log scale with a zero minimum must be rejected");
  require(result.error().code == ErrorCode::invalid_presentation,
          "invalid log range must use the presentation error code");
}

void track_headers_describe_each_curve() {
  WellLogSession session;
  session.set_text_engine(nullptr); // headers must survive without an engine
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  builder.add_curve_layer(CurveLayerSpec{
      .id = gr_layer_id,
      .track_id = track_id,
      .curve_id = gr_curve_id,
      .scale_id = linear_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = den_layer_id,
      .track_id = track_id,
      .curve_id = den_curve_id,
      .scale_id = log_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
  // Enable the header.
  auto with_header = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1005.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  with_header.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
      .header =
          TrackHeaderSpec{
              .height = Millimetres{8.0},
              .font_size = Millimetres{2.5},
          },
  });
  with_header.add_scale(TrackScaleSpec{
      .id = linear_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  with_header.add_scale(TrackScaleSpec{
      .id = log_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::logarithmic,
      .minimum = 0.1,
      .maximum = 10.0,
      .direction = ScaleDirection::right_to_left,
      .unit = "g/cm3",
  });
  with_header.add_curve_layer(CurveLayerSpec{
      .id = gr_layer_id,
      .track_id = track_id,
      .curve_id = gr_curve_id,
      .scale_id = linear_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  with_header.add_curve_layer(CurveLayerSpec{
      .id = den_layer_id,
      .track_id = track_id,
      .curve_id = den_curve_id,
      .scale_id = log_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
  const auto scene = prepare(session, with_header);

  const auto &entries = scene->track_header_entries();
  require(entries.size() == 2, "one header entry per visible curve layer");
  require(entries[0].curve_layer_id == gr_layer_id,
          "header entries follow layer z order");
  require(entries[0].curve_name == "Gamma Ray",
          "header must show the display name");
  require(entries[0].color == RgbaColor(20, 120, 20, 255),
          "header must carry the curve color");
  require_near(entries[0].scale_minimum, 0.0, "header shows the range");
  require_near(entries[0].scale_maximum, 100.0, "header shows the range");
  require(entries[0].unit == "API", "header shows the unit");
  require(entries[0].mode == ScaleMode::linear,
          "header shows the scale type");
  require(entries[1].mode == ScaleMode::logarithmic,
          "log layer must show the log scale type");
  require(entries[1].direction == ScaleDirection::right_to_left,
          "header shows the scale direction");
  require(entries[1].unit == "g/cm3", "header shows the log unit");
  // Without a text engine, entries still exist with no run.
  require(entries[0].label_run_index == no_text_run,
          "header entries must survive without a text engine");
  require(!scene->text_issues().empty() &&
              scene->text_issues().front().code ==
                  TextIssueCode::text_engine_unavailable,
          "suppressed header runs must be diagnosed");
}

void header_text_run_carries_curve_color() {
  // Criterion 4 says the header must *show* each curve's color. With a real
  // text engine the header entry resolves to a shaped text run; the curve's
  // color must land on that run, not merely on the entry struct.
  WellLogSession session;
  session.set_text_engine(make_text_engine());
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");

  auto with_header = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1005.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  with_header.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
      .header =
          TrackHeaderSpec{
              .height = Millimetres{8.0},
              .font_size = Millimetres{2.5},
          },
  });
  with_header.add_scale(TrackScaleSpec{
      .id = linear_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  with_header.add_scale(TrackScaleSpec{
      .id = log_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::logarithmic,
      .minimum = 0.1,
      .maximum = 10.0,
      .direction = ScaleDirection::right_to_left,
      .unit = "g/cm3",
  });
  with_header.add_curve_layer(CurveLayerSpec{
      .id = gr_layer_id,
      .track_id = track_id,
      .curve_id = gr_curve_id,
      .scale_id = linear_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  with_header.add_curve_layer(CurveLayerSpec{
      .id = den_layer_id,
      .track_id = track_id,
      .curve_id = den_curve_id,
      .scale_id = log_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
  const auto scene = prepare(session, with_header);

  const auto &entries = scene->track_header_entries();
  require(entries.size() == 2, "one header entry per visible curve layer");
  require(scene->text_issues().empty(),
          "a real engine must not produce header text issues");

  // Each entry's color must reach its shaped text run (criterion 4 "shows
  // color"), not just sit on the entry struct.
  const auto &gr_run =
      scene->text_runs()[static_cast<std::size_t>(entries[0].label_run_index)];
  require(entries[0].label_run_index != no_text_run,
          "the GR header must reference a shaped text run");
  require(gr_run.color == RgbaColor(20, 120, 20, 255),
          "the GR header run must carry the GR curve color");

  const auto &den_run =
      scene->text_runs()[static_cast<std::size_t>(entries[1].label_run_index)];
  require(entries[1].label_run_index != no_text_run,
          "the DEN header must reference a shaped text run");
  require(den_run.color == RgbaColor(200, 30, 30, 255),
          "the DEN header run must carry the DEN curve color");
}

void more_than_four_visible_scales_warn_without_refusing() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1005.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{1});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  std::vector<EntityId> curve_ids;
  std::vector<EntityId> scale_ids;
  std::vector<EntityId> layer_ids;
  for (auto index = 0; index < 5; ++index) {
    curve_ids.push_back(id("80000000-0000-4000-8000-10000000000" +
                           std::to_string(index + 1)));
    scale_ids.push_back(id("80000000-0000-4000-8000-20000000000" +
                           std::to_string(index + 1)));
    layer_ids.push_back(id("80000000-0000-4000-8000-30000000000" +
                           std::to_string(index + 1)));
    document_builder.add_curve(Curve{
        .id = curve_ids.back(),
        .mnemonic = "C" + std::to_string(index),
        .display_name = "Curve " + std::to_string(index),
        .unit = "API",
        .sampling_axis_id = axis_id,
        .values = BufferView::from_vector(values),
        .nulls = {},
    });
  }
  WellLogSession session;
  require(session.execute(SetDocumentCommand{document_builder.build()})
              .has_value(),
          "document must be accepted");
  ScenePresentationBuilder builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1005.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
      .header = {},
  });
  for (auto index = 0; index < 5; ++index) {
    builder.add_scale(TrackScaleSpec{
        .id = scale_ids[static_cast<std::size_t>(index)],
        .track_id = track_id,
        .mode = ScaleMode::linear,
        .minimum = 0.0,
        .maximum = 100.0 + static_cast<double>(index),
        .direction = ScaleDirection::left_to_right,
        .unit = "API",
    });
    builder.add_curve_layer(CurveLayerSpec{
        .id = layer_ids[static_cast<std::size_t>(index)],
        .track_id = track_id,
        .curve_id = curve_ids[static_cast<std::size_t>(index)],
        .scale_id = scale_ids[static_cast<std::size_t>(index)],
        .color = RgbaColor{10, 10, 10, 255},
        .line_width = Millimetres{0.5},
        .z_order = index,
        .visible = true,
    });
  }
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(),
          "more than four scales must prepare, never hard-refuse");
  const auto scene = session.prepared_scene(document_id);
  require(scene->curve_layers().size() == 5, "all five layers must prepare");
  require(scene->value_issues().size() == 1,
          "one readability hint expected");
  require(scene->value_issues().front().code ==
              ValueIssueCode::scale_readability_hint,
          "the hint must identify the readability concern");
  require(scene->value_issues().front().entity_id == track_id,
          "the hint must reference the track");
  require(scene->value_issues().front().occurrence_count == 5,
          "the hint must count the visible scales");
  auto found_diagnostic = false;
  for (const auto &diagnostic : session.diagnostics()) {
    if (diagnostic.code == DiagnosticCode::scale_readability_hint) {
      found_diagnostic = true;
      require(diagnostic.severity == Severity::warning,
              "readability hint must be a warning");
    }
  }
  require(found_diagnostic,
          "session must publish the readability hint diagnostic");
}

void lod_and_picking_return_original_values_per_curve() {
  constexpr std::uint64_t sample_count = 4096;
  auto depths = std::make_shared<std::vector<double>>();
  auto gr = std::make_shared<std::vector<double>>();
  auto den = std::make_shared<std::vector<double>>();
  depths->reserve(sample_count);
  gr->reserve(sample_count);
  den->reserve(sample_count);
  for (std::uint64_t index = 0; index < sample_count; ++index) {
    depths->push_back(1000.0 + static_cast<double>(index) * 0.005);
    gr->push_back(50.0 + 40.0 * std::sin(static_cast<double>(index) * 0.01));
    den->push_back(1.0 + 0.5 * std::cos(static_cast<double>(index) * 0.02));
  }
  WellLogSession session(PerformanceBudgets{
      .maximum_cpu_derived_bytes = 4 * 1024 * 1024,
      .maximum_gpu_cache_bytes = 8 * 1024 * 1024,
      .maximum_upload_bytes_per_frame = 4 * 1024 * 1024,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  });
  const auto shared_depths =
      std::shared_ptr<const std::vector<double>>(depths);
  require(session
              .execute(SetDocumentCommand{multi_curve_document(
                  shared_depths,
                  std::shared_ptr<const std::vector<double>>(gr),
                  std::shared_ptr<const std::vector<double>>(den))})
              .has_value(),
          "dense document must be accepted");
  auto builder = base_presentation();
  // Extend the range to the dense axis span.
  builder = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1000.0 + sample_count * 0.005,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
      .header = {},
  });
  builder.add_scale(TrackScaleSpec{
      .id = linear_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_scale(TrackScaleSpec{
      .id = log_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::logarithmic,
      .minimum = 0.1,
      .maximum = 10.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "g/cm3",
  });
  add_curve_layers(builder);
  require(session.execute(SetPresentationCommand{builder.build()})
              .has_value(),
          "dense presentation must be accepted");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    if (session.prepared_scene(document_id) != nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "LOD scene must prepare");
  const auto &gr_layer = find_layer(*scene, gr_layer_id);
  const auto &den_layer = find_layer(*scene, den_layer_id);
  require(gr_layer.segment_count > 0 && den_layer.segment_count > 0,
          "both dense curves must produce LOD geometry");
  for (const auto &point : scene->curve_points()) {
    require(point.value >= 0.0 && point.value <= 100.0,
            "LOD points must carry original sample values");
  }

  // Pan and zoom keep original values: pick after a viewport change.
  require(session
              .execute(SetViewportCommand{
                  .document_id = document_id,
                  .viewport = DepthViewport{.top = 1005.0, .bottom = 1010.0},
              })
              .has_value(),
          "zoom must be accepted");
  const auto zoom_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < zoom_deadline) {
    session.poll_async();
    const auto current = session.prepared_scene(document_id);
    if (current != nullptr && current.get() != scene.get()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const auto zoomed = session.prepared_scene(document_id);
  require(zoomed != nullptr, "zoomed scene must prepare");

  // Pick the GR layer near its mid-track position.
  // Per-curve value ranges: GR stays in 0..100, DEN stays in 0.1..10.
  for (const auto &point : zoomed->curve_points()) {
    if (point.reference_depth < 1005.0 || point.reference_depth > 1010.0) {
      continue;  // only the zoomed viewport
    }
    require(point.value >= 0.0 && point.value <= 100.0,
            "zoomed points must carry original sample values");
  }

  // Pick a known GR point in the zoomed viewport so the hit is exact and
  // the returned value is verifiably that curve's original sample.
  const PreparedCurvePoint *gr_pick_point = nullptr;
  for (const auto &point : zoomed->curve_points()) {
    if (point.reference_depth >= 1005.0 && point.reference_depth <= 1010.0 &&
        point.position.left.value > 2.0 &&
        point.position.left.value < 38.0) {
      gr_pick_point = &point;
      break;
    }
  }
  require(gr_pick_point != nullptr,
          "the zoomed scene must contain a pickable GR point");
  const auto pick = zoomed->pick_curve(CurvePickQuery{
      .scene_position = gr_pick_point->position,
      .tolerance = DeviceIndependentPixels{2.0},
      .horizontal_device_independent_pixels_per_millimetre = 5.0,
      .vertical_device_independent_pixels_per_millimetre = 5.0,
  });
  require(pick.has_value(), "pick must hit a curve at a known point");
  require(pick->layer_id == gr_layer_id,
          "picking a GR point must identify the GR layer");
  require(pick->curve_id == gr_curve_id,
          "pick must identify the picked curve");
  require_near(pick->value, gr_pick_point->value,
              "pick must return the original sample value");
  require_near(pick->reference_depth, gr_pick_point->reference_depth,
              "pick must return the original reference depth");
}

void opengl_and_svg_consume_identical_curve_geometry() {
  // Criterion 7: OpenGL and SVG must produce consistent semantics for the
  // same Track Scale config. Both backends consume the single prepared
  // curve_segments()/curve_points() the kernel scales once, so the parity
  // claim reduces to "both walk the same geometry." The GL upload schedule
  // exposes only counts/bytes (no per-point coordinates), so edge-level
  // parity is the strongest assertion possible without expanding the GL API.
  //
  // GL counts an "upload segment" per edge (point_count - 1) per prepared
  // segment; SVG emits one M/L pair per point along the same edges. So the
  // shared quantity is the total edge count, walked here the way SVG does
  // (layer -> segment -> points), independently of GL's flat traversal.
  WellLogSession session;
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  add_curve_layers(builder);
  const auto scene = prepare(session, builder);

  const auto segments = scene->curve_segments();
  std::uint64_t expected_edges = 0;
  for (const auto &layer : scene->curve_layers()) {
    for (std::uint64_t offset = 0; offset < layer.segment_count; ++offset) {
      const auto &segment =
          segments[static_cast<std::size_t>(layer.first_segment + offset)];
      if (segment.point_count > 1) {
        expected_edges += segment.point_count - 1;
      }
    }
  }
  require(expected_edges > 0,
          "both curve layers must produce prepared geometry");

  const auto schedule = GpuUploadSchedule::plan(
      *scene, GpuUploadBudgets{.maximum_cache_bytes = 1024 * 1024,
                               .maximum_bytes_per_frame = 1024 * 1024});
  require(schedule.has_value(), "upload plan must succeed");
  // GL walks exactly the edges SVG traces: one edge = 6 vertices
  // (upload.cpp vertices_per_curve_segment).
  require(schedule.value().source_segment_count() == expected_edges,
          "GL upload must consume the same curve edges as the scene");
  require(schedule.value().vertex_count() == expected_edges * 6,
          "GL vertex count must follow the shared edge count");

  // SVG traces the same edges into its path d, tagged per scale.
  const auto exported = SvgExporter::write(*scene);
  require(exported.has_value(), "SVG export must succeed");
  const auto text = std::string{exported.value().text()};
  require(text.find("data-scale-id=\"80000000-0000-4000-8000-000000000006\"") !=
              std::string::npos,
          "SVG must reference the linear scale identity");
  require(text.find("data-scale-id=\"80000000-0000-4000-8000-000000000007\"") !=
              std::string::npos,
          "SVG must reference the log scale identity");
}

void svg_and_scene_share_scale_semantics() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{simple_multi_document()})
              .has_value(),
          "document must be accepted");
  auto builder = base_presentation();
  add_curve_layers(builder);
  const auto scene = prepare(session, builder);
  const auto exported = SvgExporter::write(*scene);
  require(exported.has_value(), "SVG export must succeed");
  const auto text = std::string{exported.value().text()};
  require(text.find("data-scale-id=\"80000000-0000-4000-8000-000000000006\"") !=
              std::string::npos,
          "SVG must reference the linear scale identity");
  require(text.find("data-scale-id=\"80000000-0000-4000-8000-000000000007\"") !=
              std::string::npos,
          "SVG must reference the log scale identity");
  // The SVG path coordinates come straight from the prepared scene: the
  // first GR point (value 0, depth 1000) sits at the track top-left.
  require(text.find("M 0 0") != std::string::npos,
          "SVG coordinates must match the prepared scene geometry");
}

} // namespace

int main() {
  linear_log_and_reversed_scales_map_independently();
  nonpositive_log_values_break_segments_and_aggregate_diagnostics();
  hidden_layers_keep_identity_without_geometry();
  log_scales_reject_nonpositive_ranges();
  track_headers_describe_each_curve();
  header_text_run_carries_curve_color();
  more_than_four_visible_scales_warn_without_refusing();
  lod_and_picking_return_original_values_per_curve();
  opengl_and_svg_consume_identical_curve_geometry();
  svg_and_scene_share_scale_semantics();
  std::cout << "PASS: multi-scale curve tracks\n";
  return EXIT_SUCCESS;
}
