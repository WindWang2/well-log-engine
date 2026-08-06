"""In-process host Command audit ring (T17 / #305 · P.AUDIT skeleton).

Append-only memory log for host actions (export, import, plot open, …).
Does **not** implement full plugin Runtime, disk persistence, or UI
(those are P.AUDIT follow-ups per ADR 0055). Never stores raw curve samples.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from threading import Lock
from typing import Any, Deque, Iterator


@dataclass(frozen=True)
class CommandAuditRecord:
    """One audited host command."""

    name: str
    timestamp_utc: str
    ok: bool
    target: str = ""
    detail: str = ""


@dataclass
class CommandAuditLog:
    """Thread-safe ring buffer of command records."""

    capacity: int = 256
    _items: Deque[CommandAuditRecord] = field(default_factory=deque, repr=False)
    _lock: Lock = field(default_factory=Lock, repr=False)

    def __post_init__(self) -> None:
        if self.capacity < 1:
            self.capacity = 1
        # ensure deque has correct maxlen if reconstructed
        with self._lock:
            self._items = deque(self._items, maxlen=self.capacity)

    def record(
        self,
        name: str,
        *,
        ok: bool = True,
        target: str = "",
        detail: str = "",
    ) -> CommandAuditRecord:
        """Append one record; drops oldest when full."""
        rec = CommandAuditRecord(
            name=str(name),
            timestamp_utc=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            ok=bool(ok),
            target=str(target or ""),
            detail=str(detail or "")[:500],
        )
        with self._lock:
            if self._items.maxlen != self.capacity:
                self._items = deque(self._items, maxlen=self.capacity)
            self._items.append(rec)
        return rec

    def __len__(self) -> int:
        with self._lock:
            return len(self._items)

    def recent(self, n: int = 50) -> list[CommandAuditRecord]:
        with self._lock:
            items = list(self._items)
        if n <= 0:
            return []
        return items[-n:]

    def clear(self) -> None:
        with self._lock:
            self._items.clear()

    def __iter__(self) -> Iterator[CommandAuditRecord]:
        return iter(self.recent(len(self)))


# Process-wide default log for the desktop shell (optional use).
_default_log = CommandAuditLog()


def get_default_audit_log() -> CommandAuditLog:
    return _default_log


def audit(name: str, **kwargs: Any) -> CommandAuditRecord:
    """Record on the process default log."""
    return _default_log.record(name, **kwargs)
