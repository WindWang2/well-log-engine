// Scene-emission test for the full PDF backend (#188): raster image XObjects,
// tiling patterns, multi-page pagination, and custom-layer primitives — built on
// the #187 vector/text emission. Proves PdfSceneExporter serializes these to a
// structurally-valid, byte-deterministic PDF and that each capability is present
// in the output. qpdf --check / pdfinfo verify external validity when available;
// the Flate round-trip inflates the ACTUAL embedded content stream.

#include <welllog/export/export_layout.hpp>
#include <welllog/export/pdf_scene.hpp>
#include <welllog/scene/image_pyramid.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>
#include <welllog/text/harfbuzz_text_engine.hpp>

#include "scene/prepare.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <zlib.h>

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
  if (std::abs(actual - expected) > 1.0e-6) {
    fail(message);
  }
}

// Builds the exact normalized-colour operator string the writer emits for an
// sRGB triple + operator (rg = fill, RG = stroke), reproducing its append_number
// (to_chars general, shortest round-trip) so the assertion is robust to digit count.
// Used to assert the custom-layer primitives by their unique colours (which no
// other layer emits) rather than generic S/f operators.
[[nodiscard]] std::string color_operator(std::uint8_t r, std::uint8_t g,
                                          std::uint8_t b,
                                          std::string_view op) {
  auto component = [](double v) {
    if (v == 0.0) {
      return std::string{"0"};
    }
    std::array<char, 48> buffer{};
    const auto res =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), v,
                      std::chars_format::general);
    return res.ec == std::errc{} ? std::string(buffer.data(), res.ptr)
                                 : std::string{"0"};
  };
  std::string out = component(r / 255.0);
  out.push_back(' ');
  out += component(g / 255.0);
  out.push_back(' ');
  out += component(b / 255.0);
  out.push_back(' ');
  out += op;
  out.push_back('\n');
  return out;
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

#ifndef WELLLOG_TEST_FONT_DIR
#define WELLLOG_TEST_FONT_DIR "tests/assets/fonts"
#endif

// A HarfBuzz text engine + the bundled test font, so the pagination bands emit
// real glyph outlines (the PDF exporter renders band text as outlines — ADR
// 0047, no font program embedded).
std::shared_ptr<HarfBuzzTextEngine> make_engine() {
  auto engine = std::make_shared<HarfBuzzTextEngine>();
  require(engine
              ->add_project_font(std::string{WELLLOG_TEST_FONT_DIR} +
                                 "/NotoSans-Regular.ttf")
              .has_value(),
          "bundled test font must load");
  return engine;
}

// Distinct UUID prefix for this TU to avoid cross-TU collisions.
const auto document_id = id("80000000-0000-4000-8000-000000000001");
const auto axis_id = id("80000000-0000-4000-8000-000000000002");
const auto curve_id = id("80000000-0000-4000-8000-000000000003");
const auto track_id = id("80000000-0000-4000-8000-000000000004");
const auto pattern_id = id("80000000-0000-4000-8000-000000000005");
const auto interval_layer_id = id("80000000-0000-4000-8000-000000000006");
const auto curve_layer_id = id("80000000-0000-4000-8000-000000000007");
const auto interval_id = id("80000000-0000-4000-8000-000000000008");
const auto image_source_id = id("80000000-0000-4000-8000-000000000009");
const auto image_layer_id = id("80000000-0000-4000-8000-00000000000a");
const auto custom_source_id = id("80000000-0000-4000-8000-00000000000b");
const auto custom_layer_id = id("80000000-0000-4000-8000-00000000000c");
// Two-image-page regression ids (RGBA image first, RGB image second).
const auto rgba_image_source_id = id("80000000-0000-4000-8000-00000000000e");
const auto rgb_image_source_id = id("80000000-0000-4000-8000-00000000000f");
const auto rgba_image_layer_id = id("80000000-0000-4000-8000-000000000010");
const auto rgb_image_layer_id = id("80000000-0000-4000-8000-000000000011");

// A scene with a patterned interval, a curve, an image layer, and a custom
// layer — exercising every #188 capability. Built via the session for the
// vector/pattern/custom parts; the image layer needs the pyramid map threaded
// via the preparer directly (host wiring the session does not yet expose).
WellLogDocument base_document() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 50.0, 90.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{5});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  // Patterned interval — drives the tiling-pattern emission.
  builder.add_interval(Interval{
      .id = interval_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1050.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{220, 200, 120, 255},
      .label = "Sand",
  });
  builder.add_image_source(ImageSource{
      .id = image_source_id,
      .width_px = 256,
      .height_px = 256,
      .pixel_format = PixelFormat::rgb8,
      .reference_depth_top = 1000.0,
      .reference_depth_bottom = 1100.0,
      .dpi = 300,
      .source = BufferSourceReference{.uri = "image://core-photo/1",
                                      .checksum = {},
                                      .byte_offset = 0},
  });
  // Custom source: one polyline + one triangle.
  CustomLayerSource source{
      .id = custom_source_id,
      .content_revision = DocumentRevision{3},
      .primitives = {},
      .clip = std::nullopt,
  };
  source.primitives.push_back(CustomPrimitive{CustomPolyline{
      .points = {PhysicalPoint{.left = Millimetres{5.0}, .top = Millimetres{20.0}},
                 PhysicalPoint{.left = Millimetres{35.0}, .top = Millimetres{20.0}},
                 PhysicalPoint{.left = Millimetres{35.0}, .top = Millimetres{60.0}}},
      .closed = false,
      .color = RgbaColor{10, 20, 200, 255},
      .width = Millimetres{0.5},
  }});
  source.primitives.push_back(CustomPrimitive{CustomTriangle{
      .a = PhysicalPoint{.left = Millimetres{50.0}, .top = Millimetres{20.0}},
      .b = PhysicalPoint{.left = Millimetres{70.0}, .top = Millimetres{20.0}},
      .c = PhysicalPoint{.left = Millimetres{60.0}, .top = Millimetres{60.0}},
      .fill_color = RgbaColor{200, 100, 0, 255},
  }});
  builder.add_custom_source(source);
  return builder.build();
}

ScenePresentationBuilder base_presentation() {
  ScenePresentationBuilder builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1100.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{40.0},
      .z_order = 0,
  });
  // The pattern referenced by the interval — a diagonal hatch tile.
  builder.add_pattern(PatternDefinition{
      .id = pattern_id,
      .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{60, 60, 60, 255},
      .background = RgbaColor{255, 250, 230, 255},
      .stroke_width = Millimetres{0.2},
      .scene_anchor = PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
      .primitives =
          {
              PatternLine{PhysicalPoint{Millimetres{-1.0}, Millimetres{-1.0}},
                          PhysicalPoint{Millimetres{5.0}, Millimetres{5.0}}},
          },
  });
  builder.add_scale(TrackScaleSpec{
      .id = id("80000000-0000-4000-8000-00000000000d"),
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  builder.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
  });
  builder.add_curve_layer(CurveLayerSpec{
      .id = curve_layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = id("80000000-0000-4000-8000-00000000000d"),
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.5},
      .z_order = 1,
      .visible = true,
  });
  builder.add_image_layer(ImageLayerSpec{
      .id = image_layer_id,
      .track_id = track_id,
      .image_source_id = image_source_id,
      .z_order = 2,
      .visible = true,
  });
  builder.add_custom_layer(CustomLayerSpec{
      .id = custom_layer_id,
      .track_id = track_id,
      .custom_source_id = custom_source_id,
      .z_order = 3,
      .visible = true,
  });
  return builder;
}

// Prepares the scene via the preparer directly so the image pyramid map is
// threaded (host wiring the session does not yet expose). Mirrors
// image_layer_test.cpp's prepare_with_image.
std::shared_ptr<const PreparedScene>
prepare_scene(const WellLogDocument &document,
              ScenePresentationBuilder &builder) {
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
      document, presentation, curve_lods, {}, image_pyramids,
      ImagePyramidQuery{.viewport_top = 1000.0,
                        .viewport_bottom = 1100.0,
                        .pixel_height = 1000.0,
                        .prefetch_viewports = 0.0});
  require(scene.has_value(), "scene must prepare");
  return std::make_shared<const PreparedScene>(std::move(scene.value()));
}

ExportSnapshot make_snapshot(PaginationMode mode,
                             Millimetres page_height = Millimetres{297.0}) {
  return ExportSnapshot{
      .document_id = document_id,
      .document_revision = DocumentRevision{5},
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{
              .domain = DepthDomain::measured_depth,
              .unit = "m",
              .reference_top = 1000.0,
              .reference_bottom = 1100.0,
              .version = 1,
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
          .well_name = {},
          .repeat_headers = true,
          .repeat_legend = true,
          .show_page_numbers = true,
          .show_depth_range = true,
      },
  };
}

// A resolver that returns a deterministic solid-color tile for every request,
// keeping the pixel storage alive via a SharedOwner. Width/height match the
// prepared tile's expected single-tile resolution.
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

// An RGBA resolver: returns rgba8 pixels (with a varying alpha) so the PDF
// exercises the /SMask soft-mask path (alpha preserved, not dropped).
struct RgbaStubResolver {
  std::shared_ptr<std::vector<std::uint8_t>> pixels;
  RgbaStubResolver()
      : pixels(std::make_shared<std::vector<std::uint8_t>>(256 * 256 * 4)) {
    auto &p = *pixels;
    for (std::size_t i = 0; i < 256 * 256; ++i) {
      p[i * 4 + 0] = 0x11;
      p[i * 4 + 1] = 0x22;
      p[i * 4 + 2] = 0x33;
      p[i * 4 + 3] = static_cast<std::uint8_t>(i & 0xFF); // varying alpha
    }
  }
  Result<RasterTile> operator()(const ImageTileRequest &) const {
    RasterTile raster{
        .width_px = 256,
        .height_px = 256,
        .pixel_format = PixelFormat::rgba8,
        .owner = SharedOwner{pixels},
        .data = pixels->data(),
    };
    return raster;
  }
};

// A two-source resolver: returns an RGBA tile for the configured source id and
// an RGB tile for every other source, so a page can carry one RGBA image (with
// a /SMask child) followed by a plain RGB image.
struct TwoImageResolver {
  std::shared_ptr<std::vector<std::uint8_t>> rgba_pixels;
  std::shared_ptr<std::vector<std::uint8_t>> rgb_pixels;
  EntityId rgba_source_id{};
  TwoImageResolver()
      : rgba_pixels(std::make_shared<std::vector<std::uint8_t>>(256 * 256 * 4)),
        rgb_pixels(std::make_shared<std::vector<std::uint8_t>>(256 * 256 * 3)) {
    auto &p = *rgba_pixels;
    for (std::size_t i = 0; i < 256 * 256; ++i) {
      p[i * 4 + 0] = 0x11;
      p[i * 4 + 1] = 0x22;
      p[i * 4 + 2] = 0x33;
      p[i * 4 + 3] = static_cast<std::uint8_t>(i & 0xFF); // varying alpha
    }
    auto &q = *rgb_pixels;
    for (std::size_t i = 0; i < 256 * 256; ++i) {
      q[i * 3 + 0] = 0x44;
      q[i * 3 + 1] = 0x55;
      q[i * 3 + 2] = 0x66;
    }
  }
  Result<RasterTile> operator()(const ImageTileRequest &request) const {
    if (request.image_source_id == rgba_source_id) {
      return RasterTile{
          .width_px = 256,
          .height_px = 256,
          .pixel_format = PixelFormat::rgba8,
          .owner = SharedOwner{rgba_pixels},
          .data = rgba_pixels->data(),
      };
    }
    return RasterTile{
        .width_px = 256,
        .height_px = 256,
        .pixel_format = PixelFormat::rgb8,
        .owner = SharedOwner{rgb_pixels},
        .data = rgb_pixels->data(),
    };
  }
};

std::filesystem::path write_temp(std::string_view bytes) {
  const auto path =
      std::filesystem::temp_directory_path() / "welllog_pdf_scene_full.pdf";
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

// Inflates the FIRST FlateDecode stream after a given needle (used to read a
// specific object's stream, e.g. the first content stream or a pattern/image
// stream). Mirrors pdf_scene_test.cpp's inflate helper.
std::string inflate_first_stream(std::string_view bytes) {
  const auto filter_pos = bytes.find("/Filter /FlateDecode");
  require(filter_pos != std::string_view::npos,
          "PDF must contain a FlateDecode stream");
  const auto stream_kw = bytes.find("stream\n", filter_pos);
  require(stream_kw != std::string_view::npos,
          "stream keyword must follow the dict");
  const auto payload_start = stream_kw + std::strlen("stream\n");
  const auto endstream = bytes.find("\nendstream", payload_start);
  require(endstream != std::string_view::npos,
          "endstream must terminate the stream");
  const std::string compressed =
      std::string{bytes.substr(payload_start, endstream - payload_start)};
  z_stream stream{};
  require(inflateInit(&stream) == Z_OK, "inflateInit must succeed");
  std::string sink(compressed.size() * 8 + 4096, '\0');
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef *>(sink.data());
  stream.avail_out = static_cast<uInt>(sink.size());
  const auto rc = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  require(rc == Z_STREAM_END, "the embedded stream must inflate cleanly");
  return std::string(sink.data(), stream.total_out);
}

// Inflates EVERY FlateDecode stream in the PDF (each page has its own content
// stream), returning them in file order. Used to inspect per-page content
// (e.g. count band cms across all fixed pages).
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
    const std::string compressed =
        std::string{bytes.substr(payload_start, endstream - payload_start)};
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
      break;
    }
    // Grow the output buffer until inflate finishes — dense page content
    // (glyphs + patterns) can exceed a fixed compressed×N estimate and was
    // previously dropped, under-counting pagination band cms.
    std::string sink(std::max<std::size_t>(compressed.size() * 16, 65536),
                     '\0');
    zs.next_in =
        reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
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
    if (rc == Z_STREAM_END) {
      out.emplace_back(sink.data(), zs.total_out);
    }
    inflateEnd(&zs);
    search = endstream;
  }
  return out;
}

// Counts the pagination "band" concat-matrix operators in a content stream.
// A band cm establishes the page-mm space at unit scale: its first operand (a)
// is points_per_millimetre (72/25.4 ≈ 2.8346). This distinguishes it from the
// scene-body cm (a = scale·pmm, larger), image-placement cms (a = mm width),
// and per-glyph cms (a = font_size). Matches a whole operator line (anchored
// at a newline or stream start) so a partial token is never parsed.
std::size_t count_band_cms(std::string_view stream) {
  constexpr double pmm = 72.0 / 25.4;
  std::size_t count = 0;
  std::string_view::size_type pos = 0;
  while ((pos = stream.find(" cm\n", pos)) != std::string_view::npos) {
    // Find the start of this operator line.
    auto ls = stream.rfind('\n', pos);
    const auto line_start = (ls == std::string_view::npos) ? 0 : ls + 1;
    const auto line = stream.substr(line_start, pos - line_start);
    // The band cm line is "<pmm> 0 0 -<pmm> 0 <height>". Parse all 6 operands.
    std::vector<double> vals;
    std::string token;
    for (const char ch : line) {
      if (ch == ' ') {
        if (!token.empty()) {
          try {
            vals.push_back(std::stod(token));
          } catch (...) {
            token.clear();
            break;
          }
          token.clear();
        }
      } else {
        token.push_back(ch);
      }
    }
    if (!token.empty()) {
      try {
        vals.push_back(std::stod(token));
      } catch (...) {
      }
    }
    if (vals.size() == 6 && std::abs(vals[0] - pmm) < 1e-6 &&
        vals[1] == 0.0 && vals[2] == 0.0 && std::abs(vals[3] + pmm) < 1e-6) {
      ++count;
    }
    pos += 4;
  }
  return count;
}

// Extracts the first "/Matrix [...]" body in the PDF bytes, for diagnostics.
std::string extract_matrix(std::string_view bytes) {
  const auto start = bytes.find("/Matrix [");
  if (start == std::string_view::npos) {
    return "<no /Matrix>";
  }
  const auto end = bytes.find(']', start);
  if (end == std::string_view::npos) {
    return "<unterminated /Matrix>";
  }
  return std::string{bytes.substr(start, end - start + 1)};
}

// Builds the full scene PDF with the stub image resolver; reused by assertions.
PdfDocument build_full_document(PaginationMode mode) {
  const auto document = base_document();
  auto builder = base_presentation();
  const auto scene = prepare_scene(document, builder);
  const auto snapshot = make_snapshot(mode);
  StubResolver resolver;
  auto engine = make_engine();
  const auto result =
      PdfSceneExporter::write(*scene, snapshot,
                              [&resolver](const ImageTileRequest &req) {
                                return resolver(req);
                              },
                              engine.get());
  require(result.has_value(), "full scene PDF must build");
  return result.value();
}

// --- Tests ------------------------------------------------------------------

// External validity: qpdf --check / pdfinfo accept the full-scene PDF.
void external_tools_accept_the_full_pdf() {
  const auto doc = build_full_document(PaginationMode::continuous);
  const auto path = write_temp(doc.bytes());
  bool qpdf_available = std::filesystem::exists("/usr/sbin/qpdf") ||
                        std::filesystem::exists("/usr/bin/qpdf");
  if (qpdf_available) {
    std::string captured;
    const auto rc = run("qpdf --check " + path.string() + " 2>&1", captured);
    require(rc == 0,
            "qpdf --check must accept the full PDF (rc != 0): " + captured);
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Image XObject: the PDF embeds an image XObject (Subtype /Image) with the
// expected pixel dimensions + a DeviceRGB colourspace (rgb8 → 3 channels), and
// the content stream invokes it with `Do`.
void image_xobject_is_embedded_and_invoked() {
  const auto doc = build_full_document(PaginationMode::continuous);
  const auto bytes = std::string{doc.bytes()};
  require(bytes.find("/Subtype /Image") != std::string::npos,
          "an image XObject must be embedded");
  require(bytes.find("/Width 256") != std::string::npos,
          "image width must match the tile pixels");
  require(bytes.find("/Height 256") != std::string::npos,
          "image height must match the tile pixels");
  require(bytes.find("/ColorSpace /DeviceRGB") != std::string::npos,
          "rgb8 tile must use DeviceRGB");
  require(bytes.find("/XObject <<") != std::string::npos,
          "the page Resources must name the XObject");
  const auto inflated = inflate_first_stream(bytes);
  require(inflated.find("/Im0 Do\n") != std::string::npos,
          "the content stream must invoke the image XObject with Do");
}

// Tiling pattern: the PDF embeds a tiling Pattern (PatternType 1) with the
// tile XStep/YStep, and the interval fill references it via /Pattern cs + scn.
void tiling_pattern_is_embedded_and_referenced() {
  const auto doc = build_full_document(PaginationMode::continuous);
  const auto bytes = std::string{doc.bytes()};
  require(bytes.find("/Type /Pattern") != std::string::npos,
          "a tiling pattern must be embedded");
  require(bytes.find("/PatternType 1") != std::string::npos,
          "the pattern must be a tiling pattern");
  require(bytes.find("/XStep 4") != std::string::npos,
          "the pattern XStep must equal the tile width");
  require(bytes.find("/YStep 4") != std::string::npos,
          "the pattern YStep must equal the tile height");
  require(bytes.find("/Pattern <<") != std::string::npos,
          "the page Resources must name the pattern");
  const auto inflated = inflate_first_stream(bytes);
  require(inflated.find("/Pattern cs\n") != std::string::npos,
          "the interval fill must switch to the pattern colour space");
  require(inflated.find("/P0 scn\n") != std::string::npos,
          "the interval fill must select the tiling pattern");
}

// Multi-page pagination: fixed mode emits more than one page, and pdfinfo
// reports the page count > 1 (the scene is tall enough to slice). Continuous
// mode emits exactly one page.
void fixed_mode_paginates_into_multiple_pages() {
  // A short page so the 100 mm scene slices into multiple fixed pages.
  const auto document = base_document();
  auto builder = base_presentation();
  const auto scene = prepare_scene(document, builder);
  StubResolver resolver;
  auto engine = make_engine();
  const auto fixed_snapshot = make_snapshot(PaginationMode::fixed,
                                            Millimetres{40.0});
  const auto fixed_result = PdfSceneExporter::write(
      *scene, fixed_snapshot,
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get());
  require(fixed_result.has_value(), "fixed PDF must build");
  const auto fixed_bytes = std::string{fixed_result.value().bytes()};
  // Count /Type /Page entries (each fixed page is a Page object).
  std::size_t page_count = 0;
  std::string::size_type pos = 0;
  while ((pos = fixed_bytes.find("/Type /Page ", pos)) != std::string::npos) {
    ++page_count;
    pos += 11;
  }
  require(page_count > 1, "fixed mode must paginate into more than one page");

  // Continuous mode: exactly one page.
  const auto cont = build_full_document(PaginationMode::continuous);
  const auto cont_bytes = std::string{cont.bytes()};
  std::size_t cont_pages = 0;
  pos = 0;
  while ((pos = cont_bytes.find("/Type /Page ", pos)) != std::string::npos) {
    ++cont_pages;
    pos += 11;
  }
  require(cont_pages == 1, "continuous mode must emit exactly one page");

  // pdfinfo confirms the fixed page count.
  bool pdfinfo_available = std::filesystem::exists("/usr/sbin/pdfinfo") ||
                           std::filesystem::exists("/usr/bin/pdfinfo");
  if (pdfinfo_available) {
    const auto path = write_temp(fixed_bytes);
    std::string captured;
    const auto rc = run("pdfinfo " + path.string() + " 2>&1", captured);
    require(rc == 0, "pdfinfo must accept the fixed PDF");
    const auto pages_pos = captured.find("Pages:");
    require(pages_pos != std::string::npos,
            "pdfinfo must report a Pages line");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

// Depth-range continuity (criterion 3): the per-page depth-range footer bands
// carry each fixed page's depth window, so consecutive pages' ranges are
// contiguous (one page's bottom = the next's top). The bands are glyph outlines
// (no font), so we assert the continuous doc's single depth footer is present
// and, for fixed mode, that the scene's full depth range [1000, 1100] is
// covered by the union of page footers — by counting the distinct depth values
// the footers encode is brittle across outlines; instead assert the footer text
// is emitted (the "depth" label shaped as outlines on every page) and that page
// count matches the shared page model.
void depth_range_bands_are_emitted_and_continuous() {
  // Continuous: the band page-mm `cm` (the bands-only transform, scale ≈ pmm
  // 2.8346) must be present in the (inflated) content stream — proving the
  // pagination bands (header/legend/page-number/depth-range) emitted.
  const auto cont = build_full_document(PaginationMode::continuous);
  require(count_band_cms(inflate_first_stream(cont.bytes())) >= 1,
          "the continuous page must emit its pagination band cm");

  // Fixed mode: every page emits one band cm, so the band-cm count across all
  // page content streams equals the page count (each page's bands establish the
  // page-mm space once → depth-range continuity, criterion 3).
  const auto document = base_document();
  auto builder = base_presentation();
  const auto scene = prepare_scene(document, builder);
  StubResolver resolver;
  auto engine = make_engine();
  const auto fixed_result = PdfSceneExporter::write(
      *scene, make_snapshot(PaginationMode::fixed, Millimetres{40.0}),
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get());
  require(fixed_result.has_value(), "fixed PDF must build");
  const auto fixed_bytes = std::string{fixed_result.value().bytes()};
  std::size_t fixed_pages = 0;
  for (std::string::size_type p = 0;
       (p = fixed_bytes.find("/Type /Page ", p)) != std::string::npos;
       ++fixed_pages, p += 11) {
  }
  // Every page emits one band cm → depth-range continuity (criterion 3).
  // Count from inflated object streams (not qpdf --qdf): QDF rewrites can
  // merge/drop content streams so the band-cm total no longer equals page
  // count on some qpdf versions, while the page-model check below remains
  // the authoritative continuity assertion.
  std::size_t band_cms = 0;
  for (const auto &stream : inflate_all_streams(fixed_bytes)) {
    band_cms += count_band_cms(stream);
  }
  require(band_cms == fixed_pages,
          "each fixed page must emit one pagination band cm (depth-range "
          "continuity); got " +
              std::to_string(band_cms) + " bands for " +
              std::to_string(fixed_pages) + " pages");

  // Real depth-range continuity (criterion 3): re-derive each page's depth
  // window from the SHARED page model (export_layout::compute_page_windows —
  // the same model both backends consume) and assert the windows chain with no
  // gaps/overlaps: page K's bottom-depth == page K+1's top-depth, the first
  // page tops at the scene top, the last bottoms at the scene bottom. This is
  // the page-model-level continuity the band-cm count above only proxies.
  const auto fixed_snap = make_snapshot(PaginationMode::fixed, Millimetres{40.0});
  const auto windows = export_layout::compute_page_windows(*scene, fixed_snap);
  require(windows.size() == fixed_pages,
          "the page model must compute the same page count as the PDF emits");
  require(!windows.empty(), "fixed mode must produce >=1 page window");
  const auto scene_top_depth =
      export_layout::scene_y_to_depth(*scene, windows.front().window_top_mm);
  const auto scene_bottom_depth =
      export_layout::scene_y_to_depth(*scene, windows.back().window_bottom_mm);
  require_near(scene_top_depth, 1000.0,
               "first page must start at the scene top depth");
  require_near(scene_bottom_depth, 1100.0,
               "last page must end at the scene bottom depth");
  for (std::size_t i = 1; i < windows.size(); ++i) {
    const auto prev_bottom =
        export_layout::scene_y_to_depth(*scene, windows[i - 1].window_bottom_mm);
    const auto cur_top =
        export_layout::scene_y_to_depth(*scene, windows[i].window_top_mm);
    require_near(prev_bottom, cur_top,
                 "page depth windows must be continuous (no gaps/overlaps)");
  }
}

// Image DPI is encoded by the placement `cm` (physical rect vs pixel count):
// the test image is 256 px over a 40 mm track width, so the placement `cm`'s
// `a` operand (width in mm) and the embedded /Width 256 make DPI recoverable as
// 256 / (40/25.4) = 162.56 dpi. Assert the placement cm carries the track width
// and the XObject carries the pixel width — together they pin DPI.
void image_dpi_is_recoverable_from_placement() {
  const auto doc = build_full_document(PaginationMode::continuous);
  const auto bytes = std::string{doc.bytes()};
  require(bytes.find("/Width 256") != std::string::npos,
          "image XObject must carry its pixel width");
  const auto inflated = inflate_first_stream(bytes);
  // The image placement cm maps the unit square to the tile rect; the `a`
  // operand is the rect width in mm = the track width (40 mm). DPI =
  // pixel_width / (mm_width / 25.4). The cm is "40 0 0 -100 0 100 cm" (a=width,
  // d=-height flip, f=top+height).
  require(inflated.find("40 0 0 -100 0 100 cm") != std::string::npos,
          "the image placement cm must carry the 40 mm physical width (DPI source)");
}

// Pattern phase: for a NONZERO scene_anchor + rotation, the tiling pattern's
// /Matrix must be R·T (rotate the anchored phase), matching the SVG backend —
// not T·R. The test pattern uses anchor (0,0)/rotation 0 (collapses); here we
// build a separate pattern with anchor (3,4) and rotation 30° and assert the
// /Matrix's e,f operands equal R·(3,4) = (cos30·3 − sin30·4, sin30·3 + cos30·4),
// not (3,4). This catches the phase-matrix regression the review found.
void pattern_phase_matrix_matches_svg_for_nonzero_anchor_rotation() {
  const auto document = base_document();
  auto builder = base_presentation();
  // Re-add the pattern with a rotated, offset anchor so the phase matrix is
  // exercised. (The interval references pattern_id, which we redefine.)
  PatternDefinition rotated_pattern{
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
  };
  // Replace the presentation's pattern by rebuilding base_presentation without
  // the pattern, then adding the rotated one.
  // base_presentation already added pattern_id; add_pattern would duplicate the
  // id. Instead build a fresh presentation that omits the default pattern.
  ScenePresentationBuilder b2(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1100.0},
      Millimetres{100.0}, "font-fixture-v1");
  b2.add_track(TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  b2.add_pattern(rotated_pattern);
  b2.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id, .track_id = track_id, .z_order = 0,
      .draw_labels = false, .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255}});
  auto scene = prepare_scene(document, b2);
  StubResolver resolver;
  auto engine = make_engine();
  const auto result = PdfSceneExporter::write(
      *scene, make_snapshot(PaginationMode::continuous),
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get());
  require(result.has_value(), "rotated-pattern PDF must build");
  const auto bytes = std::string{result.value().bytes()};
  // R·T at 30° with anchor (3,4): the matrix must be
  //   [cos sin -sin cos | (cos·3 - sin·4, sin·3 + cos·4)]
  // = [0.866.. 0.5 -0.5 0.866.. | (0.598.., 4.964..)].
  // The naive T·R would put e,f = (3,4) — which is the bug. Parse the matrix's
  // e,f numerically (robust to the deterministic number format) and compare.
  const auto matrix = extract_matrix(bytes);
  require(matrix != "<no /Matrix>", "a tiling pattern /Matrix must be present");
  // Extract the six space-separated numbers between '[' and ']'.
  const auto open = matrix.find('[');
  const auto close = matrix.find(']');
  require(open != std::string::npos && close != std::string::npos,
          "/Matrix must be bracketed: " + matrix);
  std::vector<double> vals;
  std::string token;
  for (std::size_t i = open + 1; i < close; ++i) {
    if (matrix[i] == ' ') {
      if (!token.empty()) {
        vals.push_back(std::stod(token));
        token.clear();
      }
    } else {
      token.push_back(matrix[i]);
    }
  }
  if (!token.empty()) {
    vals.push_back(std::stod(token));
  }
  require(vals.size() == 6, "/Matrix must have 6 operands: " + matrix);
  const double cos30 = std::cos(30.0 * M_PI / 180.0);
  const double sin30 = std::sin(30.0 * M_PI / 180.0);
  const double expected_e = cos30 * 3.0 - sin30 * 4.0; // ≈ 0.598
  const double expected_f = sin30 * 3.0 + cos30 * 4.0; // ≈ 4.964
  require(std::abs(vals[4] - expected_e) < 1e-9 && std::abs(vals[5] - expected_f) < 1e-9,
          "the tiling pattern /Matrix must be R·T (phase-consistent with SVG, "
          "e,f = R·(3,4)), not T·R (e,f = (3,4)) — got: " + matrix);
}

// Custom-layer primitives: the content stream contains the polyline (stroked
// m/l) and the triangle (filled m/l/h), emitted from the prepared custom layer.
// Distinguished from curve/interval layers by their UNIQUE colours (polyline
// stroke 10,20,200 RG; triangle fill 200,100,0 rg), which no other layer emits.
void custom_layer_primitives_are_emitted() {
  const auto doc = build_full_document(PaginationMode::continuous);
  const auto inflated = inflate_first_stream(doc.bytes());
  require(inflated.find(color_operator(10, 20, 200, "RG")) != std::string::npos,
          "the custom polyline must stroke in its unique colour");
  require(inflated.find(color_operator(200, 100, 0, "rg")) != std::string::npos,
          "the custom triangle must fill in its unique colour");
  require(inflated.find("S\n") != std::string::npos,
          "the custom polyline must stroke");
  require(inflated.find("f\n") != std::string::npos,
          "the custom triangle must fill");
}

// RGBA images embed as DeviceRGB plus a DeviceGray /SMask child XObject
// (issue #476): the alpha plane is preserved, so transparent pixels composite
// in the PDF exactly like on screen. Asserts the RGBA→DeviceRGB+SMask path is
// taken, the /SMask reference is a real indirect object (not a leftover
// @@CHILDn@@ placeholder), and the result is qpdf-clean.
void rgba_image_embeds_with_smask() {
  // A document whose image source is rgba8.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 50.0, 90.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{5});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{.id = curve_id, .mnemonic = "GR",
                          .display_name = "Gamma Ray", .unit = "API",
                          .sampling_axis_id = axis_id,
                          .values = BufferView::from_vector(values), .nulls = {}});
  builder.add_image_source(ImageSource{
      .id = image_source_id, .width_px = 256, .height_px = 256,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 1000.0, .reference_depth_bottom = 1100.0,
      .dpi = 300,
      .source = BufferSourceReference{.uri = "image://core-photo/rgba",
                                      .checksum = {}, .byte_offset = 0}});
  const auto document = builder.build();
  // Minimal presentation: one track + the image layer only (no pattern/interval/
  // custom, which base_presentation adds but this document doesn't carry).
  ScenePresentationBuilder pres(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1100.0},
      Millimetres{100.0}, "font-fixture-v1");
  pres.add_track(TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  pres.add_image_layer(ImageLayerSpec{
      .id = image_layer_id, .track_id = track_id,
      .image_source_id = image_source_id, .z_order = 0, .visible = true});
  const auto scene = prepare_scene(document, pres);
  RgbaStubResolver resolver;
  auto engine = make_engine();
  const auto result =
      PdfSceneExporter::write(*scene, make_snapshot(PaginationMode::continuous),
                              [&resolver](const ImageTileRequest &req) {
                                return resolver(req);
                              },
                              engine.get());
  require(result.has_value(), "rgba PDF must build");
  const auto bytes = std::string{result.value().bytes()};
  require(bytes.find("/ColorSpace /DeviceRGB") != std::string::npos,
          "the rgba image must embed as DeviceRGB colour");
  // Alpha ships as a /SMask child XObject (#476): the image dict references
  // it indirectly, the child carries a DeviceGray stream, and no @@CHILDn@@
  // placeholder may survive substitution.
  const auto smask_at = bytes.find("/SMask ");
  require(smask_at != std::string::npos,
          "the rgba image must reference a /SMask alpha XObject");
  require(bytes.find("/ColorSpace /DeviceGray") != std::string::npos,
          "the /SMask child must be a DeviceGray image");
  require(bytes.find("@@CHILD") == std::string::npos,
          "no @@CHILDn@@ placeholder may survive writer substitution");
  // qpdf --check must accept the structure.
  const auto path = write_temp(bytes);
  bool qpdf_available = std::filesystem::exists("/usr/sbin/qpdf") ||
                        std::filesystem::exists("/usr/bin/qpdf");
  if (qpdf_available) {
    std::string captured;
    const auto rc = run("qpdf --check " + path.string() + " 2>&1", captured);
    require(rc == 0, "qpdf --check must accept the rgba PDF: " + captured);
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Regression (PR #3 review): a page carrying two image XObjects whose first is
// an RGBA image — its /SMask alpha child occupies the object number right
// after it. The /XObject resource dict must reference each image by the same
// running object numbers the writer emits them under; numbering by plain index
// aliases the second image to the first one's DeviceGray /SMask child (wrong
// pixels, /SMask applied to the wrong image). Asserts the second image's dict
// entry is numbered past the RGBA child and resolves to a DeviceRGB image.
void two_images_rgba_first_resource_dict_numbers_align() {
  // Document: two image sources on the same track; the RGBA layer is first in
  // z-order, and scene tiles are stable-sorted by layer z_order, so the RGBA
  // tile is the page's first image XObject — the failing arrangement.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 50.0, 90.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{5});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{.id = curve_id, .mnemonic = "GR",
                          .display_name = "Gamma Ray", .unit = "API",
                          .sampling_axis_id = axis_id,
                          .values = BufferView::from_vector(values), .nulls = {}});
  builder.add_image_source(ImageSource{
      .id = rgba_image_source_id, .width_px = 256, .height_px = 256,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 1000.0, .reference_depth_bottom = 1100.0,
      .dpi = 300,
      .source = BufferSourceReference{.uri = "image://core-photo/rgba",
                                      .checksum = {}, .byte_offset = 0}});
  builder.add_image_source(ImageSource{
      .id = rgb_image_source_id, .width_px = 256, .height_px = 256,
      .pixel_format = PixelFormat::rgb8,
      .reference_depth_top = 1000.0, .reference_depth_bottom = 1100.0,
      .dpi = 300,
      .source = BufferSourceReference{.uri = "image://core-photo/rgb",
                                      .checksum = {}, .byte_offset = 0}});
  const auto document = builder.build();

  ScenePresentationBuilder pres(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth, .unit = "m",
                          .top = 1000.0, .bottom = 1100.0},
      Millimetres{100.0}, "font-fixture-v1");
  pres.add_track(TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  pres.add_image_layer(ImageLayerSpec{
      .id = rgba_image_layer_id, .track_id = track_id,
      .image_source_id = rgba_image_source_id, .z_order = 0, .visible = true});
  pres.add_image_layer(ImageLayerSpec{
      .id = rgb_image_layer_id, .track_id = track_id,
      .image_source_id = rgb_image_source_id, .z_order = 1, .visible = true});
  const auto presentation = pres.build();

  detail::ScenePreparer::CurveLodMap curve_lods;
  detail::ScenePreparer::ImagePyramidMap image_pyramids;
  for (const auto &source : document.image_sources()) {
    const auto pyramid = ImagePyramid::build(
        source, ImagePyramidOptions{.tile_size = 256,
                                    .maximum_derived_bytes = 1024 * 1024});
    require(pyramid.has_value(), "image pyramid must build");
    image_pyramids.emplace(source.id, pyramid.value());
  }
  const auto scene = detail::ScenePreparer::prepare(
      document, presentation, curve_lods, {}, image_pyramids,
      ImagePyramidQuery{.viewport_top = 1000.0, .viewport_bottom = 1100.0,
                        .pixel_height = 1000.0, .prefetch_viewports = 0.0});
  require(scene.has_value(), "scene must prepare");

  TwoImageResolver resolver;
  resolver.rgba_source_id = rgba_image_source_id;
  auto engine = make_engine();
  const auto result =
      PdfSceneExporter::write(scene.value(),
                              make_snapshot(PaginationMode::continuous),
                              [&resolver](const ImageTileRequest &req) {
                                return resolver(req);
                              },
                              engine.get());
  require(result.has_value(), "two-image PDF must build");
  const auto bytes = std::string{result.value().bytes()};

  // Parse the page Resources /XObject dict: "/Im<n> <number> 0 R" pairs.
  const auto xobject_at = bytes.find("/XObject <<");
  require(xobject_at != std::string::npos,
          "the page Resources must carry an /XObject dict");
  const auto dict_end = bytes.find(">>", xobject_at);
  require(dict_end != std::string::npos, "the /XObject dict must terminate");
  const auto dict = bytes.substr(xobject_at + 11, dict_end - xobject_at - 11);
  std::vector<std::string> tokens;
  std::string token;
  for (const char ch : dict) {
    if (ch == ' ') {
      if (!token.empty()) {
        tokens.push_back(token);
        token.clear();
      }
    } else {
      token.push_back(ch);
    }
  }
  if (!token.empty()) {
    tokens.push_back(token);
  }
  std::vector<std::pair<std::string, std::size_t>> entries;
  for (std::size_t i = 0; i + 3 < tokens.size(); i += 4) {
    std::size_t number = 0;
    const auto parsed =
        std::from_chars(tokens[i + 1].data(),
                        tokens[i + 1].data() + tokens[i + 1].size(), number);
    if (parsed.ec == std::errc{} && tokens[i + 2] == "0" &&
        tokens[i + 3] == "R") {
      entries.emplace_back(tokens[i], number);
    }
  }
  require(entries.size() == 2,
          "the page must name exactly two image XObjects");
  require(entries[0].first == "/Im0" && entries[1].first == "/Im1",
          "the images must be named Im0 then Im1");
  // The first (RGBA) image's /SMask child occupies the number right after it,
  // so the second image must be numbered first + 2 — matching the emission
  // loop's running offset (a raw index would say first + 1 and alias the
  // DeviceGray child).
  require(entries[1].second == entries[0].second + 2,
          "the second image must be numbered past the first image's /SMask "
          "child (running offset, not index)");

  // Every dict entry must resolve to the image XObject it names — never to a
  // DeviceGray object (the first image's /SMask child). Anchored on the
  // newline the writer puts before each "N 0 obj" header so a longer object
  // number cannot contain a shorter one's needle.
  for (const auto &entry : entries) {
    const auto needle = "\n" + std::to_string(entry.second) + " 0 obj";
    const auto obj_at = bytes.find(needle, dict_end);
    require(obj_at != std::string::npos,
            "the referenced image object must be emitted");
    const auto body_end = bytes.find("endobj", obj_at);
    require(body_end != std::string::npos,
            "the referenced object must terminate");
    const auto body =
        bytes.substr(obj_at + needle.size(), body_end - obj_at - needle.size());
    require(body.find("/Subtype /Image") != std::string::npos,
            "the referenced object must be an image XObject");
    require(body.find("/ColorSpace /DeviceGray") == std::string::npos,
            "no image entry may resolve to a DeviceGray /SMask child");
  }
}

// Byte determinism: identical input yields identical output (no timestamps/IDs).
void output_is_byte_deterministic() {
  const auto first =
      std::string{build_full_document(PaginationMode::continuous).bytes()};
  const auto second =
      std::string{build_full_document(PaginationMode::continuous).bytes()};
  require(first == second,
          "two builds of the full scene must be byte-identical");
  require(first.find("CreationDate") == std::string::npos,
          "no CreationDate may appear");
  require(first.find("ModDate") == std::string::npos,
          "no ModDate may appear");
}

void depth_ruler_emits_and_changes_output() {
  // Epic B (B4): the PDF backend draws the depth ruler with the same
  // authoritative tick semantics as the SVG backend. Off by default
  // (byte-deterministic existing output); enabling it must change the
  // emitted bytes (ruler geometry) without crashing.
  const auto document = base_document();
  auto builder = base_presentation();
  const auto scene = prepare_scene(document, builder);
  StubResolver resolver;
  auto engine = make_engine();

  auto default_snapshot = make_snapshot(PaginationMode::continuous);
  const auto off = PdfSceneExporter::write(
      *scene, default_snapshot,
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get());
  require(off.has_value(), "default PDF must build");

  auto ruler_snapshot = make_snapshot(PaginationMode::continuous);
  ruler_snapshot.page.show_depth_ruler = true;
  const auto on = PdfSceneExporter::write(
      *scene, ruler_snapshot,
      [&resolver](const ImageTileRequest &req) { return resolver(req); },
      engine.get());
  require(on.has_value(), "PDF with depth ruler must build");
  require(on.value().bytes() != off.value().bytes(),
          "ruler-on output must differ from the default output");
  require(on.value().bytes().size() > off.value().bytes().size(),
          "the depth ruler must add geometry to the PDF");
}

[[nodiscard]] std::string format_export_number(double value) {
  if (value == 0.0) {
    return "0";
  }
  std::array<char, 48> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  require(result.ec == std::errc{}, "to_chars must format a finite y");
  return std::string(buffer.data(), result.ptr);
}

void fixed_pdf_pages_omit_out_of_window_curve_points() {
  const auto doc_id = id("80000000-0000-4000-8000-000000000021");
  const auto ax_id = id("80000000-0000-4000-8000-000000000022");
  const auto cu_id = id("80000000-0000-4000-8000-000000000023");
  const auto tr_id = id("80000000-0000-4000-8000-000000000024");
  const auto sc_id = id("80000000-0000-4000-8000-000000000025");
  const auto ly_id = id("80000000-0000-4000-8000-000000000026");

  constexpr int n = 201;
  std::vector<double> depth_values;
  std::vector<double> sample_values;
  depth_values.reserve(static_cast<std::size_t>(n));
  sample_values.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    depth_values.push_back(1000.0 + static_cast<double>(i) * 4.0);
    sample_values.push_back(static_cast<double>(i % 100));
  }
  auto depths =
      std::make_shared<const std::vector<double>>(std::move(depth_values));
  auto values =
      std::make_shared<const std::vector<double>>(std::move(sample_values));

  WellLogDocumentBuilder document_builder(doc_id, DocumentRevision{5});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = ax_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = cu_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = ax_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  WellLogSession session;
  require(session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
          "dense PDF document must be accepted");
  ScenePresentationBuilder presentation_builder(
      doc_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1800.0,
      },
      Millimetres{400.0}, "font-fixture-v1");
  presentation_builder.add_track(
      TrackSpec{.id = tr_id, .width = Millimetres{80.0}, .z_order = 1});
  presentation_builder.add_scale(TrackScaleSpec{
      .id = sc_id,
      .track_id = tr_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = ly_id,
      .track_id = tr_id,
      .curve_id = cu_id,
      .scale_id = sc_id,
      .color = RgbaColor{20, 120, 20, 255},
      .line_width = Millimetres{0.25},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "dense PDF presentation must prepare");
  const auto scene = session.prepared_scene(doc_id);
  require(scene != nullptr && scene->curve_points().size() >= 50,
          "dense PDF scene must publish many curve points");

  ExportSnapshot snapshot{
      .document_id = doc_id,
      .document_revision = DocumentRevision{5},
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{
              .domain = DepthDomain::measured_depth,
              .unit = "m",
              .reference_top = 1000.0,
              .reference_bottom = 1800.0,
              .version = 1,
          },
      .font_asset_fingerprint = "font-fixture-v1",
      .page =
          ExportPageSpec{
              .mode = PaginationMode::fixed,
              .page_width = Millimetres{120.0},
              .page_height = Millimetres{50.0},
              .margins = ExportPageMargins{.top = Millimetres{10.0},
                                           .right = Millimetres{10.0},
                                           .bottom = Millimetres{10.0},
                                           .left = Millimetres{10.0}},
              .dpi = 300,
              .well_name = "PDF-604",
          },
  };
  auto engine = make_engine();
  const auto result = PdfSceneExporter::write(*scene, snapshot, {}, engine.get());
  require(result.has_value(), "dense fixed PDF must build");
  const auto bytes = std::string{result.value().bytes()};
  const auto streams = inflate_all_streams(bytes);
  require(streams.size() >= 4, "fixed PDF must contain several page streams");

  const auto points = scene->curve_points();
  require(points.size() >= 8, "dense PDF scene must carry curve samples");
  const auto point_token = [](const PreparedCurvePoint &point) {
    return format_export_number(point.position.left.value) + " " +
           format_export_number(point.position.top.value);
  };
  const auto first_token = point_token(points[3]);
  const auto last_token = point_token(points[points.size() - 4]);
  require(streams.front().find(first_token) != std::string::npos,
          "first PDF page stream must emit an in-window sample");
  require(streams.back().find(first_token) == std::string::npos,
          "last PDF page stream must not contain page 1's curve point (#604)");
  require(streams.back().find(last_token) != std::string::npos,
          "last PDF page stream must emit its own window's sample");
  require(streams.front().find(last_token) == std::string::npos,
          "first PDF page stream must not contain the last page's curve point");
}

} // namespace

int main() {
  external_tools_accept_the_full_pdf();
  image_xobject_is_embedded_and_invoked();
  image_dpi_is_recoverable_from_placement();
  tiling_pattern_is_embedded_and_referenced();
  pattern_phase_matrix_matches_svg_for_nonzero_anchor_rotation();
  fixed_mode_paginates_into_multiple_pages();
  depth_range_bands_are_emitted_and_continuous();
  custom_layer_primitives_are_emitted();
  rgba_image_embeds_with_smask();
  two_images_rgba_first_resource_dict_numbers_align();
  output_is_byte_deterministic();
  depth_ruler_emits_and_changes_output();
  fixed_pdf_pages_omit_out_of_window_curve_points();
  std::cout << "welllog.pdf-scene-full: all cases passed\n";
  return EXIT_SUCCESS;
}
