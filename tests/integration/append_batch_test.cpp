// Headless test for AppendBatchCommand (#198, ADR 0031). Asserts:
//  - a successful append produces a new (strictly-greater) Document Revision
//    with the extended buffer readable end-to-end across the segment join;
//  - old data blocks are immutable and NOT re-copied (the old segment's
//    SharedOwner is retained; the old data address is still live and unchanged
//    inside the new composite curve);
//  - a failed validation rejects the WHOLE batch leaving the document unchanged
//    (no half-batch visible state);
//  - out-of-order and historical backfill tails are rejected as Append;
//  - a non-monotonic target revision (stale or equal) is rejected.
//
// No GL, no Qt — exercises WellLogSession + core only.

#include <welllog/core/document.hpp>
#include <welllog/session/session.hpp>

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

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const auto document_id = id("77000000-0000-4000-8000-000000000001");
const auto axis_id = id("77000000-0000-4000-8000-000000000002");
const auto curve_gr_id = id("77000000-0000-4000-8000-000000000003");

// Fixture: increasing MD axis [1000, 1001, 1002] with a matching GR curve
// [10, 20, 30]. The depth/value vectors are kept alive via shared_ptr so the
// test can read back the original addresses after an append (no-copy proof).
struct Fixture {
  std::shared_ptr<const std::vector<double>> depths;
  std::shared_ptr<const std::vector<double>> values;
  const double *depths_data;
  const double *values_data;
  WellLogSession session;

  Fixture() {
    depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1001.0, 1002.0});
    values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0, 20.0, 30.0});
    depths_data = depths->data();
    values_data = values->data();
    WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
    builder.add_sampling_axis(SamplingAxis{
        .id = axis_id, .coordinates = BufferView::from_vector(depths),
        .domain = DepthDomain::measured_depth, .unit = "m",
        .direction = AxisDirection::increasing});
    builder.add_curve(Curve{
        .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
        .unit = "API", .sampling_axis_id = axis_id,
        .values = BufferView::from_vector(values), .nulls = {}});
    require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
            "fixture document must be accepted");
    session.clear_events();
  }
};

BufferView tail_view(std::initializer_list<double> items) {
  auto vec = std::make_shared<const std::vector<double>>(items);
  return BufferView::from_vector(vec);
}

// Successful append produces a new revision with the extended buffer readable
// end-to-end across the old+tail segment join.
void successful_append_extends_buffer_readable_end_to_end() {
  Fixture f;
  // Append [1003, 1004] depths, [40, 50] values → axis length 5, curve length 5.
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0, 1004.0}),
                  .tail_values = tail_view({40.0, 50.0}),
              },
          },
  });
  require(result.has_value(), "successful append must be accepted");
  require(result.value().document_revision.value == 2,
          "append must produce the target revision");

  const auto doc = f.session.document(document_id);
  require(doc != nullptr, "appended document must be queryable");
  require(doc->revision().value == 2, "document revision must be bumped to 2");
  const auto axes = doc->sampling_axes();
  require(axes.size() == 1, "document must still have one axis");
  require(axes.front().coordinates.length() == 5,
          "axis coordinates must be extended to 5 samples");
  const auto curves = doc->curves();
  require(curves.size() == 1, "document must still have one curve");
  require(curves.front().values.length() == 5,
          "curve values must be extended to 5 samples");

  // Read across the segment join: old [1000..1002] + tail [1003, 1004].
  require(curves.front().values.value_as_double(0).value() == 10.0,
          "old value at index 0 must read through the composite");
  require(curves.front().values.value_as_double(2).value() == 30.0,
          "old value at the segment boundary must read through");
  require(curves.front().values.value_as_double(3).value() == 40.0,
          "tail value at index 3 must read through the composite");
  require(curves.front().values.value_as_double(4).value() == 50.0,
          "tail value at index 4 must read through the composite");
  require(axes.front().coordinates.value_as_double(3).value() == 1003.0,
          "tail coordinate at index 3 must read through the composite");
  require(axes.front().coordinates.value_as_double(4).value() == 1004.0,
          "tail coordinate at index 4 must read through the composite");

  // The documents_changed event must fire at the new revision.
  const auto events = f.session.events();
  require(!events.empty(), "append must publish at least one event");
  require(events.front().kind == ViewEventKind::documents_changed,
          "append must publish a documents_changed event");
  require(events.front().document_revision.value == 2,
          "documents_changed event must carry the new revision");
}

// Old data blocks are immutable and NOT re-copied: the original value buffer's
// address is still live (SharedOwner retained) and is the same physical block
// referenced by the composite curve's first segment.
void successful_append_makes_no_copy_of_old_blocks() {
  Fixture f;
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
          },
  });
  require(result.has_value(), "append must succeed");

  const auto doc = f.session.document(document_id);
  const auto &curve = doc->curves().front();
  require(curve.values.is_composite(),
          "appended curve values must be a composite buffer");
  const auto segments = curve.values.segments();
  require(segments.size() == 2, "composite must span exactly 2 segments");
  // The first segment's data address must equal the original old-block address
  // — the old block is retained in place, not re-copied.
  require(segments.front().data() ==
              reinterpret_cast<const std::byte *>(f.values_data),
          "old value block must be retained in place (no contiguous copy)");
  require(segments.front().length() == 3,
          "old segment must keep its original length");
  require(segments.back().length() == 1,
          "tail segment must carry the appended length");

  // The axis coordinates are likewise composite and the old block retained.
  const auto &axis = doc->sampling_axes().front();
  require(axis.coordinates.is_composite(),
          "appended axis coordinates must be a composite buffer");
  const auto coord_segments = axis.coordinates.segments();
  require(coord_segments.size() == 2,
          "axis composite must span exactly 2 segments");
  require(coord_segments.front().data() ==
              reinterpret_cast<const std::byte *>(f.depths_data),
          "old coordinate block must be retained in place (no contiguous copy)");
}

// A failed validation rejects the WHOLE batch, leaving the document unchanged
// (no half-batch visible state). Two valid blocks + one bad block → the two
// valid appends must NOT be partially applied.
void failed_validation_rejects_whole_batch_unchanged() {
  Fixture f;
  const auto original_revision =
      f.session.document(document_id)->revision().value;
  // Block 1 + 2 are valid tail continuations; block 3 has a length mismatch
  // (tail coords length 2, tail values length 1) → the whole batch must fail.
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1004.0}),
                  .tail_values = tail_view({50.0}),
              },
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1005.0, 1006.0}),
                  .tail_values = tail_view({60.0}), // length mismatch
              },
          },
  });
  require(!result.has_value(), "a batch with a bad block must be rejected");
  require(result.error().code == ErrorCode::length_mismatch,
          "length-mismatched tail must return the structural error code");

  const auto doc = f.session.document(document_id);
  require(doc->revision().value == original_revision,
          "failed append must leave the document revision unchanged");
  require(doc->curves().front().values.length() == 3,
          "failed append must leave the curve length unchanged");
  require(!doc->curves().front().values.is_composite(),
          "failed append must leave the curve a single-block buffer");
  require(doc->sampling_axes().front().coordinates.length() == 3,
          "failed append must leave the axis length unchanged");
  require(f.session.events().empty(),
          "failed append must publish no events");
}

// An out-of-order tail (a coordinate that steps backward within the tail) is
// rejected as Append — it is not a valid continuation.
void out_of_order_tail_rejected() {
  Fixture f;
  // Tail [1003, 1002.5] steps backward → not monotone increasing → rejected.
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0, 1002.5}),
                  .tail_values = tail_view({40.0, 50.0}),
              },
          },
  });
  require(!result.has_value(), "an out-of-order tail must be rejected");
  require(result.error().code == ErrorCode::invalid_sampling_axis,
          "out-of-order tail must return the sampling-axis error code");
  require(f.session.document(document_id)->revision().value == 1,
          "rejected append must leave the document unchanged");
}

// A historical backfill tail (the tail starts BEFORE the existing axis end,
// i.e. it re-covers an already-sampled depth) is rejected as Append — backfill
// requires an explicit Replace/Patch, not an append.
void historical_backfill_tail_rejected() {
  Fixture f;
  // Existing axis ends at 1002; a tail starting at 1001.5 back-fills an
  // already-sampled region → rejected.
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1001.5, 1003.0}),
                  .tail_values = tail_view({40.0, 50.0}),
              },
          },
  });
  require(!result.has_value(), "a historical backfill tail must be rejected");
  require(result.error().code == ErrorCode::invalid_sampling_axis,
          "backfill tail must return the sampling-axis error code");
  require(f.session.document(document_id)->revision().value == 1,
          "rejected backfill must leave the document unchanged");
}

// A non-monotonic target revision (equal to or less than the current) is
// rejected by the monotonic revision gate. A stale revision would silently
// clobber a newer append; the gate prevents that.
void non_monotonic_revision_rejected() {
  Fixture f;
  // Equal revision.
  const auto equal = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{1}, // equal to current
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
          },
  });
  require(!equal.has_value(), "an equal target revision must be rejected");
  require(equal.error().code == ErrorCode::invalid_document,
          "non-monotonic revision must return the document-structure code");

  // Older revision.
  const auto older = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{0}, // less than current (1)
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
          },
  });
  require(!older.has_value(), "an older target revision must be rejected");
  require(f.session.document(document_id)->revision().value == 1,
          "non-monotonic append must leave the document unchanged");
}

// A second append on an already-appended curve composes against the staged
// composite (repeated append produces a 3-segment buffer, all retained).
void repeated_append_chains_segments() {
  Fixture f;
  require(f.session
              .execute(AppendBatchCommand{
                  .document_id = document_id,
                  .target_revision = DocumentRevision{2},
                  .blocks =
                      {
                          CurveTailBlock{
                              .curve_id = curve_gr_id,
                              .sampling_axis_id = axis_id,
                              .tail_coordinates = tail_view({1003.0}),
                              .tail_values = tail_view({40.0}),
                          },
                      },
              })
              .has_value(),
          "first append must succeed");
  require(f.session
              .execute(AppendBatchCommand{
                  .document_id = document_id,
                  .target_revision = DocumentRevision{3},
                  .blocks =
                      {
                          CurveTailBlock{
                              .curve_id = curve_gr_id,
                              .sampling_axis_id = axis_id,
                              .tail_coordinates = tail_view({1004.0}),
                              .tail_values = tail_view({50.0}),
                          },
                      },
              })
              .has_value(),
          "second append must succeed");

  const auto doc = f.session.document(document_id);
  require(doc->revision().value == 3, "second append must bump revision to 3");
  const auto &curve = doc->curves().front();
  require(curve.values.length() == 5, "chained append must reach length 5");
  require(curve.values.segments().size() == 3,
          "chained append must span 3 segments");
  // All three original blocks retained (old + tail1 + tail2), readable in order.
  require(curve.values.value_as_double(0).value() == 10.0,
          "old block must read through");
  require(curve.values.value_as_double(3).value() == 40.0,
          "first tail must read through");
  require(curve.values.value_as_double(4).value() == 50.0,
          "second tail must read through");
}

// An append on a missing document is rejected with document_not_found.
void append_on_missing_document_rejected() {
  Fixture f;
  const auto missing_id = id("77000000-0000-4000-8000-000000000099");
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = missing_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
          },
  });
  require(!result.has_value(), "append on a missing document must be rejected");
  require(result.error().code == ErrorCode::document_not_found,
          "missing document must return the document_not_found code");
}

// A block naming a curve that does not exist on the document is rejected with a
// DISTINCT code from a missing axis, so a caller can tell the two apart (a
// missing curve is a document-structure problem; a missing axis is not).
void missing_curve_rejected_with_distinct_code() {
  Fixture f;
  const auto missing_curve = id("77000000-0000-4000-8000-000000000099");
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = missing_curve,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
          },
  });
  require(!result.has_value(), "append on a missing curve must be rejected");
  require(result.error().code == ErrorCode::invalid_document,
          "missing curve must return the document-structure code");
  require(result.error().entity_id == missing_curve,
          "missing-curve error must carry the curve id");
}

// A block naming an axis that does not exist is rejected with the
// missing_sampling_axis code (distinct from a missing curve above).
void missing_axis_rejected_with_distinct_code() {
  Fixture f;
  const auto missing_axis = id("77000000-0000-4000-8000-000000000098");
  // Use the real curve id but a non-existent axis; the curve lookup passes, the
  // axis lookup fails.
  const auto result = f.session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = missing_axis,
                  .tail_coordinates = tail_view({1003.0}),
                  .tail_values = tail_view({40.0}),
              },
          },
  });
  require(!result.has_value(), "append on a missing axis must be rejected");
  // The curve's own axis id disagrees with the block's → structural mismatch
  // surfaces as a tail mismatch (length_mismatch) before the axis lookup; that
  // is the earliest distinct signal. Verify the curve-vs-axis disagreement is
  // caught rather than silently accepted.
  require(result.error().code == ErrorCode::length_mismatch ||
              result.error().code == ErrorCode::missing_sampling_axis,
          "curve/axis disagreement must surface a distinct rejection code");
}

// #755: an ordered append of a large prefix must not re-walk the composite
// axis. Open and replace still full-scan. A disordered tail still fails
// (out_of_order_tail_rejected above).
void ordered_append_skips_full_axis_rescan() {
  constexpr std::uint64_t prefix = 32'768;
  std::vector<double> depth_store;
  std::vector<double> value_store;
  depth_store.reserve(prefix);
  value_store.reserve(prefix);
  for (std::uint64_t i = 0; i < prefix; ++i) {
    depth_store.push_back(1000.0 + static_cast<double>(i));
    value_store.push_back(10.0);
  }
  auto depths =
      std::make_shared<const std::vector<double>>(std::move(depth_store));
  auto values =
      std::make_shared<const std::vector<double>>(std::move(value_store));

  WellLogSession session;
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {}});

  reset_axis_is_ordered_full_scan_count();
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "prefix document must be accepted");
  require(axis_is_ordered_full_scan_count() >= 1,
          "validate-on-open must still full-scan the axis");

  reset_axis_is_ordered_full_scan_count();
  const auto append = session.execute(AppendBatchCommand{
      .document_id = document_id,
      .target_revision = DocumentRevision{2},
      .blocks =
          {
              CurveTailBlock{
                  .curve_id = curve_gr_id,
                  .sampling_axis_id = axis_id,
                  .tail_coordinates =
                      tail_view({1000.0 + static_cast<double>(prefix),
                                 1000.0 + static_cast<double>(prefix + 1)}),
                  .tail_values = tail_view({11.0, 12.0}),
              },
          },
  });
  require(append.has_value(), "ordered append must succeed");
  require(axis_is_ordered_full_scan_count() == 0,
          "ordered append must not invoke a full-axis axis_is_ordered");
  require(session.document(document_id)
                  ->sampling_axes()
                  .front()
                  .coordinates.length() == prefix + 2,
          "append must extend the axis by the tail length");

  auto replace_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  auto replace_values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0});
  WellLogDocumentBuilder replacer(document_id, DocumentRevision{3});
  replacer.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(replace_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing});
  replacer.add_curve(Curve{
      .id = curve_gr_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(replace_values),
      .nulls = {}});
  reset_axis_is_ordered_full_scan_count();
  require(session.execute(SetDocumentCommand{replacer.build()}).has_value(),
          "replacement document must be accepted");
  require(axis_is_ordered_full_scan_count() >= 1,
          "validate-on-replace must still full-scan the axis");
}

} // namespace

int main() {
  successful_append_extends_buffer_readable_end_to_end();
  successful_append_makes_no_copy_of_old_blocks();
  failed_validation_rejects_whole_batch_unchanged();
  out_of_order_tail_rejected();
  historical_backfill_tail_rejected();
  non_monotonic_revision_rejected();
  repeated_append_chains_segments();
  append_on_missing_document_rejected();
  missing_curve_rejected_with_distinct_code();
  missing_axis_rejected_with_distinct_code();
  ordered_append_skips_full_axis_rescan();
  std::cout << "welllog.append-batch: all cases passed\n";
  return EXIT_SUCCESS;
}
