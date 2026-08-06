// Headless seam validation for #206 / #158 AC #8. A patched, undone and
// redone document must present one semantic state through WellLogSession's
// graphics, table, SVG and event seams. The project-save revision remains
// host-owned (ADR 0025), while the session owns document revision and history.

#include <welllog/core/document.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

std::optional<double> svg_number_attribute(std::string_view svg,
                                           std::string_view attribute) {
  const auto value_begin = svg.find(attribute);
  if (value_begin == std::string_view::npos) {
    return std::nullopt;
  }
  const auto number_begin = value_begin + attribute.size();
  const auto number_end = svg.find('"', number_begin);
  if (number_end == std::string_view::npos) {
    return std::nullopt;
  }
  double value{};
  const auto parsed = std::from_chars(svg.data() + number_begin,
                                      svg.data() + number_end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != svg.data() + number_end) {
    return std::nullopt;
  }
  return value;
}

std::string svg_color(RgbaColor color) {
  std::string text{"#000000"};
  const auto write_component = [&text](std::size_t offset,
                                       std::uint8_t component) {
    constexpr std::string_view hex_digits = "0123456789abcdef";
    text[offset] = hex_digits[component >> 4U];
    text[offset + 1] = hex_digits[component & 0x0fU];
  };
  write_component(1, color.red);
  write_component(3, color.green);
  write_component(5, color.blue);
  return text;
}

std::optional<std::string_view> svg_element(std::string_view svg,
                                            std::string_view opening_tag) {
  const auto element_begin = svg.find(opening_tag);
  if (element_begin == std::string_view::npos) {
    return std::nullopt;
  }
  const auto element_end = svg.find("/>", element_begin);
  if (element_end == std::string_view::npos) {
    return std::nullopt;
  }
  return svg.substr(element_begin, element_end - element_begin + 2);
}

std::optional<double> svg_path_first_x(std::string_view path) {
  constexpr std::string_view path_start = "d=\"M ";
  const auto value_begin = path.find(path_start);
  if (value_begin == std::string_view::npos) {
    return std::nullopt;
  }
  double value{};
  const auto parsed = std::from_chars(
      path.data() + value_begin + path_start.size(),
      path.data() + path.size(), value);
  if (parsed.ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("ce000000-0000-4000-8000-000000000001");
const auto axis_id = id("ce000000-0000-4000-8000-000000000002");
const auto curve_id = id("ce000000-0000-4000-8000-000000000003");
const auto track_id = id("ce000000-0000-4000-8000-000000000004");
const auto scale_id = id("ce000000-0000-4000-8000-000000000005");
const auto curve_layer_id = id("ce000000-0000-4000-8000-000000000006");
const auto interval_id = id("ce000000-0000-4000-8000-000000000007");
const auto marker_id = id("ce000000-0000-4000-8000-000000000008");
const auto interval_layer_id = id("ce000000-0000-4000-8000-000000000009");
const auto marker_layer_id = id("ce000000-0000-4000-8000-000000000010");

struct Fixture {
  WellLogSession session;

  Fixture() {
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
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
    document_builder.add_interval(Interval{
        .id = interval_id,
        .top_reference_depth = 1000.0,
        .bottom_reference_depth = 1001.0,
        .semantic = IntervalSemantic::lithology,
        .pattern_id = {},
        .fill_color = {255, 0, 0, 255},
        .label = "Sand",
    });
    document_builder.add_marker(Marker{
        .id = marker_id,
        .reference_depth = 1000.5,
        .semantic = MarkerSemantic::formation_top,
        .label = "Top A",
    });
    require(session.execute(SetDocumentCommand{document_builder.build()})
                .has_value(),
            "fixture document must be accepted");

    ScenePresentationBuilder presentation_builder(
        document_id,
        ReferenceDepthRange{
            .domain = DepthDomain::measured_depth,
            .unit = "m",
            .top = 1000.0,
            .bottom = 1003.0,
        },
        Millimetres{500.0}, "fixture-font");
    presentation_builder.add_track(
        TrackSpec{.id = track_id, .width = Millimetres{40.0}});
    presentation_builder.add_scale(TrackScaleSpec{
        .id = scale_id,
        .track_id = track_id,
        .mode = ScaleMode::linear,
        .minimum = 0.0,
        .maximum = 100.0,
        .unit = "API",
    });
    presentation_builder.add_curve_layer(CurveLayerSpec{
        .id = curve_layer_id,
        .track_id = track_id,
        .curve_id = curve_id,
        .scale_id = scale_id,
        .color = {0, 0, 0, 255},
        .line_width = Millimetres{0.25},
        .visible = true,
    });
    presentation_builder.add_interval_layer(
        IntervalLayerSpec{.id = interval_layer_id, .track_id = track_id});
    presentation_builder.add_marker_layer(
        MarkerLayerSpec{.id = marker_layer_id, .track_id = track_id});
    require(session.execute(SetPresentationCommand{presentation_builder.build()})
                .has_value(),
            "fixture presentation must be accepted");
    session.clear_events();
  }
};

struct SeamExpectation {
  DocumentRevision revision;
  double track_width;
  double first_curve_point_left;
  double interval_top;
  double interval_bottom;
  double marker_depth;
  RgbaColor curve_color;
  double curve_line_width;
};

std::shared_ptr<const PreparedScene>
await_prepared_scene(WellLogSession &session, DocumentRevision revision) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    session.poll_async();
    const auto scene = session.prepared_scene(document_id);
    if (scene != nullptr && scene->document_revision() == revision) {
      return scene;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return session.prepared_scene(document_id);
}

void require_all_output_seams(Fixture &fixture,
                              const SeamExpectation &expected) {
  const auto document = fixture.session.document(document_id);
  require(document != nullptr, "session document seam must expose a document");
  require(document->revision() == expected.revision,
          "document seam must expose the expected revision");

  const auto scene = await_prepared_scene(fixture.session, expected.revision);
  require(scene != nullptr, "graphics seam must expose a prepared scene");
  require(scene->document_revision() == expected.revision,
          "prepared scene must carry the document revision");
  require_near(scene->physical_width().value, expected.track_width,
               "prepared scene must use the patched track width");

  const auto prepared_track =
      std::find_if(scene->tracks().begin(), scene->tracks().end(),
                   [](const PreparedTrack &track) { return track.id == track_id; });
  require(prepared_track != scene->tracks().end(),
          "prepared scene must retain the track");
  require_near(prepared_track->bounds.width.value, expected.track_width,
               "prepared track geometry must reflect the semantic state");

  const auto prepared_curve = std::find_if(
      scene->curve_layers().begin(), scene->curve_layers().end(),
      [](const PreparedCurveLayer &layer) { return layer.id == curve_layer_id; });
  require(prepared_curve != scene->curve_layers().end(),
          "prepared scene must retain the curve layer");
  require(prepared_curve->color == expected.curve_color,
          "prepared curve layer must reflect the patched style");
  require_near(prepared_curve->line_width.value, expected.curve_line_width,
               "prepared curve layer must reflect the patched line width");
  require(prepared_curve->segment_count == 1,
          "fixture curve must produce one prepared segment");
  require(prepared_curve->first_segment < scene->curve_segments().size(),
          "prepared curve layer must reference an in-range segment");
  const auto &segment = scene->curve_segments()[static_cast<std::size_t>(
      prepared_curve->first_segment)];
  require(segment.first_point < scene->curve_points().size(),
          "prepared curve segment must reference an in-range point");
  const auto &curve_point =
      scene->curve_points()[static_cast<std::size_t>(segment.first_point)];
  require_near(curve_point.position.left.value, expected.first_curve_point_left,
               "prepared curve geometry must reflect the patched track width");

  const auto prepared_interval = std::find_if(
      scene->intervals().begin(), scene->intervals().end(),
      [](const PreparedInterval &interval) { return interval.interval_id == interval_id; });
  require(prepared_interval != scene->intervals().end(),
          "prepared scene must retain the interval");
  require_near(prepared_interval->top_reference_depth, expected.interval_top,
               "prepared interval must reflect its semantic top depth");
  require_near(prepared_interval->bottom_reference_depth, expected.interval_bottom,
               "prepared interval must reflect its semantic bottom depth");

  const auto prepared_marker = std::find_if(
      scene->markers().begin(), scene->markers().end(),
      [](const PreparedMarker &marker) { return marker.marker_id == marker_id; });
  require(prepared_marker != scene->markers().end(),
          "prepared scene must retain the marker");
  require_near(prepared_marker->reference_depth, expected.marker_depth,
               "prepared marker must reflect its semantic depth");

  const auto tables = TableProjectionBuilder::from_document(*document);
  require(tables.size() == 1, "table seam must expose one axis projection");
  const auto &table = tables.front();
  require(table.document_id() == document_id,
          "table projection must identify the session document");
  require(table.document_revision() == expected.revision,
          "table projection must never retain a stale document revision");
  require(table.row_count() == 4 && table.column_count() == 2,
          "table projection must retain the raw curve shape");
  const auto raw_curve_cell = table.cell(2, 1);
  require(raw_curve_cell.value.has_value(), "table cell must expose raw data");
  require_near(*raw_curve_cell.value, 30.0,
               "table must keep reading immutable raw curve data, not LOD");

  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value(), "SVG seam must export the prepared scene");
  const std::string svg_text(svg.value().text());
  const auto revision_attribute =
      "data-document-revision=\"" + std::to_string(expected.revision.value) + "\"";
  const auto width_attribute =
      "width=\"" + std::to_string(static_cast<std::uint64_t>(expected.track_width)) +
      "mm\"";
  require(svg_text.find(revision_attribute) != std::string::npos,
          "SVG snapshot must carry the same document revision");
  require(svg_text.find(width_attribute) != std::string::npos,
          "SVG snapshot must reflect the prepared track width");
  const auto interval_top_attribute =
      "data-top-depth=\"" + std::to_string(static_cast<std::uint64_t>(expected.interval_top)) +
      "\"";
  const auto interval_bottom_attribute =
      "data-bottom-depth=\"" +
      std::to_string(static_cast<std::uint64_t>(expected.interval_bottom)) + "\"";
  require(svg_text.find(interval_top_attribute) != std::string::npos &&
              svg_text.find(interval_bottom_attribute) != std::string::npos,
          "SVG snapshot must reflect the interval geometry");
  const auto marker_depth =
      svg_number_attribute(svg_text, "data-reference-depth=\"");
  require(marker_depth.has_value() &&
              std::abs(*marker_depth - expected.marker_depth) < 1.0e-9,
          "SVG snapshot must reflect the marker geometry");

  const auto curve_opening_tag =
      "<path id=\"layer-" + curve_layer_id.to_string() + "\"";
  const auto curve_element = svg_element(svg_text, curve_opening_tag);
  require(curve_element.has_value(),
          "SVG snapshot must retain the curve-layer element");
  const auto expected_stroke = "stroke=\"" + svg_color(expected.curve_color) + "\"";
  require(curve_element->find(expected_stroke) != std::string_view::npos,
          "SVG snapshot must reflect the curve-layer style");
  const auto curve_line_width =
      svg_number_attribute(*curve_element, "stroke-width=\"");
  require(curve_line_width.has_value(),
          "SVG curve-layer element must expose its line width");
  require_near(*curve_line_width, expected.curve_line_width,
               "SVG snapshot must reflect the curve-layer line width");
  const auto curve_first_x = svg_path_first_x(*curve_element);
  require(curve_first_x.has_value(),
          "SVG curve-layer element must expose path geometry");
  require_near(*curve_first_x, expected.first_curve_point_left,
               "SVG snapshot must reflect the curve-layer geometry");
}

std::optional<std::size_t>
event_index(std::span<const ViewEvent> events, std::size_t first,
            std::size_t last, ViewEventKind kind, DocumentRevision revision) {
  for (auto index = first; index < last; ++index) {
    const auto &event = events[index];
    if (event.kind == kind && event.document_id == document_id &&
        event.document_revision == revision) {
      return index;
    }
  }
  return std::nullopt;
}

void require_transition_events(std::span<const ViewEvent> events,
                               std::size_t first, std::size_t last,
                               DocumentRevision revision) {
  const auto document_changed = event_index(
      events, first, last, ViewEventKind::documents_changed, revision);
  const auto selection_changed = event_index(
      events, first, last, ViewEventKind::selection_changed, revision);
  const auto history_changed = event_index(
      events, first, last, ViewEventKind::history_changed, revision);
  require(document_changed.has_value() && selection_changed.has_value() &&
              history_changed.has_value(),
          "each history transition must publish document, selection and history events");
  require(*document_changed < *selection_changed &&
              *selection_changed < *history_changed,
          "history transition events must publish document, selection, then history");
}

void require_monotonic_event_versions(std::span<const ViewEvent> events) {
  for (std::size_t index = 1; index < events.size(); ++index) {
    require(events[index - 1].state_version <= events[index].state_version,
            "ViewEvent state versions must be monotonically ordered");
  }
}

void patch_undo_redo_remain_consistent_through_every_session_seam() {
  Fixture fixture;
  DocumentRevision project_saved_revision{1};

  require(fixture.session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
              })
              .has_value(),
          "fixture selection must be accepted");
  fixture.session.clear_events();
  require_all_output_seams(
      fixture,
      SeamExpectation{
          .revision = DocumentRevision{1},
          .track_width = 40.0,
          .first_curve_point_left = 4.0,
          .interval_top = 1000.0,
          .interval_bottom = 1001.0,
          .marker_depth = 1000.5,
          .curve_color = {0, 0, 0, 255},
          .curve_line_width = 0.25,
      });

  const auto patch_events_begin = fixture.session.events().size();
  const auto patch = fixture.session.execute(ApplyPatchCommand{
      .document_id = document_id,
      .patch =
          DocumentPatch{
              .base_revision = DocumentRevision{1},
              .edits =
                  {
                      EntityEdit{UpsertEntity{TrackSpec{
                          .id = track_id,
                          .width = Millimetres{80.0},
                      }}},
                      EntityEdit{UpsertEntity{CurveLayerSpec{
                          .id = curve_layer_id,
                          .track_id = track_id,
                          .curve_id = curve_id,
                          .scale_id = scale_id,
                          .color = {18, 52, 86, 255},
                          .line_width = Millimetres{1.5},
                          .visible = true,
                      }}},
                      EntityEdit{UpsertEntity{Interval{
                          .id = interval_id,
                          .top_reference_depth = 1001.0,
                          .bottom_reference_depth = 1002.0,
                          .semantic = IntervalSemantic::lithology,
                          .pattern_id = {},
                          .fill_color = {255, 0, 0, 255},
                          .label = "Sand",
                      }}},
                      EntityEdit{UpsertEntity{Marker{
                          .id = marker_id,
                          .reference_depth = 1002.5,
                          .semantic = MarkerSemantic::formation_top,
                          .label = "Top A",
                      }}},
                  },
          },
  });
  require(patch.has_value(), "patch must succeed");
  require(patch.value().document_revision == DocumentRevision{2},
          "patch must commit the next document revision");
  const auto patch_events_end = fixture.session.events().size();
  require_transition_events(fixture.session.events(), patch_events_begin,
                            patch_events_end, DocumentRevision{2});
  require(project_saved_revision == DocumentRevision{1},
          "a patch must not claim that the host saved the project");
  require(fixture.session.can_undo(document_id) &&
              !fixture.session.can_redo(document_id),
          "patch commit must expose undo-only history");
  require_all_output_seams(
      fixture,
      SeamExpectation{
          .revision = DocumentRevision{2},
          .track_width = 80.0,
          .first_curve_point_left = 8.0,
          .interval_top = 1001.0,
          .interval_bottom = 1002.0,
          .marker_depth = 1002.5,
          .curve_color = {18, 52, 86, 255},
          .curve_line_width = 1.5,
      });

  const auto undo_events_begin = fixture.session.events().size();
  require(fixture.session.execute(UndoCommand{document_id}).has_value(),
          "undo must succeed");
  const auto undo_events_end = fixture.session.events().size();
  require_transition_events(fixture.session.events(), undo_events_begin,
                            undo_events_end, DocumentRevision{1});
  require(fixture.session.document(document_id)->revision() ==
              project_saved_revision,
          "undo can restore the host-saved document revision");
  require(!fixture.session.can_undo(document_id) &&
              fixture.session.can_redo(document_id),
          "project-saved state and redo history must remain distinguishable");
  require_all_output_seams(
      fixture,
      SeamExpectation{
          .revision = DocumentRevision{1},
          .track_width = 40.0,
          .first_curve_point_left = 4.0,
          .interval_top = 1000.0,
          .interval_bottom = 1001.0,
          .marker_depth = 1000.5,
          .curve_color = {0, 0, 0, 255},
          .curve_line_width = 0.25,
      });

  const auto redo_events_begin = fixture.session.events().size();
  require(fixture.session.execute(RedoCommand{document_id}).has_value(),
          "redo must succeed");
  const auto redo_events_end = fixture.session.events().size();
  require_transition_events(fixture.session.events(), redo_events_begin,
                            redo_events_end, DocumentRevision{2});
  require(fixture.session.document(document_id)->revision() !=
              project_saved_revision,
          "redone edits must remain unsaved until the host records the revision");
  require(fixture.session.can_undo(document_id) &&
              !fixture.session.can_redo(document_id),
          "redo must restore undo-only history");
  require_all_output_seams(
      fixture,
      SeamExpectation{
          .revision = DocumentRevision{2},
          .track_width = 80.0,
          .first_curve_point_left = 8.0,
          .interval_top = 1001.0,
          .interval_bottom = 1002.0,
          .marker_depth = 1002.5,
          .curve_color = {18, 52, 86, 255},
          .curve_line_width = 1.5,
      });

  project_saved_revision = fixture.session.document(document_id)->revision();
  require(project_saved_revision == DocumentRevision{2} &&
              fixture.session.can_undo(document_id),
          "the host-saved revision must be distinct from undo history");

  require_monotonic_event_versions(fixture.session.events());
}

} // namespace

int main() {
  patch_undo_redo_remain_consistent_through_every_session_seam();
  std::cout << "welllog.session-seam-validation: all cases passed\n";
  return EXIT_SUCCESS;
}
