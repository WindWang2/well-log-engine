#pragma once

// Backend-neutral export-layout geometry shared by the SVG (welllog_export_vector)
// and PDF (welllog_export_pdf) exporters (ADR 0047: "both backends share one
// geometric truth"). Header-only (inline) so each exporter links its own copy
// without one library depending on the other — both already depend on
// WellLog::Scene, which owns PhysicalPoint / Millimetres / PreparedScene.
//
// Two kinds of helper live here:
//   - pattern-tile line clipping (clip_line_to_tile): pure geometry, identical
//     for every vector backend;
//   - physical page-layout math (printable area, depth-window slicing, scene-y
//     to reference-depth): the shared "page model" from ADR 0048 / #186 that
//     paginated SVG and paginated PDF must agree on.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <welllog/core/units.hpp>
#include <welllog/export/pagination.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog::export_layout {

// The printable-area width/height in millimetres (page size minus margins) —
// the printable-area concept table-and-export.md §9 names. Centralized so the
// margin arithmetic is expressed once across both backends.
[[nodiscard]] inline double printable_width(const ExportPageSpec &page) noexcept {
  return page.page_width.value - page.margins.left.value -
         page.margins.right.value;
}
[[nodiscard]] inline double printable_height(const ExportPageSpec &page) noexcept {
  return page.page_height.value - page.margins.top.value -
         page.margins.bottom.value;
}

// The scene depth span that fits the page's printable depth height, in scene
// millimetres: the scene is scaled so its physical width matches the printable
// width, then the printable height is expressed in those scaled scene-mm.
[[nodiscard]] inline double
printable_depth_height_mm(const PreparedScene &scene,
                          const ExportPageSpec &page) noexcept {
  const auto scale = printable_width(page) / scene.physical_width().value;
  return printable_height(page) / scale;
}

// Legend band reserved at the bottom of the printable area (4 mm per visible
// curve-header entry, capped so the body clip cannot go negative). Shared by
// SVG and PDF so both shrink the body by the same amount (#745 / #746).
inline constexpr double legend_row_height_mm = 4.0;

[[nodiscard]] inline std::size_t
legend_entry_count(const PreparedScene &scene,
                   const ExportPageSpec &page) noexcept {
  if (!page.repeat_legend) {
    return 0;
  }
  const auto printable = printable_height(page);
  const auto max_entries =
      printable > 0.0
          ? static_cast<std::size_t>(printable / legend_row_height_mm)
          : std::size_t{0};
  return std::min(scene.track_header_entries().size(), max_entries);
}

[[nodiscard]] inline double
legend_band_height_mm(const PreparedScene &scene,
                      const ExportPageSpec &page) noexcept {
  return static_cast<double>(legend_entry_count(scene, page)) *
         legend_row_height_mm;
}

// Linear scene-y (mm, 0 at the top) → reference depth, using the scene's depth
// range. Shared so the depth-range footer is identical across backends.
[[nodiscard]] inline double scene_y_to_depth(const PreparedScene &scene,
                                             double y_mm) noexcept {
  const auto range = scene.reference_depth_range();
  const auto span = range.bottom - range.top;
  return range.top + (y_mm / scene.physical_height().value) * span;
}

// One page's depth window, in scene millimetres, from the shared page model
// (continuous = whole scene; fixed = a slice of the printable depth height,
// stepping by (1 - page_overlap) with the final page bottoming out at the scene
// height). Carries whether a per-page depth-window clip is needed (fixed only)
// and the page height in mm for the MediaBox.
struct PageWindow {
  double window_top_mm;
  double window_bottom_mm;
  bool clip;        // false for continuous, true for fixed
  double height_mm; // page height in mm (MediaBox derived)
};

// Computes all page windows for a snapshot's pagination mode. Mirrors the loop
// PaginatedSvgExporter uses (src/export_vector/pagination.cpp) so PDF and SVG
// slice the depth range identically.
[[nodiscard]] inline std::vector<PageWindow>
compute_page_windows(const PreparedScene &scene,
                     const ExportSnapshot &snapshot) noexcept {
  const auto &page = snapshot.page;
  const auto scale = printable_width(page) / scene.physical_width().value;
  std::vector<PageWindow> windows;
  if (page.mode == PaginationMode::continuous) {
    // The continuous page must fit BOTH the full scene body AND the repeating
    // legend band (repeat_legend, #839): the page height grows by the reserved
    // legend band so the legend lands below the body instead of overpainting
    // curve geometry. legend_band_height_mm is 0 when repeat_legend is off or
    // the scene has no visible headers, so existing outputs are unchanged.
    const auto legend_band_mm =
        page.repeat_legend ? legend_band_height_mm(scene, page) : 0.0;
    const auto page_height_mm = scene.physical_height().value * scale +
                                page.margins.top.value +
                                page.margins.bottom.value + legend_band_mm;
    windows.push_back({0.0, scene.physical_height().value, false, page_height_mm});
    return windows;
  }
  const auto printable_depth_mm = printable_depth_height_mm(scene, page);
  const auto effective_step = printable_depth_mm * (1.0 - page.page_overlap);
  const auto scene_height = scene.physical_height().value;
  // Guard the double→uint32 narrowing: a tiny-but-positive physical_width
  // (host-authored, still snapshot-valid) makes effective_step arbitrarily
  // small and page_count out of range (UB + multi-billion push_back).
  constexpr double k_max_page_windows = 65'536.0;
  if (!std::isfinite(effective_step) || effective_step <= 0.0 ||
      !std::isfinite(scene_height) || scene_height < 0.0) {
    return windows;
  }
  const auto page_count_d = std::ceil(scene_height / effective_step);
  if (!std::isfinite(page_count_d) || page_count_d > k_max_page_windows) {
    return windows;
  }
  auto page_count = static_cast<std::uint32_t>(page_count_d);
  if (page_count == 0) {
    page_count = 1;
  }
  for (std::uint32_t index = 0; index < page_count; ++index) {
    const auto window_top = static_cast<double>(index) * effective_step;
    auto window_bottom = window_top + printable_depth_mm;
    if (window_bottom > scene_height || index + 1 == page_count) {
      window_bottom = scene_height;
    }
    windows.push_back({window_top, window_bottom, true, page.page_height.value});
  }
  return windows;
}

// Inclusive y-range vs a page window. `pad_mm` expands the window so strokes
// and symbols that straddle the cut still emit (the page clip finishes the
// job). A null / unclipped window matches everything — continuous export
// and the single-scene SVG emitter pass that.
[[nodiscard]] inline bool range_intersects_window(const PageWindow *window,
                                                  double top_mm,
                                                  double bottom_mm,
                                                  double pad_mm = 0.0) noexcept {
  if (window == nullptr || !window->clip) {
    return true;
  }
  const auto lo = std::min(top_mm, bottom_mm);
  const auto hi = std::max(top_mm, bottom_mm);
  return hi >= window->window_top_mm - pad_mm &&
         lo <= window->window_bottom_mm + pad_mm;
}

[[nodiscard]] inline bool y_intersects_window(const PageWindow *window,
                                              double y_mm,
                                              double pad_mm = 0.0) noexcept {
  return range_intersects_window(window, y_mm, y_mm, pad_mm);
}

// An edge between two scene-y samples intersects the window (used to keep
// the one sample on each side of a page cut so the clipped stroke is
// continuous).
[[nodiscard]] inline bool edge_intersects_window(const PageWindow *window,
                                                 double y0_mm,
                                                 double y1_mm) noexcept {
  return range_intersects_window(window, y0_mm, y1_mm, 0.0);
}

// Clips a tile-local segment to the pattern tile rect (Liang-Barsky). Pure
// geometry, backend-neutral — the single source of truth both SVG and PDF
// pattern emission use, so adjacent tiles connect identically (ADR 0020).
[[nodiscard]] inline std::optional<std::pair<PhysicalPoint, PhysicalPoint>>
clip_line_to_tile(PhysicalPoint from, PhysicalPoint to, double width,
                  double height) noexcept {
  const auto delta_x = to.left.value - from.left.value;
  const auto delta_y = to.top.value - from.top.value;
  double enter = 0.0;
  double leave = 1.0;
  const auto clip_side = [&](double p, double q) {
    if (p == 0.0) {
      return q >= 0.0;
    }
    const auto ratio = q / p;
    if (p < 0.0) {
      if (ratio > leave) {
        return false;
      }
      enter = std::max(enter, ratio);
    } else {
      if (ratio < enter) {
        return false;
      }
      leave = std::min(leave, ratio);
    }
    return true;
  };
  if (!clip_side(-delta_x, from.left.value) ||
      !clip_side(delta_x, width - from.left.value) ||
      !clip_side(-delta_y, from.top.value) ||
      !clip_side(delta_y, height - from.top.value)) {
    return std::nullopt;
  }
  return std::pair{
      PhysicalPoint{.left = Millimetres{from.left.value + enter * delta_x},
                    .top = Millimetres{from.top.value + enter * delta_y}},
      PhysicalPoint{.left = Millimetres{from.left.value + leave * delta_x},
                    .top = Millimetres{from.top.value + leave * delta_y}},
  };
}

} // namespace welllog::export_layout
