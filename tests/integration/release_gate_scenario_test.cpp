// #174 — ADR 0014 reference-scenario structural + memory gate (headless).
//
// CI profile (default): reduced multi-well scene that still exercises layout,
// multi-curve tracks, discrete objects, LOD prep, pick, and memory ratio.
// Full profile (WELLLOG_GATE_SCALE=full): 20 wells × 10 curves × 500k samples
// = 100M scalars + 100k markers (heavy; for fixed-workstation acceptance only).
//
// Shared CI must NOT assert absolute frame P95 (ADR 0014 / QSP §4.3). Frame
// SLOs are measured by the optional GL dense_curve_benchmark on ref HW.

#include <welllog/export/svg.hpp>
#include <welllog/render_gl/upload.hpp>
#include <welllog/session/session.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace welllog;
using Clock = std::chrono::steady_clock;

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

EntityId id(std::string_view text) { return EntityId::parse(text).value(); }

// Deterministic UUID-ish strings for N wells (hex only).
std::string hex_id(std::uint32_t well, std::uint32_t slot) {
  std::ostringstream oss;
  oss << "d174" << std::hex << std::setfill('0') << std::setw(4) << well
      << "-0000-4000-8000-" << std::setw(12) << slot;
  return oss.str();
}

struct GateScale {
  std::size_t well_count{};
  std::size_t curves_per_well{};
  std::size_t samples_per_curve{};
  std::size_t tracks_per_well{}; // visible tracks per well (capped by curves)
  std::size_t markers_per_well{};
  std::size_t pixel_height{2160};
  bool enforce_first_interactive_ms{false};
  double first_interactive_ms_limit{2000.0};
  const char *name{"ci"};
};

GateScale resolve_scale() {
  const char *env = std::getenv("WELLLOG_GATE_SCALE");
  const std::string_view mode = env == nullptr ? "ci" : env;
  if (mode == "full" || mode == "FULL" || mode == "1e8") {
    // ADR 0014: 20 wells, 200 curves total, 500k/curve → 100M samples,
    // ~100 visible tracks (5/well), 100k discrete (5k markers/well).
    // Absolute first-interactive ≤2s is opt-in (WELLLOG_GATE_ENFORCE_SLO=1)
    // for ADR 0014 reference hardware — shared machines often exceed it when
    // allocating ~1e8 samples (QSP §4.3: no absolute frame SLO in shared CI).
    const char *enforce = std::getenv("WELLLOG_GATE_ENFORCE_SLO");
    const bool enforce_slo =
        enforce != nullptr &&
        (std::string_view{enforce} == "1" ||
         std::string_view{enforce} == "true" ||
         std::string_view{enforce} == "yes");
    return GateScale{
        .well_count = 20,
        .curves_per_well = 10,
        .samples_per_curve = 500'000,
        .tracks_per_well = 5,
        .markers_per_well = 5'000,
        .pixel_height = 2160,
        .enforce_first_interactive_ms = enforce_slo,
        .first_interactive_ms_limit = 2000.0,
        .name = "full",
    };
  }
  // CI: multi-well / multi-curve / multi-track with enough samples that
  // hierarchical LOD must compress (480 px vs 80k samples ≈ 160:1).
  return GateScale{
      .well_count = 2,
      .curves_per_well = 4,
      .samples_per_curve = 80'000,
      .tracks_per_well = 2,
      .markers_per_well = 50,
      .pixel_height = 480,
      .enforce_first_interactive_ms = false,
      .first_interactive_ms_limit = 2000.0,
      .name = "ci",
  };
}

PerformanceBudgets gate_budgets(const GateScale &scale) {
  // Generous CPU derived budget so prep can succeed; memory *ratio* is asserted.
  const std::uint64_t raw_estimate =
      static_cast<std::uint64_t>(scale.well_count) *
      static_cast<std::uint64_t>(scale.curves_per_well) *
      static_cast<std::uint64_t>(scale.samples_per_curve) *
      (sizeof(double) + sizeof(float));
  return PerformanceBudgets{
      .maximum_cpu_derived_bytes =
          std::max<std::uint64_t>(raw_estimate / 2 + 16ULL * 1024ULL * 1024ULL,
                                  32ULL * 1024ULL * 1024ULL),
      .maximum_gpu_cache_bytes = 128ULL * 1024ULL * 1024ULL,
      .maximum_upload_bytes_per_frame = 512ULL * 1024ULL,
      .prefetch_viewports = 2.0,
      // Below curve length so hierarchical LOD builds (required for 50% memory
      // / compressed prepared-point contracts).
      .asynchronous_sample_threshold = 1'024,
  };
}

struct BuiltWell {
  EntityId document_id;
  EntityId axis_id;
  std::vector<EntityId> curve_ids;
  std::vector<EntityId> track_ids;
  std::uint64_t raw_buffer_bytes{};
};

BuiltWell build_well(WellLogSession &session, const GateScale &scale,
                     std::uint32_t well_index) {
  BuiltWell well;
  well.document_id = id(hex_id(well_index, 1));
  well.axis_id = id(hex_id(well_index, 2));

  auto depths =
      std::make_shared<std::vector<double>>(scale.samples_per_curve);
  for (std::size_t i = 0; i < scale.samples_per_curve; ++i) {
    (*depths)[i] = 1000.0 + static_cast<double>(i) * 0.01;
  }
  well.raw_buffer_bytes += depths->size() * sizeof(double);

  WellLogDocumentBuilder doc(well.document_id, DocumentRevision{1});
  doc.add_sampling_axis(SamplingAxis{
      .id = well.axis_id,
      .coordinates = BufferView::from_vector(
          std::const_pointer_cast<const std::vector<double>>(depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });

  for (std::size_t c = 0; c < scale.curves_per_well; ++c) {
    auto curve_id = id(hex_id(well_index, static_cast<std::uint32_t>(10 + c)));
    well.curve_ids.push_back(curve_id);
    auto values =
        std::make_shared<std::vector<float>>(scale.samples_per_curve);
    for (std::size_t i = 0; i < scale.samples_per_curve; ++i) {
      (*values)[i] = static_cast<float>(
          std::sin(0.01 * static_cast<double>(i) + 0.1 * static_cast<double>(c) +
                   0.3 * static_cast<double>(well_index)));
    }
    well.raw_buffer_bytes += values->size() * sizeof(float);
    doc.add_curve(Curve{
        .id = curve_id,
        .mnemonic = "C" + std::to_string(c),
        .display_name = "Curve " + std::to_string(c),
        .unit = "API",
        .sampling_axis_id = well.axis_id,
        .values = BufferView::from_vector(
            std::const_pointer_cast<const std::vector<float>>(values)),
        .nulls = {},
    });
  }

  const double depth_span =
      static_cast<double>(scale.samples_per_curve) * 0.01;
  const double marker_step =
      depth_span /
      static_cast<double>(std::max<std::size_t>(scale.markers_per_well, 1));
  for (std::size_t m = 0; m < scale.markers_per_well; ++m) {
    const double depth = 1000.0 + static_cast<double>(m) * marker_step;
    doc.add_marker(Marker{
        .id = id(hex_id(well_index, static_cast<std::uint32_t>(1000 + m))),
        .reference_depth = depth,
        .semantic = MarkerSemantic::formation_top,
        .label = {},
    });
  }

  require(session.execute(SetDocumentCommand{doc.build()}).has_value(),
          "set document");
  // Wait for LOD hierarchy before presentation (dense pipeline pattern).
  {
    const auto deadline = Clock::now() + std::chrono::seconds{60};
    while (Clock::now() < deadline) {
      session.poll_async();
      const auto snap = session.performance_snapshot(well.document_id);
      if (snap.has_value() &&
          snap->preparation_state == PreparationState::ready) {
        break;
      }
      if (snap.has_value() &&
          snap->preparation_state == PreparationState::unavailable) {
        fail("LOD preparation unavailable");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto snap = session.performance_snapshot(well.document_id);
    require(snap.has_value() &&
                snap->preparation_state == PreparationState::ready,
            "LOD hierarchy ready before presentation");
  }

  const double bottom =
      1000.0 + static_cast<double>(scale.samples_per_curve - 1) * 0.01;
  ScenePresentationBuilder pres(
      well.document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = bottom,
      },
      Millimetres{200.0}, "release-gate-v1");

  const std::size_t tracks =
      std::min(scale.tracks_per_well, scale.curves_per_well);
  for (std::size_t t = 0; t < tracks; ++t) {
    auto track_id =
        id(hex_id(well_index, static_cast<std::uint32_t>(200 + t)));
    auto scale_id =
        id(hex_id(well_index, static_cast<std::uint32_t>(300 + t)));
    auto layer_id =
        id(hex_id(well_index, static_cast<std::uint32_t>(400 + t)));
    well.track_ids.push_back(track_id);
    pres.add_track(TrackSpec{.id = track_id,
                             .width = Millimetres{20.0},
                             .z_order = static_cast<std::int32_t>(t)});
    pres.add_scale(TrackScaleSpec{
        .id = scale_id,
        .track_id = track_id,
        .mode = ScaleMode::linear,
        .minimum = -2.0,
        .maximum = 2.0,
        .direction = ScaleDirection::left_to_right,
        .unit = "API",
    });
    // Stack remaining curves on first tracks (200 curves → 100 tracks target).
    for (std::size_t c = t; c < scale.curves_per_well; c += tracks) {
      auto layer_c =
          id(hex_id(well_index, static_cast<std::uint32_t>(500 + c)));
      pres.add_curve_layer(CurveLayerSpec{
          .id = layer_c,
          .track_id = track_id,
          .curve_id = well.curve_ids[c],
          .scale_id = scale_id,
          .color = {},
          .line_width = Millimetres{0.2},
          .z_order = static_cast<std::int32_t>(c),
          .visible = true,
      });
      (void)layer_id;
    }
  }

  require(session.execute(SetPresentationCommand{pres.build()}).has_value(),
          "presentation");
  require(session
              .execute(SetViewportMetricsCommand{
                  .document_id = well.document_id,
                  .viewport = DepthViewport{.top = 1000.0, .bottom = bottom},
                  .pixel_height =
                      static_cast<std::uint32_t>(scale.pixel_height),
              })
              .has_value(),
          "viewport metrics");
  return well;
}

void wait_ready(WellLogSession &session, EntityId document_id,
                std::chrono::milliseconds budget) {
  const auto deadline = Clock::now() + budget;
  while (Clock::now() < deadline) {
    session.poll_async();
    // A non-null prepared scene is the interactive readiness signal for the
    // headless gate. Frame-level LOD may continue refining in the background.
    if (session.prepared_scene(document_id) != nullptr) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const auto snap = session.performance_snapshot(document_id);
  if (snap.has_value()) {
    std::cerr << "INFO: final state="
              << static_cast<int>(snap->preparation_state)
              << " frame_pending=" << snap->frame_preparation_pending
              << " rev=" << snap->document_revision.value << '\n';
  }
  fail("preparation did not become ready in time");
}

void run_gate() {
  const auto scale = resolve_scale();
  std::cerr << "INFO: release-gate scale=" << scale.name
            << " wells=" << scale.well_count
            << " curves/well=" << scale.curves_per_well
            << " samples/curve=" << scale.samples_per_curve
            << " tracks/well=" << scale.tracks_per_well
            << " markers/well=" << scale.markers_per_well << '\n';

  const auto total_samples = scale.well_count * scale.curves_per_well *
                             scale.samples_per_curve;
  const auto total_tracks = scale.well_count * scale.tracks_per_well;
  const auto total_markers = scale.well_count * scale.markers_per_well;
  if (std::string_view{scale.name} == "full") {
    require(total_samples == 100'000'000ULL, "full scale must be 1e8 samples");
    require(scale.well_count == 20, "full: 20 wells");
    require(total_tracks == 100, "full: 100 tracks");
    require(scale.curves_per_well * scale.well_count == 200, "full: 200 curves");
    require(total_markers == 100'000, "full: 100k discrete markers");
  }

  WellLogSession session(gate_budgets(scale));
  const auto t0 = Clock::now();
  std::vector<BuiltWell> wells;
  wells.reserve(scale.well_count);
  std::uint64_t raw_bytes = 0;
  for (std::uint32_t w = 0; w < scale.well_count; ++w) {
    wells.push_back(build_well(session, scale, w));
    raw_bytes += wells.back().raw_buffer_bytes;
  }

  std::vector<WellPlacement> placements;
  placements.reserve(wells.size());
  for (const auto &w : wells) {
    placements.push_back(WellPlacement{.document_id = w.document_id});
  }
  require(session
              .execute(SetWellLayoutCommand{.wells = std::move(placements),
                                            .gap = Millimetres{4.0},
                                            .pack_left_to_right = true})
              .has_value(),
          "layout");

  const double bottom =
      1000.0 + static_cast<double>(scale.samples_per_curve - 1) * 0.01;
  require(session
              .execute(SetSharedDepthViewportCommand{
                  .viewport = DepthViewport{.top = 1000.0, .bottom = bottom},
                  .pixel_height = static_cast<std::uint32_t>(scale.pixel_height),
              })
              .has_value(),
          "shared viewport");

  for (const auto &w : wells) {
    wait_ready(session, w.document_id, std::chrono::seconds{60});
  }
  const auto first_interactive_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::cerr << "INFO: first_interactive_ms=" << first_interactive_ms << '\n';
  if (scale.enforce_first_interactive_ms) {
    require(first_interactive_ms <= scale.first_interactive_ms_limit,
            "first interactive frame must be <= 2s on full gate");
  }

  const auto surface = session.prepared_surface_scene();
  require(surface != nullptr, "surface scene must compose");
  require(session.well_layout().size() == scale.well_count,
          "layout well count");

  // Memory / GPU: no full raw copy; prepared points << source samples after LOD.
  std::uint64_t derived = 0;
  std::uint64_t prepared_points = 0;
  std::uint64_t planned_gpu = 0;
  for (const auto &w : wells) {
    if (const auto snap = session.performance_snapshot(w.document_id);
        snap.has_value()) {
      derived += snap->cpu_derived_bytes;
    }
    const auto scene = session.prepared_scene(w.document_id);
    require(scene != nullptr, "prepared scene");
    prepared_points += scene->curve_points().size();
    const auto plan = GpuUploadSchedule::plan(
        *scene, GpuUploadBudgets{
                    .maximum_cache_bytes = 64ULL * 1024ULL * 1024ULL,
                    .maximum_bytes_per_frame = 256ULL * 1024ULL,
                });
    if (plan.has_value()) {
      planned_gpu += plan.value().total_bytes();
    }
  }
  const auto source_samples = static_cast<std::uint64_t>(
      scale.well_count * scale.curves_per_well * scale.samples_per_curve);
  std::cerr << "INFO: raw_buffer_bytes=" << raw_bytes
            << " cpu_derived_bytes=" << derived
            << " prepared_points=" << prepared_points
            << " planned_gpu_upload_bytes=" << planned_gpu << '\n';
  require(raw_bytes > 0, "raw bytes");
  // LOD must not retain the full raw sample cloud in the prepared scene.
  require(prepared_points * 2 < source_samples,
          "prepared curve points must be well below source sample count");
  // Planned GPU geometry must stay well below a full raw-buffer upload.
  require(planned_gpu * 2 < raw_bytes,
          "GPU plan must not approach a full raw buffer copy");
  // When the session reports derived budgets, enforce the ADR 50% ceiling.
  if (derived > 0) {
    require(derived * 2 <= raw_bytes + raw_bytes / 50,
            "engine derived memory must be <= ~50% of raw curve buffers");
  }

  // Semantic pick is CPU and must stay responsive (structure check).
  const auto pick_t0 = Clock::now();
  constexpr int kPicks = 32;
  for (int i = 0; i < kPicks; ++i) {
    (void)session.pick_surface_curve(CurvePickQuery{
        .scene_position =
            PhysicalPoint{Millimetres{10.0 + static_cast<double>(i)},
                          Millimetres{20.0 + static_cast<double>(i)}},
        .tolerance = DeviceIndependentPixels{8.0},
        .horizontal_device_independent_pixels_per_millimetre = 4.0,
        .vertical_device_independent_pixels_per_millimetre = 4.0,
    });
  }
  const auto pick_p95_proxy_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - pick_t0).count() /
      static_cast<double>(kPicks);
  std::cerr << "INFO: pick_mean_ms=" << pick_p95_proxy_ms << '\n';
  // Soft structural bound for CI (not the ADR 16ms hardware gate).
  require(pick_p95_proxy_ms < 50.0, "pick path must stay cheap headless");

  // CPU vector export still works (no interactive software fallback needed).
  const auto svg = SvgExporter::write(*surface);
  require(svg.has_value() && !svg.value().text().empty(),
          "surface SVG export");

  std::cerr << "PASS: release-gate scale=" << scale.name
            << " samples=" << total_samples << " tracks=" << total_tracks
            << " markers=" << total_markers << '\n';
}

} // namespace

int main() {
  run_gate();
  return EXIT_SUCCESS;
}
