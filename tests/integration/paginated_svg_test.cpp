// Black-box test for the paginated, physical-scale SVG exporter (#186,
// ADR 0048). Asserts: continuous mode emits one correctly-sized page with a
// depth-range footer; fixed mode paginates a tall scene into >=2 pages each
// carrying repeating header/footer roles, continuous depth ranges, page numbers
// 1..N, and correct physical dimensions; the envelope-density prepare flows
// through; snapshot metadata is captured; invalid input is rejected. The
// existing single-scene SvgExporter::write is asserted unchanged (additivity).

#include <welllog/export/pagination.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
#include <cmath>
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

// Builds a tall single-curve-track scene (physical_height 400 mm) so a small
// fixed page necessarily paginates into several pages. Returns the prepared
// scene; the presentation carries the new export-metadata version tags.
std::shared_ptr<const PreparedScene> prepare_tall_scene() {
  const auto document_id = id("60000000-0000-4000-8000-000000000001");
  const auto axis_id = id("60000000-0000-4000-8000-000000000002");
  const auto curve_id = id("60000000-0000-4000-8000-000000000003");
  const auto track_id = id("60000000-0000-4000-8000-000000000004");
  const auto scale_id = id("60000000-0000-4000-8000-000000000005");
  const auto layer_id = id("60000000-0000-4000-8000-000000000006");

  // 9 evenly-spaced samples -> a sparse curve; pagination exercises the
  // clip-windowed body emit, not raw-point density.
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1100.0, 1200.0, 1300.0, 1400.0,
                                    1500.0, 1600.0, 1700.0, 1800.0});
  auto values =
      std::make_shared<const std::vector<double>>(std::initializer_list<double>{
          0.0, 12.5, 25.0, 37.5, 50.0, 62.5, 75.0, 87.5, 100.0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{7});
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
      .nulls = {},
  });

  WellLogSession session;
  require(
      session.execute(SetDocumentCommand{document_builder.build()}).has_value(),
      "tall-scene document must be accepted");

  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1800.0,
      },
      Millimetres{400.0}, "font-fixture-v1");
  // Set the new export-metadata version tags (criterion 1).
  presentation_builder.set_presentation_version(PresentationVersion{42});
  presentation_builder.set_depth_transform_version(3);
  presentation_builder.add_track(TrackSpec{
      .id = track_id,
      .width = Millimetres{80.0},
      .z_order = 10,
      .header = TrackHeaderSpec{.height = Millimetres{6.0},
                                .font_size = Millimetres{2.5}},
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{.red = 0x11, .green = 0x22, .blue = 0x33, .alpha = 0xff},
      .line_width = Millimetres{0.25},
      .z_order = 20,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation_builder.build()})
              .has_value(),
          "tall presentation must prepare a scene");

  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "tall presentation must publish a scene");
  require_near(scene->physical_height().value, 400.0,
               "tall scene must keep its physical height");
  require(scene->presentation_version().value == 42,
          "prepared scene must retain presentation version");
  require(scene->depth_transform().version == 3,
          "prepared scene must retain depth-transform version");
  require(scene->depth_transform().reference_top == 1000.0 &&
              scene->depth_transform().reference_bottom == 1800.0,
          "depth-transform descriptor must carry the reference window");
  return scene;
}

ExportSnapshot make_snapshot(PaginationMode mode, Millimetres page_height) {
  return ExportSnapshot{
      .document_id = id("60000000-0000-4000-8000-000000000001"),
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

void continuous_mode_emits_one_correctly_sized_page() {
  const auto scene = prepare_tall_scene();
  auto snapshot = make_snapshot(PaginationMode::continuous, Millimetres{297.0});
  const auto result = PaginatedSvgExporter::write(*scene, snapshot);
  require(result.has_value(), "continuous export must succeed");

  const auto text = std::string{result.value().text()};
  require(count_occurrences(text, "<svg ") == 1,
          "continuous mode must emit exactly one svg document");
  require(text.find("data-export-page-count=\"1\"") != std::string::npos,
          "continuous mode must report a single page");
  // Width must equal the page width (physical mm), not window pixels.
  require(text.find("width=\"120mm\"") != std::string::npos,
          "continuous page width must be the physical page width");
  // Depth-range footer present with the scene's full depth window.
  require(text.find("data-export-role=\"footer\"") != std::string::npos,
          "continuous page must carry a depth-range footer");
  require(text.find("data-page-depth-top=\"1000\"") != std::string::npos &&
              text.find("data-page-depth-bottom=\"1800\"") != std::string::npos,
          "continuous footer must report the full depth window");
  // Well-name header present.
  require(text.find("data-export-role=\"header\"") != std::string::npos,
          "continuous page must carry a header");
  require(text.find("Well-A") != std::string::npos,
          "continuous header must carry the well name");
}

void fixed_mode_paginates_with_repeating_bands_and_continuous_depths() {
  const auto scene = prepare_tall_scene();
  // Page height 50 mm with 10 mm top + 10 mm bottom margins -> 30 mm printable
  // page height. The scene maps 80 mm wide onto a 100 mm printable width (scale
  // 1.25), so the printable depth height is 30/1.25 = 24 mm. Scene is 400 mm
  // tall -> ceil(400/24) = 17 pages.
  auto snapshot = make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  const auto result = PaginatedSvgExporter::write(*scene, snapshot);
  require(result.has_value(), "fixed export must succeed");

  const auto text = std::string{result.value().text()};
  const auto svg_count = count_occurrences(text, "<svg ");
  require(svg_count >= 2, "fixed mode over a tall scene must produce >=2 pages");
  require(text.find("data-export-page-count=\"17\"") != std::string::npos,
          "fixed mode must report the expected page count");

  // Every page carries header + footer roles and the page-number label. Each
  // page emits two header-role texts (well name + page number) and one footer.
  const auto header_count = count_occurrences(text, "data-export-role=\"header\"");
  require(header_count == 2 * svg_count,
          "every page must carry a well-name and page-number header band");
  const auto footer_count = count_occurrences(text, "data-export-role=\"footer\"");
  require(footer_count == svg_count,
          "every page must carry a footer band");

  // Page numbers 1..N all present ("page K of N").
  for (std::size_t page = 1; page <= static_cast<std::size_t>(svg_count);
       ++page) {
    const auto label = "page " + std::to_string(page) + " of " +
                       std::to_string(svg_count);
    require(text.find(label) != std::string::npos,
            "every page number must appear");
  }

  // Depth ranges are continuous: collect each page's depth window and assert
  // page k top equals page k-1 bottom (page_overlap == 0).
  std::vector<std::pair<double, double>> windows;
  std::size_t pos = 0;
  while ((pos = text.find("data-page-depth-top=\"", pos)) !=
         std::string::npos) {
    const auto top_start = pos + std::strlen("data-page-depth-top=\"");
    const auto top_end = text.find('"', top_start);
    const auto bot_key = text.find("data-page-depth-bottom=\"", top_end);
    const auto bot_start =
        bot_key + std::strlen("data-page-depth-bottom=\"");
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

  // Physical-dimension correctness: each page sized page_width x page_height.
  require(count_occurrences(text, "width=\"120mm\"") == svg_count,
          "every page must carry the physical page width");
  require(count_occurrences(text, "height=\"50mm\"") == svg_count,
          "every page must carry the physical page height");

  // Per-page body re-emits the curve (shared emitter): at least one curve path
  // per page.
  require(count_occurrences(text, "data-curve-id=") >= svg_count,
          "every page body must reference the curve layer");

  // Criterion 5: a legend band must be present (repeat_legend is on). The
  // tall scene has one visible curve layer -> one legend entry per page, and
  // each entry emits two legend-role elements (a colour swatch <rect> + a
  // <text>), so the count is 2 per page.
  const auto legend_count = count_occurrences(text, "data-export-role=\"legend\"");
  require(legend_count == 2 * svg_count,
          "every page must carry a legend band when repeat_legend is set");

  // The fixed-mode body must be mapped onto the printable area: its <g> carries
  // a scale (scene->printable-width) AND a left-margin translate. The earlier
  // translate-only form under-filled the page and ignored the left margin; this
  // guards the regression. Scale = printable_width / scene_width = 100/80 = 1.25.
  require(count_occurrences(text, "data-export-role=\"body\"") == svg_count,
          "every page must carry a body group");
  require(text.find("scale(1.25 1.25)") != std::string::npos,
          "fixed-mode body must scale the scene onto the printable width");
  require(text.find("translate(10 ") != std::string::npos,
          "fixed-mode body must translate by the left page margin");

  // Criterion 1 (self-describing): each page root carries the snapshot metadata
  // (document id/revision, presentation version, the full depth-transform
  // descriptor, font). The depth-transform params (domain + reference window +
  // unit) — not just the version — let a consumer reconstruct the depth mapping.
  require(count_occurrences(text, "data-document-revision=\"7\"") == svg_count,
          "every page must carry the document revision");
  require(count_occurrences(text, "data-presentation-version=\"42\"") ==
              svg_count,
          "every page must carry the presentation version");
  require(count_occurrences(text, "data-depth-transform-domain=\"0\"") ==
              svg_count,
          "every page must carry the depth-transform domain");
  require(count_occurrences(text, "data-depth-transform-unit=\"m\"") ==
              svg_count,
          "every page must carry the depth-transform unit");
  require(count_occurrences(text, "data-depth-transform-reference-top=\"1000\"")
              == svg_count,
          "every page must carry the depth-transform reference top");
  require(count_occurrences(text,
                            "data-depth-transform-reference-bottom=\"1800\"") ==
              svg_count,
          "every page must carry the depth-transform reference bottom");
  require(count_occurrences(text, "data-depth-transform-version=\"3\"") ==
              svg_count,
          "every page must carry the depth-transform version");
  require(count_occurrences(text, "data-font-asset=\"font-fixture-v1\"") ==
              svg_count,
          "every page must carry the font asset fingerprint");
}

void aggregate_pixel_height_is_positive_and_scale_aware() {
  const auto scene = prepare_tall_scene();
  ExportPageSpec page = make_snapshot(PaginationMode::fixed, Millimetres{50.0}).page;
  const auto agg = PaginatedSvgExporter::required_aggregate_pixel_height(*scene,
                                                                         page);
  require(agg > 0, "aggregate pixel height must be positive for a real scene");
  // Higher DPI must yield a strictly greater aggregate height.
  page.dpi = 600;
  const auto agg_hi =
      PaginatedSvgExporter::required_aggregate_pixel_height(*scene, page);
  require(agg_hi > agg,
          "aggregate pixel height must increase with export DPI");
}

void single_scene_exporter_is_unchanged() {
  const auto scene = prepare_tall_scene();
  const auto svg = SvgExporter::write(*scene);
  require(svg.has_value(), "single-scene export must still succeed");
  const auto text = std::string{svg.value().text()};
  require(text.find("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80mm\" "
                    "height=\"400mm\"") != std::string::npos,
          "single-scene exporter must preserve its scene-sized document");
  require(text.find("data-document-id=\"60000000-0000-4000-8000-000000000001\"")
              != std::string::npos,
          "single-scene exporter must preserve document identity");
}

void invalid_scene_or_snapshot_is_rejected() {
  PreparedScene empty;
  auto snapshot = make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  const auto result = PaginatedSvgExporter::write(empty, snapshot);
  require(!result.has_value() &&
              result.error().code == ErrorCode::invalid_presentation,
          "empty scene must be rejected with invalid_presentation");

  // Valid scene but non-positive page width.
  const auto scene = prepare_tall_scene();
  snapshot.page.page_width = Millimetres{0.0};
  const auto bad = PaginatedSvgExporter::write(*scene, snapshot);
  require(!bad.has_value() &&
              bad.error().code == ErrorCode::invalid_presentation,
          "non-positive page width must be rejected");

  // Valid scene but margins consuming the whole page.
  auto snapshot2 =
      make_snapshot(PaginationMode::fixed, Millimetres{50.0});
  snapshot2.page.margins.left = Millimetres{70.0};
  snapshot2.page.margins.right = Millimetres{70.0};
  const auto bad2 = PaginatedSvgExporter::write(*scene, snapshot2);
  require(!bad2.has_value() &&
              bad2.error().code == ErrorCode::invalid_presentation,
          "margins consuming the page must be rejected");
}

// T3 / #275: prepare_for_export produces an export-density scene without
// disturbing the interactive scene. With no LOD pyramids built (this
// fixture's synchronous path), the density has no effect on raw-sample
// emission, but the API must still return a valid scene and leave the
// interactive prepared_scene intact.
void prepare_for_export_returns_scene_and_preserves_interactive() {
  const auto document_id = id("60000000-0000-4000-8000-000000000001");
  // prepare_tall_scene builds the session internally; rebuild here so we
  // keep a handle to the session for the prepare_for_export call.
  WellLogSession session;
  {
    const auto axis_id = id("60000000-0000-4000-8000-000000000002");
    const auto curve_id = id("60000000-0000-4000-8000-000000000003");
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0, 1100.0, 1200.0, 1300.0, 1400.0,
                                      1500.0, 1600.0, 1700.0, 1800.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{0.0, 12.5, 25.0, 37.5, 50.0, 62.5, 75.0,
                                      87.5, 100.0});
    WellLogDocumentBuilder document_builder(document_id, DocumentRevision{7});
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
        .nulls = {},
    });
    require(session.execute(SetDocumentCommand{document_builder.build()})
                .has_value(),
            "export-prepare document must be accepted");
    ScenePresentationBuilder presentation_builder(
        document_id,
        ReferenceDepthRange{
            .domain = DepthDomain::measured_depth,
            .unit = "m",
            .top = 1000.0,
            .bottom = 1800.0,
        },
        Millimetres{400.0}, "font-fixture-v1");
    const auto track_id = id("60000000-0000-4000-8000-000000000004");
    const auto scale_id = id("60000000-0000-4000-8000-000000000005");
    const auto layer_id = id("60000000-0000-4000-8000-000000000006");
    presentation_builder.add_track(TrackSpec{
        .id = track_id, .width = Millimetres{80.0}, .z_order = 10});
    presentation_builder.add_scale(TrackScaleSpec{
        .id = scale_id, .track_id = track_id, .mode = ScaleMode::linear,
        .minimum = 0.0, .maximum = 100.0,
        .direction = ScaleDirection::left_to_right, .unit = "API"});
    presentation_builder.add_curve_layer(CurveLayerSpec{
        .id = layer_id, .track_id = track_id, .curve_id = curve_id,
        .scale_id = scale_id,
        .color = RgbaColor{0x11, 0x22, 0x33, 0xff},
        .line_width = Millimetres{0.25}, .z_order = 20, .visible = true});
    require(session
                .execute(SetPresentationCommand{presentation_builder.build()})
                .has_value(),
            "export-prepare presentation must be accepted");
  }
  const auto interactive = session.prepared_scene(document_id);
  require(interactive != nullptr, "interactive scene must exist");
  const auto interactive_points = interactive->curve_points().size();

  // Re-prepare at an export density. With no LOD pyramids, this takes the
  // raw-sample fallback (same point count) but must return a valid scene.
  auto export_result = session.prepare_for_export(document_id, 8000u);
  require(export_result.has_value(),
          "prepare_for_export must succeed for a prepared document");
  require(export_result.value().document_id() == document_id,
          "export scene must carry the document id");

  // The interactive scene must be untouched by the export-density prepare.
  const auto interactive_after = session.prepared_scene(document_id);
  require(interactive_after != nullptr,
          "interactive scene must still exist after export prepare");
  require(interactive_after->curve_points().size() == interactive_points,
          "export-density prepare must not disturb the interactive scene");

  // An unknown document must surface document_not_found.
  const auto unknown = session.prepare_for_export(
      id("99999999-0000-4000-8000-000000000001"), 8000u);
  require(!unknown.has_value() &&
              unknown.error().code == ErrorCode::document_not_found,
          "prepare_for_export must reject an unknown document");
}

} // namespace

int main() {
  continuous_mode_emits_one_correctly_sized_page();
  fixed_mode_paginates_with_repeating_bands_and_continuous_depths();
  aggregate_pixel_height_is_positive_and_scale_aware();
  single_scene_exporter_is_unchanged();
  invalid_scene_or_snapshot_is_rejected();
  prepare_for_export_returns_scene_and_preserves_interactive();
  std::cout << "PASS: paginated SVG behavior\n";
  return EXIT_SUCCESS;
}
