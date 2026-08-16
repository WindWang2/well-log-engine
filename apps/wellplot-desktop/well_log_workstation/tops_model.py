"""Formation tops associated with wells (#223).

Host-side catalog binding + JSON persistence. Engine interactive pick/edit is
out of scope; missing files and bad JSON degrade to empty tops with diagnostics.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, replace as dataclasses_replace
from pathlib import Path
from typing import Any

from well_log_workstation.workspace import Workspace, WorkspaceError

TOPS_SCHEMA_VERSION = 1
TOPS_FILENAME = "tops.json"
SURVEY_FILENAME = "survey.json"
FORMULA_FILENAME = "formulas.json"


class TopsError(Exception):
    """User-visible tops load/save failure (non-fatal for shell display)."""


@dataclass(frozen=True)
class FormationTop:
    name: str
    depth: float
    unit: str = "m"
    color: str = "#c0392b"
    id: str = ""
    # Optional reference into the workspace stratigraphic dictionary (Epic C):
    # a top may name a unified unit instead of free text. Empty = historic
    # free-text behaviour; old workspaces stay readable.
    unit_id: str = ""
    # Optional marker semantic (SDK marker symbols): one of
    # formation_top | fault | fluid_contact | casing_shoe | custom.
    # Empty = legacy behaviour (submitted as formation_top).
    semantic: str = ""

    def display_label(self) -> str:
        return f"{self.name}  {self.depth:g} {self.unit}".strip()


def well_data_dir(workspace: Workspace, well_id: str) -> Path:
    """Directory for well-local assets (LAS + tops.json)."""
    entry = next((w for w in workspace.wells if w.id == well_id), None)
    if entry is None:
        raise WorkspaceError("井不在工区目录中")
    if entry.path:
        abs_path = workspace.root / entry.path
        # wells/<id>/file.las → wells/<id>/
        if abs_path.parent != workspace.root and abs_path.parent != workspace.wells_dir:
            return abs_path.parent
    # Fallback: wells/<well_id>/
    return workspace.wells_dir / well_id


def tops_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / TOPS_FILENAME


def _from_json_item(item: dict[str, Any]) -> FormationTop | None:
    name = str(item.get("name") or "").strip()
    if not name:
        return None
    try:
        depth = float(item["depth"])
    except (KeyError, TypeError, ValueError):
        return None
    if not (depth == depth):  # NaN
        return None
    color = str(item.get("color") or "#c0392b")
    unit = str(item.get("unit") or "m")
    tid = str(item.get("id") or "")
    unit_id = str(item.get("unit_id") or "")
    semantic = str(item.get("semantic") or "")
    return FormationTop(
        name=name, depth=depth, unit=unit, color=color, id=tid,
        unit_id=unit_id, semantic=semantic,
    )


def load_tops_for_well(
    workspace: Workspace, well_id: str
) -> tuple[list[FormationTop], list[str]]:
    """Load tops for a well. Returns (tops, diagnostics). Never raises for missing file."""
    diagnostics: list[str] = []
    try:
        path = tops_file_path(workspace, well_id)
    except WorkspaceError as exc:
        return [], [str(exc)]

    if not path.is_file():
        return [], []

    try:
        raw = path.read_text(encoding="utf-8")
        data = json.loads(raw)
    except (OSError, json.JSONDecodeError) as exc:
        diagnostics.append(f"层位文件无法读取: {exc}")
        return [], diagnostics

    if not isinstance(data, dict):
        diagnostics.append("层位 JSON 根必须是对象")
        return [], diagnostics

    version = int(data.get("schemaVersion", 0))
    if version not in (0, TOPS_SCHEMA_VERSION):
        diagnostics.append(
            f"不支持的层位 schemaVersion={version}（期望 {TOPS_SCHEMA_VERSION}）"
        )
        # Still try to parse tops list for forward tolerance
    items = data.get("tops")
    if items is None:
        diagnostics.append("层位文件缺少 tops 数组")
        return [], diagnostics
    if not isinstance(items, list):
        diagnostics.append("tops 必须是数组")
        return [], diagnostics

    tops: list[FormationTop] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            diagnostics.append(f"跳过无效层位项 [{i}]")
            continue
        top = _from_json_item(item)
        if top is None:
            diagnostics.append(f"跳过无效层位项 [{i}]（需 name + depth）")
            continue
        if not top.id:
            top = dataclasses_replace(top, id=str(uuid.uuid4()))
        tops.append(top)

    tops.sort(key=lambda t: t.depth)
    return tops, diagnostics


def save_tops_for_well(
    workspace: Workspace,
    well_id: str,
    tops: list[FormationTop],
) -> Path:
    """Persist tops next to well data; create parent dirs as needed."""
    path = tops_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": TOPS_SCHEMA_VERSION,
        "well_id": well_id,
        "tops": [
            {
                "id": t.id or str(uuid.uuid4()),
                "name": t.name,
                "depth": t.depth,
                "unit": t.unit,
                "color": t.color,
                **({"unit_id": t.unit_id} if t.unit_id else {}),
                **({"semantic": t.semantic} if t.semantic else {}),
            }
            for t in tops
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path


# ---------------------------------------------------------------------------
# Formulas (FRS §2.4 / P2-A) — stored next to tops under wells/<id>/.
# ---------------------------------------------------------------------------


def formula_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / FORMULA_FILENAME


def load_formulas_for_well(
    workspace: Workspace, well_id: str
) -> tuple[list[Any], list[str]]:
    """Load derived-curve formulas for a well.

    Returns ``(formulas, diagnostics)``; missing file → ``([], [])``. Lazy
    imports :mod:`well_log_workstation.formula` so this module stays light.
    """
    diagnostics: list[str] = []
    try:
        path = formula_file_path(workspace, well_id)
    except WorkspaceError as exc:
        return [], [str(exc)]
    if not path.is_file():
        return [], []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"公式文件损坏: {exc}"]
    from well_log_workstation.formula import Formula

    raw_list = data.get("formulas") if isinstance(data, dict) else data
    out: list[Formula] = []
    if isinstance(raw_list, list):
        for item in raw_list:
            if not isinstance(item, dict):
                continue
            name = str(item.get("name") or "").strip()
            expr = str(item.get("expression") or "").strip()
            if name and expr:
                out.append(Formula(name=name, expression=expr))
    return out, diagnostics


def save_formulas_for_well(
    workspace: Workspace,
    well_id: str,
    formulas: list[Any],
) -> Path:
    """Persist formulas next to well data (atomic write)."""
    path = formula_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": 1,
        "well_id": well_id,
        "formulas": [
            {"name": f.name, "expression": f.expression}
            for f in formulas
            if getattr(f, "name", "") and getattr(f, "expression", "")
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path


# ---------------------------------------------------------------------------
# Deviation survey (FRS §1.1 / P1-C) — stored next to tops under wells/<id>/.
# ---------------------------------------------------------------------------


def survey_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / SURVEY_FILENAME


def load_survey_for_well(
    workspace: Workspace, well_id: str
) -> tuple[list[Any], list[str]]:
    """Load deviation survey stations for a well.

    Returns ``(stations, diagnostics)``. Missing file → ``([], [])``. Lazy
    imports :mod:`well_log_workstation.survey` so this module stays importable
    without numpy-only consumers paying for it.
    """
    diagnostics: list[str] = []
    try:
        path = survey_file_path(workspace, well_id)
    except WorkspaceError as exc:
        return [], [str(exc)]
    if not path.is_file():
        return [], []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"测斜文件损坏: {exc}"]
    raw_stations = (
        data.get("stations") if isinstance(data, dict) else data
    )
    from well_log_workstation.survey import survey_from_json

    return survey_from_json(raw_stations), diagnostics


def save_survey_for_well(
    workspace: Workspace,
    well_id: str,
    stations: list[Any],
) -> Path:
    """Persist deviation survey next to well data (atomic write)."""
    from well_log_workstation.survey import survey_to_json

    path = survey_file_path(workspace, well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": 1,
        "well_id": well_id,
        "stations": survey_to_json(stations),
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path


def make_stub_tops(
    depth_min: float,
    depth_max: float,
    *,
    unit: str = "m",
    names: tuple[str, ...] = ("T1", "T2", "T3"),
) -> list[FormationTop]:
    """Demo tops at even fractions of the depth range (not real geology)."""
    if not (depth_max > depth_min):
        depth_min, depth_max = 0.0, 100.0
    span = depth_max - depth_min
    colors = ("#c0392b", "#2980b9", "#27ae60", "#8e44ad", "#d35400")
    n = len(names)
    out: list[FormationTop] = []
    for i, name in enumerate(names):
        frac = (i + 1) / (n + 1)
        out.append(
            FormationTop(
                id=str(uuid.uuid4()),
                name=name,
                depth=depth_min + span * frac,
                unit=unit,
                color=colors[i % len(colors)],
            )
        )
    return out


def import_tops_from_json_file(
    workspace: Workspace,
    well_id: str,
    path: Path | str,
) -> tuple[list[FormationTop], list[str]]:
    """Load tops from an external JSON file and associate with well (persist)."""
    src = Path(path).expanduser().resolve()
    if not src.is_file():
        raise TopsError(f"层位文件不存在: {src}")
    try:
        data = json.loads(src.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TopsError(f"无法解析层位 JSON: {exc}") from exc

    diagnostics: list[str] = []
    items: list[Any]
    if isinstance(data, list):
        items = data
    elif isinstance(data, dict):
        items = data.get("tops") or data.get("horizons") or []
        if not isinstance(items, list):
            raise TopsError("JSON 中 tops/horizons 必须是数组")
    else:
        raise TopsError("层位 JSON 根必须是对象或数组")

    tops: list[FormationTop] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            diagnostics.append(f"跳过 [{i}]")
            continue
        # Accept depth_md / md aliases
        if "depth" not in item:
            for key in ("depth_md", "md", "MD", "depth_m"):
                if key in item:
                    item = {**item, "depth": item[key]}
                    break
        top = _from_json_item(item)
        if top is None:
            diagnostics.append(f"跳过无效项 [{i}]")
            continue
        if not top.id:
            top = dataclasses_replace(top, id=str(uuid.uuid4()))
        tops.append(top)

    if not tops:
        raise TopsError("未解析到任何有效层位（需要 name + depth）")

    tops.sort(key=lambda t: t.depth)
    save_tops_for_well(workspace, well_id, tops)
    return tops, diagnostics
