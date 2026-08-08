// TVD/TVDSS 域区间道投影 (Epic C 收尾 slice 2): a decreasing display-depth
// transform (TVDSS: deeper MD → smaller subsea depth) must project
// intervals and markers to the correct page positions, and exports must
// stay valid. Also covers the increasing TVD domain.

#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void require_near(double actual, double expected, double tolerance,
                   std::string_view message) {
  if (std::abs(actual - expected) > tolerance) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

struct Fixture {
  WellLogDocument document;
  PreparedScene scene;
};

// Builds a session + document + presentation with a marker at 1500 m and an
// interval 1200–1800 m, under the given (MD → display) control points.
Fixture make_fixture(std::vector<DepthControlPoint> control_points,
                     double window_top, double window_bottom) {
  const auto document_id = id("15900000-0000-4000-8000-000000000001");
  const auto axis_id = id("15900000-0000-4000-8000-000000000002");
  const auto curve_id = id("15900000-0000-4000-8000-000000000003");
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1500.0, 2000.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::vector<double>{0.0, 50.0, 100.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
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
  builder.add_marker(Marker{
      .id = id("15900000-0000-4000-8000-000000000010"),
      .reference_depth = 1500.0,
      .semantic = MarkerSemantic::casing_shoe,
      .label = "Shoe",
  });
  builder.add_interval(Interval{
      .id = id("15900000-0000-4000-8000-000000000011"),
      .top_reference_depth = 1200.0,
      .bottom_reference_depth = 1800.0,
      .semantic = IntervalSemantic::custom,
      .pattern_id = {},
      .fill_color = RgbaColor{.red = 200, .green = 200, .blue = 200,
                              .alpha = 255},
      .label = "Zone",
  });
  auto document = builder.build();

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "fixture document must load");

  const auto track_id = id("15900000-0000-4000-8000-000000000020");
  const auto scale_id = id("15900000-0000-4000-8000-000000000021");
  const auto layer_id = id("15900000-0000-4000-8000-000000000022");
  const auto marker_layer_id = id("15900000-0000-4000-8000-000000000023");
  const auto interval_layer_id = id("15900000-0000-4000-8000-000000000024");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = window_top,
                          .bottom = window_bottom},
      Millimetres{100.0}, "font-fixture-v1");
  presentation.set_depth_transform(DepthTransform{
      .control_points = std::move(control_points),
      .extrapolate = DepthExtrapolatePolicy::clamp,
      .version = 1,
  });
  presentation.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 1});
  presentation.add_scale(TrackScaleSpec{.id = scale_id,
                                        .track_id = track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 0.0,
                                        .maximum = 100.0,
                                        .direction = ScaleDirection::left_to_right,
                                        .unit = "API"});
  presentation.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 0, .green = 100, .blue = 200, .alpha = 255},
      .line_width = Millimetres{0.4},
      .z_order = 1,
      .visible = true,
  });
  presentation.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 2,
      .line_color = RgbaColor{.red = 200, .green = 30, .blue = 30, .alpha = 255},
      .line_width = Millimetres{0.3},
      .draw_labels = true,
      .draw_symbols = true,
      .symbol_size = Millimetres{3.0},
  });
  presentation.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "fixture presentation must load");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr, "fixture must prepare a scene");
  return Fixture{.document = std::move(document), .scene = *scene};
}

void tvd_increasing_domain_projects_intervals_and_markers() {
  // TVD display: MD 1000→2000 maps linearly to TVD 900→1900 (window 900..1900).
  auto fixture = make_fixture(
      std::vector<DepthControlPoint>{
          {.reference_depth = 1000.0, .display_depth = 900.0},
          {.reference_depth = 2000.0, .display_depth = 1900.0},
      },
      900.0, 1900.0);
  const auto &scene = fixture.scene;
  require(scene.markers().size() == 1, "marker prepared");
  // 1500 m → TVD 1400 → (1400-900)/1000 * 100 mm = 50 mm.
  require_near(scene.markers().front().display_top.value, 50.0, 1e-6,
               "TVD marker projects to 50 mm");
  require(scene.intervals().size() == 1, "interval prepared");
  // 1200 m → 1100 TVD → 20 mm; 1800 m → 1700 TVD → 80 mm.
  require_near(scene.intervals().front().rect.top.value, 20.0, 1e-6,
               "TVD interval top projects to 20 mm");
  require_near(scene.intervals().front().rect.height.value, 60.0, 1e-6,
               "TVD interval spans 60 mm");
}

void tvdss_decreasing_domain_projects_intervals_and_markers() {
  // TVDSS display (decreasing): MD 1000→2000 maps to TVDSS +10→-300; the
  // display window runs +10 (top of page) down to -300 (bottom).
  auto fixture = make_fixture(
      std::vector<DepthControlPoint>{
          {.reference_depth = 1000.0, .display_depth = 10.0},
          {.reference_depth = 2000.0, .display_depth = -300.0},
      },
      10.0, -300.0);
  const auto &scene = fixture.scene;
  require(scene.markers().size() == 1, "marker prepared");
  // 1500 m → TVDSS -145 → (-145-10)/(-300-10) * 100 mm ≈ 50 mm.
  require_near(scene.markers().front().display_top.value, 50.0, 1e-6,
               "TVDSS marker projects to 50 mm");
  require(scene.intervals().size() == 1, "interval prepared");
  // 1200 m → TVDSS -52 → (-52-10)/(-310) * 100 = 20 mm; 1800 m → -238 → 80 mm.
  require_near(scene.intervals().front().rect.top.value, 20.0, 1e-3,
               "TVDSS interval top projects deeper than the shallow end");
  require_near(scene.intervals().front().rect.height.value, 60.0, 1e-3,
               "TVDSS interval span is positive on the page");
  // Deeper interval end must sit BELOW the shallower end on the page.
  require(scene.intervals().front().rect.top.value +
                  scene.intervals().front().rect.height.value >
              scene.intervals().front().rect.top.value,
          "deeper TVDSS renders lower on the page");

  // The inverse map must round-trip interior display values (regression for
  // the either-direction search).
  const DepthTransform xform{
      .control_points = std::vector<DepthControlPoint>{
          {.reference_depth = 1000.0, .display_depth = 10.0},
          {.reference_depth = 1500.0, .display_depth = -145.0},
          {.reference_depth = 2000.0, .display_depth = -300.0},
      },
      .extrapolate = DepthExtrapolatePolicy::clamp,
      .version = 1,
  };
  require_near(map_display_to_reference(xform, -200.0), 1677.419, 1e-3,
               "inverse map brackets decreasing display values correctly");
  require_near(map_display_to_reference(xform, -300.0), 2000.0, 1e-6,
               "inverse map hits the control-point endpoint");
  require_near(map_reference_to_display(xform, 1677.419), -200.0, 1e-3,
               "forward map round-trips the inverse result");

  // SVG export stays structurally valid under the decreasing transform.
  auto svg = SvgExporter::write(fixture.scene);
  require(svg.has_value(), "TVDSS SVG export must succeed");
  require(svg.value().text().find("interval-15900000-0000-4000-8000-000000000011") !=
              std::string::npos,
          "TVDSS SVG contains the projected interval");
}

void decreasing_window_is_rejected_when_degenerate() {
  // top == bottom is still invalid (zero-height window).
  const auto transform_ok = validate_depth_transform(DepthTransform{
      .control_points = std::vector<DepthControlPoint>{
          {.reference_depth = 1000.0, .display_depth = 10.0},
          {.reference_depth = 2000.0, .display_depth = -300.0},
      },
      .version = 1,
  });
  require(!transform_ok.has_value(), "decreasing transform validates");
  const auto flat = validate_depth_transform(DepthTransform{
      .control_points = std::vector<DepthControlPoint>{
          {.reference_depth = 1000.0, .display_depth = 10.0},
          {.reference_depth = 2000.0, .display_depth = 10.0},
      },
      .version = 1,
  });
  require(flat.has_value(), "flat display (non-monotonic) is rejected");
  const auto non_monotonic = validate_depth_transform(DepthTransform{
      .control_points = std::vector<DepthControlPoint>{
          {.reference_depth = 1000.0, .display_depth = 10.0},
          {.reference_depth = 1500.0, .display_depth = -100.0},
          {.reference_depth = 2000.0, .display_depth = -50.0},
      },
      .version = 1,
  });
  require(non_monotonic.has_value(), "direction change is rejected");
}

} // namespace

int main() {
  tvd_increasing_domain_projects_intervals_and_markers();
  tvdss_decreasing_domain_projects_intervals_and_markers();
  decreasing_window_is_rejected_when_degenerate();
  return EXIT_SUCCESS;
}
