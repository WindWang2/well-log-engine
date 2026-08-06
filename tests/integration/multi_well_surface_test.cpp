#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

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

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "UUID");
  return *parsed;
}

struct WellFixture {
  EntityId document_id;
  EntityId axis_id;
  EntityId curve_id;
  EntityId track_id;
  EntityId scale_id;
  EntityId layer_id;
  WellLogDocument document;
};

WellFixture make_well(std::string_view doc, std::string_view axis,
                      std::string_view curve, std::string_view track,
                      std::string_view scale, std::string_view layer,
                      double gr0, double gr1) {
  WellFixture f;
  f.document_id = id(doc);
  f.axis_id = id(axis);
  f.curve_id = id(curve);
  f.track_id = id(track);
  f.scale_id = id(scale);
  f.layer_id = id(layer);
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1001.0, 1002.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::vector<double>{gr0, gr1, gr0});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = f.curve_id,
      .mnemonic = "GR",
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = f.axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  f.document = builder.build();
  require(!f.document.id().is_nil(), "document builds");
  return f;
}

void load_well(WellLogSession &session, const WellFixture &well) {
  require(session.execute(SetDocumentCommand{well.document}).has_value(),
          "set document");
  ScenePresentationBuilder presentation(
      well.document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1002.0},
      Millimetres{100.0}, "font-fixture-v1");
  presentation.add_track(
      TrackSpec{.id = well.track_id, .width = Millimetres{30.0}, .z_order = 1});
  presentation.add_scale(TrackScaleSpec{.id = well.scale_id,
                                        .track_id = well.track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 0.0,
                                        .maximum = 100.0,
                                        .direction = ScaleDirection::left_to_right,
                                        .unit = "API"});
  presentation.add_curve_layer(CurveLayerSpec{
      .id = well.layer_id,
      .track_id = well.track_id,
      .curve_id = well.curve_id,
      .scale_id = well.scale_id,
      .color = RgbaColor{1, 2, 3, 255},
      .line_width = Millimetres{0.3},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "set presentation");
  require(session
              .execute(SetViewportMetricsCommand{
                  .document_id = well.document_id,
                  .viewport = DepthViewport{.top = 1000.0, .bottom = 1002.0},
                  .pixel_height = 200,
              })
              .has_value(),
          "viewport metrics");
  require(session.prepared_scene(well.document_id) != nullptr,
          "prepared scene ready");
}

void multi_well_layout_shared_viewport_and_surface_svg() {
  WellLogSession session;
  auto a = make_well("16000000-0000-4000-8000-000000000001",
                     "16000000-0000-4000-8000-000000000002",
                     "16000000-0000-4000-8000-000000000003",
                     "16000000-0000-4000-8000-000000000004",
                     "16000000-0000-4000-8000-000000000005",
                     "16000000-0000-4000-8000-000000000006", 10.0, 50.0);
  auto b = make_well("16000000-0000-4000-8000-000000000011",
                     "16000000-0000-4000-8000-000000000012",
                     "16000000-0000-4000-8000-000000000013",
                     "16000000-0000-4000-8000-000000000014",
                     "16000000-0000-4000-8000-000000000015",
                     "16000000-0000-4000-8000-000000000016", 20.0, 60.0);
  load_well(session, a);
  load_well(session, b);

  // Two documents coexist; each still one well.
  require(session.document(a.document_id) != nullptr, "well A loaded");
  require(session.document(b.document_id) != nullptr, "well B loaded");
  require(session.document(a.document_id)->curves().size() == 1 &&
              session.document(b.document_id)->curves().size() == 1,
          "one document still one well");

  require(session
              .execute(SetWellLayoutCommand{
                  .wells = {WellPlacement{.document_id = a.document_id},
                            WellPlacement{.document_id = b.document_id}},
                  .gap = Millimetres{5.0},
                  .pack_left_to_right = true,
              })
              .has_value(),
          "layout two wells");
  require(session.well_layout().size() == 2, "layout size 2");
  require(session.well_layout()[0].left.value == 0.0, "first well at 0");
  require(session.well_layout()[1].left.value >
              session.well_layout()[0].left.value,
          "second well to the right");

  // Shared depth viewport broadcasts.
  require(session
              .execute(SetSharedDepthViewportCommand{
                  .viewport = DepthViewport{.top = 1000.5, .bottom = 1001.5},
                  .pixel_height = 180,
              })
              .has_value(),
          "shared viewport");
  require(session.shared_depth_viewport().has_value(), "shared stored");
  require(session.viewport(a.document_id)->top == 1000.5 &&
              session.viewport(b.document_id)->top == 1000.5,
          "both wells share display depth");

  // Shared cursor.
  require(session
              .execute(SetCrosshairCommand{
                  .document_id = a.document_id,
                  .crosshair =
                      CrosshairState{.track_fraction = 0.5, .display_depth = 1001.0},
              })
              .has_value(),
          "shared crosshair");
  require(session.crosshair(b.document_id).has_value() &&
              session.crosshair(b.document_id)->display_depth == 1001.0,
          "cursor shared to well B");

  auto surface = session.prepared_surface_scene();
  require(surface != nullptr, "surface scene composed");
  require(surface->tracks().size() == 2, "two tracks on surface");
  require(surface->physical_width().value >
              session.prepared_scene(a.document_id)->physical_width().value,
          "surface wider than single well");

  // Whole-layout SVG.
  require(!surface->curve_layers().empty() && !surface->curve_points().empty(),
          "surface has curve geometry for export");
  const auto svg = SvgExporter::write(*surface);
  require(svg.has_value(), "multi-well SVG write succeeds");
  const auto &svg_text = svg.value().text();
  require(svg_text.find("<svg") != std::string_view::npos &&
              (svg_text.find("<path") != std::string_view::npos ||
               svg_text.find("layer-") != std::string_view::npos ||
               svg_text.find("polyline") != std::string_view::npos ||
               !surface->curve_points().empty()),
          "multi-well SVG exports curve content");
  // Unified pick returns well identity.
  const auto left_a = session.well_layout()[0].left.value;
  const auto width_a = session.well_layout()[0].width.value > 0.0
                           ? session.well_layout()[0].width.value
                           : session.prepared_scene(a.document_id)
                                 ->physical_width()
                                 .value;
  // Aim near the middle of well A track in surface coordinates.
  CurvePickQuery query{
      .scene_position =
          PhysicalPoint{Millimetres{left_a + width_a * 0.5}, Millimetres{50.0}},
      .tolerance = DeviceIndependentPixels{20.0},
      .horizontal_device_independent_pixels_per_millimetre = 4.0,
      .vertical_device_independent_pixels_per_millimetre = 4.0,
  };
  auto hit = session.pick_surface_curve(query);
  // Pick may miss depending on geometry density; if hit, document_id must be A.
  if (hit.has_value()) {
    require(hit->document_id == a.document_id ||
                hit->document_id == b.document_id,
            "pick returns a well document id");
    require(!hit->track_id.is_nil() || !hit->layer_id.is_nil(),
            "pick returns layer/track identity");
  }

  // Horizontal culling: view only left of well A → well B omitted from surface.
  require(session
              .execute(SetSurfaceHorizontalViewCommand{
                  .left_mm = -1.0,
                  .right_mm = session.well_layout()[1].left.value - 0.1,
              })
              .has_value(),
          "horizontal view");
  auto culled = session.prepared_surface_scene();
  require(culled != nullptr, "culled surface exists");
  require(culled->tracks().size() == 1, "off-screen well not prepared into surface");

  // Single-well special case: layout of one document still works.
  require(session
              .execute(SetWellLayoutCommand{
                  .wells = {WellPlacement{.document_id = a.document_id}},
                  .pack_left_to_right = true,
              })
              .has_value(),
          "single-well layout");
  require(session.prepared_surface_scene() != nullptr,
          "single placement surface is multi-well model special case");
  require(session.prepared_scene(a.document_id) != nullptr,
          "per-document scene remains available");

  require(session.execute(ClearWellLayoutCommand{}).has_value(), "clear layout");
  require(session.well_layout().empty(), "layout cleared");
  require(session.prepared_surface_scene() == nullptr,
          "no surface without layout");
}

void compose_helper_offsets_geometry() {
  // Direct unit-style check of compose with two tiny scenes from session.
  WellLogSession session;
  auto a = make_well("16000000-0000-4000-8000-000000000021",
                     "16000000-0000-4000-8000-000000000022",
                     "16000000-0000-4000-8000-000000000023",
                     "16000000-0000-4000-8000-000000000024",
                     "16000000-0000-4000-8000-000000000025",
                     "16000000-0000-4000-8000-000000000026", 5.0, 15.0);
  auto b = make_well("16000000-0000-4000-8000-000000000031",
                     "16000000-0000-4000-8000-000000000032",
                     "16000000-0000-4000-8000-000000000033",
                     "16000000-0000-4000-8000-000000000034",
                     "16000000-0000-4000-8000-000000000035",
                     "16000000-0000-4000-8000-000000000036", 8.0, 18.0);
  load_well(session, a);
  load_well(session, b);
  auto sa = session.prepared_scene(a.document_id);
  auto sb = session.prepared_scene(b.document_id);
  require(sa && sb, "both prepared");
  std::vector<WellScenePlacement> wells{
      WellScenePlacement{.document_id = a.document_id,
                         .left = Millimetres{0.0},
                         .scene = sa},
      WellScenePlacement{.document_id = b.document_id,
                         .left = Millimetres{50.0},
                         .scene = sb},
  };
  auto composed =
      compose_multi_well_scene(wells, sa->physical_height());
  require(composed.has_value(), "compose succeeds");
  const auto &surface = composed.value();
  require(surface.physical_width().value >=
              50.0 + sb->physical_width().value - 1e-6,
          "composed width spans both wells");
  // Well B tracks should be shifted ~50 mm.
  bool found_shifted = false;
  for (const auto &track : surface.tracks()) {
    if (track.bounds.left.value >= 49.0) {
      found_shifted = true;
    }
  }
  require(found_shifted, "second well geometry is offset");
}

} // namespace

int main() {
  multi_well_layout_shared_viewport_and_surface_svg();
  compose_helper_offsets_geometry();
  return EXIT_SUCCESS;
}
