#include <welllog/export/svg.hpp>

#include <welllog/export/export_layout.hpp>

#include "export_vector/svg_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace welllog {
namespace svg_internal {

// Shared emitters (svg_internal.hpp). These live here, NOT in an anonymous
// namespace, so both svg.cpp and pagination.cpp link against one definition and
// there is no Middle Man wrapper layer — the paginated pages reuse the exact
// same emission as SvgExporter::write (ADR 0048). They have no dependencies on
// the SvgExporter-internal composites below, so the composites stay private.

void append_number(std::string &output, double value) {
  if (value == 0.0) {
    output.push_back('0');
    return;
  }
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general, std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    throw std::bad_alloc{};
  }
  output.append(buffer.data(), result.ptr);
}

template <typename Integer>
  requires std::is_integral_v<Integer>
void append_integer(std::string &output, Integer value) {
  std::array<char, 32> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    throw std::bad_alloc{};
  }
  output.append(buffer.data(), result.ptr);
}

void append_xml_attribute(std::string &output, std::string_view value) {
  for (const auto character : value) {
    switch (character) {
    case '&':
      output += "&amp;";
      break;
    case '<':
      output += "&lt;";
      break;
    case '>':
      output += "&gt;";
      break;
    case '"':
      output += "&quot;";
      break;
    case '\'':
      output += "&apos;";
      break;
    default:
      output.push_back(character);
      break;
    }
  }
}

void append_color(std::string &output, RgbaColor color) {
  constexpr std::string_view digits = "0123456789abcdef";
  const auto append_byte = [&](std::uint8_t value) {
    output.push_back(digits[value >> 4U]);
    output.push_back(digits[value & 0x0fU]);
  };
  output.push_back('#');
  append_byte(color.red);
  append_byte(color.green);
  append_byte(color.blue);
}

} // namespace svg_internal

namespace {
using svg_internal::append_color;
using svg_internal::append_integer;
using svg_internal::append_number;
using svg_internal::append_xml_attribute;

void append_rect(std::string &output, const PhysicalRect &rect) {
  output += "<rect x=\"";
  append_number(output, rect.left.value);
  output += "\" y=\"";
  append_number(output, rect.top.value);
  output += "\" width=\"";
  append_number(output, rect.width.value);
  output += "\" height=\"";
  append_number(output, rect.height.value);
  output += "\"/>";
}

// Tile-local line clip comes from the shared export_layout header (identical
// to the PDF backend's — ADR 0047: one geometric truth).
using export_layout::clip_line_to_tile;

void append_tile_line(std::string &output, PhysicalPoint from, PhysicalPoint to,
                      const PatternDefinition &pattern) {
  const auto clipped =
      clip_line_to_tile(from, to, pattern.tile_width.value,
                        pattern.tile_height.value);
  if (!clipped.has_value()) {
    return;
  }
  output += "<line x1=\"";
  append_number(output, clipped->first.left.value);
  output += "\" y1=\"";
  append_number(output, clipped->first.top.value);
  output += "\" x2=\"";
  append_number(output, clipped->second.left.value);
  output += "\" y2=\"";
  append_number(output, clipped->second.top.value);
  output += "\" stroke=\"";
  append_color(output, pattern.foreground);
  output += "\" stroke-opacity=\"";
  append_number(output,
                static_cast<double>(pattern.foreground.alpha) / 255.0);
  output += "\" stroke-width=\"";
  append_number(output, pattern.stroke_width.value);
  output += "\"/>";
}

// Emits the constrained vector tile exactly once, anchored to scene
// coordinates via patternUnits="userSpaceOnUse" so adjacent intervals and
// scrolling share phase (ADR 0020).
void append_pattern_definition(std::string &output,
                               const PatternDefinition &pattern) {
  output += "<pattern id=\"pat-";
  output += pattern.id.to_string();
  output += "\" patternUnits=\"userSpaceOnUse\" x=\"";
  append_number(output, pattern.scene_anchor.left.value);
  output += "\" y=\"";
  append_number(output, pattern.scene_anchor.top.value);
  output += "\" width=\"";
  append_number(output, pattern.tile_width.value);
  output += "\" height=\"";
  append_number(output, pattern.tile_height.value);
  output += "\" patternTransform=\"rotate(";
  append_number(output, pattern.rotation_degrees);
  output += ")\">";
  if (pattern.background.alpha > 0) {
    output += "<rect x=\"0\" y=\"0\" width=\"";
    append_number(output, pattern.tile_width.value);
    output += "\" height=\"";
    append_number(output, pattern.tile_height.value);
    output += "\" fill=\"";
    append_color(output, pattern.background);
    output += "\" fill-opacity=\"";
    append_number(output,
                  static_cast<double>(pattern.background.alpha) / 255.0);
    output += "\"/>";
  }
  for (const auto &primitive : pattern.primitives) {
    if (const auto *line = std::get_if<PatternLine>(&primitive)) {
      append_tile_line(output, line->from, line->to, pattern);
    } else if (const auto *polyline =
                   std::get_if<PatternPolyline>(&primitive)) {
      for (std::size_t index = 0; index + 1 < polyline->points.size();
           ++index) {
        append_tile_line(output, polyline->points[index],
                         polyline->points[index + 1], pattern);
      }
      if (polyline->closed && polyline->points.size() > 2) {
        append_tile_line(output, polyline->points.back(),
                         polyline->points.front(), pattern);
      }
    } else {
      const auto &circle = std::get<PatternCircle>(primitive);
      output += "<circle cx=\"";
      append_number(output, circle.center.left.value);
      output += "\" cy=\"";
      append_number(output, circle.center.top.value);
      output += "\" r=\"";
      append_number(output, circle.radius.value);
      if (circle.filled) {
        output += "\" fill=\"";
        append_color(output, pattern.foreground);
        output += "\" fill-opacity=\"";
        append_number(output,
                      static_cast<double>(pattern.foreground.alpha) / 255.0);
      } else {
        output += "\" fill=\"none\" stroke=\"";
        append_color(output, pattern.foreground);
        output += "\" stroke-opacity=\"";
        append_number(output,
                      static_cast<double>(pattern.foreground.alpha) / 255.0);
        output += "\" stroke-width=\"";
        append_number(output, pattern.stroke_width.value);
      }
      output += "\"/>";
    }
  }
  output += "</pattern>";
}

// Serializes a glyph outline in em fractions as an SVG path. Scaling,
// y-flipping, rotation and placement happen at the <use> site so every
// run shares one definition.
void append_outline_path_data(std::string &output,
                              std::span<const OutlineCommand> commands) {
  bool first = true;
  for (const auto &command : commands) {
    if (!first) {
      output.push_back(' ');
    }
    first = false;
    switch (command.verb) {
    case OutlineVerb::move_to:
      output += "M ";
      append_number(output, command.coordinates[0]);
      output.push_back(' ');
      append_number(output, command.coordinates[1]);
      break;
    case OutlineVerb::line_to:
      output += "L ";
      append_number(output, command.coordinates[0]);
      output.push_back(' ');
      append_number(output, command.coordinates[1]);
      break;
    case OutlineVerb::quadratic_to:
      output += "Q ";
      append_number(output, command.coordinates[0]);
      output.push_back(' ');
      append_number(output, command.coordinates[1]);
      output.push_back(' ');
      append_number(output, command.coordinates[2]);
      output.push_back(' ');
      append_number(output, command.coordinates[3]);
      break;
    case OutlineVerb::cubic_to:
      output += "C ";
      for (const auto coordinate : command.coordinates) {
        append_number(output, coordinate);
        output.push_back(' ');
      }
      output.pop_back();
      break;
    case OutlineVerb::close:
      output.push_back('Z');
      break;
    }
  }
}

// Emits the kind-specific attributes + path data for a symbol shape centred
// at (center_x, center_y) with half-extent `half`: fill=…[ fill-opacity=…]
// d="…"/> (cross additionally uses stroke). Shared by the symbol-layer and
// custom-layer emitters so every SymbolKind renders the same geometry in
// both (issue #485: custom-layer symbols used to degrade to circles).
void append_symbol_shape(std::string &output, SymbolKind kind,
                         double center_x, double center_y, double half,
                         const RgbaColor &color, const char *fill_opacity) {
  switch (kind) {
  case SymbolKind::circle:
    output += "\" fill=\"";
    append_color(output, color);
    if (fill_opacity != nullptr) {
      output += "\" fill-opacity=\"";
      output += fill_opacity;
    }
    output += "\" d=\"M ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " A ";
    append_number(output, half);
    output.push_back(' ');
    append_number(output, half);
    output += " 0 1 0 ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " A ";
    append_number(output, half);
    output.push_back(' ');
    append_number(output, half);
    output += " 0 1 0 ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " Z\"/>";
    return;
  case SymbolKind::cross:
    output += "\" fill=\"none\" stroke=\"";
    append_color(output, color);
    output += "\" stroke-width=\"";
    append_number(output, half / 3.0);
    output += "\" d=\"M ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " M ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += "\"/>";
    return;
  case SymbolKind::square:
  case SymbolKind::triangle_up:
  case SymbolKind::diamond:
  case SymbolKind::triangle_down:
  case SymbolKind::shoe:
    break;
  }
  output += "\" fill=\"";
  append_color(output, color);
  if (fill_opacity != nullptr) {
    output += "\" fill-opacity=\"";
    output += fill_opacity;
  }
  output += "\" d=\"";
  if (kind == SymbolKind::square) {
    output += "M ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " Z\"/>";
  } else if (kind == SymbolKind::triangle_up) {
    output += "M ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " Z\"/>";
  } else if (kind == SymbolKind::triangle_down) {
    output += "M ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " Z\"/>";
  } else if (kind == SymbolKind::shoe) {
    // Casing-shoe arch (flat side up, bulge down) via one arc + close.
    output += "M ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " A ";
    append_number(output, half);
    output.push_back(' ');
    append_number(output, half);
    output += " 0 0 1 ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " Z\"/>";
  } else {
    output += "M ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " L ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " Z\"/>";
  }
}


void append_symbol(std::string &output, const PreparedSymbol &symbol,
                   const PreparedSymbolLayer &layer) {
  output += "<path id=\"symbol-";
  output += symbol.symbol_id.to_string();
  output += "\" data-layer-id=\"";
  output += layer.id.to_string();
  append_symbol_shape(output, symbol.kind, symbol.center.left.value,
                      symbol.center.top.value, layer.symbol_size.value / 2.0,
                      layer.color, nullptr);
}
// Emits the marker-semantic symbol glyph at the left end of a marker line
// (single source of shape semantics: scene::symbol_for_marker_semantic).
void append_marker_symbol(std::string &output, const PreparedMarker &marker,
                          const PreparedMarkerLayer &layer, double left) {
  const auto kind = symbol_for_marker_semantic(marker.semantic);
  const auto half = layer.symbol_size.value / 2.0;
  const auto center_x = left + 1.0 + half;
  const auto center_y = marker.display_top.value;
  output += "<path id=\"marker-symbol-";
  output += marker.marker_id.to_string();
  output += "\" data-layer-id=\"";
  output += layer.id.to_string();
  output += "\" data-semantic=\"";
  switch (marker.semantic) {
  case MarkerSemantic::formation_top:
    output += "formation_top";
    break;
  case MarkerSemantic::fault:
    output += "fault";
    break;
  case MarkerSemantic::fluid_contact:
    output += "fluid_contact";
    break;
  case MarkerSemantic::casing_shoe:
    output += "casing_shoe";
    break;
  case MarkerSemantic::custom:
    output += "custom";
    break;
  }
  switch (kind) {
  case SymbolKind::cross:
    output += "\" fill=\"none\" stroke=\"";
    append_color(output, layer.line_color);
    output += "\" stroke-width=\"";
    append_number(output, layer.symbol_size.value / 6.0);
    output += "\" d=\"M ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " M ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += "\"/>";
    return;
  case SymbolKind::circle:
    output += "\" fill=\"";
    append_color(output, layer.line_color);
    output += "\" d=\"M ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " A ";
    append_number(output, half);
    output.push_back(' ');
    append_number(output, half);
    output += " 0 1 0 ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " A ";
    append_number(output, half);
    output.push_back(' ');
    append_number(output, half);
    output += " 0 1 0 ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " Z\"/>";
    return;
  default:
    break;
  }
  output += "\" fill=\"";
  append_color(output, layer.line_color);
  output += "\" d=\"";
  if (kind == SymbolKind::square) {
    output += "M ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " Z\"/>";
  } else if (kind == SymbolKind::triangle_up) {
    output += "M ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " Z\"/>";
  } else if (kind == SymbolKind::triangle_down) {
    output += "M ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " Z\"/>";
  } else if (kind == SymbolKind::diamond) {
    output += "M ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y - half);
    output += " L ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " L ";
    append_number(output, center_x);
    output.push_back(' ');
    append_number(output, center_y + half);
    output += " L ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " Z\"/>";
  } else {
    // shoe: arch path (bulge down), matching scene::symbol_glyph.
    output += "M ";
    append_number(output, center_x - half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " A ";
    append_number(output, half);
    output.push_back(' ');
    append_number(output, half);
    output += " 0 0 1 ";
    append_number(output, center_x + half);
    output.push_back(' ');
    append_number(output, center_y);
    output += " Z\"/>";
  }
}

void append_path_data(std::string &output, const PreparedScene &scene,
                      const PreparedCurveLayer &layer) {
  const auto segments = scene.curve_segments();
  const auto points = scene.curve_points();
  bool first_segment = true;
  for (std::uint64_t segment_offset = 0; segment_offset < layer.segment_count;
       ++segment_offset) {
    const auto &segment = segments[static_cast<std::size_t>(
        layer.first_segment + segment_offset)];
    if (!first_segment && segment.point_count > 0) {
      output.push_back(' ');
    }
    for (std::uint64_t point_offset = 0; point_offset < segment.point_count;
         ++point_offset) {
      const auto &point =
          points[static_cast<std::size_t>(segment.first_point + point_offset)];
      output += point_offset == 0 ? "M " : " L ";
      append_number(output, point.position.left.value);
      output.push_back(' ');
      append_number(output, point.position.top.value);
    }
    first_segment = false;
  }
}

// Emits the closed boundary ring of one fill region as an SVG path d
// (M ... L ... Z). The ring is the crossover boundary shared by GL and SVG.
void append_fill_ring_path(std::string &output, const PreparedScene &scene,
                           const PreparedFillRegion &region) {
  const auto vertices = scene.fill_vertices();
  bool first = true;
  for (std::uint64_t offset = 0; offset < region.vertex_count; ++offset) {
    const auto &vertex = vertices[static_cast<std::size_t>(
        region.first_vertex + offset)];
    if (!first) {
      output.push_back(' ');
    }
    output += offset == 0 ? "M " : " L ";
    append_number(output, vertex.position.left.value);
    output.push_back(' ');
    append_number(output, vertex.position.top.value);
    first = false;
  }
  output += " Z";
}

[[nodiscard]] Error svg_error(ErrorCode code, MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

// Emits the <defs> block shared by the single-scene exporter and every
// paginated page: one clipPath per track (the track clip), every pattern tile,
// and one vector <path> per glyph outline. The clipPath ids are referenced by
// the per-track <g> in append_layer_body.
void append_defs(std::string &output, const PreparedScene &scene) {
  output += "<defs>";
  for (const auto &track : scene.tracks()) {
    output += "<clipPath id=\"clip-";
    output += track.id.to_string();
    output += "\">";
    append_rect(output, track.clip);
    output += "</clipPath>";
  }
  for (const auto &pattern : scene.patterns()) {
    append_pattern_definition(output, pattern);
  }
  const auto outline_commands = scene.outline_commands();
  for (const auto &outline : scene.glyph_outlines()) {
    output += "<path id=\"g";
    append_integer(output, outline.font_index);
    output.push_back('-');
    append_integer(output, outline.glyph_id);
    output += "\" d=\"";
    append_outline_path_data(
        output,
        outline_commands.subspan(
            static_cast<std::size_t>(outline.first_command),
            static_cast<std::size_t>(outline.command_count)));
    output += "\"/>";
  }
  output += "</defs>";
}

// Emits the per-track, per-layer <g> body: intervals, markers, symbols, curves,
// crossover fills, image tiles, custom primitives and text runs. Each track's
// <g> is clipped to its own track clip. This is the single geometric emitter
// shared by SvgExporter::write and the paginated exporter (ADR 0048); the
// paginated exporter wraps it in an additional per-page depth-window clip.
void append_layer_body(std::string &output, const PreparedScene &scene) {
  for (const auto &track : scene.tracks()) {
    output += "<g id=\"track-";
    output += track.id.to_string();
    output += "\" clip-path=\"url(#clip-";
    output += track.id.to_string();
    output += ")\" data-z-order=\"";
    append_integer(output, track.z_order);
    output += "\">";
    for (const auto &layer : scene.interval_layers()) {
      if (layer.track_id != track.id) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < layer.interval_count; ++offset) {
        const auto &interval = scene.intervals()[static_cast<std::size_t>(
            layer.first_interval + offset)];
        output += "<rect id=\"interval-";
        output += interval.interval_id.to_string();
        output += "\" data-layer-id=\"";
        output += layer.id.to_string();
        output += "\" data-top-depth=\"";
        append_number(output, interval.top_reference_depth);
        output += "\" data-bottom-depth=\"";
        append_number(output, interval.bottom_reference_depth);
        output += "\" x=\"";
        append_number(output, interval.rect.left.value);
        output += "\" y=\"";
        append_number(output, interval.rect.top.value);
        output += "\" width=\"";
        append_number(output, interval.rect.width.value);
        output += "\" height=\"";
        append_number(output, interval.rect.height.value);
        if (interval.pattern_id.is_nil()) {
          output += "\" fill=\"";
          append_color(output, interval.fill_color);
          output += "\" fill-opacity=\"";
          append_number(
              output,
              static_cast<double>(interval.fill_color.alpha) / 255.0);
        } else {
          output += "\" fill=\"url(#pat-";
          output += interval.pattern_id.to_string();
          output += ")";
        }
        output += "\"/>";
      }
    }
    for (const auto &layer : scene.marker_layers()) {
      if (layer.track_id != track.id) {
        continue;
      }
      const auto right = track.clip.left.value + track.clip.width.value;
      for (std::uint64_t offset = 0; offset < layer.marker_count; ++offset) {
        const auto &marker = scene.markers()[static_cast<std::size_t>(
            layer.first_marker + offset)];
        output += "<line id=\"marker-";
        output += marker.marker_id.to_string();
        output += "\" data-layer-id=\"";
        output += layer.id.to_string();
        output += "\" data-reference-depth=\"";
        append_number(output, marker.reference_depth);
        output += "\" x1=\"";
        append_number(output, track.clip.left.value);
        output += "\" y1=\"";
        append_number(output, marker.display_top.value);
        output += "\" x2=\"";
        append_number(output, right);
        output += "\" y2=\"";
        append_number(output, marker.display_top.value);
        output += "\" stroke=\"";
        append_color(output, layer.line_color);
        output += "\" stroke-opacity=\"";
        append_number(output,
                      static_cast<double>(layer.line_color.alpha) / 255.0);
        output += "\" stroke-width=\"";
        append_number(output, layer.line_width.value);
        output += "\"/>";
        if (layer.draw_symbols) {
          append_marker_symbol(output, marker, layer, track.clip.left.value);
        }
      }
    }
    for (const auto &layer : scene.symbol_layers()) {
      if (layer.track_id != track.id) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < layer.symbol_count; ++offset) {
        const auto &symbol = scene.symbols()[static_cast<std::size_t>(
            layer.first_symbol + offset)];
        append_symbol(output, symbol, layer);
      }
    }
    for (const auto &layer : scene.curve_layers()) {
      if (layer.track_id != track.id) {
        continue;
      }
      output += "<path id=\"layer-";
      output += layer.id.to_string();
      output += "\" data-curve-id=\"";
      output += layer.curve_id.to_string();
      output += "\" data-scale-id=\"";
      output += layer.scale_id.to_string();
      output += "\" data-z-order=\"";
      append_integer(output, layer.z_order);
      output += "\" fill=\"none\" stroke=\"";
      append_color(output, layer.color);
      output += "\" stroke-opacity=\"";
      append_number(output, static_cast<double>(layer.color.alpha) / 255.0);
      output += "\" stroke-width=\"";
      append_number(output, layer.line_width.value);
      output += "\" d=\"";
      append_path_data(output, scene, layer);
      output += "\"/>";
    }
    // Crossover fill regions (rendering.md section 6): each region's
    // closed boundary ring is emitted as one <path>, filled with a solid
    // color or a pattern reference, tagged with both dependent curves.
    for (const auto &fill_layer : scene.fill_layers()) {
      if (fill_layer.track_id != track.id) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < fill_layer.region_count;
           ++offset) {
        const auto &region = scene.fill_regions()[static_cast<std::size_t>(
            fill_layer.first_region + offset)];
        output += "<path id=\"fill-";
        output += fill_layer.id.to_string();
        output += "\" data-upper-curve-layer-id=\"";
        output += region.upper_curve_layer_id.to_string();
        output += "\" data-lower-curve-layer-id=\"";
        output += region.lower_curve_layer_id.to_string();
        output += "\" data-z-order=\"";
        append_integer(output, fill_layer.z_order);
        if (region.pattern_id.is_nil()) {
          output += "\" fill=\"";
          append_color(output, region.fill_color);
          output += "\" fill-opacity=\"";
          append_number(
              output, static_cast<double>(region.fill_color.alpha) / 255.0);
        } else {
          output += "\" fill=\"url(#pat-";
          output += region.pattern_id.to_string();
          output += ")";
        }
        output += "\" d=\"";
        append_fill_ring_path(output, scene, region);
        output += "\"/>";
      }
    }
    // Image layer tiles (rendering.md section 10): each visible tile is a
    // raster object with explicit physical dimensions, DPI and source
    // identity. No pixels are inlined — the host resolves the href on
    // render (ADR 0032).
    for (const auto &image_layer : scene.image_layers()) {
      if (image_layer.track_id != track.id) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < image_layer.tile_count; ++offset) {
        const auto &tile = scene.image_tiles()[static_cast<std::size_t>(
            image_layer.first_tile + offset)];
        output += "<image id=\"image-";
        output += image_layer.id.to_string();
        output += "\" data-image-source-id=\"";
        output += tile.image_source_id.to_string();
        output += "\" data-level=\"";
        append_integer(output, tile.level);
        output += "\" data-row=\"";
        append_integer(output, tile.row);
        output += "\" data-col=\"";
        append_integer(output, tile.col);
        output += "\" data-dpi=\"";
        append_integer(output, tile.dpi);
        output += "\" data-pixel-format=\"";
        append_integer(output, static_cast<std::uint8_t>(tile.pixel_format));
        output += "\" x=\"";
        append_number(output, tile.rect.left.value);
        output += "\" y=\"";
        append_number(output, tile.rect.top.value);
        output += "\" width=\"";
        append_number(output, tile.rect.width.value);
        output += "\" height=\"";
        append_number(output, tile.rect.height.value);
        output += "\" preserveAspectRatio=\"none\" href=\"";
        append_xml_attribute(output, tile.source.uri);
        output += "\"/>";
      }
    }
    // Custom layer primitives (ADR 0018/0046). Each primitive is emitted as
    // the appropriate SVG element — <path> for polylines, <polygon> for
    // triangles/quads, <circle> for symbols — all tagged with the layer,
    // source and primitive-index identity so picks and exports agree. The
    // geometry is the same scene-millimetre data the GL stream walks.
    const auto custom_vertices = scene.custom_vertices();
    for (const auto &custom_layer : scene.custom_layers()) {
      if (custom_layer.track_id != track.id) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < custom_layer.primitive_count;
           ++offset) {
        const auto &primitive =
            scene.custom_primitives()[static_cast<std::size_t>(
                custom_layer.first_primitive + offset)];
        output += "<path data-custom-layer-id=\"";
        output += custom_layer.id.to_string();
        output += "\" data-custom-source-id=\"";
        output += primitive.source_id.to_string();
        output += "\" data-primitive-index=\"";
        append_integer(output, primitive.source_primitive_index);
        output += "\" data-primitive-kind=\"";
        append_integer(output, static_cast<std::uint8_t>(primitive.kind));
        output += "\" data-z-order=\"";
        append_integer(output, custom_layer.z_order);
        if (primitive.kind == CustomPrimitiveKind::polyline) {
          output += "\" fill=\"none\" stroke=\"";
          append_color(output, primitive.color);
          output += "\" stroke-opacity=\"";
          append_number(output,
                        static_cast<double>(primitive.color.alpha) / 255.0);
          output += "\" stroke-width=\"";
          append_number(output, primitive.stroke_width.value);
          if (!primitive.dash_pattern.segments.empty()) {
            output += "\" stroke-dasharray=\"";
            for (std::size_t si = 0;
                 si < primitive.dash_pattern.segments.size(); ++si) {
              if (si > 0) {
                output.push_back(' ');
              }
              append_number(output,
                            primitive.dash_pattern.segments[si].value);
            }
          }
          output += "\" d=\"";
          bool first = true;
          for (std::uint64_t point_offset = 0;
               point_offset < primitive.vertex_count; ++point_offset) {
            const auto &point = custom_vertices[static_cast<std::size_t>(
                primitive.first_vertex + point_offset)];
            if (!first) {
              output.push_back(' ');
            }
            output += point_offset == 0 ? "M " : " L ";
            append_number(output, point.left.value);
            output.push_back(' ');
            append_number(output, point.top.value);
            first = false;
          }
          if (primitive.closed) {
            output += " Z";
          }
          output += "\"/>";
        } else if (primitive.kind == CustomPrimitiveKind::triangle ||
                   primitive.kind == CustomPrimitiveKind::quad) {
          // Triangles and quads are stored as clipped, triangulated geometry
          // (vertex_count vertices in groups of 3). Emit each triangle as a
          // closed sub-path so one <path> covers the whole primitive.
          if (primitive.kind == CustomPrimitiveKind::quad &&
              !primitive.pattern_id.is_nil()) {
            output += "\" fill=\"url(#pat-";
            output += primitive.pattern_id.to_string();
            output += ")\"";
          } else {
            output += "\" fill=\"";
            append_color(output, primitive.color);
            output += "\" fill-opacity=\"";
            append_number(output,
                          static_cast<double>(primitive.color.alpha) / 255.0);
            output += "\"";
          }
          output += " d=\"";
          const auto triangle_count = primitive.vertex_count / 3;
          for (std::uint64_t tri = 0; tri < triangle_count; ++tri) {
            if (tri > 0) {
              output.push_back(' ');
            }
            for (std::uint64_t point_offset = 0; point_offset < 3;
                 ++point_offset) {
              const auto &point = custom_vertices[static_cast<std::size_t>(
                  primitive.first_vertex + tri * 3 + point_offset)];
              output += point_offset == 0 ? "M " : " L ";
              append_number(output, point.left.value);
              output.push_back(' ');
              append_number(output, point.top.value);
            }
            output += " Z";
          }
          output += "\"/>";
        } else {
          // Symbol: the kind's real geometry — the SAME shape the
          // symbol-layer emitter and the GL backend render (issue #485:
          // non-circle kinds used to degrade to a circle in SVG only).
          const auto &center = custom_vertices[static_cast<std::size_t>(
              primitive.first_vertex)];
          char fill_opacity[32];
          std::snprintf(fill_opacity, sizeof(fill_opacity), "%.6g",
                        static_cast<double>(primitive.color.alpha) / 255.0);
          append_symbol_shape(output, primitive.symbol_kind,
                              center.left.value, center.top.value,
                              primitive.bounds.width.value * 0.5,
                              primitive.color, fill_opacity);
        }
      }
    }
    const auto glyphs = scene.glyphs();
    for (const auto &run : scene.text_runs()) {
      const auto run_track = scene.track_id_for_layer(run.layer_id);
      if (!run_track.has_value() || *run_track != track.id) {
        continue;
      }
      output += "<g id=\"run-";
      output += run.source_entity_id.to_string();
      output += "\" data-layer-id=\"";
      output += run.layer_id.to_string();
      output += "\" data-orientation=\"";
      append_integer(output, static_cast<std::uint8_t>(run.orientation));
      output += "\" fill=\"";
      append_color(output, run.color);
      output += "\" fill-opacity=\"";
      append_number(output, static_cast<double>(run.color.alpha) / 255.0);
      output += "\">";
      for (std::uint64_t offset = 0; offset < run.glyph_count; ++offset) {
        const auto &glyph = glyphs[static_cast<std::size_t>(run.first_glyph +
                                                            offset)];
        output += "<use href=\"#g";
        append_integer(output, glyph.font_index);
        output.push_back('-');
        append_integer(output, glyph.glyph_id);
        output += "\" transform=\"translate(";
        append_number(output, glyph.origin.left.value);
        output.push_back(' ');
        append_number(output, glyph.origin.top.value);
        output += ") rotate(";
        append_number(output, glyph.rotation_degrees);
        output += ") scale(";
        append_number(output, run.font_size.value);
        output += " -";
        append_number(output, run.font_size.value);
        output += ")\"/>";
      }
      output += "</g>";
    }
    output += "</g>";
  }
}

} // namespace

// Bridges from the paginated exporter to the anonymous-namespace emitters above
// (svg_internal.hpp). append_defs / append_layer_body CANNOT be defined directly
// in svg_internal the way the atoms can: their bodies call SvgExporter-private
// composites (append_rect, append_pattern_definition, append_outline_path_data,
// append_symbol, append_path_data, append_fill_ring_path) that must keep
// internal linkage as the correct idiom for translation-unit-private code. So
// these two are a deliberate linkage adapter (internal -> external), not a
// middle-man re-export — they let pagination.cpp reach TU-private emitters it
// otherwise could not name (ADR 0048).
namespace svg_internal {
void append_defs(std::string &output, const PreparedScene &scene) {
  welllog::append_defs(output, scene);
}
void append_layer_body(std::string &output, const PreparedScene &scene) {
  welllog::append_layer_body(output, scene);
}
} // namespace svg_internal

struct SvgDocument::Impl {
  std::string text;
};

SvgDocument::SvgDocument() = default;
SvgDocument::~SvgDocument() = default;
SvgDocument::SvgDocument(const SvgDocument &) = default;
SvgDocument &SvgDocument::operator=(const SvgDocument &) = default;
SvgDocument::SvgDocument(SvgDocument &&) noexcept = default;
SvgDocument &SvgDocument::operator=(SvgDocument &&) noexcept = default;

SvgDocument::SvgDocument(std::string text)
    : impl_(std::make_shared<Impl>(Impl{.text = std::move(text)})) {}

std::string_view SvgDocument::text() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->text};
}

Result<SvgDocument> SvgExporter::write(const PreparedScene &scene) noexcept {
  try {
    if (scene.document_id().is_nil() || scene.document_revision().value == 0 ||
        !std::isfinite(scene.physical_width().value) ||
        scene.physical_width().value <= 0.0 ||
        !std::isfinite(scene.physical_height().value) ||
        scene.physical_height().value <= 0.0 || scene.tracks().empty()) {
      return svg_error(ErrorCode::invalid_presentation,
                       MessageKey::presentation_invalid);
    }

    std::string output;
    output.reserve(1024 + scene.curve_points().size() * 32);
    output += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"";
    append_number(output, scene.physical_width().value);
    output += "mm\" height=\"";
    append_number(output, scene.physical_height().value);
    output += "mm\" viewBox=\"0 0 ";
    append_number(output, scene.physical_width().value);
    output.push_back(' ');
    append_number(output, scene.physical_height().value);
    output += "\" data-document-id=\"";
    output += scene.document_id().to_string();
    output += "\" data-document-revision=\"";
    append_integer(output, scene.document_revision().value);
    output += "\" data-font-asset=\"";
    append_xml_attribute(output, scene.font_asset_fingerprint());
    output += "\">";

    append_defs(output, scene);
    append_layer_body(output, scene);
    output += "</svg>";
    return SvgDocument{std::move(output)};
  } catch (const std::bad_alloc &) {
    return svg_error(ErrorCode::resource_exhausted,
                     MessageKey::resource_exhausted);
  } catch (...) {
    return svg_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

} // namespace welllog
