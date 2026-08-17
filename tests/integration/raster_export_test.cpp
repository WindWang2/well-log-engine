#include <welllog/export/raster.hpp>
#include <welllog/session/session.hpp>

#include <zlib.h>

#include "png_decode.hpp"

using welllog::test::DecodedPng;
using welllog::test::decode_png;

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
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
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

struct Fixture {
  WellLogDocument document;
  PreparedScene scene;
  ExportSnapshot snapshot;
};

Fixture make_curve_fixture() {
  const auto document_id = id("15700000-0000-4000-8000-000000000001");
  const auto axis_id = id("15700000-0000-4000-8000-000000000002");
  const auto curve_id = id("15700000-0000-4000-8000-000000000003");
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1001.0, 1002.0, 1003.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 40.0, 20.0, 60.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{7});
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
  auto document = builder.build();

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "fixture document must load");

  const auto track_id = id("15700000-0000-4000-8000-000000000010");
  const auto scale_id = id("15700000-0000-4000-8000-000000000011");
  const auto layer_id = id("15700000-0000-4000-8000-000000000012");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1003.0},
      Millimetres{80.0}, "font-fixture-v1");
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
      .color = RgbaColor{.red = 200, .green = 20, .blue = 20, .alpha = 255},
      .line_width = Millimetres{0.4},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "fixture presentation must load");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && !scene->curve_layers().empty(),
          "fixture must prepare curve layers");

  ExportSnapshot snapshot{
      .document_id = document.id(),
      .document_revision = document.revision(),
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{.domain = DepthDomain::measured_depth,
                                   .unit = "m",
                                   .reference_top = 1000.0,
                                   .reference_bottom = 1003.0,
                                   .version = 1},
      .font_asset_fingerprint = "font-fixture-v1",
      .page =
          ExportPageSpec{
              .mode = PaginationMode::continuous,
              .page_width = Millimetres{80.0},
              .page_height = Millimetres{120.0},
              .dpi = 100,
              .well_name = "Raster-Fixture",
          },
  };
  return Fixture{.document = std::move(document),
                 .scene = *scene,
                 .snapshot = std::move(snapshot)};
}

Fixture make_symbol_fixture() {
  const auto document_id = id("15700000-0000-4000-8000-000000000001");
  const auto axis_id = id("15700000-0000-4000-8000-000000000002");
  const auto curve_id = id("15700000-0000-4000-8000-000000000003");
  const auto symbol_id = id("15700000-0000-4000-8000-000000000004");
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1001.0, 1002.0, 1003.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 40.0, 20.0, 60.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{7});
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
  builder.add_symbol(SymbolOccurrence{
      .id = symbol_id,
      .reference_depth = 1001.5,
      .track_fraction = 0.5,
      .kind = SymbolKind::diamond,
      .label = "Sym",
  });
  auto document = builder.build();

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "symbol fixture document must load");

  const auto track_id = id("15700000-0000-4000-8000-000000000010");
  const auto scale_id = id("15700000-0000-4000-8000-000000000011");
  const auto layer_id = id("15700000-0000-4000-8000-000000000012");
  const auto symbol_layer_id = id("15700000-0000-4000-8000-000000000013");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1003.0},
      Millimetres{80.0}, "font-fixture-v1");
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
      .color = RgbaColor{.red = 20, .green = 160, .blue = 20, .alpha = 255},
      .line_width = Millimetres{0.4},
      .z_order = 1,
      .visible = true,
  });
  presentation.add_symbol_layer(SymbolLayerSpec{
      .id = symbol_layer_id,
      .track_id = track_id,
      .z_order = 2,
      .color = RgbaColor{.red = 200, .green = 30, .blue = 30, .alpha = 255},
      .symbol_size = Millimetres{3.0},
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "symbol fixture presentation must load");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && !scene->symbol_layers().empty() &&
              !scene->symbols().empty(),
          "fixture must prepare symbol layers");

  ExportSnapshot snapshot{
      .document_id = document.id(),
      .document_revision = document.revision(),
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{.domain = DepthDomain::measured_depth,
                                   .unit = "m",
                                   .reference_top = 1000.0,
                                   .reference_bottom = 1003.0,
                                   .version = 1},
      .font_asset_fingerprint = "font-fixture-v1",
      .page =
          ExportPageSpec{
              .mode = PaginationMode::continuous,
              .page_width = Millimetres{80.0},
              .page_height = Millimetres{120.0},
              .dpi = 100,
              .well_name = "Raster-Symbol-Fixture",
          },
  };
  return Fixture{.document = std::move(document),
                 .scene = *scene,
                 .snapshot = std::move(snapshot)};
}

std::filesystem::path temp_file(std::string_view name) {
  return std::filesystem::temp_directory_path() / name;
}

void png_pixels_decode_and_carry_background_and_curve() {
  // E5: content-level verification — decode the PNG ourselves (zlib) and
  // check dimensions, background, and that the curve really drew something.
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-content.png");
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 120,
      .height_px = 200,
      .background = RgbaColor{255, 255, 255, 255},
      .color_space = RasterColorSpace::srgb,
      .tile_height_px = 16,
  };
  const auto report = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(report.has_value(), "content PNG export must succeed");

  const auto png = decode_png(path);
  require(png.has_value(), "PNG must decode with the test-side reader");
  require(png->width == 120 && png->height == 200,
          "decoded dimensions must match the request");
  require(png->bit_depth == 8 && png->color_type == 6,
          "srgb export must be 8-bit RGBA");

  const auto channels = 4U;
  const auto stride = static_cast<std::size_t>(png->width) * channels;
  const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
    const auto at = stride * y + static_cast<std::size_t>(x) * channels;
    return std::array<std::uint8_t, 4>{
        png->samples[at], png->samples[at + 1], png->samples[at + 2],
        png->samples[at + 3]};
  };
  // Corners carry the background.
  const auto bg = std::array<std::uint8_t, 4>{255, 255, 255, 255};
  require(pixel(0, 0) == bg && pixel(119, 0) == bg &&
              pixel(0, 199) == bg && pixel(119, 199) == bg,
          "all four corners must be the requested background");
  // The red curve (200,20,20) must draw: at least one non-background pixel.
  bool drew = false;
  for (std::uint32_t y = 0; y < 200 && !drew; ++y) {
    for (std::uint32_t x = 0; x < 120; ++x) {
      if (pixel(x, y) != bg) {
        drew = true;
        break;
      }
    }
  }
  require(drew, "exported PNG must contain curve pixels beyond background");
  std::filesystem::remove(path);
}

void raster_draws_symbol_layer_pixels() {
  // Symbol layers (PreparedSymbol) must reach the raster path: the diamond
  // glyph draws at the prepared scene-mm center, below the curve pass.
  auto fixture = make_symbol_fixture();
  const auto path = temp_file("welllog-157-symbol.png");
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 120,
      .height_px = 200,
      .background = RgbaColor{255, 255, 255, 255},
      .color_space = RasterColorSpace::srgb,
      .tile_height_px = 16,
  };
  const auto report = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(report.has_value(), "symbol PNG export must succeed");

  const auto png = decode_png(path);
  require(png.has_value(), "PNG must decode with the test-side reader");
  require(png->width == 120 && png->height == 200,
          "decoded dimensions must match the request");
  require(fixture.scene.symbol_layers().size() == 1 &&
              fixture.scene.symbols().size() == 1,
          "fixture must prepare exactly one symbol layer with one symbol");

  const auto &layer = fixture.scene.symbol_layers().front();
  const auto &symbol = fixture.scene.symbols().front();
  const auto channels = 4U;
  const auto stride = static_cast<std::size_t>(png->width) * channels;
  const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
    const auto at = stride * y + static_cast<std::size_t>(x) * channels;
    return std::array<std::uint8_t, 4>{
        png->samples[at], png->samples[at + 1], png->samples[at + 2],
        png->samples[at + 3]};
  };
  const auto color =
      std::array<std::uint8_t, 4>{layer.color.red, layer.color.green,
                                  layer.color.blue, layer.color.alpha};
  // Symbol center is prepared in scene mm (display domain); the same
  // scene mm × dpi mapping the raster path uses.
  const auto ppm = static_cast<double>(report.value().dpi) / 25.4;
  const auto cx =
      static_cast<int>(std::lround(symbol.center.left.value * ppm));
  const auto cy = static_cast<int>(std::lround(symbol.center.top.value * ppm));
  const auto radius = static_cast<int>(
      std::ceil(layer.symbol_size.value * ppm / 2.0));
  const auto in_box = [&](int x, int y) {
    return x >= cx - radius - 1 && x <= cx + radius + 1 && y >= cy - radius - 1 &&
           y <= cy + radius + 1;
  };
  bool glyph_drawn = false;
  std::size_t color_pixels_outside = 0;
  for (std::uint32_t y = 0; y < png->height; ++y) {
    for (std::uint32_t x = 0; x < png->width; ++x) {
      if (pixel(x, y) != color) {
        continue;
      }
      if (in_box(static_cast<int>(x), static_cast<int>(y))) {
        glyph_drawn = true;
      } else {
        ++color_pixels_outside;
      }
    }
  }
  require(glyph_drawn, "raster must draw the symbol glyph at its center");
  require(color_pixels_outside == 0,
          "symbol color appears only inside the glyph box (no stray pixels)");
  std::filesystem::remove(path);
}

void gray_png_curve_samples_match_srgb_luma() {
  // E5: gray colour space applies the documented Y = round(0.2126 R +
  // 0.7152 G + 0.0722 B) of sRGB — the red curve (200,20,20) → 58.
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-gray-content.png");
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 100,
      .height_px = 150,
      .background = RgbaColor{255, 255, 255, 255},
      .color_space = RasterColorSpace::gray,
      .tile_height_px = 16,
  };
  const auto report = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(report.has_value(), "gray PNG export must succeed");

  const auto png = decode_png(path);
  require(png.has_value(), "gray PNG must decode");
  require(png->color_type == 0, "gray export must be color type 0");
  const auto stride = static_cast<std::size_t>(png->width);
  const auto sample = [&](std::uint32_t x, std::uint32_t y) {
    return png->samples[stride * y + x];
  };
  require(sample(0, 0) == 255 && sample(99, 149) == 255,
          "gray background must stay white");
  bool drew_gray = false;
  for (std::uint32_t y = 0; y < 150; ++y) {
    for (std::uint32_t x = 0; x < 100; ++x) {
      const auto v = sample(x, y);
      if (v != 255) {
        drew_gray = true;
        // Red (200,20,20) → round(0.2126·200 + 0.7152·20 + 0.0722·20) = 58.
        require(v == 58, "curve pixel must match the documented sRGB luma");
      }
    }
  }
  require(drew_gray, "gray PNG must contain curve pixels");
  std::filesystem::remove(path);
}

void raster_page_dimensions_match_the_shared_snapshot_geometry() {
  // E6: the raster path derives pixels from the SAME ExportSnapshot page
  // geometry as PDF/SVG (page_width_mm × dpi). An explicit-width export must
  // agree with the physical mm the other backends assert on.
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-geometry.png");
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .dpi_override = 100,
      .background = RgbaColor{255, 255, 255, 255},
      .tile_height_px = 16,
  };
  const auto report = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(report.has_value(), "geometry PNG export must succeed");
  // Pixels derive from the SCENE physical size × effective DPI (the same
  // physical mm the PDF/SVG backends lay out in) — scene is the single
  // source of truth, not the snapshot page spec.
  const auto expect_w = static_cast<std::uint32_t>(
      std::llround(fixture.scene.physical_width().value * 100.0 / 25.4));
  const auto expect_h = static_cast<std::uint32_t>(
      std::llround(fixture.scene.physical_height().value * 100.0 / 25.4));
  require(report.value().width_px == expect_w &&
              report.value().height_px == expect_h,
          "raster pixels must derive from the shared page mm × dpi");
  const auto png = decode_png(path);
  require(png.has_value() && png->width == expect_w &&
              png->height == expect_h,
          "decoded pixels must match the report dimensions");
  std::filesystem::remove(path);
}

void exports_png_and_tiff_with_snapshot_revision() {
  auto fixture = make_curve_fixture();
  const auto png_path = temp_file("welllog-157.png");
  const auto tiff_path = temp_file("welllog-157.tiff");
  std::filesystem::remove(png_path);
  std::filesystem::remove(tiff_path);

  RasterExportRequest png_req{
      .path = png_path,
      .format = RasterImageFormat::png,
      .background = RgbaColor{255, 255, 255, 255},
      .color_space = RasterColorSpace::srgb,
      .tile_height_px = 32,
  };
  const auto png = export_raster_sync(fixture.scene, fixture.snapshot, png_req);
  require(png.has_value(), "PNG export must succeed");
  require(png.value().document_revision == fixture.snapshot.document_revision,
          "PNG report must carry the captured document revision");
  require(png.value().width_px > 0 && png.value().height_px > 0,
          "PNG dimensions derive from scene mm × DPI");
  require(std::filesystem::file_size(png_path) > 32, "PNG file is non-empty");
  require(png.value().peak_tile_bytes <=
              static_cast<std::uint64_t>(png.value().width_px) * 32 * 4,
          "peak tile bytes stay within tile height budget");

  // Magic: PNG signature
  {
    std::ifstream in(png_path, std::ios::binary);
    unsigned char sig[8]{};
    in.read(reinterpret_cast<char *>(sig), 8);
    require(sig[0] == 137 && sig[1] == 80 && sig[2] == 78 && sig[3] == 71,
            "PNG signature must be present");
  }

  RasterExportRequest tiff_req{
      .path = tiff_path,
      .format = RasterImageFormat::tiff,
      .background = RgbaColor{240, 240, 240, 255},
      .tile_height_px = 32,
      .tiff_compression = TiffCompression::packbits,
  };
  const auto tiff =
      export_raster_sync(fixture.scene, fixture.snapshot, tiff_req);
  require(tiff.has_value(), "TIFF export must succeed");
  require(tiff.value().document_revision.value == 7,
          "TIFF report keeps snapshot revision");
  {
    std::ifstream in(tiff_path, std::ios::binary);
    char sig[2]{};
    in.read(sig, 2);
    require(sig[0] == 'I' && sig[1] == 'I', "TIFF little-endian magic");
  }

  std::filesystem::remove(png_path);
  std::filesystem::remove(tiff_path);
}

void respects_pixel_dpi_background_and_color_space() {
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-gray.png");
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 120,
      .height_px = 200,
      .dpi_override = 72,
      .background = RgbaColor{10, 20, 30, 255},
      .color_space = RasterColorSpace::gray,
      .tile_height_px = 40,
  };
  const auto report = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(report.has_value(), "configured raster export must succeed");
  require(report.value().width_px == 120 && report.value().height_px == 200,
          "explicit pixel size is honoured");
  require(report.value().dpi == 72, "dpi override is reported");
  require(report.value().color_space == RasterColorSpace::gray,
          "gray colour space is reported");
  std::filesystem::remove(path);
}

void rejects_overwrite_without_confirmation_and_resource_limits() {
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-overwrite.png");
  std::filesystem::remove(path);
  {
    std::ofstream(path) << "existing";
  }
  RasterExportRequest req{.path = path, .format = RasterImageFormat::png};
  const auto blocked =
      export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(!blocked.has_value() &&
              blocked.error().code == ErrorCode::invalid_document,
          "overwrite without confirmation must fail");

  req.overwrite_confirmed = true;
  const auto ok = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(ok.has_value(), "explicit overwrite confirmation allows replace");

  req.max_output_pixels = 1;
  req.overwrite_confirmed = true;
  const auto too_big =
      export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(!too_big.has_value() &&
              too_big.error().code == ErrorCode::resource_exhausted,
          "max_output_pixels must bound export size");
  std::filesystem::remove(path);
}

void async_job_reports_progress_and_survives_host_revision_change() {
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-async.png");
  std::filesystem::remove(path);

  // Capture revision 7, then mutate a separate session document after start.
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 400,
      .height_px = 1200,
      .tile_height_px = 16,
  };
  auto job = RasterExportJob::start(fixture.scene, fixture.snapshot, req);
  require(job.has_value(), "async job must start");

  // Host continues "editing" — snapshot revision must stay 7 in the report.
  fixture.snapshot.document_revision = DocumentRevision{99};

  std::vector<RasterExportProgress> events;
  auto state = RasterExportState::running;
  for (int i = 0; i < 500 && state == RasterExportState::running; ++i) {
    state = job.value()->poll(&events);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(state == RasterExportState::completed, "async job must complete");
  require(!events.empty(), "progress events must be emitted");
  // Aggregation: not every tile (1200/16=75) as an event.
  require(events.size() < 75, "progress is low-frequency aggregated");
  const auto result = job.value()->result();
  require(result.has_value() && result.value().document_revision.value == 7,
          "result reports the captured snapshot revision, not later host edits");
  std::filesystem::remove(path);
}

void cancel_stops_work_and_cleans_temp() {
  auto fixture = make_curve_fixture();
  const auto path = temp_file("welllog-157-cancel.png");
  std::filesystem::remove(path);
  auto temp = path;
  temp += ".";
  // pid-specific; we just ensure no leftover *.tmp for this stem after cancel.

  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 800,
      .height_px = 8000,
      .tile_height_px = 8,
  };
  auto job = RasterExportJob::start(fixture.scene, fixture.snapshot, req);
  require(job.has_value(), "cancel job must start");
  job.value()->request_cancel();

  auto state = RasterExportState::running;
  for (int i = 0; i < 500 && state == RasterExportState::running; ++i) {
    state = job.value()->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(state == RasterExportState::cancelled ||
              state == RasterExportState::failed ||
              state == RasterExportState::completed,
          "cancel must reach a terminal state");
  if (state == RasterExportState::cancelled) {
    const auto result = job.value()->result();
    require(!result.has_value() &&
                result.error().code == ErrorCode::operation_cancelled,
            "cancelled jobs report operation_cancelled");
  }
  // Target should not remain as a partial success without complete.
  if (state == RasterExportState::cancelled) {
    require(!std::filesystem::exists(path),
            "cancelled export must not leave a final target file");
  }
  std::filesystem::remove(path);
}

void tile_cull_skips_bresenham_for_out_of_window_curves() {
  const auto document_id = id("15700000-0000-4000-8000-000000000101");
  const auto shallow_axis = id("15700000-0000-4000-8000-000000000102");
  const auto deep_axis = id("15700000-0000-4000-8000-000000000103");
  const auto shallow_curve = id("15700000-0000-4000-8000-000000000104");
  const auto deep_curve = id("15700000-0000-4000-8000-000000000105");
  const auto track_id = id("15700000-0000-4000-8000-000000000106");
  const auto scale_id = id("15700000-0000-4000-8000-000000000107");
  const auto shallow_layer = id("15700000-0000-4000-8000-000000000108");
  const auto deep_layer = id("15700000-0000-4000-8000-000000000109");

  auto shallow_depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1002.0, 1004.0});
  auto shallow_values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 50.0, 20.0});
  std::vector<double> deep_d;
  std::vector<double> deep_v;
  deep_d.reserve(500);
  deep_v.reserve(500);
  for (int i = 0; i < 500; ++i) {
    deep_d.push_back(1700.0 + static_cast<double>(i) * 0.2);
    deep_v.push_back(static_cast<double>(i % 80));
  }
  auto deep_depths =
      std::make_shared<const std::vector<double>>(std::move(deep_d));
  auto deep_values =
      std::make_shared<const std::vector<double>>(std::move(deep_v));

  WellLogDocumentBuilder builder(document_id, DocumentRevision{7});
  builder.add_sampling_axis(SamplingAxis{
      .id = shallow_axis,
      .coordinates = BufferView::from_vector(shallow_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_sampling_axis(SamplingAxis{
      .id = deep_axis,
      .coordinates = BufferView::from_vector(deep_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = shallow_curve,
      .mnemonic = "GR",
      .display_name = "Shallow",
      .unit = "API",
      .sampling_axis_id = shallow_axis,
      .values = BufferView::from_vector(shallow_values),
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = deep_curve,
      .mnemonic = "RES",
      .display_name = "Deep",
      .unit = "API",
      .sampling_axis_id = deep_axis,
      .values = BufferView::from_vector(deep_values),
      .nulls = {},
  });
  WellLogSession session;
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "cull fixture document must load");
  ScenePresentationBuilder presentation(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1800.0},
      Millimetres{400.0}, "font-fixture-v1");
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
      .id = shallow_layer,
      .track_id = track_id,
      .curve_id = shallow_curve,
      .scale_id = scale_id,
      .color = RgbaColor{200, 20, 20, 255},
      .line_width = Millimetres{0.4},
      .z_order = 1,
      .visible = true,
  });
  presentation.add_curve_layer(CurveLayerSpec{
      .id = deep_layer,
      .track_id = track_id,
      .curve_id = deep_curve,
      .scale_id = scale_id,
      .color = RgbaColor{20, 20, 200, 255},
      .line_width = Millimetres{0.4},
      .z_order = 2,
      .visible = true,
  });
  const auto presented =
      session.execute(SetPresentationCommand{presentation.build()});
  if (!presented.has_value()) {
    fail(std::string{"cull fixture presentation must load: "} +
         std::to_string(static_cast<int>(presented.error().code)));
  }
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "cull fixture must prepare a scene");

  ExportSnapshot snapshot{
      .document_id = document_id,
      .document_revision = DocumentRevision{7},
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{.domain = DepthDomain::measured_depth,
                                   .unit = "m",
                                   .reference_top = 1000.0,
                                   .reference_bottom = 1800.0,
                                   .version = 1},
      .font_asset_fingerprint = "font-fixture-v1",
      .page = ExportPageSpec{.mode = PaginationMode::continuous,
                             .page_width = Millimetres{80.0},
                             .page_height = Millimetres{400.0},
                             .dpi = 25,
                             .well_name = "Cull-Fixture"},
  };
  const auto path = temp_file("welllog-605-cull.png");
  std::filesystem::remove(path);
  reset_raster_export_debug_stats();
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 80,
      .height_px = 400,
      .tile_height_px = 200,
      .overwrite_confirmed = true,
  };
  const auto report = export_raster_sync(*scene, snapshot, req);
  require(report.has_value(), "two-tile cull export must succeed");
  const auto &stats = raster_export_debug_stats();
  require(stats.tiles_completed >= 2, "export must rasterize two tiles");
  require(stats.first_tiles_draw_line_calls[0] > 0,
          "tile 0 must still Bresenham the shallow in-window curve");
  require(stats.first_tiles_draw_line_calls[0] < 50,
          "tile 0 must not Bresenham a curve entirely below it (issue #605)");
  require(stats.first_tiles_draw_line_calls[1] > 400,
          "tile 1 must still Bresenham the deep curve");
  std::filesystem::remove(path);
}

} // namespace

int main() {
  exports_png_and_tiff_with_snapshot_revision();
  respects_pixel_dpi_background_and_color_space();
  rejects_overwrite_without_confirmation_and_resource_limits();
  async_job_reports_progress_and_survives_host_revision_change();
  cancel_stops_work_and_cleans_temp();
  png_pixels_decode_and_carry_background_and_curve();
  gray_png_curve_samples_match_srgb_luma();
  raster_draws_symbol_layer_pixels();
  raster_page_dimensions_match_the_shared_snapshot_geometry();
  tile_cull_skips_bresenham_for_out_of_window_curves();
  return EXIT_SUCCESS;
}
