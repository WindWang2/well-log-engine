"""Semantic Selection Set for graph↔table linkage (T5 / #345, ADR 0024).

Identity is well + optional curve leaf + sample index + Reference Depth.
Never stores screen Y, Display Depth, or LOD envelope points.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True, slots=True)
class SemanticSelection:
    """One point selection in document/reference depth space."""

    well_id: str
    sample_index: int
    reference_depth: float
    curve_mnemonic: str | None = None
    leaf_id: str | None = None

    def __post_init__(self) -> None:
        if self.sample_index < 0:
            raise ValueError("sample_index must be >= 0")
        if not np.isfinite(self.reference_depth):
            raise ValueError("reference_depth must be finite")


def nearest_sample_index(depth: np.ndarray, reference_depth: float) -> int | None:
    """Map a Reference Depth to the nearest sample index on an axis."""
    arr = np.asarray(depth, dtype=np.float64)
    if arr.size == 0 or not np.isfinite(reference_depth):
        return None
    # Prefer finite samples only
    finite = np.isfinite(arr)
    if not np.any(finite):
        return None
    idx = int(np.nanargmin(np.abs(arr - reference_depth)))
    return idx


def selection_from_depth(
    *,
    well_id: str,
    depth: np.ndarray,
    reference_depth: float,
    curve_mnemonic: str | None = None,
    leaf_id: str | None = None,
) -> SemanticSelection | None:
    """Build SemanticSelection from a Reference Depth on a depth axis."""
    idx = nearest_sample_index(depth, reference_depth)
    if idx is None:
        return None
    arr = np.asarray(depth, dtype=np.float64)
    ref = float(arr[idx]) if np.isfinite(arr[idx]) else float(reference_depth)
    return SemanticSelection(
        well_id=well_id,
        sample_index=idx,
        reference_depth=ref,
        curve_mnemonic=curve_mnemonic,
        leaf_id=leaf_id,
    )


def selection_from_row(
    *,
    well_id: str,
    depth: np.ndarray,
    sample_index: int,
    curve_mnemonic: str | None = None,
    leaf_id: str | None = None,
) -> SemanticSelection | None:
    """Build SemanticSelection from a table row (sample index)."""
    arr = np.asarray(depth, dtype=np.float64)
    if sample_index < 0 or sample_index >= arr.size:
        return None
    ref = float(arr[sample_index])
    if not np.isfinite(ref):
        return None
    return SemanticSelection(
        well_id=well_id,
        sample_index=int(sample_index),
        reference_depth=ref,
        curve_mnemonic=curve_mnemonic,
        leaf_id=leaf_id,
    )
