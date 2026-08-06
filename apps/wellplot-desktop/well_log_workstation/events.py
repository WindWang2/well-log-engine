"""In-process plot-change signal bus (Phase-2, T7 / #251; ADR 0051).

The composite figure (油藏综合图) must refresh its embedded source-plot
panels when the source plots change. The Workstation has no engine observer
Python surface (ViewEvent / DocumentRevision are C++-side), so the host
emits ``plot_changed`` after its own imperative save/render calls.

- ``plot_bus`` is a module-level singleton.
- ``plot_changed(plot_id: str, revision: int)`` - revision is a per-plot
  monotonic counter, persisted in ``plots/<id>.json`` (schema v3, ADR 0051):
  ``save_plot_document`` bumps on save (new committed state), ``load_plot_document``
  restores the persisted value on load (never regresses), ``emit_plot_changed``
  bumps on emit.
- Emit sites: shell create/display paths (save bumps without emitting).
"""

from __future__ import annotations

from PySide6.QtCore import QObject, Signal


class PlotEventBus(QObject):
    """Broadcasts per-plot revision changes within the process."""

    plot_changed = Signal(str, int)  # (plot_id, revision)


# Module-level singleton (T7).
plot_bus = PlotEventBus()

# Per-plot revision counters, seeded from persisted state on load (ADR 0051).
_revisions: dict[str, int] = {}


def bump_plot_revision(plot_id: str) -> int:
    """Increment + return the revision for a plot id."""
    rev = _revisions.get(plot_id, 0) + 1
    _revisions[plot_id] = rev
    return rev


def restore_plot_revision(plot_id: str, revision: int) -> int:
    """Seed the in-memory counter from persisted state; never regresses."""
    current = _revisions.get(plot_id, 0)
    restored = max(current, int(revision))
    _revisions[plot_id] = restored
    return restored


def emit_plot_changed(plot_id: str) -> int:
    """Bump the revision and emit ``plot_bus.plot_changed``.

    Returns the new revision so callers can correlate.
    """
    rev = bump_plot_revision(plot_id)
    plot_bus.plot_changed.emit(plot_id, rev)
    return rev


def plot_revision(plot_id: str) -> int:
    """Current revision for a plot id (0 when never bumped)."""
    return _revisions.get(plot_id, 0)


def reset_revisions() -> None:
    """Clear revision counters (test/CI hook)."""
    _revisions.clear()
