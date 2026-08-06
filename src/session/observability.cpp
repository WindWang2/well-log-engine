#include <welllog/session/observability.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace welllog {
namespace {

[[nodiscard]] double mean_of(const std::vector<double> &values) noexcept {
  if (values.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (const auto v : values) {
    sum += v;
  }
  return sum / static_cast<double>(values.size());
}

} // namespace

FrameStatsAggregator::FrameStatsAggregator(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
  samples_.resize(capacity_);
}

void FrameStatsAggregator::clear() noexcept {
  next_ = 0;
  count_ = 0;
}

void FrameStatsAggregator::push(const FrameSample &sample) noexcept {
  if (samples_.empty()) {
    return;
  }
  samples_[next_] = sample;
  next_ = (next_ + 1) % capacity_;
  if (count_ < capacity_) {
    ++count_;
  }
}

std::size_t FrameStatsAggregator::size() const noexcept { return count_; }

std::size_t FrameStatsAggregator::capacity() const noexcept {
  return capacity_;
}

AggregatedFrameStats FrameStatsAggregator::aggregate() const noexcept {
  AggregatedFrameStats out{};
  if (count_ == 0) {
    return out;
  }
  out.sample_count = count_;
  std::vector<double> totals;
  std::vector<double> prepare;
  std::vector<double> upload;
  std::vector<double> draw;
  std::vector<double> present;
  std::vector<double> gpu;
  totals.reserve(count_);
  prepare.reserve(count_);
  upload.reserve(count_);
  draw.reserve(count_);
  present.reserve(count_);
  std::uint64_t upload_bytes_sum = 0;
  std::uint64_t batches_sum = 0;
  std::uint64_t vertices_sum = 0;
  std::uint64_t lod_sum = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    // Ring order: when full, oldest is at next_ (next write slot).
    const auto idx =
        count_ < capacity_ ? i : ((next_ + i) % capacity_);
    const auto &s = samples_[idx];
    totals.push_back(s.total_ms > 0.0
                         ? s.total_ms
                         : (s.prepare_ms + s.upload_ms + s.draw_ms +
                            s.present_ms));
    prepare.push_back(s.prepare_ms);
    upload.push_back(s.upload_ms);
    draw.push_back(s.draw_ms);
    present.push_back(s.present_ms);
    if (s.gpu_pass_valid) {
      gpu.push_back(s.gpu_pass_ms);
    }
    upload_bytes_sum += s.upload_bytes;
    batches_sum += s.batches;
    vertices_sum += s.vertices;
    lod_sum += s.lod_points;
    out.cache_hits_total += s.cache_hits;
    out.cache_misses_total += s.cache_misses;
    out.cache_evictions_total += s.cache_evictions;
    // Worker counters are cumulative snapshots on the last sample.
    out.worker_completed = s.worker_completed;
    out.worker_cancelled = s.worker_cancelled;
    out.worker_discarded = s.worker_discarded;
    out.diagnostics_count = s.diagnostics_count;
  }
  out.frame_ms_p50 = percentile_sorted(totals, 0.50);
  out.frame_ms_p95 = percentile_sorted(totals, 0.95);
  out.frame_ms_p99 = percentile_sorted(totals, 0.99);
  if (out.frame_ms_p50 > 1e-9) {
    out.fps = 1000.0 / out.frame_ms_p50;
  }
  out.prepare_ms_avg = mean_of(prepare);
  out.upload_ms_avg = mean_of(upload);
  out.draw_ms_avg = mean_of(draw);
  out.present_ms_avg = mean_of(present);
  out.gpu_pass_ms_avg = mean_of(gpu);
  const auto n = static_cast<double>(count_);
  out.upload_bytes_avg = static_cast<std::uint64_t>(
      std::llround(static_cast<double>(upload_bytes_sum) / n));
  out.batches_avg = static_cast<std::uint64_t>(
      std::llround(static_cast<double>(batches_sum) / n));
  out.vertices_avg = static_cast<std::uint64_t>(
      std::llround(static_cast<double>(vertices_sum) / n));
  out.lod_points_avg = static_cast<std::uint64_t>(
      std::llround(static_cast<double>(lod_sum) / n));
  return out;
}

ChromeTraceRecorder::ChromeTraceRecorder() = default;

void ChromeTraceRecorder::set_enabled(bool enabled) noexcept {
  enabled_ = enabled;
  if (!enabled_) {
    // Keep history when disabling so export still works for the last capture.
  }
}

bool ChromeTraceRecorder::enabled() const noexcept { return enabled_; }

void ChromeTraceRecorder::clear() noexcept { events_.clear(); }

void ChromeTraceRecorder::complete(std::string_view name,
                                   std::string_view category, double ts_us,
                                   double dur_us,
                                   std::string_view args_json) noexcept {
  if (!enabled_) {
    return;
  }
  try {
    events_.push_back(Event{
        .name = std::string{name},
        .category = std::string{category},
        .phase = 'X',
        .ts_us = ts_us,
        .dur_us = dur_us,
        .args_json = std::string{args_json.empty() ? "{}" : args_json},
    });
  } catch (...) {
  }
}

void ChromeTraceRecorder::instant(std::string_view name,
                                  std::string_view category, double ts_us,
                                  std::string_view args_json) noexcept {
  if (!enabled_) {
    return;
  }
  try {
    events_.push_back(Event{
        .name = std::string{name},
        .category = std::string{category},
        .phase = 'i',
        .ts_us = ts_us,
        .dur_us = 0.0,
        .args_json = std::string{args_json.empty() ? "{}" : args_json},
    });
  } catch (...) {
  }
}

std::size_t ChromeTraceRecorder::event_count() const noexcept {
  return events_.size();
}

std::string ChromeTraceRecorder::export_json() const {
  std::ostringstream out;
  out << "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[";
  for (std::size_t i = 0; i < events_.size(); ++i) {
    const auto &e = events_[i];
    if (i > 0) {
      out << ',';
    }
    // Escape is unnecessary for our fixed token names; args_json is caller-owned
    // numeric JSON.
    out << "{\"name\":\"" << e.name << "\",\"cat\":\"" << e.category
        << "\",\"ph\":\"" << e.phase << "\",\"ts\":" << e.ts_us
        << ",\"pid\":1,\"tid\":1";
    if (e.phase == 'X') {
      out << ",\"dur\":" << e.dur_us;
    } else {
      out << ",\"s\":\"t\"";
    }
    out << ",\"args\":" << e.args_json << '}';
  }
  out << "]}";
  return out.str();
}

std::string format_profiler_overlay(const AggregatedFrameStats &stats) noexcept {
  try {
    char line[512];
    std::string text;
    std::snprintf(line, sizeof(line),
                  "FPS %.1f  frame P50/P95/P99  %.2f / %.2f / %.2f ms\n",
                  stats.fps, stats.frame_ms_p50, stats.frame_ms_p95,
                  stats.frame_ms_p99);
    text += line;
    std::snprintf(line, sizeof(line),
                  "Prep %.2f  Up %.2f  Draw %.2f  Present %.2f  GPU %.2f ms\n",
                  stats.prepare_ms_avg, stats.upload_ms_avg, stats.draw_ms_avg,
                  stats.present_ms_avg, stats.gpu_pass_ms_avg);
    text += line;
    std::snprintf(line, sizeof(line),
                  "Up %.1f KB  Batches %llu  Vtx %llu  LOD pts %llu\n",
                  static_cast<double>(stats.upload_bytes_avg) / 1024.0,
                  static_cast<unsigned long long>(stats.batches_avg),
                  static_cast<unsigned long long>(stats.vertices_avg),
                  static_cast<unsigned long long>(stats.lod_points_avg));
    text += line;
    std::snprintf(
        line, sizeof(line),
        "Cache hit/miss/evict %llu / %llu / %llu\n",
        static_cast<unsigned long long>(stats.cache_hits_total),
        static_cast<unsigned long long>(stats.cache_misses_total),
        static_cast<unsigned long long>(stats.cache_evictions_total));
    text += line;
    std::snprintf(
        line, sizeof(line),
        "Workers done/cancel/discard %llu / %llu / %llu  Diag %llu  n=%llu",
        static_cast<unsigned long long>(stats.worker_completed),
        static_cast<unsigned long long>(stats.worker_cancelled),
        static_cast<unsigned long long>(stats.worker_discarded),
        static_cast<unsigned long long>(stats.diagnostics_count),
        static_cast<unsigned long long>(stats.sample_count));
    text += line;
    return text;
  } catch (...) {
    return "profiler unavailable";
  }
}

double percentile_sorted(std::vector<double> values, double p) noexcept {
  if (values.empty()) {
    return 0.0;
  }
  p = std::clamp(p, 0.0, 1.0);
  std::sort(values.begin(), values.end());
  if (values.size() == 1) {
    return values.front();
  }
  const auto rank = p * static_cast<double>(values.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  if (lo == hi) {
    return values[lo];
  }
  const auto t = rank - static_cast<double>(lo);
  return values[lo] * (1.0 - t) + values[hi] * t;
}

double chrome_trace_now_us() noexcept {
  using clock = std::chrono::steady_clock;
  static const auto origin = clock::now();
  return std::chrono::duration<double, std::micro>(clock::now() - origin)
      .count();
}

} // namespace welllog
