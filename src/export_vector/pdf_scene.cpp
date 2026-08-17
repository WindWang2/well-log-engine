// PDF scene emission (#187 vector primitives + text-as-outlines; #188 raster
// images, tiling patterns, pagination, custom layer). Serializes a
// PreparedScene + ExportSnapshot to one or more PDF pages via the #185 writer.
// The geometry emission mirrors src/export_vector/svg.cpp's append_layer_body /
// append_pattern_definition 1:1 — same per-track structure, same per-layer order
// (intervals, markers, symbols, curves, crossover fills, images, custom,
// text), same scene-millimetre coordinates — but emits PDF path operators
// (`m/l/c/h/re/f/S`) instead of SVG elements.
//
// One `cm` (concat-matrix) operator per page maps the scaled scene (mm) into
// PDF user-space points, so the per-layer code operates in millimetres exactly
// like the SVG emitter. Track clipping uses `re ... W n` per track (mirroring
// SVG's clipPath); each fixed page adds a depth-window clip (mirroring
// pagination.cpp's page-window clipPath). Text is rendered as glyph vector
// outlines under a per-glyph `cm` (translate/rotate/scale), no font embedded
// (ADR 0047). Raster images embed as image XObjects (pixels fetched via the
// host image_tile resolver — the engine never decodes), and PatternDefinition
// maps to PDF tiling patterns, phase-consistent with the SVG backend.
//
// Determinism is by construction (no CreationDate/ModDate/ID); identical input
// always yields byte-identical output.

#include <welllog/export/pdf_scene.hpp>

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>
#include <welllog/export/export_layout.hpp>
#include <welllog/export/export_report.hpp>
#include <welllog/scene/axis_ticks.hpp>
#include <welllog/io/manifest.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <deque>
#include <vector>

namespace welllog {
namespace {

// 1 inch = 25.4 mm (ADR 0039 unit conversion; never uses screen DPI).
constexpr double millimetres_per_inch = 25.4;
// 1 mm = 72/25.4 PDF user-space points.
constexpr double points_per_millimetre = 72.0 / millimetres_per_inch;
// Cubic-Bezier circle-approximation kappa (4 segments → a near-perfect circle).
constexpr double circle_kappa = 0.5522847498307936;

[[nodiscard]] Error
pdf_scene_error(ErrorCode code, MessageKey message) noexcept {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

// Mirrors pagination.cpp::snapshot_is_valid: rejects empty/invalid scenes and
// pages whose printable area is not strictly positive. Kept local (not shared)
// because the PDF exporter links the PDF library, not the SVG/vector library.
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

// Backend-neutral page-layout + tile-clip geometry comes from the shared
// export_layout header (ADR 0047: both backends share one geometric truth).
using export_layout::clip_line_to_tile;
using export_layout::compute_page_windows;
using export_layout::printable_depth_height_mm;
using export_layout::printable_height;
using export_layout::printable_width;
using export_layout::scene_y_to_depth;

// Formats a double with the engine's deterministic shortest-round-trip
// representation (mirrors pdf.cpp's append_number / svg_internal's
// append_number), into a string. Used for the synthesized band labels.
void append_number(std::string &out, double value) {
  if (value == 0.0) {
    out.push_back('0');
    return;
  }
  std::array<char, 48> buffer{};
  // Shortest round-trip representation (no explicit precision).
  const auto res =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (res.ec == std::errc{}) {
    out.append(buffer.data(), res.ptr);
  } else {
    out.push_back('0');
  }
}
void append_integer(std::string &out, std::int64_t value) {
  std::array<char, 24> buffer{};
  const auto res =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (res.ec == std::errc{}) {
    out.append(buffer.data(), res.ptr);
  }
}

// Sets the non-stroking (fill) colour + alpha for a solid RgbaColor fill: RGB
// always, plus a `gs /GSn` when the colour is semi-transparent (PDF has no
// inline opacity). Shared by interval/fill-region/custom-fill emission.
void set_solid_fill(PdfPathStream &stream, RgbaColor color) noexcept {
  stream.set_fill_color(color.red, color.green, color.blue);
  if (color.alpha < 255) {
    stream.set_fill_alpha(static_cast<double>(color.alpha) / 255.0);
  }
}

// Emits a circle as four cubic Bezier segments approximating a full circle of
// the given radius centred at (cx, cy). Mirrors the SVG emitter's two-arc
// circle but in PDF's cubic-only vocabulary. Built as OutlineCommands with
// cubic_to (so no quadratic→cubic lift occurs) and appended in one call.
void emit_circle_path(PdfPathStream &stream, double cx, double cy,
                      double radius) noexcept {
  const auto k = radius * circle_kappa;
  // Start at (cx+r, cy) and walk the four quadrant control points clockwise
  // (scene y-down). Each quadrant contributes one cubic `c`; a close makes the
  // fill a clean loop.
  const std::array<OutlineCommand, 5> commands{{
      // (cx+r,cy) → (cx,cy+r): control points (cx+r, cy+k), (cx+k, cy+r).
      {OutlineVerb::cubic_to, {cx + radius, cy + k, cx + k, cy + radius,
                               cx, cy + radius}},
      // (cx,cy+r) → (cx-r,cy): (cx-k, cy+r), (cx-r, cy+k).
      {OutlineVerb::cubic_to, {cx - k, cy + radius, cx - radius, cy + k,
                               cx - radius, cy}},
      // (cx-r,cy) → (cx,cy-r): (cx-r, cy-k), (cx-k, cy-r).
      {OutlineVerb::cubic_to, {cx - radius, cy - k, cx - k, cy - radius,
                               cx, cy - radius}},
      // (cx,cy-r) → (cx+r,cy): (cx+k, cy-r), (cx+r, cy-k).
      {OutlineVerb::cubic_to, {cx + k, cy - radius, cx + radius, cy - k,
                               cx + radius, cy}},
      {OutlineVerb::close, {}},
  }};
  stream.move_to(cx + radius, cy);
  stream.append_outline(commands);
}

void emit_arch_path(PdfPathStream &stream, double cx, double cy,
                    double radius) noexcept {
  // Casing-shoe arch: flat side up, bulge down (scene y-down). Two cubics
  // from (cx-r,cy) through (cx,cy+r) to (cx+r,cy) + close.
  const auto k = radius * circle_kappa;
  const std::array<OutlineCommand, 3> commands{{
      {OutlineVerb::cubic_to, {cx - radius, cy + k, cx - k, cy + radius,
                               cx, cy + radius}},
      {OutlineVerb::cubic_to, {cx + k, cy + radius, cx + radius, cy + k,
                               cx + radius, cy}},
      {OutlineVerb::close, {}},
  }};
  stream.move_to(cx - radius, cy);
  stream.append_outline(commands);
}

// A per-page registry of the indirect objects (image XObjects + tiling
// patterns) the page's content stream references. Patterns are keyed by their
// PatternDefinition id so the same pattern referenced by many intervals shares
// one object; images are keyed by (image_source_id, level, row, col). Names are
// assigned in first-encountered order (P0, P1, … / Im0, Im1, …) which is
// deterministic given deterministic scene traversal.
struct PageResources {
  // id → (local_name, pattern definition index in scene.patterns()).
  std::unordered_map<EntityId, std::string, EntityIdHash> pattern_names;
  std::vector<EntityId> pattern_order; // first-encountered order
  // tile-key → local_name.
  struct ImageKey {
    EntityId source_id;
    std::uint32_t level;
    std::uint32_t row;
    std::uint32_t col;
    bool operator==(const ImageKey &) const = default;
  };
  struct ImageKeyHash {
    std::size_t operator()(const ImageKey &k) const noexcept {
      const EntityIdHash id_hash;
      return id_hash(k.source_id) ^
             (static_cast<std::size_t>(k.level) << 5) ^
             (static_cast<std::size_t>(k.row) << 11) ^
             (static_cast<std::size_t>(k.col) << 17);
    }
  };
  std::unordered_map<ImageKey, std::string, ImageKeyHash> image_names;
  std::vector<ImageKey> image_order;
  // Resolved pixel data for each image, keyed by local name. SharedOwner keeps
  // the decoded bytes alive until the PDF is assembled.
  struct ImageRecord {
    std::uint32_t width_px{};
    std::uint32_t height_px{};
    PixelFormat pixel_format{PixelFormat::rgba8};
    const std::uint8_t *data{nullptr};
    SharedOwner owner;
  };
  std::unordered_map<std::string, ImageRecord> image_records;

  // Resolves (or creates) the local name for a pattern id, returning "P<n>".
  std::string name_for_pattern(EntityId pattern_id) {
    const auto it = pattern_names.find(pattern_id);
    if (it != pattern_names.end()) {
      return it->second;
    }
    std::string name = "P";
    name += std::to_string(pattern_order.size());
    pattern_names.emplace(pattern_id, name);
    pattern_order.push_back(pattern_id);
    return name;
  }
  // Resolves (or creates) the local name for an image tile, returning "Im<n>".
  std::string name_for_image(EntityId source_id, std::uint32_t level,
                             std::uint32_t row, std::uint32_t col) {
    const ImageKey key{source_id, level, row, col};
    const auto it = image_names.find(key);
    if (it != image_names.end()) {
      return it->second;
    }
    std::string name = "Im";
    name += std::to_string(image_order.size());
    image_names.emplace(key, name);
    image_order.push_back(key);
    return name;
  }
  // Records the resolved pixels for a named tile (idempotent on the name).
  void record_image(const std::string &name, const RasterTile &raster) {
    if (image_records.find(name) != image_records.end()) {
      return;
    }
    image_records.emplace(
        name, ImageRecord{.width_px = raster.width_px,
                          .height_px = raster.height_px,
                          .pixel_format = raster.pixel_format,
                          .data = raster.data,
                          .owner = raster.owner});
  }
};

// Appends a tile-local line to a pattern's content stream, clipped to the tile
// rect (Liang-Barsky) and stroked with the pattern foreground — the PDF
// equivalent of svg.cpp::append_tile_line.
void append_tile_line(PdfPathStream &stream, PhysicalPoint from,
                      PhysicalPoint to,
                      const PatternDefinition &pattern) noexcept {
  const auto clipped = clip_line_to_tile(from, to, pattern.tile_width.value,
                                         pattern.tile_height.value);
  if (!clipped.has_value()) {
    return;
  }
  stream.set_stroke_color(pattern.foreground.red, pattern.foreground.green,
                          pattern.foreground.blue);
  if (pattern.foreground.alpha < 255) {
    stream.set_fill_alpha(static_cast<double>(pattern.foreground.alpha) / 255.0);
  }
  stream.set_line_width(pattern.stroke_width.value);
  stream.move_to(clipped->first.left.value, clipped->first.top.value)
      .line_to(clipped->second.left.value, clipped->second.top.value)
      .stroke();
}

// Builds the content-stream body of one tiling pattern, mirroring svg.cpp's
// append_pattern_definition: the tile background rect (if opaque) + each
// primitive (lines/polylines clipped to the tile, circles). The pattern is
// authored in tile-local millimetres; the Pattern /Matrix carries the
// scene_anchor + rotation so adjacent tiles share phase (ADR 0020), exactly
// like SVG's patternUnits="userSpaceOnUse" x/y + patternTransform.
void emit_pattern_tile_body(PdfPathStream &stream,
                            const PatternDefinition &pattern) noexcept {
  if (pattern.background.alpha > 0) {
    stream.set_fill_color(pattern.background.red, pattern.background.green,
                          pattern.background.blue);
    if (pattern.background.alpha < 255) {
      stream.set_fill_alpha(
          static_cast<double>(pattern.background.alpha) / 255.0);
    }
    stream.rect(0.0, 0.0, pattern.tile_width.value, pattern.tile_height.value)
        .fill();
  }
  for (const auto &primitive : pattern.primitives) {
    if (const auto *line = std::get_if<PatternLine>(&primitive)) {
      append_tile_line(stream, line->from, line->to, pattern);
    } else if (const auto *polyline =
                   std::get_if<PatternPolyline>(&primitive)) {
      for (std::size_t index = 0; index + 1 < polyline->points.size(); ++index) {
        append_tile_line(stream, polyline->points[index],
                         polyline->points[index + 1], pattern);
      }
      if (polyline->closed && polyline->points.size() > 2) {
        append_tile_line(stream, polyline->points.back(),
                         polyline->points.front(), pattern);
      }
    } else {
      const auto &circle = std::get<PatternCircle>(primitive);
      stream.set_fill_color(pattern.foreground.red, pattern.foreground.green,
                            pattern.foreground.blue);
      if (circle.filled) {
        if (pattern.foreground.alpha < 255) {
          stream.set_fill_alpha(
              static_cast<double>(pattern.foreground.alpha) / 255.0);
        }
        emit_circle_path(stream, circle.center.left.value,
                         circle.center.top.value, circle.radius.value);
        stream.fill();
      } else {
        stream.set_stroke_color(pattern.foreground.red,
                                pattern.foreground.green, pattern.foreground.blue);
        if (pattern.foreground.alpha < 255) {
          stream.set_fill_alpha(
              static_cast<double>(pattern.foreground.alpha) / 255.0);
        }
        stream.set_line_width(pattern.stroke_width.value);
        emit_circle_path(stream, circle.center.left.value,
                         circle.center.top.value, circle.radius.value);
        stream.stroke();
      }
    }
  }
}

// Builds the full object body for a tiling pattern: dictionary + compressed
// content stream. The /Matrix = R(θ)·T(scene_anchor) pins the tile phase to
// scene coordinates AND rotates the tiling, matching the SVG backend's
// patternUnits="userSpaceOnUse" x/y + patternTransform="rotate(θ)" for ANY
// anchor/rotation (not just θ=0). Returns the body the writer wraps in
// "N 0 obj\n…\nendobj". Returns nullopt on a Flate failure so the caller
// surfaces an error rather than emit a malformed (dict-less) object body. NOT
// noexcept: it allocates (string building, Flate), so a bad_alloc must
// propagate to write()'s catch (→ resource_exhausted) rather than terminate.
std::optional<std::string>
build_pattern_body(const PatternDefinition &pattern) {
  PdfPathStream tile;
  emit_pattern_tile_body(tile, pattern);
  const auto ops = std::string(tile.operators());
  std::string compressed;
  if (!flate_compress_buffer(ops, compressed)) {
    return std::nullopt;
  }
  std::string body;
  body += "<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 ";
  body += "/BBox [0 0 ";
  append_number(body, pattern.tile_width.value);
  body += " ";
  append_number(body, pattern.tile_height.value);
  body += "] /XStep ";
  append_number(body, pattern.tile_width.value);
  body += " /YStep ";
  append_number(body, pattern.tile_height.value);
  // Pattern matrix = R(θ) · T(scene_anchor), so tile phase matches the SVG
  // backend exactly. SVG sets the pattern cell origin to (ax, ay) via x/y, then
  // applies patternTransform="rotate(θ)" to the whole tiling around (0,0): a
  // tile at grid (i,j) lands at R·(ax + i·tw, ay + j·th). PDF pattern space
  // paints the cell at (i·XStep, j·YStep); the /Matrix maps that into page
  // space. To reproduce SVG we need Matrix(p) = R·(ax,ay) + p, i.e. the affine
  // [cos sin −sin cos | (cos·ax − sin·ay, sin·ax + cos·ay)]. (The naive
  // T·R = [.. | (ax,ay)] only matches when θ = 0.) All in millimetres; the page
  // cm maps mm→points.
  const auto theta =
      pattern.rotation_degrees * (std::numbers::pi_v<double> / 180.0);
  const auto cos_t = std::cos(theta);
  const auto sin_t = std::sin(theta);
  const auto ax = pattern.scene_anchor.left.value;
  const auto ay = pattern.scene_anchor.top.value;
  body += " /Matrix [";
  append_number(body, cos_t);
  body += " ";
  append_number(body, sin_t);
  body += " ";
  append_number(body, -sin_t);
  body += " ";
  append_number(body, cos_t);
  body += " ";
  append_number(body, cos_t * ax - sin_t * ay);
  body += " ";
  append_number(body, sin_t * ax + cos_t * ay);
  body += "] /Resources << >> /Length ";
  append_integer(body, static_cast<std::int64_t>(compressed.size()));
  body += " /Filter /FlateDecode >>\nstream\n";
  body.append(compressed.data(), compressed.size());
  body += "\nendstream";
  return body;
}

// Builds the image XObject indirect object: dictionary + the pixel stream
// (Flate-compressed). Colourspace maps PixelFormat → DeviceRGB/DeviceGray.
// RGBA ships its alpha plane as a DeviceGray /SMask child XObject (#476), so
// transparent pixels composite in the PDF exactly like on screen. DPI is encoded by the placement `cm` (physical rect vs
// pixel count), consistent with SVG's width/height and recoverable for the
// test. Returns nullopt on a Flate failure so the caller surfaces an error. NOT
// noexcept: it allocates (string + Flate), so bad_alloc propagates to write()'s
// catch (→ resource_exhausted) rather than terminating.
std::optional<PdfIndirectObject>
build_image_object(const PageResources::ImageRecord &rec) {
  const bool is_rgba = rec.pixel_format == PixelFormat::rgba8;
  const std::uint32_t channels = is_rgba || rec.pixel_format == PixelFormat::rgb8
                                     ? 3 : 1;
  const auto pixels_in =
      static_cast<std::uint64_t>(rec.width_px) *
      static_cast<std::uint64_t>(rec.height_px);
  const std::uint32_t in_channels = is_rgba ? 4
      : rec.pixel_format == PixelFormat::rgb8 ? 3 : 1;
  // Split colour and (for RGBA) alpha channels. The alpha plane ships as a
  // grayscale /SMask XObject child object (#476) so PDF exports composite
  // transparent pixels exactly like the on-screen rendering; the writer
  // numbers the child immediately after the image and substitutes the
  // @@CHILD0@@ placeholder with that number.
  std::string pixels;
  pixels.reserve(pixels_in * channels);
  std::string alpha;
  if (is_rgba) {
    alpha.reserve(pixels_in);
  }
  for (std::uint64_t i = 0; i < pixels_in; ++i) {
    const auto base = i * in_channels;
    for (std::uint32_t c = 0; c < channels; ++c) {
      pixels.push_back(static_cast<char>(rec.data[base + c]));
    }
    if (is_rgba) {
      alpha.push_back(static_cast<char>(rec.data[base + channels]));
    }
  }
  std::string compressed;
  if (!flate_compress_buffer(pixels, compressed)) {
    return std::nullopt;
  }
  PdfIndirectObject object;
  std::string &body = object.body;
  body += "<< /Type /XObject /Subtype /Image /Width ";
  append_integer(body, static_cast<std::int64_t>(rec.width_px));
  body += " /Height ";
  append_integer(body, static_cast<std::int64_t>(rec.height_px));
  body += " /ColorSpace /";
  body += channels == 1 ? "DeviceGray" : "DeviceRGB";
  body += " /BitsPerComponent 8";
  if (is_rgba) {
    std::string alpha_compressed;
    if (!flate_compress_buffer(alpha, alpha_compressed)) {
      return std::nullopt;
    }
    // The writer substitutes "@@CHILD0@@" with "<n> 0 R" — no suffix here.
    body += " /SMask @@CHILD0@@";
    std::string smask;
    smask += "<< /Type /XObject /Subtype /Image /Width ";
    append_integer(smask, static_cast<std::int64_t>(rec.width_px));
    smask += " /Height ";
    append_integer(smask, static_cast<std::int64_t>(rec.height_px));
    smask += " /ColorSpace /DeviceGray /BitsPerComponent 8 /Length ";
    append_integer(smask, static_cast<std::int64_t>(alpha_compressed.size()));
    smask += " /Filter /FlateDecode >>\nstream\n";
    smask.append(alpha_compressed.data(), alpha_compressed.size());
    smask += "\nendstream";
    object.extra_bodies.push_back(std::move(smask));
  }
  body += " /Length ";
  append_integer(body, static_cast<std::int64_t>(compressed.size()));
  body += " /Filter /FlateDecode >>\nstream\n";
  body.append(compressed.data(), compressed.size());
  body += "\nendstream";
  object.kind = PdfObjectKind::image;
  return object;
}

// Emits one prepared symbol as PDF path operators, mirroring svg.cpp's
// append_symbol geometry (circle/diamond/square/triangle/cross).
void emit_symbol(PdfPathStream &stream, const PreparedSymbol &symbol,
                 const PreparedSymbolLayer &layer) noexcept {
  const auto half = layer.symbol_size.value / 2.0;
  const auto cx = symbol.center.left.value;
  const auto cy = symbol.center.top.value;
  stream.set_fill_color(layer.color.red, layer.color.green, layer.color.blue);
  stream.set_stroke_color(layer.color.red, layer.color.green, layer.color.blue);
  if (layer.color.alpha < 255) {
    stream.set_fill_alpha(static_cast<double>(layer.color.alpha) / 255.0);
  }
  switch (symbol.kind) {
  case SymbolKind::circle: {
    emit_circle_path(stream, cx, cy, half);
    stream.fill();
    return;
  }
  case SymbolKind::cross: {
    // Two diagonal strokes, no fill (stroke-only).
    stream.set_line_width(layer.symbol_size.value / 6.0);
    stream.move_to(cx - half, cy - half)
        .line_to(cx + half, cy + half)
        .move_to(cx + half, cy - half)
        .line_to(cx - half, cy + half)
        .stroke();
    return;
  }
  case SymbolKind::square:
    stream.move_to(cx - half, cy - half)
        .line_to(cx + half, cy - half)
        .line_to(cx + half, cy + half)
        .line_to(cx - half, cy + half)
        .close()
        .fill();
    return;
  case SymbolKind::triangle_up:
    stream.move_to(cx, cy - half)
        .line_to(cx + half, cy + half)
        .line_to(cx - half, cy + half)
        .close()
        .fill();
    return;
  case SymbolKind::triangle_down:
    stream.move_to(cx, cy + half)
        .line_to(cx + half, cy - half)
        .line_to(cx - half, cy - half)
        .close()
        .fill();
    return;
  case SymbolKind::shoe:
    // Casing-shoe arch (flat side up, bulge down) via two arcs + close.
    emit_arch_path(stream, cx, cy, half);
    stream.fill();
    return;
  case SymbolKind::diamond:
    stream.move_to(cx, cy - half)
        .line_to(cx + half, cy)
        .line_to(cx, cy + half)
        .line_to(cx - half, cy)
        .close()
        .fill();
    return;
  }
}

// Emits the marker-semantic symbol glyph at the left end of a marker line
// (shape semantics from scene::symbol_for_marker_semantic).
void emit_marker_symbol(PdfPathStream &stream, const PreparedMarker &marker,
                        const PreparedMarkerLayer &layer, double left) noexcept {
  const auto kind = symbol_for_marker_semantic(marker.semantic);
  const auto half = layer.symbol_size.value / 2.0;
  const auto cx = left + 1.0 + half;
  const auto cy = marker.display_top.value;
  stream.set_fill_color(layer.line_color.red, layer.line_color.green,
                        layer.line_color.blue);
  stream.set_stroke_color(layer.line_color.red, layer.line_color.green,
                          layer.line_color.blue);
  switch (kind) {
  case SymbolKind::circle:
    emit_circle_path(stream, cx, cy, half);
    stream.fill();
    return;
  case SymbolKind::cross:
    stream.set_line_width(layer.symbol_size.value / 6.0);
    stream.move_to(cx - half, cy - half)
        .line_to(cx + half, cy + half)
        .move_to(cx + half, cy - half)
        .line_to(cx - half, cy + half)
        .stroke();
    return;
  case SymbolKind::square:
    stream.move_to(cx - half, cy - half)
        .line_to(cx + half, cy - half)
        .line_to(cx + half, cy + half)
        .line_to(cx - half, cy + half)
        .close()
        .fill();
    return;
  case SymbolKind::triangle_up:
    stream.move_to(cx, cy - half)
        .line_to(cx + half, cy + half)
        .line_to(cx - half, cy + half)
        .close()
        .fill();
    return;
  case SymbolKind::triangle_down:
    stream.move_to(cx, cy + half)
        .line_to(cx + half, cy - half)
        .line_to(cx - half, cy - half)
        .close()
        .fill();
    return;
  case SymbolKind::diamond:
    stream.move_to(cx, cy - half)
        .line_to(cx + half, cy)
        .line_to(cx, cy + half)
        .line_to(cx - half, cy)
        .close()
        .fill();
    return;
  case SymbolKind::shoe:
    emit_arch_path(stream, cx, cy, half);
    stream.fill();
    return;
  }
}

// Emits one curve layer's polyline(s) as `m`/`l` segments + a single stroke,
// mirroring svg.cpp::append_path_data. The polyline may have multiple segments
// (log-scale gaps, nulls); each segment is a separate subpath so the gaps do
// not connect.
void emit_curve_layer(PdfPathStream &stream, const PreparedScene &scene,
                      const PreparedCurveLayer &layer,
                      const export_layout::PageWindow *window) noexcept {
  if (!layer.visible || layer.segment_count == 0) {
    return;
  }
  stream.set_stroke_color(layer.color.red, layer.color.green, layer.color.blue);
  stream.set_line_width(layer.line_width.value);
  const auto segments = scene.curve_segments();
  const auto points = scene.curve_points();
  bool emitted = false;
  for (std::uint64_t offset = 0; offset < layer.segment_count; ++offset) {
    const auto &segment = segments[static_cast<std::size_t>(
        layer.first_segment + offset)];
    if (window == nullptr || !window->clip) {
      for (std::uint64_t point_offset = 0; point_offset < segment.point_count;
           ++point_offset) {
        const auto &point = points[static_cast<std::size_t>(
            segment.first_point + point_offset)];
        if (point_offset == 0) {
          stream.move_to(point.position.left.value, point.position.top.value);
        } else {
          stream.line_to(point.position.left.value, point.position.top.value);
        }
        emitted = true;
      }
      continue;
    }
    bool subpath_open = false;
    for (std::uint64_t point_offset = 0; point_offset + 1 < segment.point_count;
         ++point_offset) {
      const auto &from =
          points[static_cast<std::size_t>(segment.first_point + point_offset)];
      const auto &to = points[static_cast<std::size_t>(segment.first_point +
                                                       point_offset + 1)];
      if (!export_layout::edge_intersects_window(
              window, from.position.top.value, to.position.top.value)) {
        subpath_open = false;
        continue;
      }
      if (!subpath_open) {
        stream.move_to(from.position.left.value, from.position.top.value);
        subpath_open = true;
      }
      stream.line_to(to.position.left.value, to.position.top.value);
      emitted = true;
    }
  }
  if (emitted) {
    stream.stroke();
  }
}

// Walks a region's closed boundary ring into the stream (m/l), shared by the
// solid and patterned fill paths. The caller closes + paints.
void append_fill_ring(PdfPathStream &stream, const PreparedScene &scene,
                      const PreparedFillRegion &region) noexcept {
  const auto vertices = scene.fill_vertices();
  for (std::uint64_t offset = 0; offset < region.vertex_count; ++offset) {
    const auto &vertex = vertices[static_cast<std::size_t>(
        region.first_vertex + offset)];
    if (offset == 0) {
      stream.move_to(vertex.position.left.value, vertex.position.top.value);
    } else {
      stream.line_to(vertex.position.left.value, vertex.position.top.value);
    }
  }
}

// Emits the closed boundary ring of one crossover-fill region, mirroring
// svg.cpp's solid-vs-pattern decision: solid fill when pattern_id is nil, else
// a tiling-pattern fill (registering the pattern in `resources`).
void emit_fill_region(PdfPathStream &stream, const PreparedScene &scene,
                      const PreparedFillRegion &region,
                      PageResources &resources,
                      const export_layout::PageWindow *window) noexcept {
  if (!export_layout::range_intersects_window(
          window, region.bounds.top.value,
          region.bounds.top.value + region.bounds.height.value)) {
    return;
  }
  append_fill_ring(stream, scene, region);
  if (region.pattern_id.is_nil()) {
    set_solid_fill(stream, region.fill_color);
    stream.close().fill();
  } else {
    // Pattern fill: switch the non-stroking colour space to /Pattern and paint
    // with the tiling pattern. The page Resources names it.
    const auto name = resources.name_for_pattern(region.pattern_id);
    stream.close();
    stream.set_pattern_fill(name);
  }
}

// Emits one text run as glyph vector outlines: each glyph is drawn under a
// per-glyph `cm` that composes translate(origin)·rotate(rotation)·scale(fs,-fs)
// — the same transform the SVG `<use>` applies. The glyph's OutlineCommand
// stream (em fractions, y-up) is emitted verbatim; the negative-d scale flips
// it to scene y-down and sizes it in millimetres. No font program is embedded.
void emit_text_run(PdfPathStream &stream, const PreparedScene &scene,
                   const PreparedTextRun &run,
                   const export_layout::PageWindow *window) noexcept {
  const auto glyphs = scene.glyphs();
  const auto outlines = scene.glyph_outlines();
  const auto outline_commands = scene.outline_commands();
  const auto fs = run.font_size.value;
  if (run.bounds.width.value > 0.0 || run.bounds.height.value > 0.0) {
    if (!export_layout::range_intersects_window(
            window, run.bounds.top.value,
            run.bounds.top.value + run.bounds.height.value, fs)) {
      return;
    }
  }
  stream.save_state();
  stream.set_fill_color(run.color.red, run.color.green, run.color.blue);
  if (run.color.alpha < 255) {
    stream.set_fill_alpha(static_cast<double>(run.color.alpha) / 255.0);
  }
  // (font_index, glyph_id) -> outline lookup, built once per run instead of a
  // linear scan per glyph (O(glyphs x outlines) -> O(glyphs + outlines),
  // issue #487). The outlines span is the single source of truth shared with
  // the SVG backend.
  std::unordered_map<std::uint64_t, const PreparedGlyphOutline *> outline_by_key;
  outline_by_key.reserve(outlines.size());
  for (const auto &candidate : outlines) {
    const auto key = (static_cast<std::uint64_t>(candidate.font_index) << 32U) |
                     candidate.glyph_id;
    outline_by_key.emplace(key, &candidate);
  }
  for (std::uint64_t offset = 0; offset < run.glyph_count; ++offset) {
    const auto &glyph = glyphs[static_cast<std::size_t>(run.first_glyph +
                                                         offset)];
    if (!export_layout::y_intersects_window(window, glyph.origin.top.value,
                                            fs)) {
      continue;
    }
    const auto key = (static_cast<std::uint64_t>(glyph.font_index) << 32U) |
                     glyph.glyph_id;
    const auto outline_it = outline_by_key.find(key);
    const PreparedGlyphOutline *outline =
        outline_it == outline_by_key.end() ? nullptr : outline_it->second;
    if (outline == nullptr || outline->command_count == 0) {
      continue;
    }
    // Per-GLYPH rotation, not per-run: in vertical typesetting rotated glyphs
    // carry their own 90° glyph rotation while upright ones carry 0°, so this
    // must read glyph.rotation_degrees (matching SVG's `rotate(glyph.rotation)`)
    // rather than run.rotation_degrees.
    const auto theta =
        glyph.rotation_degrees * (std::numbers::pi_v<double> / 180.0);
    const auto cos_t = std::cos(theta);
    const auto sin_t = std::sin(theta);
    // M = translate(ox,oy) · rotate(θ) · scale(fs,-fs) as an affine [a b c d e f]
    // (derived in the header comment): a=cos·fs, b=sin·fs, c=sin·fs, d=-cos·fs.
    stream.save_state();
    stream.concat_matrix(cos_t * fs, sin_t * fs, sin_t * fs, -cos_t * fs,
                         glyph.origin.left.value, glyph.origin.top.value);
    stream.append_outline(outline_commands.subspan(
        static_cast<std::size_t>(outline->first_command),
        static_cast<std::size_t>(outline->command_count)));
    stream.fill();
    stream.restore_state();
  }
  stream.restore_state();
}

// Shapes `text` and returns its width in millimetres at font size `fs` (sum of
// glyph em-advances × fs). Used to right-align band labels before emitting.
// Returns 0 when the engine is null or shaping fails.
double measure_text_mm(TextEngine *text_engine, std::string_view text,
                       double fs) noexcept {
  if (text_engine == nullptr || text.empty()) {
    return 0.0;
  }
  const auto run = text_engine->shape(TextShapeRequest{
      .text = text, .language = "en", .direction = TextDirection::left_to_right});
  if (!run.has_value()) {
    return 0.0;
  }
  double width_em = 0.0;
  for (const auto &glyph : run.value().glyphs) {
    width_em += glyph.advance_x;
  }
  return width_em * fs;
}

// Emits a synthesized text STRING as glyph outlines at a page-millimetre pen
// origin (px, py), horizontal (run rotation 0), font size `fs` mm, in `color`.
// Used for the pagination metadata bands (well name, page number, depth range,
// legend mnemonics) — the same no-font policy as scene text (ADR 0047). Must be
// called inside a page-mm (y-flipped) `cm` so the per-glyph placement
// [fs 0 0 -fs px py] lands upright. Outline path no-op when text_engine is null.
// When ``searchable`` is true (B1.PDF.2), also emits Base-14 Helvetica operators
// for printable ASCII so band labels are extractable (ADR 0053).
void emit_text_string(PdfPathStream &stream, TextEngine *text_engine,
                      std::string_view text, double px, double py, double fs,
                      RgbaColor color, bool searchable) noexcept {
  if (text.empty()) {
    return;
  }
  if (text_engine != nullptr) {
    const auto run = text_engine->shape(TextShapeRequest{
        .text = text,
        .language = "en",
        .direction = TextDirection::left_to_right});
    if (run.has_value() && !run.value().glyphs.empty()) {
      stream.save_state();
      stream.set_fill_color(color.red, color.green, color.blue);
      if (color.alpha < 255) {
        stream.set_fill_alpha(static_cast<double>(color.alpha) / 255.0);
      }
      double pen_x = px;
      for (const auto &glyph : run.value().glyphs) {
        const auto outline =
            text_engine->glyph_outline(glyph.font_index, glyph.glyph_id);
        if (outline.has_value() && !outline.value().commands.empty()) {
          // Em-space (y-up) → page-mm (y-down): [fs 0 0 -fs pen_x py].
          stream.save_state();
          stream.concat_matrix(fs, 0.0, 0.0, -fs, pen_x, py);
          stream.append_outline(outline.value().commands);
          stream.fill();
          stream.restore_state();
        }
        pen_x += glyph.advance_x * fs;
      }
      stream.restore_state();
    }
  }
  if (searchable) {
    // Overlay extractable Latin-1 text (B1.PDF.2/3). CJK counted as dropped.
    const auto before = stream.non_latin_codepoints_dropped();
    stream.draw_standard_text(px, py, fs, text);
    // latin_runs tracked by caller via stream stats after bands.
    (void)before;
  }
}

// Emits the pagination metadata bands for one page, mirroring
// pagination.cpp::append_fixed_page's bands (header well-name + page number,
// depth-range footer, legend swatches + mnemonics). `page_index`/`page_count`
// are 1-based/total; `window_top_mm`/`window_bottom_mm` are the scene depth
// window this page shows (for the depth-range footer). Emitted in PAGE-mm space
// (y-down) — must be called inside the page-mm (y-flipped) `cm` established by
// write(). Geometric bands (legend swatches) always emit; outline text needs a
// text engine; searchable Latin overlay (B1.PDF.2) works without a text engine.
void emit_page_bands(PdfPathStream &stream, const PreparedScene &scene,
                     const ExportSnapshot &snapshot, std::uint32_t page_index,
                     std::uint32_t page_count, double window_top_mm,
                     double window_bottom_mm, TextEngine *text_engine,
                     bool searchable_text) noexcept {
  const auto &page = snapshot.page;
  const auto content_left = page.margins.left.value;
  const auto content_top = page.margins.top.value;
  const auto printable_h = printable_height(page);
  const auto label_color = RgbaColor{0, 0, 0, 255};
  const auto band_font_size = 3.0;
  if (page.repeat_headers) {
    if (!page.well_name.empty()) {
      emit_text_string(stream, text_engine, page.well_name, content_left,
                       content_top + band_font_size, band_font_size, label_color,
                       searchable_text);
    }
    if (page.show_page_numbers) {
      std::string label = "page ";
      append_integer(label, static_cast<std::int64_t>(page_index + 1));
      label += " of ";
      append_integer(label, static_cast<std::int64_t>(page_count));
      // Right-align at the right margin: measure, then emit at the offset.
      const auto width =
          measure_text_mm(text_engine, label, band_font_size);
      const auto right_x = page.page_width.value - page.margins.right.value;
      emit_text_string(stream, text_engine, label, right_x - width,
                       content_top + band_font_size, band_font_size,
                       label_color, searchable_text);
    }
  }
  // Depth ruler (Epic B, B4): authoritative ticks in the left margin strip —
  // same scene::nice_axis_ticks / format_axis_tick_label semantics as the SVG
  // backend (pagination.cpp::append_depth_ruler), so both outputs agree.
  if (page.show_depth_ruler) {
    const auto depth_top = scene_y_to_depth(scene, window_top_mm);
    const auto depth_bottom = scene_y_to_depth(scene, window_bottom_mm);
    const auto ticks = welllog::nice_axis_ticks(depth_top, depth_bottom);
    if (!ticks.values.empty()) {
      const double left_edge = page.margins.left.value;
      const double span = depth_bottom - depth_top;
      const double y_span = window_bottom_mm - window_top_mm;
      constexpr double ruler_font = 2.4;
      for (const double value : ticks.values) {
        const double t = (value - depth_top) / span;
        const double y_mm = window_top_mm + t * y_span;
        stream.save_state();
        stream.set_stroke_color(0x33, 0x33, 0x33);
        stream.set_line_width(0.4);
        stream.move_to(left_edge - 1.0, y_mm);
        stream.line_to(left_edge - 3.5, y_mm);
        stream.stroke();
        stream.restore_state();
        emit_text_string(stream, text_engine,
                         welllog::format_axis_tick_label(value, ticks.step), 1.0,
                         y_mm + 0.9, ruler_font, label_color, searchable_text);
      }
    }
  }
  if (page.show_depth_range) {
    const auto depth_top = scene_y_to_depth(scene, window_top_mm);
    const auto depth_bottom = scene_y_to_depth(scene, window_bottom_mm);
    const auto footer_y =
        page.page_height.value - page.margins.bottom.value + band_font_size;
    std::string footer = "depth ";
    append_number(footer, depth_top);
    footer += " .. ";
    append_number(footer, depth_bottom);
    emit_text_string(stream, text_engine, footer, content_left, footer_y,
                     band_font_size, label_color, searchable_text);
  }
  if (page.repeat_legend) {
    const auto headers = scene.track_header_entries();
    double legend_y = content_top + printable_h - band_font_size;
    for (const auto &entry : headers) {
      // Legend colour swatch (pure geometry — always emitted).
      stream.set_fill_color(entry.color.red, entry.color.green, entry.color.blue);
      stream.rect(content_left, legend_y - 2.0, 3.0, 2.0).fill();
      std::string mnemonic = entry.curve_name;
      mnemonic += " ";
      append_number(mnemonic, entry.scale_minimum);
      mnemonic += "..";
      append_number(mnemonic, entry.scale_maximum);
      mnemonic += " ";
      mnemonic += entry.unit;
      emit_text_string(stream, text_engine, mnemonic, content_left + 4.0,
                       legend_y, band_font_size, label_color, searchable_text);
      legend_y -= 4.0;
    }
  }
}

// Emits the custom-layer primitives of one track, mirroring svg.cpp's custom
// loop. Polylines → stroked path; triangles/quads → filled closed sub-paths
// (triangulated, vertex_count/3 triangles); symbols → filled shape. No
// per-backend clip: the geometry is pre-clipped to the source's clip ring at
// prepare time (the same data GL/SVG draw).
void emit_custom_layer(PdfPathStream &stream, const PreparedScene &scene,
                       const PreparedCustomLayer &layer,
                       PageResources &resources,
                       const export_layout::PageWindow *window) noexcept {
  if (!layer.visible) {
    return;
  }
  const auto custom_vertices = scene.custom_vertices();
  for (std::uint64_t offset = 0; offset < layer.primitive_count; ++offset) {
    const auto &primitive = scene.custom_primitives()[static_cast<std::size_t>(
        layer.first_primitive + offset)];
    if (primitive.bounds.width.value > 0.0 ||
        primitive.bounds.height.value > 0.0) {
      if (!export_layout::range_intersects_window(
              window, primitive.bounds.top.value,
              primitive.bounds.top.value + primitive.bounds.height.value)) {
        continue;
      }
    }
    if (primitive.kind == CustomPrimitiveKind::polyline) {
      stream.set_stroke_color(primitive.color.red, primitive.color.green,
                              primitive.color.blue);
      stream.set_line_width(primitive.stroke_width.value);
      if (!primitive.dash_pattern.segments.empty()) {
        std::vector<double> dash_array;
        dash_array.reserve(primitive.dash_pattern.segments.size());
        for (const auto &seg : primitive.dash_pattern.segments) {
          dash_array.push_back(seg.value);
        }
        stream.set_dash(dash_array, primitive.dash_pattern.offset);
      }
      for (std::uint64_t point_offset = 0;
           point_offset < primitive.vertex_count; ++point_offset) {
        const auto &point = custom_vertices[static_cast<std::size_t>(
            primitive.first_vertex + point_offset)];
        if (point_offset == 0) {
          stream.move_to(point.left.value, point.top.value);
        } else {
          stream.line_to(point.left.value, point.top.value);
        }
      }
      if (primitive.closed) {
        stream.close();
      }
      stream.stroke();
      if (!primitive.dash_pattern.segments.empty()) {
        // Reset to solid so the dash doesn't leak to subsequent strokes.
        stream.set_dash({}, 0.0);
      }
    } else if (primitive.kind == CustomPrimitiveKind::triangle ||
               primitive.kind == CustomPrimitiveKind::quad) {
      if (primitive.kind == CustomPrimitiveKind::quad &&
          !primitive.pattern_id.is_nil()) {
        stream.set_pattern_fill(
            resources.name_for_pattern(primitive.pattern_id));
      } else {
        set_solid_fill(stream, primitive.color);
      }
      const auto triangle_count = primitive.vertex_count / 3;
      for (std::uint64_t tri = 0; tri < triangle_count; ++tri) {
        for (std::uint64_t point_offset = 0; point_offset < 3; ++point_offset) {
          const auto &point = custom_vertices[static_cast<std::size_t>(
              primitive.first_vertex + tri * 3 + point_offset)];
          if (point_offset == 0) {
            stream.move_to(point.left.value, point.top.value);
          } else {
            stream.line_to(point.left.value, point.top.value);
          }
        }
        stream.close();
      }
      stream.fill();
    } else {
      // Symbol. SVG currently defers non-circle kinds to a circle (svg.cpp
      // "Full SVG parity for every symbol kind is deferred") — a known SVG
      // limitation. PDF can emit each SymbolKind's true geometry correctly via
      // emit_symbol, so it does rather than inherit SVG's limitation; this is an
      // intentional, documented divergence where PDF is more capable. When SVG
      // gains full symbol parity both backends will agree; until then the
      // custom-layer parity tests use circles, which both render identically.
      PreparedSymbol sym;
      PreparedSymbolLayer slyr;
      const auto &center = custom_vertices[static_cast<std::size_t>(
          primitive.first_vertex)];
      sym.center = center;
      sym.kind = primitive.symbol_kind;
      slyr.color = primitive.color;
      slyr.symbol_size = Millimetres{primitive.bounds.width.value};
      emit_symbol(stream, sym, slyr);
    }
  }
}

// Crop/trim marks (剪切线, FRS §5) at the four printable-area corners, in
// PAGE-mm space (y-down) like emit_page_bands. Geometry matches the SVG
// backend (pagination.cpp append_crop_marks) so both outputs align on the
// printed page.
void emit_crop_marks(PdfPathStream &stream, const ExportPageSpec &page) noexcept {
  constexpr double mark_length_mm = 5.0;
  constexpr double stroke_width_pt = 0.85;  // 0.3 mm at 72 dpi points
  const double w = page.page_width.value;
  const double h = page.page_height.value;
  const double left = page.margins.left.value;
  const double top = page.margins.top.value;
  const double right = w - page.margins.right.value;
  const double bottom = h - page.margins.bottom.value;
  stream.set_stroke_color(0, 0, 0);
  stream.set_line_width(stroke_width_pt);
  const auto mark = [&stream](double x1, double y1, double x2, double y2) {
    stream.move_to(x1, y1).line_to(x2, y2).stroke();
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

// Emits the per-track, per-layer body — the single geometric emitter for one
// track, mirroring svg.cpp::append_layer_body's per-track `<g>`. Called inside
// the track's saved clip state. `resources` collects the patterns/images the
// page will need to name in its Resources dict.
void emit_track_body(PdfPathStream &stream, const PreparedScene &scene,
                     const PreparedTrack &track, PageResources &resources,
                     const std::function<Result<RasterTile>(
                         const ImageTileRequest &)> &image_tile,
                     const export_layout::PageWindow *window) noexcept {
  // Interval rects (solid or patterned fill — mirrors SVG's pattern_id branch).
  for (const auto &layer : scene.interval_layers()) {
    if (layer.track_id != track.id) {
      continue;
    }
    for (std::uint64_t offset = 0; offset < layer.interval_count; ++offset) {
      const auto &interval = scene.intervals()[static_cast<std::size_t>(
          layer.first_interval + offset)];
      if (!export_layout::range_intersects_window(
              window, interval.rect.top.value,
              interval.rect.top.value + interval.rect.height.value)) {
        continue;
      }
      stream.rect(interval.rect.left.value, interval.rect.top.value,
                  interval.rect.width.value, interval.rect.height.value);
      if (interval.pattern_id.is_nil()) {
        set_solid_fill(stream, interval.fill_color);
        stream.fill();
      } else {
        stream.set_pattern_fill(resources.name_for_pattern(interval.pattern_id));
      }
    }
  }
  // Marker lines across the full track width.
  for (const auto &layer : scene.marker_layers()) {
    if (layer.track_id != track.id) {
      continue;
    }
    stream.set_stroke_color(layer.line_color.red, layer.line_color.green,
                            layer.line_color.blue);
    stream.set_line_width(layer.line_width.value);
    const auto left = track.clip.left.value;
    const auto right = track.clip.left.value + track.clip.width.value;
    const auto marker_pad = layer.line_width.value +
                            (layer.draw_symbols ? layer.symbol_size.value : 0.0);
    for (std::uint64_t offset = 0; offset < layer.marker_count; ++offset) {
      const auto &marker = scene.markers()[static_cast<std::size_t>(
          layer.first_marker + offset)];
      if (!export_layout::y_intersects_window(window, marker.display_top.value,
                                              marker_pad)) {
        continue;
      }
      stream.move_to(left, marker.display_top.value)
          .line_to(right, marker.display_top.value);
      if (layer.draw_symbols) {
        emit_marker_symbol(stream, marker, layer, left);
      }
    }
    stream.stroke();
  }
  // Symbols.
  for (const auto &layer : scene.symbol_layers()) {
    if (layer.track_id != track.id) {
      continue;
    }
    for (std::uint64_t offset = 0; offset < layer.symbol_count; ++offset) {
      const auto &symbol = scene.symbols()[static_cast<std::size_t>(
          layer.first_symbol + offset)];
      if (!export_layout::y_intersects_window(
              window, symbol.center.top.value, layer.symbol_size.value)) {
        continue;
      }
      emit_symbol(stream, symbol, layer);
    }
  }
  // Curve polylines.
  for (const auto &layer : scene.curve_layers()) {
    if (layer.track_id == track.id) {
      emit_curve_layer(stream, scene, layer, window);
    }
  }
  // Crossover fill regions.
  for (const auto &fill_layer : scene.fill_layers()) {
    if (fill_layer.track_id != track.id) {
      continue;
    }
    for (std::uint64_t offset = 0; offset < fill_layer.region_count; ++offset) {
      const auto &region = scene.fill_regions()[static_cast<std::size_t>(
          fill_layer.first_region + offset)];
      emit_fill_region(stream, scene, region, resources, window);
    }
  }
  // Image layer tiles: place each resolved tile as an image XObject. Pixels are
  // fetched via the host resolver (the engine never decodes); a missing
  // resolver or failed resolution skips the tile (best-effort, like the SVG
  // backend which only emits a placeholder href).
  if (image_tile) {
    for (const auto &image_layer : scene.image_layers()) {
      if (image_layer.track_id != track.id) {
        continue;
      }
      for (std::uint64_t offset = 0; offset < image_layer.tile_count; ++offset) {
        const auto &tile = scene.image_tiles()[static_cast<std::size_t>(
            image_layer.first_tile + offset)];
        if (!export_layout::range_intersects_window(
                window, tile.rect.top.value,
                tile.rect.top.value + tile.rect.height.value)) {
          continue;
        }
        const auto resolved = image_tile(ImageTileRequest{
            .image_source_id = tile.image_source_id,
            .level = tile.level,
            .row = tile.row,
            .col = tile.col,
        });
        if (!resolved.has_value()) {
          continue;
        }
        const auto &raster = resolved.value();
        if (raster.data == nullptr || raster.width_px == 0 ||
            raster.height_px == 0) {
          continue;
        }
        // Register the tile (dedup by source/level/row/col) and place it: a `cm`
        // mapping the image's 1×1 unit space onto the tile's physical rect (in
        // mm, under the page cm), then `Do`. The object body is built later when
        // the page's resources are materialized.
        const auto name = resources.name_for_image(
            tile.image_source_id, tile.level, tile.row, tile.col);
        resources.record_image(name, raster);
        stream.save_state();
        // Map the image's unit square [0,1]×[0,1] (PDF image space is y-UP, row 0
        // at v=1) onto the tile's physical rect in scene mm (y-DOWN), so source
        // row 0 lands at rect.top (the shallow end): a=width, d=−height (flip),
        // e=left, f=top+height. The page cm then maps this scene rect into the
        // flipped page space; the net result is an upright image in the placed
        // rect (mirroring SVG's <image y=top> which puts row 0 at top).
        stream.concat_matrix(tile.rect.width.value, 0.0, 0.0,
                             -tile.rect.height.value, tile.rect.left.value,
                             tile.rect.top.value + tile.rect.height.value);
        stream.invoke_xobject(name);
        stream.restore_state();
      }
    }
  }
  // Custom layer primitives.
  for (const auto &custom_layer : scene.custom_layers()) {
    if (custom_layer.track_id == track.id) {
      emit_custom_layer(stream, scene, custom_layer, resources, window);
    }
  }
  // Text runs (vector outlines).
  for (const auto &run : scene.text_runs()) {
    const auto run_track = scene.track_id_for_layer(run.layer_id);
    if (run_track.has_value() && *run_track == track.id) {
      emit_text_run(stream, scene, run, window);
    }
  }
}

// Materializes one page's collected resources into the PdfIndirectObject list
// the writer appends. Patterns first (P0..), then images (Im0..) — deterministic
// order. Pattern definitions are looked up by id in the scene; image pixel
// records are carried in PageResources. Returns nullopt if any object body
// failed to build (e.g. a Flate error), so write() surfaces an Error rather than
// emit a malformed (dict-less) object that a `Do`/`scn` would dangle-reference.
std::optional<std::vector<PdfIndirectObject>>
materialize_objects(const PreparedScene &scene,
                    const PageResources &resources) {
  std::vector<PdfIndirectObject> objects;
  for (const auto &pattern_id : resources.pattern_order) {
    const PatternDefinition *pattern = nullptr;
    for (const auto &candidate : scene.patterns()) {
      if (candidate.id == pattern_id) {
        pattern = &candidate;
        break;
      }
    }
    if (pattern == nullptr) {
      continue;
    }
    auto body = build_pattern_body(*pattern);
    if (!body.has_value()) {
      return std::nullopt;
    }
    objects.push_back(PdfIndirectObject{
        .body = std::move(*body),
        .kind = PdfObjectKind::pattern,
        .local_name = resources.pattern_names.at(pattern_id),
        .extra_bodies = {},
    });
  }
  for (const auto &key : resources.image_order) {
    const auto name = resources.image_names.at(key);
    const auto rec_it = resources.image_records.find(name);
    if (rec_it == resources.image_records.end()) {
      continue;
    }
    auto object = build_image_object(rec_it->second);
    if (!object.has_value()) {
      return std::nullopt;
    }
    object->local_name = name;
    objects.push_back(std::move(*object));
  }
  return objects;
}

} // namespace

Result<PdfDocument>
PdfSceneExporter::write(const PreparedScene &scene,
                        const ExportSnapshot &snapshot,
                        std::function<Result<RasterTile>(const ImageTileRequest &)>
                            image_tile,
                        TextEngine *text_engine,
                        ExportReport *report,
                        bool searchable_text,
                        SearchableTextStats *searchable_stats) noexcept {
  if (searchable_stats != nullptr) {
    *searchable_stats = SearchableTextStats{};
  }
  try {
    if (!snapshot_is_valid(scene, snapshot)) {
      return pdf_scene_error(ErrorCode::invalid_presentation,
                             MessageKey::presentation_invalid);
    }
    // Criterion 7: evaluate the shared complexity heuristic once. In pure-vector
    // mode (default) an over-budget layer must FAIL rather than silently
    // rasterize; in mixed mode the over-budget layers are recorded in the report
    // (the actual raster path is a documented follow-up). Identical to the SVG
    // backend (pagination.cpp) so both make the same decision for the same input.
    const auto decision = evaluate_complexity(
        scene, snapshot.page.vector_complexity_budget, snapshot.page.dpi);
    if (decision.would_degrade()) {
      if (snapshot.page.export_mode == ExportMode::pure_vector) {
        return pdf_scene_error(ErrorCode::invalid_presentation,
                               MessageKey::presentation_invalid);
      }
      if (report != nullptr) {
        report->degraded_layers.insert(report->degraded_layers.end(),
                                       decision.over_budget.begin(),
                                       decision.over_budget.end());
      }
    }
    const auto &page = snapshot.page;
    const auto scale = printable_width(page) / scene.physical_width().value;
    const auto s_pt = scale * points_per_millimetre;
    const auto margin_left_pt =
        page.margins.left.value * points_per_millimetre;

    // Page windows from the shared page model (identical slicing to the SVG
    // paginated exporter — export_layout::compute_page_windows).
    const auto windows = compute_page_windows(scene, snapshot);
    if (windows.empty()) {
      return pdf_scene_error(ErrorCode::resource_exhausted,
                             MessageKey::resource_exhausted);
    }

    std::vector<PdfPageContent> contents;
    std::vector<PdfPageSpec> specs;
    // deque: push_back never invalidates references to existing elements,
    // so the spans handed to PdfPageContent::objects stay valid without
    // relying on the reserve() matching the final page count.
    std::deque<std::vector<PdfIndirectObject>> object_storage;
    contents.reserve(windows.size());
    specs.reserve(windows.size());

    const auto page_count = static_cast<std::uint32_t>(windows.size());
    for (std::uint32_t page_index = 0; page_index < page_count; ++page_index) {
      const auto &window = windows[page_index];
      PdfPathStream stream;
      // Page cm: map scene-millimetres (sx, sy), y-DOWN with depth increasing
      // downward, into PDF user-space points (x, y), y-UP — so the scene renders
      // top-down (shallow depth at the page top, deep at the bottom), matching
      // the SVG backend and well-log convention. The y-flip is d=−s_pt. The
      // vertical translation pins scene-y=window_top at the printable TOP:
      // f = (window.height_mm − margins.top)·pmm + window_top·s_pt, correct for
      // BOTH symmetric and asymmetric margins (a prior f = mt_pt +
      // window_bottom·s_pt form was wrong for asymmetric margins — hidden by
      // tests using 10/10 all round). e = margin_left_pt.
      stream.concat_matrix(s_pt, 0.0, 0.0, -s_pt, margin_left_pt,
                           (window.height_mm - page.margins.top.value) *
                                   points_per_millimetre +
                               window.window_top_mm * s_pt);

      PageResources resources;
      if (window.clip) {
        // Page depth-window clip in scene mm: the [window_top, window_bottom]
        // band, which the flipped page cm maps onto the printable area.
        stream.save_state();
        stream.rect(0.0, window.window_top_mm, scene.physical_width().value,
                    window.window_bottom_mm - window.window_top_mm)
            .clip_nonzero()
            .end_path_no_paint();
      }
      // Layered PDF (FRS §5): wrap each track's body in an OCG marked-content
      // sequence so viewers can toggle tracks; the page registers the layers
      // it used so the writer can build the per-page /Properties dict.
      std::vector<std::uint32_t> page_layers;
      if (page.layered_pdf) {
        page_layers.reserve(scene.tracks().size());
      }
      for (std::size_t t = 0; t < scene.tracks().size(); ++t) {
        const auto &track = scene.tracks()[t];
        if (page.layered_pdf) {
          stream.mark_begin(static_cast<std::uint32_t>(t));
        }
        stream.save_state();
        stream.rect(track.clip.left.value, track.clip.top.value,
                    track.clip.width.value, track.clip.height.value)
            .clip_nonzero()
            .end_path_no_paint();
        emit_track_body(stream, scene, track, resources, image_tile,
                        window.clip ? &window : nullptr);
        stream.restore_state();
        if (page.layered_pdf) {
          stream.mark_end();
          page_layers.push_back(static_cast<std::uint32_t>(t));
        }
      }
      if (window.clip) {
        stream.restore_state();
      }

      // Pagination metadata bands (header/legend/page-number/depth-range) in
      // PAGE-mm space (y-down), separate from the scene body's flipped cm. A
      // dedicated cm maps page-mm (y-down) → PDF points (y-up): a=pmm, d=−pmm,
      // f=page_height_pt, so band coordinates authored top-down land upright.
      // Emitted on every page (continuous shows the single-page bands too),
      // mirroring pagination.cpp's per-page bands.
      const auto page_height_pt = window.height_mm * points_per_millimetre;
      stream.save_state();
      stream.concat_matrix(points_per_millimetre, 0.0, 0.0,
                           -points_per_millimetre, 0.0, page_height_pt);
      emit_page_bands(stream, scene, snapshot, page_index, page_count,
                      window.window_top_mm, window.window_bottom_mm,
                      text_engine, searchable_text);
      if (page.crop_marks) {
        emit_crop_marks(stream, page);
      }
      stream.restore_state();

      if (searchable_stats != nullptr && searchable_text) {
        searchable_stats->non_latin_codepoints_dropped +=
            stream.non_latin_codepoints_dropped();
        if (stream.needs_standard_font()) {
          searchable_stats->latin_runs_emitted += 1;
        }
      }

      auto objects_opt = materialize_objects(scene, resources);
      if (!objects_opt.has_value()) {
        return pdf_scene_error(ErrorCode::internal_error,
                               MessageKey::internal_error);
      }
      PdfPageContent content{.stream = std::move(stream)};
      if (!page_layers.empty()) {
        content.layer_indices = std::move(page_layers);
      }
      object_storage.push_back(std::move(*objects_opt));
      content.objects = object_storage.back();
      contents.push_back(std::move(content));
      specs.push_back(PdfPageSpec{
          .width_points = page.page_width.value * points_per_millimetre,
          .height_points = window.height_mm * points_per_millimetre,
      });
    }

    // Layered PDF (FRS §5): the global OCG name list, one entry per track,
    // authored as human-readable "track-<index>". Pages reference them via
    // their layer_indices (see the track loop above).
    std::vector<std::string> layer_names;
    if (page.layered_pdf) {
      layer_names.reserve(scene.tracks().size());
      for (std::size_t t = 0; t < scene.tracks().size(); ++t) {
        layer_names.push_back("track-" + std::to_string(t));
      }
    }

    return PdfWriter::write(contents, specs, layer_names);
  } catch (const std::bad_alloc &) {
    return pdf_scene_error(ErrorCode::resource_exhausted,
                           MessageKey::resource_exhausted);
  } catch (...) {
    return pdf_scene_error(ErrorCode::internal_error,
                           MessageKey::internal_error);
  }
}

} // namespace welllog
