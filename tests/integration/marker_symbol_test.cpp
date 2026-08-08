// SDK marker symbols (Epic C 收尾 slice 1): MarkerSemantic → SymbolKind
// authoritative mapping, shared glyph geometry, PreparedMarker semantic
// propagation, and SVG/PDF/raster marker-symbol emission.

#include <welllog/export/pdf.hpp>
#include <welllog/export/pdf_scene.hpp>
#include <welllog/export/raster.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <zlib.h>

#include "png_decode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
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

struct Fixture {
  WellLogDocument document;
  PreparedScene scene;
  ExportSnapshot snapshot;
};

Fixture make_marker_fixture(bool draw_symbols) {
  const auto document_id = id("15800000-0000-4000-8000-000000000001");
  const auto axis_id = id("15800000-0000-4000-8000-000000000002");
  const auto curve_id = id("15800000-0000-4000-8000-000000000003");
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::vector<double>{0.0, 50.0, 100.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
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
  builder.add_marker(Marker{
      .id = id("15800000-0000-4000-8000-000000000010"),
      .reference_depth = 1010.0,
      .semantic = MarkerSemantic::formation_top,
      .label = "Top",
  });
  builder.add_marker(Marker{
      .id = id("15800000-0000-4000-8000-000000000011"),
      .reference_depth = 1020.0,
      .semantic = MarkerSemantic::fault,
      .label = "Fault",
  });
  builder.add_marker(Marker{
      .id = id("15800000-0000-4000-8000-000000000012"),
      .reference_depth = 1030.0,
      .semantic = MarkerSemantic::fluid_contact,
      .label = "Contact",
  });
  builder.add_marker(Marker{
      .id = id("15800000-0000-4000-8000-000000000013"),
      .reference_depth = 1040.0,
      .semantic = MarkerSemantic::casing_shoe,
      .label = "Shoe",
  });
  builder.add_marker(Marker{
      .id = id("15800000-0000-4000-8000-000000000014"),
      .reference_depth = 1050.0,
      .semantic = MarkerSemantic::custom,
      .label = "Custom",
  });
  auto document = builder.build();

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "fixture document must load");

  const auto track_id = id("15800000-0000-4000-8000-000000000020");
  const auto scale_id = id("15800000-0000-4000-8000-000000000021");
  const auto layer_id = id("15800000-0000-4000-8000-000000000022");
  const auto marker_layer_id = id("15800000-0000-4000-8000-000000000023");
  ScenePresentationBuilder presentation(
      document.id(),
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1100.0},
      Millimetres{120.0}, "font-fixture-v1");
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
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 0, .green = 100, .blue = 200, .alpha = 255},
      .line_width = Millimetres{0.4},
      .z_order = 1,
      .visible = true,
  });
  presentation.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 2,
      .line_color = RgbaColor{.red = 200, .green = 30, .blue = 30, .alpha = 255},
      .line_width = Millimetres{0.3},
      .draw_labels = true,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{.red = 0, .green = 0, .blue = 0, .alpha = 255},
      .draw_symbols = draw_symbols,
      .symbol_size = Millimetres{3.0},
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "fixture presentation must load");
  const auto scene = session.prepared_scene(document.id());
  require(scene != nullptr && !scene->markers().empty(),
          "fixture must prepare markers");

  ExportSnapshot snapshot{
      .document_id = document.id(),
      .document_revision = document.revision(),
      .presentation_version = PresentationVersion{1},
      .depth_transform =
          DepthTransformDescriptor{.domain = DepthDomain::measured_depth,
                                   .unit = "m",
                                   .reference_top = 1000.0,
                                   .reference_bottom = 1100.0,
                                   .version = 1},
      .font_asset_fingerprint = "font-fixture-v1",
      .page =
          ExportPageSpec{
              .mode = PaginationMode::continuous,
              .page_width = Millimetres{80.0},
              .page_height = Millimetres{120.0},
              .dpi = 100,
              .well_name = "Marker-Symbol-Fixture",
          },
  };
  return Fixture{.document = std::move(document),
                 .scene = *scene,
                 .snapshot = std::move(snapshot)};
}

void mapping_is_authoritative() {
  require(symbol_for_marker_semantic(MarkerSemantic::formation_top) ==
              SymbolKind::triangle_down,
          "formation_top maps to triangle_down");
  require(symbol_for_marker_semantic(MarkerSemantic::fault) ==
              SymbolKind::cross,
          "fault maps to cross");
  require(symbol_for_marker_semantic(MarkerSemantic::fluid_contact) ==
              SymbolKind::diamond,
          "fluid_contact maps to diamond");
  require(symbol_for_marker_semantic(MarkerSemantic::casing_shoe) ==
              SymbolKind::shoe,
          "casing_shoe maps to the shoe glyph");
  require(symbol_for_marker_semantic(MarkerSemantic::custom) ==
              SymbolKind::circle,
          "custom maps to circle");
}

void glyph_geometry_is_bounded_and_closed() {
  const auto glyph = symbol_glyph(SymbolKind::shoe, Millimetres{3.0});
  require(glyph.kind == SymbolKind::shoe, "glyph kind is preserved");
  require(glyph.outline.size() >= 17, "shoe arch has a sampled outline");
  double max_abs_x = 0.0;
  double max_abs_y = 0.0;
  for (const auto &p : glyph.outline) {
    max_abs_x = std::max(max_abs_x, std::abs(p.left.value));
    max_abs_y = std::max(max_abs_y, std::abs(p.top.value));
  }
  require(max_abs_x <= 1.5 + 1e-9 && max_abs_y <= 1.5 + 1e-9,
          "glyph outline stays within half the size");
  require(glyph.outline.front().top.value >= -1e-9 &&
              glyph.outline.back().top.value >= -1e-9,
          "shoe arch bulges downward (flat side up)");

  const auto square = symbol_glyph(SymbolKind::square, Millimetres{4.0});
  require(square.outline.size() == 4, "square has 4 corners");
  const auto cross = symbol_glyph(SymbolKind::cross, Millimetres{3.0});
  require(cross.outline.empty() && cross.stroke_width > 0.0,
          "cross is stroke-only with a positive stroke width");
}

void prepared_markers_carry_semantic_and_layer_flags() {
  auto fixture = make_marker_fixture(true);
  require(fixture.scene.marker_layers().size() == 1, "one marker layer");
  const auto &layer = fixture.scene.marker_layers().front();
  require(layer.draw_symbols, "draw_symbols reaches the prepared layer");
  require(layer.symbol_size.value == 3.0, "symbol size reaches the prepared layer");
  require(layer.marker_count == 5, "all five markers are prepared");
  // display_top: 1040 → (1040-1000)/100 * 120 mm = 48 mm (identity transform).
  bool saw_shoe = false;
  for (const auto &marker : fixture.scene.markers()) {
    if (marker.marker_id ==
        id("15800000-0000-4000-8000-000000000013")) {
      require(marker.semantic == MarkerSemantic::casing_shoe,
              "casing_shoe semantic reaches the prepared marker");
      require(std::abs(marker.display_top.value - 48.0) < 1e-6,
              "marker display position is projected");
      saw_shoe = true;
    }
  }
  require(saw_shoe, "casing_shoe marker found");
}

void svg_emits_marker_symbol_paths_only_when_requested() {
  auto with_symbols = make_marker_fixture(true);
  auto svg = SvgExporter::write(with_symbols.scene);
  require(svg.has_value(), "SVG export must succeed");
  const auto &text = svg.value().text();
  require(text.find("marker-symbol-15800000-0000-4000-8000-000000000013") !=
              std::string::npos,
          "SVG contains the marker symbol path for the casing shoe");
  require(text.find("data-semantic=\"casing_shoe\"") != std::string::npos,
          "SVG records the marker semantic");
  require(text.find("data-semantic=\"formation_top\"") != std::string::npos,
          "SVG records formation_top semantic");
  require(text.find("data-semantic=\"fault\"") != std::string::npos,
          "SVG records fault semantic");

  auto plain = make_marker_fixture(false);
  auto svg_plain = SvgExporter::write(plain.scene);
  require(svg_plain.has_value(), "plain SVG export must succeed");
  require(svg_plain.value().text().find("marker-symbol-") == std::string::npos,
          "draw_symbols=false emits no marker symbol paths (compat default)");
  require(svg_plain.value().text().find("marker-15800000-0000-4000-8000-000000000013") !=
              std::string::npos,
          "marker lines still render without symbols");
}

std::string inflate_content_stream(std::string_view bytes) {
  const auto filter_pos = bytes.find("/Filter /FlateDecode");
  require(filter_pos != std::string_view::npos,
          "PDF must contain a FlateDecode stream");
  const auto stream_start = bytes.find("stream", filter_pos);
  const auto data_start = bytes.find('\n', stream_start) + 1;
  const auto end_marker = bytes.find("endstream", data_start);
  require(end_marker != std::string_view::npos, "stream must end");
  const auto compressed =
      std::string(bytes.substr(data_start, end_marker - data_start));
  z_stream stream{};
  require(inflateInit(&stream) == Z_OK, "inflateInit must succeed");
  std::vector<unsigned char> out(64U * 1024U);
  std::string result;
  stream.next_in = reinterpret_cast<Bytef *>(
      const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  int rc = Z_OK;
  do {
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    rc = inflate(&stream, Z_NO_FLUSH);
    require(rc == Z_OK || rc == Z_STREAM_END, "stream must inflate cleanly");
    result.append(reinterpret_cast<const char *>(out.data()),
                  out.size() - stream.avail_out);
  } while (rc != Z_STREAM_END);
  inflateEnd(&stream);
  return result;
}

void pdf_emits_marker_symbol_paths_only_when_requested() {
  auto with_symbols = make_marker_fixture(true);
  auto pdf = PdfSceneExporter::write(with_symbols.scene, with_symbols.snapshot);
  require(pdf.has_value(), "PDF export must succeed");
  const auto stream_text = inflate_content_stream(pdf.value().bytes());
  // Marker line at 48 mm (casing_shoe). The shoe arch path starts with
  // move_to(cx - half, cy) = (1, 48): track left 0 + 1 mm + half 1.5 − half.
  require(stream_text.find("1 48 m\n") != std::string::npos,
          "PDF emits the shoe arch path at the casing_shoe marker");
  // A cubic + close + fill follow the arch move.
  require(stream_text.find(" c\n") != std::string::npos ||
              stream_text.find("c\n") != std::string::npos,
          "PDF arch uses cubic segments");

  auto plain = make_marker_fixture(false);
  auto pdf_plain =
      PdfSceneExporter::write(plain.scene, plain.snapshot);
  require(pdf_plain.has_value(), "plain PDF export must succeed");
  const auto plain_text = inflate_content_stream(pdf_plain.value().bytes());
  require(plain_text.find("1 48 m\n") == std::string::npos,
          "draw_symbols=false emits no marker symbol path");
  require(plain_text.find("48") != std::string::npos,
          "marker line still renders without symbols");
}

std::filesystem::path temp_file(std::string_view name) {
  return std::filesystem::temp_directory_path() / name;
}

void raster_draws_marker_symbol_pixels() {
  auto fixture = make_marker_fixture(true);
  const auto path = temp_file("welllog-158-marker-symbol.png");
  std::filesystem::remove(path);
  RasterExportRequest req{
      .path = path,
      .format = RasterImageFormat::png,
      .width_px = 160,
      .height_px = 240,
      .background = RgbaColor{255, 255, 255, 255},
      .color_space = RasterColorSpace::srgb,
      .tile_height_px = 16,
  };
  const auto report = export_raster_sync(fixture.scene, fixture.snapshot, req);
  require(report.has_value(), "raster export must succeed");

  const auto png = welllog::test::decode_png(path);
  require(png.has_value(), "PNG must decode");
  require(png->color_type == 6, "srgb export is RGBA");
  const auto channels = 4U;
  const auto stride = static_cast<std::size_t>(png->width) * channels;
  const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
    const auto at = stride * y + static_cast<std::size_t>(x) * channels;
    return std::array<std::uint8_t, 4>{png->samples[at], png->samples[at + 1],
                                       png->samples[at + 2],
                                       png->samples[at + 3]};
  };
  // 1040 m → 48 mm → round(48 * 100 / 25.4) px ≈ 189 (marker line row).
  // The shoe arch bulges ~6 px BELOW the line (y in 192..198); the marker
  // line itself sits at y≈189, so any non-background pixel below the line in
  // the symbol column band must come from the arch glyph.
  const auto line_color =
      std::array<std::uint8_t, 4>{200, 30, 30, 255};
  bool arch_drawn = false;
  for (std::uint32_t y = 192; y < 199 && y < png->height; ++y) {
    for (std::uint32_t x = 4; x < 17 && x < png->width; ++x) {
      if (pixel(x, y) == line_color) {
        arch_drawn = true;
      }
    }
  }
  require(arch_drawn, "raster must draw the shoe arch below the marker line");

  // Control: without draw_symbols the same region is pure background.
  auto plain_fixture = make_marker_fixture(false);
  const auto plain_path = temp_file("welllog-158-marker-plain.png");
  std::filesystem::remove(plain_path);
  RasterExportRequest plain_req{
      .path = plain_path,
      .format = RasterImageFormat::png,
      .width_px = 160,
      .height_px = 240,
      .background = RgbaColor{255, 255, 255, 255},
      .color_space = RasterColorSpace::srgb,
      .tile_height_px = 16,
  };
  require(export_raster_sync(plain_fixture.scene, plain_fixture.snapshot,
                             plain_req)
              .has_value(),
          "plain raster export must succeed");
  const auto plain = welllog::test::decode_png(plain_path);
  require(plain.has_value(), "plain PNG must decode");
  const auto plain_stride = static_cast<std::size_t>(plain->width) * channels;
  const auto plain_pixel = [&](std::uint32_t x, std::uint32_t y) {
    const auto at = plain_stride * y + static_cast<std::size_t>(x) * channels;
    return std::array<std::uint8_t, 4>{
        plain->samples[at], plain->samples[at + 1], plain->samples[at + 2],
        plain->samples[at + 3]};
  };
  bool plain_arch = false;
  for (std::uint32_t y = 192; y < 199 && y < plain->height; ++y) {
    for (std::uint32_t x = 4; x < 17 && x < plain->width; ++x) {
      if (plain_pixel(x, y) == line_color) {
        plain_arch = true;
      }
    }
  }
  require(!plain_arch, "draw_symbols=false draws no arch pixels");
  std::filesystem::remove(path);
  std::filesystem::remove(plain_path);
}

} // namespace

int main() {
  mapping_is_authoritative();
  glyph_geometry_is_bounded_and_closed();
  prepared_markers_carry_semantic_and_layer_flags();
  svg_emits_marker_symbol_paths_only_when_requested();
  pdf_emits_marker_symbol_paths_only_when_requested();
  raster_draws_marker_symbol_pixels();
  return EXIT_SUCCESS;
}
