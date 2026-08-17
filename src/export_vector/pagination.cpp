#include <welllog/export/pagination.hpp>

#include <welllog/export/export_layout.hpp>
#include <welllog/scene/axis_ticks.hpp>
#include <welllog/export/export_report.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

#include "export_vector/svg_internal.hpp"

namespace welllog {
namespace {

using svg_internal::append_color;
using svg_internal::append_defs;
using svg_internal::append_integer;
using svg_internal::append_layer_body;
using svg_internal::append_number;
using svg_internal::append_xml_attribute;

// 1 inch = 25.4 mm (ADR 0039 unit conversion; never uses screen DPI).
constexpr double millimetres_per_inch = 25.4;

[[nodiscard]] Error
pagination_error(ErrorCode code, MessageKey message) noexcept {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

[[nodiscard]] bool snapshot_is_valid(const PreparedScene &scene,
                                     const ExportSnapshot &snapshot) noexcept {
  if (scene.document_id().is_nil() || scene.document_revision().value == 0 ||
      !std::isfinite(scene.physical_width().value) ||
      scene.physical_width().value <= 0.0 ||
      !std::isfinite(scene.physical_height().value) ||
      scene.physical_height().value <= 0.0 || scene.tracks().empty()) {
    return false;
  }
  const auto &page = snapshot.page;
  if (!std::isfinite(page.page_width.value) || page.page_width.value <= 0.0 ||
      !std::isfinite(page.page_height.value) || page.page_height.value <= 0.0) {
    return false;
  }
  const auto finite_margin = [](Millimetres m) {
    return std::isfinite(m.value) && m.value >= 0.0;
  };
  if (!finite_margin(page.margins.top) || !finite_margin(page.margins.right) ||
      !finite_margin(page.margins.bottom) ||
      !finite_margin(page.margins.left)) {
    return false;
  }
  // The printable area must be strictly positive.
  if (page.margins.left.value + page.margins.right.value >=
          page.page_width.value ||
      page.margins.top.value + page.margins.bottom.value >=
          page.page_height.value) {
    return false;
  }
  if (page.dpi == 0 ||
      !std::isfinite(page.page_overlap) || page.page_overlap < 0.0 ||
      page.page_overlap >= 1.0) {
    return false;
  }
  return true;
}

// Printable-area + depth math come from the shared export_layout header
// (identical to the PDF backend — ADR 0047/0048: one page model).
using export_layout::compute_page_windows;
using export_layout::printable_depth_height_mm;
using export_layout::printable_height;
using export_layout::printable_width;

// Formats a depth value with the engine's deterministic shortest-round-trip
// representation (matches the rest of the SVG emitters).
void append_depth(std::string &output, double depth) noexcept {
  append_number(output, depth);
}

// Appends the per-page depth-range footer: a <text> carrying the depth window
// both as machine-readable data-page-depth-top/-bottom (for cross-page
// continuity assertions, criterion 3/8) and as a human "depth A .. B" label.
// Emitted identically by fixed pages and the continuous page (criterion 8).
void append_depth_range_footer(std::string &output, double x_mm,
                               double footer_y_mm, double depth_top,
                               double depth_bottom) noexcept {
  output += "<text data-export-role=\"footer\" x=\"";
  append_number(output, x_mm);
  output += "\" y=\"";
  append_number(output, footer_y_mm);
  output += "\" data-page-depth-top=\"";
  append_depth(output, depth_top);
  output += "\" data-page-depth-bottom=\"";
  append_depth(output, depth_bottom);
  output += "\" font-size=\"3\">depth ";
  append_depth(output, depth_top);
  output += " .. ";
  append_depth(output, depth_bottom);
  output += "</text>";
}

// scene-y → reference-depth is shared via export_layout (above using-declaration
// block would grow; keep scene_y_to_depth there too).
using export_layout::scene_y_to_depth;

// Appends one plain <text> element tagged with a data-export-role, used for the
// synthesized page header/footer/legend strings (well name, page number, depth
// range, curve legend) — plain ASCII SVG text, not the scene's glyph runs.
void append_text_element(std::string &output, std::string_view role,
                         double x_mm, double y_mm, std::string_view body,
                         double font_size_mm = 3.0) {
  output += "<text data-export-role=\"";
  output += role;
  output += "\" x=\"";
  append_number(output, x_mm);
  output += "\" y=\"";
  append_number(output, y_mm);
  output += "\" font-size=\"";
  append_number(output, font_size_mm);
  output += "\">";
  append_xml_attribute(output, body);
  output += "</text>";
}

// Depth ruler (Epic B, B4): authoritative nice-step ticks over the page's
// depth window, drawn in the LEFT MARGIN strip (no layout impact). Tick marks
// at the printable left edge, labels left-anchored from the page edge. The
// tick VALUES are the SDK scene::nice_axis_ticks output and the labels the
// SDK format_axis_tick_label — the PDF backend emits the same geometry.
void append_depth_ruler(std::string &output, const ExportPageSpec &page,
                        const PreparedScene &scene, double window_top_mm,
                        double window_bottom_mm) noexcept {
  const auto depth_top = scene_y_to_depth(scene, window_top_mm);
  const auto depth_bottom = scene_y_to_depth(scene, window_bottom_mm);
  const auto ticks = nice_axis_ticks(depth_top, depth_bottom);
  if (ticks.values.empty()) {
    return;
  }
  const double left_edge = page.margins.left.value;
  const double span = depth_bottom - depth_top;
  const double y_span = window_bottom_mm - window_top_mm;
  constexpr double font_mm = 2.4;
  for (const double value : ticks.values) {
    const double t = (value - depth_top) / span;
    const double y_mm = window_top_mm + t * y_span;
    // Tick mark: 2.5 mm into the margin from the printable left edge.
    output += "<line data-export-role=\"ruler\" x1=\"";
    append_number(output, left_edge - 1.0);
    output += "\" y1=\"";
    append_number(output, y_mm);
    output += "\" x2=\"";
    append_number(output, left_edge - 3.5);
    output += "\" y2=\"";
    append_number(output, y_mm);
    output += "\" stroke=\"#333333\" stroke-width=\"0.4\"/>";
    // Label, left-anchored from the page edge (2.4 mm font fits 4-digit
    // depths in a 10 mm margin; deeper values may crowd the tick).
    append_text_element(output, "ruler", 1.0, y_mm + 0.9,
                        format_axis_tick_label(value, ticks.step), font_mm);
  }
}

// Crop/trim marks (剪切线, FRS §5) at the four printable-area corners: two
// short registration lines per corner, drawn outward into the margins. The
// geometry is shared with the PDF backend (pdf_scene.cpp) so both outputs
// align on the printed page.
void append_crop_marks(std::string &output, const ExportPageSpec &page) noexcept {
  constexpr double mark_length_mm = 5.0;
  const double w = page.page_width.value;
  const double h = page.page_height.value;
  const double left = page.margins.left.value;
  const double top = page.margins.top.value;
  const double right = w - page.margins.right.value;
  const double bottom = h - page.margins.bottom.value;
  const auto mark = [&output](double x1, double y1, double x2, double y2) {
    output += "<line data-export-role=\"crop-mark\" x1=\"";
    append_number(output, x1);
    output += "\" y1=\"";
    append_number(output, y1);
    output += "\" x2=\"";
    append_number(output, x2);
    output += "\" y2=\"";
    append_number(output, y2);
    output += "\" stroke=\"#000000\" stroke-width=\"0.3\"/>";
  };
  // Top-left
  mark(left - mark_length_mm, top, left, top);
  mark(left, top - mark_length_mm, left, top);
  // Top-right
  mark(right, top, right + mark_length_mm, top);
  mark(right, top - mark_length_mm, right, top);
  // Bottom-left
  mark(left - mark_length_mm, bottom, left, bottom);
  mark(left, bottom, left, bottom + mark_length_mm);
  // Bottom-right
  mark(right, bottom, right + mark_length_mm, bottom);
  mark(right, bottom, right, bottom + mark_length_mm);
}

// Appends the self-describing snapshot metadata as data-* attributes on the
// page root <svg>, so every page records the document revision, presentation
// version, font fingerprint, the depth-transform descriptor (domain + reference
// window + unit + version) and the pattern versions the export was produced
// against (criterion 1 "self-describing"; table-and-export.md section 9
// "Revision 元数据"). Mirrors the single-scene exporter's document/font tags.
void append_snapshot_metadata(std::string &output,
                              const ExportSnapshot &snapshot) noexcept {
  output += "\" data-document-id=\"";
  append_xml_attribute(output, snapshot.document_id.to_string());
  output += "\" data-document-revision=\"";
  append_integer(output, snapshot.document_revision.value);
  output += "\" data-presentation-version=\"";
  append_integer(output, snapshot.presentation_version.value);
  // The full depth-transform descriptor (domain + reference window + unit), not
  // just the version tag, so a consumer can reconstruct the depth mapping
  // (criterion 1). `unit` is owning so it survives on the snapshot.
  output += "\" data-depth-transform-domain=\"";
  append_integer(output, static_cast<std::uint64_t>(snapshot.depth_transform.domain));
  output += "\" data-depth-transform-unit=\"";
  append_xml_attribute(output, snapshot.depth_transform.unit);
  output += "\" data-depth-transform-reference-top=\"";
  append_number(output, snapshot.depth_transform.reference_top);
  output += "\" data-depth-transform-reference-bottom=\"";
  append_number(output, snapshot.depth_transform.reference_bottom);
  output += "\" data-depth-transform-version=\"";
  append_integer(output, snapshot.depth_transform.version);
  output += "\" data-font-asset=\"";
  append_xml_attribute(output, snapshot.font_asset_fingerprint);
  // Pattern versions, parallel to the scene's patterns() order, as a
  // space-separated list. Omitted (empty attribute) when the scene has none.
  output += "\" data-pattern-versions=\"";
  for (std::size_t i = 0; i < snapshot.pattern_versions.size(); ++i) {
    if (i != 0) {
      output.push_back(' ');
    }
    append_integer(output, snapshot.pattern_versions[i]);
  }
  output += "\">";
}

// Emits one fixed page: page-sized <svg>, header/footer/legend bands, and the
// scene body clipped to this page's depth window and translated into place.
void append_fixed_page(std::string &output, const PreparedScene &scene,
                       const ExportSnapshot &snapshot, std::uint32_t page_index,
                       std::uint32_t page_count, double window_top_mm,
                       double window_bottom_mm) noexcept {
  const auto &page = snapshot.page;
  output += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"";
  append_number(output, page.page_width.value);
  output += "mm\" height=\"";
  append_number(output, page.page_height.value);
  output += "mm\" viewBox=\"0 0 ";
  append_number(output, page.page_width.value);
  output.push_back(' ');
  append_number(output, page.page_height.value);
  output += "\" data-export-page=\"";
  append_integer(output, page_index + 1);
  output += "\" data-export-page-count=\"";
  append_integer(output, page_count);
  append_snapshot_metadata(output, snapshot);

  // Patterns/glyph defs are emitted once per page (each page is a standalone
  // SVG document). Track clipPaths/patterns/glyphs live in the shared helper.
  append_defs(output, scene);

  if (page.crop_marks) {
    append_crop_marks(output, page);
  }

  // Page header band: well name + page number (synthesized plain text, distinct
  // from the per-track curve headers emitted in the body below).
  const auto content_left = page.margins.left.value;
  const auto content_top = page.margins.top.value;
  const auto printable_page_height = printable_height(page);
  if (page.repeat_headers) {
    if (!page.well_name.empty()) {
      append_text_element(output, "header", content_left,
                          content_top + 3.0, page.well_name);
    }
    if (page.show_page_numbers) {
      std::string page_label = "page ";
      append_integer(page_label, page_index + 1);
      page_label += " of ";
      append_integer(page_label, page_count);
      const auto page_label_x =
          page.page_width.value - page.margins.right.value;
      append_text_element(output, "header", page_label_x, content_top + 3.0,
                          page_label);
    }
  }

  // Depth ruler (Epic B, B4): authoritative ticks in the left margin strip.
  if (page.show_depth_ruler) {
    append_depth_ruler(output, page, scene, window_top_mm, window_bottom_mm);
  }

  // Per-page depth range (footer band). data-page-depth-top/-bottom carry the
  // depth window for cross-page continuity assertions (criterion 3/8).
  if (page.show_depth_range) {
    const auto depth_top = scene_y_to_depth(scene, window_top_mm);
    const auto depth_bottom = scene_y_to_depth(scene, window_bottom_mm);
    const auto footer_y =
        page.page_height.value - page.margins.bottom.value + 3.0;
    append_depth_range_footer(output, content_left, footer_y, depth_top,
                              depth_bottom);
  }

  // Legend band: one line per visible curve layer (mnemonic + colour swatch +
  // scale range), from the prepared track-header entries. Repeated per page.
  // The band is RESERVED at the bottom of the printable area (4 mm per entry),
  // and the scene body's clip rect is shrunk by that height below so the legend
  // never overpaints curve geometry.
  const auto legend_entries =
      page.repeat_legend ? scene.track_header_entries().size() : std::size_t{0};
  constexpr double legend_row_height_mm = 4.0;
  const double legend_band_height_mm =
      static_cast<double>(legend_entries) * legend_row_height_mm;
  const double body_clip_height_mm =
      printable_page_height - legend_band_height_mm;
  if (page.repeat_legend) {
    const auto headers = scene.track_header_entries();
    double legend_y = content_top + printable_page_height - 3.0;
    for (const auto &entry : headers) {
      output += "<rect data-export-role=\"legend\" x=\"";
      append_number(output, content_left);
      output += "\" y=\"";
      append_number(output, legend_y - 2.0);
      output += "\" width=\"3\" height=\"2\" fill=\"";
      append_color(output, entry.color);
      output += "\"/>";
      std::string legend = entry.curve_name;
      legend += " ";
      append_number(legend, entry.scale_minimum);
      legend += "..";
      append_number(legend, entry.scale_maximum);
      legend += " ";
      legend += entry.unit;
      append_text_element(output, "legend", content_left + 4.0, legend_y,
                          legend);
      legend_y -= legend_row_height_mm;
    }
  }

  // The scene body, mapped onto the printable area and clipped to this page's
  // depth window. The scene is scaled the same way as continuous mode
  // (scale = printable_width / scene_width) so it fills the printable width and
  // depth proportions stay true; a translate positions scene-y=window_top at the
  // page content top, so only [window_top, window_bottom] of the scaled scene
  // shows on this page. The clipPath is in PAGE millimetres over the body
  // region — the printable area MINUS the reserved legend band at the bottom —
  // so the legend never overlaps the body; the scene's own track clips are
  // preserved inside append_layer_body.
  const auto printable_width_mm = printable_width(page);
  const auto scale = printable_width_mm / scene.physical_width().value;
  output += "<clipPath id=\"page-window-";
  append_integer(output, page_index);
  output += "\"><rect x=\"";
  append_number(output, content_left);
  output += "\" y=\"";
  append_number(output, content_top);
  output += "\" width=\"";
  append_number(output, printable_width_mm);
  output += "\" height=\"";
  append_number(output, body_clip_height_mm);
  output += "\"/></clipPath>";
  output += "<g clip-path=\"url(#page-window-";
  append_integer(output, page_index);
  output += ")\" transform=\"translate(";
  append_number(output, content_left);
  output.push_back(' ');
  // Scene point (x, window_top) must land at content_top: after scale, y is
  // window_top*scale, so translate by content_top - window_top*scale.
  append_number(output, content_top - window_top_mm * scale);
  output += ") scale(";
  append_number(output, scale);
  output.push_back(' ');
  append_number(output, scale);
  output += ")\" data-export-role=\"body\">";
  const export_layout::PageWindow page_window{
      .window_top_mm = window_top_mm,
      .window_bottom_mm = window_bottom_mm,
      .clip = true,
      .height_mm = page.page_height.value,
  };
  append_layer_body(output, scene, &page_window);
  output += "</g>";

  output += "</svg>";
}

} // namespace

std::uint32_t PaginatedSvgExporter::required_aggregate_pixel_height(
    const PreparedScene &scene, const ExportPageSpec &page) noexcept {
  if (scene.physical_height().value <= 0.0 ||
      !std::isfinite(scene.physical_height().value) || page.dpi == 0) {
    return 0;
  }
  const auto printable_depth_mm = printable_depth_height_mm(scene, page);
  // Pixels per millimetre at the export DPI.
  const auto pixels_per_mm =
      static_cast<double>(page.dpi) / millimetres_per_inch;
  // Per-page depth pixels; the aggregate over the whole scene depth is the
  // sum across pages (uniform density assumption, ADR 0048).
  const auto page_depth_px = printable_depth_mm * pixels_per_mm;
  const auto effective_step = printable_depth_mm * (1.0 - page.page_overlap);
  if (effective_step <= 0.0) {
    return 0;
  }
  const auto page_count_d = std::ceil(scene.physical_height().value /
                                      effective_step);
  const auto page_count =
      page_count_d < 1.0 ? 1.0 : page_count_d;
  const auto aggregate =
      static_cast<double>(page_count) * page_depth_px;
  if (aggregate <= 0.0 || !std::isfinite(aggregate)) {
    return 0;
  }
  if (aggregate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(aggregate);
}

Result<SvgDocument>
PaginatedSvgExporter::write(const PreparedScene &scene,
                            const ExportSnapshot &snapshot,
                            ExportReport *report) noexcept {
  try {
    if (!snapshot_is_valid(scene, snapshot)) {
      return pagination_error(ErrorCode::invalid_presentation,
                              MessageKey::presentation_invalid);
    }
    // Criterion 7: evaluate the shared complexity heuristic once. In pure-vector
    // mode (default) an over-budget layer must FAIL rather than silently
    // rasterize; in mixed mode the over-budget layers are recorded in the report
    // (the actual raster path is a documented follow-up). Identical to the PDF
    // backend (pdf_scene.cpp) so both make the same decision for the same input.
    const auto decision = evaluate_complexity(
        scene, snapshot.page.vector_complexity_budget, snapshot.page.dpi);
    if (decision.would_degrade()) {
      if (snapshot.page.export_mode == ExportMode::pure_vector) {
        return pagination_error(ErrorCode::invalid_presentation,
                                MessageKey::presentation_invalid);
      }
      if (report != nullptr) {
        report->degraded_layers.insert(report->degraded_layers.end(),
                                       decision.over_budget.begin(),
                                       decision.over_budget.end());
      }
    }
    const auto &page = snapshot.page;

    std::string output;
    output.reserve(1024 + scene.curve_points().size() * 32);

    if (page.mode == PaginationMode::continuous) {
      // One continuous page: the printable width maps the scene width, and the
      // page height preserves true depth->physical-length (scene physical height
      // scaled by the same width factor), so depth proportions stay correct.
      const auto printable_width_mm = printable_width(page);
      const auto scale = printable_width_mm / scene.physical_width().value;
      const auto page_height_mm =
          scene.physical_height().value * scale +
          page.margins.top.value + page.margins.bottom.value;

      output += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"";
      append_number(output, page.page_width.value);
      output += "mm\" height=\"";
      append_number(output, page_height_mm);
      output += "mm\" viewBox=\"0 0 ";
      append_number(output, page.page_width.value);
      output.push_back(' ');
      append_number(output, page_height_mm);
      output += "\" data-export-page=\"1\" data-export-page-count=\"1\"";
      append_snapshot_metadata(output, snapshot);
      append_defs(output, scene);

      if (page.crop_marks) {
        append_crop_marks(output, page);
      }

      if (page.repeat_headers && !page.well_name.empty()) {
        append_text_element(output, "header", page.margins.left.value,
                            page.margins.top.value + 3.0, page.well_name);
      }
      if (page.show_depth_range) {
        const auto depth_top =
            scene_y_to_depth(scene, 0.0);
        const auto depth_bottom = scene_y_to_depth(
            scene, scene.physical_height().value);
        const auto footer_y = page_height_mm - page.margins.bottom.value + 3.0;
        append_depth_range_footer(output, page.margins.left.value, footer_y,
                                  depth_top, depth_bottom);
      }

      // Depth ruler (Epic B, B4) on the continuous page: same authoritative
      // ticks over the full scene depth window.
      if (page.show_depth_ruler) {
        append_depth_ruler(output, page, scene, 0.0,
                           scene.physical_height().value);
      }

      // Body translated to (margin-left, margin-top) and scaled to fit the
      // printable width; vertical scale keeps depth proportions true.
      output += "<g transform=\"translate(";
      append_number(output, page.margins.left.value);
      output.push_back(' ');
      append_number(output, page.margins.top.value);
      output += ") scale(";
      append_number(output, scale);
      output.push_back(' ');
      append_number(output, scale);
      output += ")\" data-export-role=\"body\">";
      append_layer_body(output, scene);
      output += "</g></svg>";
      return SvgDocument{std::move(output)};
    }

    // Fixed mode: slice the scene depth range into pages using the shared page
    // model (identical slicing to the PDF backend — export_layout).
    const auto windows = compute_page_windows(scene, snapshot);
    const auto page_count = static_cast<std::uint32_t>(windows.size());
    for (std::uint32_t index = 0; index < page_count; ++index) {
      append_fixed_page(output, scene, snapshot, index, page_count,
                        windows[index].window_top_mm,
                        windows[index].window_bottom_mm);
    }
    return SvgDocument{std::move(output)};
  } catch (const std::bad_alloc &) {
    return pagination_error(ErrorCode::resource_exhausted,
                            MessageKey::resource_exhausted);
  } catch (...) {
    return pagination_error(ErrorCode::internal_error,
                            MessageKey::internal_error);
  }
}

} // namespace welllog
