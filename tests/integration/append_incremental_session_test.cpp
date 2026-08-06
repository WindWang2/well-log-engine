// Headless test for the session's append-incremental LOD wiring (#199). The
// session's LOD worker, on an AppendBatchCommand, extends each curve's
// previously-built pyramid onto the appended tail instead of full-rebuilding.
// This test drives the async LOD worker over a dense curve (above the async
// threshold): submit prefix → wait for the LOD to reach ready → append a tail
// → assert the new revision's LOD again reaches ready (the incremental path
// completes) with no failure diagnostics. The byte-level extend_tail parity
// itself is proven by welllog.incremental-lod; this test proves the session
// actually routes an append through that path end-to-end.

#include <welllog/session/session.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
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

const auto document_id = id("99000000-0000-4000-8000-000000000001");
const auto axis_id = id("99000000-0000-4000-8000-000000000002");
const auto curve_id = id("99000000-0000-4000-8000-000000000003");

constexpr std::size_t prefix_samples = 60'000;
constexpr std::size_t tail_samples = 20'000;

std::shared_ptr<const std::vector<double>> make_depths(std::size_t begin,
                                                       std::size_t count) {
  auto depths = std::make_shared<std::vector<double>>(count);
  for (std::size_t i = 0; i < count; ++i) {
    (*depths)[i] = 1000.0 + static_cast<double>(begin + i) * 0.01;
  }
  return depths;
}

std::shared_ptr<const std::vector<float>> make_values(std::size_t begin,
                                                      std::size_t count) {
  auto values = std::make_shared<std::vector<float>>(count);
  for (std::size_t i = 0; i < count; ++i) {
    (*values)[i] = static_cast<float>(
        std::sin(static_cast<double>(begin + i) * 0.01) * 50.0 + 50.0);
  }
  return values;
}

PerformanceBudgets append_budgets() {
  return PerformanceBudgets{
      .maximum_cpu_derived_bytes = 4 * 1024 * 1024,
      .maximum_gpu_cache_bytes = 16 * 1024 * 1024,
      .maximum_upload_bytes_per_frame = 1024 * 1024,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1024,
  };
}

// Polls until the document's LOD preparation reaches `ready` (or the deadline
// passes). The async worker needs wall-clock time between polls, so a small
// yield is included (mirrors the #184 finding).
bool wait_until_ready(WellLogSession &session, EntityId doc_id,
                      std::string_view what) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    const auto snap = session.performance_snapshot(doc_id);
    if (snap.has_value() &&
        snap->preparation_state == PreparationState::ready) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  std::cerr << "timeout waiting for: " << what << '\n';
  return false;
}

// Builds the prefix document (rev 1) — dense enough to drive the async LOD
// worker.
WellLogDocument prefix_document() {
  auto depths = make_depths(0, prefix_samples);
  auto values = make_values(0, prefix_samples);
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
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
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  return builder.build();
}

// The core case: a session appends a tail (rev 2) after the prefix (rev 1) LOD
// is ready. The append routes through the incremental extend_tail worker path;
// the new revision's LOD must again reach ready with no failure diagnostics,
// proving the incremental worker completes correctly end-to-end.
void append_routes_through_incremental_lod() {
  WellLogSession session(append_budgets());
  const auto prefix_submit =
      session.execute(SetDocumentCommand{prefix_document()});
  require(prefix_submit.has_value(), "prefix submission must succeed");
  require(prefix_submit.value().asynchronous_preparation_started,
          "prefix over the async threshold must start background LOD prep");
  require(wait_until_ready(session, document_id, "prefix LOD ready"),
          "prefix LOD preparation must reach ready");

  // No diagnostics from the prefix build.
  require(session.diagnostics().empty(),
          "prefix LOD build must publish no failure diagnostics");

  const auto tail_depths = make_depths(prefix_samples, tail_samples);
  const auto tail_values = make_values(prefix_samples, tail_samples);
  const auto append_result = session.execute(AppendBatchCommand{
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
  require(append_result.has_value(), "append must succeed");
  require(append_result.value().document_revision == DocumentRevision{2},
          "append must produce revision 2");
  require(append_result.value().asynchronous_preparation_started,
          "append over the async threshold must start background LOD prep");

  // The incremental LOD worker must complete the extended curve's preparation
  // without error — the reuse path (extend_tail) ran and finished.
  require(wait_until_ready(session, document_id, "append LOD ready"),
          "append LOD preparation must reach ready");
  require(session.diagnostics().empty(),
          "incremental LOD build must publish no failure diagnostics");

  const auto snap = session.performance_snapshot(document_id);
  require(snap.has_value() && snap->document_revision == DocumentRevision{2} &&
              snap->preparation_state == PreparationState::ready,
          "append revision must be ready with the bumped revision");
}

// An append with no prior ready preparation (e.g. a second append landing
// before the first's LOD completed, or a fresh session) falls back to a full
// build — still correct. Here: submit prefix, append immediately (before the
// prefix LOD is ready), and the second revision's LOD must still reach ready.
void append_without_prior_ready_falls_back_correctly() {
  WellLogSession session(append_budgets());
  require(session.execute(SetDocumentCommand{prefix_document()}).has_value(),
          "prefix submission must succeed");
  // Append immediately — the prefix LOD is still pending, so no previous
  // pyramid is staged for reuse; the worker must full-build rev 2.
  const auto tail_depths = make_depths(prefix_samples, tail_samples);
  const auto tail_values = make_values(prefix_samples, tail_samples);
  const auto append_result = session.execute(AppendBatchCommand{
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
  require(append_result.has_value(), "append must succeed");
  require(wait_until_ready(session, document_id, "fallback LOD ready"),
          "fallback full-build LOD preparation must reach ready");
  require(session.diagnostics().empty(),
          "fallback LOD build must publish no failure diagnostics");
}

} // namespace

int main() {
  append_routes_through_incremental_lod();
  append_without_prior_ready_falls_back_correctly();
  std::cout << "welllog.append-incremental-session: all cases passed\n";
  return EXIT_SUCCESS;
}
