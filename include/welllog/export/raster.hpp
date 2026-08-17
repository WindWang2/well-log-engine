#pragma once

// Asynchronous high-resolution PNG/TIFF export (#157, ADR 0021,
// table-and-export.md §8.3 / §10).
//
// Consumes the same PreparedScene + ExportSnapshot surface as PDF/SVG
// (physical layout, Document Scale baked into scene mm, snapshot revision).
// Renders on a CPU worker in height-bounded tiles — never via
// QOpenGLWidget framebuffer readback. Progress is low-frequency; cancel
// stops subsequent tiles and removes temp files; successful output is
// written beside the target then atomically renamed.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <welllog/core/result.hpp>
#include <welllog/core/units.hpp>
#include <welllog/export/pagination.hpp>
#include <welllog/scene/scene.hpp>

#if defined(_WIN32) && defined(WELLLOG_EXPORT_RASTER_SHARED)
#if defined(WELLLOG_EXPORT_RASTER_BUILD)
#define WELLLOG_EXPORT_RASTER_API __declspec(dllexport)
#else
#define WELLLOG_EXPORT_RASTER_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_EXPORT_RASTER_SHARED)
#define WELLLOG_EXPORT_RASTER_API __attribute__((visibility("default")))
#else
#define WELLLOG_EXPORT_RASTER_API
#endif

namespace welllog {

enum class RasterImageFormat : std::uint8_t {
  png,
  tiff,
};

// Output colour interpretation. Phase-one writers emit 8-bit samples; gray
// stores Y = round(0.2126 R + 0.7152 G + 0.0722 B) of the sRGB tile.
enum class RasterColorSpace : std::uint8_t {
  srgb,
  gray,
};

enum class TiffCompression : std::uint8_t {
  none,
  packbits,
};

// Host-controlled raster parameters on top of ExportSnapshot.page (which
// carries DPI, page geometry, and export mode shared with PDF/SVG).
struct RasterExportRequest {
  std::filesystem::path path;
  RasterImageFormat format{RasterImageFormat::png};
  // 0 → derive from scene physical size × effective DPI (snapshot.page.dpi
  // unless dpi_override is non-zero).
  std::uint32_t width_px{};
  std::uint32_t height_px{};
  // 0 → use snapshot.page.dpi. Non-zero overrides the export raster density
  // used for pixel sizing without mutating the captured snapshot.
  std::uint32_t dpi_override{};
  RgbaColor background{255, 255, 255, 255};
  RasterColorSpace color_space{RasterColorSpace::srgb};
  // Tile height bounds peak working-set (width × tile_height × channels).
  std::uint32_t tile_height_px{256};
  TiffCompression tiff_compression{TiffCompression::none};
  // Overwrite must be an explicit host decision (table-and-export.md §10).
  bool overwrite_confirmed{false};
  // Safety ceilings for untrusted export dimensions.
  std::uint64_t max_output_pixels{100'000'000ULL};
  std::uint64_t max_tile_bytes{64ULL * 1024ULL * 1024ULL};
};

enum class RasterExportState : std::uint8_t {
  running,
  completed,
  failed,
  cancelled,
};

// Low-frequency progress sample (aggregated: only when fraction advances by
// ≥5% or the job reaches a terminal state).
struct RasterExportProgress {
  double fraction{}; // [0, 1]
  std::uint32_t tiles_completed{};
  std::uint32_t tiles_total{};
};

struct RasterExportReport {
  std::filesystem::path path;
  RasterImageFormat format{RasterImageFormat::png};
  std::uint32_t width_px{};
  std::uint32_t height_px{};
  std::uint32_t dpi{};
  RasterColorSpace color_space{RasterColorSpace::srgb};
  // The snapshot revision the export was produced against — host edits after
  // start do not change this report.
  DocumentRevision document_revision{};
  PresentationVersion presentation_version{};
  EntityId document_id{};
  std::uint64_t peak_tile_bytes{};
  // Human-readable export notes (e.g. layers the raster backend cannot
  // render synchronously — image tiles need a pixel resolver; dash patterns
  // approximate solid). Empty when nothing was skipped.
  std::vector<std::string> notes;
};

// Owns a captured scene + snapshot and a worker thread. The host keeps the
// GUI responsive by polling; cancel is cooperative between tiles.
class WELLLOG_EXPORT_RASTER_API RasterExportJob {
public:
  // Validates request, captures scene/snapshot by value, rejects overwrite
  // without confirmation, then starts the worker. The PreparedScene is
  // reference-counted and immutable after prepare — concurrent host edits
  // build a new scene without mutating this capture.
  [[nodiscard]] static Result<std::shared_ptr<RasterExportJob>>
  start(PreparedScene scene, ExportSnapshot snapshot,
        RasterExportRequest request);

  RasterExportJob(const RasterExportJob &) = delete;
  RasterExportJob &operator=(const RasterExportJob &) = delete;
  ~RasterExportJob();

  void request_cancel() noexcept;

  // Observes worker progress, drains aggregated progress events into
  // `out_progress` (may be empty), and returns the current state.
  [[nodiscard]] RasterExportState
  poll(std::vector<RasterExportProgress> *out_progress = nullptr) noexcept;

  [[nodiscard]] RasterExportState state() const noexcept;
  [[nodiscard]] double progress_fraction() const noexcept;

  // Valid once state is completed or failed. Cancelled returns
  // operation_cancelled; still-running returns internal_error.
  [[nodiscard]] Result<RasterExportReport> result() const;

private:
  struct Impl;
  explicit RasterExportJob(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Synchronous helper for tests and hosts that do not need asynchrony. Still
// uses tiled rendering + atomic write; blocks the calling thread.
[[nodiscard]] WELLLOG_EXPORT_RASTER_API Result<RasterExportReport>
export_raster_sync(const PreparedScene &scene, const ExportSnapshot &snapshot,
                   const RasterExportRequest &request) noexcept;

// Observability for tile-window culling tests (issue #605). One process-wide
// instance lives in raster_export.cpp so shared-library builds share counters
// with the test binary.
struct RasterExportDebugStats {
  std::uint64_t draw_line_calls{};
  std::uint64_t tiles_completed{};
  std::array<std::uint64_t, 2> first_tiles_draw_line_calls{};
};

[[nodiscard]] WELLLOG_EXPORT_RASTER_API RasterExportDebugStats &
raster_export_debug_stats() noexcept;
WELLLOG_EXPORT_RASTER_API void reset_raster_export_debug_stats() noexcept;

} // namespace welllog
