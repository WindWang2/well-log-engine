"""True stratigraphic thickness (Epic D) — Desktop side.

The SDK (``scene/tst.hpp``, C++) is the single source of truth for the
piecewise-planar TST model; this module mirrors its exact semantics
(``tst_through_layers`` / ``tst_along_path``, validation included) so the
workstation runs without the Python binding, and prefers the binding when it
is importable (resolver pattern, same as ``depth_ruler``). The mirror is
locked to the C++ behaviour by parity tests that assert the *same* analytic
fixture values as ``tests/integration/tst_layers_test.cpp``.

Conventions (documented in tst.hpp):
* coordinates: x = north, y = east, z = down / increasing depth (TVD);
* dip_deg measured from horizontal (0 = flat, 90 = vertical); dip_azimuth_deg
  is the compass azimuth of the dip direction (direction of maximum dip);
  normal = (sin δ·cos φ, sin δ·sin φ, cos δ) — with φ = 0 this degenerates to
  the (sin δ, 0, cos δ) convention of the C++ fixtures;
* bedding layers are (top_md, bottom_md) intervals along the well path, each
  with its own unit normal; layers must be ordered, non-overlapping and
  strictly positive in extent; intervals outside every layer contribute
  nothing (undeclared bedding is the caller's responsibility);
* zero TST is a VALID result (well parallel to bedding); every input
  violation raises ValueError — never a silent default or merge.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

from well_log_workstation.survey import SurveyTrajectory
from well_log_workstation.tops_model import well_data_dir
from well_log_workstation.workspace import Workspace

BEDDING_FILENAME = "bedding.json"
BEDDING_SCHEMA_VERSION = 1

# Mirror of the C++ ThicknessKind — the API never returns a bare thickness.
KIND_TST = "true_stratigraphic_thickness"

_UNIT_TOLERANCE = 1e-6


# ---------------------------------------------------------------------------
# Mirrors of the SDK value structs (tst.hpp)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class WellDirection3D:
    """Unit vector along the wellbore (Z = down / increasing depth)."""

    x: float = 0.0
    y: float = 0.0
    z: float = 1.0


@dataclass(frozen=True)
class BedNormal3D:
    """Unit normal of the local bedding plane."""

    x: float = 0.0
    y: float = 0.0
    z: float = 1.0


@dataclass(frozen=True)
class BeddingLayer:
    """One layer of a piecewise-planar bedding model (MD interval + normal)."""

    top_md: float
    bottom_md: float
    normal: BedNormal3D


@dataclass(frozen=True)
class PathPoint3D:
    """A point on the wellbore path: measured depth + 3D position."""

    md: float
    x: float
    y: float
    z: float


@dataclass(frozen=True)
class TrueStratigraphicThickness:
    """Mirror of the SDK result struct."""

    value: float
    kind: str = KIND_TST
    measured_interval_m: float = 0.0
    normal_dot: float = 0.0


# ---------------------------------------------------------------------------
# Orientation helpers
# ---------------------------------------------------------------------------


def normal_from_dip_azimuth(dip_deg: float, dip_azimuth_deg: float) -> BedNormal3D:
    """Bedding unit normal from dip (from horizontal) and dip azimuth.

    ``dip_azimuth_deg`` is the compass azimuth of the dip direction; with
    azimuth 0 the normal is (sin δ, 0, cos δ) — the C++ fixture convention.
    """
    d = math.radians(float(dip_deg))
    a = math.radians(float(dip_azimuth_deg))
    return BedNormal3D(
        x=math.sin(d) * math.cos(a),
        y=math.sin(d) * math.sin(a),
        z=math.cos(d),
    )


# ---------------------------------------------------------------------------
# The mirror implementation (locked to tst_layers_test.cpp by parity tests)
# ---------------------------------------------------------------------------


def _unit_vector(x: float, y: float, z: float) -> float:
    """Norm of (x, y, z) when it is a finite unit vector, else ValueError."""
    if not all(math.isfinite(v) for v in (x, y, z)):
        raise ValueError("向量必须有限")
    norm = math.sqrt(x * x + y * y + z * z)
    if not (norm > 0.0) or abs(norm - 1.0) > _UNIT_TOLERANCE:
        raise ValueError("向量必须是单位向量")
    return norm


def _valid_layers(layers: Sequence[BeddingLayer]) -> None:
    """Validate the BeddingLayer contract; raises ValueError on violation."""
    prev_bottom_md = 0.0
    for i, layer in enumerate(layers):
        if not math.isfinite(layer.top_md) or not math.isfinite(layer.bottom_md):
            raise ValueError(f"层 {i}: 深度边界必须有限")
        if not (layer.bottom_md > layer.top_md):
            raise ValueError(f"层 {i}: 厚度必须为正（倒置/零厚度层无效）")
        _unit_vector(layer.normal.x, layer.normal.y, layer.normal.z)
        if i > 0 and layer.top_md < prev_bottom_md:
            raise ValueError(f"层 {i}: 层序必须有序且不重叠")
        prev_bottom_md = layer.bottom_md


def _accumulate_layers(
    start_md: float, end_md: float, w: WellDirection3D,
    layers: Sequence[BeddingLayer],
) -> float:
    """Σ overlap(interval, layer) · |w·n| over the declared layers."""
    total = 0.0
    for layer in layers:
        lo = max(start_md, layer.top_md)
        hi = min(end_md, layer.bottom_md)
        if not (hi > lo):
            continue
        dot = abs(
            w.x * layer.normal.x + w.y * layer.normal.y + w.z * layer.normal.z
        )
        total += (hi - lo) * dot
    return total


def _mirror_tst_through_layers(
    start_md: float,
    measured_length_m: float,
    well_direction: WellDirection3D,
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """Exact mirror of ``tst_through_layers`` (tst.hpp)."""
    if not math.isfinite(start_md) or not math.isfinite(measured_length_m):
        raise ValueError("起始深度与长度必须有限")
    if measured_length_m < 0.0:
        raise ValueError("长度不能为负")
    _unit_vector(well_direction.x, well_direction.y, well_direction.z)
    _valid_layers(layers)
    total = _accumulate_layers(
        start_md, start_md + measured_length_m, well_direction, layers
    )
    return TrueStratigraphicThickness(
        value=total,
        measured_interval_m=measured_length_m,
        normal_dot=total / measured_length_m if measured_length_m > 0.0 else 0.0,
    )


def _mirror_tst_along_path(
    path: Sequence[PathPoint3D],
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """Exact mirror of ``tst_along_path`` (tst.hpp)."""
    if len(path) < 2:
        raise ValueError("路径至少需要两个点")
    _valid_layers(layers)
    total = 0.0
    total_md = 0.0
    for i in range(1, len(path)):
        a, b = path[i - 1], path[i]
        if not all(
            math.isfinite(v)
            for v in (a.md, a.x, a.y, a.z, b.md, b.x, b.y, b.z)
        ):
            raise ValueError("路径点必须有限")
        if not (b.md > a.md):
            raise ValueError("路径 MD 必须严格递增")
        dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
        leg_length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if not (leg_length > 0.0):
            raise ValueError("零长度腿（MD 递增但位置不变）路径不一致")
        leg = WellDirection3D(x=dx / leg_length, y=dy / leg_length, z=dz / leg_length)
        total += _accumulate_layers(a.md, b.md, leg, layers)
        total_md += b.md - a.md
    return TrueStratigraphicThickness(
        value=total,
        measured_interval_m=total_md,
        normal_dot=total / total_md,  # total_md > 0 by the md rule
    )


# ---------------------------------------------------------------------------
# Binding-first dispatch (SDK authoritative; mirror is the fallback)
# ---------------------------------------------------------------------------

_BINDING_CACHE: dict[str, object | None] = {}


def _binding_fn(name: str) -> object | None:
    """Resolve an SDK module-level binding function by name (cached)."""
    if name in _BINDING_CACHE:
        return _BINDING_CACHE[name]
    fn = None
    try:
        import welllog  # type: ignore

        candidate = getattr(welllog, name, None)
        if not callable(candidate):
            ext = getattr(welllog, "_QtWidgets", None)
            inner = getattr(ext, "welllog", None) if ext is not None else None
            if inner is not None:
                candidate = getattr(inner, name, None)
        if callable(candidate):
            fn = candidate
    except Exception:
        fn = None
    _BINDING_CACHE[name] = fn
    return fn


def _call_binding(name: str, *args: Any) -> Any | None:
    fn = _binding_fn(name)
    if fn is None:
        return None
    try:
        return fn(*args)
    except Exception:
        return None


def tst_through_layers(
    start_md: float,
    measured_length_m: float,
    well_direction: WellDirection3D,
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """TST of a measured interval along a straight leg (binding-first)."""
    bound = _call_binding(
        "tst_through_layers", start_md, measured_length_m,
        well_direction, list(layers),
    )
    if bound is not None:
        return _result_from_bound(bound)
    return _mirror_tst_through_layers(
        start_md, measured_length_m, well_direction, layers
    )


def tst_along_path(
    path: Sequence[PathPoint3D],
    layers: Sequence[BeddingLayer],
) -> TrueStratigraphicThickness:
    """TST along a polyline path (binding-first)."""
    bound = _call_binding("tst_along_path", list(path), list(layers))
    if bound is not None:
        return _result_from_bound(bound)
    return _mirror_tst_along_path(path, layers)


def _result_from_bound(bound: Any) -> TrueStratigraphicThickness:
    """Normalize a binding result (unknown shape) to the mirror result."""
    if isinstance(bound, TrueStratigraphicThickness):
        return bound
    try:
        value = float(bound.value if hasattr(bound, "value") else bound[0])
        measured = float(
            getattr(bound, "measured_interval_m", 0.0)
            if hasattr(bound, "measured_interval_m")
            else (bound[2] if len(bound) > 2 else 0.0)
        )
        dot = float(
            getattr(bound, "normal_dot", 0.0)
            if hasattr(bound, "normal_dot")
            else (bound[3] if len(bound) > 3 else 0.0)
        )
    except (TypeError, ValueError, IndexError):
        return _EMPTY_TST
    return TrueStratigraphicThickness(
        value=value, measured_interval_m=measured, normal_dot=dot
    )


_EMPTY_TST = TrueStratigraphicThickness(value=0.0)


# ---------------------------------------------------------------------------
# Well-path construction from the deviation survey
# ---------------------------------------------------------------------------


def path_from_trajectory(traj: SurveyTrajectory) -> list[PathPoint3D]:
    """PathPoint3D list from a computed trajectory (x=N, y=E, z=TVD down).

    Every survey station becomes a path point; consecutive stations form
    straight chord legs (the documented polyline approximation). Stations
    whose MD does not increase are skipped (carried forward by the survey
    computation) so the path stays monotonic.
    """
    out: list[PathPoint3D] = []
    md = np.asarray(traj.md, dtype=np.float64)
    for i in range(int(md.size)):
        pt = PathPoint3D(
            md=float(md[i]),
            x=float(traj.north[i]),
            y=float(traj.east[i]),
            z=float(traj.tvd[i]),
        )
        if out and not (pt.md > out[-1].md):
            continue
        out.append(pt)
    return out


# ---------------------------------------------------------------------------
# Bedding sidecar (wells/<id>/bedding.json) — mirrors the layer contract
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class BeddingLayerSpec:
    """Persisted bedding layer: MD interval + dip/azimuth (+ optional unit)."""

    top_md: float
    bottom_md: float
    dip_deg: float
    dip_azimuth_deg: float = 0.0
    unit_id: str = ""  # → stratigraphic dictionary (C1); empty = free
    source: str = ""

    def to_layer(self) -> BeddingLayer:
        return BeddingLayer(
            top_md=self.top_md,
            bottom_md=self.bottom_md,
            normal=normal_from_dip_azimuth(self.dip_deg, self.dip_azimuth_deg),
        )


def bedding_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / BEDDING_FILENAME


def _spec_from_json(item: dict[str, Any]) -> BeddingLayerSpec | None:
    try:
        top = float(item["top_md"])
        bottom = float(item["bottom_md"])
    except (KeyError, TypeError, ValueError):
        return None
    if not (math.isfinite(top) and math.isfinite(bottom)) or not (bottom > top):
        return None
    try:
        dip = float(item.get("dip_deg") or 0.0)
        az = float(item.get("dip_azimuth_deg") or 0.0)
    except (TypeError, ValueError):
        return None
    return BeddingLayerSpec(
        top_md=top,
        bottom_md=bottom,
        dip_deg=dip,
        dip_azimuth_deg=az,
        unit_id=str(item.get("unit_id") or ""),
        source=str(item.get("source") or ""),
    )


def load_bedding_for_well(
    workspace: Workspace, well_id: str
) -> tuple[list[BeddingLayerSpec], list[str]]:
    """Load the well bedding layers; missing/corrupt → ([], diagnostics)."""
    diagnostics: list[str] = []
    try:
        path = bedding_file_path(workspace, well_id)
    except Exception as exc:
        return [], [str(exc)]
    if not path.is_file():
        return [], []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"产状数据损坏: {exc}"]
    items = raw.get("layers") if isinstance(raw, dict) else raw
    if not isinstance(items, list):
        return [], ["产状数据格式无效"]
    specs: list[BeddingLayerSpec] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            diagnostics.append(f"跳过无效产状项 [{i}]")
            continue
        spec = _spec_from_json(item)
        if spec is None:
            diagnostics.append(f"跳过无效产状项 [{i}]（需 top_md/bottom_md 且 bottom>top）")
            continue
        specs.append(spec)
    specs.sort(key=lambda s: (s.top_md, s.bottom_md))
    return specs, diagnostics


def save_bedding_for_well(
    workspace: Workspace, well_id: str, specs: Iterable[BeddingLayerSpec]
) -> Path:
    """Persist the bedding layers beside the well data (atomic write)."""
    path = bedding_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": BEDDING_SCHEMA_VERSION,
        "well_id": well_id,
        "layers": [
            {
                "top_md": s.top_md,
                "bottom_md": s.bottom_md,
                "dip_deg": s.dip_deg,
                "dip_azimuth_deg": s.dip_azimuth_deg,
                **({"unit_id": s.unit_id} if s.unit_id else {}),
                **({"source": s.source} if s.source else {}),
            }
            for s in specs
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path
