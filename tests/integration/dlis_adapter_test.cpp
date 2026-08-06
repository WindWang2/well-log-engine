#include <welllog/export/svg.hpp>
#include <welllog/io/dlis.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
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
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

class Rp66Fixture {
public:
  Rp66Fixture() {
    const std::array<char, 80> label = [] {
      std::array<char, 80> result{};
      result.fill(' ');
      std::memcpy(result.data(), "   1V1.00RECORD ", 16);
      std::memcpy(result.data() + 16, "8192", 4);
      std::memcpy(result.data() + 20, "Paleo Workbench test fixture", 27);
      return result;
    }();
    for (const auto character : label) {
      append_byte(static_cast<std::uint8_t>(character));
    }
  }

  void add_logical_file() {
    add_record(true, 0, {0xf0, 0x0b, 'F', 'I', 'L', 'E', '-', 'H', 'E', 'A',
                          'D', 'E', 'R'});
    add_record(true, 1, {0xf0, 0x06, 'O', 'R', 'I', 'G', 'I', 'N'});
  }

  void add_unsupported_object(std::string_view type) {
    std::vector<std::uint8_t> body;
    append_set_start(body, type);
    add_record(true, 5, std::move(body));
  }

  void add_mismatched_object_set() {
    std::vector<std::uint8_t> body;
    append_set_start(body, "FRAME");
    add_record(true, 3, std::move(body));
  }

  void add_frame(std::string_view frame_name,
                 std::vector<std::string_view> channels,
                 std::string_view index_type,
                 std::string_view direction) {
    std::vector<std::uint8_t> body;
    append_set_start(body, "FRAME");
    append_template_attribute(body, "CHANNELS", 23);
    append_template_attribute(body, "INDEX-TYPE", 19);
    append_template_attribute(body, "DIRECTION", 19);
    append_object_start(body, frame_name);
    body.push_back(0x29); // channel count and values
    append_uvari(body, static_cast<std::uint32_t>(channels.size()));
    for (const auto channel : channels) {
      append_obname(body, channel);
    }
    body.push_back(0x21); // index-type value
    append_ident(body, index_type);
    body.push_back(0x21); // direction value
    append_ident(body, direction);
    add_record(true, 4, std::move(body));
  }

  void add_channel(std::string_view channel_name, std::string_view unit,
                   std::uint8_t representation_code, std::uint32_t dimension) {
    std::vector<std::uint8_t> body;
    append_set_start(body, "CHANNEL");
    append_template_attribute(body, "REPRESENTATION-CODE", 16);
    append_template_attribute(body, "UNITS", 27);
    append_template_attribute(body, "DIMENSION", 18);
    append_object_start(body, channel_name);
    body.push_back(0x21);
    append_u16(body, representation_code);
    body.push_back(0x21);
    append_ident(body, unit);
    body.push_back(0x21);
    append_uvari(body, dimension);
    add_record(true, 3, std::move(body));
  }

  void add_fdata(std::string_view frame_name,
                 const std::vector<std::array<float, 4>> &samples,
                 bool include_matrix_channel) {
    std::vector<std::uint8_t> body;
    append_obname(body, frame_name);
    std::uint32_t frame_number{1};
    for (const auto &sample : samples) {
      append_uvari(body, frame_number++);
      append_float(body, sample[0]); // depth
      append_float(body, sample[1]); // GR
      if (include_matrix_channel) {
        append_float(body, sample[2]);
        append_float(body, sample[3]);
      }
    }
    add_record(false, 0, std::move(body));
  }

  void add_malformed_fdata(std::string_view frame_name) {
    std::vector<std::uint8_t> body;
    append_obname(body, frame_name);
    append_uvari(body, 99);
    append_float(body, 1002.0F);
    add_record(false, 0, std::move(body));
  }

  void add_fdata_with_ascii(std::string_view frame_name, float depth,
                            std::string_view text) {
    std::vector<std::uint8_t> body;
    append_obname(body, frame_name);
    append_uvari(body, 1);
    append_float(body, depth);
    append_ident(body, text);
    add_record(false, 0, std::move(body));
  }

  void add_fdata_header_only(std::string_view frame_name) {
    std::vector<std::uint8_t> body;
    append_obname(body, frame_name);
    add_record(false, 0, std::move(body));
  }

  [[nodiscard]] std::vector<std::byte> finish() && {
    std::vector<std::byte> result;
    result.reserve(bytes_.size());
    for (const auto byte : bytes_) {
      result.push_back(static_cast<std::byte>(byte));
    }
    return result;
  }

private:
  static void append_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
  }

  static void append_uvari(std::vector<std::uint8_t> &out, std::uint32_t value) {
    if (value <= 0x7fU) {
      out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 0x3fffU) {
      append_u16(out, static_cast<std::uint16_t>(value | 0x8000U));
    } else {
      out.push_back(static_cast<std::uint8_t>(0xc0U | ((value >> 24U) & 0x3fU)));
      out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
      out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
      out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    }
  }

  static void append_ident(std::vector<std::uint8_t> &out, std::string_view value) {
    require(value.size() <= std::numeric_limits<std::uint8_t>::max(),
            "fixture identifiers must fit the RP66 IDENT representation");
    out.push_back(static_cast<std::uint8_t>(value.size()));
    for (const auto character : value) {
      out.push_back(static_cast<std::uint8_t>(character));
    }
  }

  static void append_obname(std::vector<std::uint8_t> &out, std::string_view value) {
    append_uvari(out, 1); // origin
    out.push_back(0);     // copy number
    append_ident(out, value);
  }

  static void append_float(std::vector<std::uint8_t> &out, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    out.push_back(static_cast<std::uint8_t>((bits >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((bits >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((bits >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(bits & 0xffU));
  }

  static void append_set_start(std::vector<std::uint8_t> &out,
                               std::string_view type) {
    out.push_back(0xf0); // SET with a type and no set name
    append_ident(out, type);
  }

  static void append_template_attribute(std::vector<std::uint8_t> &out,
                                        std::string_view label,
                                        std::uint8_t representation_code) {
    out.push_back(0x34); // ATTRIB with label and representation-code
    append_ident(out, label);
    out.push_back(representation_code);
  }

  static void append_object_start(std::vector<std::uint8_t> &out,
                                  std::string_view name) {
    out.push_back(0x70); // OBJECT with name
    append_obname(out, name);
  }

  void append_byte(std::uint8_t value) { bytes_.push_back(value); }

  void add_record(bool explicit_formatting, std::uint8_t type,
                  std::vector<std::uint8_t> body) {
    const auto segment_length = body.size() + 4U;
    require(segment_length <= std::numeric_limits<std::uint16_t>::max(),
            "fixture logical record segment must fit RP66 UNORM length");
    const auto visible_record_length = segment_length + 4U;
    require(visible_record_length <= std::numeric_limits<std::uint16_t>::max(),
            "fixture visible record must fit RP66 UNORM length");
    append_byte(static_cast<std::uint8_t>((visible_record_length >> 8U) & 0xffU));
    append_byte(static_cast<std::uint8_t>(visible_record_length & 0xffU));
    append_byte(0xff);
    append_byte(0x01);
    append_byte(static_cast<std::uint8_t>((segment_length >> 8U) & 0xffU));
    append_byte(static_cast<std::uint8_t>(segment_length & 0xffU));
    append_byte(explicit_formatting ? 0x80U : 0x00U);
    append_byte(type);
    for (const auto byte : body) {
      append_byte(byte);
    }
  }

  std::vector<std::uint8_t> bytes_;
};

std::vector<std::byte> representative_dlis() {
  Rp66Fixture fixture;
  fixture.add_logical_file();
  fixture.add_unsupported_object("TOOL");
  fixture.add_mismatched_object_set();
  fixture.add_frame("DEPTH-FRAME", {"DEPTH", "GR", "IMAGE"},
                    "BOREHOLE-DEPTH", "INCREASING");
  fixture.add_frame("TIME-FRAME", {"TIME", "DENS"}, "TIME", "INCREASING");
  fixture.add_channel("DEPTH", "m", 2, 1);
  fixture.add_channel("GR", "API", 2, 1);
  fixture.add_channel("IMAGE", "ohm.m", 2, 2);
  fixture.add_channel("TIME", "ms", 2, 1);
  fixture.add_channel("DENS", "g/cm3", 2, 1);
  fixture.add_fdata("DEPTH-FRAME",
                    {{1000.0F, 45.0F, 1.0F, 2.0F},
                     {1000.5F, std::numeric_limits<float>::quiet_NaN(), 3.0F,
                      4.0F},
                     {1001.0F, 50.0F, 5.0F, 6.0F}},
                    true);
  fixture.add_fdata("TIME-FRAME",
                    {{1.0F, 2.4F, 0.0F, 0.0F}, {2.0F, 2.5F, 0.0F, 0.0F}},
                    false);
  return std::move(fixture).finish();
}

std::vector<std::byte> backtracking_dlis() {
  Rp66Fixture fixture;
  fixture.add_logical_file();
  fixture.add_frame("DEPTH-FRAME", {"DEPTH", "GR"}, "BOREHOLE-DEPTH", "INCREASING");
  fixture.add_channel("DEPTH", "m", 2, 1);
  fixture.add_channel("GR", "API", 2, 1);
  fixture.add_fdata("DEPTH-FRAME",
                    {{1000.0F, 10.0F, 0.0F, 0.0F},
                     {1001.0F, 11.0F, 0.0F, 0.0F},
                     {1000.5F, 12.0F, 0.0F, 0.0F},
                     {1000.0F, 13.0F, 0.0F, 0.0F}},
                    false);
  return std::move(fixture).finish();
}

std::vector<std::byte> non_finite_axis_dlis() {
  Rp66Fixture fixture;
  fixture.add_logical_file();
  fixture.add_frame("DEPTH-FRAME", {"DEPTH", "GR"}, "BOREHOLE-DEPTH", "INCREASING");
  fixture.add_channel("DEPTH", "m", 2, 1);
  fixture.add_channel("GR", "API", 2, 1);
  fixture.add_fdata("DEPTH-FRAME",
                    {{std::numeric_limits<float>::quiet_NaN(), 10.0F, 0.0F, 0.0F},
                     {std::numeric_limits<float>::quiet_NaN(), 11.0F, 0.0F, 0.0F},
                     {std::numeric_limits<float>::quiet_NaN(), 12.0F, 0.0F, 0.0F}},
                    false);
  return std::move(fixture).finish();
}

std::vector<std::byte> partially_malformed_dlis() {
  Rp66Fixture fixture;
  fixture.add_logical_file();
  fixture.add_frame("DEPTH-FRAME", {"DEPTH", "GR"}, "BOREHOLE-DEPTH", "INCREASING");
  fixture.add_channel("DEPTH", "m", 2, 1);
  fixture.add_channel("GR", "API", 2, 1);
  fixture.add_fdata("DEPTH-FRAME", {{1000.0F, 10.0F, 0.0F, 0.0F}}, false);
  fixture.add_malformed_fdata("DEPTH-FRAME");
  return std::move(fixture).finish();
}

std::vector<std::byte> unsupported_encoding_dlis() {
  Rp66Fixture fixture;
  fixture.add_logical_file();
  fixture.add_frame("DEPTH-FRAME", {"DEPTH", "TEXT"}, "BOREHOLE-DEPTH", "INCREASING");
  fixture.add_channel("DEPTH", "m", 2, 1);
  fixture.add_channel("TEXT", "", 20, 1);
  fixture.add_fdata_with_ascii("DEPTH-FRAME", 1000.0F, "sample-note");
  return std::move(fixture).finish();
}

std::vector<std::byte> unknown_encoding_dlis() {
  Rp66Fixture fixture;
  fixture.add_logical_file();
  fixture.add_frame("DEPTH-FRAME", {"DEPTH"}, "BOREHOLE-DEPTH", "INCREASING");
  fixture.add_channel("DEPTH", "m", 28, 1);
  fixture.add_fdata_header_only("DEPTH-FRAME");
  return std::move(fixture).finish();
}

DlisObjectReference reference(std::string_view identifier) {
  return DlisObjectReference{.origin = 1, .copy_number = 0,
                             .identifier = std::string{identifier}};
}

DlisImport import_depth_frame(const std::vector<std::byte> &bytes) {
  const auto imported = DlisSourceAdapter::import(
      bytes,
      BufferSourceReference{.uri = "asset://well/Example-1.dlis",
                            .checksum = "content-v1"},
      DlisSelection{.logical_file_index = 0,
                    .frame = reference("DEPTH-FRAME"),
                    .channels = {reference("DEPTH"), reference("GR"),
                                 reference("IMAGE")}});
  require(imported.has_value(), "the selected DLIS frame must import");
  return imported.value();
}

void lists_explicit_logical_file_frame_and_channel_choices() {
  const auto bytes = representative_dlis();
  const auto inspected = DlisSourceAdapter::inspect(bytes);
  require(inspected.has_value(), "a representative DLIS source must inspect");

  const auto &catalog = inspected.value().catalog;
  require(catalog.logical_files.size() == 1,
          "the source must list its one logical file explicitly");
  const auto &logical_file = catalog.logical_files.front();
  require(logical_file.frames.size() == 2,
          "different DLIS Frames must remain separately selectable");
  require(logical_file.frames[0].reference == reference("DEPTH-FRAME") &&
              logical_file.frames[1].reference == reference("TIME-FRAME"),
          "frame choices must use stable source object references");
  require(logical_file.frames[0].channels.size() == 3 &&
              logical_file.frames[0].channels[1].reference == reference("GR") &&
              logical_file.frames[0].channels[1].unit == "API" &&
              logical_file.frames[0].channels[2].dimensions ==
                  std::vector<std::uint32_t>{2U},
          "channel choices must retain identity, unit, and dimensionality");

  const auto unsupported_count = static_cast<std::size_t>(std::count_if(
      inspected.value().diagnostics.begin(), inspected.value().diagnostics.end(),
      [](const DlisDiagnostic &diagnostic) {
        return diagnostic.code == DlisDiagnosticCode::unsupported_object_type &&
               diagnostic.byte_offset > 80;
      }));
  require(unsupported_count == 2,
          "unsupported DLIS object records and object-set mismatches must be locatable");

  const auto missing_selection = DlisSourceAdapter::import(
      bytes, BufferSourceReference{.uri = "asset://well/Example-1.dlis", .checksum = {}},
      DlisSelection{});
  require(!missing_selection.has_value() &&
              missing_selection.error().code == ErrorCode::invalid_document,
          "DLIS import must reject an implicit frame/channel choice");
}

void normalizes_a_selected_frame_through_graphic_table_and_svg_seams() {
  const auto bytes = representative_dlis();
  const auto imported = import_depth_frame(bytes);
  const auto &document = imported.document;

  require(document.sampling_axes().size() == 1 && document.curves().size() == 1,
          "the selected Frame must become one Sampling Axis plus selected scalar curves");
  const auto &axis = document.sampling_axes().front();
  const auto &curve = document.curves().front();
  require(axis.unit == "m" && axis.direction == AxisDirection::increasing &&
              curve.mnemonic == "GR" && curve.unit == "API" &&
              curve.sampling_axis_id == axis.id,
          "the adapter must preserve selected channel metadata and its Frame axis");
  require(axis.coordinates.length() == 3 && curve.values.length() == 3,
          "the adapter must preserve every selected Frame sample");
  require_near(*axis.coordinates.value_as_double(1), 1000.5,
               "the selected Frame axis must preserve source sample order");
  require(curve.nulls.is_null(1),
          "non-finite selected channel values must be represented as Null samples");

  bool saw_matrix_diagnostic{};
  for (const auto &diagnostic : imported.diagnostics) {
    saw_matrix_diagnostic = saw_matrix_diagnostic ||
                            (diagnostic.code ==
                                 DlisDiagnosticCode::unsupported_channel_dimension &&
                             diagnostic.channel == reference("IMAGE") &&
                             diagnostic.byte_offset > 0);
  }
  require(saw_matrix_diagnostic,
          "unsupported selected channel dimensions must produce a locatable diagnostic");

  const auto repeat = import_depth_frame(bytes);
  require(repeat.document.id() == document.id() &&
              repeat.document.sampling_axes().front().id == axis.id &&
              repeat.document.curves().front().id == curve.id,
          "source identity plus explicit DLIS selection must produce stable identities");

  const auto tables = TableProjectionBuilder::from_document(document);
  require(tables.size() == 1 && tables.front().row_count() == 3 &&
              tables.front().column_count() == 2,
          "the normalized DLIS selection must enter the table seam without resampling");

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "the normalized DLIS selection must be accepted by the session seam");

  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1001.0},
      Millimetres{80.0}, "font-fixture-v1");
  const auto track_id = id("16500000-0000-4000-8000-000000000001");
  const auto scale_id = id("16500000-0000-4000-8000-000000000002");
  const auto layer_id = id("16500000-0000-4000-8000-000000000003");
  presentation.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 1});
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
      .curve_id = curve.id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 1, .green = 2, .blue = 3, .alpha = 255},
      .line_width = Millimetres{0.25},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "the selected DLIS curve must enter the graphic seam");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && scene->curve_layers().size() == 1 &&
              scene->curve_points().size() == 2,
          "the graphic scene must retain selected scalar samples and null discontinuity");
  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value() && svg.value().text().find("<path") != std::string_view::npos,
          "the selected DLIS curve must traverse the SVG export seam");
}

void splits_local_axis_backtracks_without_reordering_samples() {
  const auto bytes = backtracking_dlis();
  const auto imported = DlisSourceAdapter::import(
      bytes, BufferSourceReference{.uri = "asset://well/backtracking.dlis", .checksum = {}},
      DlisSelection{.logical_file_index = 0,
                    .frame = reference("DEPTH-FRAME"),
                    .channels = {reference("DEPTH"), reference("GR")}});
  require(imported.has_value(), "a local axis backtrack must remain importable");
  const auto &document = imported.value().document;
  require(document.sampling_axes().size() == 2 && document.curves().size() == 2,
          "a local axis reversal must become two aligned monotonic segments");
  const auto first_axis = document.sampling_axes()[0];
  const auto second_axis = document.sampling_axes()[1];
  require(first_axis.direction == AxisDirection::increasing &&
              second_axis.direction == AxisDirection::decreasing &&
              first_axis.coordinates.length() == 2 && second_axis.coordinates.length() == 2,
          "each local backtrack segment must retain its own monotonic direction");
  require_near(*first_axis.coordinates.value_as_double(0), 1000.0,
               "the first segment must preserve the first source coordinate");
  require_near(*first_axis.coordinates.value_as_double(1), 1001.0,
               "the first segment must preserve source order");
  require_near(*second_axis.coordinates.value_as_double(0), 1000.5,
               "the second segment must begin at the unsorted backtrack sample");
  require_near(*second_axis.coordinates.value_as_double(1), 1000.0,
               "the second segment must preserve all remaining source samples");
}

void bounds_corrupt_and_oversized_sources_before_decoding_records() {
  const auto bytes = representative_dlis();
  const auto oversized = DlisSourceAdapter::inspect(
      bytes, DlisLimits{.max_input_bytes = 64, .max_logical_record_bytes = 1024,
                        .max_logical_records = 128, .max_samples = 1024});
  require(!oversized.has_value() &&
              oversized.error().code == ErrorCode::resource_exhausted,
          "configured input limits must reject a large source before parsing");

  auto truncated = bytes;
  truncated.resize(83);
  const auto malformed = DlisSourceAdapter::inspect(truncated);
  require(!malformed.has_value() &&
              malformed.error().code == ErrorCode::invalid_document,
          "a truncated visible-record header must not be read past its source buffer");

  auto corrupt = bytes;
  corrupt[81] = static_cast<std::byte>(3);
  const auto corrupted = DlisSourceAdapter::inspect(corrupt);
  require(!corrupted.has_value() &&
              corrupted.error().code == ErrorCode::invalid_document,
          "a corrupt visible-record length must be rejected without decoding payload bytes");

  const auto large_record = DlisSourceAdapter::inspect(
      bytes, DlisLimits{.max_input_bytes = 1024 * 1024, .max_logical_record_bytes = 8,
                        .max_logical_records = 128, .max_samples = 1024});
  require(!large_record.has_value() &&
              large_record.error().code == ErrorCode::resource_exhausted,
          "a logical-record ceiling must apply before a large record is accumulated");

  const auto invalid_axis_import = DlisSourceAdapter::import(
      non_finite_axis_dlis(), BufferSourceReference{},
      DlisSelection{.logical_file_index = 0,
                    .frame = reference("DEPTH-FRAME"),
                    .channels = {reference("DEPTH"), reference("GR")}},
      DlisLimits{.max_input_bytes = 1024 * 1024, .max_logical_record_bytes = 1024,
                 .max_logical_records = 128, .max_samples = 2});
  require(!invalid_axis_import.has_value() &&
              invalid_axis_import.error().code == ErrorCode::resource_exhausted,
          "non-finite axis rows must count against the sample ceiling");
}

void preserves_good_frame_data_after_a_recoverable_record_error() {
  const auto imported = DlisSourceAdapter::import(
      partially_malformed_dlis(), BufferSourceReference{},
      DlisSelection{.logical_file_index = 0,
                    .frame = reference("DEPTH-FRAME"),
                    .channels = {reference("DEPTH"), reference("GR")}});
  require(imported.has_value(),
          "a malformed frame-data record must not reject earlier valid frame data");
  require(imported.value().document.sampling_axes().size() == 1 &&
              imported.value().document.sampling_axes().front().coordinates.length() == 1,
          "valid frame data preceding a malformed record must remain available");
  const auto malformed = std::find_if(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const DlisDiagnostic &diagnostic) {
        return diagnostic.code == DlisDiagnosticCode::malformed_frame_data &&
               diagnostic.byte_offset > 80;
      });
  require(malformed != imported.value().diagnostics.end(),
          "recoverable frame-data errors must identify the affected source record");
}

void reports_unsupported_channel_encodings_at_their_metadata_record() {
  const auto imported = DlisSourceAdapter::import(
      unsupported_encoding_dlis(), BufferSourceReference{},
      DlisSelection{.logical_file_index = 0,
                    .frame = reference("DEPTH-FRAME"),
                    .channels = {reference("DEPTH"), reference("TEXT")}});
  require(imported.has_value(),
          "an unsupported non-axis channel encoding must not reject valid axis samples");
  const auto diagnostic = std::find_if(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const DlisDiagnostic &candidate) {
        return candidate.code == DlisDiagnosticCode::unsupported_channel_representation &&
               candidate.channel == reference("TEXT") && candidate.byte_offset > 80;
      });
  require(diagnostic != imported.value().diagnostics.end(),
          "unsupported channel encodings must have a locatable metadata diagnostic");
}

void retains_unknown_encoding_diagnostics_when_frame_data_cannot_be_decoded() {
  const auto imported = DlisSourceAdapter::import(
      unknown_encoding_dlis(), BufferSourceReference{},
      DlisSelection{.logical_file_index = 0,
                    .frame = reference("DEPTH-FRAME"),
                    .channels = {reference("DEPTH")}});
  require(imported.has_value(),
          "an unknown representation must return its source diagnostic, not a generic error");
  require(imported.value().document.sampling_axes().empty() &&
              imported.value().document.curves().empty(),
          "an undecodable selected frame must not synthesize axis or curve samples");
  const auto diagnostic = std::find_if(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const DlisDiagnostic &candidate) {
        return candidate.code == DlisDiagnosticCode::unsupported_channel_representation &&
               candidate.channel == reference("DEPTH") && candidate.byte_offset > 80;
      });
  require(diagnostic != imported.value().diagnostics.end(),
          "an unknown representation must identify its metadata record");
}

} // namespace

int main() {
  lists_explicit_logical_file_frame_and_channel_choices();
  normalizes_a_selected_frame_through_graphic_table_and_svg_seams();
  splits_local_axis_backtracks_without_reordering_samples();
  bounds_corrupt_and_oversized_sources_before_decoding_records();
  preserves_good_frame_data_after_a_recoverable_record_error();
  reports_unsupported_channel_encodings_at_their_metadata_record();
  retains_unknown_encoding_diagnostics_when_frame_data_cannot_be_decoded();
  return EXIT_SUCCESS;
}
