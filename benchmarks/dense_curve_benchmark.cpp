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
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace welllog;
using Clock = std::chrono::steady_clock;

constexpr std::size_t sample_count = 500'000;
constexpr std::uint64_t cpu_budget = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t gpu_budget = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t upload_budget = 256ULL * 1024ULL;
constexpr std::size_t measured_frames = 120;

[[nodiscard]] EntityId id(std::string_view text) {
  return EntityId::parse(text).value();
}

[[nodiscard]] detail::GlProcAddress resolve_gl_proc(void *context,
                                                    const char *name) noexcept {
  if (context == nullptr || name == nullptr) {
    return nullptr;
  }
  return static_cast<QOpenGLContext *>(context)->getProcAddress(name);
}

[[nodiscard]] WellLogDocument make_document() {
  auto depths = std::make_shared<std::vector<double>>(sample_count);
  auto values = std::make_shared<std::vector<float>>(sample_count);
  for (std::size_t index = 0; index < sample_count; ++index) {
    (*depths)[index] = 1000.0 + static_cast<double>(index) * 0.01;
    (*values)[index] =
        static_cast<float>(std::sin(static_cast<double>(index) * 0.01));
  }
  (*values)[sample_count / 2] = 10'000.0F;

  const auto document_id = id("80000000-0000-4000-8000-000000000001");
  const auto axis_id = id("80000000-0000-4000-8000-000000000002");
  const auto curve_id = id("80000000-0000-4000-8000-000000000003");
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(
          std::const_pointer_cast<const std::vector<double>>(depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(
          std::const_pointer_cast<const std::vector<float>>(values)),
      .nulls = {},
  });
  return builder.build();
}

[[nodiscard]] ScenePresentation make_presentation() {
  const auto document_id = id("80000000-0000-4000-8000-000000000001");
  const auto curve_id = id("80000000-0000-4000-8000-000000000003");
  const auto track_id = id("80000000-0000-4000-8000-000000000004");
  const auto scale_id = id("80000000-0000-4000-8000-000000000005");
  ScenePresentationBuilder builder(document_id,
                                   ReferenceDepthRange{
                                       .domain = DepthDomain::measured_depth,
                                       .unit = "m",
                                       .top = 1000.0,
                                       .bottom = 5999.99,
                                   },
                                   Millimetres{500.0}, "dense-4k-benchmark");
  builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = -2.0,
      .maximum = 10'001.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = id("80000000-0000-4000-8000-000000000006"),
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = {},
      .line_width = Millimetres{0.25},
      .z_order = 0,
      .visible = true,
  });
  return builder.build();
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

[[nodiscard]] bool wait_until_ready(WellLogSession &session,
                                    EntityId document_id) {
  const auto deadline = Clock::now() + std::chrono::seconds{10};
  while (Clock::now() < deadline) {
    session.poll_async();
    const auto snapshot = session.performance_snapshot(document_id);
    if (snapshot.has_value() &&
        snapshot->preparation_state == PreparationState::ready &&
        !snapshot->frame_preparation_pending &&
        session.prepared_scene(document_id) != nullptr) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  const auto document_id = id("80000000-0000-4000-8000-000000000001");
  WellLogSession session(PerformanceBudgets{
      .maximum_cpu_derived_bytes = cpu_budget,
      .maximum_gpu_cache_bytes = gpu_budget,
      .maximum_upload_bytes_per_frame = upload_budget,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  });

  auto source_document = make_document();
  const auto source_presentation = make_presentation();
  const auto prepare_start = Clock::now();
  const auto document =
      session.execute(SetDocumentCommand{std::move(source_document)});
  const auto presentation =
      session.execute(SetPresentationCommand{source_presentation});
  const auto submission_ms = milliseconds(Clock::now() - prepare_start);
  if (!document.has_value() || !presentation.has_value() ||
      !wait_until_ready(session, document_id)) {
    std::cerr << "dense benchmark preparation failed\n";
    return EXIT_FAILURE;
  }
  const auto prepare_ms = milliseconds(Clock::now() - prepare_start);

  std::vector<double> frame_plan_times;
  frame_plan_times.reserve(measured_frames);
  for (std::size_t frame = 0; frame < measured_frames; ++frame) {
    const auto top = 1000.0 + static_cast<double>(frame) * 20.0;
    const auto start = Clock::now();
    const auto receipt = session.execute(SetViewportMetricsCommand{
        .document_id = document_id,
        .viewport = DepthViewport{.top = top, .bottom = top + 500.0},
        .pixel_height = 2160,
    });
    if (!receipt.has_value() || !wait_until_ready(session, document_id)) {
      std::cerr << "dense benchmark frame preparation failed\n";
      return EXIT_FAILURE;
    }
    frame_plan_times.push_back(milliseconds(Clock::now() - start));
  }

  const auto scene = session.prepared_scene(document_id);
  const auto snapshot = session.performance_snapshot(document_id);
  if (scene == nullptr || !snapshot.has_value()) {
    return EXIT_FAILURE;
  }
  const auto upload = GpuUploadSchedule::plan(
      *scene, GpuUploadBudgets{
                  .maximum_cache_bytes = gpu_budget,
                  .maximum_bytes_per_frame = upload_budget,
              });
  if (!upload.has_value()) {
    std::cerr << "dense benchmark upload plan failed\n";
    return EXIT_FAILURE;
  }
  const auto chunks = upload.value().chunks();
  const auto maximum_chunk =
      std::accumulate(chunks.begin(), chunks.end(), std::uint64_t{},
                      [](std::uint64_t current, GpuUploadChunk chunk) {
                        return std::max(current, chunk.byte_count);
                      });

  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  QWindow window;
  window.setSurfaceType(QSurface::OpenGLSurface);
  window.setFormat(format);
  window.resize(3840, 2160);
  window.create();
  window.show();
  application.processEvents();

  QOpenGLContext context;
  context.setFormat(format);
  if (!context.create() || !context.makeCurrent(&window)) {
    std::cerr << "dense benchmark OpenGL context creation failed\n";
    return EXIT_FAILURE;
  }
  auto *functions = context.functions();
  if (functions == nullptr) {
    return EXIT_FAILURE;
  }
  functions->initializeOpenGLFunctions();
  detail::GlRenderer renderer;
  if (!renderer.initialize(resolve_gl_proc, &context)) {
    std::cerr << "dense benchmark renderer initialization failed\n";
    return EXIT_FAILURE;
  }

  const auto measured_viewport = session.viewport(document_id);
  if (!measured_viewport.has_value()) {
    return EXIT_FAILURE;
  }
  std::vector<double> frame_times;
  frame_times.reserve(measured_frames);
  std::vector<double> queue_times;
  auto scene_available = false;
  while (frame_times.size() < measured_frames) {
    const auto queue_start = Clock::now();
    if (!renderer.queue_upload(*scene,
                               GpuUploadBudgets{
                                   .maximum_cache_bytes = gpu_budget,
                                   .maximum_bytes_per_frame = upload_budget,
                               })) {
      std::cerr << "dense benchmark renderer upload queue failed\n";
      return EXIT_FAILURE;
    }
    queue_times.push_back(milliseconds(Clock::now() - queue_start));
    auto completed = false;
    while (!completed && frame_times.size() < measured_frames) {
      const auto start = Clock::now();
      const auto progress = renderer.upload_next();
      if (!progress.pending && !progress.completed) {
        return EXIT_FAILURE;
      }
      completed = progress.completed;
      scene_available = scene_available || completed;
      if (!renderer.render(detail::GlRenderFrame{
              .framebuffer = 0,
              .pixel_width = 3840,
              .pixel_height = 2160,
              .physical_pixels_per_millimetre = 96.0 / 25.4,
              .viewport =
                  detail::GlDepthViewport{
                      .top = measured_viewport->top,
                      .bottom = measured_viewport->bottom,
                  },
              .crosshair = std::nullopt,
              .draw_scene = scene_available,
          })) {
        return EXIT_FAILURE;
      }
      context.swapBuffers(&window);
      functions->glFinish();
      frame_times.push_back(milliseconds(Clock::now() - start));
      application.processEvents();
    }
  }
  renderer.release();
  context.doneCurrent();

  std::cout << std::fixed << std::setprecision(3) << "{\n"
            << "  \"schema\": \"welllog.dense-curve-benchmark.v1\",\n"
            << "  \"scenario\": \"single-curve-500k-4k\",\n"
            << "  \"frame_scope\": \"budgeted-upload-draw-finish-swap\",\n"
            << "  \"sample_count\": " << sample_count << ",\n"
            << "  \"viewport_pixels\": {\"width\": 3840, \"height\": 2160},\n"
            << "  \"measured_frames\": " << measured_frames << ",\n"
            << "  \"submission_ms\": " << submission_ms << ",\n"
            << "  \"prepare_ms\": " << prepare_ms << ",\n"
            << "  \"frame_ms\": {\"p50\": " << percentile(frame_times, 0.50)
            << ", \"p95\": " << percentile(frame_times, 0.95)
            << ", \"p99\": " << percentile(frame_times, 0.99) << "},\n"
            << "  \"frame_plan_ms\": {\"p50\": "
            << percentile(frame_plan_times, 0.50)
            << ", \"p95\": " << percentile(frame_plan_times, 0.95)
            << ", \"p99\": " << percentile(frame_plan_times, 0.99) << "},\n"
            << "  \"upload_queue_ms\": {\"p50\": "
            << percentile(queue_times, 0.50)
            << ", \"p95\": " << percentile(queue_times, 0.95)
            << ", \"p99\": " << percentile(queue_times, 0.99) << "},\n"
            << "  \"prepared_points\": " << scene->curve_points().size()
            << ",\n"
            << "  \"memory_bytes\": {\"cpu_source\": "
            << sample_count * (sizeof(double) + sizeof(float))
            << ", \"cpu_derived\": " << snapshot->cpu_derived_bytes
            << ", \"cpu_budget\": " << snapshot->maximum_cpu_derived_bytes
            << ", \"gpu_planned\": " << upload.value().total_bytes()
            << ", \"gpu_budget\": " << snapshot->maximum_gpu_cache_bytes
            << "},\n"
            << "  \"upload\": {\"chunks\": " << upload.value().chunk_count()
            << ", \"maximum_chunk_bytes\": " << maximum_chunk
            << ", \"per_frame_budget_bytes\": "
            << snapshot->maximum_upload_bytes_per_frame << "},\n"
            << "  \"tasks\": {\"completed\": " << snapshot->completed_tasks
            << ", \"cancelled\": " << snapshot->cancelled_tasks
            << ", \"discarded\": " << snapshot->discarded_tasks << "}\n"
            << "}\n";
  return EXIT_SUCCESS;
}
