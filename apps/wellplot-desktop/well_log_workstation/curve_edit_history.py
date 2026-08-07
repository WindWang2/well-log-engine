"""Per-well curve-edit undo/redo stacks (FRS §3.x 全局撤销/重做).

Curve edits (despike / baseline / freehand) live in ``curve_edits.json``
and are recomputed into ``edited-*`` tracks at presentation time. Each save
(edit dialog commit, freehand stroke) is a history entry. ``CurveEdit`` is
frozen with tuple points, so a plain list copy is a valid snapshot —
mirrors ``TopsHistoryBook``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class _EditStack:
    undo: list[list[Any]] = field(default_factory=list)
    redo: list[list[Any]] = field(default_factory=list)


class CurveEditHistoryBook:
    """Undo/redo book keyed by well id (snapshots are list[CurveEdit])."""

    def __init__(self, *, max_depth: int = 64) -> None:
        self._max_depth = max(1, int(max_depth))
        self._wells: dict[str, _EditStack] = {}

    def clear_well(self, well_id: str) -> None:
        self._wells.pop(well_id, None)

    def clear_all(self) -> None:
        self._wells.clear()

    def can_undo(self, well_id: str | None) -> bool:
        if not well_id:
            return False
        stack = self._wells.get(well_id)
        return bool(stack and stack.undo)

    def can_redo(self, well_id: str | None) -> bool:
        if not well_id:
            return False
        stack = self._wells.get(well_id)
        return bool(stack and stack.redo)

    def record_before_commit(
        self, well_id: str, before: list[Any]
    ) -> None:
        """Push the *before* edits list prior to a save; clear redo."""
        stack = self._wells.setdefault(well_id, _EditStack())
        stack.undo.append(list(before))
        if len(stack.undo) > self._max_depth:
            stack.undo = stack.undo[-self._max_depth :]
        stack.redo.clear()

    def undo(self, well_id: str, current: list[Any]) -> list[Any] | None:
        stack = self._wells.get(well_id)
        if not stack or not stack.undo:
            return None
        previous = stack.undo.pop()
        stack.redo.append(list(current))
        return previous

    def redo(self, well_id: str, current: list[Any]) -> list[Any] | None:
        stack = self._wells.get(well_id)
        if not stack or not stack.redo:
            return None
        nxt = stack.redo.pop()
        stack.undo.append(list(current))
        return nxt
