// #173 — Headless stress: Last-Revision-Wins under rapid replace / append,
// session destroy while workers run, export cancel isolation, and CPU paths
// (table projection + SVG) remaining available without OpenGL.

#include <welllog/export/raster.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  // Use _Exit (not std::exit) so we don't trigger CRT/DLL teardown while
  // background LOD worker jthreads are still mid-flight. On Windows,
  // std::exit -> ExitProcess runs DllMain detach under the loader lock,
  // which deadlocks if a worker thread is inside an engine DLL (#236).
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) { return EntityId::parse(text).value(); }

PerformanceBudgets async_budgets() {
  return PerformanceBudgets{
      .maximum_cpu_derived_bytes = 4 * 1024 * 1024,
      .maximum_gpu_cache_bytes = 8 * 1024 * 1024,
      .maximum_upload_bytes_per_frame = 256 * 1024,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  };
}

// Dense enough to force background prep; small enough for CI stress loops.
WellLogDocument dense_document(DocumentRevision revision, double value_offset,
                               std::size_t sample_count = 80'000) {
  auto depths = std::make_shared<std::vector<double>>(sample_count);
  auto values = std::make_shared<std::vector<float>>(sample_count);
  for (std::size_t index = 0; index < sample_count; ++index) {
    (*depths)[index] = 1000.0 + static_cast<double>(index) * 0.01;
    (*values)[index] = static_cast<float>(
        value_offset + std::sin(static_cast<double>(index) * 0.01));
  }
  const auto document_id = id("a7300000-0000-4000-8000-000000000001");
  const auto axis_id = id("a7300000-0000-4000-8000-000000000002");
  const auto curve_id = id("a7300000-0000-4000-8000-000000000003");
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
  const auto document_id = id("a7300000-0000-4000-8000-000000000001");
  const auto curve_id = id("a7300000-0000-4000-8000-000000000003");
  const auto track_id = id("a7300000-0000-4000-8000-000000000004");
  const auto scale_id = id("a7300000-0000-4000-8000-000000000005");
  const auto layer_id = id("a7300000-0000-4000-8000-000000000006");
  ScenePresentationBuilder builder(document_id,
                                   ReferenceDepthRange{
                                       .domain = DepthDomain::measured_depth,
                                       .unit = "m",
                                       .top = 1000.0,
                                       .bottom = 1799.99,
                                   },
                                   Millimetres{200.0}, "async-lrw-stress");
  builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = -2.0,
      .maximum = 200.0,
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

void wait_ready(WellLogSession &session, EntityId document_id,
                DocumentRevision expected, std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    const auto snapshot = session.performance_snapshot(document_id);
    if (snapshot.has_value() &&
        snapshot->document_revision == expected &&
        snapshot->preparation_state == PreparationState::ready &&
        session.prepared_scene(document_id) != nullptr) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  fail("timed out waiting for preparation ready");
}

void rapid_document_replace_is_last_revision_wins() {
  WellLogSession session(async_budgets());
  const auto document_id = id("a7300000-0000-4000-8000-000000000001");

  constexpr int kRevisions = 8;
  for (int rev = 1; rev <= kRevisions; ++rev) {
    const auto receipt = session.execute(SetDocumentCommand{
        dense_document(DocumentRevision{static_cast<std::uint64_t>(rev)},
                       static_cast<double>(rev) * 10.0),
    });
    require(receipt.has_value(), "document replace must succeed");
  }
  require(session.execute(SetPresentationCommand{dense_presentation()})
              .has_value(),
          "presentation must be accepted");

  wait_ready(session, document_id, DocumentRevision{kRevisions},
             std::chrono::seconds{30});

  const auto snap = session.performance_snapshot(document_id);
  require(snap.has_value() &&
              snap->document_revision == DocumentRevision{kRevisions} &&
              snap->preparation_state == PreparationState::ready,
          "only the last document revision may be ready");
  require(snap->cancelled_tasks + snap->discarded_tasks >= 1,
          "superseded prep work must cancel or discard");

  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr &&
              scene->document_revision() == DocumentRevision{kRevisions},
          "prepared scene must carry the winning revision");

  // AC6: table + SVG remain available without any OpenGL path.
  const auto doc = session.document(document_id);
  require(doc != nullptr, "document must remain readable");
  const auto tables = TableProjectionBuilder::from_document(*doc);
  require(!tables.empty() && tables.front().row_count() > 0,
          "table projection must work without OpenGL");

  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value() && !svg.value().text().empty(),
          "CPU vector SVG export must work without OpenGL");
}

void destroy_session_while_workers_run_is_safe() {
  // AC5: never write back a destroyed session (join workers in dtor).
  for (int i = 0; i < 12; ++i) {
    auto session = std::make_unique<WellLogSession>(async_budgets());
    require(session
                ->execute(SetDocumentCommand{
                    dense_document(DocumentRevision{1}, 0.0),
                })
                .has_value(),
            "submit must succeed");
    require(session
                ->execute(SetDocumentCommand{
                    dense_document(DocumentRevision{2}, 50.0),
                })
                .has_value(),
            "replace must succeed");
    require(session->execute(SetPresentationCommand{dense_presentation()})
                .has_value(),
            "presentation must succeed");
    // Brief poll so some tasks may start, then destroy.
    session->poll_async();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    session->poll_async();
    session.reset();
  }
}

void append_pressure_with_superseding_replace() {
  WellLogSession session(async_budgets());
  const auto document_id = id("a7300000-0000-4000-8000-000000000001");
  const auto axis_id = id("a7300000-0000-4000-8000-000000000002");
  const auto curve_id = id("a7300000-0000-4000-8000-000000000003");

  require(session
              .execute(SetDocumentCommand{
                  dense_document(DocumentRevision{1}, 0.0, 20'000),
              })
              .has_value(),
          "base document");
  require(session.execute(SetPresentationCommand{dense_presentation()})
              .has_value(),
          "presentation");

  wait_ready(session, document_id, DocumentRevision{1},
             std::chrono::seconds{30});

  // Append a small tail, then immediately supersede with a full replace.
  auto tail_depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1200.0, 1200.01, 1200.02});
  // Match curve scalar type (float) for a valid append attempt.
  auto tail_values = std::make_shared<const std::vector<float>>(
      std::vector<float>{1.0f, 2.0f, 3.0f});
  const auto append = session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = BufferView::from_vector(tail_depths),
                  .tail_values = BufferView::from_vector(tail_values),
              },
          },
  });
  // Append may succeed or be superseded; either is fine if final state is clean.
  (void)append;

  require(session
              .execute(SetDocumentCommand{
                  dense_document(DocumentRevision{3}, 99.0, 25'000),
              })
              .has_value(),
          "superseding replace");
  // SetDocumentCommand clears presentation; re-install so Prepared Scene can
  // form after the winning LOD revision is ready.
  require(session.execute(SetPresentationCommand{dense_presentation()})
              .has_value(),
          "re-apply presentation after replace");
  wait_ready(session, document_id, DocumentRevision{3},
             std::chrono::seconds{30});
  const auto snap = session.performance_snapshot(document_id);
  require(snap.has_value() &&
              snap->document_revision == DocumentRevision{3} &&
              snap->preparation_state == PreparationState::ready,
          "replace after append must win");
}

void export_cancel_does_not_corrupt_session_scene() {
  WellLogSession session;
  const auto document_id = id("a7300000-0000-4000-8000-000000000001");
  require(session
              .execute(SetDocumentCommand{
                  dense_document(DocumentRevision{1}, 0.0, 5'000),
              })
              .has_value(),
          "document");
  require(session.execute(SetPresentationCommand{dense_presentation()})
              .has_value(),
          "presentation");
  const auto scene_ptr = session.prepared_scene(document_id);
  require(scene_ptr != nullptr, "sync prep should yield a scene");

  ExportSnapshot snapshot{
      .document_id = document_id,
      .document_revision = DocumentRevision{1},
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{.domain = DepthDomain::measured_depth,
                                   .unit = "m",
                                   .reference_top = 1000.0,
                                   .reference_bottom = 1050.0,
                                   .version = 1},
      .font_asset_fingerprint = "async-lrw-stress",
      .page =
          ExportPageSpec{
              .mode = PaginationMode::continuous,
              .page_width = Millimetres{80.0},
              .page_height = Millimetres{120.0},
              .dpi = 100,
              .well_name = "stress",
          },
  };

  const auto path =
      std::filesystem::temp_directory_path() / "welllog-173-export-cancel.png";
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 400,
      .height_px = 4000,
      .tile_height_px = 8,
  };
  auto job = RasterExportJob::start(*scene_ptr, snapshot, req);
  require(job.has_value(), "export job starts");
  job.value()->request_cancel();

  auto state = RasterExportState::running;
  for (int i = 0; i < 500 && state == RasterExportState::running; ++i) {
    state = job.value()->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  require(state == RasterExportState::cancelled ||
              state == RasterExportState::failed ||
              state == RasterExportState::completed,
          "export reaches terminal state");

  // Session scene still usable after export cancel.
  const auto still = session.prepared_scene(document_id);
  require(still != nullptr && still->document_revision() == DocumentRevision{1},
          "session prepared scene survives export cancel");
  const auto svg = SvgExporter::write(*still);
  require(svg.has_value(), "SVG after export cancel");
  std::filesystem::remove(path);
}

} // namespace

int main() {
  rapid_document_replace_is_last_revision_wins();
  std::cout << "PASS: rapid document replace LRW + table/SVG\n";
  destroy_session_while_workers_run_is_safe();
  std::cout << "PASS: destroy session under async workers\n";
  append_pressure_with_superseding_replace();
  std::cout << "PASS: append + superseding replace\n";
  export_cancel_does_not_corrupt_session_scene();
  std::cout << "PASS: export cancel isolation\n";
  return EXIT_SUCCESS;
}
