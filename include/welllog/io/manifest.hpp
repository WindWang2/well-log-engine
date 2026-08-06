#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/export.hpp>

namespace welllog {

inline constexpr std::uint32_t manifest_schema_version = 2;
inline constexpr std::string_view welllog_sdk_version = "0.1.0";
inline constexpr std::string_view manifest_sdk_requirement = ">=0.1.0 <1.0.0";

struct BufferDescriptor {
  BufferSourceReference source;
  std::uint64_t length{};
  std::uint64_t stride_bytes{};
  ScalarType scalar_type{ScalarType::float64};
  std::uint64_t byte_capacity{};
};

struct NullBitmapDescriptor {
  BufferSourceReference source;
  std::uint64_t bit_length{};
  std::uint64_t byte_capacity{};
};

// A request for one decoded raster tile from an ImageSource, identified by its
// pyramid level and tile grid coordinates. The host resolves this into the
// pre-decoded pixel bytes (the engine never decodes images — ADR 0042).
struct ImageTileRequest {
  EntityId image_source_id;
  std::uint32_t level{};
  std::uint32_t row{};
  std::uint32_t col{};
};

// The decoded pixels for one tile. `data` points to width*height*channels
// pixels in row-major order; `owner` keeps the storage alive (non-owning view
// model, ADR 0032).
struct RasterTile {
  std::uint32_t width_px{};
  std::uint32_t height_px{};
  PixelFormat pixel_format{PixelFormat::rgba8};
  SharedOwner owner;
  const std::uint8_t *data{nullptr};

  [[nodiscard]] std::uint64_t byte_size() const noexcept {
    const auto channels = pixel_format == PixelFormat::rgba8 ? 4
                        : pixel_format == PixelFormat::rgb8 ? 3 : 1;
    return static_cast<std::uint64_t>(width_px) *
           static_cast<std::uint64_t>(height_px) *
           static_cast<std::uint64_t>(channels);
  }
};

struct ManifestResolvers {
  std::function<Result<BufferView>(const BufferDescriptor &)> buffer;
  std::function<Result<NullBitmapView>(const NullBitmapDescriptor &)>
      null_bitmap;
  // Resolves a decoded image tile on demand (host-side decode, ADR 0042).
  std::function<Result<RasterTile>(const ImageTileRequest &)> image_tile;
};

class WELLLOG_IO_API ManifestText {
public:
  ManifestText();
  ~ManifestText();
  ManifestText(const ManifestText &);
  ManifestText &operator=(const ManifestText &);
  ManifestText(ManifestText &&) noexcept;
  ManifestText &operator=(ManifestText &&) noexcept;

  [[nodiscard]] std::string_view text() const noexcept;

private:
  struct Impl;
  explicit ManifestText(std::string text);
  std::shared_ptr<const Impl> impl_;
  friend class ManifestCodec;
};

class WELLLOG_IO_API ManifestCodec {
public:
  [[nodiscard]] static Result<ManifestText>
  write(const WellLogDocument &document);
  [[nodiscard]] static Result<WellLogDocument>
  read(std::string_view manifest, const ManifestResolvers &resolvers);
};

} // namespace welllog
