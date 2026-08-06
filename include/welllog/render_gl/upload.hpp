#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <welllog/core/result.hpp>
#include <welllog/render_gl/export.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog {

struct GpuUploadBudgets {
  std::uint64_t maximum_cache_bytes{256ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_bytes_per_frame{4ULL * 1024ULL * 1024ULL};
};

struct GpuUploadChunk {
  std::uint64_t byte_offset{};
  std::uint64_t byte_count{};
};

class WELLLOG_RENDER_GL_API GpuUploadSchedule {
public:
  GpuUploadSchedule();
  ~GpuUploadSchedule();
  GpuUploadSchedule(const GpuUploadSchedule &);
  GpuUploadSchedule &operator=(const GpuUploadSchedule &);
  GpuUploadSchedule(GpuUploadSchedule &&) noexcept;
  GpuUploadSchedule &operator=(GpuUploadSchedule &&) noexcept;

  [[nodiscard]] static Result<GpuUploadSchedule>
  plan(const PreparedScene &scene, GpuUploadBudgets budgets) noexcept;

  [[nodiscard]] std::uint64_t source_segment_count() const noexcept;
  [[nodiscard]] std::uint64_t vertex_count() const noexcept;
  [[nodiscard]] std::uint64_t total_bytes() const noexcept;
  [[nodiscard]] std::uint64_t chunk_count() const noexcept;
  [[nodiscard]] std::span<const GpuUploadChunk> chunks() const noexcept;
  // Total prepared fill-region triangles the GL primitive stream will walk.
  // Used by parity tests asserting GL and SVG consume the same fill geometry.
  [[nodiscard]] std::uint64_t fill_triangle_count() const noexcept;
  // Total triangles the GL primitive stream will walk for custom-layer
  // primitives (polylines contribute 2 per segment, triangles 1, quads 2,
  // symbols a variable fan). Used by the custom-layer GL/SVG parity test.
  [[nodiscard]] std::uint64_t custom_triangle_count() const noexcept;

private:
  struct Impl;
  explicit GpuUploadSchedule(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

} // namespace welllog
