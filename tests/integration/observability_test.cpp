#include <welllog/session/observability.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void require_near(double a, double b, double tol, std::string_view message) {
  if (!(std::isfinite(a) && std::isfinite(b)) || std::abs(a - b) > tol) {
    std::cerr << "FAIL: " << message << " a=" << a << " b=" << b << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void percentile_helpers() {
  require_near(percentile_sorted({1.0, 2.0, 3.0, 4.0, 5.0}, 0.5), 3.0, 1e-9,
               "p50");
  require_near(percentile_sorted({1.0, 2.0, 3.0, 4.0, 5.0}, 0.0), 1.0, 1e-9,
               "p0");
  require_near(percentile_sorted({1.0, 2.0, 3.0, 4.0, 5.0}, 1.0), 5.0, 1e-9,
               "p100");
  require(percentile_sorted({}, 0.5) == 0.0, "empty");
}

void aggregator_rolling_stats() {
  FrameStatsAggregator agg(5);
  require(agg.size() == 0, "empty size");
  for (int i = 0; i < 5; ++i) {
    FrameSample s{};
    s.total_ms = 10.0 + i; // 10,11,12,13,14
    s.prepare_ms = 1.0;
    s.upload_ms = 2.0;
    s.draw_ms = 3.0;
    s.upload_bytes = 1000;
    s.batches = 2;
    s.vertices = 100;
    s.lod_points = 50;
    s.cache_hits = 1;
    s.worker_completed = static_cast<std::uint64_t>(i + 1);
    agg.push(s);
  }
  require(agg.size() == 5, "full");
  const auto stats = agg.aggregate();
  require(stats.sample_count == 5, "count");
  require_near(stats.frame_ms_p50, 12.0, 1e-6, "p50 mid");
  require(stats.fps > 0.0, "fps");
  require_near(stats.prepare_ms_avg, 1.0, 1e-9, "prep avg");
  require(stats.upload_bytes_avg == 1000, "upload avg");
  require(stats.cache_hits_total == 5, "hits");
  require(stats.worker_completed == 5, "workers last");

  // Ring overwrite
  FrameSample late{};
  late.total_ms = 100.0;
  late.prepare_ms = 5.0;
  agg.push(late);
  require(agg.size() == 5, "still capacity");
  const auto after = agg.aggregate();
  require(after.frame_ms_p99 >= 50.0, "p99 includes 100");
}

void chrome_trace_default_off_and_export() {
  ChromeTraceRecorder rec;
  require(!rec.enabled(), "default off");
  rec.complete("paintGL", "render", 0.0, 1000.0, "{\"upload_bytes\":12}");
  require(rec.event_count() == 0, "disabled drops events");
  rec.set_enabled(true);
  rec.complete("paintGL", "render", 10.0, 2000.0, "{\"upload_bytes\":12}");
  rec.instant("command", "session", 11.0, "{\"entity_id\":\"x\"}");
  require(rec.event_count() == 2, "two events");
  const auto json = rec.export_json();
  require(json.find("traceEvents") != std::string::npos, "traceEvents key");
  require(json.find("paintGL") != std::string::npos, "name");
  require(json.find("\"ph\":\"X\"") != std::string::npos, "complete phase");
  require(json.find("\"ph\":\"i\"") != std::string::npos, "instant phase");
  require(json.find("upload_bytes") != std::string::npos, "args");
  // Privacy: no accidental well-name style fields in exporter itself.
  require(json.find("well_name") == std::string::npos, "no well_name");
  require(json.find("curve_value") == std::string::npos, "no curve_value");
  require(!json.empty() && json.front() == '{' && json.back() == '}',
          "export must be a JSON object");
  require(json.find("\"traceEvents\":[") != std::string::npos,
          "traceEvents array must be present");
  rec.clear();
  const auto cleared = rec.export_json();
  require(cleared.find("\"traceEvents\":[]") != std::string::npos,
          "clear() must empty the traceEvents array");
}

void overlay_text_is_aggregate_only() {
  AggregatedFrameStats s{};
  s.fps = 60.0;
  s.frame_ms_p50 = 16.0;
  s.frame_ms_p95 = 20.0;
  s.frame_ms_p99 = 30.0;
  s.prepare_ms_avg = 1.0;
  s.upload_ms_avg = 2.0;
  s.draw_ms_avg = 3.0;
  s.present_ms_avg = 0.5;
  s.upload_bytes_avg = 4096;
  s.batches_avg = 4;
  s.vertices_avg = 1000;
  s.lod_points_avg = 500;
  s.sample_count = 10;
  const auto text = format_profiler_overlay(s);
  require(text.find("FPS") != std::string::npos, "fps label");
  require(text.find("Prep") != std::string::npos, "prep");
  require(text.find("Cache") != std::string::npos, "cache");
  require(text.find("Workers") != std::string::npos, "workers");
  require(text.find("Demo-Well") == std::string::npos, "no well content");
  require(text.find("GR") == std::string::npos || text.find("GR ") == std::string::npos,
          "no curve mnemonic");
}

} // namespace

int main() {
  percentile_helpers();
  aggregator_rolling_stats();
  chrome_trace_default_off_and_export();
  overlay_text_is_aggregate_only();
  return EXIT_SUCCESS;
}
