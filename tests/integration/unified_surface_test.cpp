// Unified Surface Canvas integration tests: single well = one-placement
// surface; layouts = N placements. One pan/zoom/reset path, unified surface
// accessors, horizontal virtualization with a stable compose cache, focused
// well state, and DepthTransform-correct multi-well picking.

#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <cmath>
#include <cstdio>
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

void require_near(double actual, double expected, double tol,
                  std::string_view message) {
  if (!(std::isfinite(actual) && std::abs(actual - expected) <= tol)) {
    std::cerr << "FAIL: " << message << " (actual=" << actual
              << " expected=" << expected << ")\n";
    std::_Exit(EXIT_FAILURE); // #241: see fail() — no CRT/DLL teardown
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
};

// A three-sample well over reference depths [1000, 1002] (or a shifted range
// for DepthTransform fixtures). Small enough to prepare synchronously.
WellFixture make_session_well(WellLogSession &session, std::string_view doc,
                              std::string_view axis, std::string_view curve,
                              std::string_view track, std::string_view scale,
                              std::string_view layer, double top_depth,
                              double gr0, double gr1) {
  WellFixture f;
  f.document_id = id(doc);
  f.axis_id = id(axis);
  f.curve_id = id(curve);
  f.track_id = id(track);
  f.scale_id = id(scale);
  f.layer_id = id(layer);
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{top_depth, top_depth + 1.0, top_depth + 2.0});
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
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "fixture SetDocumentCommand");
  ScenePresentationBuilder presentation(
      f.document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = top_depth,
          .bottom = top_depth + 2.0,
      },
      Millimetres{100.0}, "unified-surface-test");
  presentation.add_track(
      TrackSpec{.id = f.track_id, .width = Millimetres{40.0}, .z_order = 0});
  presentation.add_scale(TrackScaleSpec{
      .id = f.scale_id,
      .track_id = f.track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation.add_curve_layer(CurveLayerSpec{
      .id = f.layer_id,
      .track_id = f.track_id,
      .curve_id = f.curve_id,
      .scale_id = f.scale_id,
      .color = RgbaColor{0x11, 0x22, 0x33, 0xff},
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation.build()})
              .has_value(),
          "fixture SetPresentationCommand");
  return f;
}

// --- Single well is a one-placement surface ---------------------------------

void single_well_is_one_placement_surface() {
  WellLogSession session;
  const auto well = make_session_well(session, "a0000000-0000-4000-8000-000000000001",
                                      "a0000000-0000-4000-8000-000000000002",
                                      "a0000000-0000-4000-8000-000000000003",
                                      "a0000000-0000-4000-8000-000000000004",
                                      "a0000000-0000-4000-8000-000000000005",
                                      "a0000000-0000-4000-8000-000000000006",
                                      1000.0, 20.0, 80.0);
  const auto surface = session.prepared_surface_scene();
  require(surface != nullptr, "implicit one-placement surface resolves");
  require(surface == session.prepared_scene(well.document_id),
          "one-placement surface IS the well scene (shared identity)");
  const auto viewport = session.surface_depth_viewport();
  require(viewport.has_value(), "surface viewport resolves for single well");
  require_near(viewport->top, 1000.0, 1e-9, "surface viewport top");
  require_near(viewport->bottom, 1002.0, 1e-9, "surface viewport bottom");

  // Picking through the unified surface path reports the owning document.
  // The fixture curve (20/80/20 over 0..100 scale, 40mm track) places its
  // middle sample at x = 80% of 40mm = 32mm, depth 1001 = scene y 50mm.
  const auto hit = session.pick_surface_curve(CurvePickQuery{
      .scene_position = PhysicalPoint{Millimetres{32.0}, Millimetres{50.0}},
      .tolerance = DeviceIndependentPixels{8.0},
      .horizontal_device_independent_pixels_per_millimetre = 1.0,
      .vertical_device_independent_pixels_per_millimetre = 1.0,
  });
  require(hit.has_value(), "implicit surface pick hits");
  require(hit->document_id == well.document_id,
          "implicit surface pick reports owning document");
  require(hit->curve_id == well.curve_id, "implicit surface pick curve");
  require_near(hit->reference_depth, hit->display_depth, 1e-12,
               "identity transform keeps reference == display");

  const auto stats = session.surface_statistics();
  require(stats.visible_wells == 1, "single well visible");
  require(stats.culled_wells == 0, "single well nothing culled");
  require(session.surface_width_mm().has_value(), "single well width resolves");
  require_near(*session.surface_width_mm(), 40.0, 1e-9,
               "single well surface width");
}

// --- One pan/zoom/reset path for single and multi ---------------------------

void pan_zoom_reset_share_one_path() {
  // Single well.
  {
    WellLogSession session;
    const auto well =
        make_session_well(session, "b0000000-0000-4000-8000-000000000001",
                          "b0000000-0000-4000-8000-000000000002",
                          "b0000000-0000-4000-8000-000000000003",
                          "b0000000-0000-4000-8000-000000000004",
                          "b0000000-0000-4000-8000-000000000005",
                          "b0000000-0000-4000-8000-000000000006",
                          1000.0, 20.0, 80.0);
    require(session
                .execute(PanDepthCommand{.document_id = well.document_id,
                                         .display_depth_delta = 10.0})
                .has_value(),
            "single pan");
    auto viewport = session.surface_depth_viewport();
    require_near(viewport->top, 1010.0, 1e-9, "single pan top");
    require_near(viewport->bottom, 1012.0, 1e-9, "single pan bottom");
    // Cursor-anchored zoom: the anchor depth stays fixed.
    require(session
                .execute(ZoomDepthAtCommand{.document_id = well.document_id,
                                            .anchor_display_depth = 1011.0,
                                            .span_factor = 0.5})
                .has_value(),
            "single zoom");
    viewport = session.surface_depth_viewport();
    require_near(viewport->top, 1010.5, 1e-9, "single zoom top");
    require_near(viewport->bottom, 1011.5, 1e-9, "single zoom bottom");
    require(session
                .execute(ResetViewportCommand{.document_id = well.document_id})
                .has_value(),
            "single reset");
    viewport = session.surface_depth_viewport();
    require_near(viewport->top, 1000.0, 1e-9, "single reset top");
    require_near(viewport->bottom, 1002.0, 1e-9, "single reset bottom");
  }
  // Two-well layout: the SAME commands drive the shared viewport.
  {
    WellLogSession session;
    const auto a =
        make_session_well(session, "c0000000-0000-4000-8000-000000000001",
                          "c0000000-0000-4000-8000-000000000002",
                          "c0000000-0000-4000-8000-000000000003",
                          "c0000000-0000-4000-8000-000000000004",
                          "c0000000-0000-4000-8000-000000000005",
                          "c0000000-0000-4000-8000-000000000006",
                          1000.0, 20.0, 80.0);
    const auto b =
        make_session_well(session, "c0000000-0000-4000-8000-000000000011",
                          "c0000000-0000-4000-8000-000000000012",
                          "c0000000-0000-4000-8000-000000000013",
                          "c0000000-0000-4000-8000-000000000014",
                          "c0000000-0000-4000-8000-000000000015",
                          "c0000000-0000-4000-8000-000000000016",
                          1000.0, 30.0, 70.0);
    require(session
                .execute(SetWellLayoutCommand{
                    .wells = {{.document_id = a.document_id},
                              {.document_id = b.document_id}},
                    .gap = Millimetres{4.0},
                    .pack_left_to_right = true,
                })
                .has_value(),
            "layout");
    require(session
                .execute(SetSharedDepthViewportCommand{
                    .viewport = DepthViewport{.top = 1000.0, .bottom = 1002.0},
                    .pixel_height = 200,
                })
                .has_value(),
            "shared viewport");
    // The per-document getter delegates to the shared window for members.
    require(session.viewport(a.document_id) ==
                std::optional<DepthViewport>{DepthViewport{
                    .top = 1000.0, .bottom = 1002.0}},
            "layout member viewport delegates to shared");
    require(session.surface_depth_viewport() ==
                std::optional<DepthViewport>{DepthViewport{
                    .top = 1000.0, .bottom = 1002.0}},
            "surface viewport equals shared");
    // Pan on either member moves BOTH wells (same command as single well).
    require(session
                .execute(PanDepthCommand{.document_id = b.document_id,
                                         .display_depth_delta = 5.0})
                .has_value(),
            "member pan");
    const auto shifted = DepthViewport{.top = 1005.0, .bottom = 1007.0};
    require(session.shared_depth_viewport() ==
                std::optional<DepthViewport>{shifted},
            "member pan shifts shared");
    require(session.viewport(a.document_id) ==
                std::optional<DepthViewport>{shifted},
            "member pan shifts the other well");
    // Cursor-anchored zoom through the same command path.
    require(session
                .execute(ZoomDepthAtCommand{.document_id = a.document_id,
                                            .anchor_display_depth = 1006.0,
                                            .span_factor = 0.5})
                .has_value(),
            "member zoom");
    const auto zoomed = session.surface_depth_viewport();
    require_near(zoomed->top, 1005.5, 1e-9, "member zoom keeps anchor (top)");
    require_near(zoomed->bottom, 1006.5, 1e-9,
                 "member zoom keeps anchor (bottom)");
    // Reset restores the focused well's default and re-broadcasts it.
    require(session.execute(SetFocusedWellCommand{.document_id = a.document_id})
                .has_value(),
            "focus A");
    require(session
                .execute(ResetViewportCommand{.document_id = a.document_id})
                .has_value(),
            "member reset");
    const auto reset = session.surface_depth_viewport();
    require_near(reset->top, 1000.0, 1e-9, "member reset top");
    require_near(reset->bottom, 1002.0, 1e-9, "member reset bottom");
  }
}

// --- Focused well state ------------------------------------------------------

void focused_well_state_and_events() {
  WellLogSession session;
  make_session_well(session, "d0000000-0000-4000-8000-000000000001",
                    "d0000000-0000-4000-8000-000000000002",
                    "d0000000-0000-4000-8000-000000000003",
                    "d0000000-0000-4000-8000-000000000004",
                    "d0000000-0000-4000-8000-000000000005",
                    "d0000000-0000-4000-8000-000000000006",
                    1000.0, 20.0, 80.0);
  const auto b = make_session_well(session, "d0000000-0000-4000-8000-000000000011",
                                   "d0000000-0000-4000-8000-000000000012",
                                   "d0000000-0000-4000-8000-000000000013",
                                   "d0000000-0000-4000-8000-000000000014",
                                   "d0000000-0000-4000-8000-000000000015",
                                   "d0000000-0000-4000-8000-000000000016",
                                   1000.0, 30.0, 70.0);
  require(!session.focused_well().has_value(), "no focus initially");
  // Two prepared documents without focus: the implicit surface is ambiguous.
  require(session.prepared_surface_scene() == nullptr,
          "ambiguous implicit surface without focus");
  session.clear_events();
  require(session.execute(SetFocusedWellCommand{.document_id = b.document_id})
              .has_value(),
          "focus B");
  require(session.focused_well() == std::optional<EntityId>{b.document_id},
          "focused_well getter");
  require(session.prepared_surface_scene() ==
              session.prepared_scene(b.document_id),
          "implicit surface follows focus");
  const auto &events = session.events();
  require(!events.empty() && events.back().kind ==
                                  ViewEventKind::focused_well_changed,
          "focused_well_changed published");
  require(events.back().document_id == b.document_id, "focus event document");
  // Unknown document rejected.
  require(!session
               .execute(SetFocusedWellCommand{.document_id =
                                                  id("00000000-0000-4000-"
                                                     "8000-000000000099")})
               .has_value(),
          "focus unknown document rejected");
  // Crosshair on the focused well is the surface crosshair (broadcast makes
  // members agree).
  require(session
              .execute(SetCrosshairCommand{
                  .document_id = b.document_id,
                  .crosshair = CrosshairState{.track_fraction = 0.25,
                                              .display_depth = 1001.0},
              })
              .has_value(),
          "set crosshair");
  const auto crosshair = session.surface_crosshair();
  require(crosshair.has_value(), "surface crosshair resolves");
  require_near(crosshair->track_fraction, 0.25, 1e-12,
               "surface crosshair fraction");
  require_near(crosshair->display_depth, 1001.0, 1e-9,
               "surface crosshair depth");
}

// --- Horizontal virtualization ------------------------------------------------

void horizontal_pan_clamps_and_keeps_cache() {
  WellLogSession session;
  constexpr int well_count = 3;
  std::vector<EntityId> wells;
  for (int index = 0; index < well_count; ++index) {
    char doc[40];
    std::snprintf(doc, sizeof(doc),
                  "e0000000-0000-4000-8000-%012d", index + 1);
    char axis[40];
    std::snprintf(axis, sizeof(axis),
                  "e0000000-0000-4000-8000-%012d", index + 11);
    char curve[40];
    std::snprintf(curve, sizeof(curve),
                  "e0000000-0000-4000-8000-%012d", index + 21);
    char track[40];
    std::snprintf(track, sizeof(track),
                  "e0000000-0000-4000-8000-%012d", index + 31);
    char scale[40];
    std::snprintf(scale, sizeof(scale),
                  "e0000000-0000-4000-8000-%012d", index + 41);
    char layer[40];
    std::snprintf(layer, sizeof(layer),
                  "e0000000-0000-4000-8000-%012d", index + 51);
    wells.push_back(
        make_session_well(session, doc, axis, curve, track, scale, layer,
                          1000.0, 20.0 + 10.0 * index, 80.0)
            .document_id);
  }
  require(session
              .execute(SetWellLayoutCommand{
                  .wells = {{.document_id = wells[0]},
                            {.document_id = wells[1]},
                            {.document_id = wells[2]}},
                  .gap = Millimetres{4.0},
                  .pack_left_to_right = true,
              })
              .has_value(),
          "layout");
  // 3 × 40mm wells + 2 × 4mm gaps = 128mm total.
  require_near(*session.surface_width_mm(), 128.0, 1e-9, "surface width");
  require(session
              .execute(SetSurfaceHorizontalViewCommand{.left_mm = 0.0,
                                                       .right_mm = 40.0})
              .has_value(),
          "initial window");
  auto only_first = session.prepared_surface_scene();
  require(only_first != nullptr, "composed with window");
  auto stats = session.surface_statistics();
  require(stats.visible_wells == 1, "one well visible");
  require(stats.culled_wells == 2, "two wells culled");
  require(stats.visible_tracks == 1, "one track visible");

  // Shift the window WITHIN well 0's extent (well 1 starts at 44mm): same
  // cull set → the compose cache must keep the identical scene (no
  // recompose, no GPU re-upload).
  require(session
              .execute(PanSurfaceHorizontalCommand{.delta_mm = 2.0})
              .has_value(),
          "pan inside cull set");
  const auto window = session.surface_horizontal_view();
  require(window.has_value(), "window after pan");
  require_near(window->first, 2.0, 1e-9, "panned window left");
  require_near(window->second, 42.0, 1e-9, "panned window right");
  require(session.prepared_surface_scene() == only_first,
          "same cull set keeps the composed scene identity");

  // Pan far right: clamped so the window never runs past the surface.
  require(session
              .execute(PanSurfaceHorizontalCommand{.delta_mm = 1000.0})
              .has_value(),
          "pan clamps at right edge");
  const auto clamped = session.surface_horizontal_view();
  require_near(clamped->first, 88.0, 1e-9, "clamped left");
  require_near(clamped->second, 128.0, 1e-9, "clamped right");
  // Pan further right: no-op (state unchanged).
  require(session
              .execute(PanSurfaceHorizontalCommand{.delta_mm = 10.0})
              .has_value(),
          "pan past edge succeeds");
  const auto stuck = session.surface_horizontal_view();
  require_near(stuck->first, 88.0, 1e-9, "edge pan is a no-op");
  // Pan far left: clamps at zero.
  require(session
              .execute(PanSurfaceHorizontalCommand{.delta_mm = -1000.0})
              .has_value(),
          "pan clamps at left edge");
  const auto left = session.surface_horizontal_view();
  require_near(left->first, 0.0, 1e-9, "left clamp");
  require_near(left->second, 40.0, 1e-9, "left clamp right");
  // Pan without a window (cleared layout) errors.
  require(session.execute(ClearWellLayoutCommand{}).has_value(), "clear");
  require(!session.execute(PanSurfaceHorizontalCommand{.delta_mm = 1.0})
               .has_value(),
          "pan without layout rejected");
}

// --- DepthTransform-correct multi-well picking --------------------------------

void transformed_wells_pick_per_well_reference_depths() {
  WellLogSession session;
  // Well A: identity, reference/display 1000..1002.
  const auto a = make_session_well(session, "f0000000-0000-4000-8000-000000000001",
                                   "f0000000-0000-4000-8000-000000000002",
                                   "f0000000-0000-4000-8000-000000000003",
                                   "f0000000-0000-4000-8000-000000000004",
                                   "f0000000-0000-4000-8000-000000000005",
                                   "f0000000-0000-4000-8000-000000000006",
                                   1000.0, 20.0, 80.0);
  // Well B: reference 900..902, transformed +100 → display 1000..1002.
  const auto b = make_session_well(session, "f0000000-0000-4000-8000-000000000011",
                                   "f0000000-0000-4000-8000-000000000012",
                                   "f0000000-0000-4000-8000-000000000013",
                                   "f0000000-0000-4000-8000-000000000014",
                                   "f0000000-0000-4000-8000-000000000015",
                                   "f0000000-0000-4000-8000-000000000016",
                                   900.0, 30.0, 70.0);
  // B's presentation range must be expressed in Display Depth space.
  {
    ScenePresentationBuilder presentation(
        b.document_id,
        ReferenceDepthRange{
            .domain = DepthDomain::measured_depth,
            .unit = "m",
            .top = 1000.0,
            .bottom = 1002.0,
        },
        Millimetres{100.0}, "unified-surface-test");
    presentation.add_track(TrackSpec{.id = b.track_id,
                                     .width = Millimetres{40.0},
                                     .z_order = 0});
    presentation.add_scale(TrackScaleSpec{
        .id = b.scale_id,
        .track_id = b.track_id,
        .mode = ScaleMode::linear,
        .minimum = 0.0,
        .maximum = 100.0,
        .direction = ScaleDirection::left_to_right,
        .unit = "API",
    });
    presentation.add_curve_layer(CurveLayerSpec{
        .id = b.layer_id,
        .track_id = b.track_id,
        .curve_id = b.curve_id,
        .scale_id = b.scale_id,
        .color = RgbaColor{0x33, 0x22, 0x11, 0xff},
        .line_width = Millimetres{0.5},
        .z_order = 0,
        .visible = true,
    });
    presentation.set_depth_transform(DepthTransform{
        .control_points =
            {
                {.reference_depth = 900.0, .display_depth = 1000.0},
                {.reference_depth = 902.0, .display_depth = 1002.0},
            },
        .extrapolate = DepthExtrapolatePolicy::linear,
        .version = 1,
    });
    require(session.execute(SetPresentationCommand{presentation.build()})
                .has_value(),
            "transformed presentation");
  }
  require(session.execute(SetDepthTransformCommand{
              .document_id = b.document_id,
              .transform = DepthTransform{
                  .control_points =
                      {
                          {.reference_depth = 900.0, .display_depth = 1000.0},
                          {.reference_depth = 902.0, .display_depth = 1002.0},
                      },
                  .extrapolate = DepthExtrapolatePolicy::linear,
                  .version = 1,
              },
          })
              .has_value(),
          "depth transform command");
  require(session
              .execute(SetWellLayoutCommand{
                  .wells = {{.document_id = a.document_id},
                            {.document_id = b.document_id}},
                  .gap = Millimetres{4.0},
                  .pack_left_to_right = true,
              })
              .has_value(),
          "transformed layout");
  require(session
              .execute(SetSharedDepthViewportCommand{
                  .viewport = DepthViewport{.top = 1000.0, .bottom = 1002.0},
                  .pixel_height = 200,
              })
              .has_value(),
          "shared display window");
  // Display depth 1001 maps to the vertical centre of the 100mm scene.
  const auto pick_at = [&session](double left_mm) {
    return session.pick_surface_curve(CurvePickQuery{
        .scene_position =
            PhysicalPoint{Millimetres{left_mm}, Millimetres{50.0}},
        .tolerance = DeviceIndependentPixels{8.0},
        .horizontal_device_independent_pixels_per_millimetre = 1.0,
        .vertical_device_independent_pixels_per_millimetre = 1.0,
    });
  };
  const auto hit_a = pick_at(32.0);
  require(hit_a.has_value(), "pick over well A");
  require(hit_a->document_id == a.document_id, "pick A owner");
  require_near(hit_a->display_depth, 1001.0, 1e-6, "pick A display depth");
  require_near(hit_a->reference_depth, 1001.0, 1e-6,
               "pick A reference depth (identity)");
  // Well B sits at surface left 44mm (40mm well + 4mm gap); its middle
  // sample (value 70 over 0..100) lands at local x = 28mm → surface 72mm.
  const auto hit_b = pick_at(72.0);
  require(hit_b.has_value(), "pick over well B");
  require(hit_b->document_id == b.document_id, "pick B owner");
  require_near(hit_b->display_depth, 1001.0, 1e-6,
               "pick B display depth matches A");
  require_near(hit_b->reference_depth, 901.0, 1e-6,
               "pick B reference depth inverts the transform");
}

} // namespace

int main() {
  single_well_is_one_placement_surface();
  pan_zoom_reset_share_one_path();
  focused_well_state_and_events();
  horizontal_pan_clamps_and_keeps_cache();
  transformed_wells_pick_per_well_reference_depths();
  std::cout << "unified surface tests passed\n";
  return EXIT_SUCCESS;
}
