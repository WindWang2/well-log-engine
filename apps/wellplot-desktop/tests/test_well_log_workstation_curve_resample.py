"""Explicit curve resampling + version identity (Epic A 多采样率).

Covers:
* resample_curve — regular grid, non-integer rate ratio, irregular source,
  null/NaN gap propagation (never fabricated values), degenerate rejects;
* version identity — ImportedCurve.version, curve_by_mnemonic(version=...),
  leaf ids carrying the version (raw keeps the historic shape);
* persistence round-trip + apply_resamples_to_document (derive/replace);
* Desktop table projection: a per-curve-axis curve gets its own table with
  its real depth (no merge into the shared-axis table);
* shell: persisted resample definitions are re-derived on template apply.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.curve_resample import (
    CurveResample,
    apply_resamples_to_document,
    load_curve_resamples_for_well,
    resample_curve,
    save_curve_resamples_for_well,
)
from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument


# ---------------------------------------------------------------------------
# resample_curve
# ---------------------------------------------------------------------------


def test_resample_regular_grid() -> None:
    depth = np.linspace(1000.0, 1100.0, 201)  # 0.5 m source
    values = depth * 0.1  # linear ramp, exactly representable
    new_depth, new_values, new_null = resample_curve(
        depth, values, None, target_interval=1.0
    )
    assert new_depth.size == 101
    assert new_depth[0] == pytest.approx(1000.0)
    assert new_depth[-1] == pytest.approx(1100.0)
    np.testing.assert_allclose(new_values, new_depth * 0.1, atol=1e-9)
    assert not new_null.any()


def test_resample_non_integer_rate_ratio() -> None:
    # 0.125 m source → 0.2 m target (ratio 1.6): derived axis is its own.
    depth = np.arange(1000.0, 1000.5 + 1e-9, 0.125)
    values = np.arange(depth.size, dtype=np.float64)
    new_depth, new_values, _ = resample_curve(
        depth, values, None, target_interval=0.2
    )
    # Grid covers [d0, d1 + interval/2): 1000.0 .. 1000.6. Samples beyond the
    # source end are NaN gaps (never extrapolated/clamped values).
    assert new_depth.size == 4
    np.testing.assert_allclose(new_depth, [1000.0, 1000.2, 1000.4, 1000.6])
    # Linear interp of the ramp: value at 1000.2 ≈ 1.6; beyond the source the
    # sample is a gap, not a fabricated value.
    assert new_values[1] == pytest.approx(1.6)
    assert not np.isfinite(new_values[-1])


def test_resample_irregular_source() -> None:
    depth = np.array([1000.0, 1000.3, 1000.8, 1002.0])
    values = np.array([10.0, 13.0, 18.0, 30.0])
    new_depth, new_values, _ = resample_curve(
        depth, values, None, target_interval=0.5
    )
    assert new_depth.size == 5
    # 1000.5 lies between (1000.3, 13) and (1000.8, 18): 13 + 0.4*5 = 15.
    idx = int(np.searchsorted(new_depth, 1000.5))
    assert new_values[idx] == pytest.approx(15.0)


def test_resample_null_gaps_propagate_as_gaps() -> None:
    depth = np.arange(1000.0, 1006.0, 1.0)
    values = np.arange(6.0)
    nulls = np.zeros(6, dtype=bool)
    nulls[2:4] = True  # gap at 1002–1003
    new_depth, new_values, new_null = resample_curve(
        depth, values, nulls, target_interval=0.5
    )
    # Samples whose interpolation support spans the gap are NaN/null.
    gap = new_null | ~np.isfinite(new_values)
    assert not bool(gap[0])  # 1000.0 fine
    assert bool(np.all(gap[4:6]))  # 1002.0–1002.5 span the gap
    assert bool(np.all(~gap[8:]))  # beyond the gap recovers


def test_resample_rejects_degenerate() -> None:
    with pytest.raises(ValueError):
        resample_curve(np.array([1000.0]), np.array([1.0]), None, 0.5)
    with pytest.raises(ValueError):
        resample_curve(np.array([1000.0, 1001.0]), np.array([1.0, 2.0]), None, 0.0)
    with pytest.raises(ValueError):
        resample_curve(np.array([1000.0, 1001.0]), np.array([1.0, 2.0]), None, -1.0)


# ---------------------------------------------------------------------------
# Version identity
# ---------------------------------------------------------------------------


def test_curve_by_mnemonic_version_filter() -> None:
    doc = ImportedWellDocument(
        document_id="aaaaaaaa-0000-4000-8000-000000000001",
        well_name="MR", source_path="",
        depth=np.array([1000.0, 1000.5]),
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.array([1.0, 2.0]),
                null_mask=np.zeros(2, dtype=bool),
            ),
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.array([1.5]),
                null_mask=np.zeros(1, dtype=bool),
                depth=np.array([1000.25]),
                version="resample-0.5m",
            ),
        ],
    )
    assert doc.curve_by_mnemonic("gr").version == "raw"  # first match
    assert doc.curve_by_mnemonic("GR", version="raw").version == "raw"
    assert doc.curve_by_mnemonic("GR", version="resample-0.5m").version == "resample-0.5m"
    assert doc.curve_by_mnemonic("GR", version="nope") is None


def test_leaf_id_version_shape() -> None:
    from well_log_workstation.display_set import (
        leaf_id_for_curve,
        leaves_from_document,
    )

    assert leaf_id_for_curve("w1", "GR") == "w1:GR"
    assert leaf_id_for_curve("w1", "GR", "raw") == "w1:GR"
    assert leaf_id_for_curve("w1", "GR", "resample-0.5m") == "w1:GR:resample-0.5m"
    doc = ImportedWellDocument(
        document_id="w2", well_name="MR", source_path="",
        depth=np.array([1000.0, 1000.5]),
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.array([1.0, 2.0]),
                null_mask=np.zeros(2, dtype=bool),
            ),
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.array([1.5, 2.5]),
                null_mask=np.zeros(2, dtype=bool),
                depth=np.array([1000.0, 1000.5]),
                version="resample-1m",
            ),
        ],
    )
    leaves = leaves_from_document(doc)
    ids = {leaf.id for leaf in leaves}
    assert "w2:GR" in ids
    assert "w2:GR:resample-1m" in ids
    labels = {leaf.label for leaf in leaves}
    assert "GR (resample-1m)" in labels


# ---------------------------------------------------------------------------
# Persistence + apply
# ---------------------------------------------------------------------------


def test_resample_definitions_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="RS")
    well = add_well(ws, name="W1", path="wells/w1.las")
    save_curve_resamples_for_well(
        ws,
        well.id,
        [CurveResample(mnemonic="GR", interval=0.5, version="resample-0.5m")],
    )
    loaded, diags = load_curve_resamples_for_well(ws, well.id)
    assert diags == []
    assert len(loaded) == 1
    assert loaded[0].mnemonic == "GR"
    assert loaded[0].interval == 0.5
    assert loaded[0].version == "resample-0.5m"
    missing, diags2 = load_curve_resamples_for_well(ws, "no-such-well")
    assert missing == []
    assert len(diags2) == 1  # unknown well → diagnostic, not a crash


def test_apply_resamples_derives_and_replaces() -> None:
    doc = ImportedWellDocument(
        document_id="aaaaaaaa-0000-4000-8000-000000000001",
        well_name="MR", source_path="",
        depth=np.arange(1000.0, 1004.0, 0.5),
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.arange(8.0),
                null_mask=np.zeros(8, dtype=bool),
            ),
        ],
    )
    diags = apply_resamples_to_document(
        doc,
        [CurveResample(mnemonic="GR", interval=1.0, version="resample-1m")],
    )
    assert diags == []
    derived = doc.curve_by_mnemonic("GR", version="resample-1m")
    assert derived is not None
    assert derived.depth is not None
    assert derived.depth.size == 4  # 1000..1003 covers [1000, 1003.5)
    assert derived.values.size == 4
    # Re-applying the same version replaces in place (no duplicates).
    apply_resamples_to_document(
        doc,
        [CurveResample(mnemonic="GR", interval=1.0, version="resample-1m")],
    )
    versions = [
        c for c in doc.curves if c.version == "resample-1m"
    ]
    assert len(versions) == 1
    # Unknown mnemonic → diagnostic, no crash.
    diags2 = apply_resamples_to_document(
        doc, [CurveResample(mnemonic="NOPE", interval=1.0, version="resample-1m")]
    )
    assert diags2 and "NOPE" in diags2[0]


# ---------------------------------------------------------------------------
# Table projection: per-curve axis gets its own table
# ---------------------------------------------------------------------------


def test_table_projection_per_curve_axis_table() -> None:
    from well_log_workstation.display_set import (
        display_set_from_ids,
        leaf_id_for_curve,
    )
    from well_log_workstation.table_projection import (
        build_table_projections_guarded,
    )
    from well_log_workstation.template_model import get_builtin_template

    doc = ImportedWellDocument(
        document_id="aaaaaaaa-0000-4000-8000-000000000001",
        well_name="MR", source_path="",
        depth=np.arange(1000.0, 1004.0, 0.5),  # 8 samples
        depth_unit="m",
        curves=[
            ImportedCurve(
                mnemonic="GR", unit="API",
                values=np.arange(8.0),
                null_mask=np.zeros(8, dtype=bool),
            ),
            ImportedCurve(
                mnemonic="RT", unit="OHMM",
                values=np.array([1.0, 2.0, 3.0]),
                null_mask=np.zeros(3, dtype=bool),
                depth=np.array([1000.2, 1001.2, 1002.2]),
                version="resample-1m",
            ),
        ],
    )
    template = get_builtin_template("std-gr-rt-den")
    assert template is not None
    display = display_set_from_ids(
        [
            leaf_id_for_curve(doc.document_id, "GR"),
            leaf_id_for_curve(doc.document_id, "RT", "resample-1m"),
        ]
    )
    tables = build_table_projections_guarded(doc, display, template)
    per_axis = [t for t in tables if t.axis_id == "curve-RT"]
    assert len(per_axis) == 1, "per-curve-axis curve must own a table"
    assert per_axis[0].row_count == 3
    assert per_axis[0].depth[0] == pytest.approx(1000.2)
    shared = [t for t in tables if t.axis_id == "md"]
    assert len(shared) == 1
    assert shared[0].row_count == 8


# ---------------------------------------------------------------------------
# Shell: persisted resamples re-derive on template apply
# ---------------------------------------------------------------------------


def test_shell_reapplies_persisted_resamples(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws2", name="RS2")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    def _las(path: Path) -> Path:
        path.write_text(
            """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. A
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 10
1001 20
1002 30
1003 40
""",
            encoding="utf-8",
        )
        return path

    well_id = win.import_las_path(_las(tmp_path / "a.las"))
    save_curve_resamples_for_well(
        ws,
        well_id,
        [CurveResample(mnemonic="GR", interval=2.0, version="resample-2m")],
    )
    pres = win.apply_template_to_well(well_id, "std-gr-rt-den")
    derived = [
        layer
        for track in pres.tracks
        for layer in track.layers
        if getattr(layer, "depth", None) is not None
    ]
    assert len(derived) >= 1, "the derived resampled curve must bind a layer"
    # Grid 1000..1002 (2 m step over 1000–1003): [1000, 1002].
    np.testing.assert_allclose(derived[0].depth, [1000.0, 1002.0])
