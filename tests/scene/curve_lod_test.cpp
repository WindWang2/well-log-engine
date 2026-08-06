#include <welllog/scene/curve_lod.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stop_token>
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

struct CurveFixture {
  SamplingAxis axis;
  Curve curve;
};

CurveFixture extrema_and_null_fixture() {
  auto depths =
      std::make_shared<const std::vector<double>>(std::initializer_list<double>{
          0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10, 100, -50, 12, 13, 14, 15, 20, 0, 30,
                                    -80, 120, 31, 32, 33, 34, 40});
  auto null_bytes = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0, 1, 0});
  const auto axis_id =
      EntityId::parse("10000000-0000-4000-8000-000000000001").value();
  const auto curve_id =
      EntityId::parse("10000000-0000-4000-8000-000000000002").value();
  return CurveFixture{
      .axis =
          SamplingAxis{
              .id = axis_id,
              .coordinates = BufferView::from_vector(depths),
              .domain = DepthDomain::measured_depth,
              .unit = "m",
              .direction = AxisDirection::increasing,
          },
      .curve =
          Curve{
              .id = curve_id,
              .mnemonic = "GR",
              .display_name = "Gamma Ray",
              .unit = "API",
              .sampling_axis_id = axis_id,
              .values = BufferView::from_vector(values),
              .nulls = NullBitmapView::from_raw(
                  null_bytes->data(), 17,
                  static_cast<std::uint64_t>(null_bytes->size()),
                  SharedOwner{null_bytes}),
          },
  };
}

void m4_retains_extrema_source_order_and_null_breaks() {
  const auto fixture = extrema_and_null_fixture();
  const auto pyramid =
      CurveLodPyramid::build(fixture.axis, fixture.curve,
                             CurveLodBuildOptions{
                                 .algorithm = CurveLodAlgorithm::hierarchical,
                                 .base_bucket_samples = 8,
                                 .maximum_derived_bytes = 4096,
                             });
  require(pyramid.has_value(), "valid curve must build an LOD pyramid");

  const auto selection = pyramid.value().query(CurveLodQuery{
      .viewport_top = 0.0,
      .viewport_bottom = 16.0,
      .pixel_height = 1,
      .prefetch_viewports = 0.0,
  });
  require(selection.has_value(), "coarse viewport query must succeed");
  require(!selection.value().uses_raw_samples(),
          "coarse viewport must select a summary level");
  require(selection.value().segments().size() == 2,
          "a null sample must split the selected geometry");

  const std::vector<std::uint64_t> expected_indices{0, 1, 2, 7, 9, 10, 11, 16};
  std::vector<std::uint64_t> actual_indices;
  for (const auto &point : selection.value().points()) {
    actual_indices.push_back(point.sample_index);
  }
  require(actual_indices == expected_indices,
          "M4 must retain first/min/max/last in source order");
}

void pixel_density_returns_to_original_samples() {
  const auto fixture = extrema_and_null_fixture();
  const auto pyramid =
      CurveLodPyramid::build(fixture.axis, fixture.curve,
                             CurveLodBuildOptions{
                                 .algorithm = CurveLodAlgorithm::hierarchical,
                                 .base_bucket_samples = 8,
                                 .maximum_derived_bytes = 4096,
                             });
  require(pyramid.has_value(), "valid curve must build an LOD pyramid");

  const auto selection = pyramid.value().query(CurveLodQuery{
      .viewport_top = 9.0,
      .viewport_bottom = 11.0,
      .pixel_height = 400,
      .prefetch_viewports = 0.0,
  });
  require(selection.has_value(), "zoomed viewport query must succeed");
  require(selection.value().uses_raw_samples(),
          "fine pixel density must select original samples");

  const std::vector<std::uint64_t> expected_indices{9, 10, 11};
  std::vector<std::uint64_t> actual_indices;
  for (const auto &point : selection.value().points()) {
    actual_indices.push_back(point.sample_index);
  }
  require(actual_indices == expected_indices,
          "raw fallback must retain exact source sample indices");
}

void scalar_reference_and_hierarchy_have_identical_semantics() {
  const auto fixture = extrema_and_null_fixture();
  const auto reference = CurveLodPyramid::build(
      fixture.axis, fixture.curve,
      CurveLodBuildOptions{
          .algorithm = CurveLodAlgorithm::scalar_reference,
          .base_bucket_samples = 8,
          .maximum_derived_bytes = 4096,
      });
  const auto hierarchy =
      CurveLodPyramid::build(fixture.axis, fixture.curve,
                             CurveLodBuildOptions{
                                 .algorithm = CurveLodAlgorithm::hierarchical,
                                 .base_bucket_samples = 8,
                                 .maximum_derived_bytes = 4096,
                             });
  require(reference.has_value() && hierarchy.has_value(),
          "both LOD algorithms must accept the same curve");

  const auto query = CurveLodQuery{
      .viewport_top = 0.0,
      .viewport_bottom = 16.0,
      .pixel_height = 1,
      .prefetch_viewports = 0.0,
  };
  const auto expected = reference.value().query(query);
  const auto actual = hierarchy.value().query(query);
  require(expected.has_value() && actual.has_value(),
          "both LOD algorithms must answer the same query");
  require(expected.value().points().size() == actual.value().points().size() &&
              expected.value().segments().size() ==
                  actual.value().segments().size(),
          "reference and hierarchy must have the same semantic shape");
  for (std::size_t index = 0; index < expected.value().points().size();
       ++index) {
    require(expected.value().points()[index].sample_index ==
                    actual.value().points()[index].sample_index &&
                expected.value().points()[index].reference_depth ==
                    actual.value().points()[index].reference_depth &&
                expected.value().points()[index].value ==
                    actual.value().points()[index].value,
            "reference and hierarchy must select identical source points");
  }
  require(reference.value().statistics().level_count == 0 &&
              reference.value().statistics().derived_bytes <
                  hierarchy.value().statistics().derived_bytes,
          "scalar reference may retain run breaks but not hierarchy levels");
}

void cache_budget_and_cancellation_are_observable() {
  const auto fixture = extrema_and_null_fixture();
  const auto budget_limited =
      CurveLodPyramid::build(fixture.axis, fixture.curve,
                             CurveLodBuildOptions{
                                 .algorithm = CurveLodAlgorithm::hierarchical,
                                 .base_bucket_samples = 8,
                                 .maximum_derived_bytes = 128,
                             });
  require(budget_limited.has_value(),
          "an exhausted derived-cache budget must fall back safely");
  require(
      budget_limited.value().statistics().derived_bytes <= 128 &&
          budget_limited.value().statistics().derived_bytes > 0 &&
          budget_limited.value().statistics().budget_limited,
      "reported derived memory must include run metadata and respect budget");
  const auto budget_limited_query = budget_limited.value().query(CurveLodQuery{
      .viewport_top = 0.0,
      .viewport_bottom = 16.0,
      .pixel_height = 1,
      .prefetch_viewports = 0.0,
  });
  require(budget_limited_query.has_value() &&
              !budget_limited_query.value().uses_raw_samples() &&
              budget_limited_query.value().points().size() < 17,
          "an exhausted cache must compute bounded summaries on demand");

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto cancelled_build =
      CurveLodPyramid::build(fixture.axis, fixture.curve,
                             CurveLodBuildOptions{}, cancelled.get_token());
  require(!cancelled_build.has_value() &&
              cancelled_build.error().code == ErrorCode::operation_cancelled,
          "a cancelled build must stop with a stable cancellation code");

  const auto ready = CurveLodPyramid::build(fixture.axis, fixture.curve,
                                            CurveLodBuildOptions{});
  const auto cancelled_query = ready.value().query(
      CurveLodQuery{
          .viewport_top = 0.0,
          .viewport_bottom = 16.0,
          .pixel_height = 1,
          .prefetch_viewports = 0.0,
      },
      cancelled.get_token());
  require(!cancelled_query.has_value() &&
              cancelled_query.error().code == ErrorCode::operation_cancelled,
          "a cancelled viewport query must stop with the stable code");

  auto malformed = fixture;
  malformed.curve.nulls = NullBitmapView::from_raw(
      nullptr, 17, 3,
      SharedOwner{std::make_shared<const std::vector<std::uint8_t>>(3)});
  const auto malformed_result =
      CurveLodPyramid::build(malformed.axis, malformed.curve);
  require(!malformed_result.has_value() &&
              malformed_result.error().code == ErrorCode::invalid_buffer,
          "malformed public buffer views must fail instead of dereferencing");

  auto unowned = fixture;
  const auto &fixture_coords = fixture.axis.coordinates.as_single();
  unowned.axis.coordinates = BufferView::from_raw(
      fixture_coords.data(), fixture_coords.length(),
      fixture_coords.stride_bytes(), fixture_coords.scalar_type(),
      fixture_coords.byte_capacity(), SharedOwner{});
  const auto unowned_result =
      CurveLodPyramid::build(unowned.axis, unowned.curve);
  require(!unowned_result.has_value() &&
              unowned_result.error().code == ErrorCode::missing_owner,
          "public LOD pyramids must not retain unowned source buffers");
}

void prefetch_does_not_coarsen_visible_pixel_density() {
  const auto fixture = extrema_and_null_fixture();
  const auto pyramid =
      CurveLodPyramid::build(fixture.axis, fixture.curve,
                             CurveLodBuildOptions{
                                 .algorithm = CurveLodAlgorithm::hierarchical,
                                 .base_bucket_samples = 8,
                                 .maximum_derived_bytes = 4096,
                             });
  const auto selection = pyramid.value().query(CurveLodQuery{
      .viewport_top = 9.0,
      .viewport_bottom = 11.0,
      .pixel_height = 3,
      .prefetch_viewports = 2.0,
  });
  require(selection.has_value() && selection.value().uses_raw_samples(),
          "prefetch samples must not force a coarser visible LOD");
}

void decreasing_axes_use_the_same_bounded_query_semantics() {
  auto fixture = extrema_and_null_fixture();
  auto depths =
      std::make_shared<const std::vector<double>>(std::initializer_list<double>{
          16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
  fixture.axis.coordinates = BufferView::from_vector(depths);
  fixture.axis.direction = AxisDirection::decreasing;
  const auto pyramid = CurveLodPyramid::build(fixture.axis, fixture.curve,
                                              CurveLodBuildOptions{});
  const auto selection = pyramid.value().query(CurveLodQuery{
      .viewport_top = 9.0,
      .viewport_bottom = 11.0,
      .pixel_height = 400,
      .prefetch_viewports = 0.0,
  });
  require(selection.has_value() && selection.value().uses_raw_samples(),
          "decreasing axes must support fine-grained range lookup");
  const std::vector<std::uint64_t> expected{5, 6, 7};
  std::vector<std::uint64_t> actual;
  for (const auto &point : selection.value().points()) {
    actual.push_back(point.sample_index);
  }
  require(actual == expected,
          "decreasing-axis lookup must retain source order and bounds");
}

void aggregated_hierarchy_matches_scalar_reference_across_levels() {
  constexpr std::size_t count = 4096;
  auto depths = std::make_shared<std::vector<double>>(count);
  auto values = std::make_shared<std::vector<float>>(count);
  for (std::size_t index = 0; index < count; ++index) {
    (*depths)[index] = static_cast<double>(index) * 0.25;
    (*values)[index] =
        static_cast<float>(std::sin(static_cast<double>(index) * 0.07));
  }
  (*values)[777] = -1000.0F;
  (*values)[3001] = 1000.0F;
  const auto axis_id =
      EntityId::parse("10000000-0000-4000-8000-000000000011").value();
  const auto curve_id =
      EntityId::parse("10000000-0000-4000-8000-000000000012").value();
  const auto axis = SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(
          std::const_pointer_cast<const std::vector<double>>(depths)),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  };
  const auto curve = Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(
          std::const_pointer_cast<const std::vector<float>>(values)),
      .nulls = {},
  };
  const auto scalar = CurveLodPyramid::build(
      axis, curve,
      CurveLodBuildOptions{
          .algorithm = CurveLodAlgorithm::scalar_reference,
          .base_bucket_samples = 16,
          .maximum_derived_bytes = 1024 * 1024,
      });
  const auto hierarchy =
      CurveLodPyramid::build(axis, curve,
                             CurveLodBuildOptions{
                                 .algorithm = CurveLodAlgorithm::hierarchical,
                                 .base_bucket_samples = 16,
                                 .maximum_derived_bytes = 1024 * 1024,
                             });
  require(scalar.has_value() && hierarchy.has_value(),
          "large parity fixture must build both implementations");
  for (const auto pixels : {1U, 16U, 128U, 4096U}) {
    const auto query = CurveLodQuery{
        .viewport_top = 100.0,
        .viewport_bottom = 900.0,
        .pixel_height = pixels,
        .prefetch_viewports = 0.5,
    };
    const auto expected = scalar.value().query(query);
    const auto actual = hierarchy.value().query(query);
    require(expected.has_value() && actual.has_value() &&
                expected.value().points().size() ==
                    actual.value().points().size(),
            "aggregated hierarchy must match scalar shape at every level");
    for (std::size_t index = 0; index < actual.value().points().size();
         ++index) {
      require(actual.value().points()[index].sample_index ==
                  expected.value().points()[index].sample_index,
              "aggregated hierarchy must select scalar-reference samples");
    }
  }
}

} // namespace

int main() {
  m4_retains_extrema_source_order_and_null_breaks();
  pixel_density_returns_to_original_samples();
  scalar_reference_and_hierarchy_have_identical_semantics();
  cache_budget_and_cancellation_are_observable();
  prefetch_does_not_coarsen_visible_pixel_density();
  decreasing_axes_use_the_same_bounded_query_semantics();
  aggregated_hierarchy_matches_scalar_reference_across_levels();
  std::cout << "PASS: hierarchical curve LOD behavior\n";
  return EXIT_SUCCESS;
}
