#include <welllog/render_gl/upload.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/session/session.hpp>
#include <welllog/export/raster.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include "png_decode.hpp"

#include <cmath>
#include <cstdint>
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

// --- Shared identities ------------------------------------------------------
const auto document_id = id("90000000-0000-4000-8000-000000000001");
const auto axis_id = id("90000000-0000-4000-8000-000000000002");
const auto upper_curve_id = id("90000000-0000-4000-8000-000000000003");
const auto lower_curve_id = id("90000000-0000-4000-8000-000000000004");
const auto track_id = id("90000000-0000-4000-8000-000000000005");
const auto upper_scale_id = id("90000000-0000-4000-8000-000000000006");
const auto lower_scale_id = id("90000000-0000-4000-8000-000000000007");
const auto upper_layer_id = id("90000000-0000-4000-8000-000000000008");
const auto lower_layer_id = id("90000000-0000-4000-8000-000000000009");
const auto fill_layer_id = id("90000000-0000-4000-8000-00000000000a");
const auto pattern_id = id("90000000-0000-4000-8000-00000000000b");
const auto axis2_id = id("90000000-0000-4000-8000-00000000000c");
const auto upper2_curve_id = id("90000000-0000-4000-8000-00000000000d");

// Two curves sharing one axis; upper and lower value vectors.
WellLogDocument two_curve_document(
    std::shared_ptr<const std::vector<double>> depths,
    std::shared_ptr<const std::vector<double>> upper,
    std::shared_ptr<const std::vector<double>> lower) {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(std::move(depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = upper_curve_id,
      .mnemonic = "UP",
      .display_name = "Upper",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(std::move(upper)),
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = lower_curve_id,
      .mnemonic = "LO",
      .display_name = "Lower",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(std::move(lower)),
      .nulls = {},
  });
  return builder.build();
}

// A two-curve document where the upper curve sits on a second sampling axis
// (heterogeneous axes) at a different sample grid.
WellLogDocument heterogeneous_axis_document(
    std::shared_ptr<const std::vector<double>> upper_depths,
    std::shared_ptr<const std::vector<double>> upper,
    std::shared_ptr<const std::vector<double>> lower_depths,
    std::shared_ptr<const std::vector<double>> lower) {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis2_id,
      .coordinates = BufferView::from_vector(std::move(upper_depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(std::move(lower_depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = upper2_curve_id,
      .mnemonic = "UP2",
      .display_name = "Upper2",
      .unit = "API",
      .sampling_axis_id = axis2_id,
      .values = BufferView::from_vector(std::move(upper)),
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = lower_curve_id,
      .mnemonic = "LO",
      .display_name = "Lower",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(std::move(lower)),
      .nulls = {},
  });
  return builder.build();
}

// Builds a presentation with two visible curve layers (upper + lower) on the
// given scales and adds one crossover fill layer between them.
ScenePresentationBuilder fill_presentation(EntityId upper_cid,
                                           EntityId upper_lid) {
  auto builder = ScenePresentationBuilder(
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
      .header = {},
  });
  builder.add_scale(TrackScaleSpec{
      .id = upper_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_scale(TrackScaleSpec{
      .id = lower_scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = upper_lid,
      .track_id = track_id,
      .curve_id = upper_cid,
      .scale_id = upper_scale_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = lower_layer_id,
      .track_id = track_id,
      .curve_id = lower_curve_id,
      .scale_id = lower_scale_id,
      .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
  builder.add_crossover_fill_layer(CrossoverFillLayerSpec{
      .id = fill_layer_id,
      .track_id = track_id,
      .z_order = 2,
      .upper_curve_layer_id = upper_lid,
      .lower_curve_layer_id = lower_layer_id,
      .rule = CrossoverFillRule::upper_minus_lower,
      .fill_color = RgbaColor{255, 200, 0, 255},
      .pattern_id = std::nullopt,
      .visible = true,
  });
  return builder;
}

std::shared_ptr<const PreparedScene>
prepare(WellLogSession &session, const WellLogDocument &document,
        ScenePresentationBuilder &builder) {
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "crossover document must be accepted");
  const auto receipt = session.execute(SetPresentationCommand{builder.build()});
  require(receipt.has_value(), "crossover presentation must be accepted");
  auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "crossover scene must be prepared");
  return scene;
}

const PreparedFillRegion &single_region(const PreparedScene &scene) {
  const auto &layers = scene.fill_layers();
  require(layers.size() == 1, "exactly one fill layer expected");
  require(layers.front().region_count >= 1, "at least one fill region expected");
  return scene.fill_regions()[static_cast<std::size_t>(
      layers.front().first_region)];
}

// --- Tests ------------------------------------------------------------------

// Two linear curves that cross once: upper starts below lower, then rises
// above. The enclosed region between the crossing and the end is filled.
void linear_curves_cross_and_fill_one_region() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0,
                                    1010.0});
  // Upper: 10,30,50,70,80,90  Lower: 60,55,50,45,30,10
  // They are equal (~50) at depth ~1004; upper<lower before, upper>lower after.
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 30.0, 50.0, 70.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{60.0, 55.0, 50.0, 45.0, 30.0, 10.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  const auto scene = prepare(session, two_curve_document(depths, upper, lower),
                             builder);

  const auto &region = single_region(*scene);
  require(region.layer_id == fill_layer_id,
          "region must belong to the fill layer");
  require(region.upper_curve_layer_id == upper_layer_id &&
              region.lower_curve_layer_id == lower_layer_id,
          "region must carry both dependent curve layers");
  require(region.vertex_count >= 3,
          "region boundary must be a closed polygon");
  require(region.triangle_count >= 1,
          "region must be triangulated for GL");
  // The region starts at the crossing (depth ~1004) and runs to the end
  // (1010): its depth span covers that interval.
  require(region.bottom_reference_depth > region.top_reference_depth,
          "region must span a depth interval");
  // The region begins where upper rises strictly above lower (diff > 0),
  // first at depth 1006 (upper 70 vs lower 45), and runs to the end (1010).
  require_near(region.top_reference_depth, 1006.0,
               "region must start where upper-minus-lower turns positive");
}

// Reversed direction on the upper scale must still compute the crossing from
// mapped x-coordinates, not raw values.
void reversed_scale_crosses_on_mapped_coordinates() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{20.0, 40.0, 60.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{50.0, 50.0, 50.0, 50.0, 50.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  // Flip the upper scale to right-to-left.
  builder = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1010.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id, .width = Millimetres{40.0}, .z_order = 0, .header = {}});
  builder.add_scale(TrackScaleSpec{
      .id = upper_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::right_to_left, .unit = "API"});
  builder.add_scale(TrackScaleSpec{
      .id = lower_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  builder.add_curve_layer(CurveLayerSpec{
      .id = upper_layer_id, .track_id = track_id, .curve_id = upper_curve_id,
      .scale_id = upper_scale_id, .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5}, .z_order = 0, .visible = true});
  builder.add_curve_layer(CurveLayerSpec{
      .id = lower_layer_id, .track_id = track_id, .curve_id = lower_curve_id,
      .scale_id = lower_scale_id, .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5}, .z_order = 1, .visible = true});
  builder.add_crossover_fill_layer(CrossoverFillLayerSpec{
      .id = fill_layer_id, .track_id = track_id, .z_order = 2,
      .upper_curve_layer_id = upper_layer_id,
      .lower_curve_layer_id = lower_layer_id,
      .rule = CrossoverFillRule::upper_minus_lower,
      .fill_color = RgbaColor{255, 200, 0, 255}, .pattern_id = std::nullopt,
      .visible = true});
  const auto scene = prepare(session, two_curve_document(depths, upper, lower),
                             builder);

  const auto &layers = scene->fill_layers();
  require(layers.size() == 1, "one fill layer expected with a reversed scale");
  // Upper is reversed (high value -> left), lower is centred. A crossing and
  // an enclosed region must still be detected from the mapped coordinates.
  require(layers.front().region_count >= 1,
          "reversed scale must still produce a fill region");
}

// A fully-null lower curve yields no fill regions and no crash.
void fully_missing_curve_produces_no_fill() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{40.0, 60.0, 80.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{40.0, 60.0, 80.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = upper_curve_id, .mnemonic = "UP", .display_name = "Upper",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(upper), .nulls = {}});
  // Lower curve entirely null.
  auto nulls_owner = std::make_shared<std::vector<unsigned char>>(
      std::vector<unsigned char>{0x07}); // bits 0,1,2 set -> 3 nulls
  builder.add_curve(Curve{
      .id = lower_curve_id, .mnemonic = "LO", .display_name = "Lower",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(lower),
      .nulls = NullBitmapView::from_raw(nulls_owner->data(), 3,
                                        nulls_owner->size(),
                                        SharedOwner{nulls_owner})});
  const auto document = builder.build();

  WellLogSession session;
  auto presentation = fill_presentation(upper_curve_id, upper_layer_id);
  require(
      session.execute(SetDocumentCommand{document}).has_value(),
      "document with a fully-null curve must be accepted");
  require(session
              .execute(SetPresentationCommand{presentation.build()})
              .has_value(),
          "crossover presentation with a fully-null curve must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "scene must prepare despite a fully-null curve");
  require(scene->fill_layers().size() == 1,
          "the fill layer must keep its identity");
  require(scene->fill_layers().front().region_count == 0,
          "a fully-null curve must produce zero fill regions");
  require(scene->fill_vertices().empty(),
          "no fill geometry must be emitted for a fully-null curve");
}

// Oscillating curves produce several crossings and several regions.
void multiple_intersections_produce_multiple_regions() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0,
                                    1005.0, 1006.0, 1007.0, 1008.0, 1009.0});
  // Upper: three positive bands (each spanning two samples) around lower=50:
  // negative at 1000, then 80,80 / 20,20 / 80,80 / 20,20 -> three filled bands.
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{20.0, 80.0, 80.0, 20.0, 20.0, 80.0, 80.0,
                                    20.0, 20.0, 80.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{50.0, 50.0, 50.0, 50.0, 50.0, 50.0, 50.0,
                                    50.0, 50.0, 50.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  const auto scene = prepare(session, two_curve_document(depths, upper, lower),
                             builder);
  const auto &layers = scene->fill_layers();
  require(layers.size() == 1, "one fill layer expected");
  // Upper-minus-lower is positive on the two-sample 80-bands: >= 2 regions.
  require(layers.front().region_count >= 2,
          "oscillating curves must produce multiple fill regions");
}

// Heterogeneous axes: upper on a sparser grid; the lower is interpolated
// locally and a region still forms over the common depth interval.
void heterogeneous_axes_interpolate_on_common_depth() {
  auto upper_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1003.0, 1006.0});
  // Upper below 50 at the top, then above for two samples (90,90).
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 90.0, 90.0});
  auto lower_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0,
                                    1005.0, 1006.0});
  // Lower flat at 50.
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{50.0, 50.0, 50.0, 50.0, 50.0, 50.0, 50.0});
  WellLogSession session;
  const auto document = heterogeneous_axis_document(
      upper_depths, upper, lower_depths, lower);
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "heterogeneous-axis document must be accepted");

  auto builder = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1006.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id, .width = Millimetres{40.0}, .z_order = 0, .header = {}});
  builder.add_scale(TrackScaleSpec{
      .id = upper_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  builder.add_scale(TrackScaleSpec{
      .id = lower_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  builder.add_curve_layer(CurveLayerSpec{
      .id = upper_layer_id, .track_id = track_id, .curve_id = upper2_curve_id,
      .scale_id = upper_scale_id, .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5}, .z_order = 0, .visible = true});
  builder.add_curve_layer(CurveLayerSpec{
      .id = lower_layer_id, .track_id = track_id, .curve_id = lower_curve_id,
      .scale_id = lower_scale_id, .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5}, .z_order = 1, .visible = true});
  builder.add_crossover_fill_layer(CrossoverFillLayerSpec{
      .id = fill_layer_id, .track_id = track_id, .z_order = 2,
      .upper_curve_layer_id = upper_layer_id,
      .lower_curve_layer_id = lower_layer_id,
      .rule = CrossoverFillRule::upper_minus_lower,
      .fill_color = RgbaColor{255, 200, 0, 255}, .pattern_id = std::nullopt,
      .visible = true});
  require(
      session.execute(SetPresentationCommand{builder.build()}).has_value(),
      "heterogeneous-axis presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "heterogeneous-axis scene must prepare");
  const auto &layers = scene->fill_layers();
  require(layers.size() == 1, "one fill layer expected");
  require(layers.front().region_count >= 1,
          "heterogeneous axes must still produce a fill region via local "
          "interpolation on the common depth interval");
}

// A fill layer must reject when neither color nor pattern is set, or both.
void fill_requires_exactly_one_of_color_or_pattern() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{30.0, 70.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{70.0, 30.0});
  WellLogSession session;
  require(session
              .execute(SetDocumentCommand{
                  two_curve_document(depths, upper, lower)})
              .has_value(),
          "document must be accepted");

  auto neither = fill_presentation(upper_curve_id, upper_layer_id);
  neither = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1002.0},
      Millimetres{100.0}, "font-fixture-v1");
  neither.add_track(TrackSpec{
      .id = track_id, .width = Millimetres{40.0}, .z_order = 0, .header = {}});
  neither.add_scale(TrackScaleSpec{
      .id = upper_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  neither.add_scale(TrackScaleSpec{
      .id = lower_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  neither.add_curve_layer(CurveLayerSpec{
      .id = upper_layer_id, .track_id = track_id, .curve_id = upper_curve_id,
      .scale_id = upper_scale_id, .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5}, .z_order = 0, .visible = true});
  neither.add_curve_layer(CurveLayerSpec{
      .id = lower_layer_id, .track_id = track_id, .curve_id = lower_curve_id,
      .scale_id = lower_scale_id, .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5}, .z_order = 1, .visible = true});
  neither.add_crossover_fill_layer(CrossoverFillLayerSpec{
      .id = fill_layer_id, .track_id = track_id, .z_order = 2,
      .upper_curve_layer_id = upper_layer_id,
      .lower_curve_layer_id = lower_layer_id,
      .rule = CrossoverFillRule::upper_minus_lower,
      .fill_color = std::nullopt, .pattern_id = std::nullopt,
      .visible = true});
  const auto neither_result =
      session.execute(SetPresentationCommand{neither.build()});
  require(!neither_result.has_value(),
          "a fill with neither color nor pattern must be rejected");
  require(neither_result.error().code == ErrorCode::invalid_presentation,
          "missing fill style must use the presentation error code");
}

// A fill region is pickable and returns both dependent curve ids + a depth.
void fill_is_pickable_and_returns_both_curves() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 30.0, 60.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{60.0, 55.0, 50.0, 45.0, 30.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  const auto scene = prepare(session, two_curve_document(depths, upper, lower),
                             builder);
  const auto &region = single_region(*scene);
  const auto vertices = scene->fill_vertices();
  // The region ring is [upper points..., reversed lower points...]. Take the
  // mid-depth upper point (first half) and average its left with the matching
  // lower point's left to land safely between the two curves, inside the ring.
  const auto upper_count = region.vertex_count / 2;
  const auto mid_upper = static_cast<std::size_t>(
      region.first_vertex + upper_count / 2);
  const auto mid_lower = static_cast<std::size_t>(
      region.first_vertex + upper_count + (upper_count - upper_count / 2 - 1));
  const auto inside_left =
      0.5 * (vertices[mid_upper].position.left.value +
             vertices[mid_lower].position.left.value);
  const auto inside_top = vertices[mid_upper].position.top.value;
  const auto pick = scene->pick_fill(FillPickQuery{
      .scene_position = PhysicalPoint{.left = Millimetres{inside_left},
                                      .top = Millimetres{inside_top}},
  });
  require(pick.has_value(), "a point inside the region must be picked");
  require(pick->layer_id == fill_layer_id, "pick must identify the fill layer");
  require(pick->upper_curve_layer_id == upper_layer_id &&
              pick->lower_curve_layer_id == lower_layer_id,
          "pick must return both dependent curve layers");
  require(pick->upper_curve_id == upper_curve_id &&
              pick->lower_curve_id == lower_curve_id,
          "pick must resolve both dependent curve ids");
  require(pick->reference_depth > 1000.0 && pick->reference_depth < 1010.0,
          "pick must return a reference depth within the region");
}

// GL and SVG consume the same fill geometry: the upload schedule's fill
// triangle count equals the scene's, and the SVG carries both curve tags.
void opengl_and_svg_consume_identical_fill_geometry() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 30.0, 60.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{60.0, 55.0, 50.0, 45.0, 30.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  const auto scene = prepare(session, two_curve_document(depths, upper, lower),
                             builder);

  std::uint64_t expected_triangles = 0;
  for (const auto &layer : scene->fill_layers()) {
    for (std::uint64_t offset = 0; offset < layer.region_count; ++offset) {
      expected_triangles += scene->fill_regions()[static_cast<std::size_t>(
                                          layer.first_region + offset)]
                                .triangle_count;
    }
  }
  require(expected_triangles > 0, "fill geometry must be produced");

  const auto schedule = GpuUploadSchedule::plan(
      *scene, GpuUploadBudgets{.maximum_cache_bytes = 1024 * 1024,
                               .maximum_bytes_per_frame = 1024 * 1024});
  require(schedule.has_value(), "upload plan must succeed");
  require(schedule.value().fill_triangle_count() == expected_triangles,
          "GL upload must account for every prepared fill triangle");

  const auto exported = SvgExporter::write(*scene);
  require(exported.has_value(), "SVG export must succeed");
  const auto text = std::string{exported.value().text()};
  require(text.find("data-upper-curve-layer-id=\"" +
                    upper_layer_id.to_string() + "\"") != std::string::npos,
          "SVG must tag the upper dependent curve layer");
  require(text.find("data-lower-curve-layer-id=\"" +
                    lower_layer_id.to_string() + "\"") != std::string::npos,
          "SVG must tag the lower dependent curve layer");
  require(text.find("fill=\"#ffc800\"") != std::string::npos,
          "SVG must emit the solid fill color");
}

// Repeated (duplicate) depths must not divide by zero or corrupt the fill.
void repeated_depth_handles_degenerate_segments() {
  // Two consecutive samples at depth 1004 (repeated) on both curves.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1004.0, 1006.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{20.0, 40.0, 60.0, 60.0, 80.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{70.0, 60.0, 50.0, 50.0, 40.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  require(session
              .execute(SetDocumentCommand{
                  two_curve_document(depths, upper, lower)})
              .has_value(),
          "document with repeated depths must be accepted");
  require(session.execute(SetPresentationCommand{builder.build()}).has_value(),
          "crossover presentation with repeated depths must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "repeated-depth scene must prepare");
  const auto &layers = scene->fill_layers();
  require(layers.size() == 1, "one fill layer expected");
  // Upper-minus-lower turns positive around depth 1004-1006; a region must
  // form without crashing or producing NaN geometry.
  require(layers.front().region_count >= 1,
          "repeated depths must still produce a fill region");
  for (const auto &region : scene->fill_regions()) {
    for (std::uint64_t v = 0; v < region.vertex_count; ++v) {
      const auto &vertex = scene->fill_vertices()[static_cast<std::size_t>(
          region.first_vertex + v)];
      require(std::isfinite(vertex.position.left.value) &&
                  std::isfinite(vertex.position.top.value),
              "fill vertices must be finite despite repeated depths");
    }
  }
}

// An interior null gap on the lower curve must break the fill rather than
// interpolating across the missing region (criterion 3).
void interior_null_breaks_the_fill_region() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{20.0, 40.0, 60.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{60.0, 55.0, 50.0, 45.0, 40.0});
  // Null out the lower sample at index 2 (depth 1002), splitting the lower
  // polyline into two runs [0,1] and [3,4] with a gap at 1002.
  auto nulls_owner = std::make_shared<std::vector<unsigned char>>(
      std::vector<unsigned char>{0x04}); // bit 2 set
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = upper_curve_id, .mnemonic = "UP", .display_name = "Upper",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(upper), .nulls = {}});
  builder.add_curve(Curve{
      .id = lower_curve_id, .mnemonic = "LO", .display_name = "Lower",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(lower),
      .nulls = NullBitmapView::from_raw(nulls_owner->data(), 5,
                                        nulls_owner->size(),
                                        SharedOwner{nulls_owner})});
  const auto document = builder.build();

  WellLogSession session;
  auto presentation = fill_presentation(upper_curve_id, upper_layer_id);
  require(
      session.execute(SetDocumentCommand{document}).has_value(),
      "document with an interior null must be accepted");
  require(
      session.execute(SetPresentationCommand{presentation.build()}).has_value(),
      "crossover presentation with an interior null must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "interior-null scene must prepare");
  // No region may span across the gap at depth 1002: every region's depth
  // span must lie entirely above or entirely below 1002, never crossing it.
  for (const auto &region : scene->fill_regions()) {
    const auto crosses_gap =
        region.top_reference_depth < 1002.0 &&
        region.bottom_reference_depth > 1002.0;
    require(!crosses_gap,
            "a fill region must not span across a lower-curve null gap");
  }
}

// A pattern fill resolves to url(#pat-...) in SVG and is accounted for in GL.
void pattern_fill_resolves_in_both_backends() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 30.0, 60.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{60.0, 55.0, 50.0, 45.0, 30.0});
  WellLogSession session;
  require(session
              .execute(SetDocumentCommand{
                  two_curve_document(depths, upper, lower)})
              .has_value(),
          "document must be accepted");
  auto builder = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1010.0},
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id, .width = Millimetres{40.0}, .z_order = 0, .header = {}});
  builder.add_scale(TrackScaleSpec{
      .id = upper_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  builder.add_scale(TrackScaleSpec{
      .id = lower_scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  builder.add_curve_layer(CurveLayerSpec{
      .id = upper_layer_id, .track_id = track_id, .curve_id = upper_curve_id,
      .scale_id = upper_scale_id, .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5}, .z_order = 0, .visible = true});
  builder.add_curve_layer(CurveLayerSpec{
      .id = lower_layer_id, .track_id = track_id, .curve_id = lower_curve_id,
      .scale_id = lower_scale_id, .color = RgbaColor{200, 30, 30, 255},
      .line_width = Millimetres{0.5}, .z_order = 1, .visible = true});
  builder.add_pattern(PatternDefinition{
      .id = pattern_id, .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0}, .rotation_degrees = 0.0,
      .foreground = RgbaColor{0, 0, 0, 255}, .background = RgbaColor{255, 255, 0, 255},
      .stroke_width = Millimetres{0.3},
      .scene_anchor = PhysicalPoint{}, .primitives = {PatternLine{
          .from = PhysicalPoint{}, .to = PhysicalPoint{.left = Millimetres{4.0},
                                                       .top = Millimetres{4.0}}}}});
  builder.add_crossover_fill_layer(CrossoverFillLayerSpec{
      .id = fill_layer_id, .track_id = track_id, .z_order = 2,
      .upper_curve_layer_id = upper_layer_id,
      .lower_curve_layer_id = lower_layer_id,
      .rule = CrossoverFillRule::upper_minus_lower, .fill_color = std::nullopt,
      .pattern_id = pattern_id, .visible = true});
  require(session.execute(SetPresentationCommand{builder.build()}).has_value(),
          "pattern fill presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "pattern fill scene must prepare");
  require(scene->fill_layers().front().region_count >= 1,
          "pattern fill must produce a region");

  const auto schedule = GpuUploadSchedule::plan(
      *scene, GpuUploadBudgets{.maximum_cache_bytes = 1024 * 1024,
                               .maximum_bytes_per_frame = 1024 * 1024});
  require(schedule.has_value(), "pattern fill upload plan must succeed");
  require(schedule.value().fill_triangle_count() > 0,
          "GL must account for pattern fill triangles");
  const auto exported = SvgExporter::write(*scene);
  require(exported.has_value(), "pattern SVG export must succeed");
  const auto text = std::string{exported.value().text()};
  require(text.find("fill=\"url(#pat-" + pattern_id.to_string() + ")") !=
              std::string::npos,
          "SVG must reference the pattern fill");
}

// Issue #477 (paleo-workbench audit): the raster backend must draw crossover
// fill regions — the raster path previously handled only interval, symbol,
// curve and marker layers, so PNG/TIFF exports silently lacked the fills.
void raster_export_draws_crossover_fill() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1002.0, 1004.0, 1006.0, 1008.0,
                                    1010.0});
  auto upper = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 30.0, 50.0, 70.0, 80.0, 90.0});
  auto lower = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{60.0, 55.0, 50.0, 45.0, 30.0, 10.0});
  WellLogSession session;
  auto builder = fill_presentation(upper_curve_id, upper_layer_id);
  const auto scene = prepare(session, two_curve_document(depths, upper, lower),
                             builder);
  const auto &region = single_region(*scene);
  const auto fill = region.fill_color;

  ExportSnapshot snapshot{
      .document_id = document_id,
      .document_revision = DocumentRevision{1},
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{.domain = DepthDomain::measured_depth,
                                   .unit = "m",
                                   .reference_top = 1000.0,
                                   .reference_bottom = 1010.0,
                                   .version = 1},
      .font_asset_fingerprint = "font-fixture-v1",
      .page = ExportPageSpec{.mode = PaginationMode::continuous,
                             .page_width = Millimetres{100.0},
                             .page_height = Millimetres{150.0},
                             .dpi = 100,
                             .well_name = "Crossover-Raster"},
  };
  const auto path = std::filesystem::temp_directory_path() /
                    "welllog-crossover-raster.png";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  const auto report = export_raster_sync(
      *scene, snapshot,
      RasterExportRequest{.path = path,
                          .format = RasterImageFormat::png,
                          .width_px = 200,
                          .height_px = 300,
                          .background = RgbaColor{255, 255, 255, 255},
                          .color_space = RasterColorSpace::srgb,
                          .tile_height_px = 64});
  require(report.has_value(), "crossover raster export must succeed");

  const auto png = welllog::test::decode_png(path);
  std::filesystem::remove(path, ec);
  require(png.has_value(), "crossover PNG must decode");
  const auto stride = static_cast<std::size_t>(png->width) * 4;
  const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
    const auto at = stride * y + static_cast<std::size_t>(x) * 4;
    return std::array<std::uint8_t, 4>{png->samples[at], png->samples[at + 1],
                                       png->samples[at + 2],
                                       png->samples[at + 3]};
  };
  const auto expected = std::array<std::uint8_t, 4>{fill.red, fill.green,
                                                   fill.blue, fill.alpha};
  std::size_t fill_pixels = 0;
  for (std::uint32_t y = 0; y < png->height; ++y) {
    for (std::uint32_t x = 0; x < png->width; ++x) {
      if (pixel(x, y) == expected) {
        ++fill_pixels;
      }
    }
  }
  require(fill_pixels > 0,
          "the raster export must contain crossover fill pixels");
}

} // namespace

int main() {
  linear_curves_cross_and_fill_one_region();
  raster_export_draws_crossover_fill();
  reversed_scale_crosses_on_mapped_coordinates();
  fully_missing_curve_produces_no_fill();
  multiple_intersections_produce_multiple_regions();
  heterogeneous_axes_interpolate_on_common_depth();
  fill_requires_exactly_one_of_color_or_pattern();
  fill_is_pickable_and_returns_both_curves();
  opengl_and_svg_consume_identical_fill_geometry();
  repeated_depth_handles_degenerate_segments();
  interior_null_breaks_the_fill_region();
  pattern_fill_resolves_in_both_backends();
  std::cout << "PASS: cross-scale curve crossover fill\n";
  return EXIT_SUCCESS;
}
