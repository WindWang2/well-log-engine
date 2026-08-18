// CGM Version 3 Binary subset spike (B1.CGM.1 / ADR 0054).
// Proves the hand-rolled writer emits delimiter + POLYLINE commands and that
// CgmSceneExporter maps a single-well prepared scene without crashing.

#include <welllog/export/cgm.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

void low_level_writer_emits_delimiters_and_polyline() {
  CgmBinaryWriter w;
  w.begin_metafile("spike");
  w.metafile_version(3);
  w.metafile_description("B1.CGM.1 spike");
  w.vdc_type_integer();
  w.integer_precision(16);
  w.colour_precision(8);
  w.colour_value_extent();
  w.metafile_element_list_drawing_plus();
  w.begin_picture("p1");
  w.colour_selection_mode_direct();
  w.vdc_extent(0, 0, 1000, 1000);
  w.background_colour(255, 255, 255);
  w.begin_picture_body();
  w.line_colour(0, 0, 0);
  w.line_width(10);
  const std::array<std::pair<std::int16_t, std::int16_t>, 3> pts{{
      {0, 0},
      {500, 500},
      {1000, 0},
  }};
  w.polyline(pts);
  w.text_colour(0, 0, 0);
  w.character_height(100);
  w.text(10, 900, "GR 1000");
  w.end_picture();
  w.end_metafile();

  const auto doc = w.finish();
  require(doc.has_value(), "finish() must succeed");
  const auto bytes = doc.value().bytes();
  require(bytes.size() > 32, "CGM must be non-empty");
  require(cgm_has_metafile_delimiters(bytes),
          "BEGIN/END METAFILE must be present");
  require(cgm_count_polylines(bytes) >= 1,
          "at least one POLYLINE command expected");

  // Determinism: same input → same bytes.
  CgmBinaryWriter w2;
  w2.begin_metafile("spike");
  w2.metafile_version(3);
  w2.metafile_description("B1.CGM.1 spike");
  w2.vdc_type_integer();
  w2.integer_precision(16);
  w2.colour_precision(8);
  w2.colour_value_extent();
  w2.metafile_element_list_drawing_plus();
  w2.begin_picture("p1");
  w2.colour_selection_mode_direct();
  w2.vdc_extent(0, 0, 1000, 1000);
  w2.background_colour(255, 255, 255);
  w2.begin_picture_body();
  w2.line_colour(0, 0, 0);
  w2.line_width(10);
  w2.polyline(pts);
  w2.text_colour(0, 0, 0);
  w2.character_height(100);
  w2.text(10, 900, "GR 1000");
  w2.end_picture();
  w2.end_metafile();
  const auto doc2 = w2.finish();
  require(doc2.has_value(), "second finish() must succeed");
  require(std::string{doc2.value().bytes()} == std::string{bytes},
          "CGM writer must be byte-deterministic");
}

void scene_exporter_emits_curve_polylines() {
  const auto document_id = id("20000000-0000-4000-8000-000000000001");
  const auto axis_id = id("20000000-0000-4000-8000-000000000002");
  const auto curve_id = id("20000000-0000-4000-8000-000000000003");
  const auto track_id = id("20000000-0000-4000-8000-000000000004");
  const auto scale_id = id("20000000-0000-4000-8000-000000000005");
  const auto layer_id = id("20000000-0000-4000-8000-000000000006");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0, 50.0});
  auto nulls = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{5});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = NullBitmapView::from_raw(nulls->data(), values->size(),
                                        nulls->size(), SharedOwner{nulls}),
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "document must be accepted");

  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1004.0,
      },
      Millimetres{80.0}, "font-fixture-v1");
  presentation_builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{30.0},
      .z_order = 10,
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 80.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{.red = 0x12, .green = 0x34, .blue = 0x56, .alpha = 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 20,
      .visible = true,
  });

  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "presentation must prepare a scene");

  const auto cgm = CgmSceneExporter::write(*scene);
  require(cgm.has_value(), "CgmSceneExporter::write must succeed");
  const auto bytes = cgm.value().bytes();
  require(cgm_has_metafile_delimiters(bytes), "scene CGM needs delimiters");
  // Track frame polyline + at least one curve segment.
  require(cgm_count_polylines(bytes) >= 2,
          "scene CGM must contain track frame + curve polylines");
  require(bytes.size() > 64, "scene CGM must be non-trivial");
}

void diagnostics_report_pattern_flattening() {
  CgmExportDiagnostics diag;
  CgmBinaryWriter w;
  w.begin_metafile("d");
  w.metafile_version(3);
  w.vdc_type_integer();
  w.integer_precision(16);
  w.colour_precision(8);
  w.colour_value_extent();
  w.metafile_element_list_drawing_plus();
  w.begin_picture("p");
  w.colour_selection_mode_direct();
  w.vdc_extent(0, 0, 100, 100);
  w.begin_picture_body();
  w.fill_colour(200, 100, 50);
  w.rectangle_fill(10, 10, 40, 40);
  w.end_picture();
  w.end_metafile();
  const auto doc = w.finish();
  require(doc.has_value(), "polygon fill metafile must build");
  require(cgm_count_polygons(doc.value().bytes()) >= 1,
          "rectangle_fill must emit POLYGON");
  diag.patterns_flattened_to_solid = 2;
  diag.patterns_hatch_approximated = 2;
  diag.notes.push_back("pattern fills: solid + diagonal hatch approx (B1.CGM.3)");
  const auto sum = diag.summary();
  require(sum.find("patterns_hatch_approximated=2") != std::string::npos,
          "diagnostics summary must list hatch approx count");
}

void multi_picture_pagination() {
  // Reuse the working single-well fixture from scene_exporter_emits_curve_polylines
  // by building the same document, then paginate with a small page_height_mm.
  const auto document_id = id("20000000-0000-4000-8000-000000000001");
  const auto axis_id = id("20000000-0000-4000-8000-000000000002");
  const auto curve_id = id("20000000-0000-4000-8000-000000000003");
  const auto track_id = id("20000000-0000-4000-8000-000000000004");
  const auto scale_id = id("20000000-0000-4000-8000-000000000005");
  const auto layer_id = id("20000000-0000-4000-8000-000000000006");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0, 1004.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0, 50.0});
  auto nulls = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{5});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = NullBitmapView::from_raw(nulls->data(), values->size(),
                                        nulls->size(), SharedOwner{nulls}),
  });
  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "document must be accepted");
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1004.0,
      },
      Millimetres{80.0}, "font-fixture-v1");
  presentation_builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{30.0},
      .z_order = 10,
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 80.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{.red = 0x12, .green = 0x34, .blue = 0x56, .alpha = 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 20,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "prepared scene required");
  // Force at least 2 pictures: page height = 40% of scene height.
  const auto page_h = scene->physical_height().value * 0.4;
  CgmExportDiagnostics diag;
  CgmExportOptions opt{.page_height_mm = page_h, .page_overlap = 0.0};
  const auto cgm = CgmSceneExporter::write(*scene, opt, &diag);
  require(cgm.has_value(), "paginated CGM must succeed");
  const auto bytes = cgm.value().bytes();
  require(cgm_count_pictures(bytes) >= 2, "fixed pages need >= 2 PICTURE");
  require(diag.pictures_emitted >= 2, "diagnostics pictures_emitted");
  require(cgm_has_metafile_delimiters(bytes), "delimiters");
}

[[nodiscard]] std::int16_t read_i16_be(std::string_view bytes,
                                       std::size_t offset) {
  require(offset + 2 <= bytes.size(), "CGM i16 read stays in range");
  const auto hi = static_cast<std::uint8_t>(bytes[offset]);
  const auto lo = static_cast<std::uint8_t>(bytes[offset + 1]);
  return static_cast<std::int16_t>(
      static_cast<std::uint16_t>((hi << 8) | lo));
}

// Decode one command header the same way the writer encodes it (short or
// long-form). Returns false at the end of the stream.
[[nodiscard]] bool next_cgm_command(std::string_view bytes, std::size_t &offset,
                                    std::uint8_t &cls, std::uint8_t &id,
                                    std::size_t &param_start,
                                    std::size_t &param_len) {
  if (offset + 2 > bytes.size()) {
    return false;
  }
  const auto header = static_cast<std::uint16_t>(
      (static_cast<std::uint8_t>(bytes[offset]) << 8) |
      static_cast<std::uint8_t>(bytes[offset + 1]));
  cls = static_cast<std::uint8_t>((header >> 12) & 0x0F);
  id = static_cast<std::uint8_t>((header >> 5) & 0x7F);
  auto plen = static_cast<std::size_t>(header & 0x1FU);
  auto pstart = offset + 2;
  if (plen == 31) {
    if (offset + 4 > bytes.size()) {
      return false;
    }
    const auto l0 = static_cast<std::uint8_t>(bytes[offset + 2]);
    const auto l1 = static_cast<std::uint8_t>(bytes[offset + 3]);
    plen = static_cast<std::size_t>(((l0 & 0x7FU) << 8) | l1);
    pstart = offset + 4;
  }
  if (pstart + plen > bytes.size()) {
    return false;
  }
  param_start = pstart;
  param_len = plen;
  auto end = pstart + plen;
  if ((end & 1U) != 0U) {
    ++end;
  }
  offset = end;
  return true;
}

// Issue #603: a continuous picture taller than 327.67 mm used to clamp every
// VDC y at ±32767, flattening the top ~672 mm of the page onto one line.
// Per-picture scaling must keep distinct scene depths as distinct VDC y.
void tall_continuous_picture_does_not_clamp_vdc() {
  const auto document_id = id("20000000-0000-4000-8000-000000000011");
  const auto axis_id = id("20000000-0000-4000-8000-000000000012");
  const auto curve_id = id("20000000-0000-4000-8000-000000000013");
  const auto track_id = id("20000000-0000-4000-8000-000000000014");
  const auto scale_id = id("20000000-0000-4000-8000-000000000015");
  const auto layer_id = id("20000000-0000-4000-8000-000000000016");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1040.0, 1080.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 40.0, 70.0});
  auto nulls = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{1});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = NullBitmapView::from_raw(nulls->data(), values->size(),
                                        nulls->size(), SharedOwner{nulls}),
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "tall-scene document must be accepted");

  constexpr double height_mm = 800.0;
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1080.0,
      },
      Millimetres{height_mm}, "font-fixture-v1");
  presentation_builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{30.0},
      .z_order = 10,
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 80.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{.red = 0x12, .green = 0x34, .blue = 0x56, .alpha = 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 20,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "tall-scene presentation must be accepted");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "tall scene must prepare");
  require(scene->physical_height().value > 327.67,
          "fixture must exceed the unscaled int16 VDC window");

  CgmExportDiagnostics diag;
  const auto cgm = CgmSceneExporter::write(*scene, CgmExportOptions{}, &diag);
  require(cgm.has_value(), "tall continuous CGM must succeed");
  const auto bytes = cgm.value().bytes();
  require(cgm_count_pictures(bytes) == 1, "continuous export is one PICTURE");

  bool saw_extent = false;
  std::int16_t extent_y1 = 0;
  std::vector<std::pair<std::int16_t, std::int16_t>> curve_pts;
  std::size_t offset = 0;
  std::uint8_t cls = 0;
  std::uint8_t id = 0;
  std::size_t pstart = 0;
  std::size_t plen = 0;
  while (next_cgm_command(bytes, offset, cls, id, pstart, plen)) {
    if (cls == 2 && id == 6 && plen >= 8) {
      extent_y1 = read_i16_be(bytes, pstart + 6);
      saw_extent = true;
    }
    if (cls == 4 && id == 1 && plen == 12) {
      curve_pts.clear();
      for (std::size_t i = 0; i < 3; ++i) {
        curve_pts.emplace_back(read_i16_be(bytes, pstart + i * 4),
                               read_i16_be(bytes, pstart + i * 4 + 2));
      }
    }
  }
  require(saw_extent, "VDC EXTENT must be present");
  require(extent_y1 > 0, "scaled VDC height must be a positive int16 extent");
  require(curve_pts.size() == 3, "the 3-sample curve must emit 3 VDC points");
  // Unscaled 100 VDC/mm maps 0 mm and 400 mm both onto 32767. Distinct
  // depths on an 800 mm page must remain distinct after scaling.
  require(curve_pts[0].second != curve_pts[1].second,
          "top and mid depths must not collapse onto the int16 VDC edge");
  require(curve_pts[1].second != curve_pts[2].second,
          "mid and bottom depths must stay distinct in VDC");
  require(curve_pts[0].second > curve_pts[1].second &&
              curve_pts[1].second > curve_pts[2].second,
          "y-up VDC must decrease down the page");
}

void vdc_geometry_within_half_mm() {
  // ADR 0054: CGM entry golden 0.5 mm → 50 VDC units.
  const auto p = cgm_scene_to_vdc(/*x*/ 16.0, /*y*/ 0.0, /*wtop*/ 0.0,
                                  /*wheight*/ 210.0);
  require(p.first == 1600, "16 mm → 1600 VDC");
  // y=0 (top of window) → vdc_y = height * 100
  require(p.second == 21000, "top of window maps to VDC height");
  const auto p2 = cgm_scene_to_vdc(16.0, 210.0, 0.0, 210.0);
  require(p2.second == 0, "bottom of window maps to VDC 0");
  // 0.5 mm tolerance = 50 VDC
  const auto expected = 1600;
  const auto actual = static_cast<int>(std::lround(16.0 * k_cgm_vdc_per_mm));
  require(std::abs(actual - expected) <= 50, "0.5 mm VDC entry golden");
}

// Issue #468: a polyline longer than the 15-bit long-form parameter length
// (8191 points at 4 bytes) must chunk into multiple POLYLINE commands that
// keep coordinate pairs intact and repeat the previous chunk's last point —
// the old writer clamped the parameter list to 0x7FFF bytes and silently
// dropped the tail (potentially between a point's x and y).
void oversize_polyline_chunks_without_truncation() {
  CgmBinaryWriter w;
  w.begin_metafile("spike");
  w.metafile_version(3);
  w.metafile_description("B1.CGM.1 spike");
  w.vdc_type_integer();
  w.integer_precision(16);
  w.colour_precision(8);
  w.colour_value_extent();
  w.metafile_element_list_drawing_plus();
  w.begin_picture("p1");
  w.colour_selection_mode_direct();
  w.vdc_extent(0, 0, 32767, 32767);
  w.background_colour(255, 255, 255);
  w.begin_picture_body();
  w.line_colour(0, 0, 0);
  w.line_width(1);
  // 20000 points = 3 chunks (8191 + 8191 + 3618) + 2 repeated join points.
  std::vector<std::pair<std::int16_t, std::int16_t>> pts;
  pts.reserve(20000);
  for (std::size_t i = 0; i < 20000; ++i) {
    pts.emplace_back(static_cast<std::int16_t>(i % 32000),
                     static_cast<std::int16_t>((i * 7) % 32000));
  }
  w.polyline(pts);
  w.end_picture();
  w.end_metafile();
  const auto doc = w.finish();
  require(doc.has_value(), "finish() must succeed");
  const auto bytes = doc.value().bytes();
  require(cgm_count_polylines(bytes) == 3,
          "a 20000-point polyline must chunk into exactly three commands");
  require(cgm_polyline_total_points(bytes) == 20000 + 2,
          "all points must survive chunking (plus 2 repeated join points)");
}

// #854-2: polygons must chunk like polylines, not silently truncate mid-ring.
void oversize_polygon_chunks_without_truncation() {
  CgmBinaryWriter w;
  w.begin_metafile("spike");
  w.metafile_version(3);
  w.metafile_description("B1.CGM.1 spike");
  w.vdc_type_integer();
  w.integer_precision(16);
  w.colour_precision(8);
  w.colour_value_extent();
  w.metafile_element_list_drawing_plus();
  w.begin_picture("p1");
  w.colour_selection_mode_direct();
  w.vdc_extent(0, 0, 32767, 32767);
  w.background_colour(255, 255, 255);
  w.begin_picture_body();
  w.fill_colour(200, 100, 50);
  // 20000 points = multiple POLYGON commands with repeated join points.
  std::vector<std::pair<std::int16_t, std::int16_t>> pts;
  pts.reserve(20000);
  for (std::size_t i = 0; i < 20000; ++i) {
    pts.emplace_back(static_cast<std::int16_t>(i % 32000),
                     static_cast<std::int16_t>((i * 7) % 32000));
  }
  w.polygon(pts);
  w.end_picture();
  w.end_metafile();
  const auto doc = w.finish();
  require(doc.has_value(), "finish() must succeed");
  const auto bytes = doc.value().bytes();
  require(cgm_count_polygons(bytes) >= 2,
          "a 20000-point polygon must chunk into multiple commands");
}

} // namespace

int main() {
  low_level_writer_emits_delimiters_and_polyline();
  oversize_polyline_chunks_without_truncation();
  oversize_polygon_chunks_without_truncation();
  scene_exporter_emits_curve_polylines();
  diagnostics_report_pattern_flattening();
  multi_picture_pagination();
  vdc_geometry_within_half_mm();
  tall_continuous_picture_does_not_clamp_vdc();
  std::cout << "welllog.cgm-spike: all cases passed\n";
  return 0;
}
