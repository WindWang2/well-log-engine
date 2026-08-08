#pragma once

#include <QString>

struct _object;
using PyObject = _object;

namespace welllog {

class WellLogView;

namespace python {

[[nodiscard]] PyObject *
submit_curve(WellLogView *view, PyObject *depth, PyObject *values,
             const QString &document_id, const QString &axis_id,
             const QString &curve_id, const QString &mnemonic,
             const QString &depth_unit, const QString &value_unit) noexcept;

// Single-well multi-track presentation (#225): payload dict with depth,
// curves[], tracks[] (layers reference curve_id), optional markers.
[[nodiscard]] PyObject *
submit_multi_track(WellLogView *view, PyObject *payload) noexcept;

// Multi-well section (#170): payload is a dict with wells/gap/shared
// viewport/overlays (see workbench welllog_multi_well_adapter.plan_to_submit_payload).
[[nodiscard]] PyObject *
submit_multi_well_section(WellLogView *view, PyObject *payload) noexcept;

[[nodiscard]] PyObject *clear_multi_well_section(WellLogView *view) noexcept;

[[nodiscard]] PyObject *sample_value(WellLogView *view, const QString &curve_id,
                                     unsigned long long sample_index) noexcept;

// Authoritative nice-step tick selection for a depth window (Epic B): returns
// ``(step, [values])`` — the single source of truth shared with the Desktop
// ruler and exports (scene::nice_axis_ticks). Module-level (no view needed).
[[nodiscard]] PyObject *nice_axis_ticks(double d0, double d1,
                                        unsigned long max_ticks) noexcept;

// Tick label with precision trimmed to the step (scene::format_axis_tick_label,
// Epic B) — returns a str.
[[nodiscard]] PyObject *format_axis_tick_label(double value,
                                               double step) noexcept;

// Secondary-axis ticks for an either-direction monotonic (reference, display)
// point list (scene::ticks_for_secondary_window, Epic B) — returns
// ``(step, [reference-domain values])``. ``points`` is a list of [ref, disp].
[[nodiscard]] PyObject *
ticks_for_secondary_axis(PyObject *points, double display_top,
                         double display_bottom,
                         unsigned long max_ticks) noexcept;

// Render the prepared scene for ``document_id`` to SVG and return the
// document bytes (T1 / #273). The engine builds the SVG in memory only —
// it never touches the filesystem; the host writes the returned bytes.
// When ``export_pixel_height > 0`` the scene is re-prepared at that
// aggregate density (T3 / #275) for correct fixed-page pagination.
[[nodiscard]] PyObject *
export_scene_svg(WellLogView *view, const QString &document_id,
                 std::uint64_t export_pixel_height = 0) noexcept;

// Render the prepared scene for ``document_id`` to PDF and return the
// document bytes (T2 / #274). Default text is glyph outlines (non-searchable,
// ADR 0047). ``searchable_text`` (B1.PDF.2 / ADR 0053) overlays Base-14
// Helvetica for Latin/ASCII band labels. ``export_pixel_height`` opts into
// export-density re-prepare (T3 / #275). ``crop_marks`` / ``layered_pdf``
// (FRS §5) enable the corner registration marks and per-track OCG layers on
// the emitted PDF (ExportPageSpec fields; both default off).
[[nodiscard]] PyObject *
export_scene_pdf(WellLogView *view, const QString &document_id,
                 std::uint64_t export_pixel_height = 0,
                 bool searchable_text = false, bool crop_marks = false,
                 bool layered_pdf = false) noexcept;

// CGM Version 3 Binary export (B1.CGM.2–3 / ADR 0054). Returns metafile bytes.
// ``page_height_mm > 0`` enables multi-PICTURE pagination (B1.CGM.3).
[[nodiscard]] PyObject *
export_scene_cgm(WellLogView *view, const QString &document_id,
                 double page_height_mm = 0.0) noexcept;

} // namespace python
} // namespace welllog
