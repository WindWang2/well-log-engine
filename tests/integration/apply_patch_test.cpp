// Headless test for ApplyPatchCommand (#202, the #158 foundation, ADR 0025).
// Asserts: upsert + remove of document interpretation entities
// (Interval/Marker/ Annotation) and presentation layout entities
// (Track/Scale/CurveLayer) produce a new Document Revision readable end-to-end;
// the patch is atomic (one bad edit rejects the whole batch, document
// unchanged); a base-revision mismatch is rejected with patch_conflict (no
// guessing); the Selection Set remaps or invalidates per ADR 0024. No GL/Qt —
// WellLogSession + core.

#include <welllog/core/document.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

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
  require(std::abs(actual - expected) < 1.0e-9, message);
}

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("cc000000-0000-4000-8000-000000000001");
const auto axis_id = id("cc000000-0000-4000-8000-000000000002");
const auto curve_id = id("cc000000-0000-4000-8000-000000000003");
const auto second_curve_id = id("cc000000-0000-4000-8000-000000000013");
const auto track_id = id("cc000000-0000-4000-8000-000000000004");
const auto scale_id = id("cc000000-0000-4000-8000-000000000005");
const auto layer_id = id("cc000000-0000-4000-8000-000000000006");
const auto second_track_id = id("cc000000-0000-4000-8000-000000000010");
const auto second_scale_id = id("cc000000-0000-4000-8000-000000000011");
const auto second_layer_id = id("cc000000-0000-4000-8000-000000000012");
const auto interval_layer_id = id("cc000000-0000-4000-8000-000000000014");
const auto interval_id = id("cc000000-0000-4000-8000-000000000007");
const auto marker_id = id("cc000000-0000-4000-8000-000000000008");
const auto annotation_id = id("cc000000-0000-4000-8000-000000000009");

// Fixture: a document with one axis/curve + one Interval/Marker/Annotation,
// and a presentation with one Track/Scale/CurveLayer. Sets a viewport so the
// selection-remap and viewport-preservation paths are exercisable.
struct Fixture {
  WellLogSession session;
  DocumentRevision revision;

  explicit Fixture(PerformanceBudgets budgets = {}) : session(std::move(budgets)) {
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1001.0, 1002.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 20.0, 30.0});
    auto second_values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 100.0, 1000.0});
    WellLogDocumentBuilder db(document_id, DocumentRevision{1});
    db.add_sampling_axis(
        SamplingAxis{.id = axis_id,
                     .coordinates = BufferView::from_vector(depths),
                     .domain = DepthDomain::measured_depth,
                     .unit = "m",
                     .direction = AxisDirection::increasing});
    db.add_curve(Curve{.id = curve_id,
                       .mnemonic = "GR",
                       .display_name = "Gamma Ray",
                       .unit = "API",
                       .sampling_axis_id = axis_id,
                       .values = BufferView::from_vector(values),
                       .nulls = {}});
    db.add_curve(Curve{
        .id = second_curve_id,
        .mnemonic = "CPS",
        .display_name = "Counts",
        .unit = "CPS",
        .sampling_axis_id = axis_id,
        .values = BufferView::from_vector(second_values),
        .nulls = {},
    });
    db.add_interval(Interval{.id = interval_id,
                             .top_reference_depth = 1000.0,
                             .bottom_reference_depth = 1001.0,
                             .semantic = IntervalSemantic::lithology,
                             .pattern_id = {},
                             .label = "Sand"});
    db.add_marker(Marker{.id = marker_id,
                         .reference_depth = 1000.5,
                         .semantic = MarkerSemantic::formation_top,
                         .label = "Top A"});
    TextAnnotation annotation;
    annotation.id = annotation_id;
    annotation.reference_depth = 1001.0;
    annotation.text = "Note";
    db.add_annotation(annotation);
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
    pb.add_track(TrackSpec{
        .id = track_id,
        .width = Millimetres{40.0},
        .header = TrackHeaderSpec{.height = Millimetres{8.0}},
    });
    pb.add_track(TrackSpec{
        .id = second_track_id, .width = Millimetres{30.0}, .z_order = 1});
    pb.add_scale(TrackScaleSpec{.id = scale_id,
                                .track_id = track_id,
                                .mode = ScaleMode::linear,
                                .minimum = 0.0,
                                .maximum = 100.0,
                                .unit = "API"});
    pb.add_scale(TrackScaleSpec{.id = second_scale_id,
                                .track_id = second_track_id,
                                .mode = ScaleMode::linear,
                                .minimum = 0.0,
                                .maximum = 100.0,
                                .unit = "API"});
    pb.add_curve_layer(CurveLayerSpec{.id = layer_id,
                                      .track_id = track_id,
                                      .curve_id = curve_id,
                                      .scale_id = scale_id,
                                      .color = {},
                                      .line_width = Millimetres{0.25},
                                      .visible = true});
    pb.add_curve_layer(CurveLayerSpec{.id = second_layer_id,
                                      .track_id = second_track_id,
                                      .curve_id = curve_id,
                                      .scale_id = second_scale_id,
                                      .color = {},
                                      .line_width = Millimetres{0.25},
                                      .visible = true});
    pb.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id,
        .track_id = track_id,
        .draw_labels = false,
    });
    require(session.execute(SetPresentationCommand{pb.build()}).has_value(),
            "fixture presentation must be accepted");
    // Establish a viewport (presentation set the initial one; adjust it).
    require(session
                .execute(SetViewportCommand{
                    .document_id = document_id,
                    .viewport = {.top = 1000.0, .bottom = 1001.5},
                })
                .has_value(),
            "fixture viewport must be accepted");
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

// Upserting an existing document entity (modify) replaces it by id and produces
// a new revision.
void upsert_replaces_document_entity() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{UpsertEntity{
                          Interval{.id = interval_id,
                                   .top_reference_depth = 1000.0,
                                   .bottom_reference_depth = 1001.5,
                                   .semantic = IntervalSemantic::lithology,
                                   .pattern_id = {},
                                   .label = "Shale"}}},
                  },
          },
  });
  require(result.has_value(), "upsert patch must succeed");
  require(result.value().document_revision.value == f.revision.value + 1,
          "patch must produce the next revision");
  const auto doc = f.session.document(document_id);
  const auto intervals = doc->intervals();
  require(intervals.size() == 1, "interval count must stay 1 (replaced)");
  require(intervals.front().label == "Shale",
          "the upserted interval must replace the old one");
  require_near(intervals.front().bottom_reference_depth, 1001.5,
               "the upserted interval must carry the new bottom depth");
}

// Upserting a NEW document entity id (create) adds it.
void upsert_creates_document_entity() {
  Fixture f;
  const auto new_marker = id("cc000000-0000-4000-8000-000000000020");
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{
                          UpsertEntity{Marker{.id = new_marker,
                                              .reference_depth = 1001.5,
                                              .semantic = MarkerSemantic::fault,
                                              .label = "Fault"}}},
                  },
          },
  });
  require(result.has_value(), "create-marker patch must succeed");
  const auto doc = f.session.document(document_id);
  require(doc->markers().size() == 2, "a new marker must be added");
  require(std::any_of(doc->markers().begin(), doc->markers().end(),
                      [new_marker](const Marker &m) {
                        return m.id == new_marker && m.label == "Fault";
                      }),
          "the created marker must be readable");
}

// Removing a document entity deletes it by id.
void remove_deletes_document_entity() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits = {EntityEdit{RemoveEntity{annotation_id}}},
          },
  });
  require(result.has_value(), "remove-annotation patch must succeed");
  const auto doc = f.session.document(document_id);
  require(doc->annotations().empty(), "the removed annotation must be gone");
}

// A Track width patch changes the prepared geometry, rather than only the
// session's retained presentation value.
void upsert_edits_presentation_entity() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{UpsertEntity{TrackSpec{
                          .id = track_id, .width = Millimetres{80.0}}}},
                  },
          },
  });
  require(result.has_value(), "presentation patch must succeed");
  // The patched presentation is restored internally; query via the prepared
  // scene's presentation is not directly exposed, so assert the viewport was
  // preserved (the patch path restores it) and the revision advanced.
  require(f.session.viewport(document_id).has_value(),
          "the viewport must be preserved across a patch");
  require_near(f.session.viewport(document_id)->bottom, 1001.5,
               "the viewport window must be unchanged by a patch");
  const auto scene = await_prepared_scene(f.session);
  require(scene != nullptr, "track width patch must re-prepare a scene");
  const auto track =
      std::find_if(scene->tracks().begin(), scene->tracks().end(),
                   [](const PreparedTrack &t) { return t.id == track_id; });
  require(track != scene->tracks().end(),
          "prepared scene must retain the patched track");
  require_near(track->bounds.width.value, 80.0,
               "prepared track geometry must use the patched width");
  require_near(scene->physical_width().value, 110.0,
               "the physical scene width must include the patched track width");
}

// A Track z_order patch is a layout edit: re-preparing must place the patched
// track first, including its geometry, rather than merely retaining z_order as
// unread metadata on an insertion-ordered scene.
void track_z_order_patch_reorders_prepared_tracks() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits = {EntityEdit{UpsertEntity{TrackSpec{
                  .id = second_track_id,
                  .width = Millimetres{30.0},
                  .z_order = -1,
              }}}},
          },
  });
  require(result.has_value(), "track z-order patch must succeed");
  const auto scene = await_prepared_scene(f.session);
  require(scene != nullptr, "track z-order patch must re-prepare a scene");
  require(scene->tracks().size() == 2,
          "prepared scene must retain both tracks after a patch");
  require(scene->tracks().front().id == second_track_id,
          "prepared tracks must follow the patched z-order");
  require_near(scene->tracks().front().bounds.left.value, 0.0,
               "the reordered track must own the leftmost geometry");
}

// Scale mode/range/direction/unit edits must reach prepared metadata and the
// curve geometry. Changing the unit also retargets the layer to a compatible
// immutable source curve; raw curves themselves remain unedited (ADR 0025).
void scale_patch_changes_prepared_metadata_and_geometry() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{UpsertEntity{TrackScaleSpec{
                          .id = scale_id,
                          .track_id = track_id,
                          .mode = ScaleMode::logarithmic,
                          .minimum = 1.0,
                          .maximum = 1000.0,
                          .direction = ScaleDirection::right_to_left,
                          .unit = "CPS",
                      }}},
                      EntityEdit{UpsertEntity{CurveLayerSpec{
                          .id = layer_id,
                          .track_id = track_id,
                          .curve_id = second_curve_id,
                          .scale_id = scale_id,
                          .color = {},
                          .line_width = Millimetres{0.25},
                          .visible = true,
                      }}},
                  },
          },
  });
  require(result.has_value(), "scale patch must succeed");
  const auto scene = await_prepared_scene(f.session);
  require(scene != nullptr, "scale patch must re-prepare a scene");
  const auto header = std::find_if(scene->track_header_entries().begin(),
                                   scene->track_header_entries().end(),
                                   [](const PreparedTrackHeaderEntry &entry) {
                                     return entry.curve_layer_id == layer_id;
                                   });
  require(header != scene->track_header_entries().end(),
          "prepared header must retain the patched scale");
  require_near(header->scale_minimum, 1.0,
               "prepared header must carry the patched scale minimum");
  require_near(header->scale_maximum, 1000.0,
               "prepared header must carry the patched scale maximum");
  require(header->unit == "CPS", "prepared header must carry the patched unit");
  require(header->mode == ScaleMode::logarithmic,
          "prepared header must carry the patched scale mode");
  require(header->direction == ScaleDirection::right_to_left,
          "prepared header must carry the patched scale direction");
  const auto layer =
      std::find_if(scene->curve_layers().begin(), scene->curve_layers().end(),
                   [](const PreparedCurveLayer &candidate) {
                     return candidate.id == layer_id;
                   });
  require(layer != scene->curve_layers().end() && layer->segment_count == 1,
          "patched scale layer must retain one visible geometry segment");
  require(layer->first_segment < scene->curve_segments().size(),
          "prepared layer must reference an in-range segment");
  const auto &segment =
      scene->curve_segments()[static_cast<std::size_t>(layer->first_segment)];
  require(segment.first_point < scene->curve_points().size(),
          "prepared segment must reference an in-range point");
  const auto &first_point =
      scene->curve_points()[static_cast<std::size_t>(segment.first_point)];
  require(std::abs(first_point.position.left.value - (40.0 * 2.0 / 3.0)) <
              1.0e-9,
          "logarithmic right-to-left scale must remap prepared x geometry");
}

// Curve-layer style is renderer input, so color and stroke width must appear
// on the re-prepared layer rather than only in a patchable presentation entry.
void curve_layer_style_patch_changes_prepared_layer() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits = {EntityEdit{UpsertEntity{CurveLayerSpec{
                  .id = layer_id,
                  .track_id = track_id,
                  .curve_id = curve_id,
                  .scale_id = scale_id,
                  .color = RgbaColor{0x12, 0x34, 0x56, 0x78},
                  .line_width = Millimetres{1.5},
                  .visible = true,
              }}}},
          },
  });
  require(result.has_value(), "curve-layer style patch must succeed");
  const auto scene = await_prepared_scene(f.session);
  require(scene != nullptr, "style patch must re-prepare a scene");
  const auto layer =
      std::find_if(scene->curve_layers().begin(), scene->curve_layers().end(),
                   [](const PreparedCurveLayer &candidate) {
                     return candidate.id == layer_id;
                   });
  require(layer != scene->curve_layers().end(),
          "prepared scene must retain the patched curve layer");
  require(layer->color.red == 0x12 && layer->color.green == 0x34 &&
              layer->color.blue == 0x56 && layer->color.alpha == 0x78,
          "prepared layer must carry the patched RGBA style");
  require_near(layer->line_width.value, 1.5,
               "prepared layer must carry the patched line width");
}

// Hidden curve layers remain addressable for a later patch, but must emit no
// curve geometry into the prepared scene.
void curve_layer_visibility_patch_removes_prepared_geometry() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits = {EntityEdit{UpsertEntity{CurveLayerSpec{
                  .id = layer_id,
                  .track_id = track_id,
                  .curve_id = curve_id,
                  .scale_id = scale_id,
                  .color = {},
                  .line_width = Millimetres{0.25},
                  .visible = false,
              }}}},
          },
  });
  require(result.has_value(), "curve-layer visibility patch must succeed");
  const auto scene = await_prepared_scene(f.session);
  require(scene != nullptr, "visibility patch must re-prepare a scene");
  const auto layer =
      std::find_if(scene->curve_layers().begin(), scene->curve_layers().end(),
                   [](const PreparedCurveLayer &candidate) {
                     return candidate.id == layer_id;
                   });
  require(layer != scene->curve_layers().end() && !layer->visible,
          "prepared layer must retain its patched hidden state");
  require(layer->segment_count == 0,
          "hidden curve layers must contribute no prepared segments");
  require(std::none_of(scene->curve_segments().begin(),
                       scene->curve_segments().end(),
                       [](const PreparedCurveSegment &segment) {
                         return segment.layer_id == layer_id;
                       }),
          "hidden curve layers must contribute no prepared geometry");
}

// Removing a layer uses the structural-visibility path: it must be absent from
// the re-prepared scene as well as from the retained presentation.
void remove_deletes_presentation_entity() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits = {EntityEdit{RemoveEntity{layer_id}}},
          },
  });
  require(result.has_value(), "remove-layer patch must succeed");
  require(f.session.viewport(document_id).has_value(),
          "the viewport must be preserved across a presentation patch");
  const auto scene = await_prepared_scene(f.session);
  require(scene != nullptr, "remove-layer patch must re-prepare a scene");
  require(std::none_of(scene->curve_layers().begin(),
                       scene->curve_layers().end(),
                       [](const PreparedCurveLayer &layer) {
                         return layer.id == layer_id;
                       }),
          "removed curve layers must be absent from the prepared scene");
  require(std::none_of(scene->curve_segments().begin(),
                       scene->curve_segments().end(),
                       [](const PreparedCurveSegment &segment) {
                         return segment.layer_id == layer_id;
                       }),
          "removed curve layers must contribute no prepared geometry");

  const auto readd = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = f.session.document(document_id)->revision(),
          .edits = {EntityEdit{UpsertEntity{CurveLayerSpec{
              .id = layer_id,
              .track_id = track_id,
              .curve_id = curve_id,
              .scale_id = scale_id,
              .color = {},
              .line_width = Millimetres{0.25},
              .visible = true,
          }}}},
      },
  });
  require(readd.has_value(), "re-adding a removed layer must succeed");
  const auto readded_scene = await_prepared_scene(f.session);
  require(readded_scene != nullptr,
          "re-adding a layer must re-prepare a scene");
  const auto readded_layer = std::find_if(
      readded_scene->curve_layers().begin(), readded_scene->curve_layers().end(),
      [](const PreparedCurveLayer &layer) { return layer.id == layer_id; });
  require(readded_layer != readded_scene->curve_layers().end() &&
              readded_layer->segment_count > 0,
          "re-added visible layers must restore prepared geometry");
}

// TrackSpec has no separate visible flag, so the model's structural visibility
// operation is removing the track together with its dependent scale/layer and
// later re-adding that complete layout subtree. Both states must be reflected
// by the prepared scene, not merely retained in the presentation builder.
void remove_and_readd_track_changes_prepared_scene_visibility() {
  Fixture f;
  const auto remove = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = f.revision,
          .edits = {
              EntityEdit{RemoveEntity{second_layer_id}},
              EntityEdit{RemoveEntity{second_scale_id}},
              EntityEdit{RemoveEntity{second_track_id}},
          },
      },
  });
  require(remove.has_value(), "removing a complete secondary track must succeed");
  const auto hidden_scene = await_prepared_scene(f.session);
  require(hidden_scene != nullptr,
          "removing a track must re-prepare the scene");
  require(hidden_scene->tracks().size() == 1,
          "a removed track must be absent from the prepared scene");
  require(std::none_of(hidden_scene->tracks().begin(), hidden_scene->tracks().end(),
                       [](const PreparedTrack &track) {
                         return track.id == second_track_id;
                       }),
          "the removed track must have no prepared geometry");
  require_near(hidden_scene->physical_width().value, 40.0,
               "removing a track must shrink prepared scene width");

  const auto readd = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = f.session.document(document_id)->revision(),
          .edits = {
              EntityEdit{UpsertEntity{TrackSpec{
                  .id = second_track_id,
                  .width = Millimetres{30.0},
                  .z_order = 1,
              }}},
              EntityEdit{UpsertEntity{TrackScaleSpec{
                  .id = second_scale_id,
                  .track_id = second_track_id,
                  .mode = ScaleMode::linear,
                  .minimum = 0.0,
                  .maximum = 100.0,
                  .unit = "API",
              }}},
              EntityEdit{UpsertEntity{CurveLayerSpec{
                  .id = second_layer_id,
                  .track_id = second_track_id,
                  .curve_id = curve_id,
                  .scale_id = second_scale_id,
                  .color = {},
                  .line_width = Millimetres{0.25},
                  .visible = true,
              }}},
          },
      },
  });
  require(readd.has_value(), "re-adding a complete secondary track must succeed");
  const auto visible_scene = await_prepared_scene(f.session);
  require(visible_scene != nullptr,
          "re-adding a track must re-prepare the scene");
  const auto readded = std::find_if(
      visible_scene->tracks().begin(), visible_scene->tracks().end(),
      [](const PreparedTrack &track) { return track.id == second_track_id; });
  require(readded != visible_scene->tracks().end(),
          "the re-added track must restore prepared geometry");
  require_near(readded->bounds.width.value, 30.0,
               "the re-added track must retain its width");
  require_near(visible_scene->physical_width().value, 70.0,
               "re-adding a track must restore prepared scene width");
}

// A layout patch that would make the full presentation invalid must be
// rejected before any document revision or event is committed.
void invalid_presentation_patch_is_atomic() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = f.revision,
          .edits = {EntityEdit{RemoveEntity{track_id}}},
      },
  });
  require(!result.has_value(), "invalid layout patch must be rejected");
  require(result.error().code == ErrorCode::invalid_presentation,
          "invalid layout patch must use invalid_presentation");
  require(f.session.document(document_id)->revision() == f.revision,
          "rejected layout patch must preserve the document revision");
  require(f.session.events().empty(),
          "rejected layout patch must publish no state-change events");
}

// The preflight covers the patched document as well as the presentation. An
// interval that refers to an absent pattern must be rejected before it changes
// the revision, even though Interval is not itself a presentation entity.
void invalid_document_presentation_dependency_is_atomic() {
  Fixture f;
  const auto unknown_pattern = id("cc000000-0000-4000-8000-000000000015");
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = f.revision,
          .edits = {EntityEdit{UpsertEntity{Interval{
              .id = interval_id,
              .top_reference_depth = 1000.0,
              .bottom_reference_depth = 1001.0,
              .semantic = IntervalSemantic::lithology,
              .pattern_id = unknown_pattern,
              .label = "Sand",
          }}}},
      },
  });
  require(!result.has_value(), "invalid interval pattern must be rejected");
  require(result.error().code == ErrorCode::invalid_presentation,
          "unknown interval patterns must report invalid_presentation");
  require(f.session.document(document_id)->revision() == f.revision,
          "invalid document/presentation dependencies must preserve revision");
  require(f.session.events().empty(),
          "invalid document/presentation dependencies must publish no events");
}

// Presentation-only edits retain immutable source buffers and their ready LOD
// cache. Re-preparing the changed layout should complete one frame task, not a
// second LOD build plus frame task.
void presentation_patch_reuses_ready_lod_cache() {
  auto budgets = PerformanceBudgets{};
  budgets.asynchronous_sample_threshold = 1;
  budgets.maximum_cpu_derived_bytes = 1ULL * 1024ULL * 1024ULL;
  Fixture f(budgets);
  require(await_prepared_scene(f.session) != nullptr,
          "async fixture must finish its initial prepared scene");
  const auto before = f.session.performance_snapshot(document_id);
  require(before.has_value() && before->preparation_state == PreparationState::ready,
          "initial LOD preparation must be ready before the patch");

  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = f.revision,
          .edits = {EntityEdit{UpsertEntity{TrackSpec{
              .id = track_id,
              .width = Millimetres{80.0},
              .header = TrackHeaderSpec{.height = Millimetres{8.0}},
          }}}},
      },
  });
  require(result.has_value() && result.value().asynchronous_preparation_started,
          "presentation patch must start only its replacement frame task");
  require(await_prepared_scene(f.session) != nullptr,
          "presentation patch must publish its replacement scene");
  const auto after = f.session.performance_snapshot(document_id);
  require(after.has_value() && after->preparation_state == PreparationState::ready,
          "presentation patch must retain a ready LOD preparation");
  require(after->completed_tasks == before->completed_tasks + 1,
          "presentation patch must reuse LOD and complete only one frame task");
}

// The whole batch is atomic: one bad edit (a remove of a non-existent entity)
// rejects the entire patch and leaves the document unchanged.
void whole_batch_rejects_on_one_bad_edit() {
  Fixture f;
  const auto missing = id("cc000000-0000-4000-8000-000000000099");
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{UpsertEntity{
                          Interval{.id = interval_id,
                                   .top_reference_depth = 1000.0,
                                   .bottom_reference_depth = 1001.5,
                                   .pattern_id = {},
                                   .label = "Shale"}}},
                      EntityEdit{RemoveEntity{missing}}, // bad: not present
                  },
          },
  });
  require(!result.has_value(), "a patch with a bad edit must be rejected");
  require(result.error().code == ErrorCode::document_not_found,
          "a remove of a missing entity must return document_not_found");
  // The document must be unchanged (atomic).
  const auto doc = f.session.document(document_id);
  require(doc->revision().value == f.revision.value,
          "a rejected patch must leave the revision unchanged");
  require(doc->intervals().front().label == "Sand",
          "a rejected patch must leave the interval unchanged");
}

// A duplicate id within the batch is rejected (no ambiguous apply).
void duplicate_id_in_batch_rejected() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{RemoveEntity{interval_id}},
                      EntityEdit{RemoveEntity{interval_id}}, // duplicate
                  },
          },
  });
  require(!result.has_value(), "a duplicate-id patch must be rejected");
  require(result.error().code == ErrorCode::duplicate_entity_id,
          "a duplicate id must return duplicate_entity_id");
}

// A base-revision mismatch is rejected with patch_conflict (stable code), never
// applied by guessing.
void base_revision_mismatch_rejected_as_conflict() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = DocumentRevision{f.revision.value + 5}, // stale
              .edits = {EntityEdit{RemoveEntity{marker_id}}},
          },
  });
  require(!result.has_value(), "a stale-base patch must be rejected");
  require(result.error().code == ErrorCode::patch_conflict,
          "a base-revision mismatch must return patch_conflict");
  require(f.session.document(document_id)->revision().value == f.revision.value,
          "a conflict-rejected patch must leave the revision unchanged");
}

// The conflict gate applies even when there are no edits. A stale no-op is
// still a command built against obsolete state and must not look successful to
// a host that uses the receipt as an acknowledgement of its base revision.
void stale_empty_patch_is_rejected_as_conflict() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{
          .base_revision = DocumentRevision{f.revision.value + 1},
          .edits = {},
      },
  });
  require(!result.has_value(), "a stale empty patch must be rejected");
  require(result.error().code == ErrorCode::patch_conflict,
          "a stale empty patch must return patch_conflict");
  require(f.session.document(document_id)->revision() == f.revision,
          "a stale empty patch must leave the revision unchanged");
}

// An existing Selection Set survives a patch that does not move its axis range
// (remaps onto the new revision, stays valid).
void selection_survives_patch() {
  Fixture f;
  require(f.session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
              })
              .has_value(),
          "selection must be accepted");
  // Patch the interval (does not touch the axis) → selection must survive.
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{UpsertEntity{
                          Interval{.id = interval_id,
                                   .top_reference_depth = 1000.0,
                                   .bottom_reference_depth = 1001.5,
                                   .pattern_id = {},
                                   .label = "Shale"}}},
                  },
          },
  });
  require(result.has_value(), "patch must succeed");
  const auto sel = f.session.selection(document_id);
  require(sel.has_value() && sel->valid,
          "the selection must survive a non-axis patch");
  require(sel->document_revision.value ==
              result.value().document_revision.value,
          "the remapped selection must carry the patched revision");
}

// An empty patch is a no-op at the current revision.
void empty_patch_is_noop() {
  Fixture f;
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch = DocumentPatch{.base_revision = f.revision, .edits = {}},
  });
  require(result.has_value(), "an empty patch must succeed");
  require(result.value().document_revision.value == f.revision.value,
          "an empty patch must not advance the revision");
}

// A patch editing one collection leaves the OTHER collections byte-identical -
// the hand-rolled copy loops must not drop or duplicate untouched entities.
void patch_preserves_untouched_collections() {
  Fixture f;
  // Patch only the interval; markers, annotations, curves must survive.
  const auto result = f.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = f.revision,
              .edits =
                  {
                      EntityEdit{UpsertEntity{
                          Interval{.id = interval_id,
                                   .top_reference_depth = 1000.0,
                                   .bottom_reference_depth = 1001.8,
                                   .semantic = IntervalSemantic::lithology,
                                   .pattern_id = {},
                                   .label = "Shale"}}},
                  },
          },
  });
  require(result.has_value(), "patch must succeed");
  const auto doc = f.session.document(document_id);
  // The interval changed; markers/annotations/curves survive verbatim.
  require(doc->intervals().size() == 1 &&
              doc->intervals().front().label == "Shale",
          "the patched interval must reflect the upsert");
  require(doc->markers().size() == 1, "markers must survive a patch untouched");
  require(doc->markers().front().label == "Top A",
          "the surviving marker must be byte-identical");
  require(doc->annotations().size() == 1,
          "annotations must survive a patch untouched");
  require(doc->annotations().front().text == "Note",
          "the surviving annotation must be byte-identical");
  require(doc->curves().size() == 2,
          "curves must survive a patch untouched (immutable, ADR 0025)");
}

// A presentation-entity upsert on a document with NO presentation is rejected
// (a layout entity needs a presentation to live on).
void presentation_upsert_without_presentation_rejected() {
  // A document with no presentation registered.
  WellLogSession session;
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0});
  WellLogDocumentBuilder db(document_id, DocumentRevision{1});
  db.add_sampling_axis(
      SamplingAxis{.id = axis_id,
                   .coordinates = BufferView::from_vector(depths),
                   .domain = DepthDomain::measured_depth,
                   .unit = "m",
                   .direction = AxisDirection::increasing});
  db.add_curve(Curve{.id = curve_id,
                     .mnemonic = "GR",
                     .display_name = "GR",
                     .unit = "API",
                     .sampling_axis_id = axis_id,
                     .values = BufferView::from_vector(values),
                     .nulls = {}});
  require(session.execute(SetDocumentCommand{db.build()}).has_value(),
          "no-presentation document must be accepted");
  const auto result = session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = DocumentRevision{1},
              .edits =
                  {
                      EntityEdit{UpsertEntity{TrackSpec{
                          .id = track_id, .width = Millimetres{40.0}}}},
                  },
          },
  });
  require(!result.has_value(),
          "a presentation-entity upsert with no presentation must be rejected");
  require(
      result.error().code == ErrorCode::invalid_presentation,
      "no-presentation presentation upsert must return invalid_presentation");
}

} // namespace

int main() {
  upsert_replaces_document_entity();
  upsert_creates_document_entity();
  remove_deletes_document_entity();
  upsert_edits_presentation_entity();
  track_z_order_patch_reorders_prepared_tracks();
  scale_patch_changes_prepared_metadata_and_geometry();
  curve_layer_style_patch_changes_prepared_layer();
  curve_layer_visibility_patch_removes_prepared_geometry();
  remove_deletes_presentation_entity();
  remove_and_readd_track_changes_prepared_scene_visibility();
  invalid_presentation_patch_is_atomic();
  invalid_document_presentation_dependency_is_atomic();
  presentation_patch_reuses_ready_lod_cache();
  whole_batch_rejects_on_one_bad_edit();
  duplicate_id_in_batch_rejected();
  base_revision_mismatch_rejected_as_conflict();
  stale_empty_patch_is_rejected_as_conflict();
  selection_survives_patch();
  empty_patch_is_noop();
  patch_preserves_untouched_collections();
  presentation_upsert_without_presentation_rejected();
  std::cout << "welllog.apply-patch: all cases passed\n";
  return EXIT_SUCCESS;
}
