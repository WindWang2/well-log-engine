"""Export active multi-track presentation to SVG/PDF (#221).

Stage 1 / #277 (T5): engine-backend export route for single_well, routed
through export_dispatch with ``backend="engine"`` (T1 SVG + T2 PDF
bindings). The Qt paint path remains the default.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.engine_bridge import (  # noqa: E402
    engine_available,
    reset_engine_capability_cache,
)
from well_log_workstation.export_dispatch import (  # noqa: E402
    CGM_EXPORT_DISCLOSURE,
    ENGINE_PDF_NONSEARCHABLE_DISCLOSURE,
    PDF_SEARCHABLE_MODE_NOTE,
    UnsupportedFormatError,
    engine_pdf_needs_disclosure,
    export_plot,
    prefer_engine_for_single_well,
    resolve_single_well_pdf_export,
)
from well_log_workstation.export_plot import (  # noqa: E402
    ExportError,
    export_presentation_pdf,
    export_presentation_svg,
)
from well_log_workstation.shell import WellLogWorkstationWindow  # noqa: E402
from well_log_workstation.workspace import create_workspace  # noqa: E402
from well_log_workstation.plot_document import (  # noqa: E402
    PlotDocument,
    load_plot_document,
)


def _write_las(path: Path, well: str = "EXP-1") -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1004.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
RHOB.G/C3
~ASCII
1000 20 2 2.2
1001 30 5 2.3
1002 40 10 2.4
1003 50 20 2.5
1004 60 50 2.6
""",
        encoding="utf-8",
    )
    return path


def test_export_svg_and_pdf_nonempty(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws", name="Export")
    las = _write_las(tmp_path / "e.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    assert win.active_presentation is not None
    assert win.active_presentation.track_count >= 2

    svg_path = tmp_path / "out" / "plot.svg"
    pdf_path = tmp_path / "out" / "plot.pdf"
    out_svg = win.export_active_plot_svg(svg_path)
    out_pdf = win.export_active_plot_pdf(pdf_path)

    assert out_svg.is_file()
    assert out_svg.stat().st_size >= 50
    text = out_svg.read_text(encoding="utf-8", errors="replace")
    assert "svg" in text.lower() or out_svg.stat().st_size > 200

    assert out_pdf.is_file()
    assert out_pdf.stat().st_size >= 50
    # PDF magic
    assert out_pdf.read_bytes()[:4] == b"%PDF"


def test_export_without_presentation_raises(qtbot, tmp_path: Path) -> None:
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    with pytest.raises(ExportError):
        win.export_active_plot_svg(tmp_path / "x.svg")
    with pytest.raises(ExportError):
        win.export_active_plot_pdf(tmp_path / "x.pdf")


def test_export_presentation_api_direct(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "ws2")
    las = _write_las(tmp_path / "d.las", well="DIR")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    svg = export_presentation_svg(pres, tmp_path / "direct.svg")
    pdf = export_presentation_pdf(pres, tmp_path / "direct.pdf")
    assert svg.stat().st_size >= 50
    assert pdf.stat().st_size >= 50


# --- Stage 1 / #277 (T5): engine-backend export route ---------------------


def _engine_setup(qtbot, tmp_path: Path, monkeypatch):
    """Shared setup: a workstation with an engine surface + submitted scene.

    Returns ``(win, plot_doc, document_id)``. Skips when the engine is
    unavailable.
    """
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.delenv("WLWS_FORCE_HOST_CANVAS", raising=False)
    reset_engine_capability_cache()
    if not engine_available():
        pytest.skip("WellLogEngine unavailable")
    ws = create_workspace(tmp_path / "eng")
    las = _write_las(tmp_path / "g.las", well="ENG-EXP")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    win.set_prefer_engine_canvas(True)
    well_id = win.import_las_path(las)
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    pres = win.active_presentation
    assert pres is not None and pres.track_count >= 1
    win.open_engine_preview()  # submits the presentation to WellLogView
    plot_doc = load_plot_document(ws, win.active_plot_id)
    doc_id = pres.well_document_id
    return win, plot_doc, doc_id


def test_prefer_engine_for_single_well_default() -> None:
    """T11: single_well SVG/PDF prefer engine when available."""
    assert prefer_engine_for_single_well("svg", engine_available=True) == "engine"
    assert prefer_engine_for_single_well("pdf", engine_available=True) == "engine"
    assert prefer_engine_for_single_well("png", engine_available=True) == "qt"
    assert prefer_engine_for_single_well("svg", engine_available=False) == "qt"
    assert (
        prefer_engine_for_single_well(
            "pdf", engine_available=True, force_backend="qt"
        )
        == "qt"
    )
    assert engine_pdf_needs_disclosure("engine", "pdf") is True
    assert engine_pdf_needs_disclosure("engine", "svg") is False
    assert "不可搜索" in ENGINE_PDF_NONSEARCHABLE_DISCLOSURE


def test_shell_default_export_uses_engine_when_available(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """export_active_plot_* defaults to engine for single_well (T11)."""
    win, _plot_doc, _doc_id = _engine_setup(qtbot, tmp_path, monkeypatch)
    svg = win.export_active_plot_svg(tmp_path / "def.svg")
    pdf = win.export_active_plot_pdf(tmp_path / "def.pdf")
    assert svg.is_file() and svg.stat().st_size >= 50
    assert pdf.is_file() and pdf.read_bytes()[:5] == b"%PDF-"


def test_engine_route_svg_nonempty(qtbot, tmp_path: Path, monkeypatch) -> None:
    """engine backend produces a valid SVG file via export_dispatch."""
    win, plot_doc, doc_id = _engine_setup(qtbot, tmp_path, monkeypatch)
    out = export_plot(
        plot_doc,
        "svg",
        backend="engine",
        view=win._engine_view,
        document_id=doc_id,
        path=str(tmp_path / "engine.svg"),
    )
    assert out.is_file()
    assert out.stat().st_size >= 50
    text = out.read_text(encoding="utf-8", errors="replace").lstrip()
    assert text.startswith("<?xml") or text.startswith("<svg")


def test_engine_route_pdf_nonempty(qtbot, tmp_path: Path, monkeypatch) -> None:
    """engine backend produces a valid PDF file via export_dispatch."""
    win, plot_doc, doc_id = _engine_setup(qtbot, tmp_path, monkeypatch)
    out = export_plot(
        plot_doc,
        "pdf",
        backend="engine",
        view=win._engine_view,
        document_id=doc_id,
        path=str(tmp_path / "engine.pdf"),
    )
    assert out.is_file()
    assert out.stat().st_size >= 50
    assert out.read_bytes()[:5] == b"%PDF-"


def test_engine_route_missing_view_raises(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """engine backend without view/document_id surfaces a host ExportError."""
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    reset_engine_capability_cache()
    if not engine_available():
        pytest.skip("WellLogEngine unavailable")
    ws = create_workspace(tmp_path / "eng2")
    las = _write_las(tmp_path / "g2.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    plot_doc = load_plot_document(ws, win.active_plot_id)
    with pytest.raises(ExportError):
        export_plot(
            plot_doc,
            "svg",
            backend="engine",
            path=str(tmp_path / "nope.svg"),
        )


def test_correlation_export_svg_and_png(qtbot, tmp_path: Path) -> None:
    """T12 / #300: correlation exports vector SVG + PNG nonempty."""
    from well_log_workstation.tops_model import FormationTop, save_tops_for_well

    ws = create_workspace(tmp_path / "corr-exp", name="CorrExp")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "c1.las", well="C1"))
    id2 = win.import_las_path(_write_las(tmp_path / "c2.las", well="C2"))
    save_tops_for_well(
        ws, id1, [FormationTop(name="T1", depth=1001.0, id="a1")]
    )
    save_tops_for_well(
        ws, id2, [FormationTop(name="T1", depth=1001.5, id="a2")]
    )
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert len(win._correlation_presentations) >= 2

    svg = win.export_active_correlation(tmp_path / "corr.svg", "svg")
    pdf = win.export_active_correlation(tmp_path / "corr.pdf", "pdf")
    png = win.export_active_correlation(tmp_path / "corr.png", "png")

    assert svg.is_file() and svg.stat().st_size >= 50
    text = svg.read_text(encoding="utf-8", errors="replace").lower()
    assert "svg" in text
    assert pdf.is_file() and pdf.read_bytes()[:4] == b"%PDF"
    assert png.is_file() and png.stat().st_size >= 50
    # PNG magic
    assert png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def test_correlation_export_menu_enabled(qtbot, tmp_path: Path) -> None:
    """Export actions enable when a correlation plot is active."""
    ws = create_workspace(tmp_path / "corr-m", name="CorrM")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "m1.las", well="M1"))
    id2 = win.import_las_path(_write_las(tmp_path / "m2.las", well="M2"))
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")
    assert win._act_export_svg.isEnabled()
    assert win._act_export_pdf.isEnabled()
    assert win._act_export_png.isEnabled()


def test_pdf_options_applicability_correlation() -> None:
    """Correlation/section: crop marks yes; engine text/layered no."""
    from well_log_workstation.export_options_dialog import (
        pdf_options_applicability,
    )

    sw = pdf_options_applicability("single_well")
    assert sw["text_mode"] and sw["layered_pdf"] and sw["crop_marks"]
    for kind in ("correlation", "section"):
        opts = pdf_options_applicability(kind)
        assert opts["crop_marks"] is True
        assert opts["text_mode"] is False
        assert opts["layered_pdf"] is False
        assert opts["engine_options"] is False


def test_pdf_export_options_dialog_correlation_disables_engine(qtbot) -> None:
    """Correlation surface: layered/text disabled; crop + value() honest."""
    from well_log_workstation.export_options_dialog import PdfExportOptionsDialog

    dlg = PdfExportOptionsDialog(plot_type="correlation")
    qtbot.addWidget(dlg)
    assert not dlg.radio_outline.isEnabled()
    assert not dlg.radio_searchable.isEnabled()
    assert not dlg.chk_layered.isEnabled()
    assert dlg.chk_crop_marks.isEnabled()
    # User cannot force layered via value() even if checkbox were checked
    dlg.chk_layered.setChecked(True)
    dlg.radio_searchable.setChecked(True)
    dlg.chk_crop_marks.setChecked(True)
    assert dlg.value() == ("outline", True, False, False)


def test_correlation_pdf_crop_marks_via_export_active(
    qtbot, tmp_path: Path
) -> None:
    """Programmatic correlation PDF honours crop_marks (Qt paint path)."""
    ws = create_workspace(tmp_path / "corr-crop", name="CorrCrop")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "cc1.las", well="CC1"))
    id2 = win.import_las_path(_write_las(tmp_path / "cc2.las", well="CC2"))
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")

    plain = win.export_active_correlation(
        tmp_path / "corr_plain.pdf", "pdf", crop_marks=False
    )
    marked = win.export_active_correlation(
        tmp_path / "corr_marked.pdf", "pdf", crop_marks=True
    )
    assert plain.is_file() and plain.read_bytes()[:4] == b"%PDF"
    assert marked.is_file() and marked.read_bytes()[:4] == b"%PDF"
    assert marked.stat().st_size > plain.stat().st_size


def test_correlation_menu_pdf_options_accept_and_cancel(
    qtbot, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Menu PDF path: dialog accept passes crop_marks; cancel writes no file."""
    from PySide6.QtWidgets import QDialog, QMessageBox

    ws = create_workspace(tmp_path / "corr-menu", name="CorrMenu")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "cm1.las", well="CM1"))
    id2 = win.import_las_path(_write_las(tmp_path / "cm2.las", well="CM2"))
    win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")

    out_accept = tmp_path / "menu_corr.pdf"
    # Avoid modal UI noise
    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: 0)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: 0)

    # Cancel: dialog returns None → no file
    monkeypatch.setattr(
        win,
        "_choose_pdf_export_options",
        lambda **kw: None,
    )
    monkeypatch.setattr(
        "well_log_workstation.shell.QFileDialog.getSaveFileName",
        lambda *a, **k: (str(tmp_path / "should_not_exist.pdf"), ""),
    )
    win._export_active_plot("pdf")
    assert not (tmp_path / "should_not_exist.pdf").exists()

    # Accept with crop_marks on — same shell path as menu export
    monkeypatch.setattr(
        win,
        "_choose_pdf_export_options",
        lambda **kw: ("outline", True, False, False),
    )
    monkeypatch.setattr(
        "well_log_workstation.shell.QFileDialog.getSaveFileName",
        lambda *a, **k: (str(out_accept), "PDF (*.pdf)"),
    )
    captured: dict = {}
    real_export = export_plot

    def _spy(plot_doc, fmt, **kwargs):  # noqa: ANN001
        captured.update(kwargs)
        return real_export(plot_doc, fmt, **kwargs)

    monkeypatch.setattr(
        "well_log_workstation.shell.export_plot", _spy
    )
    win._export_active_plot("pdf")
    assert out_accept.is_file()
    assert out_accept.read_bytes()[:4] == b"%PDF"
    assert captured.get("crop_marks") is True
    assert captured.get("backend") == "qt"
    assert "layered_pdf" not in captured or not captured.get("layered_pdf")


def test_section_menu_pdf_options_crop_marks(
    qtbot, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Section PDF menu path shows options dialog and honours crop_marks."""
    from PySide6.QtWidgets import QMessageBox

    from well_log_workstation.plot_document import create_section_plot

    ws = create_workspace(tmp_path / "sec-menu", name="SecMenu")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "s1.las", well="S1"))
    id2 = win.import_las_path(_write_las(tmp_path / "s2.las", well="S2"))
    plot = create_section_plot(
        ws, well_ids=[id1, id2], template_id="std-gr-rt-den", name="SecPDF"
    )
    win.open_plot_document(plot.id)

    out = tmp_path / "section.pdf"
    monkeypatch.setattr(QMessageBox, "information", lambda *a, **k: 0)
    monkeypatch.setattr(QMessageBox, "warning", lambda *a, **k: 0)
    chosen_types: list[str] = []

    def _choose(**kw):  # noqa: ANN003
        chosen_types.append(str(kw.get("plot_type") or ""))
        return ("outline", True, False, False)

    monkeypatch.setattr(win, "_choose_pdf_export_options", _choose)
    monkeypatch.setattr(
        "well_log_workstation.shell.QFileDialog.getSaveFileName",
        lambda *a, **k: (str(out), "PDF (*.pdf)"),
    )
    captured: dict = {}
    real_export = export_plot

    def _spy(plot_doc, fmt, **kwargs):  # noqa: ANN001
        captured.update(kwargs)
        return real_export(plot_doc, fmt, **kwargs)

    monkeypatch.setattr("well_log_workstation.shell.export_plot", _spy)
    win._export_active_plot("pdf")
    assert chosen_types == ["section"]
    assert out.is_file() and out.read_bytes()[:4] == b"%PDF"
    assert captured.get("crop_marks") is True
    assert captured.get("backend") == "qt"


def test_engine_route_png_falls_back_to_qt(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """PNG is not covered by the engine route; falls back to Qt paint."""
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "eng3")
    las = _write_las(tmp_path / "g3.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    plot_doc = load_plot_document(ws, win.active_plot_id)
    # backend=engine + fmt=png must not raise UnsupportedFormatError; it
    # routes through Qt paint (engine route only covers svg/pdf).
    out = export_plot(
        plot_doc,
        "png",
        backend="engine",
        paint_fn=win._paint_active_plot,
        path=str(tmp_path / "fallback.png"),
    )
    assert out.is_file() and out.stat().st_size >= 50


# --- Stage 1 / #278 (T6): engine-vs-engine parity + PDF disclosure --------


def test_engine_svg_pdf_parity_same_document(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """Engine SVG and PDF of the same presentation agree on document identity.

    Both are routed through the host dispatch (T5) from the same prepared
    scene. The SVG carries data-document-id / data-document-revision
    metadata; the PDF is a pure-vector content stream from the same scene.
    This is the secondary parity seam (not pixel-level — no golden baseline).
    """
    win, plot_doc, doc_id = _engine_setup(qtbot, tmp_path, monkeypatch)
    svg_out = export_plot(
        plot_doc, "svg", backend="engine",
        view=win._engine_view, document_id=doc_id,
        path=str(tmp_path / "parity.svg"),
    )
    pdf_out = export_plot(
        plot_doc, "pdf", backend="engine",
        view=win._engine_view, document_id=doc_id,
        path=str(tmp_path / "parity.pdf"),
    )
    assert svg_out.is_file() and svg_out.stat().st_size >= 50
    assert pdf_out.is_file() and pdf_out.stat().st_size >= 50

    # Both come from the same prepared scene → SVG must carry the document
    # id we submitted, and the PDF must be a valid PDF from the same view.
    svg_text = svg_out.read_text(encoding="utf-8", errors="replace")
    assert f'data-document-id="{doc_id}"' in svg_text, (
        "SVG must embed the submitted document id for parity traceability"
    )
    assert pdf_out.read_bytes()[:5] == b"%PDF-"

    # The SVG physical dimensions are deterministic for a given scene; both
    # backends consume the same PreparedScene so the dimensions are fixed.
    assert 'width="' in svg_text and 'height="' in svg_text


def test_engine_pdf_disclosure_contract() -> None:
    """The engine PDF backend surfaces a non-searchable-text disclosure (T6)."""
    # engine PDF requires disclosure (ADR 0047 regression vs Qt QPdfWriter).
    assert engine_pdf_needs_disclosure("engine", "pdf") is True
    # engine SVG, Qt paint, and PNG-fallback do not.
    assert engine_pdf_needs_disclosure("engine", "svg") is False
    assert engine_pdf_needs_disclosure("qt", "pdf") is False
    assert engine_pdf_needs_disclosure("qt", "png") is False
    # The disclosure text is present and non-empty (host UI shows it).
    assert isinstance(ENGINE_PDF_NONSEARCHABLE_DISCLOSURE, str)
    assert len(ENGINE_PDF_NONSEARCHABLE_DISCLOSURE) > 0
    assert "pdf_searchable" in ENGINE_PDF_NONSEARCHABLE_DISCLOSURE
    assert isinstance(PDF_SEARCHABLE_MODE_NOTE, str)
    assert "pdf_searchable" in PDF_SEARCHABLE_MODE_NOTE


def test_pdf_text_mode_resolves_backend_adr_0053() -> None:
    """ADR 0053 dual mode: searchable prefers engine; outline prefers engine."""
    assert prefer_engine_for_single_well(
        "pdf", engine_available=True, pdf_text_mode="searchable"
    ) == "engine"
    assert prefer_engine_for_single_well(
        "pdf", engine_available=False, pdf_text_mode="searchable"
    ) == "qt"
    assert prefer_engine_for_single_well(
        "pdf", engine_available=True, pdf_text_mode="outline"
    ) == "engine"
    assert prefer_engine_for_single_well(
        "pdf", engine_available=False, pdf_text_mode="outline"
    ) == "qt"
    # force_backend still wins
    assert prefer_engine_for_single_well(
        "pdf",
        engine_available=True,
        pdf_text_mode="searchable",
        force_backend="qt",
    ) == "qt"

    b, note = resolve_single_well_pdf_export(
        engine_available=True, pdf_text_mode="searchable"
    )
    assert b == "engine"
    assert "可搜索" in note

    b2, note2 = resolve_single_well_pdf_export(
        engine_available=True, pdf_text_mode="outline"
    )
    assert b2 == "engine"
    assert "不可搜索" in note2 or "引擎" in note2


def test_searchable_pdf_export_is_qt_and_nonempty(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """B1.PDF.1 searchable mode writes a real PDF via Qt paint path."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "ws-search", name="SearchPDF")
    las = _write_las(tmp_path / "s.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    plot_doc = load_plot_document(ws, win.active_plot_id)
    out = export_plot(
        plot_doc,
        "pdf",
        backend="qt",
        paint_fn=win._paint_active_plot,
        path=str(tmp_path / "searchable.pdf"),
    )
    assert out.is_file() and out.stat().st_size >= 50
    assert out.read_bytes()[:5] == b"%PDF-"
    # Routing: searchable prefers engine when available (B1.PDF.2); Qt path
    # remains valid fallback (this test exercises Qt paint export).
    assert prefer_engine_for_single_well(
        "pdf", engine_available=True, pdf_text_mode="searchable"
    ) == "engine"
    assert prefer_engine_for_single_well(
        "pdf", engine_available=False, pdf_text_mode="searchable"
    ) == "qt"


def test_cgm_format_routing_and_disclosure() -> None:
    """B1.CGM.2/3: CGM is engine-only; disclosure mentions multi-page option."""
    assert prefer_engine_for_single_well("cgm", engine_available=True) == "engine"
    assert prefer_engine_for_single_well("cgm", engine_available=False) == "qt"
    assert "CGM" in CGM_EXPORT_DISCLOSURE
    assert "ADR 0054" in CGM_EXPORT_DISCLOSURE or "B1.CGM" in CGM_EXPORT_DISCLOSURE
    assert "PICTURE" in CGM_EXPORT_DISCLOSURE or "分页" in CGM_EXPORT_DISCLOSURE


def test_cgm_without_engine_binding_raises(
    qtbot, tmp_path: Path, monkeypatch
) -> None:
    """Without export_scene_cgm on the view, engine CGM fails clearly."""
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    reset_engine_capability_cache()
    ws = create_workspace(tmp_path / "ws-cgm", name="CGM")
    las = _write_las(tmp_path / "c.las")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    well_id = win.import_las_path(las)
    win.create_single_well_plot_document(well_id, "std-gr-rt-den")
    plot_doc = load_plot_document(ws, win.active_plot_id)

    class _FakeView:
        pass

    with pytest.raises(ExportError, match="export_scene_cgm|CGM"):
        export_plot(
            plot_doc,
            "cgm",
            backend="engine",
            view=_FakeView(),
            document_id="00000000-0000-4000-8000-000000000001",
            path=str(tmp_path / "x.cgm"),
        )

    with pytest.raises(UnsupportedFormatError):
        export_plot(plot_doc, "cgm", backend="qt", path=str(tmp_path / "y.cgm"))


def _minimal_single_well_doc(doc_id: str, name: str) -> PlotDocument:
    return PlotDocument(
        id=doc_id,
        name=name,
        type="single_well",
        well_ids=["w1"],
        template_id=None,
        path=f"plots/{doc_id}.json",
    )


def test_cgm_export_passes_page_height_mm(tmp_path: Path) -> None:
    """B1 closeout: page_height_mm is forwarded to export_scene_cgm when set."""
    captured: dict[str, object] = {}

    class _FakeView:
        def export_scene_cgm(self, document_id, page_height_mm=0.0):
            captured["document_id"] = document_id
            captured["page_height_mm"] = page_height_mm
            # Minimal CGM-ish payload (>= 20 bytes for host size check).
            return b"BEGMF" + b"\x00" * 32

    plot_doc = _minimal_single_well_doc("pd-cgm-page", "CGM page test")
    out = export_plot(
        plot_doc,
        "cgm",
        backend="engine",
        view=_FakeView(),
        document_id="00000000-0000-4000-8000-000000000099",
        path=str(tmp_path / "paged.cgm"),
        page_height_mm=125.5,
    )
    assert out.is_file() and out.stat().st_size >= 20
    assert captured["document_id"] == "00000000-0000-4000-8000-000000000099"
    assert captured["page_height_mm"] == pytest.approx(125.5)


def test_cgm_export_omits_page_height_when_unset(tmp_path: Path) -> None:
    """Continuous export calls export_scene_cgm with document_id only (or default 0)."""
    captured: list[tuple] = []

    class _FakeView:
        def export_scene_cgm(self, document_id, page_height_mm=0.0):
            captured.append((document_id, page_height_mm))
            return b"BEGMF" + b"\x00" * 32

    plot_doc = _minimal_single_well_doc("pd-cgm-cont", "CGM continuous")
    out = export_plot(
        plot_doc,
        "cgm",
        backend="engine",
        view=_FakeView(),
        document_id="doc-continuous",
        path=str(tmp_path / "continuous.cgm"),
    )
    assert out.is_file()
    assert len(captured) == 1
    assert captured[0][0] == "doc-continuous"
    # Host continuous path does not pass page_height_mm; binding default is 0.0.
    assert captured[0][1] == 0.0


def test_cgm_export_page_height_typeerror_fallback(tmp_path: Path) -> None:
    """Old bindings that reject page_height_mm still export via one-arg call."""
    calls: list[int] = []

    # Simulate binding that only accepts document_id: host tries 2-arg then 1-arg.
    class _StrictOneArgView:
        def export_scene_cgm(self, *args):
            if len(args) != 1:
                raise TypeError(
                    f"export_scene_cgm() takes 1 positional argument but {len(args)} were given"
                )
            calls.append(len(args))
            return b"BEGMF" + b"\x00" * 32

    plot_doc = _minimal_single_well_doc("pd-cgm-old", "CGM old binding")
    out = export_plot(
        plot_doc,
        "cgm",
        backend="engine",
        view=_StrictOneArgView(),
        document_id="doc-old",
        path=str(tmp_path / "old.cgm"),
        page_height_mm=80.0,
    )
    assert out.is_file()
    # Two-arg attempt TypeError → one-arg success.
    assert calls == [1]


# ---------------------------------------------------------------------------
# FRS §5: engine PDF crop marks / layered PDF export options
# ---------------------------------------------------------------------------


def test_engine_pdf_forwards_crop_marks_and_layered_pdf(tmp_path: Path) -> None:
    """FRS §5: crop_marks / layered_pdf kwargs reach export_scene_pdf."""
    captured: list[tuple] = []

    class _CapturingView:
        def export_scene_pdf(
            self, document_id, export_pixel_height=0, searchable_text=False,
            crop_marks=False, layered_pdf=False,
        ):
            captured.append(
                (document_id, export_pixel_height, searchable_text, crop_marks, layered_pdf)
            )
            return b"%PDF-1.7\n" + b"\x00" * 64

    plot_doc = _minimal_single_well_doc("pd-pdf-opts", "PDF options")
    out = export_plot(
        plot_doc,
        "pdf",
        backend="engine",
        view=_CapturingView(),
        document_id="doc-opts",
        path=str(tmp_path / "opts.pdf"),
        pdf_text_mode="outline",
        crop_marks=True,
        layered_pdf=True,
    )
    assert out.is_file()
    assert captured == [("doc-opts", 0, False, True, True)]


def test_engine_pdf_searchable_forwards_options(tmp_path: Path) -> None:
    """Searchable path forwards the same option flags."""
    captured: list[tuple] = []

    class _CapturingView:
        def export_scene_pdf(
            self, document_id, export_pixel_height=0, searchable_text=False,
            crop_marks=False, layered_pdf=False,
        ):
            captured.append(
                (document_id, export_pixel_height, searchable_text, crop_marks, layered_pdf)
            )
            return b"%PDF-1.7\n" + b"\x00" * 64

    plot_doc = _minimal_single_well_doc("pd-pdf-srch", "PDF searchable opts")
    export_plot(
        plot_doc,
        "pdf",
        backend="engine",
        view=_CapturingView(),
        document_id="doc-srch",
        path=str(tmp_path / "srch.pdf"),
        pdf_text_mode="searchable",
        crop_marks=True,
        layered_pdf=False,
    )
    assert captured == [("doc-srch", 0, True, True, False)]


def test_engine_pdf_default_options_stay_off(tmp_path: Path) -> None:
    """Without the option kwargs the flags default to False (backward compat)."""
    captured: list[tuple] = []

    class _CapturingView:
        def export_scene_pdf(
            self, document_id, export_pixel_height=0, searchable_text=False,
            crop_marks=False, layered_pdf=False,
        ):
            captured.append(
                (document_id, export_pixel_height, searchable_text, crop_marks, layered_pdf)
            )
            return b"%PDF-1.7\n" + b"\x00" * 64

    plot_doc = _minimal_single_well_doc("pd-pdf-plain", "PDF plain")
    export_plot(
        plot_doc,
        "pdf",
        backend="engine",
        view=_CapturingView(),
        document_id="doc-plain",
        path=str(tmp_path / "plain.pdf"),
    )
    assert captured == [("doc-plain", 0, False, False, False)]


def test_engine_pdf_falls_back_without_new_binding_args(tmp_path: Path) -> None:
    """Old bindings (pre-FRS §5) reject the extra args → one-arg call."""
    calls: list[int] = []

    # Simulate a binding that only accepts document_id: the host tries the
    # 5-arg form (options on) and falls back to the 1-arg call.
    class _StrictOneArgView:
        def export_scene_pdf(self, *args):
            if len(args) != 1:
                raise TypeError(
                    f"export_scene_pdf() takes 1 positional argument but {len(args)} were given"
                )
            calls.append(len(args))
            return b"%PDF-1.7\n" + b"\x00" * 64

    plot_doc = _minimal_single_well_doc("pd-pdf-old", "PDF old binding")
    out = export_plot(
        plot_doc,
        "pdf",
        backend="engine",
        view=_StrictOneArgView(),
        document_id="doc-old",
        path=str(tmp_path / "old.pdf"),
        crop_marks=True,
        layered_pdf=True,
    )
    assert out.is_file()
    assert calls == [1]


def test_pdf_export_options_dialog_value(qtbot) -> None:
    """Options dialog: defaults off; toggles round-trip through value()."""
    from well_log_workstation.export_options_dialog import PdfExportOptionsDialog

    dlg = PdfExportOptionsDialog()
    qtbot.addWidget(dlg)
    assert dlg.value() == ("outline", False, False, False)
    assert dlg.radio_outline.isEnabled()
    assert dlg.chk_layered.isEnabled()

    dlg.radio_searchable.setChecked(True)
    dlg.chk_crop_marks.setChecked(True)
    dlg.chk_layered.setChecked(True)
    assert dlg.value() == ("searchable", True, True, False)

    # Pre-seeded state round-trips.
    dlg2 = PdfExportOptionsDialog(
        text_mode="searchable", crop_marks=True, layered_pdf=True
    )
    qtbot.addWidget(dlg2)
    assert dlg2.value() == ("searchable", True, True, False)


# ---------------------------------------------------------------------------
# FRS §5: Qt fallback crop marks (engine parity)
# ---------------------------------------------------------------------------


def test_qt_svg_crop_marks_add_eight_lines(tmp_path: Path) -> None:
    """The Qt fallback draws the same 8 crop-mark lines as the engine.

    QSvgGenerator emits drawLine as <polyline> elements — 8 for the four
    two-line corner marks.
    """
    from PySide6.QtGui import QColor

    plot_doc = _minimal_single_well_doc("pd-qt-crop", "Qt crop marks")

    def _paint(painter, rect):
        painter.fillRect(rect, QColor("#ffffff"))

    plain = export_plot(
        plot_doc, "svg", backend="qt", paint_fn=_paint,
        path=str(tmp_path / "plain.svg"),
    )
    marked = export_plot(
        plot_doc, "svg", backend="qt", paint_fn=_paint,
        path=str(tmp_path / "marked.svg"), crop_marks=True,
    )
    plain_text = plain.read_text(encoding="utf-8")
    marked_text = marked.read_text(encoding="utf-8")
    assert (
        marked_text.count("<polyline") == plain_text.count("<polyline") + 8
    )


def test_qt_svg_default_matches_explicit_off(tmp_path: Path) -> None:
    """No kwarg == crop_marks=False byte-for-byte (backward compatible)."""
    from PySide6.QtGui import QColor

    plot_doc = _minimal_single_well_doc("pd-qt-plain", "Qt plain")

    def _paint(painter, rect):
        painter.fillRect(rect, QColor("#ffffff"))

    default = export_plot(
        plot_doc, "svg", backend="qt", paint_fn=_paint,
        path=str(tmp_path / "default.svg"),
    )
    explicit_off = export_plot(
        plot_doc, "svg", backend="qt", paint_fn=_paint,
        path=str(tmp_path / "off.svg"), crop_marks=False,
    )
    assert default.read_bytes() == explicit_off.read_bytes()


def test_qt_pdf_crop_marks_grow_output(tmp_path: Path) -> None:
    """PDF crop marks add vector content (size check; PDF is not text-parseable)."""
    from PySide6.QtGui import QColor

    plot_doc = _minimal_single_well_doc("pd-qt-pdf-crop", "Qt PDF crop")

    def _paint(painter, rect):
        painter.fillRect(rect, QColor("#ffffff"))

    plain = export_plot(
        plot_doc, "pdf", backend="qt", paint_fn=_paint,
        path=str(tmp_path / "plain.pdf"),
    )
    marked = export_plot(
        plot_doc, "pdf", backend="qt", paint_fn=_paint,
        path=str(tmp_path / "marked.pdf"), crop_marks=True,
    )
    assert marked.stat().st_size > plain.stat().st_size
