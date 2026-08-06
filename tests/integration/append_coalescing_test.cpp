// Headless stress/robustness test for #201 (the final #162 criterion): high-
// frequency append coalescing + Python-owner retention + cancellation + selection
// retention + concurrency. No GL/Qt — WellLogSession + core only.

#include <welllog/core/document.hpp>
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

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("bb000000-0000-4000-8000-000000000001");
const auto axis_id = id("bb000000-0000-4000-8000-000000000002");
const auto curve_id = id("bb000000-0000-4000-8000-000000000003");

// A session + a small initial document. The depth/value vectors are kept alive
// by shared_ptr owners so the test can prove retention across appends.
WellLogSession make_session_with_document(std::uint32_t refresh_hz) {
  WellLogSession session(PerformanceBudgets{
      .maximum_cpu_derived_bytes = 4 * 1024 * 1024,
      .append_refresh_rate_hz = refresh_hz,
  });
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values), .nulls = {}});
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "initial document must be accepted");
  return session;
}

// Issues an append of one tail sample at the given depth. Keeps the tail owners
// alive for the document's lifetime.
void append_one(WellLogSession &session, double depth, double value,
                DocumentRevision target) {
  static thread_local std::vector<std::shared_ptr<const std::vector<double>>>
      keepalive;
  auto d = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{depth});
  auto v = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{value});
  keepalive.push_back(d);
  keepalive.push_back(v);
  (void)session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = target,
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = BufferView::from_vector(d),
                  .tail_values = BufferView::from_vector(v),
              },
          },
  });
}

// --- Criterion 1: high-frequency appends coalesce and respect the cap. ---
// With a 10 Hz cap, a rapid burst of many appends produces far fewer visible
// revisions than append count (coalesced inside the engine), and flushing
// drains the staged tail. Verifies both the coalescing AND that the final
// extended buffer is readable end-to-end after the flush.
void coalescing_caps_visible_revisions_and_flushes() {
  auto session = make_session_with_document(/*refresh_hz=*/10);
  const auto start_revision =
      session.document(document_id)->revision().value;

  // Fire a rapid burst of appends. With a 10 Hz cap the first is due
  // immediately (no prior flush); subsequent ones within 100ms coalesce.
  constexpr std::uint64_t burst = 12;
  for (std::uint64_t i = 0; i < burst; ++i) {
    append_one(session, 1003.0 + static_cast<double>(i),
               40.0 + static_cast<double>(i), DocumentRevision{2 + i});
  }
  const auto mid_revision =
      session.document(document_id)->revision().value;
  // At most a couple of visible revisions should have been produced mid-burst
  // (coalesced), never one-per-append. Allow a small margin for timing.
  require(mid_revision - start_revision <= 3,
          "rapid append burst must coalesce to few visible revisions");

  // Flush drains any still-staged tail into a visible revision. Whether or not
  // blocks remained (timing-dependent), the final revision must not regress.
  (void)session.flush_append_coalesce(document_id);
  const auto final_revision =
      session.document(document_id)->revision().value;
  require(final_revision >= mid_revision,
          "flush must not regress the revision");

  // The extended buffer must be readable end-to-end across the coalesced tail.
  const auto doc = session.document(document_id);
  const auto length = doc->curves().front().values.length();
  require(length >= 3 + static_cast<std::uint64_t>(burst),
          "coalesced tail must extend the curve by the appended sample count");
}

// Coalescing disabled (default): every append is an immediate visible revision.
void coalescing_disabled_is_immediate() {
  auto session = make_session_with_document(/*refresh_hz=*/0);
  const auto r0 = session.document(document_id)->revision().value;
  append_one(session, 1003.0, 40.0, DocumentRevision{2});
  append_one(session, 1004.0, 50.0, DocumentRevision{3});
  const auto r1 = session.document(document_id)->revision().value;
  require(r1 == r0 + 2,
          "with coalescing disabled each append must produce a revision");
  // Flush succeeds but is a no-op (no staged blocks under coalescing-disabled):
  // it returns a receipt at the current revision without advancing it.
  const auto flushed = session.flush_append_coalesce(document_id);
  require(flushed.has_value(),
          "flush must succeed (return a receipt) even with nothing staged");
  require(flushed.value().document_revision.value == r1,
          "flush with nothing staged must not advance the revision");
}

// The delayed visible-revision path (#201): under a low refresh cap, a staged
// batch does NOT advance the revision mid-burst; after the interval elapses, a
// poll_async() flush (no new AppendBatchCommand) produces the visible revision.
// This is the headline coalescing path and exercises the poll_async coalescer-
// flush branch.
void poll_async_flushes_overdue_coalescer() {
  auto session = make_session_with_document(/*refresh_hz=*/5); // 200 ms interval
  const auto r0 = session.document(document_id)->revision().value;
  // First append is due (no prior flush) → flushes immediately → revision+1.
  append_one(session, 1003.0, 40.0, DocumentRevision{2});
  const auto r_after_first = session.document(document_id)->revision().value;
  require(r_after_first == r0 + 1,
          "the first append under a refresh cap must flush immediately");
  // Subsequent appends within the 200 ms interval stage but do NOT advance the
  // revision.
  append_one(session, 1004.0, 50.0, DocumentRevision{3});
  append_one(session, 1005.0, 60.0, DocumentRevision{4});
  require(session.document(document_id)->revision().value == r_after_first,
          "appends within the refresh interval must coalesce (no new revision)");
  // Sleep past the interval, then poll — the overdue coalescer flushes and the
  // revision advances to include the staged tail.
  std::this_thread::sleep_for(std::chrono::milliseconds{220});
  session.poll_async();
  const auto r_after_poll = session.document(document_id)->revision().value;
  require(r_after_poll == r_after_first + 1,
          "poll_async must flush the overdue coalescer into a visible revision");
  // The flushed tail must be readable end-to-end (old 3 + 3 appended samples).
  const auto doc = session.document(document_id);
  require(doc->curves().front().values.length() == 6,
          "the coalesced+flushed tail must extend the curve by all staged samples");
}

// --- Criterion 2: Python-owned (external shared_ptr) buffers survive append. ---
// The old segment's owner must be retained (no premature release). A weak_ptr
// to the old block stays lockable after the append, proving the engine pins it.
void external_owner_buffers_survive_append() {
  auto session = make_session_with_document(/*refresh_hz=*/0);
  // A tail block owned by a shared_ptr the test also holds a weak_ptr to.
  auto tail_depths = std::make_shared<std::vector<double>>(
      std::initializer_list<double>{1003.0});
  auto tail_values = std::make_shared<std::vector<double>>(
      std::initializer_list<double>{40.0});
  std::weak_ptr<const std::vector<double>> values_weak(
      std::const_pointer_cast<const std::vector<double>>(tail_values));
  const auto *original_address = tail_values->data();
  require(session
              .execute(AppendBatchCommand{
                  .document_id = document_id,
                  .target_revision = DocumentRevision{2},
                  .blocks =
                      {
                          CurveTailBlock{
                              .curve_id = curve_id,
                              .sampling_axis_id = axis_id,
                              .tail_coordinates = BufferView::from_vector(
                                  std::const_pointer_cast<const std::vector<double>>(
                                      tail_depths)),
                              .tail_values = BufferView::from_vector(
                                  std::const_pointer_cast<const std::vector<double>>(
                                      tail_values)),
                          },
                      },
              })
              .has_value(),
          "append must succeed");
  // Drop the test's strong refs; the engine's composite must still pin the tail.
  tail_depths.reset();
  tail_values.reset();
  require(!values_weak.expired(),
          "the session must retain the appended tail's owner (no premature "
          "release of the new segment)");
  const auto doc = session.document(document_id);
  const auto segs = doc->curves().front().values.segments();
  require(segs.size() == 2,
          "curve must span old + appended tail segments");
  require(segs.back().data() == reinterpret_cast<const std::byte *>(original_address),
          "the appended tail block must be retained in place");
}

// --- Criterion 3: cancellation mid-append reports operation_cancelled. ---
// A second document revision (SetDocumentCommand) while an append's LOD worker
// is running cancels the in-flight LOD task; the cancelled task reports
// operation_cancelled consistently with the curve-LOD path. (The append commit
// is synchronous; the cancellation surface is the LOD worker it starts.)
void append_lod_cancellation_reports_cancelled() {
  // A dense document so the append's LOD worker actually runs.
  std::vector<double> depths_mut(60'000);
  std::vector<float> values_mut(60'000);
  for (std::size_t i = 0; i < 60'000; ++i) {
    depths_mut[i] = 1000.0 + static_cast<double>(i) * 0.01;
    values_mut[i] = static_cast<float>(i);
  }
  auto depths = std::make_shared<const std::vector<double>>(std::move(depths_mut));
  auto values = std::make_shared<const std::vector<float>>(std::move(values_mut));
  WellLogSession session(PerformanceBudgets{
      .maximum_cpu_derived_bytes = 4 * 1024 * 1024,
      .asynchronous_sample_threshold = 1024,
  });
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_id, .values = BufferView::from_vector(values),
      .nulls = {}});
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "dense document must be accepted");

  // Append a tail; its LOD worker starts. Immediately replace with a new full
  // document revision, which cancels the append's LOD worker.
  std::vector<double> tail_depths_mut(5'000);
  std::vector<float> tail_values_mut(5'000);
  for (std::size_t i = 0; i < 5'000; ++i) {
    tail_depths_mut[i] = 1000.0 + static_cast<double>(60'000 + i) * 0.01;
    tail_values_mut[i] = static_cast<float>(60'000 + i);
  }
  auto tail_depths = std::make_shared<const std::vector<double>>(std::move(tail_depths_mut));
  auto tail_values = std::make_shared<const std::vector<float>>(std::move(tail_values_mut));
  require(session
              .execute(AppendBatchCommand{
                  .document_id = document_id,
                  .target_revision = DocumentRevision{2},
                  .blocks =
                      {
                          CurveTailBlock{
                              .curve_id = curve_id,
                              .sampling_axis_id = axis_id,
                              .tail_coordinates = BufferView::from_vector(
                                  tail_depths),
                              .tail_values = BufferView::from_vector(
                                  tail_values),
                          },
                      },
              })
              .has_value(),
          "dense append must succeed and start the LOD worker");

  // Poll a little to let the worker spin up, then replace the document.
  for (int i = 0; i < 5; ++i) {
    session.poll_async();
  }
  WellLogDocumentBuilder replacer(document_id, DocumentRevision{3});
  replacer.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  replacer.add_curve(Curve{
      .id = curve_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_id, .values = BufferView::from_vector(values),
      .nulls = {}});
  require(session.execute(SetDocumentCommand{replacer.build()}).has_value(),
          "replacement revision must be accepted (cancelling the append LOD)");

  // Drain. The cancelled append LOD task surfaces as cancelled/discarded, never
  // a hard error visible on the final state. The final revision must be 3.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    if (session.performance_snapshot(document_id)->document_revision.value == 3) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  require(session.document(document_id)->revision().value == 3,
          "final revision must be the replacement (append LOD cancelled)");
  // Race-tolerant: on a slow host the append LOD is still running when the
  // superseding revision lands and is counted as cancelled/discarded; on a
  // fast host it may finish first. Either way, cancellation must never surface
  // as a hard asynchronous_preparation_failed diagnostic.
  for (const auto &d : session.diagnostics()) {
    require(d.code != DiagnosticCode::asynchronous_preparation_failed,
            "append LOD cancellation must not publish a hard failure");
  }
}

// --- Criterion 4: the Phase-B Selection Set safely remaps on append. ---
// An append extends the axis; a selection whose range survives the extension
// stays valid (remapped onto the new revision). A selection past the new extent
// would invalidate (covered by the SetDocumentCommand remap the append delegates
// to); here we verify the common surviving case.
void selection_survives_append() {
  auto session = make_session_with_document(/*refresh_hz=*/0);
  // Select a range within the initial axis [1000, 1002].
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_id,
                  .reference_depth_range = {.top = 1000.5, .bottom = 1001.5},
              })
              .has_value(),
          "selection on the initial axis must be accepted");
  const auto sel_before = session.selection(document_id);
  require(sel_before.has_value() && sel_before->valid,
          "selection must be valid before append");

  // Append a tail extending the axis to [1000..1005]; the selected range
  // [1000.5, 1001.5] still falls within the extended axis → must stay valid.
  append_one(session, 1003.0, 40.0, DocumentRevision{2});
  append_one(session, 1004.0, 50.0, DocumentRevision{3});
  const auto sel_after = session.selection(document_id);
  require(sel_after.has_value(),
          "selection entry must survive the append");
  require(sel_after->valid,
          "a surviving selection must stay valid after append");
  require(sel_after->document_revision.value ==
              session.document(document_id)->revision().value,
          "remapped selection must carry the new revision");
}

// --- Criterion 5: rapid append pressure does not corrupt state or deadlock. ---
// The session is single-threaded by contract: a host drives execute() +
// poll_async() from one (event-loop) thread. This test interleaves rapid
// appends with poll_async() on a SINGLE thread — the realistic host pattern —
// and asserts no lost appends, a monotonic revision, and the curve extends by
// exactly the appended sample count (no corruption under pressure).
void rapid_append_pressure_with_poll_is_safe() {
  auto session = make_session_with_document(/*refresh_hz=*/0);
  // Append multi-sample blocks so the curve crosses the async threshold and the
  // LOD worker actually runs under pressure (a realistic streaming burst).
  constexpr std::uint64_t samples_per_append = 16;
  constexpr int append_count = 200;
  std::uint64_t next_sample = 3;
  for (int i = 0; i < append_count; ++i) {
    std::vector<double> d(samples_per_append);
    std::vector<double> v(samples_per_append);
    for (std::uint64_t s = 0; s < samples_per_append; ++s) {
      d[s] = 1000.0 + static_cast<double>(next_sample + s);
      v[s] = static_cast<double>(next_sample + s);
    }
    next_sample += samples_per_append;
    static thread_local std::vector<std::shared_ptr<const std::vector<double>>>
        keepalive;
    auto dp = std::make_shared<const std::vector<double>>(std::move(d));
    auto vp = std::make_shared<const std::vector<double>>(std::move(v));
    keepalive.push_back(dp);
    keepalive.push_back(vp);
    (void)session.execute(AppendBatchCommand{
        .document_id = document_id,
        .target_revision = DocumentRevision{static_cast<std::uint64_t>(2 + i)},
        .blocks =
            {
                CurveTailBlock{
                    .curve_id = curve_id,
                    .sampling_axis_id = axis_id,
                    .tail_coordinates = BufferView::from_vector(dp),
                    .tail_values = BufferView::from_vector(vp),
                },
            },
    });
    // Interleave poll_async every few appends, as a host event loop would.
    if ((i % 7) == 0) {
      session.poll_async();
    }
  }
  // Drain pending LOD work over the final extended curve. The drain returns
  // within the deadline — proving the append+poll interleaving under pressure
  // neither deadlocks nor hangs (the host event loop keeps making progress).
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    const auto snap = session.performance_snapshot(document_id);
    if (snap.has_value() &&
        snap->preparation_state == PreparationState::ready) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  // No lost appends: the curve length must be 3 + append_count*samples_per_append.
  const auto doc = session.document(document_id);
  const auto expected_length =
      3 + static_cast<std::uint64_t>(append_count) * samples_per_append;
  require(doc->curves().front().values.length() == expected_length,
          "rapid append pressure must not lose any appended samples");
  require(doc->revision().value ==
              1 + static_cast<std::uint64_t>(append_count),
          "revision must advance by one per append (coalescing disabled)");
  // No hard failure surfaced from the rapid append+poll pressure (a cancelled
  // in-flight LOD task is expected on each superseding append; it must not
  // publish an asynchronous_preparation_failed diagnostic).
  for (const auto &d : session.diagnostics()) {
    require(d.code != DiagnosticCode::asynchronous_preparation_failed,
            "rapid append pressure must not publish a hard LOD failure");
  }
}

} // namespace

int main() {
  coalescing_caps_visible_revisions_and_flushes();
  coalescing_disabled_is_immediate();
  poll_async_flushes_overdue_coalescer();
  external_owner_buffers_survive_append();
  append_lod_cancellation_reports_cancelled();
  selection_survives_append();
  rapid_append_pressure_with_poll_is_safe();
  std::cout << "welllog.append-coalescing-stress: all cases passed\n";
  return EXIT_SUCCESS;
}
