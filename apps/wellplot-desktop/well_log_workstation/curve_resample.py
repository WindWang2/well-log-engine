"""Explicit curve resampling as a derived operation (Epic A, FRS §3.x 多采样率).

Resampling is never implicit at ingest: source samples keep their own axis,
and a derived curve is produced on demand onto a regular target axis. The
derived curve carries its own ``depth`` (per-curve sampling axis), its own
version label, and its values are linearly interpolated from the source —
gaps (null / NaN) propagate as gaps, never as fabricated values.

Definitions persist per well as ``wells/<id>/curve_resamples.json`` (mirror
``curve_edits.json``): the samples are recomputed at load time, the source
buffer is never rewritten.

Pure numpy — headless testable.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from well_log_workstation.tops_model import well_data_dir
from well_log_workstation.workspace import Workspace


@dataclass(frozen=True)
class CurveResample:
    """Definition of one resampled (derived) curve version."""

    mnemonic: str
    interval: float  # target sampling step in the depth unit (must be > 0)
    version: str  # e.g. "resample-0.5m"

    @property
    def display_name(self) -> str:
        return f"{self.mnemonic} ({self.version})"


def resample_curve(
    depth: Any,
    values: Any,
    null_mask: Any | None,
    target_interval: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Resample onto a regular axis ``[min_depth, max_depth]`` at the interval.

    Args:
        depth: source sampling coordinates (monotonic).
        values: source samples (sample-aligned with ``depth``).
        null_mask: optional boolean array; True = missing.
        target_interval: positive step of the derived regular axis.

    Returns:
        ``(new_depth, new_values, new_null_mask)`` — the derived curve's own
        axis and samples. Missing source samples (null or non-finite) make the
        interpolation produce NaN at every derived sample whose support spans
        them, so gaps never turn into fabricated values.
    """
    depth = np.asarray(depth, dtype=np.float64)
    values = np.asarray(values, dtype=np.float64)
    if depth.size < 2 or values.size != depth.size:
        raise ValueError("重采样需要至少 2 个与深度对齐的样本")
    if not math.isfinite(float(target_interval)) or target_interval <= 0.0:
        raise ValueError("目标采样间隔必须为正的有限值")
    d0 = float(np.nanmin(depth))
    d1 = float(np.nanmax(depth))
    if not math.isfinite(d0) or not math.isfinite(d1) or d1 <= d0:
        raise ValueError("深度范围无效（非有限或为空）")
    work = values.astype(np.float64, copy=True)
    if null_mask is not None:
        work[np.asarray(null_mask, dtype=bool)] = np.nan
    new_depth = np.arange(d0, d1 + target_interval / 2.0, target_interval)
    new_values = np.interp(
        new_depth, depth, work, left=np.nan, right=np.nan
    )
    new_null = ~np.isfinite(new_values)
    return new_depth, new_values, new_null


# ---------------------------------------------------------------------------
# Persistence (definitions only — samples are recomputed at load)
# ---------------------------------------------------------------------------

RESAMPLE_FILENAME = "curve_resamples.json"
RESAMPLE_SCHEMA_VERSION = 1


def curve_resamples_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / RESAMPLE_FILENAME


def load_curve_resamples_for_well(
    workspace: Workspace, well_id: str
) -> tuple[list[CurveResample], list[str]]:
    """Load persisted resample definitions; missing/corrupt → ([], [diag])."""
    diagnostics: list[str] = []
    try:
        path = curve_resamples_path(workspace, well_id)
    except Exception as exc:  # workspace may not be fully initialised
        return [], [str(exc)]
    if not path.is_file():
        return [], []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"重采样定义损坏: {exc}"]
    items = raw.get("resamples") if isinstance(raw, dict) else raw
    if not isinstance(items, list):
        return [], ["重采样定义格式无效"]
    out: list[CurveResample] = []
    for item in items:
        if not isinstance(item, dict):
            continue
        try:
            interval = float(item.get("interval", 0.0))
            mnemonic = str(item.get("mnemonic") or "")
            version = str(item.get("version") or "")
            if not mnemonic or not version or interval <= 0.0:
                continue
            out.append(CurveResample(mnemonic=mnemonic, interval=interval, version=version))
        except (TypeError, ValueError):
            continue
    return out, diagnostics


def save_curve_resamples_for_well(
    workspace: Workspace, well_id: str, resamples: list[CurveResample]
) -> Path:
    """Persist resample definitions next to the well data (atomic write)."""
    path = curve_resamples_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": RESAMPLE_SCHEMA_VERSION,
        "well_id": well_id,
        "resamples": [
            {
                "mnemonic": r.mnemonic,
                "interval": float(r.interval),
                "version": r.version,
            }
            for r in resamples
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path


def apply_resamples_to_document(
    document: Any, resamples: list[CurveResample]
) -> list[str]:
    """Derive resampled curves onto the in-memory well document (no file IO).

    Appends one ``ImportedCurve`` per definition whose source curve exists;
    an existing version with the same label is replaced in place (re-derive).
    Returns diagnostics for skipped definitions.
    """
    from well_log_workstation.las_import import ImportedCurve

    diagnostics: list[str] = []
    for spec in resamples:
        curve = document.curve_by_mnemonic(spec.mnemonic)
        if curve is None:
            diagnostics.append(f"{spec.mnemonic}: 井中无此曲线，跳过重采样")
            continue
        try:
            new_depth, new_values, new_null = resample_curve(
                curve.depth if getattr(curve, "depth", None) is not None else document.depth,
                curve.values,
                curve.null_mask,
                spec.interval,
            )
        except ValueError as exc:
            diagnostics.append(f"{spec.mnemonic}: {exc}")
            continue
        derived = ImportedCurve(
            mnemonic=curve.mnemonic,
            unit=curve.unit,
            values=new_values,
            null_mask=new_null,
            depth=new_depth,
            version=spec.version,
        )
        replaced = False
        for i, existing in enumerate(document.curves):
            if (
                existing.mnemonic == curve.mnemonic
                and getattr(existing, "version", "raw") == spec.version
            ):
                document.curves[i] = derived
                replaced = True
                break
        if not replaced:
            document.curves.append(derived)
    return diagnostics
