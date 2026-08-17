#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <welllog/render_gl/export.hpp>
#include <welllog/render_gl/upload.hpp>
#include <welllog/io/manifest.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog::detail {

using GlProcAddress = void (*)();
using GlProcResolver = GlProcAddress (*)(void *context,
                                         const char *name) noexcept;

struct GlDepthViewport {
  double top{};
  double bottom{};
};

struct GlCrosshair {
  double horizontal_fraction{};
  double display_depth{};
};

struct GlRenderFrame {
  std::uint32_t framebuffer{};
  int pixel_width{};
  int pixel_height{};
  double physical_pixels_per_millimetre{};
  GlDepthViewport viewport;
  std::optional<GlCrosshair> crosshair;
  bool draw_scene{};
};

struct GlUploadProgress {
  std::uint64_t bytes_uploaded{};
  std::uint64_t total_bytes{};
  bool pending{};
  bool completed{};
};

// Observability for atlas reuse tests (issue #606). Counters live in this
// header so a stashed renderer.cpp still links; they stay zero unless
// queue_upload / upload_next increment them.
struct GlAtlasDebugStats {
  std::uint64_t atlas_bytes_copied{};
  std::uint64_t tex_image_2d_calls{};
};

inline GlAtlasDebugStats &gl_atlas_debug_stats() noexcept {
  static GlAtlasDebugStats stats{};
  return stats;
}

inline void reset_gl_atlas_debug_stats() noexcept {
  gl_atlas_debug_stats() = {};
}

class WELLLOG_RENDER_GL_API GlRenderer {
public:
  GlRenderer();
  ~GlRenderer();
  GlRenderer(GlRenderer &&) noexcept;
  GlRenderer &operator=(GlRenderer &&) noexcept;
  GlRenderer(const GlRenderer &) = delete;
  GlRenderer &operator=(const GlRenderer &) = delete;

  [[nodiscard]] bool initialize(GlProcResolver resolver,
                                void *resolver_context) noexcept;
  [[nodiscard]] bool upload(const PreparedScene &scene) noexcept;
  [[nodiscard]] bool queue_upload(const PreparedScene &scene,
                                  GpuUploadBudgets budgets) noexcept;
  [[nodiscard]] GlUploadProgress upload_next() noexcept;
  [[nodiscard]] bool render(const GlRenderFrame &frame) noexcept;
  // Supplies the host-side image-tile decoder (ADR 0042: the engine never
  // decodes images; it uploads pixels the host provides per visible tile).
  // Set to nullptr to disable image rendering.
  void set_image_tile_resolver(
      std::function<Result<RasterTile>(const ImageTileRequest &)> resolver,
      std::uint64_t maximum_texture_bytes) noexcept;
  void release() noexcept;
  void abandon() noexcept;
  [[nodiscard]] bool initialized() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace welllog::detail
