"""Core domain model (Epic C / FRS §3.x 岩心数据库基础).

Builds ON the existing photo model (``core_photo_model.py``) instead of
replacing it: ``CorePhotoSegment`` stays the image-track binding, and the
physical core (runs + samples) references photo segments by id — a photo is
one associated object of the core domain, never the whole model.

Every depth-bearing object carries its depth value, unit, and EXPLICIT depth
domain (C5 — no implicit "default MD"); well identity comes from the
per-well storage location (``wells/<id>/core.json``), plus source/version for
auditability.

Persistence mirrors the other well sidecars (tops.json / core_photos.json):
atomic write, tolerant read, schemaVersion.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from well_log_workstation.tops_model import well_data_dir
from well_log_workstation.workspace import Workspace

CORE_FILENAME = "core.json"
CORE_SCHEMA_VERSION = 1

# Explicit depth domains (C5): the model never assumes MD silently.
DEPTH_DOMAINS = ("MD", "TVD", "TVDSS")


@dataclass(frozen=True)
class CoreSample:
    """One physical core sample / plug with properties + linkage."""

    id: str
    depth: float
    unit: str = "m"
    depth_domain: str = "MD"
    description: str = ""
    # Reference into the workspace stratigraphic dictionary (C1); empty = free.
    lithology_unit_id: str = ""
    porosity: float | None = None
    permeability_md: float | None = None
    density_gcc: float | None = None
    # Laboratory-result reference (free id/URI; typed payloads later).
    lab_report_ref: str = ""
    # Photo linkage: id of a CorePhotoSegment in the well's photo model.
    photo_segment_id: str = ""
    attachment_ref: str = ""
    source: str = ""
    version: str = ""

    @property
    def display_label(self) -> str:
        return f"{self.depth:g} {self.unit} · {self.description or '样品'}"


@dataclass
class CoreRun:
    """One cored interval (drilling run): top/bottom + recovery + samples."""

    id: str
    top: float
    bottom: float
    unit: str = "m"
    depth_domain: str = "MD"
    recovery_m: float | None = None  # recovered core length
    label: str = ""
    samples: list[CoreSample] = field(default_factory=list)
    photo_segment_ids: list[str] = field(default_factory=list)
    source: str = ""
    version: str = ""

    def thickness(self) -> float:
        return self.bottom - self.top


@dataclass
class CoreModel:
    """Per-well core domain: ordered runs (top ascending)."""

    well_id: str
    unit: str = "m"
    runs: list[CoreRun] = field(default_factory=list)

    def validate(self) -> list[str]:
        """Structural diagnostics: depth order, domain/unit explicitness."""
        problems: list[str] = []
        ids: set[str] = set()
        for run in self.runs:
            if run.bottom < run.top:
                problems.append(f"{run.label or run.id}: 顶深大于底深")
            if run.depth_domain not in DEPTH_DOMAINS:
                problems.append(f"{run.label or run.id}: 深度域 {run.depth_domain} 未声明")
            if run.id in ids:
                problems.append(f"{run.id}: 重复 run id")
            ids.add(run.id)
            sample_ids: set[str] = set()
            for sample in run.samples:
                if sample.id in sample_ids:
                    problems.append(f"{sample.id}: 重复样品 id")
                sample_ids.add(sample.id)
                if sample.depth_domain not in DEPTH_DOMAINS:
                    problems.append(f"{sample.id}: 深度域 {sample.depth_domain} 未声明")
        return problems


# ---------------------------------------------------------------------------
# Persistence
# ---------------------------------------------------------------------------


def core_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / CORE_FILENAME


def _opt_float(item: dict[str, Any], key: str) -> float | None:
    try:
        value = float(item[key])
    except (KeyError, TypeError, ValueError):
        return None
    return value if value == value else None  # NaN → None


def _sample_from_json(item: dict[str, Any]) -> CoreSample | None:
    try:
        depth = float(item["depth"])
    except (KeyError, TypeError, ValueError):
        return None
    if depth != depth:
        return None
    return CoreSample(
        id=str(item.get("id") or str(uuid.uuid4())),
        depth=depth,
        unit=str(item.get("unit") or "m"),
        depth_domain=str(item.get("depth_domain") or "MD"),
        description=str(item.get("description") or ""),
        lithology_unit_id=str(item.get("lithology_unit_id") or ""),
        porosity=_opt_float(item, "porosity"),
        permeability_md=_opt_float(item, "permeability_md"),
        density_gcc=_opt_float(item, "density_gcc"),
        lab_report_ref=str(item.get("lab_report_ref") or ""),
        photo_segment_id=str(item.get("photo_segment_id") or ""),
        attachment_ref=str(item.get("attachment_ref") or ""),
        source=str(item.get("source") or ""),
        version=str(item.get("version") or ""),
    )


def _run_from_json(item: dict[str, Any]) -> CoreRun | None:
    try:
        top = float(item["top"])
        bottom = float(item["bottom"])
    except (KeyError, TypeError, ValueError):
        return None
    if top != top or bottom != bottom:
        return None
    samples = [
        s for s in (_sample_from_json(x) for x in item.get("samples") or []) if s
    ]
    return CoreRun(
        id=str(item.get("id") or str(uuid.uuid4())),
        top=top,
        bottom=bottom,
        unit=str(item.get("unit") or "m"),
        depth_domain=str(item.get("depth_domain") or "MD"),
        recovery_m=_opt_float(item, "recovery_m"),
        label=str(item.get("label") or ""),
        samples=samples,
        photo_segment_ids=[
            str(x) for x in (item.get("photo_segment_ids") or []) if x
        ],
        source=str(item.get("source") or ""),
        version=str(item.get("version") or ""),
    )


def load_core_for_well(
    workspace: Workspace, well_id: str
) -> tuple[CoreModel, list[str]]:
    """Load the well core domain; missing/corrupt → (empty model, diag)."""
    diagnostics: list[str] = []
    try:
        path = core_file_path(workspace, well_id)
    except Exception as exc:
        return CoreModel(well_id=well_id), [str(exc)]
    if not path.is_file():
        return CoreModel(well_id=well_id), []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return CoreModel(well_id=well_id), [f"岩心数据损坏: {exc}"]
    if not isinstance(raw, dict):
        return CoreModel(well_id=well_id), ["岩心数据格式无效"]
    runs = [
        r for r in (_run_from_json(x) for x in raw.get("runs") or []) if r
    ]
    runs.sort(key=lambda r: (r.top, r.id))
    return CoreModel(
        well_id=well_id,
        unit=str(raw.get("unit") or "m"),
        runs=runs,
    ), diagnostics


def save_core_for_well(
    workspace: Workspace, well_id: str, model: CoreModel
) -> Path:
    """Persist the core domain next to the well data (atomic write)."""
    path = core_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": CORE_SCHEMA_VERSION,
        "well_id": well_id,
        "unit": model.unit,
        "runs": [
            {
                "id": run.id,
                "top": run.top,
                "bottom": run.bottom,
                "unit": run.unit,
                "depth_domain": run.depth_domain,
                "recovery_m": run.recovery_m,
                "label": run.label,
                "source": run.source,
                "version": run.version,
                "photo_segment_ids": list(run.photo_segment_ids),
                "samples": [
                    {
                        "id": s.id,
                        "depth": s.depth,
                        "unit": s.unit,
                        "depth_domain": s.depth_domain,
                        "description": s.description,
                        "lithology_unit_id": s.lithology_unit_id,
                        "porosity": s.porosity,
                        "permeability_md": s.permeability_md,
                        "density_gcc": s.density_gcc,
                        "lab_report_ref": s.lab_report_ref,
                        "photo_segment_id": s.photo_segment_id,
                        "attachment_ref": s.attachment_ref,
                        "source": s.source,
                        "version": s.version,
                    }
                    for s in run.samples
                ],
            }
            for run in model.runs
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path
