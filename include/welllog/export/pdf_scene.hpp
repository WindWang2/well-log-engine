#pragma once

// Single-page PDF scene emission (#187). Built on the hand-rolled writer from
// #185 (ADR: PDF via hand-rolled writer) and the Export Snapshot model from
// #186 (ADR 0048): a PreparedScene + ExportSnapshot are serialized to a PDF
// whose content stream is pure vector geometry — interval rects, marker lines,
// symbol paths, curve polylines, crossover-fill rings, and text rendered as
// glyph vector outlines (no font program embedded by default; text is
// graphical / non-searchable, ADR 0047). Optional ``searchable_text`` (B1.PDF.2
// / ADR 0053) overlays PDF standard Helvetica operators for Latin/ASCII runs
// so band labels are extractable. Mirrors src/export_vector/svg.cpp's
// append_layer_body 1:1, emitting PDF operators instead of SVG elements.
//
// Coordinate model: the engine's geometry is in scene millimetres (y-down);
// PDF user space is points (1 pt = 1/72 inch). Rather than converting every
// coordinate, one `cm` (concat-matrix) operator at the page top maps the
// scaled scene (mm) into PDF points, so the per-layer emission reads identically
// to the SVG emitter and depth proportions stay true (ADR 0039). Track clipping
// is honoured with a per-track `re ... W n` clip, mirroring SVG's clipPath.
//
// This ticket emits a SINGLE page (continuous-mode layout). Raster images,
// tiling patterns, multi-page pagination and custom-layer symbol geometry
// arrive in the next ticket (#188). Patterned intervals currently fall back to
// their solid fill_color (the spec always carries one); pattern fill is #188.
//
// Determinism is by construction (no CreationDate/ModDate/ID), matching the
// writer; identical scene + snapshot always produce byte-identical output.

#include <welllog/core/result.hpp>
#include <welllog/export/pdf.hpp>
#include <welllog/export/pdf_export.hpp>
#include <welllog/export/export_report.hpp>
#include <welllog/export/pagination.hpp>
#include <welllog/io/manifest.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/scene/text_engine.hpp>

#include <functional>

namespace welllog {

// Serializes a PreparedScene + ExportSnapshot to a PDF (#187 vector primitives
// + text-as-outlines; #188 raster images, tiling patterns, pagination, custom
// layer). Returns a Result so invalid scenes/snapshots surface as
// ErrorCode::invalid_presentation, consistent with SvgExporter /
// PaginatedSvgExporter. Both pagination modes are supported: continuous (one
// long page preserving true depth length) and fixed (depth-window slicing with
// repeating header/legend/page-number/depth-range bands — #188).
//
// Raster image tiles embed as image XObjects whose pixels are fetched via the
// optional `image_tile` resolver (the engine never decodes); a missing resolver
// or failed resolution skips the tile. The pagination metadata bands (well
// name, page number, depth range, legend mnemonics) are rendered as glyph
// outlines via the optional `text_engine` (no font program embedded — ADR
// 0047); geometric band parts (legend colour swatches) are always emitted. With
// no text engine the text portions are omitted but the layout bands remain.
// ``searchable_text`` (default false) keeps B0 outline-only behaviour; when
// true, pagination band strings also emit Base-14 Helvetica text operators
// for Latin-1 (B1.PDF.2/3). CJK code points are dropped from the extractable
// layer (visual outlines still use TextEngine); counts go to
// ``SearchableTextStats`` when provided. Outline PDF remains byte-deterministic
// when searchable_text is false.
struct WELLLOG_EXPORT_PDF_API SearchableTextStats {
  std::uint32_t non_latin_codepoints_dropped{0};
  std::uint32_t latin_runs_emitted{0};
  [[nodiscard]] bool empty() const noexcept {
    return non_latin_codepoints_dropped == 0 && latin_runs_emitted == 0;
  }
};

class WELLLOG_EXPORT_PDF_API PdfSceneExporter {
public:
  [[nodiscard]] static Result<PdfDocument>
  write(const PreparedScene &scene, const ExportSnapshot &snapshot,
        std::function<Result<RasterTile>(const ImageTileRequest &)>
            image_tile = {},
        TextEngine *text_engine = nullptr,
        ExportReport *report = nullptr,
        bool searchable_text = false,
        SearchableTextStats *searchable_stats = nullptr) noexcept;
};

} // namespace welllog
