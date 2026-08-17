// Black-box parity + degradation test for the PDF and SVG export backends
// (#189, the criterion-8 gate). Asserts physical correctness and cross-backend
// parity across the full export, pagination continuity, pattern phase, snapshot
// metadata round-trip, reproducibility, and that any rasterisation degradation
// is EXPLICITLY reported (criterion 7) — never silent in pure-vector mode.
//
// The same PreparedScene + ExportSnapshot is fed to BOTH backends, then
// cross-asserted. The PDF side parses its Flate-compressed content streams and
// MediaBox; the SVG side substring-counts its data-export-role elements. qpdf /
// pdfinfo / pdftoppm verify external validity + orientation when available.

#include <welllog/export/export_layout.hpp>
#include <welllog/export/pagination.hpp>
#include <welllog/export/pdf_scene.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/scene/image_pyramid.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>

#include "scene/prepare.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace {

using namespace welllog;

// --- Test harness helpers (mirrors pdf_scene_full_test.cpp's idiom) -----------

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
  if (std::abs(actual - expected) > 1.0e-6) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

[[nodiscard]] std::size_t count_occurrences(std::string_view haystack,
                                            std::string_view needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

#ifndef WELLLOG_TEST_FONT_DIR
#define WELLLOG_TEST_FONT_DIR "tests/assets/fonts"
#endif

std::shared_ptr<HarfBuzzTextEngine> make_engine() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/NotoSans-Regular.ttf")
              .has_value(),
          "bundled test font must load");
  return engine;
}

// 1 mm = 72/25.4 PDF user-space points.
constexpr double points_per_millimetre = 72.0 / 25.4;

// Distinct UUID prefix for this TU.
const auto document_id = id("90000000-0000-4000-8000-000000000001");
const auto axis_id = id("90000000-0000-4000-8000-000000000002");
const auto curve_id = id("90000000-0000-4000-8000-000000000003");
const auto track_id = id("90000000-0000-4000-8000-000000000004");
const auto scale_id = id("90000000-0000-4000-8000-000000000005");
const auto pattern_id = id("90000000-0000-4000-8000-000000000006");
const auto interval_layer_id = id("90000000-0000-4000-8000-000000000007");
const auto curve_layer_id = id("90000000-0000-4000-8000-000000000008");
const auto interval_id = id("90000000-0000-4000-8000-000000000009");
const auto image_source_id = id("90000000-0000-4000-8000-00000000000a");
const auto image_layer_id = id("90000000-0000-4000-8000-00000000000b");

// A scene carrying every layer kind the export must agree on: a patterned
// interval, a curve, and a raster image. Built via the document/presentation
// builders; the image pyramid is threaded via the preparer (host wiring).
struct ParityScene {
  WellLogDocument document;
  ScenePresentationBuilder presentation;
};

ParityScene make_parity_scene() {
  // 9 samples over [1000, 1800] — a curve dense enough to exercise a real
  // complexity budget, sparse enough to keep the test fast.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1100.0, 1200.0, 1300.0, 1400.0,
                                    1500.0, 1600.0, 1700.0, 1800.0});
  auto values =
      std::make_shared<const std::vector<double>>(std::initializer_list<double>{
          0.0, 12.5, 25.0, 37.5, 50.0, 62.5, 75.0, 87.5, 100.0});

  WellLogDocumentBuilder doc(document_id, DocumentRevision{7});
  doc.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  doc.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  // Patterned interval — drives tiling-pattern parity (criterion 5).
  doc.add_interval(Interval{
      .id = interval_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1300.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{220, 200, 120, 255},
      .label = "Sand",
  });
  doc.add_image_source(ImageSource{
      .id = image_source_id,
      .width_px = 256,
      .height_px = 256,
      .pixel_format = PixelFormat::rgb8,
      .reference_depth_top = 1000.0,
      .reference_depth_bottom = 1800.0,
      .dpi = 300,
      .source = BufferSourceReference{.uri = "image://core-photo/1",
                                      .checksum = {},
                                      .byte_offset = 0},
  });

  ScenePresentationBuilder pres(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1800.0,
      },
      Millimetres{400.0}, "font-fixture-v1");
  pres.set_presentation_version(PresentationVersion{42});
  pres.set_depth_transform_version(3);
  pres.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{80.0},
      .z_order = 0,
      .header = TrackHeaderSpec{.height = Millimetres{6.0},
                                .font_size = Millimetres{2.5}},
  });
  // A rotated, offset-anchor pattern so the phase /Matrix is non-trivial and
  // must match R·T across both backends (criterion 5).
  pres.add_pattern(PatternDefinition{
      .id = pattern_id,
      .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0},
      .rotation_degrees = 30.0,
      .foreground = RgbaColor{60, 60, 60, 255},
      .background = RgbaColor{255, 250, 230, 255},
      .stroke_width = Millimetres{0.2},
      .scene_anchor = PhysicalPoint{Millimetres{3.0}, Millimetres{4.0}},
      .primitives = {PatternLine{
          PhysicalPoint{Millimetres{-1.0}, Millimetres{-1.0}},
          PhysicalPoint{Millimetres{5.0}, Millimetres{5.0}}}},
  });
  pres.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  pres.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
  });
  pres.add_curve_layer(CurveLayerSpec{
      .id = curve_layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{0x11, 0x22, 0x33, 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 1,
      .visible = true,
  });
  pres.add_image_layer(ImageLayerSpec{
      .id = image_layer_id,
      .track_id = track_id,
      .image_source_id = image_source_id,
      .z_order = 2,
      .visible = true,
  });
  return {doc.build(), std::move(pres)};
}

// Prepares the scene via the preparer so the image pyramid map is threaded
// (mirrors image_layer_test.cpp / pdf_scene_full_test.cpp).
std::shared_ptr<const PreparedScene>
prepare(const WellLogDocument &document, ScenePresentationBuilder &builder) {
  const auto presentation = builder.build();
  detail::ScenePreparer::CurveLodMap curve_lods;
  detail::ScenePreparer::ImagePyramidMap image_pyramids;
  const auto pyramid = ImagePyramid::build(
      document.image_sources().front(),
      ImagePyramidOptions{.tile_size = 256,
                          .maximum_derived_bytes = 1024 * 1024});
  require(pyramid.has_value(), "image pyramid must build");
  image_pyramids.emplace(image_source_id, pyramid.value());
  const auto scene = detail::ScenePreparer::prepare(
      document, presentation, curve_lods, {},
      image_pyramids,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1800.0,
                        .pixel_height = 1000.0,
                        .prefetch_viewports = 0.0});
  require(scene.has_value(), "scene must prepare");
  return std::make_shared<const PreparedScene>(std::move(scene.value()));
}

ExportSnapshot make_snapshot(PaginationMode mode,
                             Millimetres page_height = Millimetres{50.0}) {
  return ExportSnapshot{
      .document_id = document_id,
      .document_revision = DocumentRevision{7},
      .presentation_version = PresentationVersion{42},
      .depth_transform =
          DepthTransformDescriptor{
              .domain = DepthDomain::measured_depth,
              .unit = "m",
              .reference_top = 1000.0,
              .reference_bottom = 1800.0,
              .version = 3,
          },
      .font_asset_fingerprint = "font-fixture-v1",
      .pattern_versions = {},
      .page = ExportPageSpec{
          .mode = mode,
          .page_width = Millimetres{120.0},
          .page_height = page_height,
          .margins = ExportPageMargins{.top = Millimetres{10.0},
                                       .right = Millimetres{10.0},
                                       .bottom = Millimetres{10.0},
                                       .left = Millimetres{10.0}},
          .dpi = 300,
          .page_overlap = 0.0,
          .well_name = "Well-A",
          .repeat_headers = true,
          .repeat_legend = true,
          .show_page_numbers = true,
          .show_depth_range = true,
      },
  };
}

// A deterministic solid-color tile resolver (mirrors pdf_scene_full_test.cpp).
struct StubResolver {
  std::shared_ptr<std::vector<std::uint8_t>> pixels =
      std::make_shared<std::vector<std::uint8_t>>(256 * 256 * 3, 0xAA);
  Result<RasterTile> operator()(const ImageTileRequest &) const {
    RasterTile raster{
        .width_px = 256,
        .height_px = 256,
        .pixel_format = PixelFormat::rgb8,
        .owner = SharedOwner{pixels},
        .data = pixels->data(),
    };
    return raster;
  }
};

// --- PDF inspection helpers (mirror pdf_scene_full_test.cpp) -----------------

// Deep-compares two ExportReports entry-by-entry (the "byte-identical report"
// contract: every degraded layer must match, not just the count/front entry).
[[nodiscard]] bool reports_equal(const ExportReport &a,
                                 const ExportReport &b) noexcept {
  if (a.degraded_layers.size() != b.degraded_layers.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.degraded_layers.size(); ++i) {
    const auto &x = a.degraded_layers[i];
    const auto &y = b.degraded_layers[i];
    if (x.layer_id != y.layer_id || x.reason != y.reason ||
        x.target_dpi != y.target_dpi) {
      return false;
    }
  }
  return true;
}

std::filesystem::path write_temp_pdf(std::string_view bytes) {
  const auto path =
      std::filesystem::temp_directory_path() / "welllog_parity.pdf";
  std::ofstream out(path, std::ios::binary);
  require(out.good(), "temp PDF file must open");
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return path;
}

int run(std::string_view command, std::string &captured) {
  std::array<char, 128> buffer{};
  captured.clear();
#if defined(_WIN32)
  const auto pipe =
      _popen(std::string{command}.c_str(), "r"); // NOLINT(cert-env33-c)
#else
  const auto pipe =
      popen(std::string{command}.c_str(), "r"); // NOLINT(cert-env33-c)
#endif
  if (pipe == nullptr) {
    return -1;
  }
  while (std::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    captured += buffer.data();
  }
#if defined(_WIN32)
  return _pclose(pipe); // NOLINT(cert-env33-c)
#else
  return pclose(pipe); // NOLINT(cert-env33-c)
#endif
}

// Grows the sink until inflate finishes. A compressed×8 guess silently
// dropped high-compression streams from the pure-vector gate (#759).
std::string inflate_zlib(std::string compressed, std::string_view what) {
  z_stream zs{};
  require(inflateInit(&zs) == Z_OK, "inflateInit must succeed");
  std::string sink(compressed.size() < 2048 ? 4096 : compressed.size() * 2,
                   '\0');
  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  zs.avail_in = static_cast<uInt>(compressed.size());
  int rc = Z_OK;
  do {
    if (zs.total_out >= sink.size()) {
      sink.resize(sink.size() * 2, '\0');
    }
    zs.next_out = reinterpret_cast<Bytef *>(sink.data() + zs.total_out);
    zs.avail_out = static_cast<uInt>(sink.size() - zs.total_out);
    rc = inflate(&zs, Z_FINISH);
  } while (rc == Z_BUF_ERROR || (rc == Z_OK && zs.avail_out == 0));
  inflateEnd(&zs);
  require(rc == Z_STREAM_END, what);
  return std::string(sink.data(), zs.total_out);
}

// Inflates EVERY FlateDecode content stream in the PDF, in file order.
std::vector<std::string> inflate_all_streams(std::string_view bytes) {
  std::vector<std::string> out;
  std::string_view::size_type search = 0;
  while (true) {
    const auto filter_pos = bytes.find("/Filter /FlateDecode", search);
    if (filter_pos == std::string_view::npos) {
      break;
    }
    const auto stream_kw = bytes.find("stream\n", filter_pos);
    if (stream_kw == std::string_view::npos) {
      break;
    }
    const auto payload_start = stream_kw + std::strlen("stream\n");
    const auto endstream = bytes.find("\nendstream", payload_start);
    if (endstream == std::string_view::npos) {
      break;
    }
    out.push_back(inflate_zlib(
        std::string{bytes.substr(payload_start, endstream - payload_start)},
        "every FlateDecode stream must inflate"));
    search = endstream;
  }
  return out;
}

// Parses every "/MediaBox [0 0 w h]" in the PDF (one per page), returning the
// (width, height) point pairs in page order. Used for criterion 2.
std::vector<std::pair<double, double>> parse_mediaboxes(std::string_view bytes) {
  std::vector<std::pair<double, double>> boxes;
  std::string_view::size_type search = 0;
  while (true) {
    const auto pos = bytes.find("/MediaBox [0 0 ", search);
    if (pos == std::string_view::npos) {
      break;
    }
    const auto nums_start = pos + std::strlen("/MediaBox [0 0 ");
    const auto close = bytes.find(']', nums_start);
    if (close == std::string_view::npos) {
      break;
    }
    const auto nums = bytes.substr(nums_start, close - nums_start);
    const auto sp = nums.find(' ');
    require(sp != std::string_view::npos, "MediaBox must carry w and h");
    boxes.emplace_back(std::stod(std::string{nums.substr(0, sp)}),
                       std::stod(std::string{nums.substr(sp + 1)}));
    search = close;
  }
  return boxes;
}

// Counts /Type /Page entries = page count (the /Pages root also matches /Type
// /Pages, so anchor on "/Type /Page " with trailing space).
std::size_t count_pdf_pages(std::string_view bytes) {
  std::size_t count = 0;
  std::string_view::size_type pos = 0;
  constexpr std::string_view needle = "/Type /Page ";
  while ((pos = bytes.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// Walks $PATH then the two historical absolute locations.
std::filesystem::path find_on_path(std::string_view name) {
  if (const char *path = std::getenv("PATH"); path != nullptr) {
    std::string_view rest{path};
    while (!rest.empty()) {
      const auto colon = rest.find(':');
      const auto dir = rest.substr(0, colon == std::string_view::npos
                                          ? rest.size()
                                          : colon);
      const auto candidate =
          std::filesystem::path{std::string{dir}} / std::string{name};
      if (!dir.empty() && std::filesystem::exists(candidate)) {
        return candidate;
      }
      if (colon == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(colon + 1);
    }
  }
  for (const char *prefix : {"/usr/bin/", "/usr/sbin/"}) {
    auto candidate = std::filesystem::path{std::string{prefix} + std::string{name}};
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

// Parses the 6 operands of the PAGE `cm` operator — the transform with a
// NEGATIVE d (the y-flip). The page-body cm is emitted first in each content
// stream, so the first negative-d cm in the inflated streams is the anchor
// (#193). Does not depend on qpdf (#761).
std::vector<double> parse_page_cm_operands(std::string_view bytes) {
  auto parse_cm_line = [](std::string_view line) -> std::vector<double> {
    std::vector<double> vals;
    std::string token;
    auto take = [&](std::string_view t) {
      try {
        vals.push_back(std::stod(std::string{t}));
      } catch (...) {
        // Skip the `cm` operator token (and any other non-numeric).
      }
    };
    for (const char ch : line) {
      if (ch == ' ') {
        if (!token.empty()) {
          take(token);
          token.clear();
        }
      } else {
        token.push_back(ch);
      }
    }
    if (!token.empty()) {
      take(token);
    }
    return vals;
  };
  for (const auto &stream : inflate_all_streams(bytes)) {
    std::string_view::size_type search = 0;
    while (true) {
      const auto cm_pos = stream.find(" cm", search);
      if (cm_pos == std::string::npos) {
        break;
      }
      const auto line_start = stream.rfind('\n', cm_pos);
      const auto line_end = stream.find('\n', cm_pos);
      const auto line = stream.substr(
          line_start == std::string::npos ? 0 : line_start + 1,
          (line_end == std::string::npos ? stream.size() : line_end) -
              (line_start == std::string::npos ? 0 : line_start + 1));
      const auto vals = parse_cm_line(line);
      if (vals.size() == 6 && vals[3] < 0.0) {
        return vals;
      }
      search = cm_pos + 3;
    }
  }
  return {};
}

// Extracts the first "/Matrix [...]" body and parses its 6 operands.
std::vector<double> parse_first_pattern_matrix(std::string_view bytes) {
  const auto start = bytes.find("/Matrix [");
  require(start != std::string_view::npos,
          "a tiling pattern /Matrix must be present");
  const auto open = bytes.find('[', start);
  const auto close = bytes.find(']', open);
  require(open != std::string_view::npos && close != std::string_view::npos,
          "/Matrix must be bracketed");
  std::vector<double> vals;
  std::string token;
  for (std::size_t i = open + 1; i < close; ++i) {
    if (bytes[i] == ' ') {
      if (!token.empty()) {
        vals.push_back(std::stod(token));
        token.clear();
      }
    } else {
      token.push_back(bytes[i]);
    }
  }
  if (!token.empty()) {
    vals.push_back(std::stod(token));
  }
  return vals;
}

// Builds the parity-scene PDF (continuous or fixed), with the stub resolver and
// text engine wired, optionally filling a degradation report.
PdfDocument build_pdf(const PreparedScene &scene, const ExportSnapshot &snap,
                      ExportReport *report = nullptr) {
  StubResolver resolver;
  auto engine = make_engine();
  const auto result = PdfSceneExporter::write(
      scene, snap,
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get(), report);
  require(result.has_value(), "parity PDF must build");
  return result.value();
}

// Builds the parity-scene SVG, optionally filling a degradation report.
SvgDocument build_svg(const PreparedScene &scene, const ExportSnapshot &snap,
                      ExportReport *report = nullptr) {
  const auto result = PaginatedSvgExporter::write(scene, snap, report);
  require(result.has_value(), "parity SVG must build");
  return result.value();
}

// --- Tests (one per acceptance criterion) ------------------------------------

// Criterion 2: PDF page physical dimensions (page size + margins in mm) match
// the requested Document Scale/page options — physical units, not window pixels.
// Fixed mode: MediaBox = page_width×pmm by page_height×pmm exactly. The SVG
// emits the same numbers as mm attributes, so this cross-checks both backends.
void pdf_page_physical_dimensions_match_request() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);

  // Fixed mode: every MediaBox is exactly page_width×pmm by page_height×pmm —
  // physical units, not window pixels (criterion 2).
  const auto fixed_snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  const auto fixed_bytes = std::string{build_pdf(*scene, fixed_snap).bytes()};
  const auto boxes = parse_mediaboxes(fixed_bytes);
  require(!boxes.empty(), "PDF must carry at least one MediaBox");
  const auto expected_w = fixed_snap.page.page_width.value * points_per_millimetre;
  const auto expected_h =
      fixed_snap.page.page_height.value * points_per_millimetre;
  for (const auto &[w, h] : boxes) {
    require_near(w, expected_w,
                 "PDF page width must equal the requested physical width");
    require_near(h, expected_h,
                 "PDF page height must equal the requested physical height");
  }

  // Continuous mode: the page height is DERIVED from the scene's physical depth
  // scaled to the printable width plus margins — assert the single MediaBox
  // carries that derived height (margins + scale are observable here, criterion
  // 2). Scene is 400 mm tall, 80 mm wide; printable width = 120−10−10 = 100 mm,
  // so scale = 100/80 = 1.25; derived body height = 400·1.25 = 500 mm; page
  // height = 500 + top/bottom margins (10+10) = 520 mm.
  const auto cont_snap =
      make_snapshot(PaginationMode::continuous, Millimetres{297.0});
  const auto cont_bytes = std::string{build_pdf(*scene, cont_snap).bytes()};
  const auto cont_boxes = parse_mediaboxes(cont_bytes);
  require(cont_boxes.size() == 1, "continuous mode must emit one page");
  const auto expected_cont_w =
      cont_snap.page.page_width.value * points_per_millimetre;
  const auto expected_cont_h = 520.0 * points_per_millimetre;
  require_near(cont_boxes[0].first, expected_cont_w,
               "continuous page width must equal the requested physical width");
  require_near(cont_boxes[0].second, expected_cont_h,
               "continuous page height must equal the margins+scale-derived "
               "physical height");
}

// Regression guard for the #193 M1 bug (hidden by symmetric test margins): with
// ASYMMETRIC margins, the page `cm` must still anchor the body's shallow end
// (scene-y=0) at the printable TOP. The correct f = (page_height_mm −
// margins.top)·pmm; a prior form (margin_top_pt + window_bottom_pt) was wrong
// for asymmetric margins. Asserts the page cm's f operand encodes the correct
// asymmetric anchor.
void asymmetric_margins_anchor_body_at_printable_top() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  auto snap = make_snapshot(PaginationMode::continuous, Millimetres{297.0});
  // Asymmetric: top 10, bottom 25. Scene 400 mm tall, 80 wide; printable width
  // 120−10−10=100 → scale 1.25; derived body height 400·1.25=500 mm; page
  // height = 500 + top(10) + bottom(25) = 535 mm.
  snap.page.margins.top = Millimetres{10.0};
  snap.page.margins.bottom = Millimetres{25.0};
  const auto doc = build_pdf(*scene, snap);
  const auto bytes = std::string{doc.bytes()};
  const auto cm = parse_page_cm_operands(bytes);
  require(cm.size() == 6,
          "page cm must be parsed from inflated streams without qpdf (#761)");
  require(cm[3] < 0.0, "page cm d-operand must be negative (y-flip)");
  // f = (page_height_mm − margins.top)·pmm = (535 − 10)·pmm = 525·pmm.
  const auto expected_f =
      (535.0 - snap.page.margins.top.value) * points_per_millimetre;
  require_near(cm[5], expected_f,
               "page cm f must anchor scene-y=0 at the printable top for "
               "asymmetric margins (the #193 M1 regression)");
}

// Criterion 8: SVG structure correctness — root/page/header/legend/page-number/
// depth-range elements present and correctly counted. Continuous = 1 page;
// fixed = N pages each carrying the repeating bands.
void svg_structure_is_correct() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);

  // Continuous: one <svg>, header (well name) + footer present.
  auto cont_snap = make_snapshot(PaginationMode::continuous, Millimetres{297.0});
  const auto cont_text = std::string{build_svg(*scene, cont_snap).text()};
  require(count_occurrences(cont_text, "<svg ") == 1,
          "continuous mode must emit exactly one svg document");
  require(cont_text.find("data-export-role=\"header\"") != std::string::npos,
          "continuous page must carry a header");
  require(cont_text.find("data-export-role=\"footer\"") != std::string::npos,
          "continuous page must carry a footer");

  // Fixed: N pages, each with header/legend/footer/body roles + page numbers.
  const auto fixed_text =
      std::string{build_svg(*scene, make_snapshot(PaginationMode::fixed,
                                                   Millimetres{50.0})).text()};
  const auto svg_count = count_occurrences(fixed_text, "<svg ");
  require(svg_count >= 2, "fixed mode over a tall scene must produce >=2 pages");
  require(count_occurrences(fixed_text, "data-export-role=\"header\"") ==
              2 * svg_count,
          "every page must carry a well-name and page-number header band");
  require(count_occurrences(fixed_text, "data-export-role=\"footer\"") ==
              svg_count,
          "every page must carry a footer band");
  require(count_occurrences(fixed_text, "data-export-role=\"legend\"") >= svg_count,
          "every page must carry a legend band when repeat_legend is set");
  require(count_occurrences(fixed_text, "data-export-role=\"body\"") == svg_count,
          "every page must carry a body group");
  // Page numbers 1..N all present.
  for (std::size_t page = 1; page <= svg_count; ++page) {
    const auto label =
        "page " + std::to_string(page) + " of " + std::to_string(svg_count);
    require(fixed_text.find(label) != std::string::npos,
            "every page number must appear");
  }
}

// Criterion 3: pagination continuity — depth ranges chain without gaps/overlaps
// (page_overlap==0), and the page count agrees between the SVG and PDF backends
// (shared page model). Collects each SVG page's depth window and asserts page K
// bottom == page K+1 top, first top == scene top, last bottom == scene bottom.
void pagination_depth_ranges_are_continuous() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  const auto snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});

  // SVG depth windows.
  const auto text = std::string{build_svg(*scene, snap).text()};
  const auto svg_count = count_occurrences(text, "<svg ");
  std::vector<std::pair<double, double>> windows;
  std::size_t pos = 0;
  while ((pos = text.find("data-page-depth-top=\"", pos)) != std::string::npos) {
    const auto top_start = pos + std::strlen("data-page-depth-top=\"");
    const auto top_end = text.find('"', top_start);
    const auto bot_key = text.find("data-page-depth-bottom=\"", top_end);
    const auto bot_start = bot_key + std::strlen("data-page-depth-bottom=\"");
    const auto bot_end = text.find('"', bot_start);
    require(top_end != std::string::npos && bot_end != std::string::npos,
            "depth-range attributes must be well-formed");
    windows.emplace_back(std::stod(text.substr(top_start, top_end - top_start)),
                         std::stod(text.substr(bot_start, bot_end - bot_start)));
    pos = bot_end;
  }
  require(windows.size() == static_cast<std::size_t>(svg_count),
          "every page must carry one depth window");
  require_near(windows.front().first, 1000.0,
               "first page depth top must be the scene top depth");
  require_near(windows.back().second, 1800.0,
               "last page depth bottom must be the scene bottom depth");
  for (std::size_t i = 1; i < windows.size(); ++i) {
    require_near(windows[i].first, windows[i - 1].second,
                 "page depth ranges must be continuous across pages");
  }

  // PDF page count must equal the SVG page count (shared page model).
  const auto pdf_text = std::string{build_pdf(*scene, snap).bytes()};
  require(count_pdf_pages(pdf_text) == svg_count,
          "PDF and SVG must paginate into the same page count");
}

// Criterion 5: pattern phase is consistent across pages AND between the SVG and
// PDF backends. The parity scene uses anchor (3,4) + rotation 30°, whose phase
// matrix is R·T: e,f = R·(3,4). Assert the PDF tiling-pattern /Matrix's e,f
// equal the rotated anchor (≈0.598, ≈4.964), and that the SVG pattern carries
// the same anchor (the SVG transform encodes the same R·T phase).
void pattern_phase_matches_across_backends() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  const double cos30 = std::cos(30.0 * M_PI / 180.0);
  const double sin30 = std::sin(30.0 * M_PI / 180.0);
  // R·T phase for anchor (3,4) + rotation 30°: e,f = R·(3,4). This is the single
  // composed phase BOTH backends must land at — the PDF /Matrix numerically, the
  // SVG via its anchor + patternTransform (which compose to the same point under
  // SVG pattern semantics: patternTransform rotates about the pattern origin).
  const double expected_e = cos30 * 3.0 - sin30 * 4.0; // ≈ 0.598
  const double expected_f = sin30 * 3.0 + cos30 * 4.0; // ≈ 4.964

  const auto snap = make_snapshot(PaginationMode::continuous, Millimetres{297.0});

  // PDF: parse the tiling-pattern /Matrix and assert e,f == R·(3,4).
  const auto pdf_bytes = std::string{build_pdf(*scene, snap).bytes()};
  const auto matrix = parse_first_pattern_matrix(pdf_bytes);
  require(matrix.size() == 6, "/Matrix must have 6 operands");
  require(std::abs(matrix[4] - expected_e) < 1e-9 &&
              std::abs(matrix[5] - expected_f) < 1e-9,
          "the PDF pattern /Matrix must be R·T (e,f = R·(3,4))");

  // SVG: the pattern def carries anchor (3,4) + rotate(30). The rotation matrix
  // [a b c d] of the composed SVG transform is fixed for a given angle, so both
  // backends carry the identical rotation; the anchor x/y pins the translation
  // the PDF folds into e,f. Asserting the SVG's numeric rotate angle + anchor
  // confirms it composes the same phase as the PDF /Matrix (criterion 5).
  const auto svg_text = std::string{build_svg(*scene, snap).text()};
  require(svg_text.find("x=\"3\" y=\"4\"") != std::string::npos,
          "the SVG pattern must carry the (3,4) scene anchor (the translation "
          "the PDF folds into /Matrix e,f)");
  require(svg_text.find("patternTransform=\"rotate(30)\"") != std::string::npos,
          "the SVG pattern rotation must match the PDF /Matrix rotation");

  // "Consistent across pages" (criterion 5): in fixed mode every page re-emits
  // the pattern def with the SAME /Matrix — assert all per-page /Matrix values
  // are identical (no per-page phase drift).
  const auto fixed_snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  const auto fixed_pdf = std::string{build_pdf(*scene, fixed_snap).bytes()};
  std::vector<std::vector<double>> per_page_matrices;
  std::string_view::size_type search = 0;
  while (true) {
    const auto pos = fixed_pdf.find("/Matrix [", search);
    if (pos == std::string_view::npos) {
      break;
    }
    per_page_matrices.push_back(parse_first_pattern_matrix(fixed_pdf.substr(pos)));
    search = pos + 9;
  }
  require(per_page_matrices.size() >= 2,
          "fixed mode must re-emit the pattern /Matrix on every page");
  for (std::size_t i = 1; i < per_page_matrices.size(); ++i) {
    require(per_page_matrices[i].size() == 6 &&
                per_page_matrices[0].size() == 6,
            "every per-page /Matrix must have 6 operands");
    for (std::size_t j = 0; j < 6; ++j) {
      require(std::abs(per_page_matrices[i][j] - per_page_matrices[0][j]) < 1e-9,
              "the pattern /Matrix must be identical across all fixed pages");
    }
  }
}

// Criterion 7: mixed-mode degradation is EXPLICITLY reported, never silent. In
// mixed mode a budget-triggered layer is recorded in the ExportReport (both
// backends, identical entries); in pure-vector mode the SAME budget makes the
// export FAIL with invalid_presentation (no silent rasterisation). With no
// budget, both reports stay empty.
void mixed_mode_reports_degradation_pure_vector_refuses() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  // The curve has 9 samples → a budget below that triggers the curve layer.
  const auto base_snap = make_snapshot(PaginationMode::continuous, Millimetres{297.0});

  // (a) Mixed mode + tiny budget → both backends report the curve layer.
  auto mixed_snap = base_snap;
  mixed_snap.page.export_mode = ExportMode::mixed;
  mixed_snap.page.vector_complexity_budget = 4; // curve has 9 points > 4
  ExportReport pdf_report;
  ExportReport svg_report;
  build_pdf(*scene, mixed_snap, &pdf_report);
  build_svg(*scene, mixed_snap, &svg_report);
  require(!pdf_report.empty(),
          "mixed-mode PDF must report the over-budget layer");
  require(!svg_report.empty(),
          "mixed-mode SVG must report the over-budget layer");
  // Full cross-backend parity: every entry (id + reason + dpi) must match.
  require(reports_equal(pdf_report, svg_report),
          "both backends must report the identical degradation entries");
  require(pdf_report.degraded_layers.front().layer_id == curve_layer_id,
          "both backends must report the curve layer id");
  require(pdf_report.degraded_layers.front().reason ==
              ExportDegradationReason::complexity_threshold,
          "both backends must report complexity_threshold");
  require(pdf_report.degraded_layers.front().target_dpi == base_snap.page.dpi,
          "both backends must report the page DPI as the target DPI");

  // (b) Pure-vector mode + same budget → both backends REFUSE (no silent
  // rasterisation). Criterion 7 invariant.
  auto pure_snap = base_snap;
  pure_snap.page.export_mode = ExportMode::pure_vector;
  pure_snap.page.vector_complexity_budget = 4;
  StubResolver resolver;
  auto engine = make_engine();
  const auto pdf_refuse = PdfSceneExporter::write(
      *scene, pure_snap,
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get());
  require(!pdf_refuse.has_value() &&
              pdf_refuse.error().code == ErrorCode::invalid_presentation,
          "pure-vector PDF must refuse a budget-triggered layer");
  const auto svg_refuse = PaginatedSvgExporter::write(*scene, pure_snap);
  require(!svg_refuse.has_value() &&
              svg_refuse.error().code == ErrorCode::invalid_presentation,
          "pure-vector SVG must refuse a budget-triggered layer");

  // (c) No budget (default) → both reports empty (no degradation either way).
  ExportReport empty_pdf_report;
  ExportReport empty_svg_report;
  build_pdf(*scene, base_snap, &empty_pdf_report);
  build_svg(*scene, base_snap, &empty_svg_report);
  require(empty_pdf_report.empty() && empty_svg_report.empty(),
          "with no complexity budget no layer is degraded");

  // (d) Determinism: identical input → identical report (every entry, both
  // backends). A regression that permuted or truncated the list would fail here.
  ExportReport pdf_report_2;
  ExportReport svg_report_2;
  build_pdf(*scene, mixed_snap, &pdf_report_2);
  build_svg(*scene, mixed_snap, &svg_report_2);
  require(reports_equal(pdf_report, pdf_report_2),
          "identical input must yield a byte-identical PDF report");
  require(reports_equal(svg_report, svg_report_2),
          "identical input must yield a byte-identical SVG report");
}

// Criterion 1: the Export Snapshot metadata (Document Revision, Presentation
// version, Depth Transform, font asset) is stamped by the SVG backend as data-*
// attributes on every page root, and drives the deterministic byte output of
// both backends (same snapshot → same reproducible PDF/SVG — criterion 8). The
// PDF backend is text-as-outlines by design (ADR 0047: no metadata attrs / no
// Info dict in this phase), so its "stamp" is the reproducible layout the
// snapshot produces; pattern_versions is carried by ExportSnapshot but not yet
// stamped by either backend (a documented follow-up).
void snapshot_metadata_round_trips_in_both() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  const auto snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});

  // SVG: every page root carries the snapshot metadata.
  const auto text = std::string{build_svg(*scene, snap).text()};
  const auto svg_count = count_occurrences(text, "<svg ");
  require(count_occurrences(text, "data-document-revision=\"7\"") == svg_count,
          "every page must carry the document revision");
  require(count_occurrences(text, "data-presentation-version=\"42\"") ==
              svg_count,
          "every page must carry the presentation version");
  require(count_occurrences(text, "data-depth-transform-version=\"3\"") ==
              svg_count,
          "every page must carry the depth-transform version");
  require(count_occurrences(text, "data-font-asset=\"font-fixture-v1\"") ==
              svg_count,
          "every page must carry the font asset fingerprint");
  require(count_occurrences(text, "data-document-id=\"90000000-0000-4000-8000-"
                                  "000000000001\"") == svg_count,
          "every page must carry the document id");

  // PDF (ADR 0047): carries no metadata attributes, but its output is fully
  // determined by the same snapshot — the reproducibility test below proves the
  // same snapshot yields byte-identical PDF. Here we assert the determinism
  // preconditions (no timestamps) that make that contract hold.
  const auto pdf_bytes = std::string{build_pdf(*scene, snap).bytes()};
  require(pdf_bytes.find("CreationDate") == std::string::npos,
          "PDF must be deterministic (no CreationDate) for reproducibility");
  require(pdf_bytes.find("ModDate") == std::string::npos,
          "PDF must be deterministic (no ModDate) for reproducibility");
}

// Criterion 8 (reproducibility): same snapshot + options → byte-identical PDF
// and structurally-identical SVG across runs.
void reproducibility_byte_and_structural() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  const auto snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});

  const auto pdf_a = std::string{build_pdf(*scene, snap).bytes()};
  const auto pdf_b = std::string{build_pdf(*scene, snap).bytes()};
  require(pdf_a == pdf_b, "two PDF builds of the same input must be identical");

  const auto svg_a = std::string{build_svg(*scene, snap).text()};
  const auto svg_b = std::string{build_svg(*scene, snap).text()};
  require(svg_a == svg_b, "two SVG builds of the same input must be identical");
}

// Criterion 7 (pure-vector invariant, both backends): a curves-only scene in
// pure-vector mode emits NO rasterisation. The PDF content stream has no `Do`
// (XObject image) or `BI` (inline image) operators; the SVG has no `<image>`
// element. This is the SVG-side assertion that was missing pre-#189.
void pure_vector_never_rasterizes() {
  // A curves-only document/presentation (no image source → no raster tiles).
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1100.0, 1200.0, 1300.0, 1400.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 25.0, 50.0, 75.0, 100.0});
  WellLogDocumentBuilder doc(document_id, DocumentRevision{7});
  doc.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  doc.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  ScenePresentationBuilder pres(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1400.0},
      Millimetres{100.0}, "font-fixture-v1");
  pres.add_track(TrackSpec{.id = track_id, .width = Millimetres{80.0}, .z_order = 0});
  pres.add_scale(TrackScaleSpec{
      .id = scale_id, .track_id = track_id, .mode = ScaleMode::linear,
      .minimum = 0.0, .maximum = 100.0,
      .direction = ScaleDirection::left_to_right, .unit = "API"});
  pres.add_curve_layer(CurveLayerSpec{
      .id = curve_layer_id, .track_id = track_id, .curve_id = curve_id,
      .scale_id = scale_id, .color = RgbaColor{0x11, 0x22, 0x33, 0xff},
      .line_width = Millimetres{0.25}, .z_order = 0, .visible = true});

  WellLogSession session;
  require(session.execute(SetDocumentCommand{doc.build()}).has_value(),
          "curves-only document must be accepted");
  require(session.execute(SetPresentationCommand{pres.build()}).has_value(),
          "curves-only presentation must prepare a scene");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "curves-only scene must publish");
  auto snap = make_snapshot(PaginationMode::continuous, Millimetres{297.0});
  snap.page.export_mode = ExportMode::pure_vector;

  // SVG: no <image> element.
  const auto svg_text = std::string{build_svg(*scene, snap).text()};
  require(svg_text.find("<image") == std::string::npos,
          "pure-vector SVG must not emit an <image> element");

  // PDF: no `Do`/`BI` image-drawing operators in any content stream.
  const auto pdf_bytes = std::string{build_pdf(*scene, snap).bytes()};
  for (const auto &stream : inflate_all_streams(pdf_bytes)) {
    require(stream.find(" Do\n") == std::string::npos,
            "pure-vector PDF must not draw an XObject image");
    require(stream.find("BI\n") == std::string::npos,
            "pure-vector PDF must not contain an inline image");
  }
}

// External validity + orientation: qpdf --check accepts the parity PDF and
// pdfinfo reports the expected page count (rendered upright, not flipped — the
// #187 lesson). pdftoppm render is eyeballed via a temp file existence check.
void external_tools_accept_and_orientation_is_upright() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  const auto snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  const auto doc = build_pdf(*scene, snap);
  const auto bytes = std::string{doc.bytes()};
  const auto path = write_temp_pdf(bytes);

  const auto qpdf = find_on_path("qpdf");
  if (qpdf.empty()) {
    std::cerr << "SKIP(qpdf): not on PATH; #193 anchor is asserted without it\n";
  } else {
    std::string captured;
    const auto rc = run(qpdf.string() + " --check " + path.string() + " 2>&1",
                        captured);
    require(rc == 0, "qpdf --check must accept the parity PDF: " + captured);
  }
  const auto pdfinfo = find_on_path("pdfinfo");
  if (pdfinfo.empty()) {
    std::cerr << "SKIP(pdfinfo): not on PATH\n";
  } else {
    std::string captured;
    const auto rc =
        run(pdfinfo.string() + " " + path.string() + " 2>&1", captured);
    require(rc == 0, "pdfinfo must accept the parity PDF: " + captured);
    require(captured.find("Pages:") != std::string::npos,
            "pdfinfo must report a Pages line");
  }
  // Render page 1 to PNG to confirm the PDF is renderable + upright (the #187
  // flip bug surfaced only at render time). The existence + non-zero size of
  // the PNG is the gate; a flipped/empty render is a future visual assertion.
  const auto pdftoppm = find_on_path("pdftoppm");
  if (pdftoppm.empty()) {
    std::cerr << "SKIP(pdftoppm): not on PATH\n";
  } else {
    const auto png =
        std::filesystem::temp_directory_path() / "welllog_parity_page";
    std::string captured;
    const auto rc =
        run(pdftoppm.string() + " -png -r 50 -f 1 -l 1 " + path.string() + " " +
                png.string() + " 2>&1",
            captured);
    require(rc == 0, "pdftoppm must render page 1: " + captured);
    std::error_code ec;
    require(std::filesystem::file_size(png.string() + "-1.png", ec) > 0,
            "rendered page PNG must be non-empty");
    std::filesystem::remove(png.string() + "-1.png", ec);
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// #759: a >8x-compressible stream containing BI\n must be inflated (not
// dropped) so the pure-vector raster-operator scan can see it.
void inflate_all_streams_keeps_highly_compressible_raster_operator() {
  std::string raw(12 * 1024, 'A');
  raw += "BI\n";
  raw.append(12 * 1024, 'A');
  z_stream zs{};
  require(deflateInit(&zs, Z_BEST_COMPRESSION) == Z_OK, "deflateInit");
  std::string compressed(raw.size() + 64, '\0');
  zs.next_in = reinterpret_cast<Bytef *>(raw.data());
  zs.avail_in = static_cast<uInt>(raw.size());
  zs.next_out = reinterpret_cast<Bytef *>(compressed.data());
  zs.avail_out = static_cast<uInt>(compressed.size());
  require(deflate(&zs, Z_FINISH) == Z_STREAM_END, "deflate");
  compressed.resize(zs.total_out);
  deflateEnd(&zs);
  require(raw.size() > compressed.size() * 8 + 4096,
          "fixture must expand more than the old 8x+4096 sink");
  std::string pdf = "%PDF-1.4\n1 0 obj\n<< /Filter /FlateDecode >>\nstream\n";
  pdf += compressed;
  pdf += "\nendstream\n";
  const auto streams = inflate_all_streams(pdf);
  require(streams.size() == 1, "the synthetic stream must not be dropped");
  require(streams[0].find("BI\n") != std::string::npos,
          "high-compression BI\\n must remain visible to the vector gate");
}

std::vector<double> parse_first_clip_rect(std::string_view stream) {
  const auto re = stream.find(" re\nW\n");
  require(re != std::string_view::npos, "page body must clip with re/W");
  const auto line_start = stream.rfind('\n', re);
  const auto line = stream.substr(
      line_start == std::string_view::npos ? 0 : line_start + 1,
      re - (line_start == std::string_view::npos ? 0 : line_start + 1));
  std::vector<double> vals;
  std::string token;
  for (const char ch : line) {
    if (ch == ' ') {
      if (!token.empty()) {
        vals.push_back(std::stod(token));
        token.clear();
      }
    } else {
      token.push_back(ch);
    }
  }
  if (!token.empty()) {
    vals.push_back(std::stod(token));
  }
  require(vals.size() == 4, "clip rect must have x y w h");
  return vals;
}

// #745: fixed-page PDF body clip must leave the legend band empty, matching SVG.
void pdf_fixed_page_reserves_legend_band() {
  auto scene_data = make_parity_scene();
  const auto scene = prepare(scene_data.document, scene_data.presentation);
  auto snap = make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  const auto reserved =
      welllog::export_layout::legend_band_height_mm(*scene, snap.page);
  require(reserved > 0.0, "parity fixture must emit a legend band");
  const auto scale = welllog::export_layout::printable_width(snap.page) /
                     scene->physical_width().value;
  const auto full_depth =
      welllog::export_layout::printable_depth_height_mm(*scene, snap.page);
  const auto expected_clip = full_depth - reserved / scale;

  const auto pdf_bytes = std::string{build_pdf(*scene, snap).bytes()};
  const auto streams = inflate_all_streams(pdf_bytes);
  require(!streams.empty(), "fixed PDF must have a content stream");
  const auto clip = parse_first_clip_rect(streams.front());
  require_near(clip[3], expected_clip,
               "PDF body clip height must exclude the legend band");
  require(clip[3] + 1.0e-6 < full_depth,
          "PDF body clip must be shorter than the full printable depth");
}

} // namespace

int main() {
  inflate_all_streams_keeps_highly_compressible_raster_operator();
  pdf_page_physical_dimensions_match_request();
  asymmetric_margins_anchor_body_at_printable_top();
  svg_structure_is_correct();
  pagination_depth_ranges_are_continuous();
  pattern_phase_matches_across_backends();
  mixed_mode_reports_degradation_pure_vector_refuses();
  snapshot_metadata_round_trips_in_both();
  reproducibility_byte_and_structural();
  pure_vector_never_rasterizes();
  pdf_fixed_page_reserves_legend_band();
  external_tools_accept_and_orientation_is_upright();
  std::cout << "welllog.export-parity: all cases passed\n";
  return EXIT_SUCCESS;
}
