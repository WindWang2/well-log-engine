"""Per-well formation-tops undo/redo stacks (#294 / T6).

Host tops live in ``tops.json`` and are projected into the engine as markers on
each multi-track submit (immutable buffers — not in-place buffer edits). Session
``DocumentPatch`` undo is not yet bound for host tops; this history is the
Desktop first-ship command surface for tops edits (add / remove / move).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from well_log_workstation.tops_model import FormationTop


def snapshot_tops(tops: list[FormationTop]) -> list[FormationTop]:
    """Deep-ish copy of tops for stack entries (FormationTop is frozen)."""
    return [
        FormationTop(
            name=t.name,
            depth=float(t.depth),
            unit=t.unit,
            color=t.color,
            id=t.id,
        )
        for t in tops
    ]


@dataclass
class _WellStack:
    undo: list[list[FormationTop]] = field(default_factory=list)
    redo: list[list[FormationTop]] = field(default_factory=list)


class TopsHistoryBook:
    """Undo/redo book keyed by well id."""

    def __init__(self, *, max_depth: int = 64) -> None:
        self._max_depth = max(1, int(max_depth))
        self._wells: dict[str, _WellStack] = {}

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
        self, well_id: str, before: list[FormationTop]
    ) -> None:
        """Push *before* state prior to applying a new tops list; clear redo."""
        stack = self._wells.setdefault(well_id, _WellStack())
        stack.undo.append(snapshot_tops(before))
        if len(stack.undo) > self._max_depth:
            stack.undo = stack.undo[-self._max_depth :]
        stack.redo.clear()

    def undo(
        self, well_id: str, current: list[FormationTop]
    ) -> list[FormationTop] | None:
        stack = self._wells.get(well_id)
        if not stack or not stack.undo:
            return None
        previous = stack.undo.pop()
        stack.redo.append(snapshot_tops(current))
        return previous

    def redo(
        self, well_id: str, current: list[FormationTop]
    ) -> list[FormationTop] | None:
        stack = self._wells.get(well_id)
        if not stack or not stack.redo:
            return None
        nxt = stack.redo.pop()
        stack.undo.append(snapshot_tops(current))
        return nxt
