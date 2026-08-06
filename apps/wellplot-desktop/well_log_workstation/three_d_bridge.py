"""3D capability probe for the fence_3d plot type (Phase-2, T6 / #250).

T6 resolution: the 3D fence view is an independent pyqtgraph GLViewWidget
surface (decoupled from WellLogEngine). ``probe_3d()`` mirrors the cached
probe shape of ``probe_engine`` / ``probe_mapping`` (T1): it checks that
``pyqtgraph.opengl`` and the promoted ``geoviz.generate_fence_mesh`` are
reachable. When unavailable, the fence_3d plot type presents as a disabled
menu entry with an actionable hint (bare-clone Workstation still starts).
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class ThreeDCapability:
    available: bool
    detail: str
    gl_view_cls: type | None = None


_cached_3d: ThreeDCapability | None = None


def reset_3d_capability_cache() -> None:
    """Clear the cached 3D-capability probe (test/CI hook)."""
    global _cached_3d
    _cached_3d = None


def probe_3d() -> ThreeDCapability:
    """Detect pyqtgraph.opengl + geoviz.generate_fence_mesh; result is cached.

    Fails closed (available False) when pyqtgraph/OpenGL is missing or the
    facade fence mesh is absent - callers degrade to a disabled menu item.
    """
    global _cached_3d
    if _cached_3d is not None:
        return _cached_3d

    if os.environ.get("WLWS_DISABLE_3D", "").strip() in ("1", "true", "yes"):
        _cached_3d = ThreeDCapability(False, "WLWS_DISABLE_3D set")
        return _cached_3d

    try:
        import pyqtgraph.opengl as gl  # noqa: F401
        from geoviz import generate_fence_mesh  # noqa: F401
        _cached_3d = ThreeDCapability(
            True, "pyqtgraph.opengl + geoviz.generate_fence_mesh", gl.GLViewWidget
        )
    except Exception as exc:  # noqa: BLE001
        _cached_3d = ThreeDCapability(False, f"3D unavailable: {exc}")
    return _cached_3d


def three_d_available() -> bool:
    """True when the fence_3d view can be constructed."""
    return probe_3d().available
