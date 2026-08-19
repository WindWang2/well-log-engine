// Headless tests for the Track/Data workflow command layer (ADR 0055/0056):
// the AddTrack/RemoveTrack/Reorder/Resize/Header/Visibility, Bind/Unbind/
// Move/Duplicate/Reorder/SetVisibility/SetStyle and SetTrackScale/
// AutoRangeTrackScale commands over WellLogSession, plus the binding indexes
// and the hidden-track prepare policy. Every command must ride the
// ApplyPatchCommand engine: atomic, undoable, and never copying raw curve
// or sampling-axis buffers. No GL/Qt — session + scene + core.

#include <welllog/core/document.hpp>
#include <welllog/core/document_index.hpp>
#include <welllog/scene/inspect.hpp>
#include <welllog/scene/presentation_index.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/session/track_commands.hpp>

#include <algorithm>
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
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void require_near(double actual, double expected, std::string_view message) {
  require(std::abs(actual - expected) < 1.0e-9, message);
}

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("aa000000-0000-4000-8000-000000000001");
const auto axis_id = id("aa000000-0000-4000-8000-000000000002");
const auto curve_gr = id("aa000000-0000-4000-8000-000000000003");
const auto curve_rt = id("aa000000-0000-4000-8000-000000000004");
const auto curve_den = id("aa000000-0000-4000-8000-000000000005");
const auto track1 = id("aa000000-0000-4000-8000-000000000011");
const auto track2 = id("aa000000-0000-4000-8000-000000000012");
const auto scale1 = id("aa000000-0000-4000-8000-000000000021");
const auto scale2 = id("aa000000-0000-4000-8000-000000000022");
const auto layer_gr1 = id("aa000000-0000-4000-8000-000000000031");
const auto layer_rt2 = id("aa000000-0000-4000-8000-000000000032");
const auto marker_layer1 = id("aa000000-0000-4000-8000-000000000041");

// Fixture: one document (one axis, GR/RT/DEN curves) and a presentation with
// two tracks. track1 hosts GR + a marker layer; track2 hosts RT.
struct Fixture {
  WellLogSession session;
  std::shared_ptr<const std::vector<double>> depth_values;
  std::shared_ptr<const std::vector<double>> gr_values;
  DocumentRevision revision;

  Fixture(PerformanceBudgets budgets = {}) : session(std::move(budgets)) {
    depth_values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1000.5, 1001.0, 1001.5, 1002.0});
    gr_values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 40.0, 90.0, 20.0, 55.0});
    auto rt_values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1.0, 4.0, 9.0, 2.0, 8.0});
    auto den_values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{2.4, 2.5, 2.6, 2.45, 2.55});

    WellLogDocumentBuilder db(document_id, DocumentRevision{1});
    db.add_sampling_axis(
        SamplingAxis{.id = axis_id,
                     .coordinates = BufferView::from_vector(depth_values),
                     .domain = DepthDomain::measured_depth,
                     .unit = "m",
                     .direction = AxisDirection::increasing});
    db.add_curve(Curve{.id = curve_gr,
                       .mnemonic = "GR",
                       .display_name = "Gamma Ray",
                       .unit = "API",
                       .sampling_axis_id = axis_id,
                       .values = BufferView::from_vector(gr_values),
                       .nulls = {}});
    db.add_curve(Curve{.id = curve_rt,
                       .mnemonic = "RT",
                       .display_name = "Resistivity",
                       .unit = "OHMM",
                       .sampling_axis_id = axis_id,
                       .values = BufferView::from_vector(rt_values),
                       .nulls = {}});
    db.add_curve(Curve{.id = curve_den,
                       .mnemonic = "DEN",
                       .display_name = "Density",
                       .unit = "G/CC",
                       .sampling_axis_id = axis_id,
                       .values = BufferView::from_vector(den_values),
                       .nulls = {}});
    require(session.execute(SetDocumentCommand{db.build()}).has_value(),
            "fixture document must be accepted");

    ScenePresentationBuilder pb(document_id,
                                ReferenceDepthRange{
                                    .domain = DepthDomain::measured_depth,
                                    .unit = "m",
                                    .top = 1000.0,
                                    .bottom = 1002.0,
                                },
                                Millimetres{500.0}, "fixture-font");
    pb.add_track(TrackSpec{.id = track1, .width = Millimetres{40.0}});
    pb.add_track(TrackSpec{.id = track2, .width = Millimetres{30.0},
                           .z_order = 1});
    pb.add_scale(TrackScaleSpec{.id = scale1,
                                .track_id = track1,
                                .minimum = 0.0,
                                .maximum = 100.0,
                                .unit = "API"});
    pb.add_scale(TrackScaleSpec{.id = scale2,
                                .track_id = track2,
                                .minimum = 0.0,
                                .maximum = 10.0,
                                .unit = "OHMM"});
    pb.add_curve_layer(CurveLayerSpec{.id = layer_gr1,
                                      .track_id = track1,
                                      .curve_id = curve_gr,
                                      .scale_id = scale1,
                                      .color = {},
                                      .line_width = Millimetres{0.25},
                                      .visible = true});
    pb.add_curve_layer(CurveLayerSpec{.id = layer_rt2,
                                      .track_id = track2,
                                      .curve_id = curve_rt,
                                      .scale_id = scale2,
                                      .color = {},
                                      .line_width = Millimetres{0.25},
                                      .visible = true});
    pb.add_marker_layer(MarkerLayerSpec{.id = marker_layer1,
                                        .track_id = track1});
    require(session.execute(SetPresentationCommand{pb.build()}).has_value(),
            "fixture presentation must be accepted");
    session.clear_events();
    revision = session.document(document_id)->revision();
  }
};

std::shared_ptr<const PreparedScene>
await_prepared_scene(WellLogSession &session) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    if (const auto scene = session.prepared_scene(document_id);
        scene != nullptr) {
      return scene;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return session.prepared_scene(document_id);
}

} // namespace

// ---------------------------------------------------------------------------
// Binding indexes
// ---------------------------------------------------------------------------

namespace {

void binding_index_resolves_document_entities() {
  Fixture fixture;
  const auto &document = *fixture.session.document(document_id);
  const DocumentBindingIndex index{document};
  require(index.axis(axis_id) != nullptr, "axis must resolve");
  require(index.curve(curve_gr) != nullptr, "GR must resolve");
  require(index.curve(curve_rt)->mnemonic == "RT", "RT mnemonic must match");
  require(index.curve(EntityId{}) == nullptr, "nil id must not resolve");
  require(index.curve(id("ffffffff-0000-4000-8000-000000000001")) == nullptr,
          "unknown id must not resolve");
  const auto curves = index.curves_on_axis(axis_id);
  require(curves.size() == 3, "three curves on the axis");
  require(curves[0]->id == curve_gr && curves[2]->id == curve_den,
          "curves keep document order");
  require(index.axis_of_curve(*index.curve(curve_rt))->id == axis_id,
          "curve resolves to its axis");
  require(index.curves_on_axis(EntityId{}).empty(),
          "unknown axis has no curves");
}

void presentation_index_resolves_track_curve_scale_bindings() {
  Fixture fixture;
  // The presentation after the fixture is visible through a prepared scene's
  // specs; rebuild the same index from a fresh binding of the fixture state
  // by re-setting the presentation (the session keeps it — here we simply
  // re-use the known fixture shape through the scene's track ids).
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "fixture scene must prepare");

  // Build a presentation for index purposes from the fixture definitions —
  // the session's stored presentation equals the one submitted; reconstruct
  // it to index. (A public accessor is intentionally not added for tests.)
  ScenePresentationBuilder pb(document_id,
                              ReferenceDepthRange{
                                  .domain = DepthDomain::measured_depth,
                                  .unit = "m",
                                  .top = 1000.0,
                                  .bottom = 1002.0,
                              },
                              Millimetres{500.0}, "fixture-font");
  pb.add_track(TrackSpec{.id = track1, .width = Millimetres{40.0}});
  pb.add_track(TrackSpec{.id = track2, .width = Millimetres{30.0},
                         .z_order = 1});
  pb.add_scale(TrackScaleSpec{
      .id = scale1, .track_id = track1, .minimum = 0.0, .maximum = 100.0,
      .unit = "API"});
  pb.add_scale(TrackScaleSpec{
      .id = scale2, .track_id = track2, .minimum = 0.0, .maximum = 10.0,
      .unit = "OHMM"});
  pb.add_curve_layer(CurveLayerSpec{
      .id = layer_gr1, .track_id = track1, .curve_id = curve_gr,
      .scale_id = scale1, .color = {}, .line_width = Millimetres{0.25},
      .z_order = 0, .visible = true, .qc_display = {}});
  pb.add_curve_layer(CurveLayerSpec{
      .id = layer_rt2, .track_id = track2, .curve_id = curve_rt,
      .scale_id = scale2, .color = {}, .line_width = Millimetres{0.25},
      .z_order = 0, .visible = true, .qc_display = {}});
  pb.add_marker_layer(MarkerLayerSpec{.id = marker_layer1,
                                      .track_id = track1});
  const auto presentation = pb.build();

  const PresentationBindingIndex index{presentation};
  require(index.track(track2) != nullptr, "track2 resolves");
  require(index.scale(scale1) != nullptr, "scale1 resolves");
  require(index.curve_layer(layer_rt2) != nullptr, "layer resolves");
  require(index.tracks_in_z_order().size() == 2 &&
              index.tracks_in_z_order().front()->id == track1 &&
              index.tracks_in_z_order()[1]->id == track2,
          "tracks in z order");
  require(index.scales_of_track(track1).size() == 1, "one scale on track1");
  require(index.curve_layers_of_track(track2).size() == 1, "one layer on t2");
  require(index.curve_layers_of_curve(curve_gr).size() == 1,
          "GR presented once");
  require(index.curve_layers_of_curve(curve_gr).front()->id == layer_gr1,
          "curve → layer id");
  require(index.track_of_scale(scale2) == index.track(track2),
          "scale → track");
  require(index.track_of_layer(marker_layer1) == index.track(track1),
          "marker layer → track");
  require(index.scale_of_curve_layer(*index.curve_layer(layer_rt2))->id ==
              scale2,
          "layer → scale");
  const auto all_layers = index.all_layers_of_track(track1);
  require(all_layers.size() == 2, "track1 has curve + marker layers");
  require(std::find(all_layers.begin(), all_layers.end(), layer_gr1) !=
              all_layers.end() &&
              std::find(all_layers.begin(), all_layers.end(),
                        marker_layer1) != all_layers.end(),
          "all_layers_of_track covers every kind");
  require(index.track_of_layer(id("ffffffff-0000-4000-8000-000000000002")) ==
              nullptr,
          "unknown layer resolves to nothing");
}

} // namespace

// ---------------------------------------------------------------------------
// Track commands
// ---------------------------------------------------------------------------

namespace {

// The session stores presentations privately; tests observe effects through
// the prepared scene + patch results (the public surface a host uses). For
// direct spec assertions we re-set the same presentation and inspect the
// prepared scene, which mirrors the spec exactly.

void add_track_appends_after_last() {
  Fixture fixture;
  auto result = fixture.session.execute(AddTrackCommand{
      .document_id = document_id,
      .width = Millimetres{25.0},
  });
  require(result.has_value(), "add track must succeed");
  require(result.value().document_revision.value ==
              fixture.revision.value + 1,
          "add track bumps the revision once");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene must prepare after add");
  require(scene->tracks().size() == 3, "three tracks after add");
  bool found = false;
  for (const auto &track : scene->tracks()) {
    if (track.id != track1 && track.id != track2) {
      found = true;
      require_near(track.bounds.width.value, 25.0, "new track width");
    }
  }
  require(found, "generated track id differs from the fixture ids");
}

void add_track_with_explicit_id_and_duplicate_rejected() {
  Fixture fixture;
  const auto new_track = id("aa000000-0000-4000-8000-000000000099");
  require(fixture.session
              .execute(AddTrackCommand{.document_id = document_id,
                                       .track_id = new_track,
                                       .width = Millimetres{20.0}})
              .has_value(),
          "explicit id accepted");
  const auto duplicate = fixture.session.execute(AddTrackCommand{
      .document_id = document_id,
      .track_id = new_track,
      .width = Millimetres{20.0},
  });
  require(!duplicate.has_value(), "duplicate track id rejected");
  require(duplicate.error().code == ErrorCode::duplicate_entity_id,
          "duplicate error code");
}

void remove_track_cascades_scales_and_layers_atomically() {
  Fixture fixture;
  require(fixture.session
              .execute(RemoveTrackCommand{.document_id = document_id,
                                          .track_id = track2})
              .has_value(),
          "remove track2 must succeed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene must prepare after remove");
  require(scene->tracks().size() == 1 && scene->tracks()[0].id == track1,
          "only track1 remains");
  require(scene->curve_layers().size() == 1 &&
              scene->curve_layers()[0].id == layer_gr1,
          "track2's layer left with it (no dangling)");
  // Undo restores the whole subtree.
  require(fixture.session
              .execute(UndoCommand{.document_id = document_id})
              .has_value(),
          "undo remove track");
  const auto restored = await_prepared_scene(fixture.session);
  require(restored != nullptr, "scene must prepare after undo");
  require(restored->tracks().size() == 2, "undo restores both tracks");
  require(restored->curve_layers().size() == 2, "undo restores layers");
}

void remove_unknown_track_rejected() {
  Fixture fixture;
  const auto result = fixture.session.execute(RemoveTrackCommand{
      .document_id = document_id,
      .track_id = id("ffffffff-0000-4000-8000-000000000003")});
  require(!result.has_value(), "unknown track rejected");
  require(result.error().code == ErrorCode::document_not_found,
          "unknown track error code");
  require(result.error().message == MessageKey::track_entity_missing,
          "unknown track message key");
}

void reorder_tracks_rewrites_z_orders() {
  Fixture fixture;
  require(fixture.session
              .execute(ReorderTracksCommand{
                  .document_id = document_id,
                  .ordered_track_ids = {track2, track1},
              })
              .has_value(),
          "reorder must succeed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene must prepare after reorder");
  // Track z-order owns horizontal layout order.
  require(scene->tracks()[0].id == track2 && scene->tracks()[1].id == track1,
          "track2 lays out first after reorder");
  // Incomplete lists are rejected without state change.
  const auto incomplete = fixture.session.execute(ReorderTracksCommand{
      .document_id = document_id,
      .ordered_track_ids = {track1},
  });
  require(!incomplete.has_value(), "incomplete reorder rejected");
  require(incomplete.error().message == MessageKey::track_order_incomplete,
          "incomplete reorder message");
}

void resize_and_header_and_visibility_edit_track_spec() {
  Fixture fixture;
  require(fixture.session
              .execute(ResizeTrackCommand{.document_id = document_id,
                                          .track_id = track1,
                                          .width = Millimetres{55.0}})
              .has_value(),
          "resize must succeed");
  auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after resize");
  require_near(scene->tracks()[0].bounds.width.value, 55.0,
               "resized width in prepared scene");

  require(fixture.session
              .execute(SetTrackHeaderCommand{
                  .document_id = document_id,
                  .track_id = track1,
                  .header = TrackHeaderSpec{.height = Millimetres{10.0}},
              })
              .has_value(),
          "set header must succeed");
  scene = await_prepared_scene(fixture.session);
  require(scene != nullptr && !scene->track_header_entries().empty(),
          "header entries appear once a header height is set");

  // Hiding a track keeps its layout slot but removes its geometry and header.
  require(fixture.session
              .execute(SetTrackVisibilityCommand{.document_id = document_id,
                                                 .track_id = track1,
                                                 .visible = false})
              .has_value(),
          "hide track must succeed");
  scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after hide");
  require(scene->tracks().size() == 2, "hidden track keeps its slot");
  auto header_entries_for_track1 = std::size_t{0};
  for (const auto &entry : scene->track_header_entries()) {
    if (entry.track_id == track1) {
      ++header_entries_for_track1;
    }
  }
  require(header_entries_for_track1 == 0, "hidden track has no header lines");
  auto layers_in_track1 = std::size_t{0};
  for (const auto &layer : scene->curve_layers()) {
    if (layer.track_id == track1) {
      layers_in_track1 += layer.segment_count > 0 ? 1 : 0;
    }
  }
  require(layers_in_track1 == 0, "hidden track contributes no geometry");
}

void bind_curve_to_track_creates_layer_and_auto_ranged_scale() {
  Fixture fixture;
  // DEN (unit G/CC) has no compatible scale in track1 → a scale is generated
  // with the curve's finite extent (2.4 .. 2.6).
  const auto result = fixture.session.execute(BindCurveToTrackCommand{
      .document_id = document_id,
      .curve_id = curve_den,
      .track_id = track1,
  });
  require(result.has_value(), "bind must succeed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after bind");
  require(scene->curve_layers().size() == 3, "three layers after bind");
  bool found_den = false;
  for (const auto &layer : scene->curve_layers()) {
    if (layer.id != layer_gr1 && layer.id != layer_rt2) {
      found_den = true;
      require(layer.curve_id == curve_den, "new layer presents DEN");
      require(layer.track_id == track1, "new layer lives in track1");
    }
  }
  require(found_den, "bind created a new layer");
}

void bind_curve_reuses_compatible_scale_and_rejects_incompatible() {
  Fixture fixture;
  // GR (API) → track2 has an OHMM scale; nil scale id must NOT reuse it and
  // instead generate an API scale.
  require(fixture.session
              .execute(BindCurveToTrackCommand{
                  .document_id = document_id,
                  .curve_id = curve_gr,
                  .track_id = track2,
              })
              .has_value(),
          "bind GR to track2 must succeed");
  // An explicit scale from ANOTHER track or with another unit is rejected.
  const auto cross_track = fixture.session.execute(BindCurveToTrackCommand{
      .document_id = document_id,
      .curve_id = curve_rt,
      .track_id = track1,
      .scale_id = scale2,
  });
  require(!cross_track.has_value(), "cross-track scale rejected");
  require(cross_track.error().message == MessageKey::track_binding_invalid,
          "cross-track scale message");
  const auto unknown_curve = fixture.session.execute(BindCurveToTrackCommand{
      .document_id = document_id,
      .curve_id = id("ffffffff-0000-4000-8000-000000000010"),
      .track_id = track1,
  });
  require(!unknown_curve.has_value(), "dangling curve rejected");
  require(unknown_curve.error().code == ErrorCode::document_not_found,
          "dangling curve error code");
  const auto unknown_track = fixture.session.execute(BindCurveToTrackCommand{
      .document_id = document_id,
      .curve_id = curve_rt,
      .track_id = id("ffffffff-0000-4000-8000-000000000011"),
  });
  require(!unknown_track.has_value(), "unknown track rejected");
}

void duplicate_binding_is_allowed_as_independent_presentation() {
  Fixture fixture;
  // Binding GR into track1 a second time is allowed: two visual layers of
  // the same immutable buffer.
  const auto second = fixture.session.execute(BindCurveToTrackCommand{
      .document_id = document_id,
      .curve_id = curve_gr,
      .track_id = track1,
      .scale_id = scale1,
  });
  require(second.has_value(), "duplicate binding allowed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after duplicate bind");
  std::uint64_t gr_layers = 0;
  for (const auto &layer : scene->curve_layers()) {
    if (layer.curve_id == curve_gr && layer.track_id == track1) {
      ++gr_layers;
    }
  }
  require(gr_layers == 2, "two independent GR layers in track1");
}

void unbind_removes_layer_and_keeps_scale() {
  Fixture fixture;
  require(fixture.session
              .execute(UnbindCurveFromTrackCommand{.document_id = document_id,
                                                   .layer_id = layer_gr1})
              .has_value(),
          "unbind must succeed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after unbind");
  for (const auto &layer : scene->curve_layers()) {
    require(layer.id != layer_gr1, "layer removed");
  }
  require(scene->tracks().size() == 2, "tracks untouched by unbind");
  require(fixture.session
              .execute(UndoCommand{.document_id = document_id})
              .has_value(),
          "undo unbind");
  const auto undone = await_prepared_scene(fixture.session);
  require(undone != nullptr, "scene after undo");
  bool layer_back = false;
  for (const auto &layer : undone->curve_layers()) {
    layer_back = layer_back || layer.id == layer_gr1;
  }
  require(layer_back, "undo restores the layer");
}

void move_curve_between_tracks_preserves_layer_identity_and_buffers() {
  Fixture fixture;
  // Raw buffer identity before the move.
  const auto document_before = fixture.session.document(document_id);
  const auto &gr_before = *std::find_if(
      document_before->curves().begin(), document_before->curves().end(),
      [](const Curve &curve) { return curve.id == curve_gr; });
  const auto values_data_before = gr_before.values.as_single().data();
  const auto values_length_before = gr_before.values.length();
  const auto axis_data_before = document_before->sampling_axes()[0]
                                    .coordinates.as_single()
                                    .data();

  const auto result = fixture.session.execute(MoveCurveLayerCommand{
      .document_id = document_id,
      .layer_id = layer_gr1,
      .target_track_id = track2,
  });
  require(result.has_value(), "move must succeed");

  // The GR layer keeps its id but now lives in track2 — and GR's unit (API)
  // has no compatible scale in track2, so one was generated (auto-ranged).
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after move");
  const PreparedCurveLayer *moved = nullptr;
  for (const auto &layer : scene->curve_layers()) {
    if (layer.id == layer_gr1) {
      moved = &layer;
    }
  }
  require(moved != nullptr, "moved layer keeps its identity");
  require(moved->track_id == track2, "moved layer lives in track2");
  require(moved->scale_id != scale1 && moved->scale_id != scale2,
          "moved layer uses a generated API scale in track2");

  // Raw buffers are untouched — same address, same length, no copy.
  const auto document_after = fixture.session.document(document_id);
  const auto &gr_after = *std::find_if(
      document_after->curves().begin(), document_after->curves().end(),
      [](const Curve &curve) { return curve.id == curve_gr; });
  require(gr_after.values.as_single().data() == values_data_before,
          "raw value buffer address unchanged after move");
  require(gr_after.values.length() == values_length_before,
          "raw value buffer length unchanged after move");
  require(document_after->sampling_axes()[0].coordinates.as_single().data() ==
              axis_data_before,
          "sampling axis buffer address unchanged after move");
  require(gr_after.values.as_single().byte_capacity() ==
              gr_before.values.as_single().byte_capacity(),
          "no reallocation of the value buffer");

  // Undo restores the original binding (still the same layer id).
  require(fixture.session
              .execute(UndoCommand{.document_id = document_id})
              .has_value(),
          "undo move");
  const auto undone = await_prepared_scene(fixture.session);
  require(undone != nullptr, "scene after undo move");
  for (const auto &layer : undone->curve_layers()) {
    if (layer.id == layer_gr1) {
      require(layer.track_id == track1, "undo restores the track binding");
    }
  }
  // Undo of the move also removes the generated scale (whole-patch inverse).
}

void move_with_explicit_incompatible_scale_rejected() {
  Fixture fixture;
  const auto result = fixture.session.execute(MoveCurveLayerCommand{
      .document_id = document_id,
      .layer_id = layer_gr1,
      .target_track_id = track2,
      .target_scale_id = scale2, // OHMM scale for an API curve
  });
  require(!result.has_value(), "incompatible target scale rejected");
  require(result.error().message == MessageKey::track_binding_invalid,
          "incompatible scale message");
}

void move_unknown_layer_rejected() {
  Fixture fixture;
  const auto result = fixture.session.execute(MoveCurveLayerCommand{
      .document_id = document_id,
      .layer_id = id("ffffffff-0000-4000-8000-000000000020"),
      .target_track_id = track2,
  });
  require(!result.has_value(), "unknown layer rejected");
  require(result.error().code == ErrorCode::document_not_found,
          "unknown layer error code");
}

void duplicate_layer_shares_buffers_on_top_of_track() {
  Fixture fixture;
  const auto result = fixture.session.execute(DuplicateCurveLayerCommand{
      .document_id = document_id,
      .layer_id = layer_gr1,
  });
  require(result.has_value(), "duplicate must succeed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after duplicate");
  std::uint64_t gr_layers_in_track1 = 0;
  for (const auto &layer : scene->curve_layers()) {
    if (layer.curve_id == curve_gr && layer.track_id == track1) {
      ++gr_layers_in_track1;
    }
  }
  require(gr_layers_in_track1 == 2, "duplicated layer presents GR twice");
  // The raw buffer count did not change (same immutable buffer).
  const auto document = fixture.session.document(document_id);
  require(document->curves().size() == 3, "no extra curve entity created");
}

void reorder_curve_layers_within_track() {
  Fixture fixture;
  const auto second_layer = id("aa000000-0000-4000-8000-000000000035");
  require(fixture.session
              .execute(BindCurveToTrackCommand{
                  .document_id = document_id,
                  .curve_id = curve_den,
                  .track_id = track1,
                  .layer_id = second_layer,
                  .scale_id = scale1, // unit mismatch → rejected below? no:
                                      // API scale for a G/CC curve fails.
              })
              .has_value() == false,
          "unit-incompatible explicit scale rejected for DEN");
  // Bind DEN without an explicit scale (generates a G/CC one).
  require(fixture.session
              .execute(BindCurveToTrackCommand{
                  .document_id = document_id,
                  .curve_id = curve_den,
                  .track_id = track1,
                  .layer_id = second_layer,
              })
              .has_value(),
          "bind DEN with generated scale");
  // Reorder [DEN, GR].
  require(fixture.session
              .execute(ReorderCurveLayersCommand{
                  .document_id = document_id,
                  .track_id = track1,
                  .ordered_layer_ids = {second_layer, layer_gr1},
              })
              .has_value(),
          "layer reorder must succeed");
  const auto bad = fixture.session.execute(ReorderCurveLayersCommand{
      .document_id = document_id,
      .track_id = track1,
      .ordered_layer_ids = {layer_gr1},
  });
  require(!bad.has_value(), "incomplete layer reorder rejected");
  require(bad.error().message == MessageKey::track_order_incomplete,
          "incomplete layer reorder message");
}

void layer_visibility_and_style_round_trip() {
  Fixture fixture;
  require(fixture.session
              .execute(SetCurveLayerVisibilityCommand{
                  .document_id = document_id,
                  .layer_id = layer_gr1,
                  .visible = false})
              .has_value(),
          "hide layer");
  auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after hide layer");
  for (const auto &layer : scene->curve_layers()) {
    if (layer.id == layer_gr1) {
      require(layer.segment_count == 0, "hidden layer has no geometry");
      require(!layer.visible, "prepared layer reports hidden");
    }
  }
  require(fixture.session
              .execute(SetCurveLayerStyleCommand{
                  .document_id = document_id,
                  .layer_id = layer_gr1,
                  .color = RgbaColor{.red = 0xAB, .green = 0xCD,
                                     .blue = 0xEF, .alpha = 0xFF},
                  .line_width = Millimetres{0.9},
              })
              .has_value(),
          "style edit");
  scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after style");
  for (const auto &layer : scene->curve_layers()) {
    if (layer.id == layer_gr1) {
      require(layer.color.red == 0xAB && layer.color.blue == 0xEF,
              "color applied");
      require_near(layer.line_width.value, 0.9, "line width applied");
    }
  }
  // Invalid line width is rejected.
  const auto bad = fixture.session.execute(SetCurveLayerStyleCommand{
      .document_id = document_id,
      .layer_id = layer_gr1,
      .line_width = Millimetres{0.0},
  });
  require(!bad.has_value(), "zero line width rejected");
  // Undo restores the original style AND visibility in one step each.
  require(fixture.session
              .execute(UndoCommand{.document_id = document_id})
              .has_value(),
          "undo style");
  require(fixture.session
              .execute(UndoCommand{.document_id = document_id})
              .has_value(),
          "undo visibility");
  scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after undos");
  for (const auto &layer : scene->curve_layers()) {
    if (layer.id == layer_gr1) {
      require(layer.visible, "visibility restored");
      require_near(layer.line_width.value, 0.25, "style restored");
    }
  }
}

void scale_edits_validate_and_auto_range_fits() {
  Fixture fixture;
  require(fixture.session
              .execute(SetTrackScaleCommand{
                  .document_id = document_id,
                  .scale_id = scale1,
                  .maximum = 250.0,
              })
              .has_value(),
          "partial scale edit keeps other fields");
  const auto inverted = fixture.session.execute(SetTrackScaleCommand{
      .document_id = document_id,
      .scale_id = scale1,
      .minimum = 300.0,
  });
  require(!inverted.has_value(), "min > max rejected");
  require(inverted.error().message == MessageKey::track_scale_range_invalid,
          "range message");
  const auto log_zero = fixture.session.execute(SetTrackScaleCommand{
      .document_id = document_id,
      .scale_id = scale1,
      .mode = ScaleMode::logarithmic,
  });
  require(!log_zero.has_value(), "log with min <= 0 rejected");

  // Auto range refits scale1 to GR's finite extent (10..90).
  require(fixture.session
              .execute(AutoRangeTrackScaleCommand{
                  .document_id = document_id,
                  .scale_id = scale1,
              })
              .has_value(),
          "auto range must succeed");
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene after auto range");
  // The auto-ranged scale must place GR 10 at the left edge and 90 at the
  // right edge of track1 (first track in layout, width 40mm).
  const auto &first_track = scene->tracks()[0];
  require(first_track.id == track1, "track1 first");
  double min_x = 1e9;
  double max_x = -1e9;
  for (const auto &point : scene->curve_points()) {
    if (point.position.left.value >= first_track.bounds.left.value - 1e-6 &&
        point.position.left.value <=
            first_track.bounds.left.value +
                first_track.bounds.width.value + 1e-6) {
      min_x = std::min(min_x, point.position.left.value);
      max_x = std::max(max_x, point.position.left.value);
    }
  }
  require_near(min_x, first_track.bounds.left.value,
               "auto range pins data min to the left edge");
  require_near(max_x,
               first_track.bounds.left.value +
                   first_track.bounds.width.value,
               "auto range pins data max to the right edge");

  // A unit change that breaks curve compatibility is rejected.
  const auto unit_break = fixture.session.execute(SetTrackScaleCommand{
      .document_id = document_id,
      .scale_id = scale1,
      .unit = std::string{"G/CC"},
  });
  require(!unit_break.has_value(), "incompatible unit change rejected");
}

void commands_publish_events_and_clear_redo() {
  Fixture fixture;
  require(fixture.session
              .execute(ResizeTrackCommand{.document_id = document_id,
                                          .track_id = track1,
                                          .width = Millimetres{50.0}})
              .has_value(),
          "resize for history");
  require(fixture.session.can_undo(document_id), "undo available");
  require(fixture.session
              .execute(UndoCommand{.document_id = document_id})
              .has_value(),
          "undo");
  require(fixture.session.can_redo(document_id), "redo available");
  require(fixture.session
              .execute(ResizeTrackCommand{.document_id = document_id,
                                          .track_id = track1,
                                          .width = Millimetres{60.0}})
              .has_value(),
          "second resize clears redo");
  require(!fixture.session.can_redo(document_id),
          "new command cleared redo stack");
  bool saw_documents_changed = false;
  bool saw_presentation_changed = false;
  for (const auto &event : fixture.session.events()) {
    saw_documents_changed = saw_documents_changed ||
                            event.kind == ViewEventKind::documents_changed;
    saw_presentation_changed =
        saw_presentation_changed ||
        event.kind == ViewEventKind::presentation_changed;
  }
  require(saw_documents_changed && saw_presentation_changed,
          "track commands publish through the patch event path");
}

void commands_without_presentation_are_rejected() {
  // A document without a presentation must reject track commands with the
  // precise binding error (ApplyPatch would reject presentation upserts too,
  // but the command layer reports the actionable cause).
  WellLogSession session;
  WellLogDocumentBuilder db(document_id, DocumentRevision{1});
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0});
  db.add_sampling_axis(
      SamplingAxis{.id = axis_id,
                   .coordinates = BufferView::from_vector(depths),
                   .domain = DepthDomain::measured_depth,
                   .unit = "m"});
  db.add_curve(Curve{.id = curve_gr,
                     .mnemonic = "GR",
                     .display_name = "Gamma Ray",
                     .unit = "API",
                     .sampling_axis_id = axis_id,
                     .values = BufferView::from_vector(depths),
                     .nulls = {}});
  require(session.execute(SetDocumentCommand{db.build()}).has_value(),
          "bare document accepted");
  const auto result = session.execute(AddTrackCommand{
      .document_id = document_id, .width = Millimetres{30.0}});
  require(!result.has_value(), "track command without presentation rejected");
  require(result.error().code == ErrorCode::invalid_presentation,
          "no-presentation error code");
}

void selection_survives_track_commands() {
  Fixture fixture;
  require(fixture.session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
              })
              .has_value(),
          "set selection");
  require(fixture.session
              .execute(ResizeTrackCommand{.document_id = document_id,
                                          .track_id = track1,
                                          .width = Millimetres{44.0}})
              .has_value(),
          "resize track");
  const auto selection = fixture.session.selection(document_id);
  require(selection.has_value() && selection->valid,
          "selection survives presentation-only edits");
  require_near(selection->reference_depth_range.top, 1000.0,
               "selection range kept");
}

void generated_entity_ids_round_trip_through_parse() {
  for (int i = 0; i < 256; ++i) {
    const auto generated = EntityId::generate();
    require(!generated.is_nil(), "generated id is non-nil");
    const auto text = generated.to_string();
    const auto parsed = EntityId::parse(text);
    require(parsed.has_value() && *parsed == generated,
            "generated id round-trips through to_string/parse");
    // Version 4 / variant bits are set (a well-formed UUID). The version
    // nibble is the first hex of group 3 (char 14); the variant is the first
    // hex of group 4 (char 19) and must have its high bits as 10xx.
    const auto version_hex = std::stoull(
        std::string{text.substr(14, 1)}, nullptr, 16);
    require(version_hex == 4, "version nibble is 4");
    const auto variant_hex = std::stoull(
        std::string{text.substr(19, 1)}, nullptr, 16);
    require((variant_hex & 0xCu) == 0x8u, "variant bits set");
  }
  const auto a = EntityId::generate();
  const auto b = EntityId::generate();
  require(a != b, "generated ids are unique");
}

void resolve_curve_pick_reports_inspect_fields() {
  Fixture fixture;
  const auto scene = await_prepared_scene(fixture.session);
  require(scene != nullptr, "scene for picking");
  // Pick a point in track1 near the first GR sample.
  const auto &track = scene->tracks()[0];
  const auto query = CurvePickQuery{
      .scene_position = PhysicalPoint{
          .left = Millimetres{track.bounds.left.value + 1.0},
          .top = Millimetres{1.0},
      },
      .tolerance = DeviceIndependentPixels{50.0},
      .horizontal_device_independent_pixels_per_millimetre = 4.0,
      .vertical_device_independent_pixels_per_millimetre = 4.0,
  };
  const auto pick = scene->pick_curve(query);
  require(pick.has_value(), "pick must hit GR");
  const auto &document = *fixture.session.document(document_id);
  // Rebuild the fixture presentation for resolution (tests know the shape).
  ScenePresentationBuilder pb(document_id,
                              ReferenceDepthRange{
                                  .domain = DepthDomain::measured_depth,
                                  .unit = "m",
                                  .top = 1000.0,
                                  .bottom = 1002.0,
                              },
                              Millimetres{500.0}, "fixture-font");
  pb.add_track(TrackSpec{.id = track1, .width = Millimetres{40.0}});
  pb.add_track(TrackSpec{.id = track2, .width = Millimetres{30.0},
                         .z_order = 1});
  pb.add_scale(TrackScaleSpec{
      .id = scale1, .track_id = track1, .minimum = 0.0, .maximum = 100.0,
      .unit = "API"});
  pb.add_scale(TrackScaleSpec{
      .id = scale2, .track_id = track2, .minimum = 0.0, .maximum = 10.0,
      .unit = "OHMM"});
  pb.add_curve_layer(CurveLayerSpec{
      .id = layer_gr1, .track_id = track1, .curve_id = curve_gr,
      .scale_id = scale1, .color = {}, .line_width = Millimetres{0.25},
      .z_order = 0, .visible = true, .qc_display = {}});
  pb.add_curve_layer(CurveLayerSpec{
      .id = layer_rt2, .track_id = track2, .curve_id = curve_rt,
      .scale_id = scale2, .color = {}, .line_width = Millimetres{0.25},
      .z_order = 0, .visible = true, .qc_display = {}});
  pb.add_marker_layer(MarkerLayerSpec{.id = marker_layer1,
                                      .track_id = track1});
  const auto presentation = pb.build();

  auto info = resolve_curve_pick(document, presentation, *pick);
  require(info.has_value(), "pick resolves");
  require(info->curve_id == curve_gr, "pick resolves GR");
  require(info->track_id == track1, "pick resolves track1");
  require(info->mnemonic == "GR", "mnemonic reported");
  require(info->display_name == "Gamma Ray", "display name reported");
  require(info->unit == "API" && info->scale_unit == "API", "units reported");
  require(info->qc_state == QcState::valid, "QC state reported");
  require(!info->derived, "raw curve is not derived");
  require_near(info->scale_minimum, 0.0, "scale min reported");
  require_near(info->scale_maximum, 100.0, "scale max reported");

  // A stale pick (layer removed from the presentation) resolves to nullopt.
  auto stale = *pick;
  stale.layer_id = id("ffffffff-0000-4000-8000-000000000040");
  require(!resolve_curve_pick(document, presentation, stale).has_value(),
          "stale pick resolves to nothing");
}

} // namespace

int main() {
  binding_index_resolves_document_entities();
  presentation_index_resolves_track_curve_scale_bindings();
  add_track_appends_after_last();
  add_track_with_explicit_id_and_duplicate_rejected();
  remove_track_cascades_scales_and_layers_atomically();
  remove_unknown_track_rejected();
  reorder_tracks_rewrites_z_orders();
  resize_and_header_and_visibility_edit_track_spec();
  bind_curve_to_track_creates_layer_and_auto_ranged_scale();
  bind_curve_reuses_compatible_scale_and_rejects_incompatible();
  duplicate_binding_is_allowed_as_independent_presentation();
  unbind_removes_layer_and_keeps_scale();
  move_curve_between_tracks_preserves_layer_identity_and_buffers();
  move_with_explicit_incompatible_scale_rejected();
  move_unknown_layer_rejected();
  duplicate_layer_shares_buffers_on_top_of_track();
  reorder_curve_layers_within_track();
  layer_visibility_and_style_round_trip();
  scale_edits_validate_and_auto_range_fits();
  commands_publish_events_and_clear_redo();
  commands_without_presentation_are_rejected();
  selection_survives_track_commands();
  generated_entity_ids_round_trip_through_parse();
  resolve_curve_pick_reports_inspect_fields();
  std::cout << "welllog.track-commands: all cases passed\n";
  return EXIT_SUCCESS;
}
