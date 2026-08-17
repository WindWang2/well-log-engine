// Headless test for the shared semantic Selection Set in WellLogSession
// (ADR 0024, #154 Phase B). Asserts: a Reference Depth Range on a Sampling
// Axis maps to a half-open row span (increasing & decreasing axes); a row span
// maps back to a Reference Depth Range; a document replacement remaps the
// selection when the axis survives and the range still fits, and explicitly
// invalidates (publishing selection_invalidated) when the axis is gone or the
// range no longer fits; clear removes the selection; each change publishes a
// selection_changed event carrying the revision.

#include <welllog/core/document.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("66000000-0000-4000-8000-000000000001");
const auto axis_a_id = id("66000000-0000-4000-8000-000000000002");
const auto axis_b_id = id("66000000-0000-4000-8000-000000000003");
const auto curve_gr_id = id("66000000-0000-4000-8000-000000000004");
const auto curve_rhob_id = id("66000000-0000-4000-8000-000000000005");

// Axis A: increasing [1000, 1000.5, 1001, 1001.5, 1002]. A depth range over it
// resolves by lower/upper bound. Axis B (decreasing) is added by callers that
// need it.
WellLogSession make_session() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.5, 1001.0, 1001.5, 1002.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0, 50.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  WellLogSession session;
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "fixture document must be accepted");
  session.clear_events();
  return session;
}

// Depth range [1000.5, 1001.5] on the increasing axis [1000, 1000.5, 1001,
// 1001.5, 1002] must map to rows [1, 4): 1000.5<= → row 1; >1001.5 → row 4.
void depth_range_maps_to_row_span_increasing() {
  auto session = make_session();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.5, .bottom = 1001.5},
              })
              .has_value(),
          "depth-range selection must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "selection must be stored");
  require(sel->sampling_axis_id == axis_a_id, "axis id must be carried");
  require(sel->first_row == 1 && sel->last_row == 4,
          "depth range [1000.5,1001.5] must map to rows [1,4)");
  require(sel->document_revision == DocumentRevision{1},
          "selection must record the document revision");
  require(sel->valid, "a fresh selection must be valid");
  // The event carries the revision.
  require(!session.events().empty(), "selection change must publish an event");
  require(session.events().back().kind == ViewEventKind::selection_changed,
          "selection change must publish selection_changed");
  require(session.events().back().document_revision == DocumentRevision{1},
          "selection event must carry the revision");
}

// On a DEcreasing axis [1002, 1001.5, 1001, 1000.5, 1000], the same depth range
// [1000.5, 1001.5] must still resolve to the rows whose coordinates fall in it
// (rows 1..3 inclusive → [1,4)), just indexed against the reversed order.
void depth_range_maps_to_row_span_decreasing() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1002.0, 1001.5, 1001.0, 1000.5, 1000.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{50.0, 40.0, 30.0, 20.0, 10.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_b_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::decreasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_b_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  WellLogSession session;
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "decreasing-axis document must be accepted");
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_b_id,
                  .reference_depth_range = {.top = 1000.5, .bottom = 1001.5},
              })
              .has_value(),
          "decreasing-axis selection must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "selection must be stored");
  // Decreasing axis: 1001.5 is at row 1, 1000.5 at row 3 → span [1,4).
  require(sel->first_row == 1 && sel->last_row == 4,
          "decreasing depth range [1000.5,1001.5] must map to rows [1,4)");
}

// A row span maps back to a Reference Depth Range by reading the boundary
// coordinates. Rows [1,4) on the increasing axis → [1000.5, 1001.5].
void row_span_maps_to_depth_range() {
  auto session = make_session();
  require(session
              .execute(SetRowSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .first_row = 1,
                  .last_row = 4,
              })
              .has_value(),
          "row-span selection must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "row selection must be stored");
  require_near(sel->reference_depth_range.top, 1000.5,
               "row [1,4) top must be the coordinate at row 1");
  require_near(sel->reference_depth_range.bottom, 1001.5,
               "row [1,4) bottom must be the coordinate at row 3");
}

// A row→range→row round-trip is stable: setting rows then reading rows back
// yields the same span (the stored span is canonicalized via the range).
void row_to_range_to_row_round_trip() {
  auto session = make_session();
  require(session
              .execute(SetRowSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .first_row = 0,
                  .last_row = 5,
              })
              .has_value(),
          "full-span row selection must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "selection must be stored");
  require(sel->first_row == 0 && sel->last_row == 5,
          "full row span [0,5) must round-trip to [0,5)");
}

// A range entirely outside the axis extent yields an empty span (first==last).
void out_of_range_depth_yields_empty_span() {
  auto session = make_session();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 5000.0, .bottom = 6000.0},
              })
              .has_value(),
          "out-of-range selection must still be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "out-of-range selection must be stored");
  require(sel->first_row == sel->last_row,
          "a wholly out-of-range selection must resolve to an empty span");
}

// An invalid depth range (top > bottom) is rejected.
void invalid_range_is_rejected() {
  auto session = make_session();
  require(!session
               .execute(SetSelectionCommand{
                   .document_id = document_id,
                   .sampling_axis_id = axis_a_id,
                   .reference_depth_range = {.top = 1002.0, .bottom = 1000.0},
               })
               .has_value(),
           "an inverted range (top > bottom) must be rejected");
  require(!session.selection(document_id).has_value(),
          "a rejected selection must not be stored");
}

// An inverted row span (last < first) is rejected.
void inverted_row_span_is_rejected() {
  auto session = make_session();
  require(!session
               .execute(SetRowSelectionCommand{
                   .document_id = document_id,
                   .sampling_axis_id = axis_a_id,
                   .first_row = 3,
                   .last_row = 1,
               })
               .has_value(),
           "an inverted row span must be rejected");
}

// An unknown document or axis is rejected with DISTINCT error codes (Standards
// review): unknown document → document_not_found; unknown axis →
// missing_sampling_axis. The previous single invalid_viewport hid the cause.
void unknown_document_or_axis_rejected_with_distinct_codes() {
  auto session = make_session();
  const auto other_doc = id("66000000-0000-4000-8000-000000000099");
  const auto other_axis = id("66000000-0000-4000-8000-000000000098");
  const auto missing_doc = session.execute(SetSelectionCommand{
      .document_id = other_doc,
      .sampling_axis_id = axis_a_id,
      .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
  });
  require(!missing_doc.has_value(),
          "an unknown document id must be rejected");
  require(missing_doc.error().code == ErrorCode::document_not_found,
          "unknown document must return document_not_found");
  const auto missing_axis = session.execute(SetSelectionCommand{
      .document_id = document_id,
      .sampling_axis_id = other_axis,
      .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
  });
  require(!missing_axis.has_value(), "an unknown axis id must be rejected");
  require(missing_axis.error().code == ErrorCode::missing_sampling_axis,
          "unknown axis must return missing_sampling_axis");
  // An inverted range → invalid_viewport (the "bad value" code).
  const auto bad_range = session.execute(SetSelectionCommand{
      .document_id = document_id,
      .sampling_axis_id = axis_a_id,
      .reference_depth_range = {.top = 1002.0, .bottom = 1000.0},
  });
  require(!bad_range.has_value(), "an inverted range must be rejected");
  require(bad_range.error().code == ErrorCode::invalid_viewport,
          "a bad range must return invalid_viewport");
}

// ADR 0024 "one selection per document over a single Sampling Axis": selecting
// a DIFFERENT axis on the same document evicts the prior selection (the
// document holds exactly one selection). This locks the one-per-document
// intent (a Spec-review question — confirmed by the user).
void one_selection_per_document_evicts_other_axis() {
  // #764: the previous fixture only selected axis_a twice (same-axis
  // overwrite). Build a real two-axis document and evict A with B.
  auto depths_a = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.5, 1001.0, 1001.5, 1002.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0, 50.0});
  auto depths_b = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2000.0, 2000.5, 2001.0, 2001.5, 2002.0});
  auto rhob = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2.1, 2.2, 2.3, 2.4, 2.5});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths_a),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_b_id, .coordinates = BufferView::from_vector(depths_b),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  builder.add_curve(Curve{
      .id = curve_rhob_id, .mnemonic = "RHOB", .display_name = "Bulk Density",
      .unit = "g/cm3", .sampling_axis_id = axis_b_id,
      .values = BufferView::from_vector(rhob), .nulls = {}});
  WellLogSession session;
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "two-axis document must be accepted");
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1000.5},
              })
              .has_value(),
          "axis-A selection must be accepted");
  auto sel = session.selection(document_id);
  require(sel.has_value() && sel->sampling_axis_id == axis_a_id &&
              sel->first_row == 0 && sel->last_row == 2,
          "first selection must be on axis A, rows [0,2)");
  session.clear_events();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_b_id,
                  .reference_depth_range = {.top = 2001.0, .bottom = 2001.5},
              })
              .has_value(),
          "axis-B selection must be accepted");
  sel = session.selection(document_id);
  require(sel.has_value(), "the document still has exactly one selection");
  require(sel->sampling_axis_id == axis_b_id,
          "selecting axis B must evict the axis-A selection");
  require(sel->first_row == 2 && sel->last_row == 4,
          "axis-B range [2001,2001.5] must map to rows [2,4)");
  require(!session.events().empty() &&
              session.events().back().kind == ViewEventKind::selection_changed,
          "cross-axis eviction must publish selection_changed");
}

// Clear removes the selection and publishes selection_changed.
void clear_removes_selection() {
  auto session = make_session();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1001.0},
              })
              .has_value(),
          "selection must be accepted");
  require(session.selection(document_id).has_value(),
          "selection must be present before clear");
  session.clear_events();
  require(session
              .execute(ClearSelectionCommand{.document_id = document_id})
              .has_value(),
          "clear must be accepted");
  require(!session.selection(document_id).has_value(),
          "selection must be gone after clear");
  require(!session.events().empty() &&
              session.events().back().kind ==
                  ViewEventKind::selection_changed,
          "clear must publish selection_changed");
}

// A document replacement remaps the selection when the axis survives and the
// range still fits within the new axis extent.
void document_replacement_remaps_when_axis_survives() {
  auto session = make_session();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.5, .bottom = 1001.5},
              })
              .has_value(),
          "selection must be accepted");
  // Replace the document at a NEW revision with a FINER axis over the same id
  // (still [1000, 1002], now 0.25-step). The [1000.5,1001.5] range still fits.
  auto depths_v2 = std::make_shared<std::vector<double>>();
  for (int i = 0; i <= 8; ++i) {
    depths_v2->push_back(1000.0 + static_cast<double>(i) * 0.25);
  }
  auto depths = std::shared_ptr<const std::vector<double>>(std::move(depths_v2));
  auto gr = std::make_shared<const std::vector<double>>(depths->size(), 1.0);
  WellLogDocumentBuilder builder(document_id, DocumentRevision{2});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "replacement document must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "selection must survive a remappable replacement");
  require(sel->valid, "a remapped selection must stay valid");
  require(sel->document_revision == DocumentRevision{2},
          "remapped selection must record the NEW revision");
  // [1000.5,1001.5] on a 0.25-step axis → rows [2,7).
  require(sel->first_row == 2 && sel->last_row == 7,
          "remapped selection must resolve against the new axis coordinates");
  // An invalidation or change event was published (here: changed).
  bool saw_selection_event = false;
  for (const auto &ev : session.events()) {
    if (ev.kind == ViewEventKind::selection_changed ||
        ev.kind == ViewEventKind::selection_invalidated) {
      saw_selection_event = true;
      break;
    }
  }
  require(saw_selection_event,
          "replacement must publish a selection_changed/invalidated event");
}

// A document replacement that DROPS the selected axis (or whose new extent no
// longer contains the range) explicitly invalidates the selection and publishes
// selection_invalidated.
void document_replacement_invalidates_when_axis_gone() {
  auto session = make_session();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.5, .bottom = 1001.5},
              })
              .has_value(),
          "selection must be accepted");
  // Replacement document uses axis_b_id instead — axis_a_id is gone.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{2});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_b_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_b_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "replacement document must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "an invalidated selection is retained (as invalid)");
  require(!sel->valid, "a non-remappable replacement must invalidate selection");
  require(sel->document_revision == DocumentRevision{2},
          "invalidated selection must record the NEW revision");
  bool saw_invalidation = false;
  for (const auto &ev : session.events()) {
    if (ev.kind == ViewEventKind::selection_invalidated) {
      saw_invalidation = true;
      break;
    }
  }
  require(saw_invalidation,
          "a non-remappable replacement must publish selection_invalidated");
}

// A document replacement whose axis survives but whose new extent no longer
// contains the range also invalidates.
void document_replacement_invalidates_when_range_out_of_extent() {
  auto session = make_session();
  require(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1001.0, .bottom = 1002.0},
              })
              .has_value(),
          "selection must be accepted");
  // Replacement shrinks the axis to [1000, 1000.5] — the range no longer fits.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.5});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{2});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "shrunk-axis replacement must be accepted");
  const auto sel = session.selection(document_id);
  require(sel.has_value(), "invalidated selection is retained");
  require(!sel->valid,
          "a range outside the new extent must invalidate the selection");
}

} // namespace

int main() {
  depth_range_maps_to_row_span_increasing();
  depth_range_maps_to_row_span_decreasing();
  row_span_maps_to_depth_range();
  row_to_range_to_row_round_trip();
  out_of_range_depth_yields_empty_span();
  invalid_range_is_rejected();
  inverted_row_span_is_rejected();
  unknown_document_or_axis_rejected_with_distinct_codes();
  one_selection_per_document_evicts_other_axis();
  clear_removes_selection();
  document_replacement_remaps_when_axis_survives();
  document_replacement_invalidates_when_axis_gone();
  document_replacement_invalidates_when_range_out_of_extent();
  std::cout << "welllog.session-selection: all cases passed\n";
  return EXIT_SUCCESS;
}
