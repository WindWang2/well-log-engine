"""Qt platform selection for the Well Log Workstation.

Policy (Wayland-first, #216):
- Interactive desktop: do **not** force a platform; Qt uses Wayland when
  ``WAYLAND_DISPLAY`` / session type is Wayland.
- CI / headless: leave ``QT_QPA_PLATFORM=offscreen`` (or ``minimal``) alone.
- Never default to X11 (``xcb``). Clear accidental ``xcb`` on Wayland unless
  ``WLWS_FORCE_XCB=1`` (debug only; preferred over paleo Workbench's flag name).
"""

from __future__ import annotations

import os
import sys

_HEADLESS_PLATFORMS = frozenset(
    {"offscreen", "minimal", "minimalegl", "vnc", "null"}
)
_X11_PLATFORMS = frozenset({"xcb", "x11"})


def session_is_wayland() -> bool:
    if os.environ.get("WAYLAND_DISPLAY"):
        return True
    return os.environ.get("XDG_SESSION_TYPE", "").strip().lower() == "wayland"


def _force_xcb() -> bool:
    for key in ("WLWS_FORCE_XCB", "PALEO_FORCE_XCB"):
        if os.environ.get(key, "").strip().lower() in {"1", "true", "yes"}:
            return True
    return False


def configure_qt_platform_for_session(*, warn: bool = True) -> str | None:
    """Normalize ``QT_QPA_PLATFORM`` before QApplication is constructed.

    Returns the remaining platform override, or ``None`` if Qt should pick
    (Wayland on Wayland sessions).
    """
    raw = os.environ.get("QT_QPA_PLATFORM", "")
    plat = raw.strip().lower()

    if plat in _HEADLESS_PLATFORMS:
        return plat or None

    if plat in _X11_PLATFORMS and session_is_wayland():
        if _force_xcb():
            return plat
        os.environ.pop("QT_QPA_PLATFORM", None)
        if warn:
            print(
                "well_log_workstation: cleared QT_QPA_PLATFORM=xcb on Wayland "
                "(set WLWS_FORCE_XCB=1 only for XWayland debugging).",
                file=sys.stderr,
            )
        return None

    if not plat:
        return None
    return plat


def effective_qt_platform_hint() -> str:
    """Read-only diagnostic string for status bar / docs."""
    plat = os.environ.get("QT_QPA_PLATFORM", "").strip().lower()
    if plat in _HEADLESS_PLATFORMS:
        return plat
    if plat in _X11_PLATFORMS and session_is_wayland():
        if _force_xcb():
            return f"{plat} (forced)"
        return "wayland preferred (xcb would be cleared)"
    if plat:
        return plat
    if session_is_wayland():
        return "wayland (session default)"
    if os.environ.get("DISPLAY"):
        return "xcb/x11 likely"
    return "unset"
