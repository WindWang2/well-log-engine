"""Perforation (射孔) domain model (Epic C / FRS §3.x 射孔数据库基础).

Each interval carries: depth domain + unit (explicit, C5), top/bottom,
operation date, shot density, phasing, status, completion/run reference,
formation linkage (C1) and source/version. The model is the stable data
interface for the future 射孔道 / 试油解释道 / 井下工程道 — track rendering
binds to this, never to ad-hoc payloads.

Per-well sidecar persistence (``wells/<id>/perforation.json``), atomic
write, tolerant read.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from well_log_workstation.tops_model import well_data_dir
from well_log_workstation.workspace import Workspace

PERFORATION_FILENAME = "perforation.json"
PERFORATION_SCHEMA_VERSION = 1

DEPTH_DOMAINS = ("MD", "TVD", "TVDSS")

# Status values (extensible — free string allowed).
PERFORATION_STATUSES = ("已射", "未射", "已封堵", "补射")


@dataclass(frozen=True)
class PerforationInterval:
    """One perforated interval."""

    id: str
    top: float
    bottom: float
    unit: str = "m"
    depth_domain: str = "MD"
    operation_date: str = ""  # ISO date or free text
    shot_density: float | None = None  # shots per metre
    phasing: str = ""  # 相位 (e.g. "90°", free)
    status: str = "已射"
    completion_ref: str = ""  # 完井 / 下入管柱引用
    formation_unit_id: str = ""  # → stratigraphic dictionary (C1)
    source: str = ""
    version: str = ""
    attachment_refs: list[str] = field(default_factory=list)

    @property
    def display_label(self) -> str:
        return (
            f"射孔 {self.top:g}–{self.bottom:g} {self.unit}"
            f" · {self.status}"
        )


@dataclass
class PerforationModel:
    """Per-well perforation domain (intervals sorted by top)."""

    well_id: str
    unit: str = "m"
    intervals: list[PerforationInterval] = field(default_factory=list)

    def validate(self) -> list[str]:
        problems: list[str] = []
        ids: set[str] = set()
        for itv in self.intervals:
            if itv.bottom < itv.top:
                problems.append(f"{itv.id}: 顶深大于底深")
            if itv.depth_domain not in DEPTH_DOMAINS:
                problems.append(f"{itv.id}: 深度域 {itv.depth_domain} 未声明")
            if itv.id in ids:
                problems.append(f"{itv.id}: 重复 interval id")
            ids.add(itv.id)
        return problems


# ---------------------------------------------------------------------------
# Persistence
# ---------------------------------------------------------------------------


def perforation_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / PERFORATION_FILENAME


def _opt_float(item: dict[str, Any], key: str) -> float | None:
    try:
        value = float(item[key])
    except (KeyError, TypeError, ValueError):
        return None
    return value if value == value else None  # NaN → None


def _interval_from_json(item: dict[str, Any]) -> PerforationInterval | None:
    try:
        top = float(item["top"])
        bottom = float(item["bottom"])
    except (KeyError, TypeError, ValueError):
        return None
    if top != top or bottom != bottom:
        return None
    return PerforationInterval(
        id=str(item.get("id") or str(uuid.uuid4())),
        top=top,
        bottom=bottom,
        unit=str(item.get("unit") or "m"),
        depth_domain=str(item.get("depth_domain") or "MD"),
        operation_date=str(item.get("operation_date") or ""),
        shot_density=_opt_float(item, "shot_density"),
        phasing=str(item.get("phasing") or ""),
        status=str(item.get("status") or "已射"),
        completion_ref=str(item.get("completion_ref") or ""),
        formation_unit_id=str(item.get("formation_unit_id") or ""),
        source=str(item.get("source") or ""),
        version=str(item.get("version") or ""),
        attachment_refs=[str(x) for x in (item.get("attachment_refs") or []) if x],
    )


def load_perforation_for_well(
    workspace: Workspace, well_id: str
) -> tuple[PerforationModel, list[str]]:
    """Load the perforation domain; missing/corrupt → (empty model, diag)."""
    diagnostics: list[str] = []
    try:
        path = perforation_file_path(workspace, well_id)
    except Exception as exc:
        return PerforationModel(well_id=well_id), [str(exc)]
    if not path.is_file():
        return PerforationModel(well_id=well_id), []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return PerforationModel(well_id=well_id), [f"射孔数据损坏: {exc}"]
    if not isinstance(raw, dict):
        return PerforationModel(well_id=well_id), ["射孔数据格式无效"]
    intervals = [
        i
        for i in (_interval_from_json(x) for x in raw.get("intervals") or [])
        if i
    ]
    intervals.sort(key=lambda i: (i.top, i.id))
    return PerforationModel(
        well_id=well_id,
        unit=str(raw.get("unit") or "m"),
        intervals=intervals,
    ), diagnostics


def save_perforation_for_well(
    workspace: Workspace, well_id: str, model: PerforationModel
) -> Path:
    """Persist the perforation domain next to the well data (atomic write)."""
    path = perforation_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": PERFORATION_SCHEMA_VERSION,
        "well_id": well_id,
        "unit": model.unit,
        "intervals": [
            {
                "id": itv.id,
                "top": itv.top,
                "bottom": itv.bottom,
                "unit": itv.unit,
                "depth_domain": itv.depth_domain,
                "operation_date": itv.operation_date,
                "shot_density": itv.shot_density,
                "phasing": itv.phasing,
                "status": itv.status,
                "completion_ref": itv.completion_ref,
                "formation_unit_id": itv.formation_unit_id,
                "source": itv.source,
                "version": itv.version,
                "attachment_refs": list(itv.attachment_refs),
            }
            for itv in model.intervals
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path
