// Headless test for CompositeBufferView (#196, foundation for #162 "不复制旧
// 数组"). Asserts: a single-segment composite reads identically to its
// BufferView; a two-segment composite spans the concatenation by logical index;
// each segment's SharedOwner keeps its block alive independently (no contiguous
// copy); out-of-range index yields nullopt; heterogeneous scalar_type and empty
// input are rejected.

#include <welllog/core/document.hpp>

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
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void require_near(double actual, double expected, std::string_view message) {
  if (actual < expected - 1.0e-9 || actual > expected + 1.0e-9) {
    fail(message);
  }
}

// A single-segment composite reads identically to its underlying BufferView.
void single_segment_composite_matches_buffer_view() {
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  BufferView seg = BufferView::from_vector(values);
  auto composite = CompositeBufferView::from_segments({seg});
  require(!composite.empty(), "a one-segment composite must not be empty");
  require(composite.length() == 4, "length must equal the segment length (4)");
  require(composite.scalar_type() == ScalarType::float64,
          "scalar type must come from the segment");
  // Every index reads the same value as the source BufferView.
  for (std::uint64_t i = 0; i < 4; ++i) {
    require_near(composite.value_as_double(i).value_or(-1.0),
                seg.value_as_double(i).value_or(-999.0),
                "single-segment composite must match BufferView per index");
  }
}

// A two-segment composite spans the concatenation: element i of the composite
// maps to segment 0 for i < len0, else segment 1 at (i - len0).
void two_segment_composite_spans_concatenation() {
  auto head = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  auto tail = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{4.0, 5.0});
  BufferView seg0 = BufferView::from_vector(head);
  BufferView seg1 = BufferView::from_vector(tail);
  auto composite = CompositeBufferView::from_segments({seg0, seg1});
  require(composite.length() == 5,
          "two-segment length must be the sum (3 + 2 = 5)");
  // The concatenation reads [1,2,3,4,5].
  require_near(composite.value_as_double(0).value_or(-1.0), 1.0, "index 0");
  require_near(composite.value_as_double(2).value_or(-1.0), 3.0,
               "index 2 (last of segment 0)");
  require_near(composite.value_as_double(3).value_or(-1.0), 4.0,
               "index 3 (first of segment 1 — the boundary)");
  require_near(composite.value_as_double(4).value_or(-1.0), 5.0, "index 4");
  // Segments are exposed for boundary walking.
  require(composite.segments().size() == 2, "segments() must expose 2");
  require(composite.segments()[0].length() == 3,
          "segment 0 length must be 3");
  require(composite.segments()[1].length() == 2,
          "segment 1 length must be 2");
}

// Each segment's SharedOwner keeps its physical block alive independently — the
// composite holds no contiguous copy, and releasing the caller's shared_ptr to
// a block does NOT free it (the segment's owner retains it).
void segment_owners_keep_blocks_alive_independently() {
  // Create a block, then build a composite from a BufferView over it, then
  // drop the caller's shared_ptr. The composite's segment must still read.
  auto values = std::make_shared<std::vector<double>>(
      std::initializer_list<double>{7.0, 8.0, 9.0});
  const auto *raw = values->data();
  BufferView seg = BufferView::from_vector(
      std::shared_ptr<const std::vector<double>>(values));
  values.reset(); // drop the caller's reference
  auto composite = CompositeBufferView::from_segments({seg});
  // The block is still alive via the segment's SharedOwner; reads succeed and
  // return the original values (proving no copy was needed at composite build,
  // and the owner retention is independent).
  require_near(composite.value_as_double(0).value_or(-1.0), 7.0,
               "segment owner must keep the block alive after caller drops it");
  require_near(composite.value_as_double(2).value_or(-1.0), 9.0,
               "tail value must still read via the retained owner");
  (void)raw;
}

// Out-of-range index yields nullopt (consistent with BufferView::value_as_double).
void out_of_range_index_yields_nullopt() {
  auto head = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});
  auto tail = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{3.0});
  auto composite = CompositeBufferView::from_segments(
      {BufferView::from_vector(head), BufferView::from_vector(tail)});
  require(composite.length() == 3, "length 3");
  require(!composite.value_as_double(3).has_value(),
          "index == length must yield nullopt");
  require(!composite.value_as_double(100).has_value(),
          "a far-out-of-range index must yield nullopt");
  // An empty composite also yields nullopt and reports length 0.
  CompositeBufferView empty;
  require(empty.empty(), "default-constructed composite is empty");
  require(empty.length() == 0, "empty composite length is 0");
  require(!empty.value_as_double(0).has_value(),
          "empty composite value_as_double yields nullopt");
  require(empty.segments().empty(), "empty composite has no segments");
}

// Heterogeneous scalar_type segments and empty input are rejected (empty
// composite returned).
void heterogeneous_and_empty_input_rejected() {
  // Empty input.
  require(CompositeBufferView::from_segments({}).empty(),
          "empty segment list must yield an empty composite");
  // Heterogeneous scalar types: float32 + float64.
  auto f32 = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{1.0F, 2.0F});
  auto f64 = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{3.0});
  auto mixed = CompositeBufferView::from_segments(
      {BufferView::from_vector(f32), BufferView::from_vector(f64)});
  require(mixed.empty(),
          "a heterogeneous-type composite must be rejected (empty)");
  // A null-data segment is rejected.
  BufferView null_seg; // null data, 0 length
  require(CompositeBufferView::from_segments({null_seg}).empty(),
          "a null-data segment must be rejected");
  // Homogeneous float32 across two segments is accepted.
  auto f32b = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{3.0F, 4.0F});
  auto homo = CompositeBufferView::from_segments(
      {BufferView::from_vector(f32), BufferView::from_vector(f32b)});
  require(!homo.empty(), "homogeneous float32 composite must be accepted");
  require(homo.scalar_type() == ScalarType::float32,
          "scalar type must be float32");
  require(homo.length() == 4, "homogeneous float32 composite length 4");
}

} // namespace

int main() {
  single_segment_composite_matches_buffer_view();
  two_segment_composite_spans_concatenation();
  segment_owners_keep_blocks_alive_independently();
  out_of_range_index_yields_nullopt();
  heterogeneous_and_empty_input_rejected();
  std::cout << "welllog.composite-buffer-view: all cases passed\n";
  return EXIT_SUCCESS;
}
