#include <welllog/export/svg.hpp>
#include <welllog/io/las.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

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
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

constexpr std::string_view las_2 = R"LAS(~Version Information
VERS. 2.0 : CWLS log ASCII standard
WRAP. NO
~Well Information
WELL. Example-1
NULL. -999.25
~Curve Information
DEPT.M : Measured depth
GR.API : Gamma ray main
GR.API : Gamma ray repeat
RHOB.G/C3 : Bulk density
~ASCII Log Data
1000.0 45.0 46.0 2.40
1000.5 -999.25 -9999.0 2.41
1001.0 50.0 51.0 2.42
)LAS";

LasImport parse_las(std::string_view text) {
  const auto imported = LasSourceAdapter::parse(
      text, BufferSourceReference{.uri = "asset://well/Example-1",
                                   .checksum = "content-v1"});
  require(imported.has_value(), "valid LAS source must parse");
  return imported.value();
}

void matches_the_existing_fast_las_parity_fixture() {
  // This is the established fast-path row fixture from
  // tests/test_well_log_cpp.py, enriched only with the normal LAS metadata
  // that carries units. The legacy fast channel returns DEPT/GR/DEN rows
  // 100.0/45.0/2.2, 100.1/48.0/2.3 and a null GR at 100.2.
  constexpr std::string_view legacy_fast_path_fixture = R"LAS(~V
VERS. 2.0
WRAP. NO
~W
NULL. -999.25
~C
DEPT.M : Measured depth
GR.API : Gamma ray
DEN.G/C3 : Bulk density
~A DEPT GR DEN
100.0 45.0 2.2
100.1 48.0 2.3
100.2 -999.25 2.4
)LAS";

  const auto imported = parse_las(legacy_fast_path_fixture);
  const auto &document = imported.document;
  const auto &axis = document.sampling_axes().front();
  require(axis.unit == "M" && document.curves()[0].unit == "API" &&
              document.curves()[1].unit == "G/C3",
          "the adapter must preserve units alongside legacy fast-path values");
  require_near(*axis.coordinates.value_as_double(0), 100.0,
               "the adapter depth must match the existing fast-path fixture");
  require_near(*document.curves()[0].values.value_as_double(1), 48.0,
               "the adapter GR value must match the existing fast-path fixture");
  require(document.curves()[0].nulls.is_null(2),
          "the adapter NULL result must match the existing fast-path fixture");
  require_near(*document.curves()[1].values.value_as_double(2), 2.4,
               "the adapter density value must match the existing fast-path fixture");
}

void normalizes_las_2_into_a_shared_axis_and_stable_entities() {
  const auto imported = parse_las(las_2);
  const auto &document = imported.document;

  require(document.sampling_axes().size() == 1,
          "one LAS depth column must create one shared Sampling Axis");
  require(document.curves().size() == 3,
          "every non-depth LAS column must become a curve");

  const auto &axis = document.sampling_axes().front();
  require(axis.domain == DepthDomain::measured_depth,
          "LAS depth must use the measured-depth domain");
  require(axis.unit == "M", "LAS depth unit must be retained");
  require(axis.coordinates.length() == 3,
          "Sampling Axis must retain every accepted source row");
  require_near(*axis.coordinates.value_as_double(1), 1000.5,
               "Sampling Axis must retain the original depth values");

  const auto &first_gr = document.curves()[0];
  const auto &second_gr = document.curves()[1];
  const auto &rhob = document.curves()[2];
  require(first_gr.mnemonic == "GR" && second_gr.mnemonic == "GR",
          "same-named LAS curves must retain their source mnemonic");
  require(first_gr.id != second_gr.id,
          "same-named LAS curves must receive distinct stable identities");
  require(first_gr.sampling_axis_id == axis.id &&
              second_gr.sampling_axis_id == axis.id &&
              rhob.sampling_axis_id == axis.id,
          "all LAS curves must reference the one shared Sampling Axis");
  require(first_gr.unit == "API" && rhob.unit == "G/C3",
          "LAS curve units must be retained");
  require(first_gr.nulls.is_null(1),
          "only the declared NULL sentinel must create a null sample");
  require(!second_gr.nulls.is_null(1),
          "an undeclared negative value must remain a valid sample");
  require_near(*second_gr.values.value_as_double(1), -9999.0,
               "the adapter must not apply an implicit second NULL sentinel");

  const auto reparsed = parse_las(las_2);
  require(reparsed.document.id() == document.id(),
          "the same source identity must produce a stable document identity");
  require(reparsed.document.sampling_axes().front().id == axis.id,
          "the same source identity must produce a stable axis identity");
  require(reparsed.document.curves()[1].id == second_gr.id,
          "the same source identity must produce stable curve identities");

  const auto tables = TableProjectionBuilder::from_document(document);
  require(tables.size() == 1 && tables.front().row_count() == 3,
          "the normalized LAS document must enter the table projection");
  require(tables.front().column_count() == 4,
          "the table must retain depth and every LAS curve column");

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "the normalized LAS document must be accepted by a session");

  const auto track_id = id("16400000-0000-4000-8000-000000000001");
  const auto scale_id = id("16400000-0000-4000-8000-000000000002");
  const auto layer_id = id("16400000-0000-4000-8000-000000000003");
  const auto density_scale_id = id("16400000-0000-4000-8000-000000000004");
  const auto density_layer_id = id("16400000-0000-4000-8000-000000000005");
  const auto pattern_id = id("16400000-0000-4000-8000-000000000006");
  const auto interval_id = id("16400000-0000-4000-8000-000000000007");
  const auto interval_layer_id = id("16400000-0000-4000-8000-000000000008");
  require(session
              .execute(ApplyPatchCommand{
                  .document_id = document.id(),
                  .patch =
                      DocumentPatch{
                          .base_revision = document.revision(),
                          .edits = {EntityEdit{UpsertEntity{Interval{
                              .id = interval_id,
                              .top_reference_depth = 1000.0,
                              .bottom_reference_depth = 1001.0,
                              .semantic = IntervalSemantic::lithology,
                              .pattern_id = pattern_id,
                              .fill_color = RgbaColor{120, 100, 80, 255},
                              .label = "Sand",
                          }}}},
                      },
              })
              .has_value(),
          "a LAS document must accept interpretation entities through its session seam");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "M",
                          .top = 1000.0,
                          .bottom = 1001.0},
      Millimetres{80.0}, "font-fixture-v1");
  presentation.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 1});
  presentation.add_scale(TrackScaleSpec{.id = scale_id,
                                        .track_id = track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 0.0,
                                        .maximum = 100.0,
                                        .direction = ScaleDirection::left_to_right,
                                        .unit = "API"});
  presentation.add_scale(TrackScaleSpec{.id = density_scale_id,
                                        .track_id = track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 2.0,
                                        .maximum = 3.0,
                                        .direction = ScaleDirection::right_to_left,
                                        .unit = "G/C3"});
  presentation.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = first_gr.id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 1, .green = 2, .blue = 3, .alpha = 255},
      .line_width = Millimetres{0.25},
      .z_order = 2,
      .visible = true,
  });
  presentation.add_curve_layer(CurveLayerSpec{
      .id = density_layer_id,
      .track_id = track_id,
      .curve_id = rhob.id,
      .scale_id = density_scale_id,
      .color = RgbaColor{.red = 4, .green = 5, .blue = 6, .alpha = 255},
      .line_width = Millimetres{0.25},
      .z_order = 3,
      .visible = true,
  });
  presentation.add_pattern(PatternDefinition{
      .id = pattern_id,
      .tile_width = Millimetres{2.0},
      .tile_height = Millimetres{2.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{10, 10, 10, 255},
      .background = RgbaColor{0, 0, 0, 0},
      .stroke_width = Millimetres{0.2},
      .scene_anchor = PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
      .primitives = {PatternLine{
          .from = PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
          .to = PhysicalPoint{Millimetres{2.0}, Millimetres{2.0}},
      }},
      .version = 1,
  });
  presentation.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "a normalized LAS document must enter multi-scale interpretation tracks");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && scene->curve_layers().size() == 2 &&
              scene->curve_points().size() == 5,
          "the normalized LAS curves must prepare through distinct scales");
  require(scene->intervals().size() == 1,
          "a LAS document must support a patterned interpretation interval");
  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value() && svg.value().text().find("<path") != std::string_view::npos &&
              svg.value().text().find("<pattern") != std::string_view::npos,
          "a normalized LAS layout must export curves and patterns through SVG");
}

void parses_las_3_wrapped_ascii_records() {
  constexpr std::string_view las_3_wrapped = R"LAS(~V
VERS. 3.0
WRAP. YES
~W
NULL. -999.0
~C
DEPTH.FT : Depth
NPHI.V/V : Neutron porosity
RHOB.G/C3 : Bulk density
~A
1000.0 0.20
2.40 1000.5
0.21 2.41
)LAS";

  const auto imported = parse_las(las_3_wrapped);
  const auto &document = imported.document;
  require(document.sampling_axes().size() == 1 &&
              document.sampling_axes().front().coordinates.length() == 2,
          "wrapped LAS ASCII tokens must reconstruct complete sample rows");
  require(document.sampling_axes().front().unit == "FT",
          "LAS 3 depth units must be retained");
  require(document.curves().size() == 2,
          "wrapped LAS rows must create every non-depth curve");
  require_near(*document.curves()[1].values.value_as_double(1), 2.41,
               "wrapped LAS values must retain source order");
}

void diagnoses_bad_rows_without_discarding_the_valid_part_of_a_las() {
  constexpr std::string_view malformed = R"LAS(~V
VERS. 2.0
WRAP. NO
~C
DEPT.M
GR.API
RHOB.G/C3
~A
1000.0 10.0
1000.5 11.0 2.4 99.0
oops 12.0 2.5
1001.0 nan 2.6
1001.5 13.0 2.7
)LAS";

  const auto imported = parse_las(malformed);
  const auto &document = imported.document;
  require(document.sampling_axes().front().coordinates.length() == 2,
          "short, long and non-numeric rows must not discard valid rows");
  require(document.curves().front().nulls.is_null(0),
          "a non-finite curve value must become a null sample");

  bool saw_short{};
  bool saw_long{};
  bool saw_invalid{};
  bool saw_non_finite{};
  for (const auto &diagnostic : imported.diagnostics) {
    saw_short = saw_short || diagnostic.code == LasDiagnosticCode::short_ascii_row;
    saw_long = saw_long || diagnostic.code == LasDiagnosticCode::long_ascii_row;
    saw_invalid = saw_invalid || diagnostic.code == LasDiagnosticCode::invalid_ascii_value;
    saw_non_finite =
        saw_non_finite || diagnostic.code == LasDiagnosticCode::non_finite_curve_value;
  }
  require(saw_short && saw_long && saw_invalid && saw_non_finite,
          "every recoverable malformed LAS condition must report a diagnostic");

  const auto incomplete = LasSourceAdapter::parse(
      "~V\nVERS. 2.0\n~C\nDEPT.M\nGR.API\n",
      BufferSourceReference{.uri = "asset://well/incomplete", .checksum = "v1"});
  require(!incomplete.has_value() &&
              incomplete.error().code == ErrorCode::invalid_document,
          "a LAS source without ASCII samples must fail explicitly");

  const auto without_depth_column = LasSourceAdapter::parse(
      "~V\nVERS. 2.0\n~C\nTIME.S\nGR.API\n~A\n1.0 10.0\n",
      BufferSourceReference{.uri = "asset://well/no-depth", .checksum = "v1"});
  require(!without_depth_column.has_value() &&
              without_depth_column.error().code == ErrorCode::invalid_document,
          "a LAS source without a DEPT or DEPTH column must fail explicitly");
}

} // namespace

int main() {
  matches_the_existing_fast_las_parity_fixture();
  normalizes_las_2_into_a_shared_axis_and_stable_entities();
  parses_las_3_wrapped_ascii_records();
  diagnoses_bad_rows_without_discarding_the_valid_part_of_a_las();
  return EXIT_SUCCESS;
}
