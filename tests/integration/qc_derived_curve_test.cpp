#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

struct SourceFixture {
  WellLogDocument document;
  EntityId document_id;
  EntityId axis_id;
  EntityId curve_id;
  const void *raw_values_data{};
  std::uint64_t raw_values_length{};
  std::shared_ptr<const std::vector<double>> depths;
  std::shared_ptr<const std::vector<double>> values;
};

SourceFixture make_source() {
  SourceFixture fixture;
  fixture.document_id = id("15900000-0000-4000-8000-000000000001");
  fixture.axis_id = id("15900000-0000-4000-8000-000000000002");
  fixture.curve_id = id("15900000-0000-4000-8000-000000000003");
  fixture.depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1001.0, 1002.0, 1003.0});
  fixture.values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 40.0, 20.0, 60.0});
  WellLogDocumentBuilder builder(fixture.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = fixture.axis_id,
      .coordinates = BufferView::from_vector(fixture.depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = fixture.curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = fixture.axis_id,
      .values = BufferView::from_vector(fixture.values),
      .nulls = {},
  });
  fixture.document = builder.build();
  require(!fixture.document.id().is_nil(), "source document must build");
  const auto &curve = fixture.document.curves().front();
  fixture.raw_values_data = curve.values.as_single().data();
  fixture.raw_values_length = curve.values.length();
  return fixture;
}

void original_curve_buffer_stays_byte_identical_after_qc_and_derived() {
  auto source = make_source();
  WellLogSession session;
  require(session.execute(SetDocumentCommand{source.document}).has_value(),
          "source document loads");

  const auto mask_id = id("15900000-0000-4000-8000-000000000010");
  auto states = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{
          static_cast<std::uint8_t>(QcState::valid),
          static_cast<std::uint8_t>(QcState::suspect),
          static_cast<std::uint8_t>(QcState::invalid),
          static_cast<std::uint8_t>(QcState::user_excluded),
      });
  const auto mask = QcMask{
      .id = mask_id,
      .curve_id = source.curve_id,
      .states = BufferView::from_vector(states),
  };

  // Host-computed derived values (e.g. box smooth) — original untouched.
  auto derived_values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 25.0, 30.0, 40.0});
  const auto derived_id = id("15900000-0000-4000-8000-000000000011");
  Curve derived{
      .id = derived_id,
      .mnemonic = "GR_SMOOTH",
      .display_name = "GR smoothed",
      .unit = "API",
      .sampling_axis_id = source.axis_id,
      .values = BufferView::from_vector(derived_values),
      .nulls = {},
      .derived =
          DerivedCurveProvenance{
              .input_curve_id = source.curve_id,
              .input_revision = DocumentRevision{1},
              .algorithm_id = "smooth.box",
              .algorithm_version = "1",
              .parameters = R"({"window":3})",
              .output_sampling_axis_id = source.axis_id,
              .input_values_data = source.raw_values_data,
              .input_values_length = source.raw_values_length,
              .freshness = DerivedFreshness::current,
          },
  };

  auto receipt = session.execute(ApplyPatchCommand{
      .document_id = source.document_id,
      .patch =
          DocumentPatch{
              .base_revision = DocumentRevision{1},
              .edits =
                  {
                      EntityEdit{UpsertEntity{mask}},
                      EntityEdit{UpsertEntity{derived}},
                  },
          },
  });
  require(receipt.has_value(), "QC + derived patch must apply");
  require(receipt.value().document_revision.value == 2,
          "patch advances document revision");

  const auto &doc = *session.document(source.document_id);
  const auto &raw = [&]() -> const Curve & {
    for (const auto &curve : doc.curves()) {
      if (curve.id == source.curve_id) {
        return curve;
      }
    }
    fail("raw curve missing");
  }();
  require(raw.values.as_single().data() == source.raw_values_data &&
              raw.values.length() == source.raw_values_length,
          "raw curve buffer identity is unchanged");
  require(std::memcmp(raw.values.as_single().data(), source.values->data(),
                      source.values->size() * sizeof(double)) == 0,
          "raw curve bytes are identical after QC/derived ops");
  require(doc.qc_masks().size() == 1, "qc mask stored on document");
  require(qc_state_at(doc, raw, 0) == QcState::valid, "valid sample");
  require(qc_state_at(doc, raw, 1) == QcState::suspect, "suspect sample");
  require(qc_state_at(doc, raw, 2) == QcState::invalid, "invalid sample");
  require(qc_state_at(doc, raw, 3) == QcState::user_excluded,
          "user-excluded sample");

  const auto &derived_curve = [&]() -> const Curve & {
    for (const auto &curve : doc.curves()) {
      if (curve.id == derived_id) {
        return curve;
      }
    }
    fail("derived curve missing");
  }();
  require(derived_curve.derived.has_value(), "derived provenance present");
  require(derived_curve.derived->algorithm_id == "smooth.box",
          "algorithm id recorded");
  require(derived_curve.derived->freshness == DerivedFreshness::current,
          "derived is current while input buffer matches");
}

void table_and_graphics_respect_qc_policy() {
  auto source = make_source();
  WellLogSession session;
  require(session.execute(SetDocumentCommand{source.document}).has_value(),
          "source document loads");
  const auto mask_id = id("15900000-0000-4000-8000-000000000020");
  auto states = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{
          static_cast<std::uint8_t>(QcState::valid),
          static_cast<std::uint8_t>(QcState::suspect),
          static_cast<std::uint8_t>(QcState::invalid),
          static_cast<std::uint8_t>(QcState::user_excluded),
      });
  require(session
              .execute(ApplyPatchCommand{
                  .document_id = source.document_id,
                  .patch =
                      DocumentPatch{
                          .base_revision = DocumentRevision{1},
                          .edits = {EntityEdit{UpsertEntity{QcMask{
                              .id = mask_id,
                              .curve_id = source.curve_id,
                              .states = BufferView::from_vector(states),
                          }}}},
                      },
              })
              .has_value(),
          "qc mask patch applies");

  const auto &doc = *session.document(source.document_id);
  auto tables = TableProjectionBuilder::from_document(doc);
  require(tables.size() == 1 && tables.front().column_count() == 2,
          "table projects depth + GR");
  // Default policy: invalid + user_excluded null; suspect visible.
  require(!tables.front().cell(0, 1).null(), "valid cell has value");
  require(!tables.front().cell(1, 1).null() &&
              tables.front().cell(1, 1).qc_state == QcState::suspect,
          "suspect remains visible by default");
  require(tables.front().cell(2, 1).null() &&
              tables.front().cell(2, 1).qc_state == QcState::invalid,
          "invalid is nullified in table");
  require(tables.front().cell(3, 1).null(),
          "user-excluded is nullified in table");

  // Configurable: hide suspect as well.
  auto strict = TableProjectionBuilder::from_document(
      doc, TableQcPolicy{.nullify_suspect = true,
                         .nullify_invalid = true,
                         .nullify_user_excluded = true});
  require(strict.front().cell(1, 1).null(),
          "table policy can nullify suspect");

  // Graphics: default QC policy on CurveLayerSpec hides invalid/excluded.
  const auto track_id = id("15900000-0000-4000-8000-000000000030");
  const auto scale_id = id("15900000-0000-4000-8000-000000000031");
  const auto layer_id = id("15900000-0000-4000-8000-000000000032");
  ScenePresentationBuilder presentation(
      source.document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1003.0},
      Millimetres{80.0}, "font-fixture-v1");
  presentation.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 1});
  presentation.add_scale(TrackScaleSpec{.id = scale_id,
                                        .track_id = track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 0.0,
                                        .maximum = 100.0,
                                        .direction = ScaleDirection::left_to_right,
                                        .unit = "API"});
  presentation.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = source.curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{1, 2, 3, 255},
      .line_width = Millimetres{0.3},
      .z_order = 1,
      .visible = true,
      .qc_display = QcDisplayPolicy{.hide_suspect = false,
                                    .hide_invalid = true,
                                    .hide_user_excluded = true},
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "presentation with qc policy loads");
  const auto scene = session.prepared_scene(source.document_id);
  require(scene != nullptr, "scene prepares");
  // 4 samples, invalid+excluded hidden → 2 drawable points (valid+suspect).
  require(scene->curve_points().size() == 2,
          "graphics hide invalid and user-excluded samples");
}

void derived_becomes_stale_when_input_buffer_changes() {
  auto source = make_source();
  WellLogSession session;
  require(session.execute(SetDocumentCommand{source.document}).has_value(),
          "source loads");
  const auto derived_id = id("15900000-0000-4000-8000-000000000040");
  auto derived_values = std::make_shared<const std::vector<double>>(
      std::vector<double>{1.0, 2.0, 3.0, 4.0});
  Curve derived{
      .id = derived_id,
      .mnemonic = "GR_FILT",
      .display_name = "filtered",
      .unit = "API",
      .sampling_axis_id = source.axis_id,
      .values = BufferView::from_vector(derived_values),
      .nulls = {},
      .derived =
          DerivedCurveProvenance{
              .input_curve_id = source.curve_id,
              .input_revision = DocumentRevision{1},
              .algorithm_id = "filter.lowpass",
              .algorithm_version = "1",
              .parameters = R"({"hz":0.1})",
              .output_sampling_axis_id = source.axis_id,
              .input_values_data = source.raw_values_data,
              .input_values_length = source.raw_values_length,
          },
  };
  require(session
              .execute(ApplyPatchCommand{
                  .document_id = source.document_id,
                  .patch =
                      DocumentPatch{.base_revision = DocumentRevision{1},
                                    .edits = {EntityEdit{UpsertEntity{derived}}}},
              })
              .has_value(),
          "derived patch applies");
  {
    const auto &doc = *session.document(source.document_id);
    for (const auto &curve : doc.curves()) {
      if (curve.id == derived_id) {
        require(curve.derived->freshness == DerivedFreshness::current,
                "fresh after create");
      }
    }
  }

  // Replace the source document with a new buffer (new SharedOwner) via
  // SetDocumentCommand — input identity changes → derived must go stale.
  auto new_values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 40.0, 20.0, 60.0});
  WellLogDocumentBuilder rebuilt(source.document_id, DocumentRevision{3});
  rebuilt.add_sampling_axis(SamplingAxis{
      .id = source.axis_id,
      .coordinates = BufferView::from_vector(source.depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  rebuilt.add_curve(Curve{
      .id = source.curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = source.axis_id,
      .values = BufferView::from_vector(new_values),
      .nulls = {},
  });
  // Carry the derived curve forward with OLD input identity snapshot.
  rebuilt.add_curve(derived);
  require(session.execute(SetDocumentCommand{rebuilt.build()}).has_value(),
          "replacement document loads");
  const auto &doc = *session.document(source.document_id);
  bool saw_stale = false;
  for (const auto &curve : doc.curves()) {
    if (curve.id == derived_id) {
      require(curve.derived.has_value(), "provenance retained");
      require(curve.derived->freshness == DerivedFreshness::stale,
              "derived is stale after input buffer replacement");
      require(curve.derived->parameters == R"({"hz":0.1})",
              "parameters retained for resample/export reuse");
      saw_stale = true;
    }
  }
  require(saw_stale, "derived curve still present");

  // Table surfaces freshness for resampled-export consumers.
  auto tables = TableProjectionBuilder::from_document(doc);
  require(tables.size() == 1, "one axis table");
  bool saw_column = false;
  for (std::uint64_t c = 0; c < tables.front().column_count(); ++c) {
    const auto col = tables.front().column(c);
    if (col.curve_id == derived_id) {
      require(col.derived.has_value(), "column carries derived provenance");
      require(col.derived_freshness == DerivedFreshness::stale,
              "column reports stale");
      require(col.derived->algorithm_id == "filter.lowpass",
              "column reuses algorithm identity");
      saw_column = true;
    }
  }
  require(saw_column, "derived column present in table");
}

void qc_and_derived_support_undo_redo() {
  auto source = make_source();
  WellLogSession session;
  require(session.execute(SetDocumentCommand{source.document}).has_value(),
          "source loads");
  const auto mask_id = id("15900000-0000-4000-8000-000000000050");
  auto states = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{0, 0, 2, 0});
  require(session
              .execute(ApplyPatchCommand{
                  .document_id = source.document_id,
                  .patch =
                      DocumentPatch{
                          .base_revision = DocumentRevision{1},
                          .edits = {EntityEdit{UpsertEntity{QcMask{
                              .id = mask_id,
                              .curve_id = source.curve_id,
                              .states = BufferView::from_vector(states),
                          }}}},
                      },
              })
              .has_value(),
          "mask patch applies");
  require(session.document(source.document_id)->qc_masks().size() == 1,
          "mask present");
  require(session.execute(UndoCommand{source.document_id}).has_value(),
          "undo succeeds");
  require(session.document(source.document_id)->qc_masks().empty(),
          "undo removes mask");
  require(session.execute(RedoCommand{source.document_id}).has_value(),
          "redo succeeds");
  require(session.document(source.document_id)->qc_masks().size() == 1,
          "redo restores mask");
}

void rejects_raw_curve_patch_and_mismatched_mask() {
  auto source = make_source();
  WellLogSession session;
  require(session.execute(SetDocumentCommand{source.document}).has_value(),
          "source loads");
  Curve raw_rewrite = source.document.curves().front();
  raw_rewrite.display_name = "hacked";
  const auto bad = session.execute(ApplyPatchCommand{
      .document_id = source.document_id,
      .patch =
          DocumentPatch{
              .base_revision = DocumentRevision{1},
              .edits = {EntityEdit{UpsertEntity{raw_rewrite}}},
          },
  });
  // Raw curve upsert is not a document entity route (derived required).
  require(!bad.has_value() || session.document(source.document_id)
                                      ->curves()
                                      .front()
                                      .display_name == "Gamma Ray",
          "raw curve must not be rewritten via patch");

  auto states = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{0, 0}); // wrong length
  const auto mask_bad = session.execute(ApplyPatchCommand{
      .document_id = source.document_id,
      .patch =
          DocumentPatch{
              .base_revision = DocumentRevision{1},
              .edits = {EntityEdit{UpsertEntity{QcMask{
                  .id = id("15900000-0000-4000-8000-000000000060"),
                  .curve_id = source.curve_id,
                  .states = BufferView::from_vector(states),
              }}}},
          },
  });
  require(!mask_bad.has_value(), "mask length mismatch is rejected");
}

} // namespace

int main() {
  original_curve_buffer_stays_byte_identical_after_qc_and_derived();
  table_and_graphics_respect_qc_policy();
  derived_becomes_stale_when_input_buffer_changes();
  qc_and_derived_support_undo_redo();
  rejects_raw_curve_patch_and_mismatched_mask();
  return EXIT_SUCCESS;
}
