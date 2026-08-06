#pragma once

// Built-in performance observability (#168, ADR 0043).
// Aggregated frame timings + optional Chrome Trace Event export. Metrics use
// Entity IDs and counts only — never well names, labels, or curve values.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <welllog/session/export.hpp>

namespace welllog {

// One completed frame's CPU-side phase timings and counters.
struct FrameSample {
  // Wall times in milliseconds for the GUI-thread paint phases.
  double prepare_ms{};
  double upload_ms{};
  double draw_ms{};
  double present_ms{}; // residual present / frame bookkeeping
  double total_ms{};

  std::uint64_t upload_bytes{};
  std::uint64_t batches{};
  std::uint64_t vertices{};
  std::uint64_t lod_points{};
  std::uint64_t cache_hits{};
  std::uint64_t cache_misses{};
  std::uint64_t cache_evictions{};
  std::uint64_t worker_completed{};
  std::uint64_t worker_cancelled{};
  std::uint64_t worker_discarded{};
  std::uint64_t diagnostics_count{};
  // Optional GPU pass time when timer queries are available (async, may lag).
  double gpu_pass_ms{};
  bool gpu_pass_valid{};
};

// Rolling-window aggregates for overlay / Python poll (not per-frame events).
struct AggregatedFrameStats {
  std::uint64_t sample_count{};
  double fps{};
  double frame_ms_p50{};
  double frame_ms_p95{};
  double frame_ms_p99{};
  double prepare_ms_avg{};
  double upload_ms_avg{};
  double draw_ms_avg{};
  double present_ms_avg{};
  double gpu_pass_ms_avg{};
  std::uint64_t upload_bytes_avg{};
  std::uint64_t batches_avg{};
  std::uint64_t vertices_avg{};
  std::uint64_t lod_points_avg{};
  std::uint64_t cache_hits_total{};
  std::uint64_t cache_misses_total{};
  std::uint64_t cache_evictions_total{};
  std::uint64_t worker_completed{};
  std::uint64_t worker_cancelled{};
  std::uint64_t worker_discarded{};
  std::uint64_t diagnostics_count{};
};

// Rolling ring of FrameSample used by the view each paintGL.
class WELLLOG_SESSION_API FrameStatsAggregator {
public:
  explicit FrameStatsAggregator(std::size_t capacity = 120);

  void clear() noexcept;
  void push(const FrameSample &sample) noexcept;
  [[nodiscard]] AggregatedFrameStats aggregate() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;

private:
  std::vector<FrameSample> samples_;
  std::size_t capacity_{120};
  std::size_t next_{0};
  std::size_t count_{0};
};

// Chrome Trace Event (JSON) recorder. Disabled by default (detailed path).
// Event names/categories are fixed English tokens; args are numeric / entity
// id strings only.
class WELLLOG_SESSION_API ChromeTraceRecorder {
public:
  ChromeTraceRecorder();

  void set_enabled(bool enabled) noexcept;
  [[nodiscard]] bool enabled() const noexcept;
  void clear() noexcept;

  // Complete event (duration from begin to end of a scope).
  void complete(std::string_view name, std::string_view category, double ts_us,
                double dur_us,
                std::string_view args_json = "{}") noexcept;

  // Instant event.
  void instant(std::string_view name, std::string_view category, double ts_us,
               std::string_view args_json = "{}") noexcept;

  // Full document JSON suitable for chrome://tracing or Perfetto.
  [[nodiscard]] std::string export_json() const;

  [[nodiscard]] std::size_t event_count() const noexcept;

private:
  struct Event {
    std::string name;
    std::string category;
    char phase{'X'}; // X complete, i instant
    double ts_us{};
    double dur_us{};
    std::string args_json;
  };
  bool enabled_{false};
  std::vector<Event> events_;
};

// Human-readable multi-line overlay text (no host content / well names).
[[nodiscard]] WELLLOG_SESSION_API std::string
format_profiler_overlay(const AggregatedFrameStats &stats) noexcept;

// Percentile of a sorted ascending copy of *values* (p in [0,1]).
[[nodiscard]] WELLLOG_SESSION_API double
percentile_sorted(std::vector<double> values, double p) noexcept;

// Steady-clock microseconds since an arbitrary origin (trace timestamps).
[[nodiscard]] WELLLOG_SESSION_API double chrome_trace_now_us() noexcept;

} // namespace welllog
