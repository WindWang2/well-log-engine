// Headless test for incremental LOD tail-extension (#199, ADR 0031 "LOD 只增
// 量更新受影响尾块"). Asserts that `CurveLodPyramid::extend_tail` produces a
// pyramid IDENTICAL (envelope points + derived-byte accounting) to a full
// `build` over the extended curve, while reusing the unchanged earlier region's
// summaries. No GL, no Qt — exercises the scene LOD only.

#include <welllog/core/document.hpp>
#include <welllog/scene/curve_lod.hpp>

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
  std::exit(EXIT_FAILURE);
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

const auto axis_id = id("88000000-0000-4000-8000-000000000001");
const auto curve_id = id("88000000-0000-4000-8000-000000000002");

// Builds an increasing-MD axis + matching GR curve from explicit value lists.
// The depth/value vectors are kept alive via shared_ptr so the same physical
// block can be reused as the "prefix" of an extended curve (no reallocation →
// pointer-identity holds for the prefix).
struct Built {
  SamplingAxis axis;
  Curve curve;
};

Built make_built(const std::vector<double> &depths,
                 const std::vector<double> &values) {
  auto depth_owner =
      std::make_shared<const std::vector<double>>(depths);
  auto value_owner =
      std::make_shared<const std::vector<double>>(values);
  return Built{
      .axis = SamplingAxis{
          .id = axis_id,
          .coordinates = BufferView::from_vector(depth_owner),
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .direction = AxisDirection::increasing,
      },
      .curve = Curve{
          .id = curve_id,
          .mnemonic = "GR",
          .display_name = "Gamma Ray",
          .unit = "API",
          .sampling_axis_id = axis_id,
          .values = BufferView::from_vector(value_owner),
          .nulls = {},
      },
  };
}

// An explicit, generous budget (not the default auto-budget). extend_tail only
// guarantees parity when the budget matches between the previous and the
// extended build — the auto-budget grows with curve length, so an append would
// change it and extend_tail would (correctly) refuse. A fixed explicit budget
// is the realistic contract a host uses across appends (the session passes a
// constant per-curve budget).
CurveLodBuildOptions opts() {
  return CurveLodBuildOptions{
      .algorithm = CurveLodAlgorithm::hierarchical,
      .base_bucket_samples = 4,
      .maximum_derived_bytes = 1 * 1024 * 1024,
  };
}

// Queries both pyramids across a viewport spanning the whole extended range and
// asserts the resulting point streams are identical (same sample indices, same
// depths, same values, in order). This is the envelope-equality check.
void require_queries_equal(const CurveLodPyramid &a, const CurveLodPyramid &b,
                           std::string_view message) {
  const CurveLodQuery query{
      .viewport_top = -1.0e9,
      .viewport_bottom = 1.0e9,
      .pixel_height = 1, // force max coarsening → exercises the level summaries
      .prefetch_viewports = 0.0,
  };
  const auto sel_a = a.query(query);
  const auto sel_b = b.query(query);
  require(sel_a.has_value() && sel_b.has_value(), message);
  const auto pa = sel_a.value().points();
  const auto pb = sel_b.value().points();
  require(pa.size() == pb.size(), message);
  for (std::size_t i = 0; i < pa.size(); ++i) {
    require(pa[i].sample_index == pb[i].sample_index, message);
    require(pa[i].reference_depth == pb[i].reference_depth, message);
    require(pa[i].value == pb[i].value, message);
  }
}

// The core parity case: build over a 20-sample curve, append 12 samples to make
// a 32-sample curve, extend_tail the 20-sample pyramid onto the 32-sample curve,
// and assert the result equals a full build over the 32-sample curve (envelope
// points + derived bytes + level count + source bytes).
void extend_tail_matches_full_rebuild() {
  std::vector<double> depths_short;
  std::vector<double> values_short;
  for (int i = 0; i < 20; ++i) {
    depths_short.push_back(1000.0 + i);
    // A non-trivial signal so extrema selection is exercised.
    values_short.push_back(std::sin(static_cast<double>(i)) * 50.0 + 50.0);
  }
  std::vector<double> depths_ext = depths_short;
  std::vector<double> values_ext = values_short;
  for (int i = 20; i < 32; ++i) {
    depths_ext.push_back(1000.0 + i);
    values_ext.push_back(std::cos(static_cast<double>(i)) * 50.0 + 50.0);
  }

  const auto short_built = make_built(depths_short, values_short);
  const auto ext_built = make_built(depths_ext, values_ext);

  const auto previous =
      CurveLodPyramid::build(short_built.axis, short_built.curve, opts());
  require(previous.has_value(), "previous build must succeed");

  const auto full =
      CurveLodPyramid::build(ext_built.axis, ext_built.curve, opts());
  require(full.has_value(), "full extended build must succeed");

  const auto incremental = CurveLodPyramid::extend_tail(
      previous.value(), ext_built.axis, ext_built.curve, opts());
  require(incremental.has_value(), "extend_tail must succeed");

  // Envelope parity: the visible point streams must be identical.
  require_queries_equal(full.value(), incremental.value(),
                        "extend_tail envelope must equal full rebuild");

  // Derived-byte + level-count parity (ADR 0034 accounting must match).
  require(full.value().statistics().derived_bytes ==
              incremental.value().statistics().derived_bytes,
          "extend_tail derived_bytes must equal full rebuild");
  require(full.value().statistics().level_count ==
              incremental.value().statistics().level_count,
          "extend_tail level_count must equal full rebuild");
  require(full.value().statistics().source_bytes ==
              incremental.value().statistics().source_bytes,
          "extend_tail source_bytes must equal full rebuild");
  require(full.value().statistics().budget_limited ==
              incremental.value().statistics().budget_limited,
          "extend_tail budget_limited flag must match full rebuild");
}

// Parity holds across multiple successive appends (a 3-segment curve built by
// two chained extend_tail calls equals one full build over the whole curve).
void chained_extend_matches_full_rebuild() {
  std::vector<double> d1, v1, d2, v2, d3, v3;
  for (int i = 0; i < 12; ++i) {
    d1.push_back(2000.0 + i);
    v1.push_back(static_cast<double>(i * i));
  }
  d2 = d1;
  v2 = v1;
  for (int i = 12; i < 24; ++i) {
    d2.push_back(2000.0 + i);
    v2.push_back(static_cast<double>(i * 3 - 5));
  }
  d3 = d2;
  v3 = v2;
  for (int i = 24; i < 40; ++i) {
    d3.push_back(2000.0 + i);
    v3.push_back(static_cast<double>(40 - i));
  }

  const auto b1 = make_built(d1, v1);
  const auto b2 = make_built(d2, v2);
  const auto b3 = make_built(d3, v3);

  const auto p1 = CurveLodPyramid::build(b1.axis, b1.curve, opts());
  require(p1.has_value(), "first build must succeed");
  const auto p2 = CurveLodPyramid::extend_tail(p1.value(), b2.axis, b2.curve,
                                                opts());
  require(p2.has_value(), "first extend must succeed");
  const auto p3 = CurveLodPyramid::extend_tail(p2.value(), b3.axis, b3.curve,
                                                opts());
  require(p3.has_value(), "second extend must succeed");

  const auto full = CurveLodPyramid::build(b3.axis, b3.curve, opts());
  require(full.has_value(), "full build over whole curve must succeed");

  require_queries_equal(full.value(), p3.value(),
                        "chained extend envelope must equal full rebuild");
  require(full.value().statistics().derived_bytes ==
              p3.value().statistics().derived_bytes,
          "chained extend derived_bytes must equal full rebuild");
}

// An edited (non-append) extended curve — the prefix is changed — is rejected:
// reusing the earlier summaries would be wrong, so extend_tail refuses and the
// caller must issue a full build.
void edited_prefix_rejected() {
  std::vector<double> d_short, v_short, d_ext, v_ext;
  for (int i = 0; i < 16; ++i) {
    d_short.push_back(3000.0 + i);
    v_short.push_back(static_cast<double>(i));
  }
  d_ext = d_short;
  v_ext = v_short;
  for (int i = 16; i < 24; ++i) {
    d_ext.push_back(3000.0 + i);
    v_ext.push_back(static_cast<double>(i));
  }
  // EDIT the prefix: change an early value so the extended curve is NOT the
  // short curve plus a tail.
  v_ext[3] = 999.0;

  const auto short_built = make_built(d_short, v_short);
  const auto ext_built = make_built(d_ext, v_ext);
  const auto previous =
      CurveLodPyramid::build(short_built.axis, short_built.curve, opts());
  require(previous.has_value(), "previous build must succeed");

  const auto result = CurveLodPyramid::extend_tail(
      previous.value(), ext_built.axis, ext_built.curve, opts());
  require(!result.has_value(),
          "extend_tail must reject an edited (non-append) prefix");
  require(result.error().code == ErrorCode::invalid_document,
          "edited prefix must return the document-structure code");
}

// A shorter or equal-length extended curve is rejected (extend_tail only grows).
void non_growing_curve_rejected() {
  std::vector<double> d, v;
  for (int i = 0; i < 16; ++i) {
    d.push_back(4000.0 + i);
    v.push_back(static_cast<double>(i));
  }
  const auto built = make_built(d, v);
  const auto previous =
      CurveLodPyramid::build(built.axis, built.curve, opts());
  require(previous.has_value(), "previous build must succeed");

  // Equal length (no growth).
  const auto equal = CurveLodPyramid::extend_tail(previous.value(),
                                                  built.axis, built.curve, opts());
  require(!equal.has_value(), "extend_tail must reject an equal-length curve");
  require(equal.error().code == ErrorCode::invalid_document,
          "non-growing curve must return the document-structure code");
}

// A null gap in the appended tail (which splits the last region into two runs)
// is handled: extend_tail re-derives the run structure of the appended region,
// so a null-introduced run break still matches a full rebuild.
void null_gap_in_tail_matches_full_rebuild() {
  std::vector<double> d_short, v_short;
  for (int i = 0; i < 20; ++i) {
    d_short.push_back(5000.0 + i);
    v_short.push_back(static_cast<double>(i));
  }
  // Extended curve: 20 valid + 4 null + 8 valid. The null gap splits the tail
  // into a new run, exercising run-boundary re-derivation.
  std::vector<double> d_ext = d_short;
  std::vector<double> v_ext = v_short;
  for (int i = 20; i < 24; ++i) { // null gap
    d_ext.push_back(5000.0 + i);
    v_ext.push_back(std::numeric_limits<double>::quiet_NaN());
  }
  for (int i = 24; i < 32; ++i) { // valid tail run
    d_ext.push_back(5000.0 + i);
    v_ext.push_back(static_cast<double>(i));
  }
  // Null bitmap marking the 4 NaN gap samples as null (indices 20..23).
  std::vector<std::uint8_t> null_bytes((32 + 7) / 8, 0);
  for (std::size_t i = 20; i < 24; ++i) {
    null_bytes[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
  }

  auto depth_owner = std::make_shared<const std::vector<double>>(d_ext);
  auto value_owner = std::make_shared<const std::vector<double>>(v_ext);
  auto null_owner = std::make_shared<const std::vector<std::uint8_t>>(null_bytes);
  const auto ext_built = Built{
      .axis = SamplingAxis{
          .id = axis_id,
          .coordinates = BufferView::from_vector(depth_owner),
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .direction = AxisDirection::increasing,
      },
      .curve = Curve{
          .id = curve_id,
          .mnemonic = "GR",
          .display_name = "Gamma Ray",
          .unit = "API",
          .sampling_axis_id = axis_id,
          .values = BufferView::from_vector(value_owner),
          .nulls = NullBitmapView::from_raw(
              null_owner->data(), 32,
              static_cast<std::uint64_t>(null_owner->size()),
              SharedOwner{null_owner}),
      },
  };
  // The short curve is the prefix with no nulls (indices 0..19 valid).
  const auto short_built = make_built(d_short, v_short);

  const auto previous =
      CurveLodPyramid::build(short_built.axis, short_built.curve, opts());
  require(previous.has_value(), "previous build (no nulls) must succeed");
  const auto full =
      CurveLodPyramid::build(ext_built.axis, ext_built.curve, opts());
  require(full.has_value(), "full build with null gap must succeed");
  const auto incremental = CurveLodPyramid::extend_tail(
      previous.value(), ext_built.axis, ext_built.curve, opts());
  require(incremental.has_value(),
          "extend_tail across a null gap must succeed");

  require_queries_equal(full.value(), incremental.value(),
                        "extend_tail across null gap must equal full rebuild");
  require(full.value().statistics().derived_bytes ==
              incremental.value().statistics().derived_bytes,
          "extend_tail null-gap derived_bytes must equal full rebuild");
}

// Parity under a BINDING (tight) budget — the regime the earlier review flagged
// as untested. A budget small enough to truncate levels (budget_limited) must
// still yield a pyramid identical to a full rebuild: same derived_bytes, same
// level_count, same budget_limited flag, same envelope. This is the guard
// against the SourceRun-overhead pre-charge divergence (the per-run overhead
// must be charged exactly as build does, or extend_tail gets more budget than
// build and emits a level build truncates).
void extend_tail_matches_full_rebuild_under_tight_budget() {
  std::vector<double> depths_short, values_short, depths_ext, values_ext;
  for (int i = 0; i < 40; ++i) {
    depths_short.push_back(6000.0 + i);
    values_short.push_back(std::sin(static_cast<double>(i)) * 50.0 + 50.0);
  }
  depths_ext = depths_short;
  values_ext = values_short;
  for (int i = 40; i < 80; ++i) {
    depths_ext.push_back(6000.0 + i);
    values_ext.push_back(std::cos(static_cast<double>(i)) * 50.0 + 50.0);
  }
  const auto short_built = make_built(depths_short, values_short);
  const auto ext_built = make_built(depths_ext, values_ext);

  // A deliberately tight budget: enough for the first one or two levels but
  // not the full hierarchy, so budget_limited is exercised.
  const CurveLodBuildOptions tight{
      .algorithm = CurveLodAlgorithm::hierarchical,
      .base_bucket_samples = 4,
      .maximum_derived_bytes = 256,
  };
  const auto previous =
      CurveLodPyramid::build(short_built.axis, short_built.curve, tight);
  require(previous.has_value(), "tight-budget previous build must succeed");
  require(previous.value().statistics().budget_limited,
          "tight budget must truncate the previous build's levels");

  const auto full = CurveLodPyramid::build(ext_built.axis, ext_built.curve, tight);
  require(full.has_value(), "tight-budget full build must succeed");
  const auto incremental = CurveLodPyramid::extend_tail(
      previous.value(), ext_built.axis, ext_built.curve, tight);
  require(incremental.has_value(),
          "tight-budget extend_tail must succeed");

  require_queries_equal(full.value(), incremental.value(),
                        "tight-budget extend_tail envelope must equal full");
  require(full.value().statistics().derived_bytes ==
              incremental.value().statistics().derived_bytes,
          "tight-budget derived_bytes must match full rebuild");
  require(full.value().statistics().level_count ==
              incremental.value().statistics().level_count,
          "tight-budget level_count must match full rebuild");
  require(full.value().statistics().budget_limited ==
              incremental.value().statistics().budget_limited,
          "tight-budget budget_limited flag must match full rebuild");
}

// When the budget differs between the previous build and the extended build
// (e.g. the auto-budget grew because the curve grew), extend_tail must REFUSE
// rather than produce a non-parity result — the caller issues a full build.
// Documents the budget-parity precondition contract.
void mismatched_budget_rejected() {
  std::vector<double> depths_short, values_short, depths_ext, values_ext;
  for (int i = 0; i < 20; ++i) {
    depths_short.push_back(7000.0 + i);
    values_short.push_back(static_cast<double>(i));
  }
  depths_ext = depths_short;
  values_ext = values_short;
  for (int i = 20; i < 60; ++i) {
    depths_ext.push_back(7000.0 + i);
    values_ext.push_back(static_cast<double>(i));
  }
  const auto short_built = make_built(depths_short, values_short);
  const auto ext_built = make_built(depths_ext, values_ext);

  // Previous built with an explicit budget; extend requested with a DIFFERENT
  // (larger) explicit budget → extend_tail must refuse (no parity guarantee).
  const CurveLodBuildOptions small{
      .algorithm = CurveLodAlgorithm::hierarchical,
      .base_bucket_samples = 4,
      .maximum_derived_bytes = 512,
  };
  const CurveLodBuildOptions large{
      .algorithm = CurveLodAlgorithm::hierarchical,
      .base_bucket_samples = 4,
      .maximum_derived_bytes = 65536,
  };
  const auto previous =
      CurveLodPyramid::build(short_built.axis, short_built.curve, small);
  require(previous.has_value(), "previous build must succeed");
  const auto result = CurveLodPyramid::extend_tail(previous.value(),
                                                   ext_built.axis, ext_built.curve,
                                                   large);
  require(!result.has_value(),
          "extend_tail must refuse when the budget differs from the previous");
  require(result.error().code == ErrorCode::invalid_document,
          "mismatched budget must return the document-structure code");
}

} // namespace

int main() {
  extend_tail_matches_full_rebuild();
  chained_extend_matches_full_rebuild();
  edited_prefix_rejected();
  non_growing_curve_rejected();
  null_gap_in_tail_matches_full_rebuild();
  extend_tail_matches_full_rebuild_under_tight_budget();
  mismatched_budget_rejected();
  std::cout << "welllog.incremental-lod: all cases passed\n";
  return EXIT_SUCCESS;
}
