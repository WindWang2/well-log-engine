"""Non-destructive curve edits (FRS §2.x 曲线编辑 Despike / 基线平移).

Curve samples are read-only after LAS import (``las_import._freeze``), so
edits are stored as *definitions* (``wells/<id>/curve_edits.json``) and
recomputed at presentation time into ``edited-*`` tracks — mirroring the
derived-curve (formulas) mechanism. The source arrays are never mutated.

Pure numpy / dataclasses — no Qt, headless-testable.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from typing import Any, Iterable, Sequence

import numpy as np

from well_log_workstation.tops_model import well_data_dir

CURVE_EDIT_FILENAME = "curve_edits.json"
CURVE_EDIT_SCHEMA_VERSION = 1

VALID_METHODS = ("despike", "baseline")

# Despike is a median filter with a MAD threshold; a fully flat neighbourhood
# (MAD = 0) degenerates to this epsilon so constant curves stay untouched.
_EPS = 1e-12


@dataclass(frozen=True)
class CurveEdit:
    """One curve edit definition.

    ``method`` ∈ {"despike", "baseline"}:
    - despike: median-filter the curve; samples deviating more than
      ``threshold`` × MAD from the local median are replaced by the median.
    - baseline: add a constant ``shift`` to every sample.
    """

    mnemonic: str
    method: str
    window: int = 5
    threshold: float = 3.0
    shift: float = 0.0

    def __post_init__(self) -> None:
        if self.method not in VALID_METHODS:
            raise ValueError(f"未知编辑方法: {self.method}")
        if not self.mnemonic.strip():
            raise ValueError("曲线助记符不能为空")


# ---------------------------------------------------------------------------
# Algorithms (pure numpy)
# ---------------------------------------------------------------------------


def despike(
    values: np.ndarray,
    null_mask: np.ndarray,
    *,
    window: int = 5,
    threshold: float = 3.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Median despike: replace outliers with the local median.

    For each sample, the neighbourhood (half-width ``window//2`` on each
    side, shrunk at the edges) contributes its median ``med`` and MAD; a
    sample with ``|v - med| > threshold * max(MAD, eps)`` is replaced by
    ``med``. Null samples stay null. Returns ``(values, null_mask)`` copies.
    """
    vals = np.asarray(values, dtype=np.float64).copy()
    mask = np.asarray(null_mask, dtype=bool).copy()
    n = vals.shape[0]
    if n == 0:
        return vals, mask
    half = max(1, int(window) // 2)
    th = max(float(threshold), 0.0)
    for i in range(n):
        if mask[i]:
            continue
        lo, hi = max(0, i - half), min(n, i + half + 1)
        nb = vals[lo:hi][~mask[lo:hi]]
        if nb.size < 3:
            continue
        med = float(np.median(nb))
        mad = float(np.median(np.abs(nb - med)))
        scale = max(mad, _EPS)
        if abs(float(vals[i]) - med) > th * scale:
            vals[i] = med
    return vals, mask


def apply_baseline(
    values: np.ndarray,
    null_mask: np.ndarray,
    shift: float,
) -> tuple[np.ndarray, np.ndarray]:
    """Add a constant shift; null samples stay null."""
    vals = np.asarray(values, dtype=np.float64).copy()
    mask = np.asarray(null_mask, dtype=bool).copy()
    vals[~mask] = vals[~mask] + float(shift)
    return vals, mask


def apply_curve_edits(
    values: np.ndarray,
    null_mask: np.ndarray,
    edits: Sequence[CurveEdit],
) -> tuple[np.ndarray, np.ndarray]:
    """Apply edits in order; each output feeds the next."""
    vals = np.asarray(values, dtype=np.float64)
    mask = np.asarray(null_mask, dtype=bool)
    for edit in edits:
        if edit.mnemonic == "":
            continue
        if edit.method == "despike":
            vals, mask = despike(vals, mask, window=edit.window, threshold=edit.threshold)
        elif edit.method == "baseline":
            vals, mask = apply_baseline(vals, mask, edit.shift)
    return np.ascontiguousarray(vals), np.ascontiguousarray(mask)


# ---------------------------------------------------------------------------
# JSON roundtrip
# ---------------------------------------------------------------------------


def edits_to_json(edits: Iterable[CurveEdit]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for e in edits:
        if not e.mnemonic.strip() or e.method not in VALID_METHODS:
            continue
        row: dict[str, Any] = {
            "mnemonic": str(e.mnemonic),
            "method": str(e.method),
        }
        if e.method == "despike":
            row["window"] = int(e.window)
            row["threshold"] = float(e.threshold)
        elif e.method == "baseline":
            row["shift"] = float(e.shift)
        out.append(row)
    return out


def edits_from_json(raw: Any) -> list[CurveEdit]:
    out: list[CurveEdit] = []
    if not isinstance(raw, list):
        return out
    for item in raw:
        if not isinstance(item, dict):
            continue
        mnemonic = str(item.get("mnemonic") or "").strip()
        method = str(item.get("method") or "")
        if not mnemonic or method not in VALID_METHODS:
            continue
        try:
            window = max(1, int(item.get("window", 5)))
            threshold = max(0.0, float(item.get("threshold", 3.0)))
            shift = float(item.get("shift", 0.0))
        except (TypeError, ValueError):
            continue
        out.append(
            CurveEdit(
                mnemonic=mnemonic,
                method=method,
                window=window,
                threshold=threshold,
                shift=shift,
            )
        )
    return out


# ---------------------------------------------------------------------------
# Per-well persistence (mirrors formulas.json under wells/<id>/)
# ---------------------------------------------------------------------------


def curve_edit_file_path(workspace, well_id: str):
    return well_data_dir(workspace, well_id) / CURVE_EDIT_FILENAME


def load_curve_edits_for_well(
    workspace, well_id: str
) -> tuple[list[CurveEdit], list[str]]:
    """Load curve edits for a well; missing file → ``([], [])``."""
    diagnostics: list[str] = []
    try:
        path = curve_edit_file_path(workspace, well_id)
    except Exception as exc:  # noqa: BLE001 — workspace errors degrade
        return [], [str(exc)]
    if not path.is_file():
        return [], []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"曲线编辑文件损坏: {exc}"]
    raw_list = data.get("edits") if isinstance(data, dict) else data
    return edits_from_json(raw_list), diagnostics


def save_curve_edits_for_well(
    workspace,
    well_id: str,
    edits: Iterable[CurveEdit],
) -> Any:
    """Persist curve edits next to well data (atomic write)."""
    path = curve_edit_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": CURVE_EDIT_SCHEMA_VERSION,
        "well_id": well_id,
        "edits": edits_to_json(edits),
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path
