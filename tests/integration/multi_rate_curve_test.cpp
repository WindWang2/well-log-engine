// Multi-rate curve data model (Epic A): per-curve sampling axes end-to-end.
//
// The SDK document model expresses multi-rate sampling as multiple
// `SamplingAxis` entities — each curve carries its own `sampling_axis_id`
// (document.hpp), so different curves may have different sample counts,
// intervals, starts, coverage and even the same mnemonic. This suite locks
// the chain: builder → session validation → LOD → table projection →
// manifest round-trip, plus the new `nominal_interval` axis metadata
// (regular-sampling description; coordinates remain the source of truth).
//
// Deliberately NOT relaxed: a curve's values must be sample-aligned with its
// own axis (`curve_length_mismatch`). Multi-rate never means "shared axis
// with ragged lengths" — it means one axis per sampling.

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>
#include <welllog/io/manifest.hpp>
#include <welllog/scene/curve_lod.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
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
  if (std::abs(actual - expected) > 1e-9) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

const SamplingAxis *find_axis(const WellLogDocument &document,
                              EntityId axis_id) {
  for (const auto &axis : document.sampling_axes()) {
    if (axis.id == axis_id) {
      return &axis;
    }
  }
  return nullptr;
}

const Curve *find_curve(const WellLogDocument &document, EntityId curve_id) {
  for (const auto &curve : document.curves()) {
    if (curve.id == curve_id) {
      return &curve;
    }
  }
  return nullptr;
}

const TableProjection *find_table(const std::vector<TableProjection> &tables,
                                  EntityId axis_id) {
  for (const auto &table : tables) {
    if (table.sampling_axis_id() == axis_id) {
      return &table;
    }
  }
  return nullptr;
}

struct Fixture {
  EntityId document_id{id("aaaaaaaa-0000-4000-8000-000000000000")};
  EntityId gr_axis_id{id("bbbbbbbb-0000-4000-8000-000000000000")};
  EntityId rt_axis_id{id("cccccccc-0000-4000-8000-000000000000")};
  EntityId irr_axis_id{id("dddddddd-0000-4000-8000-000000000000")};
  EntityId gr_id{id("eeeeeeee-0000-4000-8000-000000000000")};
  EntityId rt_id{id("ffffffff-0000-4000-8000-000000000000")};
  EntityId gr_hi_id{id("11111111-0000-4000-8000-000000000000")};
  EntityId irr_id{id("22222222-0000-4000-8000-000000000000")};
};

// GR at 0.125 m over 1000.000–1000.500 (5 samples); RT at 0.2 m over
// 1000.100–1000.500 (3 samples): non-integer rate ratio (1.6) and partial
// overlap. RT deliberately starts later to exercise different starts.
WellLogDocumentBuilder multi_rate_builder(const Fixture &f) {
  auto gr_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.125, 1000.25, 1000.375,
                                    1000.5});
  auto rt_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.1, 1000.3, 1000.5});
  auto gr_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{10.0F, 12.0F, 11.0F, 13.0F, 14.0F});
  auto rt_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{1.0F, 2.0F, 3.0F});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.gr_axis_id,
      .coordinates = BufferView::from_vector(gr_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.125,
  });
  builder.add_sampling_axis(SamplingAxis{
      .id = f.rt_axis_id,
      .coordinates = BufferView::from_vector(rt_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.2,
  });
  builder.add_curve(Curve{
      .id = f.gr_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray 0.125m",
      .unit = "API",
      .sampling_axis_id = f.gr_axis_id,
      .values = BufferView::from_vector(gr_values),
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = f.rt_id,
      .mnemonic = "RT",
      .display_name = "Resistivity 0.2m",
      .unit = "OHMM",
      .sampling_axis_id = f.rt_axis_id,
      .values = BufferView::from_vector(rt_values),
      .nulls = {},
  });
  return builder;
}

void session_accepts_multi_rate_document() {
  const Fixture f;
  const auto document = multi_rate_builder(f).build();
  require(document.sampling_axes().size() == 2,
          "multi-rate document must carry both axes");
  WellLogSession session;
  const auto receipt = session.execute(SetDocumentCommand{document});
  require(receipt.has_value(), "session must accept a multi-rate document");
}

void per_curve_axis_length_invariant_is_kept() {
  // A curve whose values do not match ITS axis is still rejected — the
  // invariant is per curve-axis pair, not "all curves share one length".
  const Fixture f;
  auto gr_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.125, 1000.25, 1000.375,
                                    1000.5});
  auto short_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{10.0F, 12.0F});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.gr_axis_id,
      .coordinates = BufferView::from_vector(gr_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.125,
  });
  builder.add_curve(Curve{
      .id = f.gr_id,
      .mnemonic = "GR",
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = f.gr_axis_id,
      .values = BufferView::from_vector(short_values),
      .nulls = {},
  });
  WellLogSession session;
  const auto receipt =
      session.execute(SetDocumentCommand{builder.build()});
  require(!receipt.has_value(), "ragged curve/axis lengths must be rejected");
  require(receipt.error().message == MessageKey::curve_length_mismatch,
          "ragged lengths must surface curve_length_mismatch");
}

void nominal_interval_validated() {
  const Fixture f;
  const auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.125});
  const auto values =
      std::make_shared<const std::vector<float>>(std::initializer_list<float>{1.0F, 2.0F});
  for (const auto bad : {-1.0, 0.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
    WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
    builder.add_sampling_axis(SamplingAxis{
        .id = f.gr_axis_id,
        .coordinates = BufferView::from_vector(depths),
        .domain = DepthDomain::measured_depth,
        .unit = "m",
        .direction = AxisDirection::increasing,
        .nominal_interval = bad,
    });
    builder.add_curve(Curve{
        .id = f.gr_id,
        .mnemonic = "GR",
        .display_name = "GR",
        .unit = "API",
        .sampling_axis_id = f.gr_axis_id,
        .values = BufferView::from_vector(values),
        .nulls = {},
    });
    WellLogSession session;
    const auto receipt = session.execute(SetDocumentCommand{builder.build()});
    require(!receipt.has_value(),
            "a non-positive / non-finite nominal_interval must be rejected");
    require(receipt.error().message == MessageKey::sampling_axis_interval_invalid,
            "bad interval must surface sampling_axis_interval_invalid");
  }
}

void irregular_axis_without_interval_ok() {
  // Non-uniform coordinates: irregular sampling is expressed by the
  // coordinates alone; nominal_interval stays unset.
  const Fixture f;
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.3, 1000.8, 1002.0, 1003.0});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{1.0F, 2.0F, 3.0F, 4.0F, 5.0F});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.irr_axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = f.irr_id,
      .mnemonic = "IRR",
      .display_name = "Irregular",
      .unit = "mS/m",
      .sampling_axis_id = f.irr_axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  WellLogSession session;
  const auto receipt = session.execute(SetDocumentCommand{builder.build()});
  require(receipt.has_value(), "an irregular axis without an interval is valid");
}

void same_mnemonic_multi_version_ok() {
  // Same mnemonic, two curves on two axes: version identity = distinct curve
  // entities (EntityId), never the mnemonic.
  const Fixture f;
  auto fine_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.125});
  auto fine_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{10.0F, 12.0F});
  auto coarse_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.5, 1001.0});
  auto coarse_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{11.0F, 13.0F, 15.0F});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.gr_axis_id,
      .coordinates = BufferView::from_vector(fine_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.125,
  });
  builder.add_sampling_axis(SamplingAxis{
      .id = f.rt_axis_id,
      .coordinates = BufferView::from_vector(coarse_depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.5,
  });
  builder.add_curve(Curve{
      .id = f.gr_id,
      .mnemonic = "GR",
      .display_name = "GR raw",
      .unit = "API",
      .sampling_axis_id = f.gr_axis_id,
      .values = BufferView::from_vector(fine_values),
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = f.gr_hi_id,
      .mnemonic = "GR",
      .display_name = "GR resampled 0.5m",
      .unit = "API",
      .sampling_axis_id = f.rt_axis_id,
      .values = BufferView::from_vector(coarse_values),
      .nulls = {},
  });
  WellLogSession session;
  const auto receipt = session.execute(SetDocumentCommand{builder.build()});
  require(receipt.has_value(),
          "two curves sharing a mnemonic on distinct axes must be accepted");
}

void nan_values_ok_at_document_level() {
  // NaN values are representable; draw/table policy suppresses them
  // (document contract: "NaN and Infinity are never drawn regardless of the
  // bitmap"). Validation only guards structure, not values.
  const Fixture f;
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.125, 1000.25});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{10.0F, std::numeric_limits<float>::quiet_NaN(),
                                   12.0F});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.gr_axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.125,
  });
  builder.add_curve(Curve{
      .id = f.gr_id,
      .mnemonic = "GR",
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = f.gr_axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  WellLogSession session;
  const auto receipt = session.execute(SetDocumentCommand{builder.build()});
  require(receipt.has_value(), "NaN sample values must not fail validation");
}

void lod_builds_per_multi_rate_curve() {
  const Fixture f;
  const auto document = multi_rate_builder(f).build();
  const auto *gr_axis = find_axis(document, f.gr_axis_id);
  const auto *rt_axis = find_axis(document, f.rt_axis_id);
  require(gr_axis != nullptr && rt_axis != nullptr, "axes must resolve");
  const auto *gr = find_curve(document, f.gr_id);
  const auto *rt = find_curve(document, f.rt_id);
  require(gr != nullptr && rt != nullptr, "curves must resolve");
  // Tiny test buffers would trip the default derived-cache budget
  // ((axis+curve) bytes / 4); give the pyramid an explicit budget.
  CurveLodBuildOptions options;
  options.maximum_derived_bytes = 1u << 20;
  const auto gr_lod = CurveLodPyramid::build(
      *gr_axis, *gr, options, std::stop_token{});
  const auto rt_lod = CurveLodPyramid::build(
      *rt_axis, *rt, options, std::stop_token{});
  require(gr_lod.has_value() && rt_lod.has_value(),
          "LOD must build for each multi-rate curve");
  require(gr_lod.value().statistics().source_samples == 5 &&
              rt_lod.value().statistics().source_samples == 3,
          "LOD must reflect each curve's own sample count");
}

void table_projection_splits_by_axis() {
  const Fixture f;
  const auto document = multi_rate_builder(f).build();
  const auto tables = TableProjectionBuilder::from_document(document);
  require(tables.size() == 2, "multi-rate curves must project to two tables");
  const auto *gr_table = find_table(tables, f.gr_axis_id);
  const auto *rt_table = find_table(tables, f.rt_axis_id);
  require(gr_table != nullptr && rt_table != nullptr,
          "each axis must own a table");
  require(gr_table->row_count() == 5 && rt_table->row_count() == 3,
          "each table must carry its own sampling count");
  require(gr_table->column_count() == 2 && rt_table->column_count() == 2,
          "each table has the axis column plus its curve");
  // GR samples land on GR depths, RT samples on RT depths (no cross-align).
  require_near(gr_table->cell(4, 0).value.value_or(-1.0), 1000.5,
               "GR table depth column must read GR axis coordinates");
  require_near(rt_table->cell(0, 0).value.value_or(-1.0), 1000.1,
               "RT table depth column must read RT axis coordinates");
}

void manifest_round_trip_preserves_multi_rate_and_interval() {
  const Fixture f;
  auto gr_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.125, 1000.25});
  auto rt_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.1, 1000.3});
  auto gr_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{10.0F, 12.0F, 11.0F});
  auto rt_values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{1.0F, 2.0F});
  const auto gr_depth_view = BufferView::from_vector(
      gr_depths, BufferSourceReference{.uri = "mmap://w.bin#gr-d",
                                       .checksum = "sha256:gr-d",
                                       .byte_offset = 0});
  const auto rt_depth_view = BufferView::from_vector(
      rt_depths, BufferSourceReference{.uri = "mmap://w.bin#rt-d",
                                       .checksum = "sha256:rt-d",
                                       .byte_offset = 24});
  const auto gr_value_view = BufferView::from_vector(
      gr_values, BufferSourceReference{.uri = "mmap://w.bin#gr-v",
                                       .checksum = "sha256:gr-v",
                                       .byte_offset = 64});
  const auto rt_value_view = BufferView::from_vector(
      rt_values, BufferSourceReference{.uri = "mmap://w.bin#rt-v",
                                       .checksum = "sha256:rt-v",
                                       .byte_offset = 80});
  WellLogDocumentBuilder builder(f.document_id, DocumentRevision{7});
  builder.add_sampling_axis(SamplingAxis{
      .id = f.gr_axis_id,
      .coordinates = gr_depth_view,
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      .nominal_interval = 0.125,
  });
  builder.add_sampling_axis(SamplingAxis{
      .id = f.rt_axis_id,
      .coordinates = rt_depth_view,
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
      // Unset: exercises the omitted-key round trip.
  });
  builder.add_curve(Curve{
      .id = f.gr_id,
      .mnemonic = "GR",
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = f.gr_axis_id,
      .values = gr_value_view,
      .nulls = {},
  });
  builder.add_curve(Curve{
      .id = f.rt_id,
      .mnemonic = "RT",
      .display_name = "RT",
      .unit = "OHMM",
      .sampling_axis_id = f.rt_axis_id,
      .values = rt_value_view,
      .nulls = {},
  });
  const auto encoded = ManifestCodec::write(builder.build());
  require(encoded.has_value(), "multi-rate manifest must serialize");
  require(encoded.value().text().find("\"nominalInterval\":0.125000") !=
              std::string::npos,
          "writer must emit nominalInterval when set");

  ManifestResolvers resolvers{
      .buffer = [&](const BufferDescriptor &descriptor) -> Result<BufferView> {
        if (descriptor.source.uri == gr_depth_view.source().uri) {
          return gr_depth_view;
        }
        if (descriptor.source.uri == rt_depth_view.source().uri) {
          return rt_depth_view;
        }
        if (descriptor.source.uri == gr_value_view.source().uri) {
          return gr_value_view;
        }
        if (descriptor.source.uri == rt_value_view.source().uri) {
          return rt_value_view;
        }
        return Error{
            .code = ErrorCode::unresolved_buffer,
            .entity_id = std::nullopt,
            .message = MessageKey::external_buffer_unresolved,
            .arguments = {},
        };
      },
      .null_bitmap = {},
      .image_tile = {},
  };
  const auto decoded = ManifestCodec::read(encoded.value().text(), resolvers);
  require(decoded.has_value(), "multi-rate manifest must round-trip");
  require(decoded.value().sampling_axes().size() == 2,
          "both axes must survive the round trip");
  const auto *gr_axis = find_axis(decoded.value(), f.gr_axis_id);
  const auto *rt_axis = find_axis(decoded.value(), f.rt_axis_id);
  require(gr_axis != nullptr && rt_axis != nullptr, "axes must resolve");
  require(gr_axis->nominal_interval.has_value() &&
              *gr_axis->nominal_interval == 0.125,
          "nominalInterval must round-trip when set");
  require(!rt_axis->nominal_interval.has_value(),
          "an absent nominalInterval must stay unset");
  const auto *gr = find_curve(decoded.value(), f.gr_id);
  const auto *rt = find_curve(decoded.value(), f.rt_id);
  require(gr != nullptr && rt != nullptr, "curves must resolve");
  require(gr->sampling_axis_id == f.gr_axis_id &&
              rt->sampling_axis_id == f.rt_axis_id,
          "per-curve axis references must round-trip");
  // The decoded multi-rate document still passes session validation.
  WellLogSession session;
  const auto receipt =
      session.execute(SetDocumentCommand{decoded.value()});
  require(receipt.has_value(),
          "the round-tripped multi-rate document must validate");
}

} // namespace

int main() {
  session_accepts_multi_rate_document();
  per_curve_axis_length_invariant_is_kept();
  nominal_interval_validated();
  irregular_axis_without_interval_ok();
  same_mnemonic_multi_version_ok();
  nan_values_ok_at_document_level();
  lod_builds_per_multi_rate_curve();
  table_projection_splits_by_axis();
  manifest_round_trip_preserves_multi_rate_and_interval();
  std::cout << "PASS: multi-rate curve model\n";
  return EXIT_SUCCESS;
}
