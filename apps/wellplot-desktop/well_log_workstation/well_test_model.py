"""Well-test (试油/测试) domain model (Epic C / FRS §3.x 试油数据库基础).

Common fields are structured (interval, formation, type, date, pressure,
flow, fluid, result, interpretation, source, attachments); type-specific
data lives in an opaque ``payload`` dict (typed/extensible schema) — NOT one
giant struct with dozens of always-nullable fields (C3).

Every depth-bearing object carries depth value + unit + EXPLICIT depth domain
(C5). Stratigraphy linkage goes through the unified dictionary (C1). Storage
is the per-well sidecar pattern (``wells/<id>/well_test.json``), atomic
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

WELL_TEST_FILENAME = "well_test.json"
WELL_TEST_SCHEMA_VERSION = 1

DEPTH_DOMAINS = ("MD", "TVD", "TVDSS")

# Well-known test types (extensible — free string allowed).
TEST_TYPES = ("DST", "MFE", "试采", "压裂测试", "注入测试")


@dataclass(frozen=True)
class WellTestInterval:
    """One tested interval with structured common fields + typed payload."""

    id: str
    top: float
    bottom: float
    unit: str = "m"
    depth_domain: str = "MD"
    formation_unit_id: str = ""  # → stratigraphic dictionary (C1); empty = free
    test_type: str = "DST"
    date: str = ""  # ISO date or free text
    fluid: str = ""  # oil / gas / water / mixed / …
    result: str = ""  # 结论 (free text; typed payloads may refine)
    pressure_mpa: float | None = None
    flow_rate_m3d: float | None = None
    interpretation: str = ""
    # Type-specific data (extensible schema): opaque JSON-able dict.
    payload: dict[str, Any] = field(default_factory=dict)
    source: str = ""
    version: str = ""
    attachment_refs: list[str] = field(default_factory=list)

    @property
    def display_label(self) -> str:
        return (
            f"{self.test_type} · {self.top:g}–{self.bottom:g} {self.unit}"
            f" · {self.fluid or '—'}"
        )


@dataclass
class WellTestModel:
    """Per-well well-test domain (intervals sorted by top)."""

    well_id: str
    unit: str = "m"
    intervals: list[WellTestInterval] = field(default_factory=list)

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


def well_test_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / WELL_TEST_FILENAME


def _opt_float(item: dict[str, Any], key: str) -> float | None:
    try:
        value = float(item[key])
    except (KeyError, TypeError, ValueError):
        return None
    return value if value == value else None  # NaN → None


def _interval_from_json(item: dict[str, Any]) -> WellTestInterval | None:
    try:
        top = float(item["top"])
        bottom = float(item["bottom"])
    except (KeyError, TypeError, ValueError):
        return None
    if top != top or bottom != bottom:
        return None
    payload = item.get("payload")
    return WellTestInterval(
        id=str(item.get("id") or str(uuid.uuid4())),
        top=top,
        bottom=bottom,
        unit=str(item.get("unit") or "m"),
        depth_domain=str(item.get("depth_domain") or "MD"),
        formation_unit_id=str(item.get("formation_unit_id") or ""),
        test_type=str(item.get("test_type") or "DST"),
        date=str(item.get("date") or ""),
        fluid=str(item.get("fluid") or ""),
        result=str(item.get("result") or ""),
        pressure_mpa=_opt_float(item, "pressure_mpa"),
        flow_rate_m3d=_opt_float(item, "flow_rate_m3d"),
        interpretation=str(item.get("interpretation") or ""),
        payload=dict(payload) if isinstance(payload, dict) else {},
        source=str(item.get("source") or ""),
        version=str(item.get("version") or ""),
        attachment_refs=[str(x) for x in (item.get("attachment_refs") or []) if x],
    )


def load_well_test_for_well(
    workspace: Workspace, well_id: str
) -> tuple[WellTestModel, list[str]]:
    """Load the well-test domain; missing/corrupt → (empty model, diag)."""
    diagnostics: list[str] = []
    try:
        path = well_test_file_path(workspace, well_id)
    except Exception as exc:
        return WellTestModel(well_id=well_id), [str(exc)]
    if not path.is_file():
        return WellTestModel(well_id=well_id), []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return WellTestModel(well_id=well_id), [f"试油数据损坏: {exc}"]
    if not isinstance(raw, dict):
        return WellTestModel(well_id=well_id), ["试油数据格式无效"]
    intervals = [
        i
        for i in (_interval_from_json(x) for x in raw.get("intervals") or [])
        if i
    ]
    intervals.sort(key=lambda i: (i.top, i.id))
    return WellTestModel(
        well_id=well_id,
        unit=str(raw.get("unit") or "m"),
        intervals=intervals,
    ), diagnostics


def save_well_test_for_well(
    workspace: Workspace, well_id: str, model: WellTestModel
) -> Path:
    """Persist the well-test domain next to the well data (atomic write)."""
    path = well_test_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": WELL_TEST_SCHEMA_VERSION,
        "well_id": well_id,
        "unit": model.unit,
        "intervals": [
            {
                "id": itv.id,
                "top": itv.top,
                "bottom": itv.bottom,
                "unit": itv.unit,
                "depth_domain": itv.depth_domain,
                "formation_unit_id": itv.formation_unit_id,
                "test_type": itv.test_type,
                "date": itv.date,
                "fluid": itv.fluid,
                "result": itv.result,
                "pressure_mpa": itv.pressure_mpa,
                "flow_rate_m3d": itv.flow_rate_m3d,
                "interpretation": itv.interpretation,
                "payload": itv.payload,
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
