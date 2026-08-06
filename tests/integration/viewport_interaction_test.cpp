#include <welllog/session/session.hpp>

#include <cmath>
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

void require_near(double actual, double expected, std::string_view message) {
  if (std::abs(actual - expected) > 1.0e-9) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

struct SessionFixture {
  EntityId document_id;
  WellLogSession session;
};

SessionFixture prepared_session() {
  const auto document_id = id("50000000-0000-4000-8000-000000000001");
  const auto axis_id = id("50000000-0000-4000-8000-000000000002");
  const auto curve_id = id("50000000-0000-4000-8000-000000000003");
  const auto track_id = id("50000000-0000-4000-8000-000000000004");
  const auto scale_id = id("50000000-0000-4000-8000-000000000005");
  const auto layer_id = id("50000000-0000-4000-8000-000000000006");
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 50.0, 100.0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{1});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "fixture document must be accepted");
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1100.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation_builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 0});
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 0x12, .green = 0x34, .blue = 0x56},
      .line_width = Millimetres{0.25},
      .z_order = 0,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "fixture presentation must be accepted");
  session.clear_events();
  return SessionFixture{
      .document_id = document_id,
      .session = std::move(session),
  };
}

void session_owns_pan_zoom_and_reset_state() {
  auto fixture = prepared_session();
  const auto initial = fixture.session.viewport(fixture.document_id);
  require(initial.has_value(), "presentation must establish a viewport");
  require_near(initial->top, 1000.0,
               "initial viewport top must use the presentation range");
  require_near(initial->bottom, 1100.0,
               "initial viewport bottom must use the presentation range");

  require(fixture.session
              .execute(SetViewportCommand{
                  .document_id = fixture.document_id,
                  .viewport = DepthViewport{.top = 1020.0, .bottom = 1060.0},
              })
              .has_value(),
          "explicit viewport must be accepted");
  require(fixture.session
              .execute(PanDepthCommand{
                  .document_id = fixture.document_id,
                  .display_depth_delta = 10.0,
              })
              .has_value(),
          "pan command must be accepted");
  auto viewport = fixture.session.viewport(fixture.document_id);
  require_near(viewport->top, 1030.0, "pan must move the viewport top");
  require_near(viewport->bottom, 1070.0, "pan must move the viewport bottom");

  require(fixture.session
              .execute(ZoomDepthAtCommand{
                  .document_id = fixture.document_id,
                  .anchor_display_depth = 1050.0,
                  .span_factor = 0.5,
              })
              .has_value(),
          "zoom command must be accepted");
  viewport = fixture.session.viewport(fixture.document_id);
  require_near(viewport->top, 1040.0,
               "zoom must retain the depth below the pointer");
  require_near(viewport->bottom, 1060.0,
               "zoom must retain the depth above the pointer");

  require(fixture.session
              .execute(ResetViewportCommand{
                  .document_id = fixture.document_id,
              })
              .has_value(),
          "reset command must be accepted");
  viewport = fixture.session.viewport(fixture.document_id);
  require_near(viewport->top, 1000.0,
               "reset must restore the presentation top");
  require_near(viewport->bottom, 1100.0,
               "reset must restore the presentation bottom");
  require(!fixture.session.events().empty() &&
              fixture.session.events().back().kind ==
                  ViewEventKind::viewport_changed,
          "viewport commands must publish semantic viewport events");
}

void session_owns_crosshair_state() {
  auto fixture = prepared_session();
  require(fixture.session
              .execute(SetCrosshairCommand{
                  .document_id = fixture.document_id,
                  .crosshair =
                      CrosshairState{
                          .track_fraction = 0.25,
                          .display_depth = 1042.5,
                      },
              })
              .has_value(),
          "crosshair command must be accepted");
  const auto crosshair = fixture.session.crosshair(fixture.document_id);
  require(crosshair.has_value(), "crosshair must be owned by the session");
  require_near(crosshair->track_fraction, 0.25,
               "crosshair must retain its horizontal semantic position");
  require_near(crosshair->display_depth, 1042.5,
               "crosshair must retain Display Depth");
  require(fixture.session.events().back().kind ==
              ViewEventKind::crosshair_changed,
          "crosshair changes must publish a semantic event");

  require(fixture.session
              .execute(SetCrosshairCommand{
                  .document_id = fixture.document_id,
                  .crosshair = std::nullopt,
              })
              .has_value(),
          "crosshair clearing must be accepted");
  require(!fixture.session.crosshair(fixture.document_id).has_value(),
          "cleared crosshair must not remain in session state");
}

void invalid_view_commands_leave_session_state_unchanged() {
  auto fixture = prepared_session();
  const auto before = fixture.session.viewport(fixture.document_id);
  const auto event_count = fixture.session.events().size();

  const auto result = fixture.session.execute(ZoomDepthAtCommand{
      .document_id = fixture.document_id,
      .anchor_display_depth = 1050.0,
      .span_factor = 0.0,
  });
  require(!result.has_value() &&
              result.error().code == ErrorCode::invalid_viewport,
          "non-positive zoom factors must return a stable viewport error");
  require(fixture.session.viewport(fixture.document_id) == before,
          "rejected view commands must preserve viewport state");
  require(fixture.session.events().size() == event_count,
          "rejected view commands must not publish events");
}

void view_event_observers_receive_committed_state_changes() {
  auto fixture = prepared_session();
  auto observed_count = 0;
  auto observed_viewport = DepthViewport{};
  const auto observer_id =
      fixture.session.subscribe_view_events([&](const ViewEvent &event) {
        if (event.kind == ViewEventKind::viewport_changed) {
          ++observed_count;
          observed_viewport = *fixture.session.viewport(fixture.document_id);
        }
      });
  require(observer_id != 0, "view event observer must subscribe");

  require(fixture.session
              .execute(PanDepthCommand{
                  .document_id = fixture.document_id,
                  .display_depth_delta = 5.0,
              })
              .has_value(),
          "observed viewport command must succeed");
  require(observed_count == 1,
          "observer must receive one committed viewport event");
  require_near(observed_viewport.top, 1005.0,
               "observer must see committed session state");

  fixture.session.unsubscribe_view_events(observer_id);
  require(fixture.session
              .execute(PanDepthCommand{
                  .document_id = fixture.document_id,
                  .display_depth_delta = 5.0,
              })
              .has_value(),
          "viewport command after unsubscribe must succeed");
  require(observed_count == 1,
          "unsubscribed observer must receive no further events");
}

void reentrant_observers_cannot_invalidate_command_receipts() {
  const auto replace_document_on = [](ViewEventKind observed_kind) {
    auto fixture = prepared_session();
    auto replacement_submitted = false;
    const auto observer_id =
        fixture.session.subscribe_view_events([&](const ViewEvent &event) {
          if (event.kind != observed_kind || replacement_submitted) {
            return;
          }
          replacement_submitted = true;
          const auto replacement_axis_id =
              id("50000000-0000-4000-8000-000000000010");
          const auto replacement_curve_id =
              id("50000000-0000-4000-8000-000000000011");
          auto replacement_depths = std::make_shared<const std::vector<double>>(
              std::initializer_list<double>{1000.0, 1100.0});
          auto replacement_values = std::make_shared<const std::vector<double>>(
              std::initializer_list<double>{10.0, 20.0});
          WellLogDocumentBuilder replacement_builder(fixture.document_id,
                                                     DocumentRevision{2});
          replacement_builder.add_sampling_axis(SamplingAxis{
              .id = replacement_axis_id,
              .coordinates = BufferView::from_vector(replacement_depths),
              .domain = DepthDomain::measured_depth,
              .unit = "m",
              .direction = AxisDirection::increasing,
          });
          replacement_builder.add_curve(Curve{
              .id = replacement_curve_id,
              .mnemonic = "GR2",
              .display_name = "Replacement Gamma Ray",
              .unit = "API",
              .sampling_axis_id = replacement_axis_id,
              .values = BufferView::from_vector(replacement_values),
              .nulls = {},
          });
          require(fixture.session
                      .execute(SetDocumentCommand{replacement_builder.build()})
                      .has_value(),
                  "reentrant replacement document must be accepted");
        });
    require(observer_id != 0, "reentrant observer must subscribe");

    const auto result = observed_kind == ViewEventKind::viewport_changed
                            ? fixture.session.execute(PanDepthCommand{
                                  .document_id = fixture.document_id,
                                  .display_depth_delta = 1.0,
                              })
                            : fixture.session.execute(SetCrosshairCommand{
                                  .document_id = fixture.document_id,
                                  .crosshair =
                                      CrosshairState{
                                          .track_fraction = 0.5,
                                          .display_depth = 1050.0,
                                      },
                              });
    require(result.has_value(), "outer observed command must succeed");
    require(result.value().document_revision == DocumentRevision{1},
            "command receipt must retain its committed revision snapshot");
    require(fixture.session.document(fixture.document_id)->revision() ==
                DocumentRevision{2},
            "observer must have replaced the document reentrantly");
  };

  replace_document_on(ViewEventKind::viewport_changed);
  replace_document_on(ViewEventKind::crosshair_changed);
}

} // namespace

int main() {
  session_owns_pan_zoom_and_reset_state();
  session_owns_crosshair_state();
  invalid_view_commands_leave_session_state_unchanged();
  view_event_observers_receive_committed_state_changes();
  reentrant_observers_cannot_invalidate_command_receipts();
  std::cout << "PASS: session viewport interaction behavior\n";
  return EXIT_SUCCESS;
}
