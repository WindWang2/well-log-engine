#include <welllog/export/svg.hpp>
#include <welllog/export/table_writers.hpp>
#include <welllog/io/format716.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

void require_near(double actual, double expected, std::string_view message) {
  if (std::abs(actual - expected) > 1.0e-5) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

class Format716Fixture {
public:
  Format716Fixture &well_name(std::string_view name) {
    well_name_.assign(name);
    return *this;
  }

  Format716Fixture &depths(float start, float end, float interval) {
    start_ = start;
    end_ = end;
    interval_ = interval;
    return *this;
  }

  Format716Fixture &null_value(float value) {
    null_ = value;
    return *this;
  }

  Format716Fixture &sample_count(std::uint32_t count) {
    samples_ = count;
    return *this;
  }

  Format716Fixture &add_curve(std::string_view mnemonic, std::string_view unit,
                              std::vector<float> values) {
    curves_.push_back(CurveDef{std::string{mnemonic}, std::string{unit},
                               std::move(values)});
    return *this;
  }

  Format716Fixture &endian(Format716Endian value) {
    endian_ = value;
    return *this;
  }

  Format716Fixture &dirty_file_reserved() {
    dirty_file_reserved_ = true;
    return *this;
  }

  [[nodiscard]] std::vector<std::byte> finish() const {
    require(!curves_.empty(), "fixture needs at least one curve");
    require(samples_ > 0, "fixture needs a positive sample count");
    for (const auto &curve : curves_) {
      require(curve.values.size() == samples_,
              "every fixture curve must supply exactly sample_count values");
    }

    std::vector<std::byte> bytes;
    bytes.resize(128 + curves_.size() * 64 +
                 static_cast<std::size_t>(samples_) * curves_.size() * 4);

    // File header.
    std::memset(bytes.data(), 0, 128);
    std::memcpy(bytes.data(), well_name_.data(),
                std::min(well_name_.size(), std::size_t{80}));
    write_u32(bytes, 80, static_cast<std::uint32_t>(curves_.size()));
    write_f32(bytes, 84, start_);
    write_f32(bytes, 88, end_);
    write_f32(bytes, 92, interval_);
    write_f32(bytes, 96, null_);
    write_u32(bytes, 100, samples_);
    if (dirty_file_reserved_) {
      bytes[104] = std::byte{0x01};
    }

    // Curve headers.
    for (std::size_t curve = 0; curve < curves_.size(); ++curve) {
      const auto base = 128 + curve * 64;
      std::memset(bytes.data() + base, 0, 64);
      std::memcpy(bytes.data() + base, curves_[curve].mnemonic.data(),
                  std::min(curves_[curve].mnemonic.size(), std::size_t{16}));
      std::memcpy(bytes.data() + base + 16, curves_[curve].unit.data(),
                  std::min(curves_[curve].unit.size(), std::size_t{16}));
      write_f32(bytes, base + 32, 0.0F);
      write_f32(bytes, base + 36, 100.0F);
    }

    // Sample-major data.
    const auto data_offset = 128 + curves_.size() * 64;
    for (std::uint32_t sample = 0; sample < samples_; ++sample) {
      for (std::size_t curve = 0; curve < curves_.size(); ++curve) {
        const auto offset =
            data_offset +
            (static_cast<std::size_t>(sample) * curves_.size() + curve) * 4;
        write_f32(bytes, offset, curves_[curve].values[sample]);
      }
    }
    return bytes;
  }

private:
  struct CurveDef {
    std::string mnemonic;
    std::string unit;
    std::vector<float> values;
  };

  void write_u32(std::vector<std::byte> &bytes, std::size_t offset,
                 std::uint32_t value) const {
    if (endian_ == Format716Endian::little) {
      bytes[offset] = static_cast<std::byte>(value & 0xffU);
      bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
      bytes[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xffU);
      bytes[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xffU);
    } else {
      bytes[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
      bytes[offset + 1] = static_cast<std::byte>((value >> 16U) & 0xffU);
      bytes[offset + 2] = static_cast<std::byte>((value >> 8U) & 0xffU);
      bytes[offset + 3] = static_cast<std::byte>(value & 0xffU);
    }
  }

  void write_f32(std::vector<std::byte> &bytes, std::size_t offset,
                 float value) const {
    std::uint32_t bits{};
    static_assert(sizeof(float) == 4);
    std::memcpy(&bits, &value, sizeof(float));
    write_u32(bytes, offset, bits);
  }

  std::string well_name_{"Example-716-1"};
  float start_{1000.0F};
  float end_{1002.0F};
  float interval_{1.0F};
  float null_{-999.25F};
  std::uint32_t samples_{3};
  Format716Endian endian_{Format716Endian::little};
  bool dirty_file_reserved_{false};
  std::vector<CurveDef> curves_;
};

Format716Import import_bytes(const std::vector<std::byte> &bytes,
                             Format716Endian endian = Format716Endian::little) {
  const auto imported = Format716SourceAdapter::import(
      bytes, BufferSourceReference{.uri = "asset://well/Example-716-1",
                                   .checksum = "content-v1"},
      Format716Selection{.endian = endian,
                         .layout = Format716Layout::multi_curve_disk_v1});
  require(imported.has_value(), "valid 716 fixture must import");
  return imported.value();
}

std::vector<std::byte> representative_production_fixture() {
  // Representative multi-curve 716 disk file with an explicit depth channel,
  // null sentinel, repeated curve mnemonics, and increasing MD samples.
  return Format716Fixture{}
      .well_name("CNPC-Demo-Well-A")
      .depths(1000.0F, 1002.0F, 1.0F)
      .null_value(-999.25F)
      .sample_count(3)
      .add_curve("DEPT", "m", {1000.0F, 1001.0F, 1002.0F})
      .add_curve("GR", "API", {45.0F, -999.25F, 55.0F})
      .add_curve("GR", "API", {46.0F, 47.0F, 56.0F})
      .add_curve("RHOB", "g/cm3", {2.40F, 2.41F, 2.42F})
      .finish();
}

void normalizes_depth_channel_fixture_into_stable_document_semantics() {
  const auto bytes = representative_production_fixture();
  const auto imported = import_bytes(bytes);
  const auto &document = imported.document;

  require(imported.depth_strategy_used ==
              Format716DepthStrategy::depth_as_first_curve,
          "DEPT first curve must select the depth-as-first-curve strategy");
  require(imported.endian_used == Format716Endian::little &&
              imported.layout_used == Format716Layout::multi_curve_disk_v1,
          "import must report the actual parse strategy");
  require(document.sampling_axes().size() == 1,
          "one depth channel must create one Sampling Axis");
  require(document.curves().size() == 3,
          "measurement curves exclude the depth channel");

  const auto &axis = document.sampling_axes().front();
  require(axis.domain == DepthDomain::measured_depth, "depth domain is MD");
  require(axis.unit == "m", "depth unit is retained from the DEPT channel");
  require(axis.direction == AxisDirection::increasing, "depth is increasing");
  require(axis.coordinates.length() == 3, "every depth sample is retained");
  require_near(*axis.coordinates.value_as_double(1), 1001.0,
               "depth samples keep source order and values");

  const auto &first_gr = document.curves()[0];
  const auto &second_gr = document.curves()[1];
  const auto &rhob = document.curves()[2];
  require(first_gr.mnemonic == "GR" && second_gr.mnemonic == "GR",
          "same-named curves keep their source mnemonic");
  require(first_gr.id != second_gr.id,
          "same-named curves receive distinct stable identities");
  require(first_gr.sampling_axis_id == axis.id &&
              second_gr.sampling_axis_id == axis.id &&
              rhob.sampling_axis_id == axis.id,
          "all curves share the depth axis");
  require(first_gr.nulls.is_null(1),
          "declared null sentinel becomes a null sample");
  require(!second_gr.nulls.is_null(1),
          "non-sentinel values remain measurements");
  require_near(*rhob.values.value_as_double(2), 2.42, "curve values retained");
  const auto reparsed = import_bytes(bytes);
  require(reparsed.document.id() == document.id(),
          "reload yields a stable document identity");
  require(reparsed.document.curves()[1].id == second_gr.id,
          "reload yields stable curve identities");
}

void synthesizes_depth_when_no_depth_channel_is_present() {
  const auto bytes =
      Format716Fixture{}
          .well_name("Synthetic-Depth")
          .depths(2000.0F, 2001.0F, 0.5F)
          .sample_count(3)
          .add_curve("GR", "API", {10.0F, 11.0F, 12.0F})
          .add_curve("SP", "mV", {-20.0F, -21.0F, -22.0F})
          .finish();
  const auto imported = import_bytes(bytes);
  require(imported.depth_strategy_used ==
              Format716DepthStrategy::synthetic_depth,
          "missing depth mnemonic selects synthetic depth");
  const auto &axis = imported.document.sampling_axes().front();
  require(axis.coordinates.length() == 3, "synthetic axis uses sample_count");
  require_near(*axis.coordinates.value_as_double(0), 2000.0,
               "synthetic depth starts at header start_depth");
  require_near(*axis.coordinates.value_as_double(2), 2001.0,
               "synthetic depth ends at start + (n-1)*interval");
  require(imported.document.curves().size() == 2,
          "every source curve becomes a measurement curve");
}

void preserves_decreasing_axis_and_repeated_depths() {
  const auto decreasing =
      Format716Fixture{}
          .depths(1002.0F, 1000.0F, -1.0F)
          .sample_count(3)
          .add_curve("DEPT", "m", {1002.0F, 1001.0F, 1000.0F})
          .add_curve("GR", "API", {1.0F, 2.0F, 3.0F})
          .finish();
  const auto imported = import_bytes(decreasing);
  require(imported.document.sampling_axes().front().direction ==
              AxisDirection::decreasing,
          "decreasing source depths keep decreasing axis direction");

  const auto repeats =
      Format716Fixture{}
          .depths(1000.0F, 1001.0F, 0.5F)
          .sample_count(4)
          .add_curve("DEPT", "m", {1000.0F, 1000.0F, 1000.5F, 1001.0F})
          .add_curve("GR", "API", {1.0F, 2.0F, 3.0F, 4.0F})
          .finish();
  const auto repeated = import_bytes(repeats);
  require(repeated.document.sampling_axes().front().coordinates.length() == 4,
          "repeated depths are retained in source order");
  require_near(*repeated.document.curves().front().values.value_as_double(1),
               2.0, "curve samples stay aligned with repeated depths");
}

void rejects_truncation_unknown_layout_and_resource_limits() {
  auto bytes = representative_production_fixture();
  bytes.pop_back();
  const auto truncated = Format716SourceAdapter::import(
      bytes, BufferSourceReference{.uri = "asset://truncated", .checksum = "v1"});
  require(!truncated.has_value() &&
              truncated.error().code == ErrorCode::invalid_document,
          "truncated files must fail as invalid documents");

  const auto too_many_curves =
      Format716SourceAdapter::import(
          representative_production_fixture(),
          BufferSourceReference{.uri = "asset://limits", .checksum = "v1"},
          Format716Selection{},
          Format716Limits{.max_input_bytes = 64U * 1024U * 1024U,
                          .max_curves = 1,
                          .max_samples = 10'000'000U});
  require(!too_many_curves.has_value() &&
              too_many_curves.error().code == ErrorCode::resource_exhausted,
          "curve-count limits must fail as resource exhaustion");

  auto oversized = Format716Fixture{}
                       .sample_count(2)
                       .add_curve("GR", "API", {1.0F, 2.0F})
                       .finish();
  const auto too_large = Format716SourceAdapter::import(
      oversized, BufferSourceReference{.uri = "asset://big", .checksum = "v1"},
      Format716Selection{},
      Format716Limits{.max_input_bytes = 64,
                      .max_curves = 4096,
                      .max_samples = 10'000'000U});
  require(!too_large.has_value() &&
              too_large.error().code == ErrorCode::resource_exhausted,
          "input-byte limits must fail as resource exhaustion");

  // Interval that does not reach end_depth is an unknown/invalid segment.
  const auto bad_interval =
      Format716Fixture{}
          .depths(1000.0F, 1100.0F, 1.0F)
          .sample_count(3)
          .add_curve("GR", "API", {1.0F, 2.0F, 3.0F})
          .finish();
  const auto rejected = Format716SourceAdapter::import(
      bad_interval,
      BufferSourceReference{.uri = "asset://bad-interval", .checksum = "v1"});
  require(!rejected.has_value() &&
              rejected.error().code == ErrorCode::invalid_document,
          "inconsistent depth headers must be rejected");
}

void detect_endian_requires_unique_match() {
  const auto little_bytes = representative_production_fixture();
  const auto detected = Format716SourceAdapter::detect_endian(little_bytes);
  require(detected.has_value() &&
              detected.value() == Format716Endian::little,
          "unique little-endian structure must be detected");

  const auto big_bytes = Format716Fixture{}
                             .endian(Format716Endian::big)
                             .sample_count(2)
                             .add_curve("DEPT", "m", {10.0F, 11.0F})
                             .add_curve("GR", "API", {1.0F, 2.0F})
                             .finish();
  const auto detected_big = Format716SourceAdapter::detect_endian(big_bytes);
  require(detected_big.has_value() &&
              detected_big.value() == Format716Endian::big,
          "unique big-endian structure must be detected");

  // Empty input matches neither endian and must not guess.
  const auto empty = Format716SourceAdapter::detect_endian({});
  require(!empty.has_value() && empty.error().code == ErrorCode::invalid_document,
          "ambiguous or missing structure must not invent an endianness");
}

void reports_non_finite_values_and_reserved_noise() {
  const auto bytes =
      Format716Fixture{}
          .dirty_file_reserved()
          .sample_count(2)
          .add_curve("DEPT", "m", {1000.0F, 1001.0F})
          .add_curve("GR", "API",
                     {std::numeric_limits<float>::quiet_NaN(), 20.0F})
          .finish();
  const auto imported = import_bytes(bytes);
  require(imported.document.curves().front().nulls.is_null(0),
          "non-finite curve values become null samples");
  bool saw_non_finite{};
  bool saw_reserved{};
  for (const auto &diagnostic : imported.diagnostics) {
    saw_non_finite =
        saw_non_finite ||
        diagnostic.code == Format716DiagnosticCode::non_finite_curve_value;
    saw_reserved =
        saw_reserved ||
        diagnostic.code == Format716DiagnosticCode::reserved_bytes_nonzero;
  }
  require(saw_non_finite && saw_reserved,
          "non-finite values and reserved noise must be diagnosed");
}

void production_fixture_supports_table_svg_and_csv_export() {
  const auto bytes = representative_production_fixture();
  const auto imported = import_bytes(bytes);
  const auto &document = imported.document;

  const auto tables = TableProjectionBuilder::from_document(document);
  require(tables.size() == 1 && tables.front().row_count() == 3,
          "normalized 716 document must enter table projection");
  require(tables.front().column_count() == 4,
          "table retains depth plus three measurement curves");

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "normalized 716 document must enter a session");

  const auto track_id = id("16700000-0000-4000-8000-000000000001");
  const auto scale_id = id("16700000-0000-4000-8000-000000000002");
  const auto layer_id = id("16700000-0000-4000-8000-000000000003");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1002.0},
      Millimetres{60.0}, "font-fixture-v1");
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
      .curve_id = document.curves().front().id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 1, .green = 2, .blue = 3, .alpha = 255},
      .line_width = Millimetres{0.25},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "716 document must accept a presentation");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && !scene->curve_layers().empty(),
          "716 curves must prepare a scene");
  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value() &&
              svg.value().text().find("<path") != std::string_view::npos,
          "716 layout must export through the shared SVG path");

  const auto temp = std::filesystem::temp_directory_path() /
                    "welllog-format716-csv-export";
  std::filesystem::remove_all(temp);
  std::filesystem::create_directories(temp);
  const auto csv = CsvPackageExporter::write_to_directory(tables, temp);
  require(csv.has_value(), "716 tables must export through shared CSV package");
  bool found_csv = false;
  for (const auto &entry : std::filesystem::directory_iterator(temp)) {
    if (entry.path().extension() == ".csv") {
      found_csv = true;
      std::ifstream input(entry.path());
      std::string content((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
      require(content.find("GR") != std::string::npos,
              "CSV export must include the 716 curve mnemonic");
    }
  }
  require(found_csv, "CSV package must write at least one table file");
  std::filesystem::remove_all(temp);
}

void inspect_exposes_catalog_without_building_document() {
  const auto bytes = representative_production_fixture();
  const auto inspected = Format716SourceAdapter::inspect(
      bytes, Format716Selection{.endian = Format716Endian::little});
  require(inspected.has_value(), "valid 716 files must inspect");
  const auto &catalog = inspected.value();
  require(catalog.well_name == "CNPC-Demo-Well-A", "well name retained");
  require(catalog.curve_count == 4 && catalog.sample_count == 3,
          "inspect exposes curve and sample counts");
  require(catalog.curve_mnemonics.size() == 4 &&
              catalog.curve_mnemonics[0] == "DEPT",
          "inspect exposes curve mnemonics");
  require(catalog.depth_strategy ==
              Format716DepthStrategy::depth_as_first_curve,
          "inspect reports depth strategy without import");
}

} // namespace

int main() {
  normalizes_depth_channel_fixture_into_stable_document_semantics();
  synthesizes_depth_when_no_depth_channel_is_present();
  preserves_decreasing_axis_and_repeated_depths();
  rejects_truncation_unknown_layout_and_resource_limits();
  detect_endian_requires_unique_match();
  reports_non_finite_values_and_reserved_noise();
  production_fixture_supports_table_svg_and_csv_export();
  inspect_exposes_catalog_without_building_document();
  return EXIT_SUCCESS;
}
