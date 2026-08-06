"""Per-well lithology segments for the single-well lithology track (FRS §2.x).

Qt-free model: depth bands with a SY/T 5615 pattern id, persisted next to
tops under ``wells/<id>/lithology.json``. Missing files and bad JSON degrade
to an empty model with diagnostics (mirrors tops_model).
"""

from __future__ import annotations

import json
import math
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from well_log_workstation.tops_model import well_data_dir
from well_log_workstation.workspace import Workspace, WorkspaceError

LITHOLOGY_SCHEMA_VERSION = 1
LITHOLOGY_FILENAME = "lithology.json"

# Stable builtin pattern ids (SY/T 5615 core catalog, syt5615.json) used by
# the demo-data generator. The dialog validates against the live catalog.
STUB_PATTERN_SEQUENCE: tuple[tuple[str, str], ...] = (
    ("syt-mudstone", "泥岩"),
    ("syt-sandstone", "砂岩"),
    ("syt-limestone", "灰岩"),
    ("syt-shale", "页岩"),
    ("syt-mudstone", "泥岩"),
)


@dataclass(frozen=True)
class LithologySegment:
    id: str
    top: float
    bottom: float
    pattern_id: str
    label: str = ""

    def thickness(self) -> float:
        return self.bottom - self.top


@dataclass
class LithologyModel:
    well_id: str
    unit: str = "m"
    segments: list[LithologySegment] = field(default_factory=list)


def lithology_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / LITHOLOGY_FILENAME


def normalize_segments(
    segments: list[LithologySegment] | None,
) -> tuple[list[LithologySegment], list[str]]:
    """Drop invalid segments (assign missing ids), sort by top depth.

    Pure function — no IO. Returns ``(segments, diagnostics)``.
    """
    out: list[LithologySegment] = []
    diagnostics: list[str] = []
    for i, seg in enumerate(segments or []):
        if (
            not math.isfinite(seg.top)
            or not math.isfinite(seg.bottom)
            or seg.bottom <= seg.top
        ):
            diagnostics.append(f"跳过无效岩性段 [{i}]（需 top < bottom）")
            continue
        if not str(seg.pattern_id).strip():
            diagnostics.append(f"跳过无效岩性段 [{i}]（缺 pattern_id）")
            continue
        out.append(
            LithologySegment(
                id=seg.id or str(uuid.uuid4()),
                top=seg.top,
                bottom=seg.bottom,
                pattern_id=seg.pattern_id,
                label=seg.label,
            )
        )
    out.sort(key=lambda s: (s.top, s.bottom))
    return out, diagnostics


def _from_json_item(item: dict[str, Any]) -> LithologySegment | None:
    try:
        top = float(item["top"])
        bottom = float(item["bottom"])
    except (KeyError, TypeError, ValueError):
        return None
    if not math.isfinite(top) or not math.isfinite(bottom) or bottom <= top:
        return None
    pattern_id = str(item.get("pattern_id") or "").strip()
    if not pattern_id:
        return None
    label = str(item.get("label") or "")
    seg_id = str(item.get("id") or "")
    return LithologySegment(
        id=seg_id, top=top, bottom=bottom, pattern_id=pattern_id, label=label
    )


def load_lithology_for_well(
    workspace: Workspace, well_id: str
) -> tuple[LithologyModel, list[str]]:
    """Load lithology segments for a well; missing file → empty model.

    Returns ``(model, diagnostics)``; never raises for missing/bad file.
    """
    diagnostics: list[str] = []
    try:
        path = lithology_file_path(workspace, well_id)
    except WorkspaceError as exc:
        return LithologyModel(well_id=well_id), [str(exc)]

    if not path.is_file():
        return LithologyModel(well_id=well_id), []

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return LithologyModel(well_id=well_id), [f"岩性文件无法读取: {exc}"]

    if not isinstance(data, dict):
        return LithologyModel(well_id=well_id), ["岩性 JSON 根必须是对象"]

    version = int(data.get("schemaVersion", 0))
    if version not in (0, LITHOLOGY_SCHEMA_VERSION):
        diagnostics.append(
            f"不支持的岩性 schemaVersion={version}（期望 {LITHOLOGY_SCHEMA_VERSION}）"
        )
    items = data.get("segments")
    if items is None:
        diagnostics.append("岩性文件缺少 segments 数组")
        return LithologyModel(well_id=well_id), diagnostics
    if not isinstance(items, list):
        diagnostics.append("segments 必须是数组")
        return LithologyModel(well_id=well_id), diagnostics

    segs: list[LithologySegment] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            diagnostics.append(f"跳过无效岩性项 [{i}]")
            continue
        seg = _from_json_item(item)
        if seg is None:
            diagnostics.append(f"跳过无效岩性项 [{i}]（需 top < bottom + pattern_id）")
            continue
        segs.append(seg)
    segs, more_diags = normalize_segments(segs)
    diagnostics.extend(more_diags)
    unit = str(data.get("unit") or "m")
    return LithologyModel(well_id=well_id, unit=unit, segments=segs), diagnostics


def save_lithology_for_well(workspace: Workspace, model: LithologyModel) -> Path:
    """Persist lithology segments next to well data (atomic write)."""
    path = lithology_file_path(workspace, model.well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    segments, _diags = normalize_segments(model.segments)
    payload = {
        "schemaVersion": LITHOLOGY_SCHEMA_VERSION,
        "well_id": model.well_id,
        "unit": model.unit or "m",
        "segments": [
            {
                "id": seg.id or str(uuid.uuid4()),
                "top": seg.top,
                "bottom": seg.bottom,
                "pattern_id": seg.pattern_id,
                "label": seg.label,
            }
            for seg in segments
        ],
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    tmp.replace(path)
    return path


def make_stub_lithology(
    depth_min: float,
    depth_max: float,
) -> list[LithologySegment]:
    """Demo segments at even depth bands (SY/T 5615 core patterns)."""
    if not (depth_max > depth_min):
        depth_min, depth_max = 0.0, 100.0
    span = depth_max - depth_min
    n = len(STUB_PATTERN_SEQUENCE)
    out: list[LithologySegment] = []
    for i, (pattern_id, label) in enumerate(STUB_PATTERN_SEQUENCE):
        out.append(
            LithologySegment(
                id=str(uuid.uuid4()),
                top=depth_min + span * i / n,
                bottom=depth_min + span * (i + 1) / n,
                pattern_id=pattern_id,
                label=label,
            )
        )
    return out
