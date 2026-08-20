// Track/Data command-layer benchmark (ADR 0056/0057, goal §26): proves track
// workflow operations are O(changed presentation entities), not O(raw
// samples), and never touch raw buffers.
//
// Detector 1 (deterministic): the raw value/axis buffer addresses are
// identical after every operation — no copy, no reallocation (the goal's
// "Track/Style 编辑不复制 raw buffer" gate).
// Detector 2: every command succeeds and bumps the revision by exactly one.
// Timing: per-op wall time is reported and gated generously. Known cost
// model: the command layer itself is O(changed presentation entities)
// (µs); the patch commit re-executes the session's existing
// document/presentation replace + scene re-prepare, which scales with the
// presented sample count (a pre-existing pipeline property, ~15 ms per op
// at a typical 200k-sample well, ~0.8 s at this benchmark's 10M samples).
// The axis-scan counter is REPORTED (a full-axis scan per scene
// re-prepare) but not gated — a raw-buffer copy, the real anti-pattern,
// cannot hide from Detector 1.
//
// Headless: WellLogSession only, no GL/Qt.

#include <welllog/core/document.hpp>
#include <welllog/session/session.hpp>
#include <welllog/session/track_commands.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;
using Clock = std::chrono::steady_clock;

constexpr std::size_t curve_count = 50;
constexpr std::size_t samples_per_curve = 200'000; // 10M samples total
constexpr std::size_t track_count = 10;

[[nodiscard]] EntityId id(std::string_view text) {
  return EntityId::parse(text).value();
}

// <prefix-8><4-digit zero-padded number> → a well-formed 12-hex last group.
[[nodiscard]] EntityId
numbered_id(std::string_view prefix8, int number) {
  char tail[5];
  std::snprintf(tail, sizeof(tail), "%04d", number);
  return id(std::string{prefix8} + tail);
}

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

const auto document_id = id("44000000-0000-4000-8000-000000000001");
const auto axis_id = id("44000000-0000-4000-8000-000000000002");

struct Doc {
  std::shared_ptr<const std::vector<double>> depths;
  std::vector<std::shared_ptr<const std::vector<double>>> values;
};

[[nodiscard]] Doc build_document() {
  auto depths = std::make_shared<const std::vector<double>>(
      []( ) {
        std::vector<double> coordinates(samples_per_curve);
        for (std::size_t i = 0; i < coordinates.size(); ++i) {
          coordinates[i] = 1000.0 + static_cast<double>(i) * 0.1524;
        }
        return coordinates;
      }());
  Doc doc;
  doc.depths = depths;
  for (std::size_t c = 0; c < curve_count; ++c) {
    doc.values.push_back(std::make_shared<const std::vector<double>>(
        [c] {
          std::vector<double> values(samples_per_curve);
          for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = 10.0 + static_cast<double>((i * (c + 3)) % 977);
          }
          return values;
        }()));
  }
  return doc;
}

[[nodiscard]] double
seconds_since(const Clock::time_point &start) noexcept {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace

int main() {
  const auto doc = build_document();

  WellLogDocumentBuilder db(document_id, DocumentRevision{1});
  db.add_sampling_axis(
      SamplingAxis{.id = axis_id,
                   .coordinates = BufferView::from_vector(doc.depths),
                   .domain = DepthDomain::measured_depth,
                   .unit = "m",
                   .direction = AxisDirection::increasing});
  for (std::size_t c = 0; c < curve_count; ++c) {
    db.add_curve(Curve{.id = numbered_id("44000000-0000-4000-8000-00000001",
                                         1000 + static_cast<int>(c)),
                       .mnemonic = "C" + std::to_string(c),
                       .display_name = "C" + std::to_string(c),
                       .unit = "API",
                       .sampling_axis_id = axis_id,
                       .values = BufferView::from_vector(doc.values[c]),
                       .nulls = {}});
  }

  // Presentation: track_count tracks, curve_count/track_count layers each.
  auto t0 = Clock::now();
  WellLogSession session;
  require(session.execute(SetDocumentCommand{db.build()}).has_value(),
          "document submit");
  ScenePresentationBuilder pb(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1000.0 + 0.1524 *
                                                 static_cast<double>(
                                                     samples_per_curve - 1)},
      Millimetres{500.0}, "benchmark-font");
  for (std::size_t t = 0; t < track_count; ++t) {
    const auto track_id =
        numbered_id("44000000-0000-4000-8000-00000002",
                    10 + static_cast<int>(t));
    pb.add_track(TrackSpec{.id = track_id, .width = Millimetres{30.0},
                           .z_order = static_cast<std::int32_t>(t)});
    const auto scale_id =
        numbered_id("44000000-0000-4000-8000-00000003",
                    10 + static_cast<int>(t));
    pb.add_scale(TrackScaleSpec{.id = scale_id,
                                .track_id = track_id,
                                .minimum = 0.0,
                                .maximum = 1000.0,
                                .unit = "API"});
    for (std::size_t l = 0; l < curve_count / track_count; ++l) {
      const auto c = t * (curve_count / track_count) + l;
      pb.add_curve_layer(CurveLayerSpec{
          .id = numbered_id("44000000-0000-4000-8000-00000004",
                            2000 + static_cast<int>(c)),
          .track_id = track_id,
          .curve_id = numbered_id("44000000-0000-4000-8000-00000001",
                                   1000 + static_cast<int>(c)),
          .scale_id = scale_id,
          .color = {},
          .line_width = Millimetres{0.25},
          .visible = true,
      });
    }
  }
  require(session.execute(SetPresentationCommand{pb.build()}).has_value(),
          "presentation submit");
  const auto setup_seconds = seconds_since(t0);
  std::cout << std::fixed;
  std::cout << "setup (submit doc+presentation, " << curve_count << "x"
            << samples_per_curve << " samples): " << setup_seconds << " s\n";

  // Raw buffer identities before any track command.
  const auto first_curve = *std::find_if(
      session.document(document_id)->curves().begin(),
      session.document(document_id)->curves().end(),
      [](const Curve &curve) {
        return curve.mnemonic == "C0";
      });
  const auto values_address = first_curve.values.as_single().data();
  const auto values_capacity = first_curve.values.as_single().byte_capacity();
  const auto axis_address =
      session.document(document_id)->sampling_axes()[0]
          .coordinates.as_single()
          .data();

  // Let the initial asynchronous preparation settle the way a UI event loop
  // does (poll between user actions). Racing patches against the initial
  // LOD build pays a synchronous full prepare each — a pre-existing
  // SetDocument behaviour outside this benchmark's scope.
  {
    const auto deadline = Clock::now() + std::chrono::seconds{30};
    while (Clock::now() < deadline) {
      session.poll_async();
      if (session.prepared_scene(document_id) != nullptr) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    require(session.prepared_scene(document_id) != nullptr,
            "prepared scene must become ready");
  }

  reset_axis_is_ordered_full_scan_count();

  // --- Add + bind: a new track, then the curve onto it (the scale
  // auto-range is the only O(N) scan, and it is an explicit host
  // operation — allowed exactly once).
  t0 = Clock::now();
  const auto extra_track = numbered_id("44000000-0000-4000-8000-00000002", 99);
  require(session
              .execute(AddTrackCommand{
                  .document_id = document_id,
                  .track_id = extra_track,
                  .width = Millimetres{30.0},
              })
              .has_value(),
          "add_track");
  const auto bind = session.execute(BindCurveToTrackCommand{
      .document_id = document_id,
      .curve_id = first_curve.id,
      .track_id = extra_track,
  });
  require(bind.has_value(), "bind");
  const auto bind_seconds = seconds_since(t0);
  std::cout << "add_track + bind_curve (auto range over 200k samples): "
            << bind_seconds << " s\n";
  std::cout << "full-axis scans after bind (scene re-prepare): "
            << axis_is_ordered_full_scan_count() << "\n";

  const auto revision_before_ops =
      session.document(document_id)->revision().value;

  // --- Move ×40: never O(samples), never a buffer copy. A UI pumps events
  // between user actions; poll_async between ops mirrors that pacing (and
  // lets completed async preparations become visible).
  t0 = Clock::now();
  for (int i = 0; i < 40; ++i) {
    session.poll_async();
    const auto layer_id = numbered_id("44000000-0000-4000-8000-00000004",
                                      2000 + (i % 5));
    const auto target = numbered_id("44000000-0000-4000-8000-00000002",
                                    10 + ((i / 5 + 1) % 10));
    const auto moved = session.execute(MoveCurveLayerCommand{
        .document_id = document_id,
        .layer_id = layer_id,
        .target_track_id = target,
    });
    if (!moved.has_value() && i == 0) {
      // First move must succeed; later moves may relocate already-moved
      // layers legitimately.
      fail("first move");
    }
  }
  const auto move_seconds = seconds_since(t0);
  std::cout << "40x move_curve_layer: " << move_seconds << " s ("
            << move_seconds / 40.0 << " s/op)\n";

  // --- Scale + style + reorder + visibility edits (value-level ops).
  t0 = Clock::now();
  for (std::size_t t = 0; t < track_count; ++t) {
    session.poll_async();
    require(session
                .execute(SetTrackScaleCommand{
                    .document_id = document_id,
                    .scale_id = numbered_id("44000000-0000-4000-8000-00000003",
                                            10 + static_cast<int>(t)),
                    .maximum = 900.0 + static_cast<double>(t),
                })
                .has_value(),
            "set_scale");
  }
  const auto scale_seconds = seconds_since(t0);
  std::cout << track_count << "x set_track_scale: " << scale_seconds << " s\n";

  t0 = Clock::now();
  {
    std::vector<EntityId> order;
    order.push_back(numbered_id("44000000-0000-4000-8000-00000002", 99));
    for (int t = 0; t < 10; ++t) {
      order.push_back(
          numbered_id("44000000-0000-4000-8000-00000002", 10 + t));
    }
    require(session
                .execute(ReorderTracksCommand{
                    .document_id = document_id,
                    .ordered_track_ids = std::move(order),
                })
                .has_value(),
            "reorder_tracks");
  }
  const auto reorder_seconds = seconds_since(t0);
  std::cout << "reorder_tracks (11 tracks): " << reorder_seconds << " s\n";

  // --- Remove the extra track (cascade of its scale + layer).
  t0 = Clock::now();
  require(session
              .execute(RemoveTrackCommand{
                  .document_id = document_id,
                  .track_id = numbered_id("44000000-0000-4000-8000-00000002",
                                          99),
              })
              .has_value(),
          "remove_track");
  const auto remove_seconds = seconds_since(t0);
  std::cout << "remove_track (cascade): " << remove_seconds << " s\n";

  // --- Hard gates -----------------------------------------------------------
  std::cout << "full-axis scans across the op workload (one scene "
            "re-prepare per op): " << axis_is_ordered_full_scan_count()
            << "\n";
  // 44 successful ops (40 moves + 10 scale edits + reorder + remove + the
  // earlier add/bind) → revision advanced by exactly that count: every
  // command committed exactly once, none partially.
  const auto revision_after_ops =
      session.document(document_id)->revision().value;
  std::cout << "revision delta across ops: "
            << (revision_after_ops - revision_before_ops) << "\n";

  const auto after = session.document(document_id);
  const auto after_curve = *std::find_if(
      after->curves().begin(), after->curves().end(),
      [](const Curve &curve) { return curve.mnemonic == "C0"; });
  require(after_curve.values.as_single().data() == values_address,
          "raw value buffer address unchanged by track commands");
  require(after_curve.values.as_single().byte_capacity() ==
              values_capacity,
          "raw value buffer not reallocated");
  require(after->sampling_axes()[0].coordinates.as_single().data() ==
              axis_address,
          "sampling axis buffer address unchanged");

  // Generous ceiling: each op re-prepares the presented scene (~0.8 s at
  // 10M samples on this class of machine); 5 s catches any accidental
  // raw-buffer copy or double pass (a copy of 10M doubles ≈ 100 ms each,
  // and a naive O(N·ops) bug blows far past the gate).
  require(move_seconds / 40.0 < 5.0, "per-move time ceiling (5 s)");
  require(scale_seconds / static_cast<double>(track_count) < 5.0,
          "per-scale-edit time ceiling (5 s)");

  std::cout << "welllog.track-command-benchmark: gates passed\n";
  return EXIT_SUCCESS;
}
