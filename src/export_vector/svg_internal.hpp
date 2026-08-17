#pragma once

// Internal helpers shared by the single-scene SVG exporter (svg.cpp) and the
// paginated exporter (pagination.cpp). NOT part of the public API; lives in the
// export_vector translation units only. The shared geometric emitter
// (append_defs + append_layer_body) guarantees the paginated pages use exactly
// the same per-layer emission as SvgExporter::write (ADR 0048).
//
// append_number / append_integer / append_xml_attribute / append_color are
// defined directly in this namespace (svg.cpp) — they have no dependencies on
// SvgExporter-internal composites, so they are linked straight across TUs. The
// two entry points append_defs / append_layer_body are emitted via thin bridges
// because their bodies use SvgExporter-private composites that cannot live in
// this namespace.

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

#include <welllog/core/units.hpp>
#include <welllog/export/export_layout.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog::svg_internal {

// Appends a double using the engine's deterministic shortest-round-trip format
// (the same format every other SVG emitter uses, so paginated output matches).
void append_number(std::string &output, double value);

// Appends an integer deterministically (templated over integral width so signed
// and unsigned callers get exact-width output).
template <typename Integer>
  requires std::is_integral_v<Integer>
void append_integer(std::string &output, Integer value);

// Appends an XML-attribute-escaped copy of `value`.
void append_xml_attribute(std::string &output, std::string_view value);

// Appends an SVG #rrggbb colour (alpha ignored — emitted separately as opacity).
void append_color(std::string &output, RgbaColor color);

// Emits the <defs> block (track clipPaths, pattern tiles, glyph outline paths).
// Bridge to a SvgExporter-private definition (uses internal composites).
void append_defs(std::string &output, const PreparedScene &scene);

// Emits the per-track, per-layer <g> body (the single geometric emitter shared
// by both exporters). Each track <g> is clipped to its own track clip. Bridge to
// a SvgExporter-private definition.
//
// `window` (issue #604): when non-null and `window->clip`, geometry whose
// scene-y range lies entirely outside the page depth window is omitted so
// fixed-mode multi-page export is O(points) not O(pages × points). Null
// (or unclipped) keeps the historical full-scene emit.
void append_layer_body(std::string &output, const PreparedScene &scene,
                       const export_layout::PageWindow *window = nullptr);

} // namespace welllog::svg_internal
