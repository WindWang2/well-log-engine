// Headless test for #205: Interval/Marker/Annotation create/move/modify/delete
// via ApplyPatchCommand (#202), with prepared-scene reflection and invalid-edit
// rejections. Covers AC #2 of the #158 epic (ADR 0025). The fixture includes
// interval/marker/text layers in the presentation so the prepared scene renders
// the interpretation entities, enabling end-to-end assertions. No GL/Qt.

#include <welllog/core/document.hpp>
#include <welllog/core/utf8.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
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
  require(std::abs(actual - expected) < 1.0e-9, message);
}

// The patch tests need prepared text runs to prove annotation and label changes
// reach the scene. Keep that proof independent of optional HarfBuzz/FreeType
// build dependencies with a deterministic, minimal shaping implementation.
class TestTextEngine final : public TextEngine {
public:
  [[nodiscard]] Result<ShapedRun>
  shape(const TextShapeRequest &request) noexcept override {
    ShapedRun run;
    run.ascender = 0.8;
    run.descender = -0.2;
    run.glyphs.reserve(request.text.size());
    std::uint32_t cluster{};
    for (const auto byte : request.text) {
      const auto code_point = static_cast<unsigned char>(byte);
      run.glyphs.push_back(ShapedGlyph{
          .glyph_id = static_cast<std::uint32_t>(code_point) + 1,
          .font_index = 0,
          .cluster = cluster++,
          .code_point = code_point,
          .advance_x = 0.6,
          .advance_y = 0.0,
          .offset_x = 0.0,
          .offset_y = 0.0,
          .upright = true,
      });
    }
    return run;
  }

  [[nodiscard]] Result<GlyphOutline>
  glyph_outline(std::uint32_t, std::uint32_t) noexcept override {
    return GlyphOutline{
        .commands = {},
        .advance_x = 0.6,
        .left = 0.0,
        .bottom = 0.0,
        .right = 0.6,
        .top = 0.8,
    };
  }

  [[nodiscard]] std::string font_fingerprint(std::uint32_t) const override {
    return "patch-test-font-v1";
  }

  [[nodiscard]] std::string font_family_name(std::uint32_t) const override {
    return "Patch Test";
  }
};

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("dd000000-0000-4000-8000-000000000001");
const auto axis_id = id("dd000000-0000-4000-8000-000000000002");
const auto curve_id = id("dd000000-0000-4000-8000-000000000003");
const auto track_id = id("dd000000-0000-4000-8000-000000000004");
const auto scale_id = id("dd000000-0000-4000-8000-000000000005");
const auto layer_id = id("dd000000-0000-4000-8000-000000000006");
const auto interval_layer_id = id("dd000000-0000-4000-8000-000000000010");
const auto marker_layer_id = id("dd000000-0000-4000-8000-000000000011");
const auto text_layer_id = id("dd000000-0000-4000-8000-000000000012");
const auto interval_id = id("dd000000-0000-4000-8000-000000000007");
const auto marker_id = id("dd000000-0000-4000-8000-000000000008");
const auto annotation_id = id("dd000000-0000-4000-8000-000000000009");

const PreparedInterval *find_interval(const PreparedScene &scene,
                                      EntityId entity_id) {
  const auto found = std::find_if(
      scene.intervals().begin(), scene.intervals().end(),
      [entity_id](const PreparedInterval &interval) {
        return interval.interval_id == entity_id;
      });
  return found == scene.intervals().end() ? nullptr : &*found;
}

const PreparedMarker *find_marker(const PreparedScene &scene, EntityId entity_id) {
  const auto found = std::find_if(
      scene.markers().begin(), scene.markers().end(),
      [entity_id](const PreparedMarker &marker) {
        return marker.marker_id == entity_id;
      });
  return found == scene.markers().end() ? nullptr : &*found;
}

const PreparedTextRun *find_text_run(const PreparedScene &scene,
                                     EntityId entity_id) {
  const auto found = std::find_if(
      scene.text_runs().begin(), scene.text_runs().end(),
      [entity_id](const PreparedTextRun &run) {
        return run.source_entity_id == entity_id;
      });
  return found == scene.text_runs().end() ? nullptr : &*found;
}

// Fixture: document with axis/curve + one Interval/Marker/Annotation, and a
// presentation with Track/Scale/CurveLayer + IntervalLayer/MarkerLayer/TextLayer
// so the prepared scene renders all three interpretation entity types.
struct Fixture {
  WellLogSession session;
  DocumentRevision revision;

  Fixture() {
    session.set_text_engine(std::make_shared<TestTextEngine>());
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
    WellLogDocumentBuilder db(document_id, DocumentRevision{1});
    db.add_sampling_axis(SamplingAxis{
        .id = axis_id, .coordinates = BufferView::from_vector(depths),
        .domain = DepthDomain::measured_depth, .unit = "m",
        .direction = AxisDirection::increasing});
    db.add_curve(Curve{
        .id = curve_id, .mnemonic = "GR", .display_name = "Gamma Ray",
        .unit = "API", .sampling_axis_id = axis_id,
        .values = BufferView::from_vector(values), .nulls = {}});
    db.add_interval(Interval{
        .id = interval_id, .top_reference_depth = 1000.0,
        .bottom_reference_depth = 1001.0, .semantic = IntervalSemantic::lithology,
        .pattern_id = {}, .fill_color = {255, 0, 0, 255}, .label = "Sand"});
    db.add_marker(Marker{
        .id = marker_id, .reference_depth = 1000.5,
        .semantic = MarkerSemantic::formation_top, .label = "Top A"});
    TextAnnotation ann;
    ann.id = annotation_id;
    ann.reference_depth = 1001.0;
    ann.text = "Note";
    db.add_annotation(ann);
    require(session.execute(SetDocumentCommand{db.build()}).has_value(),
            "fixture document must be accepted");

    ScenePresentationBuilder pb(
        document_id,
        ReferenceDepthRange{
            .domain = DepthDomain::measured_depth, .unit = "m",
            .top = 1000.0, .bottom = 1003.0,
        },
        Millimetres{500.0}, "fixture-font");
    pb.add_track(TrackSpec{.id = track_id, .width = Millimetres{40.0}});
    pb.add_scale(TrackScaleSpec{
        .id = scale_id, .track_id = track_id, .mode = ScaleMode::linear,
        .minimum = 0.0, .maximum = 100.0, .unit = "API"});
    pb.add_curve_layer(CurveLayerSpec{
        .id = layer_id, .track_id = track_id, .curve_id = curve_id,
        .scale_id = scale_id, .color = {}, .line_width = Millimetres{0.25},
        .visible = true});
    pb.add_interval_layer(IntervalLayerSpec{
        .id = interval_layer_id, .track_id = track_id, .z_order = 0});
    pb.add_marker_layer(MarkerLayerSpec{
        .id = marker_layer_id, .track_id = track_id});
    pb.add_text_layer(TextLayerSpec{
        .id = text_layer_id, .track_id = track_id});
    require(session.execute(SetPresentationCommand{pb.build()}).has_value(),
            "fixture presentation must be accepted");
    session.clear_events();
    revision = session.document(document_id)->revision();
  }

  // Applies a patch and returns the result.
  Result<CommandReceipt> patch(std::vector<EntityEdit> edits) {
    return session.execute(ApplyPatchCommand{
        .document_id = document_id,
        .patch = DocumentPatch{.base_revision = revision, .edits = std::move(edits)},
    });
  }
};

// --- Interval: create, move, modify, delete ---

void interval_create_move_modify_delete() {
  Fixture f;
  // Create a new interval.
  const auto new_iv = id("dd000000-0000-4000-8000-000000000020");
  require(f.patch({EntityEdit{UpsertEntity{Interval{
              .id = new_iv, .top_reference_depth = 1001.0,
              .bottom_reference_depth = 1002.0,
              .semantic = IntervalSemantic::facies, .pattern_id = {},
              .fill_color = {0, 255, 0, 255}, .label = "Shale"}}}})
              .has_value(),
          "create interval must succeed");
  const auto doc1 = f.session.document(document_id);
  require(doc1->intervals().size() == 2, "new interval must be added");
  const auto created_document_interval = std::find_if(
      doc1->intervals().begin(), doc1->intervals().end(),
      [new_iv](const Interval &interval) { return interval.id == new_iv; });
  require(created_document_interval != doc1->intervals().end() &&
              created_document_interval->semantic == IntervalSemantic::facies &&
              created_document_interval->label == "Shale" &&
              created_document_interval->fill_color == RgbaColor{0, 255, 0, 255},
          "the created interval fields must be readable on the document");
  const auto created_scene = f.session.prepared_scene(document_id);
  require(created_scene != nullptr,
          "creating an interval must refresh the prepared scene");
  const auto created_interval = find_interval(*created_scene, new_iv);
  require(created_interval != nullptr &&
              created_interval->fill_color == RgbaColor{0, 255, 0, 255},
          "the prepared scene must contain the created interval and its color");

  // Move the original interval (change depths).
  f.revision = doc1->revision();
  require(f.patch({EntityEdit{UpsertEntity{Interval{
              .id = interval_id, .top_reference_depth = 1000.5,
              .bottom_reference_depth = 1001.5,
              .semantic = IntervalSemantic::lithology, .pattern_id = {},
              .fill_color = {255, 0, 0, 255}, .label = "Sand"}}}})
              .has_value(),
          "move interval must succeed");
  const auto doc2 = f.session.document(document_id);
  const auto moved = std::find_if(
      doc2->intervals().begin(), doc2->intervals().end(),
      [](const Interval &i) { return i.id == interval_id; });
  require(moved != doc2->intervals().end(),
          "moved interval must remain readable on the document");
  require_near(moved->top_reference_depth, 1000.5,
               "moved interval must carry the new top depth");
  require_near(moved->bottom_reference_depth, 1001.5,
               "moved interval must carry the new bottom depth");
  const auto moved_scene = f.session.prepared_scene(document_id);
  require(moved_scene != nullptr, "moving an interval must refresh the scene");
  const auto moved_interval = find_interval(*moved_scene, interval_id);
  require(moved_interval != nullptr, "the moved interval must be prepared");
  require_near(moved_interval->top_reference_depth, 1000.5,
               "the prepared interval must carry the moved top depth");
  require_near(moved_interval->bottom_reference_depth, 1001.5,
               "the prepared interval must carry the moved bottom depth");

  // Modify the interval (change label + fill_color).
  f.revision = doc2->revision();
  require(f.patch({EntityEdit{UpsertEntity{Interval{
              .id = interval_id, .top_reference_depth = 1000.5,
              .bottom_reference_depth = 1001.5,
              .semantic = IntervalSemantic::stratigraphy, .pattern_id = {},
              .fill_color = {0, 0, 255, 255}, .label = "Modified"}}}})
              .has_value(),
          "modify interval must succeed");
  const auto doc3 = f.session.document(document_id);
  const auto mod = std::find_if(
      doc3->intervals().begin(), doc3->intervals().end(),
      [](const Interval &i) { return i.id == interval_id; });
  require(mod != doc3->intervals().end() && mod->label == "Modified" &&
              mod->semantic == IntervalSemantic::stratigraphy &&
              mod->fill_color == RgbaColor{0, 0, 255, 255},
          "modified interval must carry the new label, semantic, and color");
  const auto modified_scene = f.session.prepared_scene(document_id);
  require(modified_scene != nullptr,
          "modifying an interval must refresh the prepared scene");
  const auto modified_interval = find_interval(*modified_scene, interval_id);
  require(modified_interval != nullptr &&
              modified_interval->fill_color == RgbaColor{0, 0, 255, 255},
          "the prepared interval must carry the modified fill color");
  const auto modified_label = find_text_run(*modified_scene, interval_id);
  require(modified_label != nullptr && modified_label->text == "Modified",
          "the prepared interval label must carry the modified text");

  // Delete the new interval.
  f.revision = doc3->revision();
  require(f.patch({EntityEdit{RemoveEntity{new_iv}}}).has_value(),
          "delete interval must succeed");
  const auto doc4 = f.session.document(document_id);
  require(doc4->intervals().size() == 1, "deleted interval must be gone");
  require(doc4->intervals().front().id == interval_id,
          "the remaining interval must be the original");
  const auto deleted_scene = f.session.prepared_scene(document_id);
  require(deleted_scene != nullptr,
          "deleting an interval must refresh the prepared scene");
  require(find_interval(*deleted_scene, new_iv) == nullptr,
          "the deleted interval must be absent from the prepared scene");
}

// --- Marker: create, move, modify, delete ---

void marker_create_move_modify_delete() {
  Fixture f;
  // Create.
  const auto new_m = id("dd000000-0000-4000-8000-000000000021");
  require(f.patch({EntityEdit{UpsertEntity{Marker{
              .id = new_m, .reference_depth = 1002.0,
              .semantic = MarkerSemantic::fault, .label = "Fault"}}}})
              .has_value(),
          "create marker must succeed");
  const auto created_document_markers = f.session.document(document_id)->markers();
  require(created_document_markers.size() == 2,
          "new marker must be added");
  const auto created_document_marker = std::find_if(
      created_document_markers.begin(), created_document_markers.end(),
      [new_m](const Marker &marker) { return marker.id == new_m; });
  require(created_document_marker != created_document_markers.end() &&
              created_document_marker->semantic == MarkerSemantic::fault &&
              created_document_marker->label == "Fault",
          "the created marker fields must be readable on the document");
  const auto created_scene = f.session.prepared_scene(document_id);
  require(created_scene != nullptr && find_marker(*created_scene, new_m) != nullptr,
          "the prepared scene must contain the created marker");

  // Move.
  f.revision = f.session.document(document_id)->revision();
  require(f.patch({EntityEdit{UpsertEntity{Marker{
              .id = marker_id, .reference_depth = 1001.5,
              .semantic = MarkerSemantic::formation_top, .label = "Top A"}}}})
              .has_value(),
          "move marker must succeed");
  const auto markers_moved = f.session.document(document_id)->markers();
  const auto moved_m = std::find_if(
      markers_moved.begin(), markers_moved.end(),
      [](const Marker &m) { return m.id == marker_id; });
  require(moved_m != markers_moved.end(),
          "moved marker must remain readable on the document");
  require_near(moved_m->reference_depth, 1001.5,
               "moved marker must carry the new depth");
  const auto moved_scene = f.session.prepared_scene(document_id);
  require(moved_scene != nullptr, "moving a marker must refresh the scene");
  const auto moved_marker = find_marker(*moved_scene, marker_id);
  require(moved_marker != nullptr, "the moved marker must be prepared");
  require_near(moved_marker->reference_depth, 1001.5,
               "the prepared marker must carry the moved depth");

  // Modify (label + semantic).
  f.revision = f.session.document(document_id)->revision();
  require(f.patch({EntityEdit{UpsertEntity{Marker{
              .id = marker_id, .reference_depth = 1001.5,
              .semantic = MarkerSemantic::casing_shoe, .label = "Shoe"}}}})
              .has_value(),
          "modify marker must succeed");
  const auto markers_mod = f.session.document(document_id)->markers();
  const auto mod_m = std::find_if(
      markers_mod.begin(), markers_mod.end(),
      [](const Marker &m) { return m.id == marker_id; });
  require(mod_m != markers_mod.end() && mod_m->label == "Shoe" &&
              mod_m->semantic == MarkerSemantic::casing_shoe,
          "modified marker must carry the new label and semantic");
  const auto modified_scene = f.session.prepared_scene(document_id);
  require(modified_scene != nullptr,
          "modifying a marker must refresh the prepared scene");
  const auto modified_label = find_text_run(*modified_scene, marker_id);
  require(modified_label != nullptr && modified_label->text == "Shoe",
          "the prepared marker label must carry the modified text");

  // Delete.
  f.revision = f.session.document(document_id)->revision();
  require(f.patch({EntityEdit{RemoveEntity{new_m}}}).has_value(),
          "delete marker must succeed");
  require(f.session.document(document_id)->markers().size() == 1,
          "deleted marker must be gone");
  const auto deleted_scene = f.session.prepared_scene(document_id);
  require(deleted_scene != nullptr && find_marker(*deleted_scene, new_m) == nullptr,
          "the deleted marker must be absent from the prepared scene");
}

// --- Annotation: create, move, modify, delete ---

void annotation_create_move_modify_delete() {
  Fixture f;
  // Create.
  const auto new_a = id("dd000000-0000-4000-8000-000000000022");
  TextAnnotation new_ann;
  new_ann.id = new_a;
  new_ann.reference_depth = 1002.0;
  new_ann.text = "New note";
  require(f.patch({EntityEdit{UpsertEntity{new_ann}}}).has_value(),
          "create annotation must succeed");
  const auto created_document_annotations =
      f.session.document(document_id)->annotations();
  require(created_document_annotations.size() == 2,
          "new annotation must be added");
  const auto created_document_annotation = std::find_if(
      created_document_annotations.begin(), created_document_annotations.end(),
      [new_a](const TextAnnotation &annotation) { return annotation.id == new_a; });
  require(created_document_annotation != created_document_annotations.end() &&
              created_document_annotation->text == "New note",
          "the created annotation fields must be readable on the document");
  const auto created_scene = f.session.prepared_scene(document_id);
  require(created_scene != nullptr, "creating an annotation must refresh the scene");
  const auto created_run = find_text_run(*created_scene, new_a);
  require(created_run != nullptr && created_run->text == "New note",
          "the prepared scene must contain the created annotation text");

  // Move (change depth).
  f.revision = f.session.document(document_id)->revision();
  TextAnnotation moved_ann;
  moved_ann.id = annotation_id;
  moved_ann.reference_depth = 1000.5;
  moved_ann.text = "Note";
  require(f.patch({EntityEdit{UpsertEntity{moved_ann}}}).has_value(),
          "move annotation must succeed");
  const auto anns_moved = f.session.document(document_id)->annotations();
  const auto moved_a = std::find_if(
      anns_moved.begin(), anns_moved.end(),
      [](const TextAnnotation &a) { return a.id == annotation_id; });
  require(moved_a != anns_moved.end(),
          "moved annotation must remain readable on the document");
  require_near(moved_a->reference_depth, 1000.5,
               "moved annotation must carry the new depth");
  const auto moved_scene = f.session.prepared_scene(document_id);
  require(moved_scene != nullptr, "moving an annotation must refresh the scene");
  const auto moved_run = find_text_run(*moved_scene, annotation_id);
  require(moved_run != nullptr, "the moved annotation must be prepared");
  require_near(moved_run->anchor.top.value, 500.0 / 6.0,
               "the prepared annotation anchor must reflect the moved depth");

  // Modify (change text).
  f.revision = f.session.document(document_id)->revision();
  TextAnnotation mod_ann;
  mod_ann.id = annotation_id;
  mod_ann.reference_depth = 1000.5;
  mod_ann.text = "Changed";
  require(f.patch({EntityEdit{UpsertEntity{mod_ann}}}).has_value(),
          "modify annotation must succeed");
  const auto anns_mod = f.session.document(document_id)->annotations();
  const auto mod_a = std::find_if(
      anns_mod.begin(), anns_mod.end(),
      [](const TextAnnotation &a) { return a.id == annotation_id; });
  require(mod_a != anns_mod.end() && mod_a->text == "Changed",
          "modified annotation must carry the new text");
  const auto modified_scene = f.session.prepared_scene(document_id);
  require(modified_scene != nullptr,
          "modifying an annotation must refresh the prepared scene");
  const auto modified_run = find_text_run(*modified_scene, annotation_id);
  require(modified_run != nullptr && modified_run->text == "Changed",
          "the prepared annotation text must carry the modification");

  // Delete.
  f.revision = f.session.document(document_id)->revision();
  require(f.patch({EntityEdit{RemoveEntity{new_a}}}).has_value(),
          "delete annotation must succeed");
  require(f.session.document(document_id)->annotations().size() == 1,
          "deleted annotation must be gone");
  const auto deleted_scene = f.session.prepared_scene(document_id);
  require(deleted_scene != nullptr && find_text_run(*deleted_scene, new_a) == nullptr,
          "the deleted annotation must be absent from the prepared scene");
}

// --- Invalid-edit rejections (ADR 0028 strict validation) ---

// An Interval with top >= bottom is rejected; the document is unchanged.
void invalid_interval_top_ge_bottom_rejected() {
  Fixture f;
  const auto result = f.patch({EntityEdit{UpsertEntity{Interval{
      .id = interval_id, .top_reference_depth = 1001.5,
      .bottom_reference_depth = 1001.0, // top > bottom -> invalid
      .semantic = IntervalSemantic::lithology, .pattern_id = {},
      .fill_color = {}, .label = "Bad"}}}});
  require(!result.has_value(), "an interval with top >= bottom must be rejected");
  require(result.error().code == ErrorCode::invalid_document,
          "an invalid interval must return invalid_document");
  // Document unchanged.
  require(f.session.document(document_id)->revision().value == f.revision.value,
          "a rejected patch must leave the revision unchanged");
  require(f.session.document(document_id)->intervals().front().label == "Sand",
          "a rejected patch must leave the interval unchanged");
}

// An invalid UTF-8 label is rejected.
void invalid_utf8_label_rejected() {
  Fixture f;
  // A lone continuation byte 0x80 is invalid UTF-8.
  const std::string bad_label = "Bad\x80";
  const auto result = f.patch({EntityEdit{UpsertEntity{Marker{
      .id = marker_id, .reference_depth = 1000.5,
      .semantic = MarkerSemantic::formation_top, .label = bad_label}}}});
  require(!result.has_value(), "an invalid UTF-8 label must be rejected");
  require(result.error().code == ErrorCode::invalid_document,
          "an invalid-encoding label must return invalid_document");
  require(f.session.document(document_id)->markers().front().label == "Top A",
          "a rejected patch must leave the marker unchanged");
}

// --- Prepared-scene reflection ---

// A patched Interval move is reflected in the prepared scene's interval depths.
void interval_move_reflected_in_prepared_scene() {
  Fixture f;
  require(f.patch({EntityEdit{UpsertEntity{Interval{
              .id = interval_id, .top_reference_depth = 1000.0,
              .bottom_reference_depth = 1002.0,
              .semantic = IntervalSemantic::lithology, .pattern_id = {},
              .fill_color = {255, 0, 0, 255}, .label = "Sand"}}}})
              .has_value(),
          "move interval must succeed");
  const auto scene = f.session.prepared_scene(document_id);
  require(scene != nullptr, "a prepared scene must exist");
  const auto intervals = scene->intervals();
  require(!intervals.empty(), "the prepared scene must have intervals");
  const auto pi = std::find_if(
      intervals.begin(), intervals.end(),
      [](const PreparedInterval &p) { return p.interval_id == interval_id; });
  require(pi != intervals.end(),
          "the moved interval must appear in the prepared scene");
  require_near(pi->bottom_reference_depth, 1002.0,
               "the prepared interval must reflect the moved bottom depth");
}

// A patched Marker move is reflected in the prepared scene's marker depth.
void marker_move_reflected_in_prepared_scene() {
  Fixture f;
  require(f.patch({EntityEdit{UpsertEntity{Marker{
              .id = marker_id, .reference_depth = 1002.0,
              .semantic = MarkerSemantic::formation_top, .label = "Top A"}}}})
              .has_value(),
          "move marker must succeed");
  const auto scene = f.session.prepared_scene(document_id);
  require(scene != nullptr, "a prepared scene must exist");
  const auto markers = scene->markers();
  const auto pm = std::find_if(
      markers.begin(), markers.end(),
      [](const PreparedMarker &p) { return p.marker_id == marker_id; });
  require(pm != markers.end(),
          "the moved marker must appear in the prepared scene");
  require_near(pm->reference_depth, 1002.0,
               "the prepared marker must reflect the moved depth");
}

// A patched Annotation text change is reflected in the prepared scene's text.
void annotation_modify_reflected_in_prepared_scene() {
  Fixture f;
  TextAnnotation mod;
  mod.id = annotation_id;
  mod.reference_depth = 1001.0;
  mod.text = "Changed";
  require(f.patch({EntityEdit{UpsertEntity{mod}}}).has_value(),
          "modify annotation must succeed");
  const auto scene = f.session.prepared_scene(document_id);
  require(scene != nullptr, "a prepared scene must exist");
  const auto run = find_text_run(*scene, annotation_id);
  require(run != nullptr && run->text == "Changed",
          "the prepared text run must reflect the modified annotation text");
  const auto anns = f.session.document(document_id)->annotations();
  require(std::any_of(anns.begin(), anns.end(),
                      [](const TextAnnotation &a) { return a.text == "Changed"; }),
          "the modified annotation must be readable on the document");
}

// Deleting an Interval removes it from the prepared scene.
void interval_delete_reflected_in_prepared_scene() {
  Fixture f;
  require(f.patch({EntityEdit{RemoveEntity{interval_id}}}).has_value(),
          "delete interval must succeed");
  const auto scene = f.session.prepared_scene(document_id);
  require(scene != nullptr, "a prepared scene must exist");
  const auto intervals = scene->intervals();
  require(std::none_of(intervals.begin(), intervals.end(),
                       [](const PreparedInterval &p) {
                         return p.interval_id == interval_id;
                       }),
          "the deleted interval must not appear in the prepared scene");
}

} // namespace

int main() {
  interval_create_move_modify_delete();
  marker_create_move_modify_delete();
  annotation_create_move_modify_delete();
  invalid_interval_top_ge_bottom_rejected();
  invalid_utf8_label_rejected();
  interval_move_reflected_in_prepared_scene();
  marker_move_reflected_in_prepared_scene();
  annotation_modify_reflected_in_prepared_scene();
  interval_delete_reflected_in_prepared_scene();
  std::cout << "welllog.interpretation-patch: all cases passed\n";
  return EXIT_SUCCESS;
}
