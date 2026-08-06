#include <welllog/render_gl/upload.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace welllog {
namespace {

constexpr std::uint64_t vertices_per_curve_segment = 6;
constexpr std::uint64_t floats_per_curve_vertex = 6;
constexpr std::uint64_t bytes_per_curve_vertex =
    floats_per_curve_vertex * sizeof(float);
constexpr std::uint64_t bytes_per_curve_segment =
    vertices_per_curve_segment * bytes_per_curve_vertex;

[[nodiscard]] Error upload_error(ErrorCode code, MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

} // namespace

struct GpuUploadSchedule::Impl {
  std::uint64_t source_segment_count{};
  std::uint64_t vertex_count{};
  std::uint64_t total_bytes{};
  std::uint64_t fill_triangle_count{};
  std::uint64_t custom_triangle_count{};
  std::vector<GpuUploadChunk> chunks;
};

GpuUploadSchedule::GpuUploadSchedule() = default;
GpuUploadSchedule::~GpuUploadSchedule() = default;
GpuUploadSchedule::GpuUploadSchedule(const GpuUploadSchedule &) = default;
GpuUploadSchedule &
GpuUploadSchedule::operator=(const GpuUploadSchedule &) = default;
GpuUploadSchedule::GpuUploadSchedule(GpuUploadSchedule &&) noexcept = default;
GpuUploadSchedule &
GpuUploadSchedule::operator=(GpuUploadSchedule &&) noexcept = default;

GpuUploadSchedule::GpuUploadSchedule(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

Result<GpuUploadSchedule>
GpuUploadSchedule::plan(const PreparedScene &scene,
                        GpuUploadBudgets budgets) noexcept {
  try {
    if (budgets.maximum_cache_bytes == 0 ||
        budgets.maximum_bytes_per_frame < bytes_per_curve_segment) {
      return upload_error(ErrorCode::invalid_buffer,
                          MessageKey::buffer_extent_exceeds_capacity);
    }
    std::uint64_t source_segments{};
    for (const auto &segment : scene.curve_segments()) {
      if (segment.point_count > 1) {
        const auto count = segment.point_count - 1;
        if (count >
            std::numeric_limits<std::uint64_t>::max() - source_segments) {
          return upload_error(ErrorCode::arithmetic_overflow,
                              MessageKey::buffer_extent_overflow);
        }
        source_segments += count;
      }
    }
    if (source_segments >
        std::numeric_limits<std::uint64_t>::max() / bytes_per_curve_segment) {
      return upload_error(ErrorCode::arithmetic_overflow,
                          MessageKey::buffer_extent_overflow);
    }
    const auto total_bytes = source_segments * bytes_per_curve_segment;
    if (total_bytes > budgets.maximum_cache_bytes) {
      return upload_error(ErrorCode::resource_exhausted,
                          MessageKey::resource_exhausted);
    }

    auto impl = std::make_shared<Impl>();
    impl->source_segment_count = source_segments;
    impl->vertex_count = source_segments * vertices_per_curve_segment;
    impl->total_bytes = total_bytes;
    // Account for crossover fill triangles so the schedule reports the same
    // primitive geometry the GL renderer walks (ADR 0036 parity).
    std::uint64_t fill_triangles{};
    for (const auto &layer : scene.fill_layers()) {
      for (std::uint64_t offset = 0; offset < layer.region_count; ++offset) {
        const auto &region = scene.fill_regions()[static_cast<std::size_t>(
            layer.first_region + offset)];
        if (fill_triangles >
            std::numeric_limits<std::uint64_t>::max() -
                region.triangle_count) {
          return upload_error(ErrorCode::arithmetic_overflow,
                              MessageKey::buffer_extent_overflow);
        }
        fill_triangles += region.triangle_count;
      }
    }
    impl->fill_triangle_count = fill_triangles;
    // Account for custom-layer primitives so the schedule reports the same
    // triangle geometry the GL primitive stream walks (ADR 0036 parity).
    // Symbols are approximated as a circle fan (24 triangles); non-circle
    // symbol kinds (square/diamond/...) emit a different count, so the parity
    // count is exact only for circles. The custom-layer parity test uses
    // circles.
    constexpr std::uint64_t custom_symbol_triangles = 24; // circle fan
    std::uint64_t custom_triangles{};
    for (const auto &layer : scene.custom_layers()) {
      for (std::uint64_t offset = 0; offset < layer.primitive_count; ++offset) {
        const auto &primitive = scene.custom_primitives()[static_cast<std::size_t>(
            layer.first_primitive + offset)];
        std::uint64_t contribution{};
        switch (primitive.kind) {
        case CustomPrimitiveKind::polyline: {
          if (primitive.vertex_count >= 2) {
            contribution = 2 * (primitive.vertex_count - 1);
            if (primitive.closed && primitive.vertex_count >= 3) {
              contribution += 2;
            }
          }
          break;
        }
        case CustomPrimitiveKind::triangle:
        case CustomPrimitiveKind::quad:
          // Both store clipped, triangulated geometry: vertex_count / 3
          // triangles (exact, including post-clip triangle counts).
          contribution = primitive.vertex_count / 3;
          break;
        case CustomPrimitiveKind::symbol:
          contribution = custom_symbol_triangles;
          break;
        }
        if (contribution >
            std::numeric_limits<std::uint64_t>::max() - custom_triangles) {
          return upload_error(ErrorCode::arithmetic_overflow,
                              MessageKey::buffer_extent_overflow);
        }
        custom_triangles += contribution;
      }
    }
    impl->custom_triangle_count = custom_triangles;
    const auto chunk_capacity =
        budgets.maximum_bytes_per_frame -
        budgets.maximum_bytes_per_frame % bytes_per_curve_segment;
    const auto chunk_count = total_bytes == 0
                                 ? std::uint64_t{}
                                 : 1 + (total_bytes - 1) / chunk_capacity;
    impl->chunks.reserve(static_cast<std::size_t>(chunk_count));
    for (std::uint64_t offset = 0; offset < total_bytes;
         offset += chunk_capacity) {
      impl->chunks.push_back(GpuUploadChunk{
          .byte_offset = offset,
          .byte_count = std::min(chunk_capacity, total_bytes - offset),
      });
    }
    return GpuUploadSchedule{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return upload_error(ErrorCode::resource_exhausted,
                        MessageKey::resource_exhausted);
  } catch (...) {
    return upload_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

std::uint64_t GpuUploadSchedule::source_segment_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->source_segment_count;
}

std::uint64_t GpuUploadSchedule::vertex_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->vertex_count;
}

std::uint64_t GpuUploadSchedule::total_bytes() const noexcept {
  return impl_ == nullptr ? 0 : impl_->total_bytes;
}

std::uint64_t GpuUploadSchedule::fill_triangle_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->fill_triangle_count;
}

std::uint64_t GpuUploadSchedule::custom_triangle_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->custom_triangle_count;
}

std::uint64_t GpuUploadSchedule::chunk_count() const noexcept {
  return impl_ == nullptr ? 0
                          : static_cast<std::uint64_t>(impl_->chunks.size());
}

std::span<const GpuUploadChunk> GpuUploadSchedule::chunks() const noexcept {
  return impl_ == nullptr ? std::span<const GpuUploadChunk>{}
                          : std::span<const GpuUploadChunk>{impl_->chunks};
}

} // namespace welllog
