"""Section-x coordinate conventions and core-dialog sample binding (#512,
#513, #593, #594, #741).

The section overlays (faults, contacts, surfaces, tie quads) are ALL in
well-index units (well i sits at x = i); the canvas used to divide four of
them by (n-1) — compressing every overlay into the first inter-well gap on
sections with >= 3 wells — and the tie-quad builder emitted cumulative CRS
distances instead of indices.
"""
from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.section_geometry import (
    SectionFault2D,
    fault_polyline,
    tie_quads,
)


def test_tie_quads_x_in_well_index_units():
    """#593: quads between wells (1, 2) must span x=1..2 regardless of the
    wells' CRS positions (the old builder emitted cumulative distances,
    which the index-unit canvas compressed into the leftmost gap)."""
    well_pos = [(0.0, 0.0), (5000.0, 0.0), (30000.0, 0.0)]
    tops = [
        [{"name": "T1", "depth": 100.0}],
        [{"name": "T1", "depth": 110.0}],
        [{"name": "T1", "depth": 120.0}],
    ]
    quads = tie_quads(tops, well_pos)
    assert len(quads) == 2
    assert quads[0].corners[:, 0].tolist() == [0.0, 1.0, 1.0, 0.0]
    assert quads[1].corners[:, 0].tolist() == [1.0, 2.0, 2.0, 1.0]


def test_tie_quads_edges_use_their_own_well_depths():
    """#594: a dipping shared top must produce a dipping quad edge — the
    left edge from the left well, the right edge from the right well (the
    old builder flattened both edges to the left well's depths)."""
    well_pos = [(0.0, 0.0), (1000.0, 0.0)]
    tops = [
        [{"name": "T1", "depth": 100.0}, {"name": "T2", "depth": 200.0}],
        [{"name": "T1", "depth": 150.0}, {"name": "T2", "depth": 250.0}],
    ]
    quads = tie_quads(tops, well_pos)
    assert len(quads) == 1
    c = quads[0].corners
    # [left_top, right_top, right_bottom, left_bottom]
    assert c[0, 1] == 100.0  # left top from LEFT well
    assert c[1, 1] == 150.0  # right top from RIGHT well (dipping edge)
    assert c[2, 1] == 250.0  # right bottom from RIGHT well
    assert c[3, 1] == 200.0  # left bottom from LEFT well


def test_tie_quads_datum_shift_applies_per_column():
    """#741 lock: shifts keyed by column index reach the builder's tops."""
    well_pos = [(0.0, 0.0), (1000.0, 0.0)]
    tops = [
        [{"name": "T1", "depth": 100.0}],
        [{"name": "T1", "depth": 100.0}],
    ]
    quads = tie_quads(tops, well_pos, datum_shifts={"0": -10.0, "1": 0.0})
    c = quads[0].corners
    assert c[0, 1] == 90.0  # left column shifted
    assert c[1, 1] == 100.0  # right column unshifted


def test_fault_between_wells_one_two_maps_right_of_first_gap(qtbot):
    """#513: with 3 wells, a fault between wells (1, 2) must render in the
    SECOND gap — its red pixels sit right of the canvas midpoint, not
    compressed into the first gap (the old / (n-1) remap drew it there)."""
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(900, 480)

    depth = np.array([1000.0, 1010.0])

    class _Layer:
        color = "#1f77b4"
        values = np.array([10.0, 30.0])
        null_mask = np.array([False, False])

    class _Scale:
        mode = "linear"
        min = 0.0
        max = 100.0

    class _Track:
        role = "curve"
        layers = [_Layer()]
        scale = _Scale()

    def _pres() -> HostPresentation:
        return HostPresentation(
            template_id="t", template_name="T", well_document_id="w",
            well_name="W", depth=depth, depth_unit="m",
            tracks=[_Track()],  # type: ignore[arg-type]
        )

    fault = SectionFault2D(
        name="F", between=(1, 2), x_frac=0.5,
        top_depth=1000.0, bottom_depth=1010.0, color="#ff0000",
    )
    canvas.set_section([_pres(), _pres(), _pres()], [[], [], []], faults=[fault])
    canvas.set_depth_range(999.0, 1011.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)
    arr = np.ndarray(
        (img.height(), img.width(), 4), dtype=np.uint8, buffer=img.constBits()
    )
    # constBits() exposes ARGB32 as BGRA bytes on little-endian hosts.
    red_mask = (arr[:, :, 2] > 200) & (arr[:, :, 1] < 80) & (arr[:, :, 0] < 80)
    assert red_mask.any(), "fault line must be visible"
    xs = np.where(red_mask.any(axis=0))[0]
    center = img.width() / 2
    assert xs.mean() > center, (
        f"fault between wells (1,2) rendered left of midpoint "
        f"(mean x={xs.mean():.0f} <= {center:.0f}): the / (n-1) remap regression"
    )


def test_fault_polyline_index_units_are_canvas_consistent():
    """The fault geometry (well-index units) feeds the canvas unchanged."""
    f = SectionFault2D(name="F", between=(1, 2), x_frac=0.5)
    pts = fault_polyline(f, 3)
    assert pts[0, 0] == pytest.approx(1.5)
