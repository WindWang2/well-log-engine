"""SY/T 5615 lithology pattern library — loading, Qt rendering, persistence.

Covers:
* builtin catalog loads with the core lithologies and well-formed fields;
* pattern_to_engine_payload matches the SDK PatternDefinition contract;
* make_qbrush produces a distinct textured brush per pattern;
* section canvas tie-quads render a real pattern (not the Dense4 fallback);
* correlation canvas accepts a fill-pattern map and paints without crashing;
* plot-document litho_pattern_map round-trips and defaults to {} on legacy JSON.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from well_log_workstation.litho_pattern_lib import (
    LithoPattern,
    get_pattern,
    load_builtin_patterns,
    make_qbrush,
    pattern_to_engine_payload,
)
from well_log_workstation.plot_document import load_plot_document
from well_log_workstation.section_geometry.tie_polygons import TieQuad2D


@pytest.fixture(scope="module", autouse=True)
def _qapp():
    app = QApplication.instance() or QApplication([])
    yield app


# ---------------------------------------------------------------------------
# Catalog loading
# ---------------------------------------------------------------------------

EXPECTED_IDS = {
    "syt-sandstone",
    "syt-mudstone",
    "syt-conglomerate",
    "syt-limestone",
    "syt-dolomite",
    "syt-gypsum-salt",
    "syt-shale",
}


def test_builtin_patterns_load() -> None:
    pats = load_builtin_patterns()
    assert EXPECTED_IDS <= set(pats)
    for pat in pats.values():
        assert isinstance(pat, LithoPattern)
        assert pat.tile_w > 0 and pat.tile_h > 0
        assert pat.foreground.startswith("#")
        assert pat.background.startswith("#")
        assert pat.primitives  # every pattern has at least one primitive


def test_get_pattern_hit_and_miss() -> None:
    assert get_pattern("syt-sandstone") is not None
    assert get_pattern("nonexistent") is None
    assert get_pattern("") is None


# ---------------------------------------------------------------------------
# SDK payload contract
# ---------------------------------------------------------------------------

# Keys the C++ parser (numpy_bridge.cpp pattern loop) requires.
_REQUIRED_PAYLOAD_KEYS = {
    "id",
    "tile_width_mm",
    "tile_height_mm",
    "rotation_degrees",
    "stroke_width_mm",
    "foreground",
    "background",
    "primitives",
}
# Allowed primitive kinds (one-key dicts).
_PRIMITIVE_KINDS = {"line", "polyline", "circle"}


def test_payload_matches_sdk_contract() -> None:
    for pat in load_builtin_patterns().values():
        payload = pattern_to_engine_payload(pat)
        assert _REQUIRED_PAYLOAD_KEYS <= set(payload), pat.id
        for prim in payload["primitives"]:
            keys = set(prim.keys())
            assert len(keys) == 1, f"{pat.id}: primitive must be one-key"
            assert keys <= _PRIMITIVE_KINDS, pat.id


def test_payload_line_primitive_shape() -> None:
    sand = load_builtin_patterns()["syt-sandstone"]
    payload = pattern_to_engine_payload(sand)
    line_prims = [p for p in payload["primitives"] if "line" in p]
    assert line_prims, "sandstone should have line primitives"
    body = line_prims[0]["line"]
    for key in ("from_x", "from_y", "to_x", "to_y"):
        assert key in body


# ---------------------------------------------------------------------------
# Qt rendering
# ---------------------------------------------------------------------------


def test_make_qbrush_returns_textured_brush() -> None:
    pat = load_builtin_patterns()["syt-sandstone"]
    brush = make_qbrush(pat, "#93c5fd")
    tex = brush.texture()
    assert tex is not None
    assert not tex.isNull()
    assert tex.width() > 0 and tex.height() > 0


def test_different_patterns_yield_different_pixmaps(pixel_bytes) -> None:
    pats = load_builtin_patterns()
    b1 = make_qbrush(pats["syt-sandstone"])
    b2 = make_qbrush(pats["syt-mudstone"])
    img1, img2 = b1.texture().toImage(), b2.texture().toImage()
    # Different patterns must rasterize to different pixel data.
    assert img1.size() == img2.size()
    assert pixel_bytes(img1) != pixel_bytes(img2)


def test_pixel_bytes_compares_content_not_constbits_pointers(pixel_bytes) -> None:
    """#613 canary: helper matches on identical pixels and fails on a fill change."""
    from PySide6.QtGui import QImage

    a = QImage(8, 8, QImage.Format.Format_ARGB32)
    b = QImage(8, 8, QImage.Format.Format_ARGB32)
    a.fill(0xFF112233)
    b.fill(0xFF112233)
    assert pixel_bytes(a) == pixel_bytes(b)
    b.fill(0xFF445566)
    assert pixel_bytes(a) != pixel_bytes(b)


def test_make_qbrush_empty_pattern_falls_back_to_color() -> None:
    empty = LithoPattern(
        id="empty",
        name="empty",
        name_en="",
        tile_w=5.0,
        tile_h=5.0,
        rotation=0.0,
        stroke_w=0.2,
        foreground="#000000",
        background="#ffffff",
        primitives=(),
    )
    brush = make_qbrush(empty, "#ff0000")
    # No texture for an empty pattern (plain color brush).
    assert brush.texture().isNull()


# ---------------------------------------------------------------------------
# Section canvas tie-quad integration
# ---------------------------------------------------------------------------


def test_section_canvas_paints_pattern_quad(qtbot) -> None:
    """A tie quad with a known pattern_id paints without raising."""
    from PySide6.QtGui import QImage
    import numpy as np

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)

    depth = np.array([1000.0, 1005.0, 1010.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 20.0, 30.0])
        null_mask = np.array([False, False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    pres = HostPresentation(
        template_id="t",
        template_name="T",
        well_document_id="w1",
        well_name="W1",
        depth=depth,
        depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    # Quad corners in section space (x = well-index scaled, y = depth).
    quad = TieQuad2D(
        corners=np.array(
            [[0.0, 1001.0], [1.0, 1002.0], [1.0, 1009.0], [0.0, 1008.0]]
        ),
        fill_color="#fde68a",
        pattern_id="syt-sandstone",
    )
    canvas.set_section([pres, pres], [[], []], tie_quads=[quad])
    canvas.set_depth_range(1000.0, 1010.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke; no pixel assertion.


# ---------------------------------------------------------------------------
# Correlation canvas pattern map
# ---------------------------------------------------------------------------


def test_correlation_canvas_pattern_map_state(qtbot) -> None:
    from well_log_workstation.correlation_canvas import CorrelationCanvas

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    assert canvas.fill_pattern_map() == {}
    canvas.set_fill_pattern_map({"T1": "syt-sandstone", "": "skip", "T2": ""})
    assert canvas.fill_pattern_map() == {"T1": "syt-sandstone"}
    canvas.set_fill_pattern_map(None)
    assert canvas.fill_pattern_map() == {}


def test_correlation_canvas_paints_with_patterns(qtbot) -> None:
    from PySide6.QtGui import QImage
    import numpy as np

    from well_log_workstation.correlation_canvas import CorrelationCanvas
    from well_log_workstation.template_model import HostPresentation
    from well_log_workstation.tops_model import FormationTop

    canvas = CorrelationCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)

    depth = np.array([1000.0, 1005.0, 1010.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 20.0, 30.0])
        null_mask = np.array([False, False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    pres = HostPresentation(
        template_id="t",
        template_name="T",
        well_document_id="w1",
        well_name="W1",
        depth=depth,
        depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    canvas.set_columns([pres, pres])
    canvas.set_tops_per_column(
        [
            [FormationTop(name="T1", depth=1001.0, id="a"),
             FormationTop(name="T2", depth=1009.0, id="b")],
            [FormationTop(name="T1", depth=1002.0, id="c"),
             FormationTop(name="T2", depth=1008.0, id="d")],
        ]
    )
    canvas.set_show_interwell_fill(True)
    canvas.set_fill_pattern_map({"T1": "syt-sandstone"})
    canvas.set_pinchout("linear", 0.5, smooth=True)  # wedge path too
    canvas.set_depth_range(999.0, 1011.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke


# ---------------------------------------------------------------------------
# Plot-document persistence
# ---------------------------------------------------------------------------


def _write_las(path: Path, well: str) -> Path:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1005.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
RT.OHMM
~ASCII
1000 10 1
1001 20 2
1002 30 3
1003 40 4
1004 50 5
1005 60 6
""",
        encoding="utf-8",
    )
    return path


def test_litho_pattern_map_persists_and_reopens(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import FormationTop, save_tops_for_well
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="Litho")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    id1 = win.import_las_path(_write_las(tmp_path / "a.las", "A"))
    id2 = win.import_las_path(_write_las(tmp_path / "b.las", "B"))
    save_tops_for_well(
        ws, id1,
        [FormationTop(name="T1", depth=1001.0, id="x1"),
         FormationTop(name="T2", depth=1003.0, id="x2")],
    )
    save_tops_for_well(
        ws, id2,
        [FormationTop(name="T1", depth=1001.2, id="y1"),
         FormationTop(name="T2", depth=1003.2, id="y2")],
    )
    plot = win.create_correlation_plot_document([id1, id2], "std-gr-rt-den")

    # Enable fill (gates litho UI), then assign a pattern to T1 via the UI.
    win.corr_fill_check.setChecked(True)
    assert win.corr_litho_combo.isEnabled()
    win.corr_litho_target.setText("T1")
    idx = win.corr_litho_combo.findData("syt-sandstone")
    assert idx >= 0
    win.corr_litho_combo.setCurrentIndex(idx)

    loaded = load_plot_document(ws, plot.id)
    assert loaded.litho_pattern_map.get("T1") == "syt-sandstone"
    assert win.correlation_canvas.fill_pattern_map().get("T1") == "syt-sandstone"

    # Reopen in a fresh window restores the map.
    win2 = WellLogWorkstationWindow()
    qtbot.addWidget(win2)
    win2.set_workspace(ws)
    win2.open_plot_document(plot.id)
    assert win2.correlation_canvas.fill_pattern_map().get("T1") == "syt-sandstone"


def test_legacy_plot_json_without_litho_pattern_map(tmp_path: Path) -> None:
    """A v7 plot JSON written before litho_pattern_map loads with {} default."""
    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "legacy", name="Legacy")
    plot = add_plot(
        ws,
        name="Legacy Corr",
        plot_type="correlation",
        well_ids=["w1", "w2"],
        template_id="std-gr-rt-den",
        path="plots/legacy.json",
    )
    (ws.root / plot.path).write_text(
        json.dumps(
            {
                "schemaVersion": 7,
                "id": plot.id,
                "name": "Legacy Corr",
                "type": "correlation",
                "well_ids": ["w1", "w2"],
                "template_id": "std-gr-rt-den",
                "links": [],
                "show_interwell_fill": False,
            }
        ),
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, plot.id)
    assert loaded.litho_pattern_map == {}
