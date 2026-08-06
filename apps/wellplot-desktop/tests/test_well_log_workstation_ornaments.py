"""Publication ornaments — title block / legend / location map (FRS §5 / P2-C).

Covers:
* pure layout helpers (layout_legend_items, title_block_fields);
* draw_* smoke (each ornament paints onto a QImage without raising);
* section canvas ornaments state + render smoke;
* plot-document ornaments round-trip + legacy default;
* shell data collection (legend items from patterns/faults/contacts).
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QRectF
from PySide6.QtGui import QBrush, QColor, QImage, QPainter
from PySide6.QtWidgets import QApplication

from well_log_workstation.ornament import (
    OrnamentData,
    draw_legend_block,
    draw_location_map,
    draw_ornaments,
    draw_scale_bar,
    draw_title_block,
    layout_legend_items,
    title_block_fields,
)


@pytest.fixture(scope="module", autouse=True)
def _qapp():
    app = QApplication.instance() or QApplication([])
    yield app


def _painter() -> tuple[QPainter, QImage]:
    img = QImage(400, 300, QImage.Format.Format_ARGB32)
    img.fill(0)
    return QPainter(img), img


# ---------------------------------------------------------------------------
# Pure layout helpers
# ---------------------------------------------------------------------------


def test_layout_legend_items_grid() -> None:
    items = [("x", f"L{i}") for i in range(6)]
    pos = layout_legend_items(items, QRectF(0, 0, 100, 60), 22, 14, 2)
    assert len(pos) == 6
    # Row 0: (0,0), (22,0); row 1: (0,14), (22,14).
    assert (pos[0][0], pos[0][1]) == (0.0, 0.0)
    assert (pos[1][0], pos[1][1]) == (22.0, 0.0)
    assert (pos[2][0], pos[2][1]) == (0.0, 14.0)
    assert (pos[3][0], pos[3][1]) == (22.0, 14.0)


def test_layout_legend_items_clips_outside_rect() -> None:
    items = [("x", f"L{i}") for i in range(6)]
    # 1 column, 4 rows fit in height 60 → only 4 placed.
    pos = layout_legend_items(items, QRectF(0, 0, 100, 60), 22, 14, 1)
    assert len(pos) == 4


def test_title_block_fields() -> None:
    fields = title_block_fields("剖面图", "工区A", "1:500", date="2026-08-06")
    assert fields == {
        "title": "剖面图",
        "workspace": "工区A",
        "scale": "1:500",
        "date": "2026-08-06",
    }


def test_title_block_fields_default_date() -> None:
    fields = title_block_fields("X", "Y")
    assert fields["date"]  # today, non-empty


# ---------------------------------------------------------------------------
# Draw smoke
# ---------------------------------------------------------------------------


def test_draw_title_block_smoke() -> None:
    p, img = _painter()
    draw_title_block(p, QRectF(200, 240, 190, 50), title_block_fields("T", "W", "1:500"))
    p.end()
    assert not img.isNull()


def test_draw_legend_block_smoke() -> None:
    p, img = _painter()
    items = [("#dc2626", "断层"), (QBrush(QColor("#2563eb")), "油水界面")]
    draw_legend_block(p, QRectF(10, 10, 180, 80), items, cols=1)
    p.end()
    assert not img.isNull()


def test_draw_legend_block_empty() -> None:
    p, img = _painter()
    draw_legend_block(p, QRectF(10, 10, 180, 80), [], cols=1)  # no crash
    p.end()


def test_draw_location_map_smoke() -> None:
    p, img = _painter()
    pts = [(116.5, 30.2), (116.6, 30.3), (116.55, 30.25)]
    draw_location_map(p, QRectF(280, 10, 110, 90), pts, [0, 2])
    p.end()
    assert not img.isNull()


def test_draw_location_map_insufficient_points() -> None:
    p, img = _painter()
    draw_location_map(p, QRectF(10, 10, 110, 90), [(1.0, 2.0)], [0])  # no crash
    p.end()


def test_draw_scale_bar_smoke() -> None:
    p, img = _painter()
    draw_scale_bar(p, QRectF(10, 100, 80, 14), "1:500")
    p.end()


def test_draw_ornaments_composite_smoke() -> None:
    p, img = _painter()
    data = OrnamentData(
        title_fields=title_block_fields("剖面", "工区", "1:500"),
        legend_items=[("#dc2626", "断层"), ("#2563eb", "油水界面")],
        map_points=[(116.5, 30.2), (116.6, 30.3)],
        map_highlight=[0],
        scale_text="1:500",
    )
    draw_ornaments(p, QRectF(200, 100, 190, 130), data)
    p.end()
    assert not img.isNull()


def test_draw_ornaments_empty_noop() -> None:
    p, img = _painter()
    draw_ornaments(p, QRectF(0, 0, 100, 100), OrnamentData())
    p.end()
    assert OrnamentData().is_empty()


# ---------------------------------------------------------------------------
# Section canvas
# ---------------------------------------------------------------------------


def test_canvas_ornaments_state_and_paint(qtbot) -> None:
    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)
    depth = np.array([0.0, 100.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 20.0])
        null_mask = np.array([False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    canvas.set_section([pres, pres])
    canvas.set_depth_range(0.0, 100.0)

    assert canvas.show_ornaments() is False
    data = OrnamentData(
        title_fields=title_block_fields("剖面", "工区", "1:500"),
        legend_items=[("#dc2626", "断层")],
        map_points=[(1.0, 2.0), (3.0, 4.0)],
        map_highlight=[0],
        scale_text="1:500",
    )
    canvas.set_ornament_data(data)
    canvas.set_show_ornaments(True)
    assert canvas.show_ornaments() is True
    assert canvas.ornament_data() is data

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke (ornaments painted)


# ---------------------------------------------------------------------------
# Plot-document persistence
# ---------------------------------------------------------------------------


def test_ornaments_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.plot_document import (
        create_section_plot,
        load_plot_document,
        save_plot_document,
    )
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="Orn")
    add_well(ws, name="W1", path="wells/w1.las")
    add_well(ws, name="W2", path="wells/w2.las")
    plot = create_section_plot(
        ws, well_ids=[ws.wells[0].id, ws.wells[1].id],
        template_id="gr-only", name="S",
    )
    assert plot.ornaments is False  # default

    loaded = load_plot_document(ws, plot.id)
    loaded.ornaments = True
    save_plot_document(ws, loaded)

    again = load_plot_document(ws, plot.id)
    assert again.ornaments is True


def test_legacy_section_json_ornaments_default_false(tmp_path: Path) -> None:
    import json

    from well_log_workstation.plot_document import load_plot_document
    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "legacy", name="Legacy")
    plot = add_plot(
        ws, name="Legacy Section", plot_type="section",
        well_ids=["w1", "w2"], template_id="gr-only", path="plots/legacy.json",
    )
    (ws.root / plot.path).write_text(
        json.dumps(
            {
                "schemaVersion": 7,
                "id": plot.id, "name": "Legacy Section", "type": "section",
                "well_ids": ["w1", "w2"], "template_id": "gr-only",
            }
        ),
        encoding="utf-8",
    )
    loaded = load_plot_document(ws, plot.id)
    assert loaded.ornaments is False


# ---------------------------------------------------------------------------
# Shell data collection
# ---------------------------------------------------------------------------


def test_collect_section_ornaments(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import WellCatalogEntry, create_workspace

    ws = create_workspace(tmp_path / "ws", name="OrnWS")
    ws.wells.append(WellCatalogEntry("w1", "W1", "wells/w1.las", lng=116.5, lat=30.2))
    ws.wells.append(WellCatalogEntry("w2", "W2", "wells/w2.las", lng=116.6, lat=30.3))
    ws.wells.append(WellCatalogEntry("w3", "W3", "wells/w3.las"))  # no coords

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    from well_log_workstation.plot_document import create_section_plot

    plot = create_section_plot(
        ws, well_ids=["w1", "w2"], template_id="gr-only", name="S"
    )
    plot.faults = [{"name": "F1", "between": [0, 1], "x_frac": 0.5,
                    "top_depth": 0, "bottom_depth": 100, "throw": 5.0}]
    plot.contacts = [{"fluid_type": "owc", "depths": [[0, 50], [1, 52]]}]

    data = win._collect_section_ornaments(
        plot, [(116.5, 30.2), (116.6, 30.3)]
    )
    assert isinstance(data, OrnamentData)
    # Legend contains 断层 and 油水界面 entries.
    labels = [label for _, label in data.legend_items]
    assert "断层" in labels
    assert "油水界面" in labels
    # Location map: 2 coord wells, both highlighted (section wells).
    assert len(data.map_points) == 2
    assert sorted(data.map_highlight) == [0, 1]
    # Title block fields populated.
    assert data.title_fields["title"] == "S"
    assert data.title_fields["workspace"] == "OrnWS"
    assert not data.is_empty()
