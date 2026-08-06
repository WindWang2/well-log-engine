"""Section fluid contacts OWC/GOC + dual-fill split (FRS §3.3 / P1-B).

Covers:
* contact_segment_2d: contiguous runs break at missing wells, <2 wells empty;
* split_quad_by_contact: contact crossing a quad splits into above (oil/gas)
  + below (water); quad entirely above/below untouched; missing-well contact
  does not split; precise corner assertions;
* serialization round-trip + tolerant parsing;
* section canvas render smoke (contact line + split quad);
* plot-document contacts round-trip + legacy default [].
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.section_geometry import (
    FluidContact2D,
    SectionFault2D,
    TieQuad2D,
    contact_segment_2d,
    contacts_from_json,
    contacts_to_json,
    split_quad_by_contact,
    split_quad_composite,
)


# ---------------------------------------------------------------------------
# contact_segment_2d
# ---------------------------------------------------------------------------


def test_segment_connects_adjacent_wells() -> None:
    c = FluidContact2D(fluid_type="owc", depths={0: 800.0, 1: 810.0, 2: 820.0})
    segs = contact_segment_2d(c, 3)
    assert len(segs) == 1
    assert segs[0].shape == (3, 2)
    assert segs[0][0, 1] == 800.0 and segs[0][2, 1] == 820.0


def test_segment_breaks_at_missing_well() -> None:
    # well 1 missing -> two separate runs (well 0 alone has <2 → dropped;
    # wells 1-2 missing/absent → only the well-0 run, which is too short).
    c = FluidContact2D(fluid_type="owc", depths={0: 800.0, 2: 820.0, 3: 830.0})
    segs = contact_segment_2d(c, 4)
    # well 0 alone → dropped; wells 2-3 contiguous → one run.
    assert len(segs) == 1
    assert segs[0].shape == (2, 2)


def test_segment_needs_two_wells() -> None:
    c = FluidContact2D(fluid_type="owc", depths={0: 800.0})
    assert contact_segment_2d(c, 3) == []


def test_resolved_color_owc_goc() -> None:
    assert FluidContact2D("owc").resolved_color() == "#2563eb"
    assert FluidContact2D("goc").resolved_color() == "#f59e0b"


# ---------------------------------------------------------------------------
# split_quad_by_contact
# ---------------------------------------------------------------------------


def _quad(x0: float, d_top: float, d_bot: float) -> TieQuad2D:
    """Quad spanning columns x0..x0+1, depths d_top..d_bot."""
    return TieQuad2D(
        corners=np.array(
            [[x0, d_top], [x0 + 1, d_top], [x0 + 1, d_bot], [x0, d_bot]]
        ),
        fill_color="#aaa",
    )


def test_split_owc_above_oil_below_water() -> None:
    q = _quad(0.0, 1000.0, 1100.0)
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1055.0})
    res = split_quad_by_contact(q, c, 4)
    assert res is not None
    above, below = res
    # Above = oil red, below = water blue.
    assert above.fill_color == "#dc2626"
    assert below.fill_color == "#2563eb"
    # Above corners: left_top(1000), right_top(1000), right contact(1055), left contact(1050)
    assert above.corners[0, 1] == 1000.0
    assert above.corners[2, 1] == pytest.approx(1055.0)
    assert above.corners[3, 1] == pytest.approx(1050.0)
    # Below corners: left contact(1050), right contact(1055), right_bottom(1100), left_bottom(1100)
    assert below.corners[0, 1] == pytest.approx(1050.0)
    assert below.corners[2, 1] == 1100.0


def test_split_goc_above_gas_below_oil() -> None:
    q = _quad(0.0, 1000.0, 1100.0)
    c = FluidContact2D(fluid_type="goc", depths={0: 1050.0, 1: 1050.0})
    res = split_quad_by_contact(q, c, 4)
    assert res is not None
    # GOC: above = gas yellow, below = oil red.
    assert res[0].fill_color == "#f59e0b"
    assert res[1].fill_color == "#dc2626"


def test_quad_above_contact_not_split() -> None:
    """Quad entirely shallower than the contact → no split."""
    q = _quad(0.0, 1000.0, 1040.0)
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1050.0})
    assert split_quad_by_contact(q, c, 4) is None


def test_quad_below_contact_not_split() -> None:
    """Quad entirely deeper than the contact → no split."""
    q = _quad(0.0, 1060.0, 1100.0)
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1050.0})
    assert split_quad_by_contact(q, c, 4) is None


def test_missing_well_contact_not_split() -> None:
    """Contact absent on one of the bounding wells → no split."""
    q = _quad(0.0, 1000.0, 1100.0)
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0})  # well 1 missing
    assert split_quad_by_contact(q, c, 4) is None


def test_zero_width_quad_not_split() -> None:
    q = TieQuad2D(
        corners=np.array(
            [[1.0, 1000.0], [1.0, 1000.0], [1.0, 1100.0], [1.0, 1100.0]]
        ),
        fill_color="#aaa",
    )
    c = FluidContact2D(fluid_type="owc", depths={1: 1050.0, 2: 1050.0})
    assert split_quad_by_contact(q, c, 4) is None


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def test_contacts_round_trip() -> None:
    cs = [
        FluidContact2D(fluid_type="owc", depths={0: 800.0, 1: 810.0}),
        FluidContact2D(fluid_type="goc", depths={0: 700.0, 1: 705.0, 2: 710.0}),
    ]
    back = contacts_from_json(contacts_to_json(cs))
    assert len(back) == 2
    assert back[0].fluid_type == "owc" and back[0].depths == {0: 800.0, 1: 810.0}
    assert back[1].fluid_type == "goc" and back[1].depths[2] == 710.0


def test_contacts_from_json_drops_invalid() -> None:
    raw = [
        {"fluid_type": "owc", "depths": [[0, 800], [1, 810]]},
        {"fluid_type": "bogus"},  # bad fluid type
        "not a dict",
        None,
    ]
    assert len(contacts_from_json(raw)) == 1


def test_contacts_from_json_non_list_returns_empty() -> None:
    assert contacts_from_json(None) == []
    assert contacts_from_json({"not": "list"}) == []


# ---------------------------------------------------------------------------
# Section canvas render smoke
# ---------------------------------------------------------------------------


def test_section_canvas_renders_with_contact(qtbot) -> None:
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)

    depth = np.array([1000.0, 1100.0])

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

    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    quad = _quad(0.0, 1000.0, 1100.0)
    contact = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1055.0})
    canvas.set_section(
        [pres, pres], [[], []], contacts=[contact], tie_quads=[quad]
    )
    canvas.set_depth_range(999.0, 1101.0)

    img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
    img.fill(0)
    canvas.render(img)  # crash smoke


# ---------------------------------------------------------------------------
# Plot-document persistence
# ---------------------------------------------------------------------------


def test_section_contacts_persist_and_reopen(tmp_path: Path) -> None:
    from well_log_workstation.plot_document import (
        create_section_plot,
        load_plot_document,
        save_plot_document,
    )
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="Contact")
    add_well(ws, name="W1", path="wells/w1.las")
    add_well(ws, name="W2", path="wells/w2.las")
    plot = create_section_plot(
        ws, well_ids=[ws.wells[0].id, ws.wells[1].id],
        template_id="gr-only", name="S",
    )

    loaded = load_plot_document(ws, plot.id)
    loaded.contacts = contacts_to_json(
        [FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1055.0})]
    )
    save_plot_document(ws, loaded)

    again = load_plot_document(ws, plot.id)
    assert len(again.contacts) == 1
    parsed = contacts_from_json(again.contacts)
    assert parsed[0].fluid_type == "owc"
    assert parsed[0].depths == {0: 1050.0, 1: 1055.0}


def test_legacy_section_json_without_contacts(tmp_path: Path) -> None:
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
    assert loaded.contacts == []


# ---------------------------------------------------------------------------
# Composite split: faults then contacts (FRS §3.x P2 × P1-B)
# ---------------------------------------------------------------------------


def test_split_composite_nothing_returns_quad() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    pieces = split_quad_composite(q, [], [], 3)
    assert len(pieces) == 1
    assert pieces[0] is q


def test_split_composite_contact_only() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1050.0})
    pieces = split_quad_composite(q, [], [c], 3)
    assert len(pieces) == 2
    assert pieces[0].fill_color == "#dc2626"  # above = oil
    assert pieces[1].fill_color == "#2563eb"  # below = water


def test_split_composite_fault_only() -> None:
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000, bottom_depth=1100
    )
    pieces = split_quad_composite(q, [f], [], 3)
    assert len(pieces) == 2
    # Structural split keeps the quad's fill (no recolouring).
    assert pieces[0].fill_color == "#aaa"
    assert pieces[1].fill_color == "#aaa"


def test_split_composite_fault_then_contact_four_pieces() -> None:
    """A fault at x=0.5 with differing per-well contact depths → the contact
    line offsets at the fault plane: each half splits independently."""
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000, bottom_depth=1100
    )
    # Contact at 1050 on well 0 (left half) and 1030 on well 1 (right half),
    # both inside their halves' depth extent.
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1030.0})
    pieces = split_quad_composite(q, [f], [c], 3)
    assert len(pieces) == 4
    for piece in pieces:
        assert piece.fill_color in ("#dc2626", "#2563eb")
    assert pieces[0].fill_color == "#dc2626"  # left half above
    assert pieces[1].fill_color == "#2563eb"  # left half below
    assert pieces[2].fill_color == "#dc2626"  # right half above
    assert pieces[3].fill_color == "#2563eb"  # right half below
    # Contact depths differ per side → the line offsets at the fault plane.
    assert pieces[0].corners[3, 1] == pytest.approx(1050.0)
    assert pieces[2].corners[2, 1] == pytest.approx(1030.0)


def test_split_composite_contact_crosses_one_half() -> None:
    """A contact inside only the left half leaves the right half whole → 3."""
    q = _quad(0.0, 1005.0, 1095.0)
    f = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000, bottom_depth=1100
    )
    # Well-1 depth 1000 sits above the right half's top (1005) → no split
    # there; well-0 depth 1050 splits the left half.
    c = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1000.0})
    pieces = split_quad_composite(q, [f], [c], 3)
    assert len(pieces) == 3
    assert pieces[0].fill_color == "#dc2626"
    assert pieces[1].fill_color == "#2563eb"
    assert pieces[2].fill_color == "#aaa"  # right half unsplit


def test_split_composite_two_contacts_full_cut() -> None:
    """GOC then OWC: sequential full cut → gas / oil / water (3 pieces)."""
    q = _quad(0.0, 1000.0, 1100.0)
    goc = FluidContact2D(fluid_type="goc", depths={0: 1020.0, 1: 1020.0})
    owc = FluidContact2D(fluid_type="owc", depths={0: 1060.0, 1: 1060.0})
    pieces = split_quad_composite(q, [], [goc, owc], 3)
    assert len(pieces) == 3
    # GOC above = gas amber; then OWC splits remaining into oil / water.
    assert pieces[0].fill_color == "#f59e0b"  # gas (above GOC)
    assert pieces[1].fill_color == "#dc2626"  # oil (above OWC)
    assert pieces[2].fill_color == "#2563eb"  # water (below OWC)
    assert pieces[0].corners[2, 1] == pytest.approx(1020.0)
    assert pieces[1].corners[0, 1] == pytest.approx(1020.0)
    assert pieces[1].corners[2, 1] == pytest.approx(1060.0)
    assert pieces[2].corners[0, 1] == pytest.approx(1060.0)


def test_split_composite_two_faults_full_cut() -> None:
    """Two fault planes: each splits current pieces → 3 columns."""
    q = _quad(0.0, 1005.0, 1095.0)
    f1 = SectionFault2D(
        name="F1", between=(0, 1), x_frac=0.33, top_depth=1000, bottom_depth=1100
    )
    f2 = SectionFault2D(
        name="F2", between=(0, 1), x_frac=0.66, top_depth=1000, bottom_depth=1100
    )
    pieces = split_quad_composite(q, [f1, f2], [], 3)
    assert len(pieces) == 3
    # Left edge of first piece at x=0; right edge of last at x=1.
    assert pieces[0].corners[0, 0] == pytest.approx(0.0)
    assert pieces[-1].corners[1, 0] == pytest.approx(1.0)
    # Shared vertical edges abut.
    assert pieces[0].corners[1, 0] == pytest.approx(pieces[1].corners[0, 0])
    assert pieces[1].corners[1, 0] == pytest.approx(pieces[2].corners[0, 0])


def test_split_composite_two_faults_and_two_contacts() -> None:
    """Multi-fault × multi-contact: every plane applied, not first-hit only."""
    q = _quad(0.0, 1000.0, 1100.0)
    f1 = SectionFault2D(
        name="F1", between=(0, 1), x_frac=0.4, top_depth=1000, bottom_depth=1100
    )
    f2 = SectionFault2D(
        name="F2", between=(0, 1), x_frac=0.7, top_depth=1000, bottom_depth=1100
    )
    goc = FluidContact2D(fluid_type="goc", depths={0: 1025.0, 1: 1025.0})
    owc = FluidContact2D(fluid_type="owc", depths={0: 1065.0, 1: 1065.0})
    pieces = split_quad_composite(q, [f1, f2], [goc, owc], 3)
    # 3 columns × 3 fluid bands = 9 when both contacts cross every column.
    assert len(pieces) == 9
    colors = {p.fill_color for p in pieces}
    assert colors == {"#f59e0b", "#dc2626", "#2563eb"}


def test_section_canvas_fault_and_contact_composite_render(qtbot) -> None:
    from PySide6.QtGui import QImage

    from well_log_workstation.section_canvas import SectionCanvas
    from well_log_workstation.section_geometry import SectionFault2D
    from well_log_workstation.template_model import HostPresentation

    canvas = SectionCanvas()
    qtbot.addWidget(canvas)
    canvas.resize(600, 480)

    depth = np.array([1000.0, 1100.0])

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

    pres = HostPresentation(
        template_id="t", template_name="T", well_document_id="w1",
        well_name="W1", depth=depth, depth_unit="m",
        tracks=[_Track()],  # type: ignore[arg-type]
    )
    quad = _quad(0.0, 1005.0, 1095.0)
    fault = SectionFault2D(
        name="F", between=(0, 1), x_frac=0.5, top_depth=1000,
        bottom_depth=1100, throw=20.0,
    )
    contact = FluidContact2D(fluid_type="owc", depths={0: 1050.0, 1: 1050.0})

    def grab(faults, contacts) -> QImage:
        img = QImage(canvas.size(), QImage.Format.Format_ARGB32)
        img.fill(0)
        canvas.set_section(
            [pres, pres], [[], []], faults=faults, contacts=contacts,
            tie_quads=[quad],
        )
        canvas.render(img)
        return img

    fault_only = grab([fault], [])
    composite = grab([fault], [contact])
    assert composite.constBits() != fault_only.constBits()
    contact_only = grab([], [contact])
    assert composite.constBits() != contact_only.constBits()
