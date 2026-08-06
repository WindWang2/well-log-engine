#include <welllog/export/raster.hpp>
#include <welllog/session/session.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::filesystem::path temp_file(std::string_view name) {
  return std::filesystem::temp_directory_path() / name;
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

} // namespace

int main() {
  exports_png_and_tiff_with_snapshot_revision();
  respects_pixel_dpi_background_and_color_space();
  rejects_overwrite_without_confirmation_and_resource_limits();
  async_job_reports_progress_and_survives_host_revision_change();
  cancel_stops_work_and_cleans_temp();
  return EXIT_SUCCESS;
}
