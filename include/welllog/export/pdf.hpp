#pragma once

// Minimal hand-rolled PDF writer (ADR: PDF via hand-rolled writer, #185 spike,
// extended by #187 scene emission).
//
// This writer produces a byte-deterministic, Flate-compressed multi-page PDF
// with no third-party PDF library — only zlib for stream compression. It exposes
// the primitives the per-layer scene emission (#187's PdfSceneExporter) needs: a
// path operator stream built from the engine's backend-neutral OutlineCommand
// vocabulary, plus the graphics-state operators (clip, transform, alpha) those
// layers require, and page assembly. Output is deterministic by construction
// (no CreationDate/ModDate/ID).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <welllog/core/result.hpp>
#include <welllog/export/pdf_export.hpp>
#include <welllog/scene/text_engine.hpp>

namespace welllog {

// One flattened PDF content-stream path, in PDF user-space units (points),
// built from the engine's OutlineCommand model. The mapping mirrors the SVG
// path-data emitter (src/export_vector/svg.cpp append_outline_path_data) but
// emits PDF operators and lifts quadratic curves to cubics (PDF has no
// quadratic).
class WELLLOG_EXPORT_PDF_API PdfPathStream {
public:
  PdfPathStream();
  ~PdfPathStream();
  PdfPathStream(const PdfPathStream &);
  PdfPathStream &operator=(const PdfPathStream &);
  PdfPathStream(PdfPathStream &&) noexcept;
  PdfPathStream &operator=(PdfPathStream &&) noexcept;

  // move-to (PDF `m`).
  PdfPathStream &move_to(double x, double y) noexcept;
  // line-to (PDF `l`).
  PdfPathStream &line_to(double x, double y) noexcept;
  // Append an OutlineCommand (em-space, y-up, glyph-local). Coordinates are
  // used verbatim; quadratic_to is lifted to a cubic. close → PDF `h`.
  PdfPathStream &append_outline(std::span<const OutlineCommand> commands,
                                double scale = 1.0,
                                double dx = 0.0, double dy = 0.0) noexcept;
  // Close the current subpath.
  PdfPathStream &close() noexcept;
  // Fill the current path (non-zero winding). PDF `f`.
  PdfPathStream &fill() noexcept;
  // Stroke the current path. PDF `S`.
  PdfPathStream &stroke() noexcept;
  // Set the non-stroking (fill) colour, sRGB 0–255.
  PdfPathStream &set_fill_color(std::uint8_t r, std::uint8_t g,
                                std::uint8_t b) noexcept;
  // Set the stroking colour, sRGB 0–255.
  PdfPathStream &set_stroke_color(std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b) noexcept;
  // Set the line width in points.
  PdfPathStream &set_line_width(double width) noexcept;
  // Set the line dash pattern (PDF `d`). `dash_array` alternates on/off lengths
  // in points; an empty vector means solid. `phase` is the offset into the
  // pattern at which to start.
  PdfPathStream &set_dash(std::span<const double> dash_array,
                          double phase) noexcept;
  // Save/restore the graphics state (PDF `q`/`Q`). Used to scope per-track
  // clips and per-glyph transforms so they never leak across layers.
  PdfPathStream &save_state() noexcept;
  PdfPathStream &restore_state() noexcept;
  // Begin/end an Optional Content Group marked-content sequence (PDF
  // `/Lay<i> OC` … `BMC`/`EMC`). ``layer_index`` is a global layer index the
  // page's PdfPageContent::layer_indices registers; the writer names /Lay<i>
  // in the page Resources (/Properties) and declares the OCG in the Catalog
  // OCProperties. Used by layered-PDF export (FRS §5) so viewers can toggle
  // tracks on/off.
  PdfPathStream &mark_begin(std::uint32_t layer_index) noexcept;
  PdfPathStream &mark_end() noexcept;
  // Concatenate the current transformation matrix with the given 6-element
  // matrix (PDF `cm`). This is how the page maps scene millimetres into PDF
  // user-space points, and how per-glyph placement/rotation is applied.
  PdfPathStream &concat_matrix(double a, double b, double c, double d,
                               double e, double f) noexcept;
  // Append a rectangle subpath (PDF `re`). Equivalent to m/l/l/l/h but the
  // dedicated operator is the idiomatic, compact form for axis-aligned rects
  // (intervals, track clips, printable areas).
  PdfPathStream &rect(double x, double y, double width,
                      double height) noexcept;
  // Clip the current path (non-zero winding, PDF `W`) then discard it without
  // painting (PDF `n`). The `clip` + `end_path_no_paint` pair establishes the
  // clip region the following operators draw against; mirror SVG's clipPath.
  PdfPathStream &clip_nonzero() noexcept;
  PdfPathStream &end_path_no_paint() noexcept;
  // Set the non-stroking (fill) alpha to `alpha` in [0,1]. PDF has no inline
  // opacity, so this resolves to a named /ExtGState object (`/GSn`) emitted by
  // the writer; the alpha values a stream uses are recorded deterministically
  // (first-encountered order, deduplicated) and the writer names the objects
  // in that same order, so identical content always yields identical /GS names
  // and object layout. A `gs` operator is emitted referencing the assigned name.
  PdfPathStream &set_fill_alpha(double alpha) noexcept;
  // Set the STRKING (stroke) alpha to `alpha` in [0,1], mirroring
  // set_fill_alpha but resolved to a dedicated `/GSs<n>` ExtGState carrying
  // /CA (PDF 32000-1 §11.6.4.2, "stroke alpha") — without it, semi-transparent
  // strokes exported through PDF rendered fully opaque (#854). The name prefix
  // is distinct from the fill list's `/GSn` so a page using both never emits
  // duplicate ExtGState keys.
  PdfPathStream &set_stroke_alpha(double alpha) noexcept;
  // Paint the current path with a tiling pattern instead of a solid colour
  // (#188). Switches the non-stroking colour space to /Pattern and paints the
  // current path with the named pattern: `/Pattern cs` + `/<name> scn` + `f`.
  // The pattern object is registered separately by the scene exporter and named
  // in the page Resources. Call after building the path (the `f` paints it).
  PdfPathStream &set_pattern_fill(std::string_view pattern_name) noexcept;
  // Invoke a named XObject (PDF `Do`), e.g. an image placed under a prior `cm`.
  // The XObject is registered separately and named in the page Resources.
  PdfPathStream &invoke_xobject(std::string_view name) noexcept;

  // Emit a searchable Latin text run using the PDF standard Type1 Helvetica
  // font (Base-14 — no embedded font program). Used by B1.PDF.2/3 (ADR 0053)
  // as an overlay alongside glyph-outline visuals.
  //
  // B1.PDF.3: accepts UTF-8; emits WinAnsi-compatible Latin-1 (U+0020–007E and
  // U+00A0–00FF). CJK / other code points are dropped and counted in
  // ``non_latin_codepoints_dropped()``. Empty result is a no-op. Marks the
  // stream so the writer names `/F1 /Helvetica` with WinAnsiEncoding.
  PdfPathStream &draw_standard_text(double x, double y, double font_size,
                                    std::string_view text) noexcept;

  // The raw (uncompressed) content-stream operators.
  [[nodiscard]] std::string_view operators() const noexcept;
  // The distinct fill-alpha values this stream emitted `gs /GSn` for, in
  // first-encountered order. The writer emits one /ExtGState object per value
  // and names them /GS0, /GS1, … by this same order. Empty when no alpha was
  // set (opaque only).
  [[nodiscard]] std::span<const double> fill_alphas() const noexcept;
  // The distinct stroke-alpha values this stream emitted `gs /GSs<n>` for, in
  // first-encountered order. Empty when no stroke alpha was set (opaque only).
  [[nodiscard]] std::span<const double> stroke_alphas() const noexcept;
  // True if draw_standard_text was used (writer must emit /Font Helvetica).
  [[nodiscard]] bool needs_standard_font() const noexcept;
  // B1.PDF.3: count of Unicode code points dropped from searchable overlay
  // (CJK etc.). Outline glyphs may still render via TextEngine.
  [[nodiscard]] std::uint32_t non_latin_codepoints_dropped() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Physical page geometry in points (1 pt = 1/72 inch). The spike defaults match
// ISO A4 portrait. Depth (well-log) layouts use a tall custom size in later
// tickets; the spike only needs a valid fixed page.
struct PdfPageSpec {
  double width_points{595.275591};  // A4 width  (210 mm)
  double height_points{841.889764}; // A4 height (297 mm)
};

// A caller-supplied indirect object the writer appends to the PDF (#188): used
// for image XObjects and tiling patterns, which the content stream references by
// name. `body` is the full object body after the "N 0 obj\n" header — i.e. a
// dictionary plus an optional "stream\n…\nendstream". `kind` tells the writer
// which page resource dictionary to name it under (/XObject for images,
// /Pattern for tiling patterns). `local_name` is the per-page name the stream's
// `Do` / `scn` operator uses (e.g. "Im0", "P0"); it must be unique among objects
// of the same kind on one page. Determinism: the writer assigns object numbers
// in `objects` order, so identical content yields identical layout.
enum class PdfObjectKind : std::uint8_t {
  image,
  pattern,
};

struct PdfIndirectObject {
  std::string body;
  PdfObjectKind kind{PdfObjectKind::image};
  std::string local_name;
  // Child indirect objects the writer numbers immediately AFTER this one and
  // emits right after its body. The parent body references a child by the
  // placeholder "@@CHILD<n>@@" (n = 0-based index into extra_bodies): the
  // writer assigns object numbers at write time, so a body built before
  // writing cannot know them — build_image_body uses this to point an image
  // XObject's /SMask at its alpha XObject (issue #476).
  std::vector<std::string> extra_bodies;
};

// One page's worth of content-stream operators plus the extra indirect objects
// (image XObjects, tiling patterns) the stream references by name. `objects`
// defaults empty, in which case the writer emits no /XObject or /Pattern
// resource dicts — byte-identical to the pre-#188 writer (the #185 spike and
// #187 scene tests).
struct PdfPageContent {
  PdfPathStream stream;
  std::span<const PdfIndirectObject> objects{};
  // Layered-PDF global layer indices this page's stream references via
  // mark_begin (/Lay<i> OC … BMC/EMC). The writer emits one OCG per global
  // layer (see PdfWriter::write `layers`) and a /Properties dict naming the
  // indices used on this page. Empty → no /Properties and no OCG involvement
  // (byte-identical to the pre-layered writer).
  std::vector<std::uint32_t> layer_indices{};
};

class WELLLOG_EXPORT_PDF_API PdfDocument {
public:
  PdfDocument();
  ~PdfDocument();
  PdfDocument(const PdfDocument &);
  PdfDocument &operator=(const PdfDocument &);
  PdfDocument(PdfDocument &&) noexcept;
  PdfDocument &operator=(PdfDocument &&) noexcept;

  [[nodiscard]] std::string_view bytes() const noexcept;

private:
  struct Impl;
  explicit PdfDocument(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend class PdfWriter;
};

// Assembles a byte-deterministic, Flate-compressed multi-page PDF from page
// content + page specs. No CreationDate/ModDate and no /ID are emitted, so two
// identical inputs always produce identical output.
class WELLLOG_EXPORT_PDF_API PdfWriter {
public:
  // Build a PDF with one page per content entry (page_specs defaults to A4 when
  // empty, sized to the content count). `layers` (optional) is the global list
  // of Optional Content Group names for layered export (FRS §5): each entry
  // becomes one OCG object, pages reference them via /Lay<i> (i = index into
  // `layers`) from PdfPageContent::layer_indices, and the Catalog gains an
  // OCProperties dict with all layers initially ON. Empty → no OCG output
  // (byte-identical to the pre-layered writer). Returns a Result so
  // allocation/path failures surface as Errors like the other exporters.
  [[nodiscard]] static Result<PdfDocument>
  write(std::span<const PdfPageContent> pages,
        std::span<const PdfPageSpec> page_specs = {},
        std::span<const std::string> layers = {}) noexcept;
};

// Compresses a byte range with zlib (FlateDecode), the same codec the writer
// uses for content streams (#188 image XObject pixel streams). Returns false on
// a zlib error. Exposed so the scene exporter can pre-compress image pixels and
// embed them in a PdfIndirectObject body without reaching into writer internals.
[[nodiscard]] WELLLOG_EXPORT_PDF_API bool
flate_compress_buffer(std::string_view input, std::string &output) noexcept;

} // namespace welllog
