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

// --- Pure DepthTransform unit-style tests ----------------------------------

void depth_transform_identity_and_round_trip() {
  DepthTransform identity{};
  require(!validate_depth_transform(identity).has_value(), "empty ok");
  require_near(map_reference_to_display(identity, 1234.5), 1234.5, 1e-12,
               "identity forward");
  require_near(map_display_to_reference(identity, 1234.5), 1234.5, 1e-12,
               "identity inverse");

  // Whole-baseline shift: display = reference - 100.
  DepthTransform shift{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 900.0},
              {.reference_depth = 2000.0, .display_depth = 1900.0},
          },
      .extrapolate = DepthExtrapolatePolicy::linear,
      .version = 1,
  };
  require(!validate_depth_transform(shift).has_value(), "shift valid");
  require_near(map_reference_to_display(shift, 1500.0), 1400.0, 1e-9,
               "shift mid");
  require_near(map_display_to_reference(shift, 1400.0), 1500.0, 1e-9,
               "shift inverse");

  // Multi-marker piecewise stretch.
  DepthTransform stretch{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 1000.0},
              {.reference_depth = 1100.0, .display_depth = 1200.0},
              {.reference_depth = 1300.0, .display_depth = 1300.0},
          },
      .extrapolate = DepthExtrapolatePolicy::linear,
      .version = 2,
  };
  require(!validate_depth_transform(stretch).has_value(), "stretch valid");
  // Mid first segment: ref 1050 → display 1100.
  require_near(map_reference_to_display(stretch, 1050.0), 1100.0, 1e-9,
               "stretch first segment");
  require_near(map_display_to_reference(stretch, 1100.0), 1050.0, 1e-9,
               "stretch first inverse");
  // Mid second segment: ref 1200 → display 1250.
  require_near(map_reference_to_display(stretch, 1200.0), 1250.0, 1e-9,
               "stretch second segment");
  require_near(map_display_to_reference(stretch, 1250.0), 1200.0, 1e-9,
               "stretch second inverse");

  // Property: round-trip over dense samples inside the control span.
  for (int i = 0; i <= 100; ++i) {
    const auto ref = 1000.0 + i * 3.0;
    const auto display = map_reference_to_display(stretch, ref);
    const auto back = map_display_to_reference(stretch, display);
    require_near(back, ref, 1e-8, "round-trip property");
  }
}

void depth_transform_rejects_conflicts() {
  // Display direction change (fold): decreasing then increasing is NOT a
  // valid transform (a uniformly decreasing display IS valid — the TVDSS
  // domain, covered by tvdss_projection_test).
  DepthTransform fold{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 1000.0},
              {.reference_depth = 1100.0, .display_depth = 900.0},
              {.reference_depth = 1200.0, .display_depth = 1000.0},
          },
  };
  require(validate_depth_transform(fold).has_value(), "fold rejected");

  // Uniformly decreasing display (TVDSS) is accepted.
  DepthTransform decreasing{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 10.0},
              {.reference_depth = 1100.0, .display_depth = -300.0},
          },
  };
  require(!validate_depth_transform(decreasing).has_value(),
          "decreasing display (TVDSS) is valid");

  // Duplicate reference.
  DepthTransform dup{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 1000.0},
              {.reference_depth = 1000.0, .display_depth = 1100.0},
          },
  };
  require(validate_depth_transform(dup).has_value(), "dup ref rejected");

  // Single control point.
  DepthTransform single{
      .control_points = {{.reference_depth = 1000.0, .display_depth = 1000.0}},
  };
  require(validate_depth_transform(single).has_value(), "single rejected");

  // NaN.
  DepthTransform nan_pt{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 1000.0},
              {.reference_depth = 1100.0,
               .display_depth = std::numeric_limits<double>::quiet_NaN()},
          },
  };
  require(validate_depth_transform(nan_pt).has_value(), "nan rejected");

  // Aligning markers with a direction-changing target display fails.
  // (A uniformly decreasing target — TVDSS — is valid; see below.)
  const double src[] = {1000.0, 1100.0, 1200.0};
  const double tgt[] = {1200.0, 1100.0, 1300.0};
  require(!depth_transform_aligning_markers(src, tgt).has_value(),
          "align conflict rejected");

  // Uniformly decreasing target display (TVDSS-style) aligns fine.
  const double tgt_decreasing[] = {1200.0, 1100.0, 1000.0};
  require(depth_transform_aligning_markers(src, tgt_decreasing).has_value(),
          "align decreasing display (TVDSS) succeeds");
}

void depth_transform_clamp_and_linear_extrapolate() {
  DepthTransform xform{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 1000.0},
              {.reference_depth = 1100.0, .display_depth = 1200.0},
          },
      .extrapolate = DepthExtrapolatePolicy::clamp,
  };
  require_near(map_reference_to_display(xform, 900.0), 1000.0, 1e-12,
               "clamp below");
  require_near(map_reference_to_display(xform, 1500.0), 1200.0, 1e-12,
               "clamp above");

  xform.extrapolate = DepthExtrapolatePolicy::linear;
  // Slope = 2.0 display per reference.
  require_near(map_reference_to_display(xform, 900.0), 800.0, 1e-9,
               "linear below");
  require_near(map_reference_to_display(xform, 1200.0), 1400.0, 1e-9,
               "linear above");
}

// --- Session: transform + marker align + overlays --------------------------

struct WellWithMarkers {
  EntityId document_id;
  EntityId axis_id;
  EntityId curve_id;
  EntityId track_id;
  EntityId scale_id;
  EntityId layer_id;
  EntityId marker_top_id;
  EntityId marker_bottom_id;
  EntityId marker_layer_id;
  WellLogDocument document;
  double marker_top_ref{};
  double marker_bottom_ref{};
};

WellWithMarkers make_well_with_markers(
    std::string_view doc, std::string_view axis, std::string_view curve,
    std::string_view track, std::string_view scale, std::string_view layer,
    std::string_view m_top, std::string_view m_bot, std::string_view m_layer,
    double gr0, double gr1, double top_ref, double bot_ref,
    double axis_start, double axis_end) {
  WellWithMarkers f;
  f.document_id = id(doc);
  f.axis_id = id(axis);
  f.curve_id = id(curve);
  f.track_id = id(track);
  f.scale_id = id(scale);
  f.layer_id = id(layer);
  f.marker_top_id = id(m_top);
  f.marker_bottom_id = id(m_bot);
  f.marker_layer_id = id(m_layer);
  f.marker_top_ref = top_ref;
  f.marker_bottom_ref = bot_ref;

  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{axis_start, (axis_start + axis_end) * 0.5, axis_end});
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
  builder.add_marker(Marker{
      .id = f.marker_top_id,
      .reference_depth = top_ref,
      .semantic = MarkerSemantic::formation_top,
      .label = "Top",
  });
  builder.add_marker(Marker{
      .id = f.marker_bottom_id,
      .reference_depth = bot_ref,
      .semantic = MarkerSemantic::formation_top,
      .label = "Base",
  });
  f.document = builder.build();
  require(!f.document.id().is_nil(), "document builds");
  return f;
}

void load_well_display_range(WellLogSession &session, const WellWithMarkers &well,
                             double display_top, double display_bottom) {
  require(session.execute(SetDocumentCommand{well.document}).has_value(),
          "set document");
  ScenePresentationBuilder presentation(
      well.document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = display_top,
                          .bottom = display_bottom},
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
  presentation.add_marker_layer(MarkerLayerSpec{
      .id = well.marker_layer_id,
      .track_id = well.track_id,
      .z_order = 10,
      .line_color = RgbaColor{200, 0, 0, 255},
      .line_width = Millimetres{0.4},
      .draw_labels = false,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "set presentation");
  require(session
              .execute(SetViewportMetricsCommand{
                  .document_id = well.document_id,
                  .viewport = DepthViewport{.top = display_top,
                                            .bottom = display_bottom},
                  .pixel_height = 200,
              })
              .has_value(),
          "viewport metrics");
  require(session.prepared_scene(well.document_id) != nullptr,
          "prepared scene ready");
}

void set_depth_transform_rebuilds_curves_with_display_depth() {
  WellLogSession session;
  auto well = make_well_with_markers(
      "16100000-0000-4000-8000-0000000000a1",
      "16100000-0000-4000-8000-0000000000a2",
      "16100000-0000-4000-8000-0000000000a3",
      "16100000-0000-4000-8000-0000000000a4",
      "16100000-0000-4000-8000-0000000000a5",
      "16100000-0000-4000-8000-0000000000a6",
      "16100000-0000-4000-8000-0000000000a7",
      "16100000-0000-4000-8000-0000000000a8",
      "16100000-0000-4000-8000-0000000000a9", 10.0, 50.0, 1000.0, 1200.0, 1000.0,
      1200.0);
  // Presentation depth range is Display Depth space [900, 1100] after a -100
  // shift of the well's MD samples.
  load_well_display_range(session, well, 900.0, 1100.0);

  DepthTransform shift{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 900.0},
              {.reference_depth = 1200.0, .display_depth = 1100.0},
          },
      .version = 1,
  };
  require(session
              .execute(SetDepthTransformCommand{.document_id = well.document_id,
                                                .transform = shift})
              .has_value(),
          "set depth transform");
  const auto stored = session.depth_transform(well.document_id);
  require(stored.control_points.size() == 2, "transform stored");
  require(stored.version == 1, "version stored");

  auto scene = session.prepared_scene(well.document_id);
  require(scene != nullptr, "scene after transform");
  require(!scene->curve_points().empty(), "has curve points");
  bool saw_display = false;
  for (const auto &pt : scene->curve_points()) {
    // Samples at ref 1000 → display 900, mid → 1000, 1200 → 1100.
    require(std::isfinite(pt.display_depth), "display depth finite");
    require_near(pt.display_depth,
                 map_reference_to_display(shift, pt.reference_depth), 1e-9,
                 "point display matches map");
    if (std::abs(pt.reference_depth - 1000.0) < 1e-9) {
      require_near(pt.display_depth, 900.0, 1e-9, "first sample display");
      saw_display = true;
    }
  }
  require(saw_display, "found reference 1000 sample");

  // Reject conflict via session.
  require(!session
               .execute(SetDepthTransformCommand{
                   .document_id = well.document_id,
                   .transform =
                       DepthTransform{
                           .control_points =
                               {
                                   {.reference_depth = 1000.0,
                                    .display_depth = 1000.0},
                                   {.reference_depth = 1100.0,
                                    .display_depth = 900.0},
                                   {.reference_depth = 1200.0,
                                    .display_depth = 1000.0},
                               },
                       },
               })
               .has_value(),
          "session rejects fold");
}

void align_wells_to_markers_shares_display_depth() {
  WellLogSession session;
  // Target well: markers at 1000 and 1200 (identity).
  auto target = make_well_with_markers(
      "16100000-0000-4000-8000-0000000000b1",
      "16100000-0000-4000-8000-0000000000b2",
      "16100000-0000-4000-8000-0000000000b3",
      "16100000-0000-4000-8000-0000000000b4",
      "16100000-0000-4000-8000-0000000000b5",
      "16100000-0000-4000-8000-0000000000b6",
      "16100000-0000-4000-8000-0000000000b7",
      "16100000-0000-4000-8000-0000000000b8",
      "16100000-0000-4000-8000-0000000000b9", 10.0, 40.0, 1000.0, 1200.0, 1000.0,
      1200.0);
  // Source well: same formations at deeper MD 1050 / 1250.
  auto source = make_well_with_markers(
      "16100000-0000-4000-8000-0000000000c1",
      "16100000-0000-4000-8000-0000000000c2",
      "16100000-0000-4000-8000-0000000000c3",
      "16100000-0000-4000-8000-0000000000c4",
      "16100000-0000-4000-8000-0000000000c5",
      "16100000-0000-4000-8000-0000000000c6",
      "16100000-0000-4000-8000-0000000000c7",
      "16100000-0000-4000-8000-0000000000c8",
      "16100000-0000-4000-8000-0000000000c9", 20.0, 60.0, 1050.0, 1250.0, 1050.0,
      1250.0);

  // Load both into a shared Display Depth window [1000, 1200].
  load_well_display_range(session, target, 1000.0, 1200.0);
  load_well_display_range(session, source, 1000.0, 1200.0);

  require(session
              .execute(AlignWellsToMarkersCommand{
                  .target_document_id = target.document_id,
                  .target_marker_ids = {target.marker_top_id,
                                       target.marker_bottom_id},
                  .wells =
                      {
                          AlignWellsToMarkersCommand::WellMarkers{
                              .document_id = source.document_id,
                              .marker_ids = {source.marker_top_id,
                                             source.marker_bottom_id},
                          },
                      },
                  .shared_viewport =
                      DepthViewport{.top = 1000.0, .bottom = 1200.0},
                  .pixel_height = 200,
              })
              .has_value(),
          "align wells");

  // Target keeps identity.
  require(session.depth_transform(target.document_id).control_points.empty(),
          "target identity");
  // Source maps 1050→1000, 1250→1200.
  const auto xform = session.depth_transform(source.document_id);
  require(xform.control_points.size() == 2, "source has 2 control points");
  require_near(map_reference_to_display(xform, 1050.0), 1000.0, 1e-9,
               "source top aligned");
  require_near(map_reference_to_display(xform, 1250.0), 1200.0, 1e-9,
               "source base aligned");

  require(session.shared_depth_viewport().has_value() &&
              session.shared_depth_viewport()->top == 1000.0,
          "shared viewport set");
  require(session.well_layout().size() == 2, "layout auto-created");

  auto src_scene = session.prepared_scene(source.document_id);
  require(src_scene != nullptr, "source scene after align");
  // Marker display tops should match target's display tops under the shared
  // window (both markers map onto 1000 and 1200 display → top/bottom of scene).
  bool found_top = false;
  bool found_bot = false;
  for (const auto &m : src_scene->markers()) {
    if (m.marker_id == source.marker_top_id) {
      require_near(m.display_top.value, 0.0, 1e-6,
                   "aligned top marker at scene top");
      found_top = true;
    }
    if (m.marker_id == source.marker_bottom_id) {
      require_near(m.display_top.value, 100.0, 1e-6,
                   "aligned base marker at scene bottom");
      found_bot = true;
    }
  }
  require(found_top && found_bot, "source markers present after align");
}

void cross_well_overlay_on_surface_svg() {
  WellLogSession session;
  auto left = make_well_with_markers(
      "16100000-0000-4000-8000-0000000000d1",
      "16100000-0000-4000-8000-0000000000d2",
      "16100000-0000-4000-8000-0000000000d3",
      "16100000-0000-4000-8000-0000000000d4",
      "16100000-0000-4000-8000-0000000000d5",
      "16100000-0000-4000-8000-0000000000d6",
      "16100000-0000-4000-8000-0000000000d7",
      "16100000-0000-4000-8000-0000000000d8",
      "16100000-0000-4000-8000-0000000000d9", 15.0, 55.0, 1000.0, 1100.0, 1000.0,
      1100.0);
  auto right = make_well_with_markers(
      "16100000-0000-4000-8000-0000000000e1",
      "16100000-0000-4000-8000-0000000000e2",
      "16100000-0000-4000-8000-0000000000e3",
      "16100000-0000-4000-8000-0000000000e4",
      "16100000-0000-4000-8000-0000000000e5",
      "16100000-0000-4000-8000-0000000000e6",
      "16100000-0000-4000-8000-0000000000e7",
      "16100000-0000-4000-8000-0000000000e8",
      "16100000-0000-4000-8000-0000000000e9", 25.0, 65.0, 1000.0, 1100.0, 1000.0,
      1100.0);
  load_well_display_range(session, left, 1000.0, 1100.0);
  load_well_display_range(session, right, 1000.0, 1100.0);

  require(session
              .execute(SetWellLayoutCommand{
                  .wells = {WellPlacement{.document_id = left.document_id},
                            WellPlacement{.document_id = right.document_id}},
                  .gap = Millimetres{8.0},
                  .pack_left_to_right = true,
              })
              .has_value(),
          "layout");

  const auto horizon_id = id("16100000-0000-4000-8000-0000000000f1");
  const auto band_id = id("16100000-0000-4000-8000-0000000000f2");
  require(session
              .execute(SetCrossWellOverlaysCommand{
                  .overlays =
                      {
                          CrossWellOverlay{
                              .id = horizon_id,
                              .kind = CrossWellOverlay::Kind::horizon_line,
                              .left_document_id = left.document_id,
                              .right_document_id = right.document_id,
                              .left_marker_id = left.marker_top_id,
                              .right_marker_id = right.marker_top_id,
                              .color = RgbaColor{255, 128, 0, 255},
                              .line_width = Millimetres{0.5},
                              .z_order = 80,
                          },
                          CrossWellOverlay{
                              .id = band_id,
                              .kind = CrossWellOverlay::Kind::correlation_band,
                              .left_document_id = left.document_id,
                              .right_document_id = right.document_id,
                              .left_marker_id = left.marker_top_id,
                              .right_marker_id = right.marker_top_id,
                              .left_bottom_marker_id = left.marker_bottom_id,
                              .right_bottom_marker_id = right.marker_bottom_id,
                              .color = RgbaColor{0, 128, 255, 60},
                              .z_order = 40,
                          },
                      },
              })
              .has_value(),
          "set overlays");
  require(session.cross_well_overlays().size() == 2, "overlays stored");
  require(session.cross_well_overlays()[0].left_marker_id == left.marker_top_id,
          "overlay references stable marker entity id");

  auto surface = session.prepared_surface_scene();
  require(surface != nullptr, "surface with overlays");
  require(!surface->custom_layers().empty(), "overlay custom layer present");
  require(surface->custom_primitives().size() >= 2,
          "horizon polyline + band quad");

  bool found_horizon = false;
  bool found_band = false;
  for (const auto &prim : surface->custom_primitives()) {
    if (prim.source_id == horizon_id) {
      require(prim.kind == CustomPrimitiveKind::polyline, "horizon is polyline");
      require(prim.vertex_count == 2, "horizon two endpoints");
      found_horizon = true;
    }
    if (prim.source_id == band_id) {
      require(prim.kind == CustomPrimitiveKind::quad, "band is quad");
      require(prim.vertex_count == 6, "band two triangles");
      found_band = true;
    }
  }
  require(found_horizon && found_band, "both overlays in surface geometry");

  const auto svg = SvgExporter::write(*surface);
  require(svg.has_value(), "surface SVG with overlays");
  const auto &text = svg.value().text();
  require(text.find("<svg") != std::string_view::npos, "svg root");
  // Overlay id is emitted as data-custom-source-id on custom path elements.
  require(text.find(horizon_id.to_string()) != std::string_view::npos,
          "horizon entity id in SVG");
  require(text.find(band_id.to_string()) != std::string_view::npos,
          "band entity id in SVG");

  // Clear overlays → no custom overlay layer required (may still be empty).
  require(session.execute(SetCrossWellOverlaysCommand{.overlays = {}}).has_value(),
          "clear overlays");
  require(session.cross_well_overlays().empty(), "overlays cleared");
}

void domain_mismatch_diagnostic_under_transform() {
  WellLogSession session;
  auto well = make_well_with_markers(
      "16100000-0000-4000-8000-000000000071",
      "16100000-0000-4000-8000-000000000072",
      "16100000-0000-4000-8000-000000000073",
      "16100000-0000-4000-8000-000000000074",
      "16100000-0000-4000-8000-000000000075",
      "16100000-0000-4000-8000-000000000076",
      "16100000-0000-4000-8000-000000000077",
      "16100000-0000-4000-8000-000000000078",
      "16100000-0000-4000-8000-000000000079", 10.0, 50.0, 1000.0, 1100.0, 1000.0,
      1100.0);
  // Document axis is MD; presentation claims TVD domain → should fail when
  // a non-identity transform is applied (no silent conversion).
  require(session.execute(SetDocumentCommand{well.document}).has_value(),
          "set document");
  ScenePresentationBuilder presentation(
      well.document_id,
      ReferenceDepthRange{.domain = DepthDomain::true_vertical_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1100.0},
      Millimetres{100.0}, "font-fixture-v1");
  presentation.add_track(
      TrackSpec{.id = well.track_id, .width = Millimetres{30.0}, .z_order = 1});
  presentation.add_scale(TrackScaleSpec{.id = well.scale_id,
                                        .track_id = well.track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 0.0,
                                        .maximum = 100.0,
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
  // Identity first may or may not enforce domain; force non-identity transform
  // on the presentation before set.
  presentation.set_depth_transform(DepthTransform{
      .control_points =
          {
              {.reference_depth = 1000.0, .display_depth = 1000.0},
              {.reference_depth = 1100.0, .display_depth = 1100.0},
          },
      .version = 1,
  });
  const auto result =
      session.execute(SetPresentationCommand{presentation.build()});
  require(!result.has_value(),
          "domain mismatch under transform yields diagnostic");
}

} // namespace

int main() {
  depth_transform_identity_and_round_trip();
  depth_transform_rejects_conflicts();
  depth_transform_clamp_and_linear_extrapolate();
  set_depth_transform_rebuilds_curves_with_display_depth();
  align_wells_to_markers_shares_display_depth();
  cross_well_overlay_on_surface_svg();
  domain_mismatch_diagnostic_under_transform();
  return EXIT_SUCCESS;
}
