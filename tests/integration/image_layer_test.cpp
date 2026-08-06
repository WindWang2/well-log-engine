// Integration tests for the raster image layer (#152, rendering.md section 10).
// Exercises the document model (ImageSource), the multi-resolution pyramid's
// visible-tile selection, asset-limit rejection, the prepared-scene tile
// placement, and SVG raster-object export. The engine never decodes images;
// tests supply decoded tile bytes through a fake resolver.

#include "scene/prepare.hpp"

#include <welllog/export/svg.hpp>
#include <welllog/scene/image_pyramid.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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

const auto document_id = id("a0000000-0000-4000-8000-000000000001");
const auto track_id = id("a0000000-0000-4000-8000-000000000002");
const auto image_source_id = id("a0000000-0000-4000-8000-000000000003");
const auto image_layer_id = id("a0000000-0000-4000-8000-000000000004");

// A 2048x2048 image spanning depth 1000..1100, dpi 300.
WellLogDocument image_document(std::uint64_t width_px = 2048,
                               std::uint64_t height_px = 2048,
                               double dpi = 300.0) {
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_image_source(ImageSource{
      .id = image_source_id,
      .width_px = width_px,
      .height_px = height_px,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 1000.0,
      .reference_depth_bottom = 1100.0,
      .dpi = static_cast<std::uint32_t>(dpi),
      .source = BufferSourceReference{.uri = "image://core-photo/1",
                                      .checksum = {},
                                      .byte_offset = 0},
  });
  return builder.build();
}

ScenePresentationBuilder image_presentation() {
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
  builder.add_image_layer(ImageLayerSpec{
      .id = image_layer_id,
      .track_id = track_id,
      .image_source_id = image_source_id,
      .z_order = 0,
      .visible = true,
  });
  return builder;
}

// Builds the pyramid map and prepares the scene via the preparer directly
// (the session does not yet thread the image pyramid map; that is host wiring).
PreparedScene
prepare_with_image(const WellLogDocument &document,
                   ScenePresentationBuilder &builder,
                   const ImagePyramidQuery &query,
                   std::uint64_t derived_budget = 16 * 1024) {
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  detail::ScenePreparer::ImagePyramidMap image_pyramids;
  const auto pyramid = ImagePyramid::build(
      document.image_sources().front(),
      ImagePyramidOptions{.tile_size = 256, .maximum_derived_bytes = derived_budget});
  require(pyramid.has_value(), "image pyramid must build");
  image_pyramids.emplace(image_source_id, pyramid.value());
  const auto scene = detail::ScenePreparer::prepare(
      document, presentation, curve_lods, {}, image_pyramids, query);
  require(scene.has_value(), "image scene must prepare");
  return scene.value();
}

// --- Tests ------------------------------------------------------------------

// Criterion 1: an image layer declares its depth range, dimensions, DPI,
// pixel format and data-source identity, all of which round-trip into the
// prepared scene.
void image_layer_declares_metadata_and_prepares() {
  const auto document = image_document();
  auto builder = image_presentation();
  const auto scene = prepare_with_image(
      document, builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 1000.0,
                        .prefetch_viewports = 0.0});
  require(scene.image_layers().size() == 1, "one image layer expected");
  const auto &layer = scene.image_layers().front();
  require(layer.id == image_layer_id, "layer identity must round-trip");
  require(layer.image_source_id == image_source_id,
          "image source identity must round-trip");
  require(layer.tile_count > 0, "at least one visible tile must be prepared");

  const auto &tile = scene.image_tiles()[0];
  require(tile.dpi == 300, "tile must carry the source DPI");
  require(tile.pixel_format == PixelFormat::rgba8,
          "tile must carry the pixel format");
  require(tile.source.uri == "image://core-photo/1",
          "tile must carry the data-source identity");
  // The image spans the full presentation depth range.
  require_near(tile.rect.width.value, 80.0,
               "tile must span the full track width");
}

// Criterion 2: the pyramid selects only tiles overlapping the visible depth
// window (+ prefetch), not the whole image.
void pyramid_selects_only_visible_tiles() {
  const auto document = image_document(2048, 8192); // tall image

  // Full-depth viewport: all tiles.
  auto full_builder = image_presentation();
  const auto full_scene = prepare_with_image(
      document, full_builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 100.0,
                        .prefetch_viewports = 0.0});
  const auto full_count = full_scene.image_layers().front().tile_count;
  require(full_count > 0, "full viewport must select tiles");

  // Partial (top-quarter) viewport: strictly fewer tiles.
  auto partial_builder = image_presentation();
  const auto partial_scene = prepare_with_image(
      document, partial_builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1025.0,
                        .pixel_height = 100.0,
                        .prefetch_viewports = 0.0});
  const auto partial_count =
      partial_scene.image_layers().front().tile_count;
  require(partial_count > 0, "partial viewport must still select tiles");
  require(partial_count < full_count,
          "a partial viewport must select fewer tiles than the full image");
}

// Criterion 5: oversized / invalid-metadata images are rejected.
void invalid_images_are_rejected() {
  auto builder = image_presentation();

  // Zero dimensions.
  {
    WellLogDocumentBuilder b(document_id, DocumentRevision{1});
    b.add_image_source(ImageSource{
        .id = image_source_id, .width_px = 0, .height_px = 100,
        .pixel_format = PixelFormat::rgba8,
        .reference_depth_top = 1000.0, .reference_depth_bottom = 1100.0,
        .dpi = 300, .source = BufferSourceReference{.uri = "x", .checksum = {}, .byte_offset = 0}});
    const auto scene = detail::ScenePreparer::prepare(
        b.build(), builder.build());
    require(!scene.has_value(), "zero-dimension image must be rejected");
    require(scene.error().code == ErrorCode::invalid_presentation,
            "zero-dimension image must use invalid_presentation");
  }

  // Pixel count over the limit: dimensions each under the per-side cap
  // (65536) but total pixels beyond maximum_image_pixels.
  {
    WellLogDocumentBuilder b(document_id, DocumentRevision{1});
    // 24000 * 24000 = 576M pixels > 512M limit, each side < 65536.
    b.add_image_source(ImageSource{
        .id = image_source_id,
        .width_px = 24000, .height_px = 24000,
        .pixel_format = PixelFormat::rgba8,
        .reference_depth_top = 1000.0, .reference_depth_bottom = 1100.0,
        .dpi = 300, .source = BufferSourceReference{.uri = "x", .checksum = {}, .byte_offset = 0}});
    const auto scene = detail::ScenePreparer::prepare(
        b.build(), builder.build());
    require(!scene.has_value(), "oversized image must be rejected");
    require(scene.error().code == ErrorCode::invalid_image,
            "oversized image must use invalid_image");
  }

  // Inverted depth range.
  {
    WellLogDocumentBuilder b(document_id, DocumentRevision{1});
    b.add_image_source(ImageSource{
        .id = image_source_id, .width_px = 100, .height_px = 100,
        .pixel_format = PixelFormat::rgba8,
        .reference_depth_top = 1100.0, .reference_depth_bottom = 1000.0,
        .dpi = 300, .source = BufferSourceReference{.uri = "x", .checksum = {}, .byte_offset = 0}});
    const auto scene = detail::ScenePreparer::prepare(
        b.build(), builder.build());
    require(!scene.has_value(), "inverted-depth image must be rejected");
  }
}

// Criterion 7: SVG keeps each visible tile as a raster object with explicit
// physical dimensions, DPI and source identity.
void svg_keeps_raster_object_with_physical_dimensions() {
  const auto document = image_document();
  auto builder = image_presentation();
  const auto scene = prepare_with_image(
      document, builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 500.0,
                        .prefetch_viewports = 0.0});
  const auto exported = SvgExporter::write(scene);
  require(exported.has_value(), "SVG export must succeed");
  const auto text = std::string{exported.value().text()};
  require(text.find("<image ") != std::string::npos,
          "SVG must emit an <image> element");
  require(text.find("data-image-source-id=\"" +
                    image_source_id.to_string() + "\"") != std::string::npos,
          "SVG must tag the image source identity");
  require(text.find("data-dpi=\"300\"") != std::string::npos,
          "SVG must carry the explicit DPI");
  require(text.find("width=") != std::string::npos &&
              text.find("height=") != std::string::npos,
          "SVG must emit physical width/height");
  require(text.find("href=\"image://core-photo/1\"") != std::string::npos,
          "SVG must reference the data-source URI, not inline pixels");
}

// The pyramid build itself: statistics report levels, budget-limited degrade.
void pyramid_build_reports_levels_and_budget() {
  const auto document = image_document(2048, 2048);
  const auto pyramid = ImagePyramid::build(
      document.image_sources().front(),
      ImagePyramidOptions{.tile_size = 256, .maximum_derived_bytes = 1024 * 1024});
  require(pyramid.has_value(), "pyramid must build");
  const auto stats = pyramid.value().statistics();
  require(stats.width_px == 2048 && stats.height_px == 2048,
          "statistics must report source dimensions");
  require(stats.tile_size == 256, "statistics must report tile size");
  require(stats.level_count >= 1, "pyramid must have at least one level");
}

// A hidden image layer keeps its identity but emits no tiles.
void hidden_image_layer_emits_no_tiles() {
  const auto document = image_document();
  auto builder = ScenePresentationBuilder(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1100.0},
      Millimetres{200.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id, .width = Millimetres{80.0}, .z_order = 0, .header = {}});
  builder.add_image_layer(ImageLayerSpec{
      .id = image_layer_id, .track_id = track_id,
      .image_source_id = image_source_id, .z_order = 0, .visible = false});
  const auto scene = prepare_with_image(
      document, builder,
      ImagePyramidQuery{.viewport_top = 1000.0, .viewport_bottom = 1100.0,
                        .pixel_height = 500.0, .prefetch_viewports = 0.0});
  require(scene.image_layers().size() == 1,
          "hidden layer must keep its identity");
  require(scene.image_layers().front().tile_count == 0,
          "hidden layer must emit no tiles");
}

// Criterion 4: an image tile is pickable by scene position and returns the
// image source identity plus the reference depth at the hit.
void image_tile_is_pickable_and_returns_depth() {
  const auto document = image_document();
  auto builder = image_presentation();
  const auto scene = prepare_with_image(
      document, builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 500.0,
                        .prefetch_viewports = 0.0});
  const auto &layer = scene.image_layers().front();
  require(layer.tile_count > 0, "at least one tile must be prepared");
  const auto &tile = scene.image_tiles()[0];
  // Pick the center of the first tile.
  const auto pick = scene.pick_image(ImagePickQuery{
      .scene_position = PhysicalPoint{
          .left = Millimetres{tile.rect.left.value +
                              tile.rect.width.value * 0.5},
          .top = Millimetres{tile.rect.top.value +
                             tile.rect.height.value * 0.5}},
  });
  require(pick.has_value(), "a point inside the tile must be picked");
  require(pick->layer_id == image_layer_id,
          "pick must identify the image layer");
  require(pick->image_source_id == image_source_id,
          "pick must return the image source identity");
  require(pick->reference_depth >= 1000.0 && pick->reference_depth <= 1100.0,
          "pick must return a reference depth within the image range");
  // A point far outside any tile must not be picked.
  const auto miss = scene.pick_image(ImagePickQuery{
      .scene_position = PhysicalPoint{.left = Millimetres{9999.0},
                                       .top = Millimetres{9999.0}},
  });
  require(!miss.has_value(), "a point outside every tile must not be picked");
}

// Criterion 8: a large virtual image selects a bounded number of visible
// tiles (proportional to the viewport, not the source size), so peak tile
// loading stays controlled regardless of image height.
void large_image_selects_bounded_visible_tiles() {
  // A large image (within the per-side cap) over the depth range. 8000x40000
  // = 320M pixels, each side under 65536.
  const auto document = image_document(8000, 40000);

  // Full-depth viewport: selects the whole image's tile grid.
  auto full_builder = image_presentation();
  const auto full_scene = prepare_with_image(
      document, full_builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 1000.0,
                        .prefetch_viewports = 0.0},
      /*derived_budget=*/64 * 1024 * 1024);
  const auto full_count = full_scene.image_layers().front().tile_count;
  require(full_count > 0, "full viewport must select tiles");

  // Narrow viewport over the top 10%: must select strictly fewer tiles.
  auto narrow_builder = image_presentation();
  const auto narrow_scene = prepare_with_image(
      document, narrow_builder,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1010.0,
                        .pixel_height = 200.0,
                        .prefetch_viewports = 0.0},
      /*derived_budget=*/64 * 1024 * 1024);
  const auto narrow_count = narrow_scene.image_layers().front().tile_count;
  require(narrow_count > 0, "narrow viewport must still select tiles");
  require(narrow_count < full_count,
          "visible-tile count must be bounded by the viewport, not the "
          "source image size");
}

} // namespace

// #184: an image layer rendered through the production session path
// (execute(SetDocument) → execute(SetPresentation) → prepared_scene) produces
// the SAME visible tiles as the direct ScenePreparer::prepare path. The session
// REQUIRES an axis+curve (validate_document rejects image-only docs), so the
// fixture carries a minimal curve alongside the image.
void session_image_layer_matches_direct_prepare() {
  const auto axis_id = id("a0000000-0000-4000-8000-00000000000a");
  const auto curve_id = id("a0000000-0000-4000-8000-00000000000b");
  // Build a document with one axis, one curve, one image source, one image
  // layer. The curve exceeds the default asynchronous_sample_threshold (16384)
  // so the session takes the ASYNC LOD+frame path (the path #184 wires image
  // pyramids through); an image-only or tiny-curve doc would take the sync
  // fallback, which has no LOD at all.
  constexpr std::uint64_t curve_samples = 20'000;
  auto depths_fill = std::make_shared<std::vector<double>>(curve_samples);
  auto values_fill = std::make_shared<std::vector<double>>(curve_samples);
  for (std::uint64_t i = 0; i < curve_samples; ++i) {
    (*depths_fill)[i] = 1000.0 + static_cast<double>(i) * 0.005; // 1000..1100
    (*values_fill)[i] = static_cast<double>(i);
  }
  auto depths = std::shared_ptr<const std::vector<double>>(std::move(depths_fill));
  auto values = std::shared_ptr<const std::vector<double>>(std::move(values_fill));
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
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  builder.add_image_source(ImageSource{
      .id = image_source_id,
      .width_px = 2048,
      .height_px = 2048,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 1000.0,
      .reference_depth_bottom = 1100.0,
      .dpi = 300,
      .source = BufferSourceReference{.uri = "image://core-photo/1",
                                      .checksum = {}, .byte_offset = 0},
  });
  const auto document = builder.build();
  auto presentation_builder = image_presentation();
  const auto presentation = presentation_builder.build();

  // Configure the session's image-pyramid options to match the direct path.
  PerformanceBudgets budgets;
  budgets.image_pyramid_options =
      ImagePyramidOptions{.tile_size = 256, .maximum_derived_bytes = 16 * 1024};

  WellLogSession session(budgets);
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "session must accept the image+curve document");
  require(session.execute(SetPresentationCommand{presentation}).has_value(),
          "session must accept the image presentation");
  // Pump the async LOD + frame tasks to completion. The LOD worker runs on a
  // jthread; poll_async reaps finished tasks and re-issues the frame task. A
  // short yield between polls lets the worker thread make progress (a tight
  // poll loop starves it).
  for (int i = 0; i < 200; ++i) {
    session.poll_async();
    const auto scene_now = session.prepared_scene(document_id);
    if (scene_now != nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "session must prepare an image scene");
  require(scene->image_layers().size() == 1, "one image layer expected");
  require(scene->image_layers().front().tile_count > 0,
          "the session path must produce visible image tiles");

  // Assert the session's viewport matches what the direct query will use, so
  // the parity comparison is between equivalent viewports (not coincidence).
  const auto session_viewport = session.viewport(document_id);
  require(session_viewport.has_value(),
          "the session must have a viewport for the document");
  require(session_viewport->top == 1000.0 && session_viewport->bottom == 1100.0,
          "the session viewport must be the presentation depth range [1000,1100]");
  const auto session_pixel_height = session.viewport_pixel_height(document_id);
  require(session_pixel_height.has_value() && *session_pixel_height == 2160,
          "the session pixel height must be the default 2160");

  // Collect the session path's visible tiles as a (level,row,col) set.
  auto tile_key = [](const PreparedImageTile &t) {
    return std::tuple{t.level, t.row, t.col};
  };
  std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> session_tiles;
  for (const auto &t : scene->image_tiles()) {
    session_tiles.insert(tile_key(t));
  }
  require(!session_tiles.empty(), "session path must select >=1 tile");

  // Direct path: build the same pyramid map and prepare with an equivalent
  // viewport (1000..1100 × 2160, prefetch 2.0 — the session defaults).
  detail::ScenePreparer::CurveLodMap curve_lods;
  detail::ScenePreparer::ImagePyramidMap image_pyramids;
  const auto pyramid = ImagePyramid::build(
      document.image_sources().front(),
      ImagePyramidOptions{.tile_size = 256, .maximum_derived_bytes = 16 * 1024});
  require(pyramid.has_value(), "direct image pyramid must build");
  image_pyramids.emplace(image_source_id, pyramid.value());
  const auto direct = detail::ScenePreparer::prepare(
      document, presentation, curve_lods, {}, image_pyramids,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 2160.0,
                        .prefetch_viewports = 2.0});
  require(direct.has_value(), "direct image scene must prepare");
  require(direct.value().image_layers().size() == 1,
          "direct path must have one image layer");
  std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> direct_tiles;
  for (const auto &t : direct.value().image_tiles()) {
    direct_tiles.insert(tile_key(t));
  }

  // Parity: the session path selects the SAME visible tiles (level/row/col set)
  // as the direct prepare path — not just the same count.
  require(session_tiles == direct_tiles,
          "session and direct paths must select the same visible tile set");
}

int main() {
  image_layer_declares_metadata_and_prepares();
  pyramid_selects_only_visible_tiles();
  invalid_images_are_rejected();
  svg_keeps_raster_object_with_physical_dimensions();
  pyramid_build_reports_levels_and_budget();
  hidden_image_layer_emits_no_tiles();
  image_tile_is_pickable_and_returns_depth();
  large_image_selects_bounded_visible_tiles();
  session_image_layer_matches_direct_prepare();
  std::cout << "PASS: raster image layer\n";
  return EXIT_SUCCESS;
}
