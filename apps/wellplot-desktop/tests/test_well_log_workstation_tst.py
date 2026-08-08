"""Epic D slice 3 — Desktop TST wiring (well_log_workstation.tst).

Covers:
* parity — the Python mirror asserts the SAME analytic fixture values as
  tests/integration/tst_layers_test.cpp (the SDK is the single source of
  truth; the mirror is locked to it by shared fixtures);
* validation mirror (ValueError on every C++ input-error case);
* normal_from_dip_azimuth convention (φ=0 degenerates to the C++ convention);
* binding-first dispatch (fake welllog module wins when present, mirror
  otherwise);
* path_from_trajectory (survey → PathPoint3D, monotonic filtering);
* bedding.json sidecar round-trip + tolerant read;
* end-to-end: survey stations + bedding sidecar → TST.
"""

from __future__ import annotations

import json
import math
import os
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.stratigraphy import StratigraphicDictionary
from well_log_workstation.survey import SurveyStation, compute_trajectory
from well_log_workstation.tst import (
    BEDDING_FILENAME,
    BeddingLayer,
    BedNormal3D,
    BeddingLayerSpec,
    PathPoint3D,
    WellDirection3D,
    _BINDING_CACHE,
    load_bedding_for_well,
    normal_from_dip_azimuth,
    path_from_trajectory,
    save_bedding_for_well,
    tst_along_path,
    tst_through_layers,
)

KPI = math.pi / 180.0
KS2 = math.sqrt(2.0) / 2.0  # √2/2

VERTICAL = WellDirection3D(x=0.0, y=0.0, z=1.0)
HORIZONTAL = BedNormal3D(x=0.0, y=0.0, z=1.0)


def dipping_bed(dip_deg: float) -> BedNormal3D:
    return normal_from_dip_azimuth(dip_deg, 0.0)


def _two_layers() -> list[BeddingLayer]:
    return [
        BeddingLayer(top_md=0.0, bottom_md=50.0, normal=HORIZONTAL),
        BeddingLayer(top_md=50.0, bottom_md=100.0, normal=dipping_bed(45.0)),
    ]


# ---------------------------------------------------------------------------
# Parity with the C++ fixtures (same analytic values as tst_layers_test.cpp)
# ---------------------------------------------------------------------------


def test_parity_varying_orientation() -> None:
    tst = tst_through_layers(0.0, 100.0, VERTICAL, _two_layers())
    assert tst.value == pytest.approx(50.0 + 50.0 * KS2, abs=1e-9)
    assert tst.measured_interval_m == 100.0
    assert tst.normal_dot == pytest.approx((50.0 + 50.0 * KS2) / 100.0, abs=1e-9)
    assert tst.kind == "true_stratigraphic_thickness"


def test_parity_surface_intersection_spanning_boundary() -> None:
    tst = tst_through_layers(25.0, 50.0, VERTICAL, _two_layers())
    assert tst.value == pytest.approx(25.0 + 25.0 * KS2, abs=1e-9)


def test_parity_near_parallel() -> None:
    layer = [BeddingLayer(top_md=0.0, bottom_md=100.0, normal=dipping_bed(45.0))]
    a = (45.0 + 0.1) * KPI
    w = WellDirection3D(x=math.cos(a), y=0.0, z=-math.sin(a))
    tst = tst_through_layers(0.0, 100.0, w, layer)
    assert tst.value == pytest.approx(100.0 * math.sin(0.1 * KPI), abs=1e-9)
    assert tst.value > 0.0


def test_parity_zero_tst_layer_contribution_legal() -> None:
    layers = [
        BeddingLayer(top_md=0.0, bottom_md=40.0, normal=HORIZONTAL),
        BeddingLayer(top_md=40.0, bottom_md=100.0, normal=dipping_bed(45.0)),
    ]
    w = WellDirection3D(x=KS2, y=0.0, z=-KS2)  # parallel to layer 2 bedding
    tst = tst_through_layers(0.0, 100.0, w, layers)
    assert tst.value == pytest.approx(40.0 * KS2, abs=1e-9)


def test_parity_extreme_dip_inside_one_layer() -> None:
    layers = [
        BeddingLayer(top_md=0.0, bottom_md=50.0, normal=dipping_bed(89.0)),
        BeddingLayer(top_md=50.0, bottom_md=100.0, normal=HORIZONTAL),
    ]
    tst = tst_through_layers(0.0, 100.0, VERTICAL, layers)
    assert tst.value == pytest.approx(50.0 * math.cos(89.0 * KPI) + 50.0, abs=1e-9)


def test_parity_polyline_exact_geometry() -> None:
    layers = [
        BeddingLayer(top_md=0.0, bottom_md=60.0, normal=HORIZONTAL),
        BeddingLayer(top_md=60.0, bottom_md=100.0, normal=dipping_bed(45.0)),
    ]
    s = 50.0 * KS2
    path = [
        PathPoint3D(md=0.0, x=0.0, y=0.0, z=0.0),
        PathPoint3D(md=50.0, x=0.0, y=0.0, z=50.0),
        PathPoint3D(md=100.0, x=s, y=0.0, z=50.0 + s),
    ]
    tst = tst_along_path(path, layers)
    assert tst.value == pytest.approx(50.0 + 10.0 * KS2 + 40.0, abs=1e-9)
    assert tst.measured_interval_m == 100.0


def test_parity_degenerate_single_layer_matches_planar_form() -> None:
    single = [
        BeddingLayer(top_md=-1000.0, bottom_md=1000.0, normal=HORIZONTAL)
    ]
    layered = tst_through_layers(0.0, 100.0, VERTICAL, single)
    expected = 100.0  # TST = L·|w·n| with w·n = 1
    assert layered.value == pytest.approx(expected, abs=1e-12)


def test_parity_empty_layer_book_and_zero_length() -> None:
    tst = tst_through_layers(0.0, 100.0, VERTICAL, [])
    assert tst.value == 0.0 and tst.measured_interval_m == 100.0
    zero = tst_through_layers(10.0, 0.0, VERTICAL, _two_layers())
    assert zero.value == 0.0


# ---------------------------------------------------------------------------
# Validation mirror (every C++ input-error case raises ValueError)
# ---------------------------------------------------------------------------


def test_validation_errors_raise() -> None:
    with pytest.raises(ValueError):
        tst_through_layers(0.0, -5.0, VERTICAL, _two_layers())
    with pytest.raises(ValueError):
        tst_through_layers(0.0, math.inf, VERTICAL, _two_layers())
    with pytest.raises(ValueError):
        tst_through_layers(0.0, 100.0, WellDirection3D(z=2.0), _two_layers())
    with pytest.raises(ValueError):
        tst_through_layers(
            0.0, 100.0, VERTICAL,
            [BeddingLayer(0.0, 50.0, BedNormal3D(0.0, 0.0, 0.0))],
        )
    with pytest.raises(ValueError):
        tst_through_layers(
            0.0, 100.0, VERTICAL,
            [BeddingLayer(0.0, 50.0, BedNormal3D(0.0, 0.0, 2.0))],
        )
    # Overlapping layers.
    with pytest.raises(ValueError):
        tst_through_layers(0.0, 100.0, VERTICAL, [
            BeddingLayer(0.0, 60.0, HORIZONTAL),
            BeddingLayer(50.0, 100.0, HORIZONTAL),
        ])
    # Inverted layer.
    with pytest.raises(ValueError):
        tst_through_layers(0.0, 100.0, VERTICAL,
                           [BeddingLayer(60.0, 10.0, HORIZONTAL)])
    # Zero-extent layer.
    with pytest.raises(ValueError):
        tst_through_layers(0.0, 100.0, VERTICAL,
                           [BeddingLayer(50.0, 50.0, HORIZONTAL)])
    # Out-of-order book.
    with pytest.raises(ValueError):
        tst_through_layers(0.0, 100.0, VERTICAL, [
            BeddingLayer(60.0, 100.0, HORIZONTAL),
            BeddingLayer(0.0, 50.0, HORIZONTAL),
        ])
    # Non-finite bound.
    with pytest.raises(ValueError):
        tst_through_layers(0.0, 100.0, VERTICAL,
                           [BeddingLayer(0.0, math.nan, HORIZONTAL)])


def test_validation_path_errors_raise() -> None:
    layers = [BeddingLayer(0.0, 100.0, HORIZONTAL)]
    ok = [PathPoint3D(0.0, 0.0, 0.0, 0.0), PathPoint3D(50.0, 0.0, 0.0, 50.0)]
    with pytest.raises(ValueError):
        tst_along_path(ok[:1], layers)  # single point
    with pytest.raises(ValueError):
        tst_along_path([], layers)
    # Non-increasing md.
    with pytest.raises(ValueError):
        tst_along_path(
            [PathPoint3D(50.0, 0.0, 0.0, 0.0), PathPoint3D(50.0, 0.0, 0.0, 50.0)],
            layers,
        )
    # Zero-length leg with increasing md (inconsistent path).
    with pytest.raises(ValueError):
        tst_along_path(
            [PathPoint3D(0.0, 1.0, 2.0, 3.0), PathPoint3D(50.0, 1.0, 2.0, 3.0)],
            layers,
        )
    # Non-finite point.
    with pytest.raises(ValueError):
        tst_along_path(
            [PathPoint3D(0.0, 0.0, 0.0, 0.0), PathPoint3D(50.0, 0.0, 0.0, math.nan)],
            layers,
        )
    # Invalid layers propagate.
    with pytest.raises(ValueError):
        tst_along_path(ok, [
            BeddingLayer(0.0, 60.0, HORIZONTAL),
            BeddingLayer(50.0, 100.0, HORIZONTAL),
        ])


# ---------------------------------------------------------------------------
# Orientation convention
# ---------------------------------------------------------------------------


def test_normal_from_dip_azimuth_convention() -> None:
    n0 = normal_from_dip_azimuth(45.0, 0.0)
    assert (n0.x, n0.y, n0.z) == pytest.approx((KS2, 0.0, KS2))
    n90 = normal_from_dip_azimuth(45.0, 90.0)
    assert (n90.x, n90.y, n90.z) == pytest.approx((0.0, KS2, KS2))
    flat = normal_from_dip_azimuth(0.0, 120.0)
    assert (flat.x, flat.y, flat.z) == pytest.approx((0.0, 0.0, 1.0))
    for n in (n0, n90, flat):
        assert math.sqrt(n.x**2 + n.y**2 + n.z**2) == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# Binding-first dispatch
# ---------------------------------------------------------------------------


def test_binding_first_dispatch(monkeypatch: pytest.MonkeyPatch) -> None:
    _BINDING_CACHE.clear()

    class _FakeWellLog(SimpleNamespace):
        def tst_through_layers(self, start, length, direction, layers):
            return SimpleNamespace(
                value=123.0,
                measured_interval_m=length,
                normal_dot=1.23,
            )

    monkeypatch.setitem(sys.modules, "welllog", _FakeWellLog())
    try:
        result = tst_through_layers(0.0, 100.0, VERTICAL, _two_layers())
        assert result.value == 123.0  # binding wins when present
        assert result.normal_dot == 1.23
    finally:
        monkeypatch.delitem(sys.modules, "welllog", raising=False)
        _BINDING_CACHE.clear()


def test_fallback_without_binding() -> None:
    _BINDING_CACHE.clear()
    # The real welllog module (if importable) has no tst bindings yet, or is
    # absent — the mirror must produce the parity value either way.
    result = tst_through_layers(0.0, 100.0, VERTICAL, _two_layers())
    assert result.value == pytest.approx(50.0 + 50.0 * KS2, abs=1e-9)


# ---------------------------------------------------------------------------
# Survey path wiring
# ---------------------------------------------------------------------------


def test_path_from_trajectory() -> None:
    traj = compute_trajectory(
        [
            SurveyStation(md=0.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=50.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=100.0, inc_deg=45.0, az_deg=0.0),
        ]
    )
    path = path_from_trajectory(traj)
    assert [p.md for p in path] == [0.0, 50.0, 100.0]
    assert path[0].z == pytest.approx(0.0)
    assert path[1].z == pytest.approx(50.0)
    # Every path point mirrors the trajectory arrays (x=N, y=E, z=TVD down).
    assert path[2].x == pytest.approx(float(traj.north[2]), abs=1e-9)
    assert path[2].y == pytest.approx(float(traj.east[2]), abs=1e-9)
    assert path[2].z == pytest.approx(float(traj.tvd[2]), abs=1e-9)
    # Second leg chords the minimum-curvature arc (not a straight 45° line);
    # with dogleg 45° the ratio factor RF = (2/DL)·tan(DL/2) ≈ 1.0548.
    dx = path[2].x - path[1].x
    dz = path[2].z - path[1].z
    rf = (2.0 / (45.0 * KPI)) * math.tan(45.0 * KPI / 2.0)
    assert dx == pytest.approx(25.0 * KS2 * rf, abs=1e-9)
    assert dz == pytest.approx(25.0 * (1.0 + KS2) * rf, abs=1e-9)


def test_path_from_trajectory_skips_non_monotonic_md() -> None:
    traj = compute_trajectory(
        [
            SurveyStation(md=0.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=40.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=40.0, inc_deg=5.0, az_deg=0.0),  # carried forward
            SurveyStation(md=90.0, inc_deg=5.0, az_deg=0.0),
        ]
    )
    path = path_from_trajectory(traj)
    assert [p.md for p in path] == [0.0, 40.0, 90.0]


def test_end_to_end_survey_and_bedding() -> None:
    """Vertical well + bedding sidecar → hand-computed TST."""
    traj = compute_trajectory(
        [
            SurveyStation(md=0.0, inc_deg=0.0, az_deg=0.0),
            SurveyStation(md=100.0, inc_deg=0.0, az_deg=0.0),
        ]
    )
    path = path_from_trajectory(traj)
    layers = [
        BeddingLayer(0.0, 40.0, HORIZONTAL),
        BeddingLayer(40.0, 100.0, dipping_bed(45.0)),
    ]
    tst = tst_along_path(path, layers)
    assert tst.value == pytest.approx(40.0 + 60.0 * KS2, abs=1e-9)


# ---------------------------------------------------------------------------
# Bedding sidecar
# ---------------------------------------------------------------------------


def test_bedding_sidecar_roundtrip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws")
    add_well(ws, name="W1", path="wells/w1.las", well_id="w1")
    specs = [
        BeddingLayerSpec(top_md=0.0, bottom_md=40.0, dip_deg=0.0, unit_id="e1"),
        BeddingLayerSpec(
            top_md=40.0, bottom_md=100.0, dip_deg=45.0, dip_azimuth_deg=30.0,
            source="well-report",
        ),
    ]
    save_bedding_for_well(ws, "w1", specs)
    loaded, diags = load_bedding_for_well(ws, "w1")
    assert not diags
    assert loaded == specs
    # The spec → layer conversion carries the orientation convention.
    layer = loaded[1].to_layer()
    n = normal_from_dip_azimuth(45.0, 30.0)
    assert (layer.normal.x, layer.normal.y, layer.normal.z) == pytest.approx(
        (n.x, n.y, n.z)
    )
    # Unit reference survives.
    assert loaded[0].unit_id == "e1"


def test_bedding_sidecar_tolerant_read(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws")
    add_well(ws, name="W1", path="wells/w1.las", well_id="w1")
    # Missing file → empty.
    assert load_bedding_for_well(ws, "w1") == ([], [])
    # Corrupt JSON → diagnostic.
    p = ws.wells_dir / "w1"
    p.mkdir(parents=True, exist_ok=True)
    (p / BEDDING_FILENAME).write_text("{not json", encoding="utf-8")
    specs, diags = load_bedding_for_well(ws, "w1")
    assert specs == [] and diags
    # Invalid items are skipped with diagnostics; valid ones survive.
    (p / BEDDING_FILENAME).write_text(
        json.dumps({
            "schemaVersion": 1,
            "layers": [
                {"top_md": "x", "bottom_md": 10.0, "dip_deg": 0.0},
                {"top_md": 0.0, "bottom_md": 20.0, "dip_deg": 5.0},
                {"top_md": 30.0, "bottom_md": 10.0, "dip_deg": 5.0},
            ],
        }),
        encoding="utf-8",
    )
    specs, diags = load_bedding_for_well(ws, "w1")
    assert len(specs) == 1 and specs[0].top_md == 0.0
    assert len(diags) == 2
