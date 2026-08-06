// Headless test for the session's append viewport mode (#200, ADR 0031 "Session
// 可固定视口或跟随最新深度"). On an AppendBatchCommand producing a new revision,
// the session either preserves the current viewport (Fixed, the default) or
// advances the viewport's bottom to the new tail's reference depth, preserving
// the span (Follow-Latest). No GL/Qt — WellLogSession + core only.

#include <welllog/core/document.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
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

const auto document_id = id("aa000000-0000-4000-8000-000000000001");
const auto axis_id = id("aa000000-0000-4000-8000-000000000002");
const auto curve_id = id("aa000000-0000-4000-8000-000000000003");
const auto track_id = id("aa000000-0000-4000-8000-000000000004");
const auto scale_id = id("aa000000-0000-4000-8000-000000000005");
const auto layer_id = id("aa000000-0000-4000-8000-000000000006");

// A minimal presentation over [1000, 1002]. SetPresentationCommand establishes
// the initial viewport + pixel height (the first viewport a session holds), so
// the viewport-mode test can then adjust it and observe the append's effect.
ScenePresentation make_presentation() {
  ScenePresentationBuilder builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth, .unit = "m",
          .top = 1000.0, .bottom = 1002.0,
      },
      Millimetres{500.0}, "append-viewport-mode-fixture");
  builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
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

// Fixture: increasing-MD axis [1000..1002] with a matching GR curve, a
// presentation (which establishes an initial viewport), and a viewport
// [1000, 1001] (span 1.0) at 1080px. The host tail to append extends the axis
// to 1004.
struct Fixture {
  WellLogSession session;
  Fixture() {
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
            "fixture document must be accepted");
    require(session.execute(SetPresentationCommand{make_presentation()})
                .has_value(),
            "fixture presentation must be accepted");
    // Adjust the viewport to [1000, 1001] (span 1.0) at 1080px so the append
    // mode has a known window to preserve or advance.
    require(session
                .execute(SetViewportCommand{
                    .document_id = document_id,
                    .viewport = {.top = 1000.0, .bottom = 1001.0},
                })
                .has_value(),
            "fixture viewport must be accepted");
    require(session
                .execute(SetViewportMetricsCommand{
                    .document_id = document_id,
                    .viewport = {.top = 1000.0, .bottom = 1001.0},
                    .pixel_height = 1080,
                })
                .has_value(),
            "fixture viewport metrics must be accepted");
    session.clear_events();
  }
};

// Issues an append that extends the axis to [1000..1004] (tail [1003, 1004]).
Result<CommandReceipt> append_tail(WellLogSession &session,
                                   DocumentRevision target) {
  auto tail_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1003.0, 1004.0});
  auto tail_values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{40.0, 50.0});
  // Keep the owners alive for the document's lifetime.
  static thread_local std::vector<std::shared_ptr<const std::vector<double>>>
      keepalive;
  keepalive.push_back(tail_depths);
  keepalive.push_back(tail_values);
  return session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = target,
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
}

// Fixed mode (the default): the viewport is preserved across the append.
void fixed_mode_preserves_viewport() {
  Fixture f;
  // Default mode is Fixed; assert that before setting anything.
  require(f.session.append_viewport_mode(document_id) ==
              AppendViewportMode::fixed,
          "default append viewport mode must be fixed");

  const auto result = append_tail(f.session, DocumentRevision{2});
  require(result.has_value(), "append must succeed");

  const auto vp = f.session.viewport(document_id);
  require(vp.has_value(), "viewport must still be present after append");
  require(vp->top == 1000.0 && vp->bottom == 1001.0,
          "Fixed mode must preserve the viewport window unchanged");
  require(f.session.viewport_pixel_height(document_id).value_or(0) == 1080,
          "Fixed mode must preserve the viewport pixel height");
}

// Follow-Latest mode: the viewport bottom advances to the new tail's last
// reference depth (1004), preserving the span (1.0) → top = 1003.
void follow_latest_advances_viewport() {
  Fixture f;
  f.session.set_append_viewport_mode(document_id,
                                     AppendViewportMode::follow_latest);
  require(f.session.append_viewport_mode(document_id) ==
              AppendViewportMode::follow_latest,
          "set mode must be observable on the session");

  const auto result = append_tail(f.session, DocumentRevision{2});
  require(result.has_value(), "append must succeed");

  const auto vp = f.session.viewport(document_id);
  require(vp.has_value(), "viewport must still be present after append");
  require(vp->bottom == 1004.0,
          "Follow-Latest must advance the bottom to the new tail depth");
  // Span preserved: 1001 - 1000 = 1.0 → top = 1004 - 1.0 = 1003.
  require(vp->top == 1003.0,
          "Follow-Latest must preserve the viewport span");
  require(f.session.viewport_pixel_height(document_id).value_or(0) == 1080,
          "Follow-Latest must preserve the viewport pixel height");
}

// The mode is per-document and observable on the session (set + read round
// trip), and the default for an untouched document is Fixed.
void mode_is_observable_and_defaults_fixed() {
  WellLogSession session;
  require(session.append_viewport_mode(document_id) ==
              AppendViewportMode::fixed,
          "an untouched document's mode must default to fixed");
  session.set_append_viewport_mode(document_id,
                                   AppendViewportMode::follow_latest);
  require(session.append_viewport_mode(document_id) ==
              AppendViewportMode::follow_latest,
          "set follow_latest must be readable back");
  session.set_append_viewport_mode(document_id, AppendViewportMode::fixed);
  require(session.append_viewport_mode(document_id) ==
              AppendViewportMode::fixed,
          "set fixed must be readable back");
}

// A Follow-Latest append publishes a viewport_changed event at the new
// revision, so the host/view can observe the advanced window.
void follow_latest_publishes_viewport_changed_event() {
  Fixture f;
  f.session.set_append_viewport_mode(document_id,
                                     AppendViewportMode::follow_latest);
  f.session.clear_events();
  const auto result = append_tail(f.session, DocumentRevision{2});
  require(result.has_value(), "append must succeed");
  const auto events = f.session.events();
  require(std::any_of(events.begin(), events.end(),
                      [](const ViewEvent &e) {
                        return e.kind == ViewEventKind::viewport_changed;
                      }),
          "Follow-Latest append must publish a viewport_changed event");
}

// An append with no prior viewport (never set) leaves the viewport cleared in
// both modes — there is nothing to preserve or advance.
void append_with_no_prior_viewport_stays_cleared() {
  WellLogSession session;
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
          "document submission must succeed");
  session.set_append_viewport_mode(document_id,
                                   AppendViewportMode::follow_latest);
  require(append_tail(session, DocumentRevision{2}).has_value(),
          "append must succeed");
  require(!session.viewport(document_id).has_value(),
          "append with no prior viewport must leave the viewport cleared");
}

// Follow-Latest on a DECREASING axis: the appended tail's newest sample is the
// SHALLOWEST depth (the axis descends), so it must become the viewport's `top`
// and the span extends downward (bottom = top + span). A DepthViewport is always
// normalized top<bottom regardless of axis direction. Regression-guards the
// direction-aware Follow-Latest math (initially direction-blind → wrong window
// below the data extent on a decreasing axis).
void follow_latest_advances_on_decreasing_axis() {
  WellLogSession session;
  // Decreasing axis [1002, 1001, 1000]; tail [999, 998] continues descending.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1002.0, 1001.0, 1000.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{30.0, 20.0, 10.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::decreasing});
  builder.add_curve(Curve{
      .id = curve_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values), .nulls = {}});
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "decreasing-axis document must be accepted");
  require(session.execute(SetPresentationCommand{make_presentation()})
              .has_value(),
          "decreasing-axis presentation must be accepted");
  // Viewport [1000, 1001] (span 1.0), normalized top<bottom.
  require(session
              .execute(SetViewportCommand{
                  .document_id = document_id,
                  .viewport = {.top = 1000.0, .bottom = 1001.0},
              })
              .has_value(),
          "decreasing-axis viewport must be accepted");
  require(session
              .execute(SetViewportMetricsCommand{
                  .document_id = document_id,
                  .viewport = {.top = 1000.0, .bottom = 1001.0},
                  .pixel_height = 1080,
              })
              .has_value(),
          "decreasing-axis viewport metrics must be accepted");
  session.set_append_viewport_mode(document_id,
                                   AppendViewportMode::follow_latest);
  session.clear_events();

  // Append the descending tail [999, 998]; the newest sample is 998 (shallowest).
  auto tail_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{999.0, 998.0});
  auto tail_values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{5.0, 2.0});
  static thread_local std::vector<std::shared_ptr<const std::vector<double>>>
      keepalive;
  keepalive.push_back(tail_depths);
  keepalive.push_back(tail_values);
  require(session
              .execute(AppendBatchCommand{
                  .document_id = document_id,
                  .target_revision = DocumentRevision{2},
                  .blocks =
                      {
                          CurveTailBlock{
                              .curve_id = curve_id,
                              .sampling_axis_id = axis_id,
                              .tail_coordinates =
                                  BufferView::from_vector(tail_depths),
                              .tail_values = BufferView::from_vector(tail_values),
                          },
                      },
              })
              .has_value(),
          "decreasing-axis append must succeed");

  const auto vp = session.viewport(document_id);
  require(vp.has_value(), "viewport must be present after decreasing append");
  // Tail newest = 998 → top; span 1.0 → bottom = 999. Normalized top<bottom.
  require(vp->top == 998.0,
          "Follow-Latest on a decreasing axis must set top to the tail's "
          "newest (shallowest) depth");
  require(vp->bottom == 999.0,
          "Follow-Latest on a decreasing axis must extend bottom by the span");
  require(vp->top < vp->bottom,
          "Follow-Latest viewport must stay normalized top < bottom");
}

} // namespace

int main() {
  fixed_mode_preserves_viewport();
  follow_latest_advances_viewport();
  mode_is_observable_and_defaults_fixed();
  follow_latest_publishes_viewport_changed_event();
  append_with_no_prior_viewport_stays_cleared();
  follow_latest_advances_on_decreasing_axis();
  std::cout << "welllog.append-viewport-mode: all cases passed\n";
  return EXIT_SUCCESS;
}
