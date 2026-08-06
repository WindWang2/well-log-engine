"""Section geometry + datum + SectionCanvas tests (Phase-2 PR-C3)."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.datum.well_section_datum import WellSectionDatum  # noqa: E402
from well_log_workstation.section_geometry.tie_polygons import tie_quads  # noqa: E402


# --- WellSectionDatum (T5) ---

def test_datum_md_zero_shift():
    d = WellSectionDatum(mode="md")
    assert d.compute_shifts([{"name": "A"}, {"name": "B"}]) == {
        "A": 0.0, "B": 0.0,
    }


def test_datum_tvdss_negative_kb():
    d = WellSectionDatum(mode="tvdss")
    shifts = d.compute_shifts(
        [{"name": "A", "kb_m": 25.0}, {"name": "B", "kb_m": 30.0}]
    )
    assert shifts == {"A": -25.0, "B": -30.0}


def test_datum_horizon_flattens_on_named_top():
    d = WellSectionDatum(mode="horizon", target_horizon="T1")
    shifts = d.compute_shifts(
        [{"name": "A", "tops": [{"name": "T1", "depth": 100.0}]}]
    )
    assert shifts == {"A": -100.0}


def test_datum_rejects_tvd():
    with pytest.raises(ValueError, match="tvd"):
        WellSectionDatum(mode="tvd")


# --- fault_2d (T4) ---
# The legacy 3D-CRS curtain_slice_fault path was retired in P1-A in favour of
# the section 2D position+throw model. Coverage of the new model lives in
# test_well_log_workstation_fault_section.py.


# --- contact_2d (T4) ---
# The legacy 3D-CRS contact_polyline path was retired in P1-B in favour of
# the section 2D per-well-depth model. Coverage of the new model lives in
# test_well_log_workstation_contact_section.py.


# --- tie_polygons (T4) ---

def test_tie_quads_between_shared_tops():
    well_pos = [(0.0, 0.0), (100.0, 0.0)]
    tops = [
        [{"name": "T1", "depth": 100.0}, {"name": "T2", "depth": 200.0}],
        [{"name": "T1", "depth": 110.0}, {"name": "T2", "depth": 210.0}],
    ]
    quads = tie_quads(tops, well_pos)
    assert len(quads) == 1
    quad = quads[0]
    assert quad.corners.shape == (4, 2)
    # Solid fill color default
    assert quad.fill_color == "#cbd5e1"


def test_tie_quads_no_shared_tops_empty():
    well_pos = [(0.0, 0.0), (100.0, 0.0)]
    tops = [[{"name": "A", "depth": 100.0}], [{"name": "B", "depth": 110.0}]]
    assert tie_quads(tops, well_pos) == []


def test_tie_quads_requires_two_wells():
    assert tie_quads([[{"name": "A", "depth": 1.0}]], [(0.0, 0.0)]) == []


# --- SectionCanvas smoke (offscreen Qt) ---

def test_section_canvas_constructs_and_paints(qtbot):
    from well_log_workstation.section_canvas import SectionCanvas
    from PySide6.QtCore import QPoint
    from PySide6.QtGui import QImage, QPainter

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 400)

    img = QImage(600, 400, QImage.Format.Format_RGB32)
    img.fill(0xFFFFFFFF)
    p = QPainter(img)
    try:
        canvas.render(p, QPoint())
    finally:
        # Always end the painter so an exception cannot leave an active
        # QPainter on img and crash in its GC/destructor.
        p.end()
    # Empty canvas paints the placeholder text; no crash.
    assert img.width() == 600
