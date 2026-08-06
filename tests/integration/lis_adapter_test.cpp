#include <welllog/export/svg.hpp>
#include <welllog/io/lis.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
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
  if (std::fabs(actual - expected) > 1.0e-6) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

class LisFixture {
public:
  void add_file_header() { add_record(128U, {}); }

  void add_unknown_record() { add_record(1U, {0U, 1U, 2U}); }

  void add_null_padding() { bytes_.push_back(0U); }

  void
  add_format_specification(std::string_view depth_unit = "FT",
                           std::string_view index_mnemonic = "DEPT",
                           std::string_view gamma_mnemonic = "GR",
                           std::string_view gamma_unit = "API",
                           std::optional<double> absent_value = std::nullopt) {
    std::vector<std::uint8_t> body;
    append_entry_i32(body, 3U, 12); // frame size: DEPT, GR, RHOB
    if (absent_value.has_value()) {
      body.insert(body.end(), {12U, 4U, 68U});
      append_lis_f32(body, *absent_value);
    }
    body.insert(body.end(), {0U, 0U, 0U}); // entry-block terminator
    append_spec_block(body, index_mnemonic, depth_unit, 4U, 1U, 68U);
    append_spec_block(body, gamma_mnemonic, gamma_unit, 4U, 1U, 68U);
    append_spec_block(body, "RHOB", "G/CC", 4U, 1U, 68U);
    add_record(64U, std::move(body));
  }

  void add_non_scalar_format_specification() {
    std::vector<std::uint8_t> body;
    append_entry_i32(body, 3U, 12); // DEPT plus a two-sample image channel.
    body.insert(body.end(), {0U, 0U, 0U});
    append_spec_block(body, "DEPT", "M", 4U, 1U, 68U);
    append_spec_block(body, "IMG", "", 8U, 2U, 68U);
    add_record(64U, std::move(body));
  }

  void add_normal_data(double first_depth = 1000.0,
                       double first_gamma_ray = 45.0) {
    add_normal_data({{{first_depth, first_gamma_ray, 2.30},
                      {first_depth + 1.0, first_gamma_ray + 1.0, 2.31},
                      {first_depth + 2.0, first_gamma_ray + 2.0, 2.32}}});
  }

  void add_normal_data(const std::vector<std::array<double, 3>> &frames) {
    std::vector<std::uint8_t> body;
    for (const auto &frame : frames) {
      append_frame(body, frame[0U], frame[1U], frame[2U]);
    }
    add_record(0U, std::move(body));
  }

  void add_non_scalar_data() {
    std::vector<std::uint8_t> body;
    append_lis_f32(body, 1000.0);
    append_lis_f32(body, 1.0);
    append_lis_f32(body, 2.0);
    add_record(0U, std::move(body));
  }

  void add_malformed_normal_data() {
    add_record(0U, std::vector<std::uint8_t>(11U, 0U));
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

  static void append_i32(std::vector<std::uint8_t> &out, std::int32_t value) {
    const auto encoded = static_cast<std::uint32_t>(value);
    out.push_back(static_cast<std::uint8_t>((encoded >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((encoded >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((encoded >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(encoded & 0xffU));
  }

  static void append_lis_f32(std::vector<std::uint8_t> &out, double value) {
    if (value == 0.0) {
      out.insert(out.end(), 4U, 0U);
      return;
    }
    int exponent{};
    const auto fraction = std::frexp(std::fabs(value), &exponent);
    const auto significand = static_cast<std::uint32_t>(
        std::llround(fraction * static_cast<double>(1U << 23U)));
    const auto negative = value < 0.0;
    const auto exponent_bits =
        negative ? static_cast<std::uint8_t>(
                       ~static_cast<std::uint8_t>(exponent + 128))
                 : static_cast<std::uint8_t>(exponent + 128);
    const auto fraction_bits =
        negative ? static_cast<std::uint32_t>((~significand + 1U) & 0x007fffffU)
                 : significand & 0x007fffffU;
    const auto encoded = (negative ? 0x80000000U : 0U) |
                         (static_cast<std::uint32_t>(exponent_bits) << 23U) |
                         fraction_bits;
    out.push_back(static_cast<std::uint8_t>((encoded >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((encoded >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((encoded >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(encoded & 0xffU));
  }

  static void append_entry_i32(std::vector<std::uint8_t> &out,
                               std::uint8_t type, std::int32_t value) {
    out.push_back(type);
    out.push_back(4U);
    out.push_back(73U);
    append_i32(out, value);
  }

  static void
  append_spec_block(std::vector<std::uint8_t> &out, std::string_view mnemonic,
                    std::string_view unit, std::uint16_t reserved_size,
                    std::uint8_t samples, std::uint8_t representation) {
    if (mnemonic.size() > 4U || unit.size() > 4U) {
      fail("test mnemonic and unit must fit LIS79 spec-block fields");
    }
    std::vector<std::uint8_t> block(40U, static_cast<std::uint8_t>(' '));
    for (std::size_t index{}; index < mnemonic.size(); ++index) {
      block[index] = static_cast<std::uint8_t>(mnemonic[index]);
    }
    for (std::size_t index{}; index < unit.size(); ++index) {
      block[18U + index] = static_cast<std::uint8_t>(unit[index]);
    }
    block[28U] = static_cast<std::uint8_t>((reserved_size >> 8U) & 0xffU);
    block[29U] = static_cast<std::uint8_t>(reserved_size & 0xffU);
    block[33U] = samples;
    block[34U] = representation;
    out.insert(out.end(), block.begin(), block.end());
  }

  static void append_frame(std::vector<std::uint8_t> &out, double depth,
                           double gamma_ray, double density) {
    append_lis_f32(out, depth);
    append_lis_f32(out, gamma_ray);
    append_lis_f32(out, density);
  }

  void add_record(std::uint8_t type, std::vector<std::uint8_t> body) {
    const auto length = body.size() + 6U;
    if (length > 0xffffU) {
      fail("LIS test record must fit a physical record");
    }
    append_u16(bytes_, static_cast<std::uint16_t>(length));
    append_u16(bytes_, 0U);
    bytes_.push_back(type);
    bytes_.push_back(0U);
    bytes_.insert(bytes_.end(), body.begin(), body.end());
  }

  std::vector<std::uint8_t> bytes_;
};

[[nodiscard]] std::vector<std::byte>
representative_lis(double first_gamma_ray = 45.0) {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification();
  fixture.add_normal_data(1000.0, first_gamma_ray);
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> two_logical_files_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification();
  fixture.add_normal_data();
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_normal_data(2000.0, 90.0);
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> backtracking_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_normal_data({{{1000.0, 10.0, 2.10},
                            {1001.0, 11.0, 2.11},
                            {1001.0, 12.0, 2.12},
                            {1000.5, 13.0, 2.13},
                            {1000.0, 14.0, 2.14}}});
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> inferred_null_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_normal_data(
      {{{1000.0, 45.0, 2.30}, {1001.0, -999.25, 2.31}, {1002.0, 47.0, 2.32}}});
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> source_index_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("MS", "TIME");
  fixture.add_normal_data(
      {{{100.0, 45.0, 2.30}, {200.0, 46.0, 2.31}, {300.0, 47.0, 2.32}}});
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> multiple_data_sets_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_normal_data(1000.0, 40.0);
  fixture.add_format_specification("M");
  fixture.add_normal_data(2000.0, 90.0);
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> collision_prone_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  std::vector<std::array<double, 3>> frames;
  frames.reserve(329U);
  for (std::size_t index{}; index < 329U; ++index) {
    const auto coordinate = index < 165U
                                ? 1000.0 + static_cast<double>(index)
                                : 1328.0 - static_cast<double>(index);
    frames.push_back(
        {coordinate, 40.0 + static_cast<double>(index), 2.30});
  }
  fixture.add_normal_data(frames);
  // These six padding bytes place the next DFS at offset 4102. Legacy curve
  // IDs then collide: 6 + 4096 + 1 == 4102 + 1.
  for (std::size_t index{}; index < 6U; ++index) {
    fixture.add_null_padding();
  }
  fixture.add_format_specification("M");
  fixture.add_normal_data(2000.0, 90.0);
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> non_scalar_only_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_non_scalar_format_specification();
  fixture.add_non_scalar_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> redundant_format_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_format_specification("M");
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> unknown_record_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_unknown_record();
  fixture.add_format_specification("M");
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> locally_malformed_data_set_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_malformed_normal_data();
  fixture.add_format_specification("M");
  fixture.add_normal_data(2000.0, 90.0);
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> custom_alias_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M", "DEPT", "XGR", "API");
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> padded_direct_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_null_padding();
  fixture.add_format_specification("M");
  fixture.add_null_padding();
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> explicit_null_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M", "DEPT", "GR", "API", -999.0);
  fixture.add_normal_data({{{1000.0, -999.25, 2.30},
                            {1001.0, -999.0, 2.31},
                            {1002.0, 50.0, 2.32}}});
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> constant_axis_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M");
  fixture.add_normal_data(
      {{{1000.0, 45.0, 2.30}, {1000.0, 46.0, 2.31}, {1000.0, 47.0, 2.32}}});
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> spaced_mnemonic_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M", "DEPT", "D TC", "US/M");
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> incompatible_unit_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M", "DEPT", "RHOB", "API");
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> non_ascii_text_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  fixture.add_format_specification("M", "DEPT", "GR", "\xe9");
  fixture.add_normal_data();
  return std::move(fixture).finish();
}

[[nodiscard]] std::vector<std::byte> header_only_lis() {
  LisFixture fixture;
  fixture.add_file_header();
  return std::move(fixture).finish();
}

void append_le32(std::vector<std::byte> &out, std::uint32_t value) {
  for (std::uint32_t index{}; index < 4U; ++index) {
    out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

[[nodiscard]] std::vector<std::byte> tif_wrapped_lis() {
  const auto source = representative_lis();
  std::vector<std::byte> result;
  result.reserve(source.size() + 36U);
  append_le32(result, 0U); // beginning-of-file tape mark
  append_le32(result, 0U);
  append_le32(result, static_cast<std::uint32_t>(source.size() + 12U));
  result.insert(result.end(), source.begin(), source.end());
  append_le32(result, 1U); // end-of-file tape marks
  append_le32(result, 0U);
  append_le32(result, static_cast<std::uint32_t>(source.size() + 24U));
  append_le32(result, 1U);
  append_le32(result, static_cast<std::uint32_t>(source.size() + 24U));
  append_le32(result, static_cast<std::uint32_t>(source.size() + 36U));
  return result;
}

[[nodiscard]] std::vector<std::byte>
rp66_v1_rejection_fixture(bool also_valid_direct_lis79) {
  // With the polyglot sequence number, the first two spaces encode a LIS79
  // physical-record length of 0x2020 and "10" is a legal no-link attribute
  // word. The RP66 record at offset 80 remains independently valid.
  const auto size = also_valid_direct_lis79 ? 0x2020U : 100U;
  std::vector<std::byte> result(size, static_cast<std::byte>(' '));
  const auto write_ascii = [&result](std::size_t offset,
                                     std::string_view value) {
    for (std::size_t index{}; index < value.size(); ++index) {
      result[offset + index] = static_cast<std::byte>(value[index]);
    }
  };
  write_ascii(0U, also_valid_direct_lis79 ? "  10" : "   1");
  write_ascii(4U, "V1.00");
  write_ascii(9U, "RECORD");
  write_ascii(15U, " 8192");
  write_ascii(20U, "Paleo Workbench RP66 rejection fixture");

  result[80U] = static_cast<std::byte>(0x00U);
  result[81U] = static_cast<std::byte>(0x14U);
  result[82U] = static_cast<std::byte>(0xffU);
  result[83U] = static_cast<std::byte>(0x01U);
  result[84U] = static_cast<std::byte>(0x00U);
  result[85U] = static_cast<std::byte>(0x10U);
  result[86U] = static_cast<std::byte>(0x80U);
  result[87U] = static_cast<std::byte>(0x00U);
  return result;
}

void test_default_profile_normalizes_a_selected_lis79_logical_file() {
  const auto source_bytes = representative_lis();
  const auto inspection = LisSourceAdapter::inspect(source_bytes);
  require(inspection.has_value(),
          "a structurally valid LIS79 stream must inspect");
  require(inspection.value().catalog.logical_files.size() == 1U,
          "one file header must expose one logical-file choice");

  const auto imported = LisSourceAdapter::import(
      source_bytes,
      BufferSourceReference{.uri = "asset://well/A1.lis", .checksum = "a1-v1"},
      LisSelection{.logical_file_index = 0U});
  require(imported.has_value(), "the selected LIS79 logical file must import");

  const auto &document = imported.value().document;
  require(document.revision() == DocumentRevision{1},
          "a first LIS79 import must begin at revision one");
  require(document.sampling_axes().size() == 1U,
          "one normal LIS data run must create one sampling axis");
  require(document.curves().size() == 2U,
          "the index must not be duplicated as a curve");

  const auto &axis = document.sampling_axes().front();
  require(axis.domain == DepthDomain::measured_depth,
          "DEPT with a length unit must normalize to measured depth");
  require(axis.unit == "m", "feet must normalize to metres");
  require_near(*axis.coordinates.value_as_double(0U), 304.8,
               "the depth values must convert from feet to metres");
  require_near(*axis.coordinates.value_as_double(2U), 305.4096,
               "each depth sample must convert independently");

  const auto &gamma_ray = document.curves()[0U];
  require(gamma_ray.mnemonic == "GR", "GR must retain canonical mnemonic");
  require(gamma_ray.display_name == "GR",
          "the raw source mnemonic is displayed");
  require(gamma_ray.unit == "API", "GR must retain API units");
  require_near(*gamma_ray.values.value_as_double(1U), 46.0,
               "GR values must remain numerically stable");

  const auto &density = document.curves()[1U];
  require(density.mnemonic == "DEN", "RHOB must normalize to density");
  require(density.display_name == "RHOB",
          "density must retain its source name");
  require(density.unit == "g/cm3", "source density spelling must normalize");
  require_near(*density.values.value_as_double(2U), 2.32,
               "density values in canonical units must remain stable");

  const auto tables = TableProjectionBuilder::from_document(document);
  require(tables.size() == 1U && tables.front().row_count() == 3U &&
              tables.front().column_count() == 3U,
          "an imported LIS document must enter the standard table projection");
}

void test_imported_lis_enters_the_standard_svg_consumer_path() {
  const auto imported = LisSourceAdapter::import(
      representative_lis(), BufferSourceReference{.uri = "asset://well/svg.lis",
                                                  .checksum = "svg-v1"});
  require(imported.has_value(),
          "a representative LIS document must import for SVG");
  const auto &document = imported.value().document;
  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "an imported LIS document must enter a WellLogSession");

  const auto track_id = id("16600000-0000-4000-8000-000000000001");
  const auto scale_id = id("16600000-0000-4000-8000-000000000002");
  const auto layer_id = id("16600000-0000-4000-8000-000000000003");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 304.8,
                          .bottom = 305.4096},
      Millimetres{80.0}, "font-fixture-v1");
  presentation.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 1});
  presentation.add_scale(
      TrackScaleSpec{.id = scale_id,
                     .track_id = track_id,
                     .mode = ScaleMode::linear,
                     .minimum = 0.0,
                     .maximum = 100.0,
                     .direction = ScaleDirection::left_to_right,
                     .unit = "API"});
  presentation.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = document.curves().front().id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 1, .green = 2, .blue = 3, .alpha = 255},
      .line_width = Millimetres{0.25},
      .z_order = 2,
      .visible = true,
  });
  require(
      session.execute(SetPresentationCommand{presentation.build()}).has_value(),
      "the standard scene seam must accept an imported LIS curve");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && !scene->curve_points().empty(),
          "the imported LIS curve must prepare into the standard scene");
  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value() &&
              svg.value().text().find("<path") != std::string_view::npos,
          "the imported LIS curve must export through the standard SVG writer");
}

void test_multiple_logical_files_require_an_explicit_selection() {
  const auto source_bytes = two_logical_files_lis();
  const auto inspection = LisSourceAdapter::inspect(source_bytes);
  require(inspection.has_value(), "a multi-file LIS79 stream must inspect");
  require(inspection.value().catalog.logical_files.size() == 2U,
          "two file headers must expose two logical-file choices");

  const auto implicit_import = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/two-files.lis",
                                          .checksum = "two-v1"});
  require(!implicit_import.has_value(),
          "an import must not silently choose among multiple logical files");

  const auto selected_import = LisSourceAdapter::import(
      source_bytes,
      BufferSourceReference{.uri = "asset://well/two-files.lis",
                            .checksum = "two-v1"},
      LisSelection{.logical_file_index = 1U});
  require(selected_import.has_value(),
          "the selected second logical file must import");
  const auto &document = selected_import.value().document;
  require_near(
      *document.sampling_axes().front().coordinates.value_as_double(0U), 2000.0,
      "the selected logical file must not import the first file's depth");
  require_near(
      *document.curves().front().values.value_as_double(0U), 90.0,
      "the selected logical file must not import the first file's curves");
}

void test_backtracking_index_creates_separate_axes_without_sorting() {
  const auto source_bytes = backtracking_lis();
  const auto imported = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/backtrack.lis",
                                          .checksum = "back-v1"});
  require(imported.has_value(),
          "a backtracking LIS data run must remain importable");

  const auto &document = imported.value().document;
  require(document.sampling_axes().size() == 2U,
          "a first index reversal must start a separate sampling axis");
  const auto &increasing = document.sampling_axes()[0U];
  require(increasing.direction == AxisDirection::increasing,
          "the first non-repeated difference determines the first direction");
  require(increasing.coordinates.length() == 3U,
          "repeated depth samples must stay in the first source segment");
  require_near(*increasing.coordinates.value_as_double(2U), 1001.0,
               "repeated depth values must retain source order");

  const auto &decreasing = document.sampling_axes()[1U];
  require(decreasing.direction == AxisDirection::decreasing,
          "the post-reversal axis must retain its physical direction");
  require(
      decreasing.coordinates.length() == 2U,
      "the reversal sample must begin rather than duplicate the new segment");
  require_near(*decreasing.coordinates.value_as_double(0U), 1000.5,
               "the reversal sample must remain at the start of the new axis");
  require(document.curves().size() == 4U,
          "each scalar curve must be represented for each source-axis segment");
  require(document.curves()[2U].sampling_axis_id == decreasing.id,
          "the second GR curve must bind to the second source-axis segment");
}

void test_profile_infers_nulls_in_the_source_numeric_domain() {
  const auto source_bytes = inferred_null_lis();
  const auto imported = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/null.lis",
                                          .checksum = "null-v1"});
  require(imported.has_value(),
          "an inferred-null curve must remain importable");

  const auto &gamma_ray = imported.value().document.curves()[0U];
  require(
      gamma_ray.nulls.is_null(1U),
      "the raw -999.25 sentinel must become a Curve null before conversion");
  require(!gamma_ray.nulls.is_null(0U), "ordinary values must remain plotted");
  require(std::isnan(*gamma_ray.values.value_as_double(1U)),
          "a normalized null must not retain a plottable numeric value");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::inferred_null &&
               diagnostic.severity == Severity::info &&
               diagnostic.channel_name == "GR";
      });
  require(reported,
          "inferred null conversion must be visible in the import audit");
}

void test_unrecognized_numeric_index_is_not_misrepresented_as_depth() {
  const auto source_bytes = source_index_lis();
  const auto imported = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/time.lis",
                                          .checksum = "time-v1"});
  require(imported.has_value(),
          "a numeric source index must remain importable");
  const auto &axis = imported.value().document.sampling_axes().front();
  require(axis.domain == DepthDomain::source_index,
          "TIME must not be guessed to be a measured-depth axis");
  require(axis.unit == "MS", "a source-index axis must retain its source unit");
  require_near(*axis.coordinates.value_as_double(2U), 300.0,
               "a source-index axis must retain unconverted values");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::unknown_index_semantics &&
               diagnostic.severity == Severity::warning;
      });
  require(reported,
          "the unrecognized index semantic must be visible in the audit");
}

void test_each_lis_data_set_retains_its_own_axis_and_curves() {
  const auto source_bytes = multiple_data_sets_lis();
  const auto imported = LisSourceAdapter::import(
      source_bytes,
      BufferSourceReference{.uri = "asset://well/two-data-sets.lis",
                            .checksum = "sets-v1"});
  require(imported.has_value(),
          "two normal LIS data sets must import together");
  const auto &document = imported.value().document;
  require(document.sampling_axes().size() == 2U,
          "each LIS data set must own a distinct sampling axis");
  require(document.curves().size() == 4U,
          "each data set must retain both source scalar curves");
  require_near(*document.sampling_axes()[1U].coordinates.value_as_double(0U),
               2000.0,
               "the second data set must retain its own source coordinates");
  require(document.curves()[2U].sampling_axis_id ==
              document.sampling_axes()[1U].id,
          "the second data set curves must bind to its own axis");
  require_near(
      *document.curves()[2U].values.value_as_double(0U), 90.0,
      "the second data set GR values must not be merged with the first");
}

void test_valid_non_scalar_lis_data_set_becomes_an_empty_document() {
  const auto source_bytes = non_scalar_only_lis();
  const auto imported = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/image.lis",
                                          .checksum = "image-v1"});
  require(
      imported.has_value(),
      "a structurally valid data set with no scalar curve must still import");
  require(imported.value().document.sampling_axes().empty(),
          "a data set without scalar curves must not leave an orphan axis");
  require(imported.value().document.curves().empty(),
          "non-scalar LIS channels must not be coerced into Core curves");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::no_importable_curve &&
               diagnostic.severity == Severity::warning;
      });
  require(reported,
          "a zero-curve import must explain why no scalar data was retained");
}

void test_tif_wrapped_lis79_is_detected_structurally() {
  const auto source_bytes = tif_wrapped_lis();
  const auto inspection = LisSourceAdapter::inspect(source_bytes);
  require(inspection.has_value(),
          "a valid TIF envelope must be detected before LIS parsing");
  const auto imported = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/tif.lis",
                                          .checksum = "tif-v1"});
  require(imported.has_value(),
          "the unique TIF-wrapped LIS79 parse must import");
  require(imported.value().document.curves().size() == 2U,
          "TIF unwrapping must preserve the enclosed LIS curves");
}

void test_rp66_v1_family_is_rejected_instead_of_guessed_as_lis79() {
  const auto unsupported = rp66_v1_rejection_fixture(false);
  const auto unsupported_inspection = LisSourceAdapter::inspect(unsupported);
  require(!unsupported_inspection.has_value() &&
              unsupported_inspection.error().code ==
                  ErrorCode::invalid_document,
          "an RP66 V1 envelope without a LIS79 parse must be rejected");
  const auto unsupported_import = LisSourceAdapter::import(
      unsupported,
      BufferSourceReference{.uri = "asset://well/rp66-v1.dlis",
                            .checksum = "rp66-v1"});
  require(!unsupported_import.has_value() &&
              unsupported_import.error().code == ErrorCode::invalid_document,
          "the LIS importer must not guess-read an unsupported RP66 V1 file");

  const auto ambiguous = rp66_v1_rejection_fixture(true);
  const auto ambiguous_inspection = LisSourceAdapter::inspect(ambiguous);
  require(!ambiguous_inspection.has_value() &&
              ambiguous_inspection.error().code ==
                  ErrorCode::invalid_document,
          "bytes valid as both RP66 V1 and direct LIS79 must inspect as "
          "ambiguous");
  const auto ambiguous_import = LisSourceAdapter::import(
      ambiguous,
      BufferSourceReference{.uri = "asset://well/rp66-lis79-polyglot.lis",
                            .checksum = "rp66-lis79-polyglot"});
  require(!ambiguous_import.has_value() &&
              ambiguous_import.error().code == ErrorCode::invalid_document,
          "bytes valid as both RP66 V1 and direct LIS79 must not import");
}

void test_import_identity_versions_content_and_prevents_curve_collisions() {
  const auto source_bytes = representative_lis();
  const BufferSourceReference source{.uri = "asset://well/stable.lis",
                                     .checksum = "stable-v1"};
  const auto v1 = LisSourceAdapter::import(
      source_bytes, source,
      LisSelection{.identity_scheme = LisIdentityScheme::resform_compatible_v1});
  require(v1.has_value(), "the explicit legacy identity scheme must import");
  require(v1.value().document.id() ==
              id("0dfa0e2f-01b0-4bee-bd7f-9398b6a2cb47"),
          "the explicit v1 scheme must preserve its established document "
          "identity");
  require(v1.value().document.sampling_axes().front().id ==
              id("638fffe8-49fd-4133-93b4-371e1452bfdb"),
          "the explicit v1 scheme must preserve its established axis identity");
  require(v1.value().document.curves()[0U].id ==
              id("02dcfe9f-71af-49a3-b291-473851e1a00d") &&
              v1.value().document.curves()[1U].id ==
                  id("6d0b4718-d53c-40f8-9dc0-8fb0b46e6762"),
          "the explicit v1 scheme must preserve its established curve "
          "identities");

  const auto first = LisSourceAdapter::import(source_bytes, source);
  const auto second = LisSourceAdapter::import(source_bytes, source);
  require(first.has_value() && second.has_value(),
          "repeat imports must remain valid");
  require(
      first.value().document.id() == second.value().document.id(),
      "same content and normalization profile must preserve document identity");
  require(first.value().document.sampling_axes().front().id ==
              second.value().document.sampling_axes().front().id,
          "repeat imports must preserve sampling-axis identities");
  require(first.value().document.curves().front().id ==
              second.value().document.curves().front().id,
          "repeat imports must preserve curve identities");
  require(first.value().document.revision() == DocumentRevision{1} &&
              second.value().document.revision() == DocumentRevision{1},
          "repeat import does not manufacture a new revision");

  require(first.value().document.id() != v1.value().document.id(),
          "content-bound v2 must be distinct from the legacy v1 identity");
  const auto changed_content = LisSourceAdapter::import(
      representative_lis(46.0), source);
  require(changed_content.has_value() &&
              changed_content.value().document.id() != first.value().document.id(),
          "content-bound v2 must not trust a reused source checksum over "
          "different bytes");

  const auto collision_prone = LisSourceAdapter::import(
      collision_prone_lis(),
      BufferSourceReference{.uri = "asset://well/collision.lis",
                            .checksum = "collision-v1"});
  require(collision_prone.has_value(),
          "content-bound v2 must import a stream with legacy ordinal overlap");
  const auto &collision_document = collision_prone.value().document;
  for (std::size_t left{}; left < collision_document.sampling_axes().size();
       ++left) {
    for (std::size_t right = left + 1U;
         right < collision_document.sampling_axes().size(); ++right) {
      require(collision_document.sampling_axes()[left].id !=
                  collision_document.sampling_axes()[right].id,
              "each LIS data-set segment must have a distinct axis identity");
    }
  }
  for (std::size_t left{}; left < collision_document.curves().size(); ++left) {
    for (std::size_t right = left + 1U;
         right < collision_document.curves().size(); ++right) {
      require(collision_document.curves()[left].id !=
                  collision_document.curves()[right].id,
              "each LIS data-set curve must have a distinct curve identity");
    }
  }

  auto changed_profile = default_lis_normalization_profile();
  changed_profile.version = "2";
  const auto changed =
      LisSourceAdapter::import(source_bytes, source, {}, changed_profile);
  require(changed.has_value(), "a versioned normalization profile must import");
  require(changed.value().document.id() != first.value().document.id(),
          "a changed normalization profile must intentionally create a new "
          "identity");
}

void test_profile_identity_is_injective_and_text_encoding_is_closed() {
  const auto source_bytes = representative_lis();
  const BufferSourceReference source{.uri = "asset://well/profile-id.lis",
                                     .checksum = "profile-id-v1"};

  auto first_null_profile = default_lis_normalization_profile();
  first_null_profile.inferred_null_values = {-999.0000001};
  auto second_null_profile = first_null_profile;
  second_null_profile.inferred_null_values = {-999.0000002};
  const auto first_null = LisSourceAdapter::import(source_bytes, source, {},
                                                    first_null_profile);
  const auto second_null = LisSourceAdapter::import(source_bytes, source, {},
                                                     second_null_profile);
  require(first_null.has_value() && second_null.has_value() &&
              first_null.value().document.id() != second_null.value().document.id(),
          "distinct null sentinels must produce distinct profile identities");

  const LisSelection legacy_selection{
      .identity_scheme = LisIdentityScheme::resform_compatible_v1};
  const auto first_legacy_null = LisSourceAdapter::import(
      source_bytes, source, legacy_selection, first_null_profile);
  const auto second_legacy_null = LisSourceAdapter::import(
      source_bytes, source, legacy_selection, second_null_profile);
  require(first_legacy_null.has_value() && second_legacy_null.has_value() &&
              first_legacy_null.value().document.id() ==
                  second_legacy_null.value().document.id(),
          "the legacy v1 scheme must retain its historical double "
          "serialization exactly");

  auto first_unit_profile = default_lis_normalization_profile();
  first_unit_profile.unit_rules.push_back(
      LisUnitRule{.canonical_mnemonic = "GR",
                  .source_unit = "API",
                  .canonical_unit = "cps",
                  .multiplier = 1.0000001});
  auto second_unit_profile = first_unit_profile;
  second_unit_profile.unit_rules.back().multiplier = 1.0000002;
  const auto first_unit = LisSourceAdapter::import(source_bytes, source, {},
                                                    first_unit_profile);
  const auto second_unit = LisSourceAdapter::import(source_bytes, source, {},
                                                     second_unit_profile);
  require(first_unit.has_value() && second_unit.has_value() &&
              first_unit.value().document.id() != second_unit.value().document.id(),
          "distinct unit multipliers must produce distinct profile identities");

  auto latin_1_profile = default_lis_normalization_profile();
  latin_1_profile.text_encoding = LisTextEncoding::iso_8859_1;
  const auto ascii = LisSourceAdapter::import(source_bytes, source);
  const auto latin_1 =
      LisSourceAdapter::import(source_bytes, source, {}, latin_1_profile);
  require(ascii.has_value() && latin_1.has_value() &&
              ascii.value().document.id() != latin_1.value().document.id(),
          "text encoding must participate in profile identity");

  auto invalid_encoding = default_lis_normalization_profile();
  invalid_encoding.text_encoding = static_cast<LisTextEncoding>(255U);
  const auto rejected = LisSourceAdapter::import(source_bytes, source, {},
                                                  invalid_encoding);
  require(!rejected.has_value() &&
              rejected.error().code == ErrorCode::invalid_document,
          "an out-of-domain text encoding must reject before importing");
}

void test_invalid_normalization_profile_is_rejected_before_importing() {
  auto invalid_profile = default_lis_normalization_profile();
  invalid_profile.version.clear();
  const auto rejected = LisSourceAdapter::import(
      representative_lis(),
      BufferSourceReference{.uri = "asset://well/invalid-profile.lis",
                            .checksum = "none"},
      {}, invalid_profile);
  require(!rejected.has_value(), "an invalid normalization profile must be "
                                 "rejected without accepting source bytes");

  auto conflicting_profile = default_lis_normalization_profile();
  conflicting_profile.aliases = {
      LisAliasRule{.source_mnemonic = "XGR", .canonical_mnemonic = "GR"},
      LisAliasRule{.source_mnemonic = "x-gr", .canonical_mnemonic = "DEN"},
  };
  const auto conflicting = LisSourceAdapter::import(
      representative_lis(),
      BufferSourceReference{.uri = "asset://well/conflict-profile.lis",
                            .checksum = "none"},
      {}, conflicting_profile);
  require(!conflicting.has_value(),
          "conflicting normalized alias rules must be rejected before import");
}

void test_normalization_profile_can_supply_auditable_custom_alias_and_unit_rules() {
  auto profile = default_lis_normalization_profile();
  profile.aliases.push_back(
      LisAliasRule{.source_mnemonic = "XGR", .canonical_mnemonic = "GR"});
  profile.unit_rules.push_back(LisUnitRule{.canonical_mnemonic = "GR",
                                           .source_unit = "API",
                                           .canonical_unit = "cps",
                                           .multiplier = 2.0});
  const auto imported = LisSourceAdapter::import(
      custom_alias_lis(),
      BufferSourceReference{.uri = "asset://well/profile.lis",
                            .checksum = "profile-v1"},
      {}, profile);
  require(imported.has_value(),
          "a valid explicit normalization profile must import");
  const auto &gamma_ray = imported.value().document.curves().front();
  require(
      gamma_ray.mnemonic == "GR" && gamma_ray.unit == "cps",
      "custom alias and unit rules must take precedence over built-in rules");
  require_near(*gamma_ray.values.value_as_double(0U), 90.0,
               "a custom unit rule must transform source values");
  const auto alias_reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::alias_normalized &&
               diagnostic.channel_name == "XGR";
      });
  require(alias_reported,
          "custom normalization must remain visible in the audit");
}

void test_direct_physical_stream_preserves_short_record_headers_after_padding() {
  const auto imported = LisSourceAdapter::import(
      padded_direct_lis(),
      BufferSourceReference{.uri = "asset://well/padding.lis",
                            .checksum = "padding-v1"});
  require(
      imported.has_value(),
      "null padding must not consume a following short physical-record header");
  require(imported.value().document.curves().size() == 2U,
          "padding recovery must preserve the subsequent LIS data set");
}

void test_explicit_lis_absent_value_has_priority_over_inferred_nulls() {
  const auto imported = LisSourceAdapter::import(
      explicit_null_lis(),
      BufferSourceReference{.uri = "asset://well/explicit-null.lis",
                            .checksum = "explicit-v1"});
  require(imported.has_value(), "an explicit LIS absent value must import");
  const auto &gamma_ray = imported.value().document.curves().front();
  require(!gamma_ray.nulls.is_null(0U) && gamma_ray.nulls.is_null(1U),
          "an explicit source null must replace rather than combine with "
          "inferred sentinels");
  require_near(*gamma_ray.values.value_as_double(0U), -999.25,
               "an inferred sentinel remains numeric when the source declares "
               "another null");
}

void test_constant_lis_axis_defaults_to_increasing_with_an_audit_entry() {
  const auto imported = LisSourceAdapter::import(
      constant_axis_lis(),
      BufferSourceReference{.uri = "asset://well/constant.lis",
                            .checksum = "constant-v1"});
  require(imported.has_value(), "a constant LIS index must remain importable");
  require(imported.value().document.sampling_axes().front().direction ==
              AxisDirection::increasing,
          "an all-equal axis must use the documented increasing fallback");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::constant_axis &&
               diagnostic.severity == Severity::info;
      });
  require(reported,
          "the all-equal direction fallback must be visible in the audit");
}

void test_normalization_does_not_remove_internal_mnemonic_spaces() {
  const auto imported = LisSourceAdapter::import(
      spaced_mnemonic_lis(),
      BufferSourceReference{.uri = "asset://well/spaced.lis",
                            .checksum = "spaced-v1"});
  require(imported.has_value(),
          "a spaced scalar mnemonic must remain importable");
  const auto &curve = imported.value().document.curves().front();
  require(curve.mnemonic == "D TC" && curve.unit == "US/M",
          "only spaces at the mnemonic boundaries may be ignored for aliases");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::unknown_curve_semantics;
      });
  require(reported,
          "an unrecognized spaced mnemonic must be audited distinctly");
}

void test_known_curve_with_incompatible_unit_is_a_normalization_conflict() {
  const auto imported = LisSourceAdapter::import(
      incompatible_unit_lis(),
      BufferSourceReference{.uri = "asset://well/conflict.lis",
                            .checksum = "conflict-v1"});
  require(imported.has_value(),
          "an incompatible scalar unit must retain raw data");
  const auto &curve = imported.value().document.curves().front();
  require(curve.mnemonic == "RHOB" && curve.unit == "API",
          "a name/unit conflict must preserve the raw source curve");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::normalization_conflict &&
               diagnostic.representation == 68U;
      });
  require(reported,
          "a name/unit conflict must retain its representation in the audit");
}

void test_default_ascii_reports_non_ascii_text_and_explicit_codepage_decodes_it() {
  const auto source_bytes = non_ascii_text_lis();
  const auto default_import = LisSourceAdapter::import(
      source_bytes, BufferSourceReference{.uri = "asset://well/text.lis",
                                          .checksum = "text-v1"});
  require(default_import.has_value(),
          "default ASCII handling must preserve importability");
  const auto default_reported = std::any_of(
      default_import.value().diagnostics.begin(),
      default_import.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::non_ascii_text &&
               diagnostic.severity == Severity::warning;
      });
  require(default_reported, "default ASCII must audit non-ASCII source text");

  auto latin_1 = default_lis_normalization_profile();
  latin_1.text_encoding = LisTextEncoding::iso_8859_1;
  const auto decoded = LisSourceAdapter::import(
      source_bytes,
      BufferSourceReference{.uri = "asset://well/text.lis",
                            .checksum = "text-v1"},
      {}, latin_1);
  require(decoded.has_value(), "an explicit supported code page must import");
  require(decoded.value().document.curves().front().unit == "\xc3\xa9",
          "the explicit ISO-8859-1 profile must decode source text as UTF-8");
}

void test_valid_logical_file_without_a_data_set_returns_an_audited_empty_document() {
  const auto imported = LisSourceAdapter::import(
      header_only_lis(), BufferSourceReference{.uri = "asset://well/empty.lis",
                                               .checksum = "empty-v1"});
  require(imported.has_value(),
          "a valid header-only logical file must import as empty");
  require(imported.value().document.sampling_axes().empty() &&
              imported.value().document.curves().empty(),
          "an empty import must not synthesize axes or curves");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::no_importable_curve &&
               diagnostic.severity == Severity::warning;
      });
  require(reported,
          "an empty logical file must state why no curve was imported");
}

void test_redundant_format_and_unknown_records_are_audited_without_losing_data() {
  const auto redundant = LisSourceAdapter::import(
      redundant_format_lis(),
      BufferSourceReference{.uri = "asset://well/redundant.lis",
                            .checksum = "redundant-v1"});
  require(redundant.has_value(),
          "a redundant DFS must not suppress following data");
  require(
      redundant.value().document.sampling_axes().size() == 1U,
      "an adjacent identical DFS without data must collapse to one data set");
  const auto redundant_reported = std::any_of(
      redundant.value().diagnostics.begin(),
      redundant.value().diagnostics.end(), [](const LisDiagnostic &diagnostic) {
        return diagnostic.code ==
                   LisDiagnosticCode::redundant_format_specification &&
               diagnostic.severity == Severity::info;
      });
  require(redundant_reported,
          "the redundant DFS collapse must remain auditable");

  const auto unknown = LisSourceAdapter::import(
      unknown_record_lis(),
      BufferSourceReference{.uri = "asset://well/unknown.lis",
                            .checksum = "unknown-v1"});
  require(unknown.has_value(),
          "a bounded unknown record must not reject a valid data set");
  const auto unknown_reported = std::any_of(
      unknown.value().diagnostics.begin(), unknown.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::unknown_record &&
               diagnostic.severity == Severity::warning;
      });
  require(unknown_reported,
          "a skipped unknown record must be visible in the audit");
}

void test_configured_lis_resource_limits_reject_without_truncating() {
  const auto source_bytes = representative_lis();
  auto input_limit = LisLimits{};
  input_limit.max_input_bytes = source_bytes.size() - 1U;
  const auto rejected_input = LisSourceAdapter::import(
      source_bytes,
      BufferSourceReference{.uri = "asset://well/limit.lis",
                            .checksum = "limit-v1"},
      {}, default_lis_normalization_profile(), input_limit);
  require(!rejected_input.has_value() &&
              rejected_input.error().code == ErrorCode::resource_exhausted,
          "an input-size breach must reject rather than partially import a LIS "
          "file");

  auto sample_limit = LisLimits{};
  sample_limit.max_samples_per_data_set = 2U;
  const auto rejected_samples = LisSourceAdapter::import(
      source_bytes,
      BufferSourceReference{.uri = "asset://well/limit.lis",
                            .checksum = "limit-v1"},
      {}, default_lis_normalization_profile(), sample_limit);
  require(!rejected_samples.has_value() &&
              rejected_samples.error().code == ErrorCode::resource_exhausted,
          "a sample-limit breach must reject rather than truncate a data set");
}

void test_malformed_bounded_data_set_isolated_from_following_data_set() {
  const auto imported = LisSourceAdapter::import(
      locally_malformed_data_set_lis(),
      BufferSourceReference{.uri = "asset://well/local-error.lis",
                            .checksum = "local-v1"});
  require(imported.has_value(), "a malformed bounded data run must not "
                                "suppress a following valid data set");
  const auto &document = imported.value().document;
  require(document.sampling_axes().size() == 1U &&
              document.curves().size() == 2U,
          "only the following valid data set must enter the document");
  require_near(
      *document.sampling_axes().front().coordinates.value_as_double(0U), 2000.0,
      "the retained data set must preserve its own coordinates");
  const auto reported = std::any_of(
      imported.value().diagnostics.begin(), imported.value().diagnostics.end(),
      [](const LisDiagnostic &diagnostic) {
        return diagnostic.code == LisDiagnosticCode::malformed_dataset &&
               diagnostic.severity == Severity::warning &&
               diagnostic.data_set_ordinal == 0U;
      });
  require(reported,
          "the isolated malformed data set must remain visible in the audit");
}

} // namespace

int main() {
  test_default_profile_normalizes_a_selected_lis79_logical_file();
  test_imported_lis_enters_the_standard_svg_consumer_path();
  test_multiple_logical_files_require_an_explicit_selection();
  test_backtracking_index_creates_separate_axes_without_sorting();
  test_profile_infers_nulls_in_the_source_numeric_domain();
  test_unrecognized_numeric_index_is_not_misrepresented_as_depth();
  test_each_lis_data_set_retains_its_own_axis_and_curves();
  test_valid_non_scalar_lis_data_set_becomes_an_empty_document();
  test_tif_wrapped_lis79_is_detected_structurally();
  test_rp66_v1_family_is_rejected_instead_of_guessed_as_lis79();
  test_import_identity_versions_content_and_prevents_curve_collisions();
  test_profile_identity_is_injective_and_text_encoding_is_closed();
  test_invalid_normalization_profile_is_rejected_before_importing();
  test_normalization_profile_can_supply_auditable_custom_alias_and_unit_rules();
  test_direct_physical_stream_preserves_short_record_headers_after_padding();
  test_explicit_lis_absent_value_has_priority_over_inferred_nulls();
  test_constant_lis_axis_defaults_to_increasing_with_an_audit_entry();
  test_normalization_does_not_remove_internal_mnemonic_spaces();
  test_known_curve_with_incompatible_unit_is_a_normalization_conflict();
  test_default_ascii_reports_non_ascii_text_and_explicit_codepage_decodes_it();
  test_valid_logical_file_without_a_data_set_returns_an_audited_empty_document();
  test_redundant_format_and_unknown_records_are_audited_without_losing_data();
  test_configured_lis_resource_limits_reject_without_truncating();
  test_malformed_bounded_data_set_isolated_from_following_data_set();
  return EXIT_SUCCESS;
}
