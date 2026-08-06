"""Recent workspace paths for WellPlot Desktop startup page (#291 / T3).

Persisted via QSettings under branding organization/application names.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings

from well_log_workstation.branding import ORGANIZATION_NAME, PRODUCT_NAME

_SETTINGS_KEY = "recent_workspaces"
_DEFAULT_MAX = 12


def _settings() -> QSettings:
    return QSettings(ORGANIZATION_NAME, PRODUCT_NAME)


def load_recent(*, max_entries: int = _DEFAULT_MAX) -> list[str]:
    """Return recent workspace root paths (most recent first), de-duplicated."""
    raw = _settings().value(_SETTINGS_KEY, [])
    if raw is None:
        return []
    if isinstance(raw, str):
        items = [raw] if raw.strip() else []
    elif isinstance(raw, (list, tuple)):
        items = [str(x) for x in raw if str(x).strip()]
    else:
        items = []
    seen: set[str] = set()
    out: list[str] = []
    for p in items:
        key = str(Path(p).expanduser().resolve()) if Path(p).exists() else str(p)
        # Keep unresolved string for missing paths so user can clear them
        display = str(Path(p).expanduser())
        if display in seen:
            continue
        seen.add(display)
        out.append(display)
        if len(out) >= max_entries:
            break
    return out


def add_recent(path: Path | str, *, max_entries: int = _DEFAULT_MAX) -> list[str]:
    """Prepend a workspace path and persist; returns updated list."""
    p = str(Path(path).expanduser().resolve())
    prev = load_recent(max_entries=max_entries * 2)
    next_list = [p] + [x for x in prev if Path(x).expanduser().resolve() != Path(p)]
    next_list = next_list[:max_entries]
    _settings().setValue(_SETTINGS_KEY, next_list)
    return next_list


def remove_recent(path: Path | str) -> list[str]:
    """Drop a path (missing or user-cleared) from the recent list."""
    target = Path(path).expanduser()
    try:
        target_res = target.resolve()
    except OSError:
        target_res = target
    prev = load_recent()
    next_list: list[str] = []
    for x in prev:
        xp = Path(x).expanduser()
        try:
            if xp.resolve() == target_res or str(xp) == str(target):
                continue
        except OSError:
            if str(xp) == str(target):
                continue
        next_list.append(x)
    _settings().setValue(_SETTINGS_KEY, next_list)
    return next_list


def clear_recent() -> None:
    _settings().setValue(_SETTINGS_KEY, [])
