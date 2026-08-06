// Integration tests for the declarative custom layer (#153, ADR 0018/0046,
// rendering.md section 11). Exercises the document entity (CustomLayerSource),
// primitive decomposition into the prepared-scene primitive stream, asset-limit
// rejection, layer-local clipping, semantic picking, and SVG/GL parity. The
// engine accepts only plain-data primitives — there is no field for shaders,
// scripts or commands, so the ADR 0042 constraint is structural.

#include "scene/prepare.hpp"

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>
#include <welllog/core/result.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/render_gl/upload.hpp>
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
  std::exit(EXIT_FAILURE);
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

const auto document_id = id("b0000000-0000-4000-8000-000000000001");
const auto track_id = id("b0000000-0000-4000-8000-000000000002");
const auto custom_source_id = id("b0000000-0000-4000-8000-000000000003");
const auto custom_layer_id = id("b0000000-0000-4000-8000-000000000004");

// A document whose only entity is a custom layer source. The presentation's
// depth range is 1000..1100 mapped onto 200 mm of physical height.
WellLogDocument custom_document(CustomLayerSource source) {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_custom_source(source);
  return builder.build();
}

ScenePresentationBuilder custom_presentation() {
  auto builder = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1100.0,
      },
      Millimetres{200.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{80.0},
      .z_order = 0,
      .header = {},
  });
  return builder;
}

// Prepares directly via the scene preparer (the custom layer needs no host
// resolver, unlike the image layer), mirroring the image-layer test.
PreparedScene prepare_custom(const WellLogDocument &document,
                             ScenePresentationBuilder &builder) {
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  const auto scene =
      detail::ScenePreparer::prepare(document, presentation, curve_lods, {});
  require(scene.has_value(), "custom scene must prepare");
  return scene.value();
}

// A source with one of each primitive kind, placed in scene millimetres.
CustomLayerSource mixed_source() {
  CustomLayerSource source{
      .id = custom_source_id,
      .content_revision = DocumentRevision{7},
      .primitives = {},
      .clip = std::nullopt,
  };
  source.primitives.push_back(CustomPrimitive{CustomPolyline{
      .points = {PhysicalPoint{.left = Millimetres{10.0}, .top = Millimetres{20.0}},
                 PhysicalPoint{.left = Millimetres{40.0}, .top = Millimetres{20.0}},
                 PhysicalPoint{.left = Millimetres{40.0}, .top = Millimetres{80.0}}},
      .closed = false,
      .color = RgbaColor{10, 20, 200, 255},
      .width = Millimetres{0.5},
  }});
  source.primitives.push_back(CustomPrimitive{CustomTriangle{
      .a = PhysicalPoint{.left = Millimetres{50.0}, .top = Millimetres{20.0}},
      .b = PhysicalPoint{.left = Millimetres{70.0}, .top = Millimetres{20.0}},
      .c = PhysicalPoint{.left = Millimetres{60.0}, .top = Millimetres{60.0}},
      .fill_color = RgbaColor{200, 100, 0, 255},
  }});
  source.primitives.push_back(CustomPrimitive{CustomQuad{
      .rect = PhysicalRect{
          .left = Millimetres{5.0}, .top = Millimetres{100.0},
          .width = Millimetres{30.0}, .height = Millimetres{40.0}},
      .fill_color = RgbaColor{0, 180, 80, 255},
  }});
  source.primitives.push_back(CustomPrimitive{CustomSymbolOccurrence{
      .center = PhysicalPoint{.left = Millimetres{60.0}, .top = Millimetres{120.0}},
      .kind = SymbolKind::circle,
      .color = RgbaColor{128, 0, 128, 255},
      .size = Millimetres{6.0},
  }});
  return source;
}

// --- Tests ------------------------------------------------------------------

// Criterion 1: a custom source carries its Entity ID and content revision into
// the prepared scene, and the layer references it by id.
void custom_source_declares_identity_and_revision() {
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto scene =
      prepare_custom(custom_document(mixed_source()), builder);
  require(scene.custom_layers().size() == 1, "one custom layer must prepare");
  const auto &layer = scene.custom_layers().front();
  require(layer.id == custom_layer_id, "layer id must round-trip");
  require(layer.custom_source_id == custom_source_id,
          "source id must round-trip");
  require(layer.track_id == track_id, "track id must round-trip");
}

// Each primitive kind decomposes into the prepared-scene primitive stream.
void each_primitive_kind_is_prepared() {
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto scene =
      prepare_custom(custom_document(mixed_source()), builder);
  const auto &layer = scene.custom_layers().front();
  require(layer.primitive_count == 4,
          "polyline + triangle + quad + symbol must prepare four primitives");
  const auto primitives = scene.custom_primitives();
  require(primitives[static_cast<std::size_t>(layer.first_primitive)].kind ==
              CustomPrimitiveKind::polyline,
          "first primitive must be the polyline");
  require(primitives[static_cast<std::size_t>(layer.first_primitive + 1)].kind ==
              CustomPrimitiveKind::triangle,
          "second primitive must be the triangle");
  require(primitives[static_cast<std::size_t>(layer.first_primitive + 2)].kind ==
              CustomPrimitiveKind::quad,
          "third primitive must be the quad");
  require(primitives[static_cast<std::size_t>(layer.first_primitive + 3)].kind ==
              CustomPrimitiveKind::symbol,
          "fourth primitive must be the symbol");
  // The triangle's three vertices round-trip into custom_vertices.
  const auto vertices = scene.custom_vertices();
  const auto &triangle =
      primitives[static_cast<std::size_t>(layer.first_primitive + 1)];
  require(triangle.vertex_count == 3, "triangle must store three vertices");
  require_near(vertices[static_cast<std::size_t>(triangle.first_vertex)]
                   .left.value,
               50.0, "triangle vertex a.x must round-trip");
}

// A nil or missing source is rejected with the presentation error.
void missing_source_is_rejected() {
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = id("b0000000-0000-4000-8000-000000000099"),
      .z_order = 0, .visible = true});
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  const auto scene = detail::ScenePreparer::prepare(
      custom_document(mixed_source()), presentation, curve_lods, {});
  require(!scene.has_value(), "a missing custom source must be rejected");
  require(scene.error().code == ErrorCode::invalid_presentation,
          "missing source must use the presentation error code");
}

// An empty source is rejected with the dedicated custom-source error code and
// message (criterion 6: invalid primitives become a local diagnostic).
void empty_source_is_rejected_with_custom_error() {
  CustomLayerSource source{.id = custom_source_id,
                           .content_revision = DocumentRevision{1},
                           .primitives = {},
                           .clip = std::nullopt};
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  const auto scene = detail::ScenePreparer::prepare(
      custom_document(source), presentation, curve_lods, {});
  require(!scene.has_value(), "an empty custom source must be rejected");
  require(scene.error().code == ErrorCode::invalid_custom_source,
          "empty source must use the custom-source error code");
  require(scene.error().message == MessageKey::custom_source_empty,
          "empty source must report the empty message key");
}

// An over-limit source (too many primitives) is rejected with the dedicated
// limit error, not the generic presentation error.
void oversized_source_is_rejected_with_limit_error() {
  CustomLayerSource source{.id = custom_source_id,
                           .content_revision = DocumentRevision{1},
                           .primitives = {},
                           .clip = std::nullopt};
  // One more than the documented maximum (4096).
  for (std::size_t index = 0; index < 4097; ++index) {
    source.primitives.push_back(CustomPrimitive{CustomTriangle{
        .a = PhysicalPoint{.left = Millimetres{0.0}, .top = Millimetres{0.0}},
        .b = PhysicalPoint{.left = Millimetres{1.0}, .top = Millimetres{0.0}},
        .c = PhysicalPoint{.left = Millimetres{0.0}, .top = Millimetres{1.0}},
        .fill_color = RgbaColor{0, 0, 0, 255},
    }});
  }
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  const auto scene = detail::ScenePreparer::prepare(
      custom_document(source), presentation, curve_lods, {});
  require(!scene.has_value(), "an oversized custom source must be rejected");
  require(scene.error().code == ErrorCode::invalid_custom_source,
          "oversized source must use the custom-source error code");
  require(scene.error().message ==
              MessageKey::custom_source_primitives_exceed_limit,
          "oversized source must report the primitives-limit message key");
}

// A non-finite primitive point is rejected (ADR 0042 — geometry is validated).
void non_finite_geometry_is_rejected() {
  CustomLayerSource source{.id = custom_source_id,
                           .content_revision = DocumentRevision{1},
                           .primitives = {},
                           .clip = std::nullopt};
  source.primitives.push_back(CustomPrimitive{CustomTriangle{
      .a = PhysicalPoint{.left = Millimetres{0.0}, .top = Millimetres{0.0}},
      .b = PhysicalPoint{.left = Millimetres{std::numeric_limits<double>::infinity()},
                         .top = Millimetres{0.0}},
      .c = PhysicalPoint{.left = Millimetres{0.0}, .top = Millimetres{1.0}},
      .fill_color = RgbaColor{0, 0, 0, 255},
  }});
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  const auto scene = detail::ScenePreparer::prepare(
      custom_document(source), presentation, curve_lods, {});
  require(!scene.has_value(), "non-finite geometry must be rejected");
  require(scene.error().code == ErrorCode::invalid_presentation,
          "bad geometry must use the presentation error code");
}

// A hidden custom layer keeps its identity but contributes no primitives.
void hidden_layer_keeps_identity_without_geometry() {
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = false});
  const auto scene =
      prepare_custom(custom_document(mixed_source()), builder);
  require(scene.custom_layers().size() == 1,
          "hidden layer must still be present in the prepared scene");
  const auto &layer = scene.custom_layers().front();
  require(!layer.visible, "layer must remain marked hidden");
  require(layer.primitive_count == 0,
          "hidden layer must contribute no primitives");
}

// A custom primitive is pickable and returns the layer, source, primitive
// index, kind and an inverted reference depth (criterion 4).
void primitive_is_pickable() {
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto scene =
      prepare_custom(custom_document(mixed_source()), builder);
  const auto &layer = scene.custom_layers().front();
  // The quad is at top=100mm of a 200mm scene over depth 1000..1100.
  const auto &quad =
      scene.custom_primitives()[static_cast<std::size_t>(layer.first_primitive + 2)];
  const auto center_left = quad.bounds.left.value + quad.bounds.width.value * 0.5;
  const auto center_top = quad.bounds.top.value + quad.bounds.height.value * 0.5;
  const auto pick = scene.pick_custom(CustomPickQuery{
      .scene_position =
          PhysicalPoint{.left = Millimetres{center_left}, .top = Millimetres{center_top}},
      .tolerance = DeviceIndependentPixels{1.0},
      .horizontal_device_independent_pixels_per_millimetre = 1.0,
      .vertical_device_independent_pixels_per_millimetre = 1.0,
  });
  require(pick.has_value(), "the quad must be pickable at its centre");
  require(pick->layer_id == custom_layer_id, "pick must return the layer id");
  require(pick->source_id == custom_source_id,
          "pick must return the source id");
  require(pick->source_primitive_index == 2,
          "pick must return the quad's primitive index");
  require(pick->kind == CustomPrimitiveKind::quad,
          "pick must return the quad kind");
  // The quad is at top=100mm, height=40mm; its centre is at top=120mm.
  // 120mm / 200mm * 100 depth-span + 1000 top = 1060.
  require_near(pick->reference_depth, 1060.0,
               "pick must invert the scene position to a reference depth");
}

// A point outside a layer-local clip path is not pickable (criterion 2: the
// clip masks the layer's own primitives).
void clip_path_masks_primitives() {
  CustomLayerSource source{.id = custom_source_id,
                           .content_revision = DocumentRevision{1},
                           .primitives = {},
                           .clip = std::nullopt};
  source.primitives.push_back(CustomPrimitive{CustomQuad{
      .rect = PhysicalRect{
          .left = Millimetres{0.0}, .top = Millimetres{0.0},
          .width = Millimetres{80.0}, .height = Millimetres{200.0}},
      .fill_color = RgbaColor{0, 0, 0, 255},
  }});
  // Clip to the top-left quadrant only.
  source.clip = CustomClipPath{.points = {
      PhysicalPoint{.left = Millimetres{0.0}, .top = Millimetres{0.0}},
      PhysicalPoint{.left = Millimetres{40.0}, .top = Millimetres{0.0}},
      PhysicalPoint{.left = Millimetres{40.0}, .top = Millimetres{100.0}},
      PhysicalPoint{.left = Millimetres{0.0}, .top = Millimetres{100.0}},
  }};
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto scene = prepare_custom(custom_document(source), builder);
  require(scene.custom_clip_paths().size() == 1,
          "the clip path must be prepared");
  // Inside the clip quadrant.
  const auto inside = scene.pick_custom(CustomPickQuery{
      .scene_position =
          PhysicalPoint{.left = Millimetres{20.0}, .top = Millimetres{50.0}},
      .tolerance = DeviceIndependentPixels{1.0},
      .horizontal_device_independent_pixels_per_millimetre = 1.0,
      .vertical_device_independent_pixels_per_millimetre = 1.0,
  });
  require(inside.has_value(), "a point inside the clip must be pickable");
  // Outside the clip quadrant (still inside the quad's bounds).
  const auto outside = scene.pick_custom(CustomPickQuery{
      .scene_position =
          PhysicalPoint{.left = Millimetres{60.0}, .top = Millimetres{50.0}},
      .tolerance = DeviceIndependentPixels{1.0},
      .horizontal_device_independent_pixels_per_millimetre = 1.0,
      .vertical_device_independent_pixels_per_millimetre = 1.0,
  });
  require(!outside.has_value(),
          "a point outside the clip must not be pickable");
}

// Criterion 3 + criterion 8: GL and SVG consume the same custom geometry, and
// the SVG tags the layer/source/primitive identity. Also proves a new symbol
// layer renders without touching the GL or layout core (criterion 8).
void opengl_and_svg_consume_identical_custom_geometry() {
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto scene =
      prepare_custom(custom_document(mixed_source()), builder);

  // Recompute the triangle count the GL stream will walk: polyline
  // (2 segments * 2) + triangle (1) + quad (2) + symbol circle (24) = 31.
  const auto schedule = GpuUploadSchedule::plan(
      scene, GpuUploadBudgets{.maximum_cache_bytes = 1024 * 1024,
                              .maximum_bytes_per_frame = 1024 * 1024});
  require(schedule.has_value(), "upload plan must succeed");
  require(schedule.value().custom_triangle_count() == 31,
          "GL upload must account for every prepared custom triangle");

  const auto exported = SvgExporter::write(scene);
  require(exported.has_value(), "SVG export must succeed");
  const auto text = std::string(exported.value().text());
  require(text.find("data-custom-layer-id=\"" + custom_layer_id.to_string() +
                    "\"") != std::string::npos,
          "SVG must tag the custom layer id");
  require(text.find("data-custom-source-id=\"" + custom_source_id.to_string() +
                    "\"") != std::string::npos,
          "SVG must tag the custom source id");
  require(text.find("fill=\"#c86400\"") != std::string::npos,
          "SVG must emit the triangle fill colour");
  require(text.find("fill=\"#00b450\"") != std::string::npos,
          "SVG must emit the quad fill colour");
}

// The example-symbol-layer criterion (criterion 8): a custom layer whose only
// primitive is a symbol renders through the existing GL/SVG paths without any
// change to the renderer or layout core.
void example_symbol_layer_renders() {
  CustomLayerSource source{.id = custom_source_id,
                           .content_revision = DocumentRevision{1},
                           .primitives = {},
                           .clip = std::nullopt};
  source.primitives.push_back(CustomPrimitive{CustomSymbolOccurrence{
      .center = PhysicalPoint{.left = Millimetres{40.0}, .top = Millimetres{100.0}},
      .kind = SymbolKind::circle,
      .color = RgbaColor{200, 0, 0, 255},
      .size = Millimetres{8.0},
  }});
  auto builder = custom_presentation();
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id, .track_id = track_id,
      .custom_source_id = custom_source_id, .z_order = 0, .visible = true});
  const auto scene = prepare_custom(custom_document(source), builder);
  require(scene.custom_layers().size() == 1,
          "the example symbol layer must prepare");
  require(scene.custom_primitives().size() == 1,
          "one symbol primitive must be prepared");
  const auto schedule = GpuUploadSchedule::plan(
      scene, GpuUploadBudgets{.maximum_cache_bytes = 1024 * 1024,
                              .maximum_bytes_per_frame = 1024 * 1024});
  require(schedule.has_value(), "symbol upload plan must succeed");
  require(schedule.value().custom_triangle_count() == 24,
          "the symbol circle must account for 24 GL triangles");
  const auto exported = SvgExporter::write(scene);
  require(exported.has_value(), "symbol SVG export must succeed");
  require(std::string(exported.value().text()).find("fill=\"#c80000\"") !=
              std::string::npos,
          "SVG must emit the symbol fill colour");
}

} // namespace

int main() {
  custom_source_declares_identity_and_revision();
  each_primitive_kind_is_prepared();
  missing_source_is_rejected();
  empty_source_is_rejected_with_custom_error();
  oversized_source_is_rejected_with_limit_error();
  non_finite_geometry_is_rejected();
  hidden_layer_keeps_identity_without_geometry();
  primitive_is_pickable();
  clip_path_masks_primitives();
  opengl_and_svg_consume_identical_custom_geometry();
  example_symbol_layer_renders();
  std::cout << "welllog.custom-layer: all cases passed\n";
  return 0;
}
