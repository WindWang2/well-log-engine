"""Epic D slice 3 — Desktop TST wiring (well_log_workstation.tst).

Covers:
* parity — the Python mirror asserts the SAME analytic fixture values as
  tests/integration/tst_layers_test.cpp (the SDK is the single source of
  truth; the mirror is locked to it by shared fixtures);
* surface parity — tst_along_surface_path asserts the SAME values as
  tests/integration/tst_surfaces_test.cpp;
* validation mirror (ValueError on every C++ input-error case);
* normal_from_dip_azimuth convention (φ=0 degenerates to the C++ convention);
* binding-first dispatch (fake welllog module wins when present, mirror
  otherwise);
* path_from_trajectory (survey → PathPoint3D, monotonic filtering);
* bedding.json sidecar round-trip + tolerant read (v1 planar + v2 surface);
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
    SurfaceGrid,
    SurfaceGridSpec,
    WellDirection3D,
    _BINDING_CACHE,
    layer_surfaces,
    load_bedding_for_well,
    normal_from_dip_azimuth,
    path_from_trajectory,
    save_bedding_for_well,
    tst_along_path,
    tst_along_surface_path,
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


# ---------------------------------------------------------------------------
# Surface parity — SAME analytic values as tests/integration/tst_surfaces_test.cpp
# ---------------------------------------------------------------------------


def _grid_const(g, height: float) -> SurfaceGrid:
    x0, y0, xs, ys, xn, yn = g
    return SurfaceGrid(
        x_origin_m=x0, y_origin_m=y0, x_step_m=xs, y_step_m=ys,
        x_nodes=xn, y_nodes=yn, z_tvd=[height] * (xn * yn),
    )


def _grid_from(g, f) -> SurfaceGrid:
    x0, y0, xs, ys, xn, yn = g
    z = [
        f(x0 + i * xs, y0 + j * ys)
        for j in range(yn)
        for i in range(xn)
    ]
    return SurfaceGrid(
        x_origin_m=x0, y_origin_m=y0, x_step_m=xs, y_step_m=ys,
        x_nodes=xn, y_nodes=yn, z_tvd=z,
    )


# x, y ∈ [0, 400] with x step 100 — the plane-fixture grid.
PLANE_GRID = (0.0, 0.0, 100.0, 100.0, 5, 2)


def _sine_mesh(g) -> SurfaceGrid:
    """Fold sampled on a coarse sine: z = 100, 110, 100, 90, 100 (y-const)."""
    heights = [100.0, 110.0, 100.0, 90.0, 100.0]
    return SurfaceGrid(
        x_origin_m=g[0], y_origin_m=g[1], x_step_m=g[2], y_step_m=g[3],
        x_nodes=g[4], y_nodes=g[5], z_tvd=heights * g[5],
    )


def test_surface_parity_horizontal_planes() -> None:
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    surfaces = [_grid_const(g, 100.0), _grid_const(g, 300.0)]
    path = [
        PathPoint3D(md=0.0, x=10.0, y=10.0, z=0.0),
        PathPoint3D(md=1000.0, x=10.0, y=10.0, z=1000.0),
    ]
    tst = tst_along_surface_path(path, surfaces)
    assert tst.value == pytest.approx(200.0, abs=1e-9)
    assert tst.measured_interval_m == pytest.approx(1000.0, abs=1e-9)
    assert tst.normal_dot == pytest.approx(0.2, abs=1e-9)
    assert tst.kind == "true_stratigraphic_thickness"


def test_surface_parity_dipping_plane_non_node() -> None:
    f = lambda x, _y: 0.25 * x + 100.0
    fb = lambda x, _y: 0.25 * x + 300.0
    surfaces = [_grid_from(PLANE_GRID, f), _grid_from(PLANE_GRID, fb)]
    # Well at x = 175 (between grid nodes), y = 50.
    path = [
        PathPoint3D(md=0.0, x=175.0, y=50.0, z=0.0),
        PathPoint3D(md=1000.0, x=175.0, y=50.0, z=1000.0),
    ]
    tst = tst_along_surface_path(path, surfaces)
    cos_dip = 1.0 / math.sqrt(1.0625)
    assert tst.value == pytest.approx(200.0 * cos_dip, abs=1e-9)
    assert tst.normal_dot == pytest.approx(200.0 * cos_dip / 1000.0, abs=1e-9)


def test_surface_parity_folded_stack_at_node() -> None:
    top = _sine_mesh(PLANE_GRID)
    bottom = _sine_mesh(PLANE_GRID)
    bottom = SurfaceGrid(
        x_origin_m=bottom.x_origin_m, y_origin_m=bottom.y_origin_m,
        x_step_m=bottom.x_step_m, y_step_m=bottom.y_step_m,
        x_nodes=bottom.x_nodes, y_nodes=bottom.y_nodes,
        z_tvd=[v + 200.0 for v in bottom.z_tvd],
    )
    path = [
        PathPoint3D(md=0.0, x=200.0, y=50.0, z=0.0),
        PathPoint3D(md=1000.0, x=200.0, y=50.0, z=1000.0),
    ]
    tst = tst_along_surface_path(path, [top, bottom])
    assert tst.value == pytest.approx(200.0 / math.sqrt(1.01), abs=1e-9)


def test_surface_parity_stack_with_bays() -> None:
    """3 parallel dipping planes; the path starts inside unit 0, exits and
    re-enters it upward through the bottom surface (bay), runs a
    strike-parallel leg (zero contribution), and ends inside unit 0."""
    g = (-400.0, 0.0, 100.0, 100.0, 9, 2)
    surfaces = [
        _grid_from(g, lambda x, _y: 0.25 * x + 100.0),
        _grid_from(g, lambda x, _y: 0.25 * x + 300.0),
        _grid_from(g, lambda x, _y: 0.25 * x + 500.0),
    ]
    path = [
        PathPoint3D(md=0.0, x=-400.0, y=0.0, z=100.0),
        PathPoint3D(md=400.0, x=-400.0, y=0.0, z=500.0),
        PathPoint3D(md=800.0, x=-400.0, y=0.0, z=100.0),
        PathPoint3D(md=1200.0, x=400.0, y=0.0, z=300.0),
    ]
    tst = tst_along_surface_path(path, surfaces)
    cos_dip = 1.0 / math.sqrt(1.0625)
    expected = 600.0 * cos_dip
    assert tst.value == pytest.approx(expected, abs=1e-9)
    assert tst.measured_interval_m == pytest.approx(1200.0, abs=1e-9)
    assert tst.normal_dot == pytest.approx(expected / 1200.0, abs=1e-9)


def test_surface_parity_tangency_at_crest() -> None:
    """A horizontal path at the crest level grazes the fold (d = 0 at the
    crest, d > 0 elsewhere): not a crossing, whole leg contributes with the
    midpoint normal."""
    top = _sine_mesh(PLANE_GRID)
    bottom = _grid_const(PLANE_GRID, 120.0)
    path = [
        PathPoint3D(md=0.0, x=0.0, y=50.0, z=110.0),
        PathPoint3D(md=200.0, x=200.0, y=50.0, z=110.0),
    ]
    tst = tst_along_surface_path(path, [top, bottom])
    nx = 0.1 / math.sqrt(1.01)
    nz = 1.0 / math.sqrt(1.01) + 1.0
    expected = 200.0 * nx / math.sqrt(nx * nx + nz * nz)
    assert tst.value == pytest.approx(expected, abs=1e-9)


def test_surface_parity_strike_parallel_zero_tst() -> None:
    f = lambda x, _y: 0.25 * x + 100.0
    fb = lambda x, _y: 0.25 * x + 300.0
    g = (0.0, 0.0, 100.0, 100.0, 5, 6)  # y ∈ [0, 500]
    surfaces = [_grid_from(g, f), _grid_from(g, fb)]
    path = [
        PathPoint3D(md=0.0, x=50.0, y=0.0, z=162.5),
        PathPoint3D(md=500.0, x=50.0, y=500.0, z=162.5),
    ]
    tst = tst_along_surface_path(path, surfaces)
    assert tst.value == pytest.approx(0.0, abs=1e-9)
    assert tst.normal_dot == pytest.approx(0.0, abs=1e-9)


def test_surface_parity_empty_and_single_stack_legal_zero() -> None:
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    path = [
        PathPoint3D(md=0.0, x=10.0, y=10.0, z=0.0),
        PathPoint3D(md=500.0, x=10.0, y=10.0, z=500.0),
    ]
    empty = tst_along_surface_path(path, [])
    assert empty.value == 0.0 and empty.measured_interval_m == 500.0
    single = tst_along_surface_path(path, [_grid_const(g, 100.0)])
    assert single.value == 0.0


def test_surface_crossing_at_path_node() -> None:
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    surfaces = [_grid_const(g, 200.0), _grid_const(g, 400.0)]
    # p1 lies exactly on the top surface: duplicate candidates from the two
    # adjacent legs merge into one genuine crossing.
    path = [
        PathPoint3D(md=0.0, x=50.0, y=50.0, z=0.0),
        PathPoint3D(md=200.0, x=50.0, y=50.0, z=200.0),
        PathPoint3D(md=1000.0, x=50.0, y=50.0, z=1000.0),
    ]
    tst = tst_along_surface_path(path, surfaces)
    assert tst.value == pytest.approx(200.0, abs=1e-9)


# ---------------------------------------------------------------------------
# Surface validation mirror (every C++ input-error case raises ValueError)
# ---------------------------------------------------------------------------


def test_surface_validation_errors_raise() -> None:
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    ok = [
        PathPoint3D(md=0.0, x=10.0, y=10.0, z=0.0),
        PathPoint3D(md=100.0, x=10.0, y=10.0, z=100.0),
    ]
    good = _grid_const(g, 100.0)
    good_bottom = _grid_const(g, 300.0)
    # Grid-contract errors.
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [SurfaceGrid(**{**good.__dict__, "x_nodes": 1}), good_bottom])
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [SurfaceGrid(**{**good.__dict__, "y_nodes": 1}), good_bottom])
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [SurfaceGrid(**{**good.__dict__, "x_step_m": 0.0}), good_bottom])
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [SurfaceGrid(**{**good.__dict__, "y_step_m": -50.0}), good_bottom])
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [SurfaceGrid(**{**good.__dict__, "x_origin_m": math.nan}), good_bottom])
    bad_z = SurfaceGrid(**{**good.__dict__, "z_tvd": [math.nan, *good.z_tvd[1:]]})
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [bad_z, good_bottom])
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [SurfaceGrid(**{**good.__dict__, "z_tvd": []}), good_bottom])
    # Footprint excursion.
    out_x = [
        PathPoint3D(md=0.0, x=10.0, y=10.0, z=0.0),
        PathPoint3D(md=100.0, x=250.0, y=10.0, z=100.0),
    ]
    with pytest.raises(ValueError):
        tst_along_surface_path(out_x, [good, good_bottom])
    # Inverted surfaces (bottom above top at a path point).
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [good_bottom, good])
    # Touching surfaces.
    with pytest.raises(ValueError):
        tst_along_surface_path(ok, [good, _grid_const(g, 100.0)])
    # Coincident path segment (leg lies in the top surface).
    coincident = [
        PathPoint3D(md=0.0, x=0.0, y=0.0, z=100.0),
        PathPoint3D(md=500.0, x=500.0, y=0.0, z=100.0),
    ]
    with pytest.raises(ValueError):
        tst_along_surface_path(coincident, [good, good_bottom])
    # Path validation mirrors the polyline contract.
    with pytest.raises(ValueError):
        tst_along_surface_path(ok[:1], [good, good_bottom])
    with pytest.raises(ValueError):
        tst_along_surface_path(
            [PathPoint3D(0.0, 10.0, 10.0, 0.0), PathPoint3D(0.0, 10.0, 10.0, 50.0)],
            [good, good_bottom],
        )
    with pytest.raises(ValueError):
        tst_along_surface_path(
            [PathPoint3D(0.0, 10.0, 10.0, 0.0), PathPoint3D(100.0, 10.0, 10.0, 0.0)],
            [good, good_bottom],
        )


def test_surface_validation_surfaces_crossing_between_path_points() -> None:
    """Surfaces ordered at the path points but crossing between them: the
    crossing-side check must reject (never a silent merge)."""
    g = (0.0, 0.0, 100.0, 100.0, 11, 2)  # x ∈ [0, 1000]
    top = _grid_from(g, lambda x, _y: 0.1 * x + 100.0)
    bottom = _grid_from(
        g, lambda x, _y: 0.001 * (x - 300.0) ** 2 + 0.1 * x + 95.0
    )
    path = [
        PathPoint3D(md=0.0, x=0.0, y=0.0, z=0.0),
        PathPoint3D(md=500.0, x=1000.0, y=0.0, z=500.0),
        PathPoint3D(md=1000.0, x=0.0, y=0.0, z=1000.0),
    ]
    with pytest.raises(ValueError):
        tst_along_surface_path(path, [top, bottom])


def test_surface_binding_first_dispatch(monkeypatch: pytest.MonkeyPatch) -> None:
    _BINDING_CACHE.clear()

    class _FakeWellLog(SimpleNamespace):
        def tst_along_surface_path(self, path, surfaces):
            return SimpleNamespace(value=456.0, measured_interval_m=10.0, normal_dot=4.56)

    monkeypatch.setitem(sys.modules, "welllog", _FakeWellLog())
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    path = [
        PathPoint3D(md=0.0, x=10.0, y=10.0, z=0.0),
        PathPoint3D(md=100.0, x=10.0, y=10.0, z=100.0),
    ]
    try:
        result = tst_along_surface_path(path, [_grid_const(g, 100.0)])
        assert result.value == 456.0  # binding wins when present
    finally:
        monkeypatch.delitem(sys.modules, "welllog", raising=False)
        _BINDING_CACHE.clear()


def test_surface_fallback_without_binding() -> None:
    _BINDING_CACHE.clear()
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    path = [
        PathPoint3D(md=0.0, x=10.0, y=10.0, z=0.0),
        PathPoint3D(md=1000.0, x=10.0, y=10.0, z=1000.0),
    ]
    result = tst_along_surface_path(path, [_grid_const(g, 100.0), _grid_const(g, 300.0)])
    assert result.value == pytest.approx(200.0, abs=1e-9)


# ---------------------------------------------------------------------------
# Bedding sidecar v2 — surface grids
# ---------------------------------------------------------------------------


def _surface_spec(g, height: float) -> SurfaceGridSpec:
    x0, y0, xs, ys, xn, yn = g
    return SurfaceGridSpec(
        x_origin_m=x0, y_origin_m=y0, x_step_m=xs, y_step_m=ys,
        x_nodes=xn, y_nodes=yn, z_tvd=tuple([height] * (xn * yn)),
    )


def test_bedding_sidecar_surface_roundtrip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws")
    add_well(ws, name="W1", path="wells/w1.las", well_id="w1")
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    specs = [
        BeddingLayerSpec(
            top_md=0.0, bottom_md=200.0, dip_deg=0.0,
            top_surface=_surface_spec(g, 100.0),
            bottom_surface=_surface_spec(g, 300.0),
        ),
        BeddingLayerSpec(top_md=200.0, bottom_md=400.0, dip_deg=5.0),
    ]
    save_bedding_for_well(ws, "w1", specs)
    loaded, diags = load_bedding_for_well(ws, "w1")
    assert not diags
    assert loaded == specs
    # The surface pair converts to computation-ready grids.
    pair = layer_surfaces(loaded[0])
    assert pair is not None
    top_g, bottom_g = pair
    assert top_g.z_tvd == [100.0] * 9 and bottom_g.z_tvd == [300.0] * 9
    # The planar spec has no surfaces.
    assert layer_surfaces(loaded[1]) is None


def test_layer_surfaces_inconsistent_spec() -> None:
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    one_sided = BeddingLayerSpec(
        top_md=0.0, bottom_md=200.0, dip_deg=0.0,
        top_surface=_surface_spec(g, 100.0),  # no bottom_surface
    )
    with pytest.raises(ValueError):
        layer_surfaces(one_sided)


def test_bedding_sidecar_v1_files_still_load() -> None:
    """v1 planar files (no surface keys) load as planar specs."""
    g = (0.0, 0.0, 100.0, 100.0, 3, 3)
    spec = BeddingLayerSpec(top_md=0.0, bottom_md=200.0, dip_deg=10.0)
    assert spec.top_surface is None and spec.bottom_surface is None
    assert layer_surfaces(spec) is None
