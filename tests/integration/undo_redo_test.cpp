// Headless tests for the kernel-owned undo/redo history (#203, ADR 0025).
// Covers patch round trips, redo clearing, exact semantic Selection Set
// restoration, history observability, and immutable append-buffer restoration.

#include <welllog/core/document.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
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

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("cd000000-0000-4000-8000-000000000001");
const auto axis_id = id("cd000000-0000-4000-8000-000000000002");
const auto curve_id = id("cd000000-0000-4000-8000-000000000003");
const auto interval_id = id("cd000000-0000-4000-8000-000000000004");
const auto marker_id = id("cd000000-0000-4000-8000-000000000005");
const auto annotation_id = id("cd000000-0000-4000-8000-000000000006");
const auto extra_marker_id = id("cd000000-0000-4000-8000-000000000007");

struct Fixture {
  WellLogSession session;

  Fixture() {
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1001.0, 1002.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 20.0, 30.0});
    WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
    builder.add_sampling_axis(
        SamplingAxis{.id = axis_id,
                     .coordinates = BufferView::from_vector(depths),
                     .domain = DepthDomain::measured_depth,
                     .unit = "m",
                     .direction = AxisDirection::increasing});
    builder.add_curve(Curve{.id = curve_id,
                            .mnemonic = "GR",
                            .display_name = "Gamma Ray",
                            .unit = "API",
                            .sampling_axis_id = axis_id,
                            .values = BufferView::from_vector(values),
                            .nulls = {}});
    builder.add_interval(Interval{.id = interval_id,
                                  .top_reference_depth = 1000.0,
                                  .bottom_reference_depth = 1001.0,
                                  .semantic = IntervalSemantic::lithology,
                                  .pattern_id = {},
                                  .label = "Start"});
    builder.add_marker(Marker{.id = marker_id,
                              .reference_depth = 1000.5,
                              .semantic = MarkerSemantic::formation_top,
                              .label = "Top A"});
    TextAnnotation annotation;
    annotation.id = annotation_id;
    annotation.reference_depth = 1001.0;
    annotation.text = "Initial note";
    builder.add_annotation(annotation);
    require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
            "fixture document must be accepted");
    session.clear_events();
  }

  [[nodiscard]] DocumentRevision revision() const {
    return session.document(document_id)->revision();
  }

  void apply(std::vector<EntityEdit> edits) {
    const auto result = session.execute(ApplyPatchCommand{
        .document_id = document_id,
        .patch = DocumentPatch{.base_revision = revision(),
                               .edits = std::move(edits)},
    });
    require(result.has_value(), "patch must succeed");
  }

  [[nodiscard]] std::string interval_label() const {
    return session.document(document_id)->intervals().front().label;
  }
};

[[nodiscard]] EntityEdit interval_with_label(std::string label) {
  return UpsertEntity{Interval{.id = interval_id,
                               .top_reference_depth = 1000.0,
                               .bottom_reference_depth = 1001.0,
                               .semantic = IntervalSemantic::lithology,
                               .pattern_id = {},
                               .label = std::move(label)}};
}

[[nodiscard]] bool saw_history_changed(const WellLogSession &session) {
  return std::any_of(session.events().begin(), session.events().end(),
                     [](const ViewEvent &event) {
                       return event.kind == ViewEventKind::history_changed;
                     });
}

void patch_round_trip_restores_each_semantic_revision() {
  Fixture fixture;
  fixture.apply({interval_with_label("One")});
  fixture.apply({UpsertEntity{Marker{.id = extra_marker_id,
                                     .reference_depth = 1001.5,
                                     .semantic = MarkerSemantic::fault,
                                     .label = "Fault"}}});
  fixture.apply({RemoveEntity{annotation_id}});

  require(fixture.revision().value == 4, "three patches must reach revision 4");
  require(fixture.session.can_undo(document_id),
          "three patches must be undoable");
  require(!fixture.session.can_redo(document_id), "fresh history has no redo");

  const auto undo_annotation =
      fixture.session.execute(UndoCommand{document_id});
  require(undo_annotation.has_value(), "undo must restore removed annotation");
  require(fixture.revision().value == 3, "undo must restore revision 3");
  require(fixture.session.document(document_id)->annotations().size() == 1,
          "undo must restore annotation semantics");
  require(fixture.session.document(document_id)->markers().size() == 2,
          "undo must retain the earlier marker creation");

  const auto undo_marker = fixture.session.execute(UndoCommand{document_id});
  require(undo_marker.has_value(), "second undo must succeed");
  require(fixture.revision().value == 2, "second undo must restore revision 2");
  require(fixture.session.document(document_id)->markers().size() == 1,
          "second undo must remove only the created marker");
  require(fixture.interval_label() == "One",
          "second undo must retain the first patch value");

  const auto undo_interval = fixture.session.execute(UndoCommand{document_id});
  require(undo_interval.has_value(), "third undo must succeed");
  require(fixture.revision().value == 1, "third undo must restore revision 1");
  require(fixture.interval_label() == "Start",
          "third undo must restore original interval value");
  require(!fixture.session.can_undo(document_id), "initial state has no undo");
  require(fixture.session.can_redo(document_id),
          "undone patches must be redoable");

  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "first redo must succeed");
  require(fixture.revision().value == 2 && fixture.interval_label() == "One",
          "first redo must restore first patch semantics");
  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "second redo must succeed");
  require(fixture.revision().value == 3 &&
              fixture.session.document(document_id)->markers().size() == 2,
          "second redo must restore marker creation semantics");
  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "third redo must succeed");
  require(fixture.revision().value == 4 &&
              fixture.session.document(document_id)->annotations().empty(),
          "third redo must restore annotation removal semantics");
}

void new_patch_clears_redo_history() {
  Fixture fixture;
  fixture.apply({interval_with_label("One")});
  require(fixture.session.execute(UndoCommand{document_id}).has_value(),
          "undo must create a redo entry");
  require(fixture.session.can_redo(document_id), "undo must expose redo");

  fixture.apply({interval_with_label("Branch")});
  require(!fixture.session.can_redo(document_id),
          "a new patch after undo must clear redo history");
  const auto redo = fixture.session.execute(RedoCommand{document_id});
  require(!redo.has_value(), "cleared redo command must fail");
  require(redo.error().code == ErrorCode::history_empty,
          "an empty redo must report history_empty");
  require(fixture.interval_label() == "Branch",
          "the branch patch must remain the current semantic state");
}

void selection_state_is_restored_with_document_revision() {
  Fixture fixture;
  require(fixture.session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
              })
              .has_value(),
          "fixture selection must be accepted");
  fixture.apply({interval_with_label("Selected")});
  const auto patched = fixture.session.selection(document_id);
  require(patched.has_value() && patched->valid &&
              patched->document_revision.value == 2,
          "patch must carry the remapped selection at revision 2");

  bool observer_saw_restored_selection = false;
  const auto observer = fixture.session.subscribe_view_events(
      [&fixture, &observer_saw_restored_selection](const ViewEvent &event) {
        if (event.kind != ViewEventKind::documents_changed ||
            event.document_revision.value != 1) {
          return;
        }
        const auto observed = fixture.session.selection(document_id);
        observer_saw_restored_selection =
            observed.has_value() && observed->valid &&
            observed->document_revision == event.document_revision;
      });
  require(observer != 0, "history observer must subscribe");
  require(fixture.session.execute(UndoCommand{document_id}).has_value(),
          "undo must succeed with a selection");
  fixture.session.unsubscribe_view_events(observer);
  const auto restored = fixture.session.selection(document_id);
  require(
      restored.has_value() && restored->valid &&
          restored->document_revision.value == 1,
      "undo must restore the selection semantic revision, not widget pixels");
  require(observer_saw_restored_selection,
          "document observers must see the restored selection, never a "
          "transient one");

  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "redo must succeed with a selection");
  const auto redone = fixture.session.selection(document_id);
  require(redone.has_value() && redone->valid &&
              redone->document_revision.value == 2,
          "redo must restore the selected patch revision");
}

void history_state_is_observable_through_accessors_and_events() {
  Fixture fixture;
  fixture.apply({interval_with_label("One")});
  require(fixture.session.can_undo(document_id) &&
              !fixture.session.can_redo(document_id),
          "patch commit must expose undo-only history");
  require(saw_history_changed(fixture.session),
          "patch commit must publish history_changed");

  fixture.session.clear_events();
  require(fixture.session.execute(UndoCommand{document_id}).has_value(),
          "undo must succeed");
  require(!fixture.session.can_undo(document_id) &&
              fixture.session.can_redo(document_id),
          "undo must expose redo-only history");
  require(saw_history_changed(fixture.session),
          "undo must publish history_changed");

  fixture.session.clear_events();
  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "redo must succeed");
  require(fixture.session.can_undo(document_id) &&
              !fixture.session.can_redo(document_id),
          "redo must restore undo-only history");
  require(saw_history_changed(fixture.session),
          "redo must publish history_changed");
}

void append_commit_is_undoable_without_copying_old_semantics() {
  Fixture fixture;
  auto tail_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1003.0, 1004.0});
  auto tail_values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{40.0, 50.0});
  const auto append = fixture.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks = {CurveTailBlock{
          .curve_id = curve_id,
          .sampling_axis_id = axis_id,
          .tail_coordinates = BufferView::from_vector(tail_depths),
          .tail_values = BufferView::from_vector(tail_values),
      }},
  });
  require(append.has_value(), "append must succeed");
  require(
      fixture.session.document(document_id)->curves().front().values.length() ==
          5,
      "append must expose tail values before undo");

  require(fixture.session.execute(UndoCommand{document_id}).has_value(),
          "append must be undoable");
  require(
      fixture.revision().value == 1 && fixture.session.document(document_id)
                                               ->curves()
                                               .front()
                                               .values.length() == 3,
      "append undo must restore original document revision and curve length");

  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "append must be redoable");
  require(fixture.revision().value == 2 &&
              fixture.session.document(document_id)
                      ->curves()
                      .front()
                      .values.length() == 5,
          "append redo must restore appended semantic state");

  require(fixture.session.execute(UndoCommand{document_id}).has_value(),
          "second append undo must create a redo entry");
  auto replacement_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1003.0});
  auto replacement_values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{45.0});
  const auto replacement = fixture.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks = {CurveTailBlock{
          .curve_id = curve_id,
          .sampling_axis_id = axis_id,
          .tail_coordinates = BufferView::from_vector(replacement_depths),
          .tail_values = BufferView::from_vector(replacement_values),
      }},
  });
  require(replacement.has_value(), "branch append must succeed");
  require(!fixture.session.can_redo(document_id),
          "a new visible append must clear redo history");
}

void existing_error_code_values_remain_stable() {
  require(static_cast<std::uint16_t>(ErrorCode::diagnostic_warning) == 19,
          "diagnostic_warning numeric value must remain stable");
  require(static_cast<std::uint16_t>(ErrorCode::history_empty) == 20,
          "history_empty numeric value must remain stable");
  require(static_cast<std::uint16_t>(ErrorCode::patch_conflict) == 21,
          "patch_conflict must append after existing stable error codes");
}

} // namespace

int main() {
  patch_round_trip_restores_each_semantic_revision();
  new_patch_clears_redo_history();
  selection_state_is_restored_with_document_revision();
  history_state_is_observable_through_accessors_and_events();
  append_commit_is_undoable_without_copying_old_semantics();
  existing_error_code_values_remain_stable();
  std::cout << "welllog.undo-redo: all cases passed\n";
  return EXIT_SUCCESS;
}
