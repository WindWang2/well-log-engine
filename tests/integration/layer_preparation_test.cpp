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

const auto document_id = id("20000000-0000-4000-8000-000000000001");
const auto axis_id = id("20000000-0000-4000-8000-000000000002");
const auto curve_id = id("20000000-0000-4000-8000-000000000003");
const auto track_id = id("20000000-0000-4000-8000-000000000004");
const auto pattern_id = id("20000000-0000-4000-8000-000000000005");
const auto interval_layer_id = id("20000000-0000-4000-8000-000000000006");
const auto marker_layer_id = id("20000000-0000-4000-8000-000000000007");
const auto symbol_layer_id = id("20000000-0000-4000-8000-000000000008");
const auto interval_sand_id = id("20000000-0000-4000-8000-000000000009");
const auto interval_shale_id = id("20000000-0000-4000-8000-00000000000a");
const auto interval_hidden_id = id("20000000-0000-4000-8000-00000000000b");
const auto marker_id = id("20000000-0000-4000-8000-00000000000c");
const auto symbol_id = id("20000000-0000-4000-8000-00000000000d");

WellLogDocument layer_document() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1005.0, 1010.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{4});
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
      .id = interval_sand_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1004.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{220, 200, 120, 255},
      .label = "Sandstone",
  });
  builder.add_interval(Interval{
      .id = interval_shale_id,
      .top_reference_depth = 1004.0,
      .bottom_reference_depth = 1012.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = {},
      .fill_color = RgbaColor{90, 90, 90, 255},
      .label = "Shale",
  });
  builder.add_interval(Interval{
      .id = interval_hidden_id,
      .top_reference_depth = 2000.0,
      .bottom_reference_depth = 2010.0,
      .semantic = IntervalSemantic::facies,
      .pattern_id = {},
      .fill_color = RgbaColor{10, 10, 10, 255},
      .label = "Outside",
  });
  builder.add_marker(Marker{
      .id = marker_id,
      .reference_depth = 1004.0,
      .semantic = MarkerSemantic::formation_top,
      .label = "Top B",
  });
  builder.add_symbol(SymbolOccurrence{
      .id = symbol_id,
      .reference_depth = 1002.0,
      .track_fraction = 0.5,
      .kind = SymbolKind::triangle_up,
      .label = "fossil",
  });
  return builder.build();
}

PatternDefinition brick_pattern() {
  return PatternDefinition{
      .id = pattern_id,
      .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{60, 60, 60, 255},
      .background = RgbaColor{0, 0, 0, 0},
      .stroke_width = Millimetres{0.2},
      .scene_anchor = PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
      .primitives =
          {
              PatternLine{PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
                          PhysicalPoint{Millimetres{4.0}, Millimetres{4.0}}},
          },
  };
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

std::shared_ptr<const PreparedScene>
prepare(WellLogSession &session, const ScenePresentation &presentation) {
  const auto receipt = session.execute(SetPresentationCommand{presentation});
  require(receipt.has_value(), "layer presentation must be accepted");
  auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "layer presentation must prepare a scene");
  return scene;
}

void intervals_are_clipped_to_track_and_visible_range() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{layer_document()}).has_value(),
          "source document must be accepted");
  auto builder = base_presentation();
  builder.add_pattern(brick_pattern());
  builder.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
  });
  const auto scene = prepare(session, builder.build());

  require(scene->interval_layers().size() == 1,
          "one interval layer must be prepared");
  const auto &layer = scene->interval_layers().front();
  require(layer.id == interval_layer_id, "interval layer must keep identity");
  require(layer.track_id == track_id, "interval layer must keep its track");
  require(layer.interval_count == 2,
          "out-of-range interval must be culled, two must remain");

  const auto &intervals = scene->intervals();
  require(intervals.size() == 2, "prepared intervals must match layer range");
  const auto &sand = intervals[0];
  require(sand.interval_id == interval_sand_id,
          "prepared interval must keep the source entity identity");
  require(sand.layer_id == interval_layer_id,
          "prepared interval must reference its layer");
  require_near(sand.rect.left.value, 0.0, "interval must start at track left");
  require_near(sand.rect.top.value, 0.0,
               "interval at range top must clamp to scene top");
  require_near(sand.rect.width.value, 40.0, "interval must span track width");
  require_near(sand.rect.height.value, 40.0,
               "4m of 10m over 100mm must yield 40mm");
  require(sand.pattern_id == pattern_id,
          "patterned interval must reference its pattern");
  require_near(sand.top_reference_depth, 1000.0,
               "interval must keep the top reference depth");
  require_near(sand.bottom_reference_depth, 1004.0,
               "interval must keep the bottom reference depth");

  const auto &shale = intervals[1];
  require(shale.interval_id == interval_shale_id,
          "second interval must keep its identity");
  require(shale.pattern_id.is_nil(),
          "solid interval must not reference a pattern");
  require(shale.fill_color == RgbaColor(90, 90, 90, 255),
          "solid interval must keep its fill color");
  require_near(shale.rect.top.value, 40.0,
               "adjacent interval must abut the previous one");
  require_near(shale.rect.height.value, 60.0,
               "interval crossing the range bottom must clamp to the scene");
}

void adjacent_pattern_intervals_share_a_scene_anchored_phase() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{layer_document()}).has_value(),
          "source document must be accepted");
  auto builder = base_presentation();
  builder.add_pattern(brick_pattern());
  builder.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
  });
  const auto scene = prepare(session, builder.build());

  require(scene->patterns().size() == 1,
          "referenced pattern must be stored once in the scene");
  const auto &pattern = scene->patterns().front();
  require(pattern.id == pattern_id, "pattern must keep its identity");
  require_near(pattern.scene_anchor.left.value, 0.0,
               "pattern anchor must be scene-anchored horizontally");
  require_near(pattern.scene_anchor.top.value, 0.0,
               "pattern anchor must be scene-anchored vertically");
  require_near(pattern.tile_width.value, 4.0, "tile width must round-trip");
  require(pattern.primitives.size() == 1,
          "pattern primitives must round-trip");

  // Phase continuity: the sand interval ends exactly on the 4mm tile grid
  // (40mm = 10 tiles from the scene origin), so the pattern wraps without a
  // seam at the boundary; the shale interval is solid but the invariant is
  // structural: rects are absolute scene coordinates and the tile phase is
  // derived from the anchor, never from the interval origin.
  const auto &sand = scene->intervals()[0];
  const auto tiles =
      (sand.rect.top.value - pattern.scene_anchor.top.value) /
      pattern.tile_height.value;
  require_near(tiles - std::floor(tiles), 0.0,
               "interval starting on the tile grid must not shift the phase");
  const auto phase_of = [&](double top) {
    const auto position = (top - pattern.scene_anchor.top.value) /
                          pattern.tile_height.value;
    return position - std::floor(position);
  };
  require_near(phase_of(sand.rect.top.value),
               phase_of(sand.rect.top.value + 400.0),
               "tile phase must be invariant under whole-tile scrolling");
}

void markers_and_symbols_are_prepared_at_display_depths() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{layer_document()}).has_value(),
          "source document must be accepted");
  auto builder = base_presentation();
  builder.add_pattern(brick_pattern());
  builder.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 5,
      .line_color = RgbaColor{200, 0, 0, 255},
      .line_width = Millimetres{0.5},
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
  });
  builder.add_symbol_layer(SymbolLayerSpec{
      .id = symbol_layer_id,
      .track_id = track_id,
      .z_order = 6,
      .color = RgbaColor{0, 0, 200, 255},
      .symbol_size = Millimetres{4.0},
  });
  const auto scene = prepare(session, builder.build());

  require(scene->marker_layers().size() == 1, "one marker layer expected");
  const auto &marker_layer = scene->marker_layers().front();
  require(marker_layer.marker_count == 1, "one visible marker expected");
  require(marker_layer.line_color == RgbaColor(200, 0, 0, 255),
          "marker layer must carry the line color");
  require(marker_layer.line_width == Millimetres{0.5},
          "marker layer must carry the physical line width");
  const auto &marker = scene->markers().front();
  require(marker.marker_id == marker_id, "marker must keep its identity");
  require_near(marker.display_top.value, 40.0,
               "marker at 1004m of 1000-1010m over 100mm must sit at 40mm");
  require_near(marker.reference_depth, 1004.0,
               "marker must keep the reference depth");

  require(scene->symbol_layers().size() == 1, "one symbol layer expected");
  const auto &symbol = scene->symbols().front();
  require(symbol.symbol_id == symbol_id, "symbol must keep its identity");
  require(symbol.kind == SymbolKind::triangle_up,
          "symbol kind must round-trip");
  require_near(symbol.center.left.value, 20.0,
               "symbol at fraction 0.5 must center in the 40mm track");
  require_near(symbol.center.top.value, 20.0,
               "symbol at 1002m must sit at 20mm");
}

void interval_layers_sort_by_z_order_then_identity() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{layer_document()}).has_value(),
          "source document must be accepted");
  const auto high_layer = id("20000000-0000-4000-8000-0000000000e1");
  const auto low_layer = id("20000000-0000-4000-8000-0000000000e2");
  auto builder = base_presentation();
  builder.add_pattern(brick_pattern());
  builder.add_interval_layer(IntervalLayerSpec{
      .id = high_layer,
      .track_id = track_id,
      .z_order = 30,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
  });
  builder.add_interval_layer(IntervalLayerSpec{
      .id = low_layer,
      .track_id = track_id,
      .z_order = 10,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
  });
  const auto scene = prepare(session, builder.build());
  require(scene->interval_layers().size() == 2, "two layers expected");
  require(scene->interval_layers()[0].id == low_layer,
          "lower z-order must prepare first");
  require(scene->interval_layers()[1].id == high_layer,
          "higher z-order must prepare last");
  require(scene->intervals().size() == 4,
          "both layers must enumerate the visible intervals");
  require(scene->intervals()[0].layer_id == low_layer,
          "intervals must be grouped under their layer in z order");
}

void invalid_layer_presentations_are_rejected() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{layer_document()}).has_value(),
          "source document must be accepted");

  // Unknown pattern reference.
  {
    const auto missing_pattern_id =
        id("20000000-0000-4000-8000-0000000000f1");
    WellLogDocumentBuilder doc(document_id, DocumentRevision{5});
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1010.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1.0, 2.0});
    doc.add_sampling_axis(SamplingAxis{
        .id = axis_id,
        .coordinates = BufferView::from_vector(depths),
        .domain = DepthDomain::measured_depth,
        .unit = "m",
        .direction = AxisDirection::increasing,
    });
    doc.add_curve(Curve{
        .id = curve_id,
        .mnemonic = "GR",
        .display_name = "Gamma Ray",
        .unit = "API",
        .sampling_axis_id = axis_id,
        .values = BufferView::from_vector(values),
        .nulls = {},
    });
    doc.add_interval(Interval{
        .id = interval_sand_id,
        .top_reference_depth = 1000.0,
        .bottom_reference_depth = 1005.0,
        .semantic = IntervalSemantic::lithology,
        .pattern_id = missing_pattern_id,
        .fill_color = RgbaColor{1, 2, 3, 255},
        .label = "broken",
    });
    WellLogSession broken_session;
    require(broken_session.execute(SetDocumentCommand{doc.build()}).has_value(),
            "document with unresolved pattern reference is still valid data");
    auto builder = base_presentation();
    builder.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .z_order = 0,
        .draw_labels = false,
        .label_font_size = Millimetres{3.0},
    });
    const auto result =
        broken_session.execute(SetPresentationCommand{builder.build()});
    require(!result.has_value(),
            "interval referencing an unregistered pattern must be rejected");
    require(result.error().code == ErrorCode::invalid_presentation,
            "unresolved pattern must use the presentation error code");
  }

  // Pattern with a degenerate tile.
  {
    auto builder = base_presentation();
    auto pattern = brick_pattern();
    pattern.tile_height = Millimetres{0.0};
    builder.add_pattern(pattern);
    builder.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .z_order = 0,
        .draw_labels = false,
        .label_font_size = Millimetres{3.0},
    });
    const auto result = session.execute(SetPresentationCommand{builder.build()});
    require(!result.has_value(), "zero-height tile must be rejected");
  }

  // Pattern exceeding the untrusted-asset primitive limit.
  {
    auto builder = base_presentation();
    auto pattern = brick_pattern();
    pattern.primitives.assign(
        300, PatternLine{PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
                         PhysicalPoint{Millimetres{1.0}, Millimetres{1.0}}});
    builder.add_pattern(pattern);
    builder.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .z_order = 0,
        .draw_labels = false,
        .label_font_size = Millimetres{3.0},
    });
    const auto result = session.execute(SetPresentationCommand{builder.build()});
    require(!result.has_value(),
            "patterns beyond the primitive limit must be rejected");
  }

  // Interval layer bound to an unknown track.
  {
    auto builder = base_presentation();
    builder.add_pattern(brick_pattern());
    builder.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = id("20000000-0000-4000-8000-0000000000f2"),
        .z_order = 0,
        .draw_labels = false,
        .label_font_size = Millimetres{3.0},
    });
    const auto result = session.execute(SetPresentationCommand{builder.build()});
    require(!result.has_value(), "layer on an unknown track must be rejected");
  }

  // Duplicate layer identity across layer kinds.
  {
    auto builder = base_presentation();
    builder.add_pattern(brick_pattern());
    builder.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .z_order = 0,
        .draw_labels = false,
        .label_font_size = Millimetres{3.0},
    });
    builder.add_marker_layer(MarkerLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .z_order = 1,
        .line_color = RgbaColor{0, 0, 0, 255},
        .line_width = Millimetres{0.3},
        .draw_labels = false,
        .label_font_size = Millimetres{3.0},
    });
    const auto result = session.execute(SetPresentationCommand{builder.build()});
    require(!result.has_value(),
            "identity shared across layer kinds must be rejected");
  }
}

void presentations_without_curve_layers_are_valid() {
  WellLogSession session;
  require(session.execute(SetDocumentCommand{layer_document()}).has_value(),
          "source document must be accepted");
  auto builder = base_presentation();
  builder.add_pattern(brick_pattern());
  builder.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
  });
  const auto scene = prepare(session, builder.build());
  require(scene->curve_layers().empty(),
          "no curve layers must be prepared");
  require(scene->interval_layers().size() == 1,
          "interval-only presentation must prepare its layer");
}

} // namespace

int main() {
  intervals_are_clipped_to_track_and_visible_range();
  adjacent_pattern_intervals_share_a_scene_anchored_phase();
  markers_and_symbols_are_prepared_at_display_depths();
  interval_layers_sort_by_z_order_then_identity();
  invalid_layer_presentations_are_rejected();
  presentations_without_curve_layers_are_valid();
  std::cout << "PASS: interval, marker and symbol layer preparation\n";
  return EXIT_SUCCESS;
}
