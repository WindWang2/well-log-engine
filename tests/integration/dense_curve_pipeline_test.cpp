#include <welllog/render_gl/upload.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace welllog;

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

WellLogDocument dense_document(DocumentRevision revision, double value_offset) {
  constexpr std::size_t sample_count = 500'000;
  auto depths = std::make_shared<std::vector<double>>(sample_count);
  auto values = std::make_shared<std::vector<float>>(sample_count);
  for (std::size_t index = 0; index < sample_count; ++index) {
    (*depths)[index] = 1000.0 + static_cast<double>(index) * 0.01;
    (*values)[index] = static_cast<float>(
        value_offset + std::sin(static_cast<double>(index) * 0.01));
  }
  (*values)[250'000] = static_cast<float>(value_offset + 10'000.0);

  const auto document_id = id("70000000-0000-4000-8000-000000000001");
  const auto axis_id = id("70000000-0000-4000-8000-000000000002");
  const auto curve_id = id("70000000-0000-4000-8000-000000000003");
  WellLogDocumentBuilder builder(document_id, revision);
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

ScenePresentation dense_presentation() {
  const auto document_id = id("70000000-0000-4000-8000-000000000001");
  const auto curve_id = id("70000000-0000-4000-8000-000000000003");
  const auto track_id = id("70000000-0000-4000-8000-000000000004");
  const auto scale_id = id("70000000-0000-4000-8000-000000000005");
  const auto layer_id = id("70000000-0000-4000-8000-000000000006");
  ScenePresentationBuilder builder(document_id,
                                   ReferenceDepthRange{
                                       .domain = DepthDomain::measured_depth,
                                       .unit = "m",
                                       .top = 1000.0,
                                       .bottom = 5999.99,
                                   },
                                   Millimetres{500.0}, "dense-fixture");
  builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = -2.0,
      .maximum = 10'101.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
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

void dense_revisions_prepare_asynchronously_and_stale_work_cannot_win() {
  constexpr std::uint64_t cpu_budget = 2 * 1024 * 1024;
  WellLogSession session(PerformanceBudgets{
      .maximum_cpu_derived_bytes = cpu_budget,
      .maximum_gpu_cache_bytes = 8 * 1024 * 1024,
      .maximum_upload_bytes_per_frame = 256 * 1024,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  });
  const auto document_id = id("70000000-0000-4000-8000-000000000001");

  const auto first = session.execute(SetDocumentCommand{
      dense_document(DocumentRevision{1}, 0.0),
  });
  require(first.has_value() && first.value().asynchronous_preparation_started,
          "dense document submission must start background LOD preparation");
  const auto pending = session.performance_snapshot(document_id);
  require(pending.has_value() &&
              pending->preparation_state == PreparationState::pending,
          "background preparation must be observable without waiting");

  const auto second = session.execute(SetDocumentCommand{
      dense_document(DocumentRevision{2}, 100.0),
  });
  require(second.has_value() && second.value().asynchronous_preparation_started,
          "replacement revision must start its own background preparation");
  const auto presentation =
      session.execute(SetPresentationCommand{dense_presentation()});
  require(presentation.has_value() &&
              presentation.value().asynchronous_preparation_started &&
              session.prepared_scene(document_id) == nullptr,
          "dense presentation must wait for the background hierarchy");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    const auto snapshot = session.performance_snapshot(document_id);
    if (snapshot.has_value() &&
        snapshot->preparation_state == PreparationState::ready &&
        session.prepared_scene(document_id) != nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  const auto ready = session.performance_snapshot(document_id);
  require(ready.has_value() &&
              ready->document_revision == DocumentRevision{2} &&
              ready->preparation_state == PreparationState::ready,
          "only the newest document revision may become ready");
  require(ready->cpu_derived_bytes <= cpu_budget,
          "background summaries must stay within the CPU cache budget");
  require(ready->cancelled_tasks + ready->discarded_tasks >= 1,
          "replaced revision work must be cancelled or discarded");

  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr && scene->document_revision() == DocumentRevision{2},
          "Prepared Scene must carry the newest revision");
  require(scene->curve_points().size() < 20'000,
          "4K preparation must not retain all 500k source points");
  require(std::any_of(
              scene->curve_points().begin(), scene->curve_points().end(),
              [](const PreparedCurvePoint &point) {
                return point.sample_index == 250'000 && point.value == 10'100.0;
              }),
          "Prepared Scene LOD must preserve the dense-curve spike");

  const auto upload = GpuUploadSchedule::plan(
      *scene, GpuUploadBudgets{
                  .maximum_cache_bytes = 8 * 1024 * 1024,
                  .maximum_bytes_per_frame = 64 * 1024,
              });
  require(upload.has_value() && upload.value().chunk_count() > 1,
          "dense geometry must be split across budgeted frame uploads");
  for (const auto &chunk : upload.value().chunks()) {
    require(chunk.byte_count <= 64 * 1024,
            "each planned upload must respect the per-frame byte budget");
  }
  require(upload.value().total_bytes() <= 8 * 1024 * 1024,
          "visible geometry must fit the configured GPU cache budget");
  const auto rejected_upload = GpuUploadSchedule::plan(
      *scene, GpuUploadBudgets{
                  .maximum_cache_bytes = upload.value().total_bytes() - 1,
                  .maximum_bytes_per_frame = 64 * 1024,
              });
  require(
      !rejected_upload.has_value() &&
          rejected_upload.error().code == ErrorCode::resource_exhausted,
      "GPU geometry must be rejected instead of exceeding its cache budget");

  const auto task_count_before_viewport =
      ready->cancelled_tasks + ready->discarded_tasks;
  const auto stale_zoom = session.execute(SetViewportMetricsCommand{
      .document_id = document_id,
      .viewport = DepthViewport{.top = 1499.0, .bottom = 1501.0},
      .pixel_height = 1,
  });
  const auto zoom = session.execute(SetViewportMetricsCommand{
      .document_id = document_id,
      .viewport = DepthViewport{.top = 3499.0, .bottom = 3501.0},
      .pixel_height = 4000,
  });
  require(stale_zoom.has_value() &&
              stale_zoom.value().asynchronous_preparation_started &&
              zoom.has_value() && zoom.value().asynchronous_preparation_started,
          "dense viewport changes must prepare frames in the background");
  const auto viewport_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < viewport_deadline) {
    session.poll_async();
    const auto snapshot = session.performance_snapshot(document_id);
    if (snapshot.has_value() && !snapshot->frame_preparation_pending) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const auto zoomed_scene = session.prepared_scene(document_id);
  require(zoomed_scene != nullptr &&
              zoomed_scene->curve_points().size() < 2'000,
          "zoomed frame must contain only visible and bounded prefetch points");
  const auto spike = std::find_if(zoomed_scene->curve_points().begin(),
                                  zoomed_scene->curve_points().end(),
                                  [](const PreparedCurvePoint &point) {
                                    return point.sample_index == 250'000;
                                  });
  require(spike != zoomed_scene->curve_points().end() &&
              spike + 1 != zoomed_scene->curve_points().end() &&
              (spike + 1)->sample_index == 250'001,
          "fine zoom must return to consecutive original samples");
  const auto after_viewport = session.performance_snapshot(document_id);
  require(after_viewport.has_value() &&
              after_viewport->cancelled_tasks +
                      after_viewport->discarded_tasks >
                  task_count_before_viewport,
          "a superseded viewport task must be cancelled or discarded");
  require(session.viewport_pixel_height(document_id) ==
              std::optional<std::uint32_t>{4000},
          "the latest physical widget height must drive local LOD density");

  WellLogSession constrained(PerformanceBudgets{
      .maximum_cpu_derived_bytes = 1,
      .maximum_gpu_cache_bytes = 8 * 1024 * 1024,
      .maximum_upload_bytes_per_frame = 256 * 1024,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  });
  const auto constrained_receipt = constrained.execute(SetDocumentCommand{
      dense_document(DocumentRevision{3}, 0.0),
  });
  require(constrained_receipt.has_value(),
          "valid dense input must be accepted before background preparation");
  const auto failure_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < failure_deadline) {
    constrained.poll_async();
    const auto snapshot = constrained.performance_snapshot(document_id);
    if (snapshot.has_value() &&
        snapshot->preparation_state == PreparationState::unavailable) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const auto failure = std::find_if(
      constrained.diagnostics().begin(), constrained.diagnostics().end(),
      [](const Diagnostic &diagnostic) {
        return diagnostic.code ==
               DiagnosticCode::asynchronous_preparation_failed;
      });
  require(failure != constrained.diagnostics().end(),
          "background preparation errors must publish stable diagnostics");
  const auto failure_error = constrained.diagnostic_error(failure->id);
  require(failure_error.has_value() &&
              failure_error->code == ErrorCode::resource_exhausted &&
              failure_error->message == MessageKey::resource_exhausted,
          "async diagnostic details must be available without changing the "
          "stable Diagnostic layout");
}

} // namespace

int main() {
  dense_revisions_prepare_asynchronously_and_stale_work_cannot_win();
  std::cout << "PASS: dense curve asynchronous preparation\n";
  return EXIT_SUCCESS;
}
