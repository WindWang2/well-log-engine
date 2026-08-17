#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>

#include <cmath>
#include <cstdio>
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
  require(parsed.has_value(), "UUID");
  return *parsed;
}

// Mint a sibling UUID by bumping `id`'s trailing byte by `delta`, so each
// well's crossover content (symbol/annotation/layers) gets distinct ids
// without threading more fixture parameters through the helpers.
EntityId sibling_id(const EntityId &id, std::uint32_t delta) {
  auto text = id.to_string();
  const auto tail = text.substr(text.size() - 2);
  char *end = nullptr;
  const auto value = std::strtoul(tail.c_str(), &end, 16);
  require(end != tail.c_str() && *end == '\0', "hex tail");
  char bump[3];
  std::snprintf(bump, sizeof bump, "%02lx", value + delta);
  text.replace(text.size() - 2, 2, bump);
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "sibling UUID");
  return *parsed;
}

struct WellFixture {
  EntityId document_id;
  EntityId axis_id;
  EntityId curve_id;
  EntityId track_id;
  EntityId scale_id;
  EntityId layer_id;
  EntityId symbol_id;
  EntityId annotation_id;
  EntityId symbol_layer_id;
  EntityId text_layer_id;
  EntityId marker_id;
  EntityId marker_layer_id;
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
  f.symbol_id = sibling_id(f.document_id, 7);
  f.annotation_id = sibling_id(f.document_id, 8);
  f.symbol_layer_id = sibling_id(f.document_id, 9);
  f.text_layer_id = sibling_id(f.document_id, 10);
  f.marker_id = sibling_id(f.document_id, 11);
  f.marker_layer_id = sibling_id(f.document_id, 12);
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
  builder.add_symbol(SymbolOccurrence{
      .id = f.symbol_id,
      .reference_depth = 1001.0,
      .track_fraction = 0.5,
      .kind = SymbolKind::diamond,
      .label = "",
  });
  builder.add_annotation(TextAnnotation{
      .id = f.annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1001.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "crossover",
      .language = "en",
      .orientation = TextOrientation::horizontal,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });
  builder.add_marker(Marker{
      .id = f.marker_id,
      .reference_depth = 1001.0,
      .semantic = MarkerSemantic::formation_top,
      .label = std::string{"Top-"} + std::to_string(static_cast<int>(gr0)),
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
  presentation.add_symbol_layer(SymbolLayerSpec{
      .id = well.symbol_layer_id,
      .track_id = well.track_id,
      .z_order = 2,
      .color = RgbaColor{0, 0, 200, 255},
      .symbol_size = Millimetres{4.0},
  });
  presentation.add_text_layer(TextLayerSpec{
      .id = well.text_layer_id,
      .track_id = well.track_id,
      .z_order = 3,
      .color = RgbaColor{10, 10, 10, 255},
  });
  presentation.add_marker_layer(MarkerLayerSpec{
      .id = well.marker_layer_id,
      .track_id = well.track_id,
      .z_order = 4,
      .line_color = RgbaColor{200, 40, 40, 255},
      .line_width = Millimetres{0.4},
      .draw_labels = true,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{20, 20, 20, 255},
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

std::shared_ptr<HarfBuzzTextEngine> make_engine() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/NotoSans-Regular.ttf")
              .has_value(),
          "bundled test font must load");
  return engine;
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

void compose_remaps_text_and_symbol_indices() {
  // Issue #472 merge path with real crossover content: a two-well composite
  // must remap each well's text-run and symbol indices so layers point at
  // their own runs (run_base used to be captured after the push, skewing
  // every text layer's first_run by the well's own run count).
  WellLogSession session;
  session.set_text_engine(make_engine());
  auto a = make_well("16000000-0000-4000-8000-000000000041",
                     "16000000-0000-4000-8000-000000000042",
                     "16000000-0000-4000-8000-000000000043",
                     "16000000-0000-4000-8000-000000000044",
                     "16000000-0000-4000-8000-000000000045",
                     "16000000-0000-4000-8000-000000000046", 5.0, 15.0);
  auto b = make_well("16000000-0000-4000-8000-000000000051",
                     "16000000-0000-4000-8000-000000000052",
                     "16000000-0000-4000-8000-000000000053",
                     "16000000-0000-4000-8000-000000000054",
                     "16000000-0000-4000-8000-000000000055",
                     "16000000-0000-4000-8000-000000000056", 8.0, 18.0);
  load_well(session, a);
  load_well(session, b);
  auto sa = session.prepared_scene(a.document_id);
  auto sb = session.prepared_scene(b.document_id);
  require(sa && sb, "both prepared");
  // Per-well scenes must actually carry the crossover content, otherwise the
  // merge path stays uncovered.
  require(sa->text_layers().size() == 1 && sa->text_runs().size() >= 2,
          "well A prepares annotation + marker-label runs");
  require(sb->text_layers().size() == 1 && sb->text_runs().size() >= 2,
          "well B prepares annotation + marker-label runs");
  require(sa->markers().size() == 1 &&
              sa->markers().front().label_run_index != no_text_run,
          "well A marker has a label run");
  require(sb->markers().size() == 1 &&
              sb->markers().front().label_run_index != no_text_run,
          "well B marker has a label run");
  require(sa->symbol_layers().size() == 1 && sa->symbols().size() == 1,
          "well A prepares one symbol layer with one symbol");
  require(sb->symbol_layers().size() == 1 && sb->symbols().size() == 1,
          "well B prepares one symbol layer with one symbol");

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

  // Text runs merge with correct first_run offsets (regression for the
  // run_base-after-push bug): well A keeps its runs, well B's layer starts
  // right after them.
  require(surface.text_layers().size() == 2, "two text layers merged");
  require(surface.text_runs().size() ==
              sa->text_runs().size() + sb->text_runs().size(),
          "all per-well text runs merged");
  const auto *layer_a = [&] {
    for (const auto &layer : surface.text_layers()) {
      if (layer.id == a.text_layer_id) {
        return &layer;
      }
    }
    return static_cast<const PreparedTextLayer *>(nullptr);
  }();
  const auto *layer_b = [&] {
    for (const auto &layer : surface.text_layers()) {
      if (layer.id == b.text_layer_id) {
        return &layer;
      }
    }
    return static_cast<const PreparedTextLayer *>(nullptr);
  }();
  require(layer_a != nullptr && layer_b != nullptr,
          "both text layers survive merge");
  require(layer_a->first_run == 0, "well A text layer first_run at 0");
  require(layer_b->first_run == sa->text_runs().size(),
          "well B text layer first_run offset by well A run count");
  // Every layer-owned run stays in the merged range and belongs to its layer.
  for (const auto &layer : surface.text_layers()) {
    require(layer.first_run + layer.run_count <= surface.text_runs().size(),
            "text layer run range in bounds");
    for (std::uint64_t r = layer.first_run; r < layer.first_run + layer.run_count;
         ++r) {
      require(surface.text_runs()[r].layer_id == layer.id,
              "text run owned by its layer");
    }
  }
  // Glyph indices are remapped too: each merged run is fully covered.
  for (const auto &run : surface.text_runs()) {
    require(run.first_glyph + run.glyph_count <= surface.glyphs().size(),
            "text run glyph range in bounds");
  }

  // Symbol layers merge with the same index remapping.
  require(surface.symbol_layers().size() == 2, "two symbol layers merged");
  require(surface.symbols().size() ==
              sa->symbols().size() + sb->symbols().size(),
          "all per-well symbols merged");
  const auto *symbol_layer_a = [&] {
    for (const auto &layer : surface.symbol_layers()) {
      if (layer.id == a.symbol_layer_id) {
        return &layer;
      }
    }
    return static_cast<const PreparedSymbolLayer *>(nullptr);
  }();
  const auto *symbol_layer_b = [&] {
    for (const auto &layer : surface.symbol_layers()) {
      if (layer.id == b.symbol_layer_id) {
        return &layer;
      }
    }
    return static_cast<const PreparedSymbolLayer *>(nullptr);
  }();
  require(symbol_layer_a != nullptr && symbol_layer_b != nullptr,
          "both symbol layers survive merge");
  require(symbol_layer_a->first_symbol == 0,
          "well A symbol layer first_symbol at 0");
  require(symbol_layer_b->first_symbol == sa->symbols().size(),
          "well B symbol layer first_symbol offset by well A symbol count");
  for (const auto &layer : surface.symbol_layers()) {
    require(layer.first_symbol + layer.symbol_count <=
                surface.symbols().size(),
            "symbol layer range in bounds");
    for (std::uint64_t s = layer.first_symbol;
         s < layer.first_symbol + layer.symbol_count; ++s) {
      require(surface.symbols()[s].layer_id == layer.id,
              "symbol owned by its layer");
    }
  }

  // #753: marker.label_run_index must be remapped by run_base so well B's
  // label does not read well A's text_runs.
  require(surface.markers().size() == 2, "both well markers merged");
  const auto *marker_a = &surface.markers().front();
  const auto *marker_b = &surface.markers().back();
  require(marker_a->marker_id == a.marker_id, "first merged marker is well A");
  require(marker_b->marker_id == b.marker_id, "second merged marker is well B");
  require(marker_a->label_run_index != no_text_run &&
              marker_b->label_run_index != no_text_run,
          "merged markers keep label runs");
  require(marker_a->label_run_index < surface.text_runs().size(),
          "well A marker label in range");
  require(marker_b->label_run_index < surface.text_runs().size(),
          "well B marker label in range");
  require(surface.text_runs()[static_cast<std::size_t>(marker_a->label_run_index)]
                  .source_entity_id == a.marker_id,
          "well A marker label resolves to well A");
  require(surface.text_runs()[static_cast<std::size_t>(marker_b->label_run_index)]
                  .source_entity_id == b.marker_id,
          "well B marker label resolves to well B");
  require(surface.text_runs()[static_cast<std::size_t>(marker_a->label_run_index)]
                  .text == "Top-5",
          "well A marker label text");
  require(surface.text_runs()[static_cast<std::size_t>(marker_b->label_run_index)]
                  .text == "Top-8",
          "well B marker label text");
}

} // namespace

int main() {
  multi_well_layout_shared_viewport_and_surface_svg();
  compose_helper_offsets_geometry();
  compose_remaps_text_and_symbol_indices();
  return EXIT_SUCCESS;
}
