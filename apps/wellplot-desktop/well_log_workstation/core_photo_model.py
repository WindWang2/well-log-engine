"""Per-well core-photo segments for the single-well image track (FRS §2.x).

Qt-free model: depth-ranged core photographs, persisted next to tops under
``wells/<id>/core_photos.json``. Each segment references an image file
(copied into ``wells/<id>/core_photos/``) by relative path. Missing files
and bad JSON degrade to an empty model with diagnostics (mirrors
lithology_model / tops_model).

This first slice targets demo-scale images; large-format pyramid tiling
is a documented follow-up (the SDK ``ImagePyramid`` is C++-only and not
wired to the host canvas - same status as the lithology track today).
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

CORE_PHOTO_SCHEMA_VERSION = 1
CORE_PHOTO_FILENAME = "core_photos.json"
CORE_PHOTO_DIRNAME = "core_photos"


@dataclass(frozen=True)
class CorePhotoSegment:
    id: str
    top: float
    bottom: float
    image_path: str  # relative to wells/<id>/core_photos/
    label: str = ""

    def thickness(self) -> float:
        return self.bottom - self.top


@dataclass
class CorePhotoModel:
    well_id: str
    unit: str = "m"
    segments: list[CorePhotoSegment] = field(default_factory=list)


def core_photo_file_path(workspace: Workspace, well_id: str) -> Path:
    return well_data_dir(workspace, well_id) / CORE_PHOTO_FILENAME


def core_photo_dir(workspace: Workspace, well_id: str) -> Path:
    """Directory holding the image files referenced by segments."""
    return well_data_dir(workspace, well_id) / CORE_PHOTO_DIRNAME


def normalize_segments(
    segments: list[CorePhotoSegment] | None,
) -> tuple[list[CorePhotoSegment], list[str]]:
    """Drop invalid segments (assign missing ids), sort by top depth.

    Pure function — no IO. Returns ``(segments, diagnostics)``.
    """
    out: list[CorePhotoSegment] = []
    diagnostics: list[str] = []
    for i, seg in enumerate(segments or []):
        if (
            not math.isfinite(seg.top)
            or not math.isfinite(seg.bottom)
            or seg.bottom <= seg.top
        ):
            diagnostics.append(f"跳过无效岩心照片段 [{i}]（需 top < bottom）")
            continue
        if not str(seg.image_path).strip():
            diagnostics.append(f"跳过无效岩心照片段 [{i}]（缺 image_path）")
            continue
        out.append(
            CorePhotoSegment(
                id=seg.id or str(uuid.uuid4()),
                top=seg.top,
                bottom=seg.bottom,
                image_path=seg.image_path,
                label=seg.label,
            )
        )
    out.sort(key=lambda s: (s.top, s.bottom))
    return out, diagnostics


def _from_json_item(item: dict[str, Any]) -> CorePhotoSegment | None:
    try:
        top = float(item["top"])
        bottom = float(item["bottom"])
    except (KeyError, TypeError, ValueError):
        return None
    if not math.isfinite(top) or not math.isfinite(bottom) or bottom <= top:
        return None
    image_path = str(item.get("image_path") or "").strip()
    if not image_path:
        return None
    label = str(item.get("label") or "")
    seg_id = str(item.get("id") or "")
    return CorePhotoSegment(
        id=seg_id, top=top, bottom=bottom, image_path=image_path, label=label
    )


def load_core_photos_for_well(
    workspace: Workspace, well_id: str
) -> tuple[CorePhotoModel, list[str]]:
    """Load core-photo segments for a well; missing file → empty model.

    Returns ``(model, diagnostics)``; never raises for missing/bad file.
    """
    diagnostics: list[str] = []
    try:
        path = core_photo_file_path(workspace, well_id)
    except WorkspaceError as exc:
        return CorePhotoModel(well_id=well_id), [str(exc)]

    if not path.is_file():
        return CorePhotoModel(well_id=well_id), []

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return CorePhotoModel(well_id=well_id), [f"岩心照片文件无法读取: {exc}"]

    if not isinstance(data, dict):
        return CorePhotoModel(well_id=well_id), ["岩心照片 JSON 根必须是对象"]

    version = int(data.get("schemaVersion", 0))
    if version not in (0, CORE_PHOTO_SCHEMA_VERSION):
        diagnostics.append(
            f"不支持的岩心照片 schemaVersion={version}（期望 {CORE_PHOTO_SCHEMA_VERSION}）"
        )
    items = data.get("segments")
    if items is None:
        diagnostics.append("岩心照片文件缺少 segments 数组")
        return CorePhotoModel(well_id=well_id), diagnostics
    if not isinstance(items, list):
        diagnostics.append("segments 必须是数组")
        return CorePhotoModel(well_id=well_id), diagnostics

    segs: list[CorePhotoSegment] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            diagnostics.append(f"跳过无效岩心照片项 [{i}]")
            continue
        seg = _from_json_item(item)
        if seg is None:
            diagnostics.append(
                f"跳过无效岩心照片项 [{i}]（需 top < bottom + image_path）"
            )
            continue
        segs.append(seg)
    segs, more_diags = normalize_segments(segs)
    diagnostics.extend(more_diags)
    unit = str(data.get("unit") or "m")
    return CorePhotoModel(well_id=well_id, unit=unit, segments=segs), diagnostics


def save_core_photos_for_well(
    workspace: Workspace, model: CorePhotoModel
) -> Path:
    """Persist core-photo segments next to well data (atomic write)."""
    path = core_photo_file_path(workspace, model.well_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    segments, _diags = normalize_segments(model.segments)
    payload = {
        "schemaVersion": CORE_PHOTO_SCHEMA_VERSION,
        "well_id": model.well_id,
        "unit": model.unit or "m",
        "segments": [
            {
                "id": seg.id or str(uuid.uuid4()),
                "top": seg.top,
                "bottom": seg.bottom,
                "image_path": seg.image_path,
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
