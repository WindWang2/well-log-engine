#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

void session_prepares_a_physical_curve_track() {
  const auto document_id = id("10000000-0000-4000-8000-000000000001");
  const auto axis_id = id("10000000-0000-4000-8000-000000000002");
  const auto curve_id = id("10000000-0000-4000-8000-000000000003");
  const auto track_id = id("10000000-0000-4000-8000-000000000004");
  const auto scale_id = id("10000000-0000-4000-8000-000000000005");
  const auto layer_id = id("10000000-0000-4000-8000-000000000006");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0,
                                    1005.0, 1006.0, 1007.0, 1008.0});
  auto values =
      std::make_shared<const std::vector<double>>(std::initializer_list<double>{
          0.0, 10.0, std::numeric_limits<double>::quiet_NaN(), 30.0, 40.0,
          std::numeric_limits<double>::infinity(), 60.0, 70.0, 80.0});
  auto nulls = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0b10000000, 0});
  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{9});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = NullBitmapView::from_raw(nulls->data(), values->size(),
                                        nulls->size(), SharedOwner{nulls}),
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "source document must be accepted");

  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1008.0,
      },
      Millimetres{80.0}, "font-fixture-v1");
  presentation_builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{30.0},
      .z_order = 10,
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 80.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{.red = 0x12, .green = 0x34, .blue = 0x56, .alpha = 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 20,
      .visible = true,
  });

  const auto receipt =
      session.execute(SetPresentationCommand{presentation_builder.build()});
  require(receipt.has_value(), "valid curve presentation must be accepted");
  require(receipt.value().document_id == document_id,
          "presentation receipt must retain document identity");
  require(receipt.value().document_revision == DocumentRevision{9},
          "presentation receipt must retain document revision");

  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "presentation command must prepare a scene");
  require(scene->document_id() == document_id,
          "prepared scene must retain document identity");
  require(scene->document_revision() == DocumentRevision{9},
          "prepared scene must retain document revision");
  require(scene->tracks().size() == 1,
          "prepared scene must contain the configured track");
  require(scene->tracks().front().id == track_id,
          "prepared track must retain stable identity");
  require_near(scene->tracks().front().bounds.width.value, 30.0,
               "track width must remain a physical measurement");
  require_near(scene->tracks().front().clip.width.value, 30.0,
               "track clip must match its physical bounds");
  require(scene->tracks().front().z_order == 10,
          "prepared track must retain explicit z order");
  require(scene->curve_layers().size() == 1,
          "prepared scene must contain the configured curve layer");
  require(scene->curve_layers().front().id == layer_id,
          "prepared layer must retain stable identity");
  require(scene->curve_layers().front().curve_id == curve_id,
          "prepared layer must retain its curve identity");
  require(scene->curve_layers().front().scale_id == scale_id,
          "prepared layer must retain its scale identity");
  require(scene->curve_layers().front().z_order == 20,
          "prepared layer must retain explicit z order");
  require(scene->curve_segments().size() == 4,
          "null and non-finite samples must split curve geometry");
  require(scene->curve_points().size() == 6,
          "null and non-finite samples must not enter curve geometry");
  require(scene->curve_segments()[0].point_count == 2 &&
              scene->curve_segments()[1].point_count == 2 &&
              scene->curve_segments()[2].point_count == 1 &&
              scene->curve_segments()[3].point_count == 1,
          "each contiguous valid run must remain a separate segment");
  require(scene->curve_points()[0].sample_index == 0 &&
              scene->curve_points()[1].sample_index == 1 &&
              scene->curve_points()[2].sample_index == 3 &&
              scene->curve_points()[3].sample_index == 4 &&
              scene->curve_points()[4].sample_index == 6 &&
              scene->curve_points()[5].sample_index == 8,
          "prepared geometry must retain original sample indices");
  require_near(scene->curve_points().front().position.left.value, 0.0,
               "scale minimum must map to the track left edge");
  require_near(scene->curve_points().front().position.top.value, 0.0,
               "viewport top depth must map to the scene top edge");
  require_near(scene->curve_points().back().position.left.value, 30.0,
               "scale maximum must map to the track right edge");
  require_near(scene->curve_points().back().position.top.value, 80.0,
               "viewport bottom depth must map to the scene bottom edge");

  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value(), "scene containing missing samples must export");
  require(svg.value().text().find(
              "d=\"M 0 0 L 3.75 10 M 11.25 30 L 15 40 M 22.5 60 M 30 "
              "80\"") != std::string_view::npos,
          "SVG path data must preserve every missing-data break");

  const auto hit = scene->pick_curve(CurvePickQuery{
      .scene_position =
          PhysicalPoint{
              .left = Millimetres{15.1},
              .top = Millimetres{40.1},
          },
      .tolerance = DeviceIndependentPixels{4.0},
      .horizontal_device_independent_pixels_per_millimetre = 4.0,
      .vertical_device_independent_pixels_per_millimetre = 8.0,
  });
  require(hit.has_value(), "curve geometry must be pickable in physical space");
  require(hit->layer_id == layer_id && hit->curve_id == curve_id,
          "pick result must retain stable layer and curve identities");
  require(hit->sample_index == 4,
          "pick result must return the original source sample index");
  require_near(hit->reference_depth, 1004.0,
               "pick result must return Reference Depth");
  require_near(hit->value, 40.0,
               "pick result must return the original curve value");

  require_near(hit->distance.value, std::hypot(0.1 * 4.0, 0.1 * 8.0),
               "pick distance must be reported in device-independent pixels");

  const auto miss = scene->pick_curve(CurvePickQuery{
      .scene_position =
          PhysicalPoint{
              .left = Millimetres{100.0},
              .top = Millimetres{40.0},
          },
      .tolerance = DeviceIndependentPixels{4.0},
      .horizontal_device_independent_pixels_per_millimetre = 4.0,
      .vertical_device_independent_pixels_per_millimetre = 8.0,
  });
  require(!miss.has_value(),
          "picking outside the track clip and tolerance must miss");
}

std::shared_ptr<const PreparedScene>
prepare_directional_scene(AxisDirection direction) {
  const auto document_id = id("20000000-0000-4000-8000-000000000001");
  const auto axis_id = id("20000000-0000-4000-8000-000000000002");
  const auto curve_id = id("20000000-0000-4000-8000-000000000003");
  const auto track_id = id("20000000-0000-4000-8000-000000000004");
  const auto scale_id = id("20000000-0000-4000-8000-000000000005");
  const auto layer_id = id("20000000-0000-4000-8000-000000000006");

  auto depths =
      direction == AxisDirection::increasing
          ? std::make_shared<const std::vector<double>>(
                std::initializer_list<double>{1000.0, 1001.0, 1002.0})
          : std::make_shared<const std::vector<double>>(
                std::initializer_list<double>{1002.0, 1001.0, 1000.0});
  auto values = direction == AxisDirection::increasing
                    ? std::make_shared<const std::vector<double>>(
                          std::initializer_list<double>{0.0, 50.0, 100.0})
                    : std::make_shared<const std::vector<double>>(
                          std::initializer_list<double>{100.0, 50.0, 0.0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{5});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = direction,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "directional source document must be accepted");

  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1002.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation_builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{30.0},
      .z_order = 10,
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{.red = 0x12, .green = 0x34, .blue = 0x56, .alpha = 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 20,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "directional presentation must be accepted");
  return session.prepared_scene(document_id);
}

void svg_is_deterministic_for_each_sampling_axis_direction() {
  const auto increasing_scene =
      prepare_directional_scene(AxisDirection::increasing);
  const auto decreasing_scene =
      prepare_directional_scene(AxisDirection::decreasing);
  require(increasing_scene != nullptr && decreasing_scene != nullptr,
          "both sampling directions must prepare scenes");

  const auto increasing_svg = SvgExporter::write(*increasing_scene);
  const auto decreasing_svg = SvgExporter::write(*decreasing_scene);
  require(increasing_svg.has_value() && decreasing_svg.has_value(),
          "headless SVG export must accept both sampling directions");
  const auto repeated_decreasing_svg = SvgExporter::write(*decreasing_scene);
  require(repeated_decreasing_svg.has_value() &&
              repeated_decreasing_svg.value().text() ==
                  decreasing_svg.value().text(),
          "identical scene and font inputs must export deterministically");

  constexpr std::string_view expected =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"30mm\" "
      "height=\"100mm\" viewBox=\"0 0 30 100\" "
      "data-document-id=\"20000000-0000-4000-8000-000000000001\" "
      "data-document-revision=\"5\" data-font-asset=\"font-fixture-v1\">"
      "<defs><clipPath "
      "id=\"clip-20000000-0000-4000-8000-000000000004\"><rect x=\"0\" "
      "y=\"0\" width=\"30\" height=\"100\"/></clipPath></defs>"
      "<g id=\"track-20000000-0000-4000-8000-000000000004\" "
      "clip-path=\"url(#clip-20000000-0000-4000-8000-000000000004)\" "
      "data-z-order=\"10\"><path "
      "id=\"layer-20000000-0000-4000-8000-000000000006\" "
      "data-curve-id=\"20000000-0000-4000-8000-000000000003\" "
      "data-scale-id=\"20000000-0000-4000-8000-000000000005\" "
      "data-z-order=\"20\" fill=\"none\" stroke=\"#123456\" "
      "stroke-opacity=\"1\" stroke-width=\"0.25\" "
      "d=\"M 0 0 L 15 50 L 30 100\"/></g></svg>";
  require(increasing_svg.value().text() == expected,
          "SVG output must match the reviewed semantic snapshot");
  require(decreasing_svg.value().text().find("d=\"M 30 100 L 15 50 L 0 0\"") !=
              std::string_view::npos,
          "decreasing axes must preserve source sample order and map depth "
          "semantically");
  require(decreasing_scene->curve_points().front().sample_index == 0 &&
              decreasing_scene->curve_points().back().sample_index == 2,
          "decreasing axes must retain original sample ordering");
}

void replacing_a_document_invalidates_its_prepared_scene() {
  const auto document_id = id("30000000-0000-4000-8000-000000000001");
  const auto axis_id = id("30000000-0000-4000-8000-000000000002");
  const auto curve_id = id("30000000-0000-4000-8000-000000000003");
  const auto track_id = id("30000000-0000-4000-8000-000000000004");
  const auto scale_id = id("30000000-0000-4000-8000-000000000005");
  const auto layer_id = id("30000000-0000-4000-8000-000000000006");
  const auto make_document = [&](DocumentRevision revision) {
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1001.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{0.0, 100.0});
    WellLogDocumentBuilder builder(document_id, revision);
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
    return builder.build();
  };

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{make_document(DocumentRevision{1})})
          .has_value(),
      "initial document must be accepted");
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1001.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation_builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 0});
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = {},
      .line_width = Millimetres{0.25},
      .z_order = 0,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "initial presentation must prepare a scene");
  require(session.prepared_scene(document_id) != nullptr,
          "initial prepared scene must be observable");

  require(
      session.execute(SetDocumentCommand{make_document(DocumentRevision{2})})
          .has_value(),
      "replacement document must be accepted");
  require(session.prepared_scene(document_id) == nullptr,
          "document replacement must invalidate the prior prepared revision");
}

void source_index_requires_an_explicit_reference_depth_transform() {
  const auto document_id = id("40000000-0000-4000-8000-000000000001");
  const auto axis_id = id("40000000-0000-4000-8000-000000000002");
  const auto curve_id = id("40000000-0000-4000-8000-000000000003");
  const auto track_id = id("40000000-0000-4000-8000-000000000004");
  const auto scale_id = id("40000000-0000-4000-8000-000000000005");
  const auto layer_id = id("40000000-0000-4000-8000-000000000006");
  auto indices = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 1.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 100.0});
  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{1});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(indices),
      .domain = DepthDomain::source_index,
      .unit = "index",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "source-index document remains valid source data");
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1001.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation_builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 0});
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = {},
      .line_width = Millimetres{0.25},
      .z_order = 0,
      .visible = true,
  });

  const auto result =
      session.execute(SetPresentationCommand{presentation_builder.build()});
  require(!result.has_value() &&
              result.error().code == ErrorCode::invalid_presentation,
          "source-index axes must not masquerade as Reference Depth");
  require(session.prepared_scene(document_id) == nullptr,
          "a rejected depth-domain mapping must not publish a scene");
}

} // namespace

int main() {
  session_prepares_a_physical_curve_track();
  svg_is_deterministic_for_each_sampling_axis_direction();
  replacing_a_document_invalidates_its_prepared_scene();
  source_index_requires_an_explicit_reference_depth_transform();
  std::cout << "PASS: headless SVG behavior\n";
  return EXIT_SUCCESS;
}
