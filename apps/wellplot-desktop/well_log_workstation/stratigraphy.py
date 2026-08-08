"""Unified stratigraphic dictionary (Epic C / FRS §3.x 统一地层层序).

Workspace-level hierarchy of stratigraphic units (界-系-统-组-段-小层-砂层
组-砂层). Units carry stable ids, parent links, name/code, display style
(color / line style / pattern), geological age, explicit sibling ordering,
source and version. The levels are NOT a closed enum — the standard eight are
provided as constants, but any level name is allowed so regional / future
schemes stay representable (extensible, not hardcoded Chinese enums).

``FormationTop`` references a unit by ``unit_id`` (tops_model.py); tops
without one keep the historic free-text behaviour, so old workspaces stay
readable without migration.

Persistence is a workspace-level sidecar ``stratigraphy.json`` (mirror of the
per-well sidecar pattern: definitions live beside the data, plots only bind).
Pure Python + dataclasses — headless-testable.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

from well_log_workstation.workspace import Workspace

# The standard eight levels (descending). Extensible: any string is allowed.
STANDARD_LEVELS = ("界", "系", "统", "组", "段", "小层", "砂层组", "砂层")

STRATIGRAPHY_FILENAME = "stratigraphy.json"
STRATIGRAPHY_SCHEMA_VERSION = 1


@dataclass(frozen=True)
class StratigraphicUnit:
    """One node of the stratigraphic hierarchy."""

    id: str
    name: str
    level: str = "组"
    parent_id: str | None = None  # None = top-level (界)
    code: str = ""
    color: str = "#8c8c8c"
    line_style: str = "solid"
    pattern: str = ""
    age: str = ""  # geological age / time description (free text)
    order: int = 0  # explicit ordering within the parent's children
    source: str = ""
    version: str = ""

    def display_label(self) -> str:
        prefix = f"{self.code} " if self.code else ""
        suffix = f" · {self.age}" if self.age else ""
        return f"{prefix}{self.name}（{self.level}）{suffix}"


@dataclass
class StratigraphicDictionary:
    """Ordered unit collection with hierarchy navigation + validation."""

    units: list[StratigraphicUnit] = field(default_factory=list)

    def unit_by_id(self, unit_id: str) -> StratigraphicUnit | None:
        for unit in self.units:
            if unit.id == unit_id:
                return unit
        return None

    def children_of(self, parent_id: str | None) -> list[StratigraphicUnit]:
        return sorted(
            (u for u in self.units if u.parent_id == parent_id),
            key=lambda u: (u.order, u.name),
        )

    def root_units(self) -> list[StratigraphicUnit]:
        return self.children_of(None)

    def path_to(self, unit_id: str) -> list[StratigraphicUnit]:
        """Ancestor chain from the top level down to (and including) the unit.

        Returns [] for an unknown id; detects cycles by walking a bounded
        number of parent hops.
        """
        unit = self.unit_by_id(unit_id)
        if unit is None:
            return []
        chain: list[StratigraphicUnit] = []
        seen: set[str] = set()
        current: StratigraphicUnit | None = unit
        while current is not None:
            if current.id in seen or len(chain) > len(self.units):
                return []  # cycle or runaway parent chain
            seen.add(current.id)
            chain.append(current)
            current = (
                self.unit_by_id(current.parent_id)
                if current.parent_id is not None
                else None
            )
        chain.reverse()
        return chain

    def validate(self) -> list[str]:
        """Structural diagnostics: duplicate ids, unknown parents, cycles."""
        problems: list[str] = []
        ids: set[str] = set()
        for unit in self.units:
            if not unit.id:
                problems.append(f"{unit.name}: 缺少稳定 id")
            if unit.id in ids:
                problems.append(f"{unit.name}: 重复 id {unit.id}")
            ids.add(unit.id)
        for unit in self.units:
            if unit.parent_id is not None and unit.parent_id not in ids:
                problems.append(f"{unit.name}: 父节点 {unit.parent_id} 不存在")
            elif self.path_to(unit.id) == [] and unit.parent_id is not None:
                problems.append(f"{unit.name}: 层级链异常（疑似环）")
        return problems


def make_demo_stratigraphy() -> StratigraphicDictionary:
    """Demo eight-level hierarchy: one 界→…→砂层 chain plus a sibling 组.

    Used by the dictionary editor's 「生成演示字典」 and by tests; ids are
    fresh uuids so multiple invocations never collide.
    """
    e = StratigraphicUnit(id=str(uuid.uuid4()), name="新生界", level="界")
    s = StratigraphicUnit(
        id=str(uuid.uuid4()), name="古近系", level="系", parent_id=e.id
    )
    u = StratigraphicUnit(
        id=str(uuid.uuid4()), name="始新统", level="统", parent_id=s.id
    )
    f1 = StratigraphicUnit(
        id=str(uuid.uuid4()), name="沙四段", level="组", parent_id=u.id
    )
    m = StratigraphicUnit(
        id=str(uuid.uuid4()), name="沙四上亚段", level="段", parent_id=f1.id
    )
    b = StratigraphicUnit(
        id=str(uuid.uuid4()), name="1 小层", level="小层", parent_id=m.id
    )
    g = StratigraphicUnit(
        id=str(uuid.uuid4()), name="砂层组 A", level="砂层组", parent_id=b.id
    )
    z = StratigraphicUnit(
        id=str(uuid.uuid4()), name="砂层 1", level="砂层", parent_id=g.id
    )
    f2 = StratigraphicUnit(
        id=str(uuid.uuid4()),
        name="沙三段",
        level="组",
        parent_id=u.id,
        order=1,
        code="E2s3",
        color="#3b6fb5",
        age="始新世",
    )
    return StratigraphicDictionary(units=[e, s, u, f1, m, b, g, z, f2])


# ---------------------------------------------------------------------------
# Persistence (workspace-level sidecar, tolerant read)
# ---------------------------------------------------------------------------


def stratigraphy_path(workspace: Workspace) -> Path:
    return workspace.root / STRATIGRAPHY_FILENAME


def _unit_from_json(item: dict[str, Any]) -> StratigraphicUnit | None:
    name = str(item.get("name") or "").strip()
    if not name:
        return None
    try:
        order = int(item.get("order", 0))
    except (TypeError, ValueError):
        order = 0
    return StratigraphicUnit(
        id=str(item.get("id") or str(uuid.uuid4())),
        name=name,
        level=str(item.get("level") or "组"),
        parent_id=(
            str(item["parent_id"]) if item.get("parent_id") else None
        ),
        code=str(item.get("code") or ""),
        color=str(item.get("color") or "#8c8c8c"),
        line_style=str(item.get("line_style") or "solid"),
        pattern=str(item.get("pattern") or ""),
        age=str(item.get("age") or ""),
        order=order,
        source=str(item.get("source") or ""),
        version=str(item.get("version") or ""),
    )


def load_stratigraphy(workspace: Workspace) -> tuple[StratigraphicDictionary, list[str]]:
    """Load the workspace stratigraphic dictionary; missing/corrupt → ([], diag)."""
    diagnostics: list[str] = []
    path = stratigraphy_path(workspace)
    if not path.is_file():
        return StratigraphicDictionary(), []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return StratigraphicDictionary(), [f"层序字典损坏: {exc}"]
    items = raw.get("units") if isinstance(raw, dict) else raw
    if not isinstance(items, list):
        return StratigraphicDictionary(), ["层序字典格式无效"]
    units: list[StratigraphicUnit] = []
    for item in items:
        if not isinstance(item, dict):
            continue
        unit = _unit_from_json(item)
        if unit is not None:
            units.append(unit)
    return StratigraphicDictionary(units=units), diagnostics


def save_stratigraphy(
    workspace: Workspace, dictionary: StratigraphicDictionary
) -> Path:
    """Persist the dictionary beside the workspace (atomic write)."""
    path = stratigraphy_path(workspace)
    payload = {
        "schemaVersion": STRATIGRAPHY_SCHEMA_VERSION,
        "units": [
            {
                "id": u.id,
                "name": u.name,
                "level": u.level,
                "parent_id": u.parent_id,
                "code": u.code,
                "color": u.color,
                "line_style": u.line_style,
                "pattern": u.pattern,
                "age": u.age,
                "order": u.order,
                "source": u.source,
                "version": u.version,
            }
            for u in dictionary.units
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path
