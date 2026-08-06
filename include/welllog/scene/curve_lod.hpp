#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/scene/export.hpp>

namespace welllog {

enum class CurveLodAlgorithm : std::uint8_t {
  scalar_reference,
  hierarchical,
};

struct CurveLodBuildOptions {
  CurveLodAlgorithm algorithm{CurveLodAlgorithm::hierarchical};
  std::uint64_t base_bucket_samples{16};
  std::uint64_t maximum_derived_bytes{};
};

struct CurveLodQuery {
  double viewport_top{};
  double viewport_bottom{};
  std::uint32_t pixel_height{};
  double prefetch_viewports{2.0};
};

struct CurveLodPoint {
  std::uint64_t sample_index{};
  double reference_depth{};
  double value{};
};

struct CurveLodSegment {
  std::uint64_t first_point{};
  std::uint64_t point_count{};
};

struct CurveLodStatistics {
  std::uint64_t source_samples{};
  std::uint64_t source_bytes{};
  std::uint64_t derived_bytes{};
  std::uint64_t maximum_derived_bytes{};
  std::uint32_t level_count{};
  bool budget_limited{};
};

class WELLLOG_SCENE_API CurveLodSelection {
public:
  CurveLodSelection();
  ~CurveLodSelection();
  CurveLodSelection(const CurveLodSelection &);
  CurveLodSelection &operator=(const CurveLodSelection &);
  CurveLodSelection(CurveLodSelection &&) noexcept;
  CurveLodSelection &operator=(CurveLodSelection &&) noexcept;

  [[nodiscard]] bool uses_raw_samples() const noexcept;
  [[nodiscard]] std::uint64_t bucket_samples() const noexcept;
  [[nodiscard]] std::span<const CurveLodPoint> points() const noexcept;
  [[nodiscard]] std::span<const CurveLodSegment> segments() const noexcept;

private:
  struct Impl;
  explicit CurveLodSelection(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend class CurveLodPyramid;
};

class WELLLOG_SCENE_API CurveLodPyramid {
public:
  CurveLodPyramid();
  ~CurveLodPyramid();
  CurveLodPyramid(const CurveLodPyramid &);
  CurveLodPyramid &operator=(const CurveLodPyramid &);
  CurveLodPyramid(CurveLodPyramid &&) noexcept;
  CurveLodPyramid &operator=(CurveLodPyramid &&) noexcept;

  [[nodiscard]] static Result<CurveLodPyramid>
  build(const SamplingAxis &axis, const Curve &curve,
        CurveLodBuildOptions options = {},
        std::stop_token stop_token = {}) noexcept;
  // Incrementally tail-extends a pyramid built over the shorter `previous_axis`
  // / `previous_curve` onto the longer `extended_axis` / `extended_curve` (#199,
  // ADR 0031 "LOD 只增量更新受影响尾块"). The earlier SourceRuns — every run
  // except possibly the last — are reused byte-for-byte (their sample ranges
  // are untouched by a tail-append, so their level summaries are unchanged);
  // only the last run (whose end extended) and any new tail runs are
  // re-derived. The result is identical (envelope values + derived-byte
  // accounting) to a full `build` over the extended curve, but skips
  // re-derivation of the unchanged earlier region.
  //
  // Preconditions: the axis/curve ids match `previous`; the extended curve is
  // the previous curve with samples appended (the first `previous` samples are
  // numerically equal — an append, not an edit); the options match those used
  // to build `previous` (algorithm + base_bucket_samples). A violation returns
  // invalid_document; the caller should fall back to a full `build`.
  [[nodiscard]] static Result<CurveLodPyramid>
  extend_tail(const CurveLodPyramid &previous, const SamplingAxis &extended_axis,
              const Curve &extended_curve,
              CurveLodBuildOptions options = {},
              std::stop_token stop_token = {}) noexcept;
  [[nodiscard]] Result<CurveLodSelection>
  query(const CurveLodQuery &query,
        std::stop_token stop_token = {}) const noexcept;
  [[nodiscard]] CurveLodStatistics statistics() const noexcept;

private:
  struct Impl;
  explicit CurveLodPyramid(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

} // namespace welllog
