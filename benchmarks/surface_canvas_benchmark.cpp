// Unified Surface Canvas benchmark: one-placement (single well) and
// N-placement (correlation) surfaces through the SAME code path.
//
// Scenarios (see task_plan §Phase 5):
//   A single — 1 well, 20 tracks, 30+ curves, 1M+ samples, 4K
//   B correlation — 5 wells, 10 tracks/well, markers + depth transforms +
//     cross-well overlays, 10M+ logical samples, 4K
//   C virtualized — 20 wells with an active horizontal window, 20M+ logical
//   D (--large) 1 well, 100 tracks, up to 100M logical samples
//
// Curves share synthetic buffers (zero-copy BufferView sharing), so logical
// sample counts scale without duplicating gigabytes of source arrays.

#include <welllog/render_gl/upload.hpp>
#include <welllog/session/session.hpp>

#include "render_gl/renderer.hpp"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace welllog;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t cpu_budget = 256ULL * 1024ULL * 1024ULL;
// 4K correlation surfaces (100 dense curves) legitimately plan ~270MB of
// vertex data; the GPU cache is host policy, so the benchmark sizes it for
// the scenarios it runs rather than the 256MB default.
constexpr std::uint64_t gpu_budget = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t upload_budget = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t measured_frames = 120;

[[nodiscard]] EntityId id(std::string_view text) {
  return EntityId::parse(text).value();
}

// Deterministic per-well/per-curve UUID minting from small integers.
[[nodiscard]] EntityId well_id(int well) {
  char text[40];
  std::snprintf(text, sizeof(text), "900000%02d-0000-4000-8000-000000000000",
                well);
  return id(text);
}
[[nodiscard]] EntityId entity_id(int well, int slot) {
  char text[40];
  std::snprintf(text, sizeof(text),
                "910000%02d-0000-4000-8000-%012d", well, slot);
  return id(text);
}

[[nodiscard]] detail::GlProcAddress resolve_gl_proc(void *context,
                                                    const char *name) noexcept {
  if (context == nullptr || name == nullptr) {
    return nullptr;
  }
  return static_cast<QOpenGLContext *>(context)->getProcAddress(name);
}

[[nodiscard]] double milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>{duration}.count();
}

[[nodiscard]] double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  const auto position = static_cast<std::size_t>(
      fraction * static_cast<double>(values.size() - 1));
  return values[position];
}

// Shared synthetic source buffers: every curve of the same size class points
// at one immutable allocation (the engine is zero-copy over BufferView).
struct SharedSources {
  std::shared_ptr<const std::vector<double>> depths;
  std::vector<std::shared_ptr<const std::vector<float>>> value_sets;
};

[[nodiscard]] SharedSources make_sources(std::size_t samples,
                                         std::size_t value_curves) {
  SharedSources sources;
  auto depths = std::make_shared<std::vector<double>>(samples);
  for (std::size_t index = 0; index < samples; ++index) {
    (*depths)[index] = 1000.0 + static_cast<double>(index) *
                                    1000.0 / static_cast<double>(samples);
  }
  sources.depths = std::move(depths);
  for (std::size_t set = 0; set < value_curves; ++set) {
    auto values = std::make_shared<std::vector<float>>(samples);
    const auto phase = static_cast<double>(set) * 0.37;
    for (std::size_t index = 0; index < samples; ++index) {
      (*values)[index] = static_cast<float>(
          50.0 + 45.0 * std::sin(phase + static_cast<double>(index) * 0.001));
    }
    sources.value_sets.push_back(std::move(values));
  }
  return sources;
}

struct WellSpec {
  int well{};
  int tracks{};
  int curves_per_track{};
  bool markers{};
  bool depth_transform{};
};

// Builds one well document + presentation from shared sources. Returns the
// marker ids (for overlays) when markers were requested.
[[nodiscard]] std::vector<EntityId> build_well(WellLogSession &session,
                                               const SharedSources &sources,
                                               const WellSpec &spec) {
  const auto document = well_id(spec.well);
  WellLogDocumentBuilder builder(document, DocumentRevision{1});
  const auto axis = entity_id(spec.well, 1);
  builder.add_sampling_axis(SamplingAxis{
      .id = axis,
      .coordinates = BufferView::from_vector(sources.depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  const auto total_curves =
      static_cast<std::size_t>(spec.tracks) *
      static_cast<std::size_t>(spec.curves_per_track);
  for (std::size_t curve = 0; curve < total_curves; ++curve) {
    const auto &values =
        sources.value_sets[curve % sources.value_sets.size()];
    builder.add_curve(Curve{
        .id = entity_id(spec.well, 1000 + static_cast<int>(curve)),
        .mnemonic = "GR",
        .display_name = "GR",
        .unit = "API",
        .sampling_axis_id = axis,
        .values = BufferView::from_vector(values),
        .nulls = {},
    });
  }
  std::vector<EntityId> markers;
  if (spec.markers) {
    for (int marker = 0; marker < 4; ++marker) {
      const auto marker_id =
          entity_id(spec.well, 5000 + marker);
      markers.push_back(marker_id);
      builder.add_marker(Marker{
          .id = marker_id,
          .reference_depth = 1100.0 + 150.0 * marker,
          .semantic = MarkerSemantic::formation_top,
          .label = "M",
      });
    }
  }
  if (!session.execute(SetDocumentCommand{builder.build()}).has_value()) {
    std::cerr << "surface benchmark SetDocumentCommand failed\n";
    std::_Exit(EXIT_FAILURE);
  }
  ScenePresentationBuilder presentation(
      document,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 2000.0,
      },
      Millimetres{1000.0}, "surface-canvas-benchmark");
  if (spec.depth_transform) {
    presentation.set_depth_transform(DepthTransform{
        .control_points =
            {
                {.reference_depth = 1000.0, .display_depth = 1000.0},
                {.reference_depth = 2000.0,
                 .display_depth = 2000.0 + 25.0 * spec.well},
            },
        .extrapolate = DepthExtrapolatePolicy::linear,
        .version = 1,
    });
  }
  for (int track = 0; track < spec.tracks; ++track) {
    presentation.add_track(TrackSpec{
        .id = entity_id(spec.well, 2000 + track),
        .width = Millimetres{20.0},
        .z_order = 0,
    });
  }
  // One scale per track keeps validation simple — layers reference the
  // track's own scale.
  for (int track = 0; track < spec.tracks; ++track) {
    presentation.add_scale(TrackScaleSpec{
        .id = entity_id(spec.well, 3000 + track),
        .track_id = entity_id(spec.well, 2000 + track),
        .mode = ScaleMode::linear,
        .minimum = 0.0,
        .maximum = 100.0,
        .direction = ScaleDirection::left_to_right,
        .unit = "API",
    });
  }
  for (int track = 0; track < spec.tracks; ++track) {
    for (int layer = 0; layer < spec.curves_per_track; ++layer) {
      const auto curve_index = static_cast<std::size_t>(track) *
                                   static_cast<std::size_t>(
                                       spec.curves_per_track) +
                               static_cast<std::size_t>(layer);
      presentation.add_curve_layer(CurveLayerSpec{
          .id = entity_id(spec.well, 4000 + static_cast<int>(curve_index)),
          .track_id = entity_id(spec.well, 2000 + track),
          .curve_id = entity_id(spec.well, 1000 + static_cast<int>(curve_index)),
          .scale_id = entity_id(spec.well, 3000 + track),
          .color = RgbaColor{0x20, 0x60, 0xa0, 0xff},
          .line_width = Millimetres{0.25},
          .z_order = 0,
          .visible = true,
      });
    }
  }
  if (spec.markers) {
    presentation.add_marker_layer(MarkerLayerSpec{
        .id = entity_id(spec.well, 6000),
        .track_id = entity_id(spec.well, 2000),
        .z_order = 1,
        .draw_labels = false,
        .draw_symbols = false,
    });
  }
  if (!session.execute(SetPresentationCommand{presentation.build()})
           .has_value()) {
    std::cerr << "surface benchmark SetPresentationCommand failed\n";
    std::_Exit(EXIT_FAILURE);
  }
  return markers;
}

[[nodiscard]] bool wait_until_ready(WellLogSession &session,
                                    EntityId document_id) {
  const auto deadline = Clock::now() + std::chrono::seconds{120};
  while (Clock::now() < deadline) {
    session.poll_async();
    const auto snapshot = session.performance_snapshot(document_id);
    if (snapshot.has_value() &&
        snapshot->preparation_state == PreparationState::ready &&
        !snapshot->frame_preparation_pending &&
        session.prepared_scene(document_id) != nullptr) {
      return true;
    }
    if (snapshot.has_value() &&
        snapshot->preparation_state == PreparationState::unavailable) {
      for (const auto &diagnostic : session.diagnostics()) {
        const auto error = session.diagnostic_error(diagnostic.id);
        std::cerr << "diagnostic code=" << static_cast<int>(diagnostic.code)
                  << " error="
                  << (error.has_value() ? static_cast<int>(error->code) : -1)
                  << "\n";
      }
      return false;
    }
    std::this_thread::yield();
  }
  return false;
}

struct ScenarioResult {
  std::string scenario;
  std::uint64_t wells{};
  std::uint64_t logical_samples{};
  std::vector<double> prepare_ms;
  std::vector<double> compose_ms;
  std::vector<double> frame_ms;
  std::vector<double> pan_ms;
  std::vector<double> zoom_ms;
  std::vector<double> horizontal_pan_ms;
  std::vector<double> pick_ms;
  std::uint64_t prepared_points{};
  std::uint64_t upload_bytes{};
  std::uint64_t gpu_planned_bytes{};
  std::uint64_t cpu_derived_bytes{};
  WellLogSession::SurfaceStatistics statistics{};
};

struct GlContext {
  QGuiApplication *application{};
  QWindow *window{};
  QOpenGLContext *context{};
  bool ok{};
};

[[nodiscard]] GlContext make_gl_context(QGuiApplication &application) {
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  static auto window = new QWindow();
  window->setSurfaceType(QWindow::OpenGLSurface);
  window->setFormat(format);
  window->resize(3840, 2160);
  window->create();
  window->show();
  application.processEvents();
  static auto context = new QOpenGLContext();
  context->setFormat(format);
  GlContext result;
  result.application = &application;
  result.window = window;
  result.context = context;
  result.ok = context->create() && context->makeCurrent(window);
  return result;
}

void run_scenario(QGuiApplication &application, const char *scenario_name,
                  const SharedSources &sources, const WellSpec &base_spec,
                  int well_count, bool use_layout, double window_mm,
                  std::optional<double> transform_shift,
                  std::uint64_t logical_samples) {
  WellLogSession session(PerformanceBudgets{
      .maximum_cpu_derived_bytes = cpu_budget,
      .maximum_gpu_cache_bytes = gpu_budget,
      .maximum_upload_bytes_per_frame = upload_budget,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  });
  ScenarioResult result;
  result.scenario = scenario_name;
  result.wells = static_cast<std::uint64_t>(well_count);
  result.logical_samples = logical_samples;

  std::vector<std::vector<EntityId>> marker_ids;
  const auto build_start = Clock::now();
  for (int well = 0; well < well_count; ++well) {
    WellSpec spec = base_spec;
    spec.well = well;
    if (transform_shift.has_value() && well > 0) {
      spec.depth_transform = true;
    }
    marker_ids.push_back(build_well(session, sources, spec));
    const auto well_start = Clock::now();
    if (!wait_until_ready(session, well_id(well))) {
      std::cerr << scenario_name << ": preparation failed for well " << well
                << "\n";
      std::_Exit(EXIT_FAILURE);
    }
    result.prepare_ms.push_back(milliseconds(Clock::now() - well_start));
  }
  const auto build_ms = milliseconds(Clock::now() - build_start);

  // Surface layout + shared Display Depth viewport.
  if (use_layout) {
    std::vector<WellPlacement> placements;
    placements.reserve(static_cast<std::size_t>(well_count));
    for (int well = 0; well < well_count; ++well) {
      placements.push_back(
          WellPlacement{.document_id = well_id(well)});
    }
    if (!session
             .execute(SetWellLayoutCommand{
                 .wells = std::move(placements),
                 .gap = Millimetres{4.0},
                 .pack_left_to_right = true,
             })
             .has_value()) {
      std::cerr << scenario_name << ": layout failed\n";
      std::_Exit(EXIT_FAILURE);
    }
    if (!session
             .execute(SetSharedDepthViewportCommand{
                 .viewport = DepthViewport{.top = 1200.0, .bottom = 1260.0},
                 .pixel_height = 2160,
             })
             .has_value()) {
      std::cerr << scenario_name << ": shared viewport failed\n";
      std::_Exit(EXIT_FAILURE);
    }
    for (int well = 0; well < well_count; ++well) {
      if (!wait_until_ready(session, well_id(well))) {
        std::cerr << scenario_name << ": reprepare failed\n";
        std::_Exit(EXIT_FAILURE);
      }
    }
    // Cross-well horizon overlays between adjacent wells (scenario B).
    if (!marker_ids.empty() && !marker_ids.front().empty()) {
      std::vector<CrossWellOverlay> overlays;
      for (int well = 0; well + 1 < well_count; ++well) {
        overlays.push_back(CrossWellOverlay{
            .id = entity_id(well, 7000),
            .kind = CrossWellOverlay::Kind::horizon_line,
            .left_document_id = well_id(well),
            .right_document_id = well_id(well + 1),
            .left_marker_id =
                marker_ids[static_cast<std::size_t>(well)][1],
            .right_marker_id =
                marker_ids[static_cast<std::size_t>(well) + 1U][1],
        });
      }
      if (!session.execute(SetCrossWellOverlaysCommand{std::move(overlays)})
               .has_value()) {
        std::cerr << scenario_name << ": overlays failed\n";
        std::_Exit(EXIT_FAILURE);
      }
    }
    if (window_mm > 0.0) {
      if (!session
               .execute(SetSurfaceHorizontalViewCommand{
                   .left_mm = 0.0,
                   .right_mm = window_mm,
               })
               .has_value()) {
        std::cerr << scenario_name << ": horizontal window failed\n";
        std::_Exit(EXIT_FAILURE);
      }
    }
  }

  // Compose measurements: fresh compose after a focused-well scene change,
  // and cache-hit calls in between (must be near-free).
  auto surface = session.prepared_surface_scene();
  if (surface == nullptr) {
    std::cerr << scenario_name << ": no surface scene\n";
    std::_Exit(EXIT_FAILURE);
  }
  result.statistics = session.surface_statistics();
  for (int round = 0; round < 10; ++round) {
    // Bump the focused well's prepared scene (pan inside prefetched LOD)
    // then recompose.
    static_cast<void>(session.execute(PanDepthCommand{
        .document_id = well_id(round % well_count),
        .display_depth_delta = 2.0}));
    if (!wait_until_ready(session, well_id(round % well_count))) {
      std::cerr << scenario_name << ": pan prepare failed\n";
      std::_Exit(EXIT_FAILURE);
    }
    const auto compose_start = Clock::now();
    surface = session.prepared_surface_scene();
    result.compose_ms.push_back(milliseconds(Clock::now() - compose_start));
    // Cache hit timing (same inputs).
    const auto hit_start = Clock::now();
    static_cast<void>(session.prepared_surface_scene());
    result.compose_ms.push_back(milliseconds(Clock::now() - hit_start));
  }
  result.prepared_points = surface->curve_points().size();

  // Interactive vertical pan / zoom (shared Display Depth window).
  for (int round = 0; round < 20; ++round) {
    const auto start = Clock::now();
    static_cast<void>(session.execute(PanDepthCommand{
        .document_id = well_id(0), .display_depth_delta = 5.0}));
    for (int well = 0; well < well_count; ++well) {
      if (!wait_until_ready(session, well_id(well))) {
        std::cerr << scenario_name << ": interactive pan prepare failed\n";
        std::_Exit(EXIT_FAILURE);
      }
    }
    surface = session.prepared_surface_scene();
    result.pan_ms.push_back(milliseconds(Clock::now() - start));
  }
  for (int round = 0; round < 20; ++round) {
    const auto start = Clock::now();
    static_cast<void>(session.execute(ZoomDepthAtCommand{
        .document_id = well_id(0),
        .anchor_display_depth = 1230.0,
        .span_factor = round % 2 == 0 ? 0.9 : 1.0 / 0.9,
    }));
    for (int well = 0; well < well_count; ++well) {
      if (!wait_until_ready(session, well_id(well))) {
        std::cerr << scenario_name << ": interactive zoom prepare failed\n";
        std::_Exit(EXIT_FAILURE);
      }
    }
    surface = session.prepared_surface_scene();
    result.zoom_ms.push_back(milliseconds(Clock::now() - start));
  }
  // Horizontal pan (virtualization path; no per-well reprepare when the
  // cull set is unchanged).
  if (use_layout && window_mm > 0.0) {
    for (int round = 0; round < 20; ++round) {
      const auto start = Clock::now();
      static_cast<void>(session.execute(
          PanSurfaceHorizontalCommand{.delta_mm = 2.0}));
      surface = session.prepared_surface_scene();
      result.horizontal_pan_ms.push_back(milliseconds(Clock::now() - start));
    }
    result.statistics = session.surface_statistics();
  }

  // Picking across the visible surface.
  {
    const auto width_mm = surface->physical_width().value;
    for (int round = 0; round < 100; ++round) {
      const auto x = width_mm * static_cast<double>(round) / 100.0;
      const auto start = Clock::now();
      static_cast<void>(session.pick_surface_curve(CurvePickQuery{
          .scene_position = PhysicalPoint{Millimetres{x}, Millimetres{30.0}},
          .tolerance = DeviceIndependentPixels{6.0},
          .horizontal_device_independent_pixels_per_millimetre = 4.0,
          .vertical_device_independent_pixels_per_millimetre = 2.0,
      }));
      result.pick_ms.push_back(milliseconds(Clock::now() - start));
    }
  }

  // CPU cache footprint (per-document derived bytes).
  for (int well = 0; well < well_count; ++well) {
    const auto snapshot = session.performance_snapshot(well_id(well));
    if (snapshot.has_value()) {
      result.cpu_derived_bytes += snapshot->cpu_derived_bytes;
    }
  }

  // GPU upload + render frames (offscreen 4K).
  const auto upload = GpuUploadSchedule::plan(
      *surface, GpuUploadBudgets{.maximum_cache_bytes = gpu_budget,
                                 .maximum_bytes_per_frame = upload_budget});
  if (!upload.has_value()) {
    std::cerr << scenario_name << ": upload plan failed (points="
              << surface->curve_points().size() << " segments="
              << surface->curve_segments().size() << " error="
              << static_cast<int>(upload.error().code) << ")\n";
    std::_Exit(EXIT_FAILURE);
  }
  result.gpu_planned_bytes = upload.value().total_bytes();
  auto gl = make_gl_context(application);
  if (!gl.ok) {
    std::cerr << scenario_name << ": OpenGL context failed\n";
    std::_Exit(EXIT_FAILURE);
  }
  auto *functions = gl.context->functions();
  functions->initializeOpenGLFunctions();
  {
    detail::GlRenderer renderer;
    if (!renderer.initialize(resolve_gl_proc, gl.context)) {
      std::cerr << scenario_name << ": renderer init failed\n";
      std::_Exit(EXIT_FAILURE);
    }
    if (!renderer.queue_upload(*surface,
                               GpuUploadBudgets{
                                   .maximum_cache_bytes = gpu_budget,
                                   .maximum_bytes_per_frame = upload_budget,
                               })) {
      std::cerr << scenario_name << ": queue_upload failed\n";
      std::_Exit(EXIT_FAILURE);
    }
    std::uint64_t uploaded = 0;
    auto scene_available = false;
    while (result.frame_ms.size() < measured_frames) {
      const auto start = Clock::now();
      const auto progress = renderer.upload_next();
      if (progress.pending) {
        // Still streaming chunks within the per-frame budget.
      } else if (progress.completed) {
        uploaded = progress.total_bytes;
        scene_available = true;
      } else if (!scene_available) {
        // A real failure before anything completed.
        std::cerr << scenario_name << ": upload failed\n";
        std::_Exit(EXIT_FAILURE);
      }
      // Otherwise: idle (the upload already completed on an earlier frame).
      const auto viewport = session.surface_depth_viewport()
                                .value_or(DepthViewport{.top = 1200.0,
                                                        .bottom = 1260.0});
      if (!renderer.render(detail::GlRenderFrame{
              .framebuffer = 0,
              .pixel_width = 3840,
              .pixel_height = 2160,
              .physical_pixels_per_millimetre = 96.0 / 25.4,
              .viewport =
                  detail::GlDepthViewport{
                      .top = viewport.top,
                      .bottom = viewport.bottom,
                  },
              .horizontal = std::nullopt,
              .crosshair = std::nullopt,
              .draw_scene = scene_available,
          })) {
        std::cerr << scenario_name << ": render failed\n";
        std::_Exit(EXIT_FAILURE);
      }
      gl.context->swapBuffers(gl.window);
      functions->glFinish();
      result.frame_ms.push_back(milliseconds(Clock::now() - start));
      application.processEvents();
    }
    result.upload_bytes = uploaded;
    renderer.release();
  }
  gl.context->doneCurrent();

  const auto p = [](const std::vector<double> &values, double fraction) {
    return values.empty() ? 0.0 : percentile(values, fraction);
  };
  std::cout << std::fixed << std::setprecision(3) << "  \""
            << scenario_name
            << "\": {\n"
            << "    \"wells\": " << result.wells << ",\n"
            << "    \"logical_samples\": " << result.logical_samples << ",\n"
            << "    \"build_ms\": " << build_ms << ",\n"
            << "    \"prepare_ms\": {\"p50\": " << p(result.prepare_ms, 0.50)
            << ", \"p95\": " << p(result.prepare_ms, 0.95)
            << ", \"p99\": " << p(result.prepare_ms, 0.99) << "},\n"
            << "    \"surface_compose_ms\": {\"p50\": "
            << p(result.compose_ms, 0.50) << ", \"p95\": "
            << p(result.compose_ms, 0.95) << ", \"p99\": "
            << p(result.compose_ms, 0.99) << "},\n"
            << "    \"frame_ms\": {\"p50\": " << p(result.frame_ms, 0.50)
            << ", \"p95\": " << p(result.frame_ms, 0.95)
            << ", \"p99\": " << p(result.frame_ms, 0.99) << "},\n"
            << "    \"pan_ms\": {\"p50\": " << p(result.pan_ms, 0.50)
            << ", \"p95\": " << p(result.pan_ms, 0.95) << "},\n"
            << "    \"zoom_ms\": {\"p50\": " << p(result.zoom_ms, 0.50)
            << ", \"p95\": " << p(result.zoom_ms, 0.95) << "},\n"
            << "    \"horizontal_pan_ms\": {\"p50\": "
            << p(result.horizontal_pan_ms, 0.50) << ", \"p95\": "
            << p(result.horizontal_pan_ms, 0.95) << "},\n"
            << "    \"pick_ms_p95\": " << p(result.pick_ms, 0.95) << ",\n"
            << "    \"prepared_points\": " << result.prepared_points << ",\n"
            << "    \"upload_bytes\": " << result.upload_bytes << ",\n"
            << "    \"gpu_planned_bytes\": " << result.gpu_planned_bytes
            << ",\n"
            << "    \"cpu_derived_bytes\": " << result.cpu_derived_bytes
            << ",\n"
            << "    \"visible_wells\": " << result.statistics.visible_wells
            << ",\n"
            << "    \"culled_wells\": " << result.statistics.culled_wells
            << ",\n"
            << "    \"visible_tracks\": " << result.statistics.visible_tracks
            << ",\n"
            << "    \"culled_tracks\": " << result.statistics.culled_tracks
            << "\n  }";
}

} // namespace

int main(int argc, char **argv) {
  const auto large = argc > 1 && std::strcmp(argv[1], "--large") == 0;
  QGuiApplication application(argc, argv);
  std::cout << "{\n  \"schema\": \"welllog.surface-canvas-benchmark.v1\",\n";

  // Scenario A — single well, 20 tracks, 32 curves, 1M samples each
  // (32M logical samples via shared buffers), 4K.
  {
    const auto sources = make_sources(1'000'000, 8);
    run_scenario(application, "A-single-20track-32curve-1m", sources,
                 WellSpec{.tracks = 20, .curves_per_track = 2}, 1, false, 0.0,
                 std::nullopt, 32'000'000);
    std::cout << ",\n";
  }

  // Scenario B — 5 wells × 10 tracks × 2 curves, 1M samples each
  // (100M logical via sharing; 10M+ independent sample positions),
  // markers + depth transforms + cross-well overlays.
  {
    const auto sources = make_sources(2'000'000, 8);
    run_scenario(application, "B-correlation-5well-10track", sources,
                 WellSpec{.tracks = 10, .curves_per_track = 2, .markers = true},
                 5, true, 0.0, 25.0, 100'000'000);
    std::cout << ",\n";
  }

  // Scenario C — 20 wells with horizontal virtualization (window covers
  // ~2 wells of the 20-well surface).
  {
    const auto sources = make_sources(1'000'000, 8);
    run_scenario(application, "C-virtualized-20well", sources,
                 WellSpec{.tracks = 10, .curves_per_track = 2}, 20, true,
                 500.0, std::nullopt, 400'000'000);
    std::cout << ",\n";
  }

  if (large) {
    // Scenario D — 1 well, 100 visible tracks, 100M logical samples.
    const auto sources = make_sources(1'000'000, 8);
    run_scenario(application, "D-large-100track", sources,
                 WellSpec{.tracks = 100, .curves_per_track = 1}, 1, false, 0.0,
                 std::nullopt, 100'000'000);
    std::cout << ",\n";
  }

  std::cout << "\n  \"note\": \"logical_samples counts shared zero-copy "
               "BufferView positions; memory stays O(value buffer set)\"\n}\n";
  return EXIT_SUCCESS;
}
