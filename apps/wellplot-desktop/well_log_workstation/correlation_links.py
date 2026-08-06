"""Horizon links between wells on correlation-lite plots (#229)."""

from __future__ import annotations

import uuid
from dataclasses import dataclass
from typing import Any

from well_log_workstation.tops_model import FormationTop


@dataclass(frozen=True)
class HorizonLink:
    """Link a named horizon between two wells (usually by matching top name)."""

    id: str
    left_well_id: str
    right_well_id: str
    name: str
    left_depth: float
    right_depth: float
    left_marker_id: str = ""
    right_marker_id: str = ""
    color: str = "#8e44ad"

    def to_json(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "left_well_id": self.left_well_id,
            "right_well_id": self.right_well_id,
            "name": self.name,
            "left_depth": self.left_depth,
            "right_depth": self.right_depth,
            "left_marker_id": self.left_marker_id,
            "right_marker_id": self.right_marker_id,
            "color": self.color,
        }

    @staticmethod
    def from_json(data: dict[str, Any]) -> HorizonLink | None:
        try:
            name = str(data.get("name") or "").strip()
            if not name:
                return None
            return HorizonLink(
                id=str(data.get("id") or uuid.uuid4()),
                left_well_id=str(data["left_well_id"]),
                right_well_id=str(data["right_well_id"]),
                name=name,
                left_depth=float(data["left_depth"]),
                right_depth=float(data["right_depth"]),
                left_marker_id=str(data.get("left_marker_id") or ""),
                right_marker_id=str(data.get("right_marker_id") or ""),
                color=str(data.get("color") or "#8e44ad"),
            )
        except (KeyError, TypeError, ValueError):
            return None


def make_horizon_link(
    left_well_id: str,
    left_top: FormationTop,
    right_well_id: str,
    right_top: FormationTop,
    *,
    name: str | None = None,
    color: str | None = None,
) -> HorizonLink:
    """Build a link between two tops on different wells."""
    if left_well_id == right_well_id:
        raise ValueError("连线两端必须是不同井")
    label = (name or left_top.name or right_top.name or "link").strip()
    if not label:
        label = "link"
    return HorizonLink(
        id=str(uuid.uuid4()),
        left_well_id=left_well_id,
        right_well_id=right_well_id,
        name=label,
        left_depth=float(left_top.depth),
        right_depth=float(right_top.depth),
        left_marker_id=left_top.id or str(uuid.uuid4()),
        right_marker_id=right_top.id or str(uuid.uuid4()),
        color=color or left_top.color or right_top.color or "#8e44ad",
    )


def match_tops_by_name(
    well_ids: list[str],
    tops_by_well: dict[str, list[FormationTop]],
    *,
    adjacent_only: bool = True,
) -> list[HorizonLink]:
    """Create links for tops with the same name on neighboring wells.

    When ``adjacent_only`` is True (default), only well_ids[i]–well_ids[i+1]
    pairs are linked — matches correlation column order.
    """
    if len(well_ids) < 2:
        return []
    links: list[HorizonLink] = []
    pairs: list[tuple[str, str]] = []
    if adjacent_only:
        pairs = list(zip(well_ids, well_ids[1:]))
    else:
        for i, a in enumerate(well_ids):
            for b in well_ids[i + 1 :]:
                pairs.append((a, b))

    for left_id, right_id in pairs:
        left_tops = {t.name.strip(): t for t in tops_by_well.get(left_id, []) if t.name.strip()}
        right_tops = {
            t.name.strip(): t for t in tops_by_well.get(right_id, []) if t.name.strip()
        }
        for name in sorted(set(left_tops) & set(right_tops)):
            lt, rt = left_tops[name], right_tops[name]
            lid = lt.id if lt.id else str(uuid.uuid4())
            rid = rt.id if rt.id else str(uuid.uuid4())
            links.append(
                HorizonLink(
                    id=str(uuid.uuid4()),
                    left_well_id=left_id,
                    right_well_id=right_id,
                    name=name,
                    left_depth=float(lt.depth),
                    right_depth=float(rt.depth),
                    left_marker_id=lid,
                    right_marker_id=rid,
                    color=lt.color or "#8e44ad",
                )
            )
    return links


def links_to_engine_overlays(
    links: list[HorizonLink],
    *,
    well_document_ids: dict[str, str] | None = None,
) -> list[dict[str, Any]]:
    """Map host links to submit_multi_well_section overlay dicts.

    ``well_document_ids`` maps catalog well_id → document_id used in payload
    (defaults to identity). Engine requires non-nil UUID strings.
    """
    idmap = well_document_ids or {}
    out: list[dict[str, Any]] = []
    for link in links:
        left_doc = idmap.get(link.left_well_id, link.left_well_id)
        right_doc = idmap.get(link.right_well_id, link.right_well_id)
        out.append(
            {
                "id": link.id if _is_uuid(link.id) else str(uuid.uuid4()),
                "kind": "horizon_line",
                "left_document_id": left_doc if _is_uuid(left_doc) else str(uuid.uuid4()),
                "right_document_id": right_doc if _is_uuid(right_doc) else str(uuid.uuid4()),
                "left_marker_id": (
                    link.left_marker_id
                    if _is_uuid(link.left_marker_id)
                    else str(uuid.uuid4())
                ),
                "right_marker_id": (
                    link.right_marker_id
                    if _is_uuid(link.right_marker_id)
                    else str(uuid.uuid4())
                ),
            }
        )
    return out


def _is_uuid(text: str) -> bool:
    try:
        uuid.UUID(text)
        return True
    except (ValueError, TypeError, AttributeError):
        return False
