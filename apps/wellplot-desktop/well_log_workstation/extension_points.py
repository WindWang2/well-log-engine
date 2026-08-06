"""Built-in extension catalogue for WellPlot Desktop (T17 / #305 · P.SPEC).

First-ship surface is **declarative Custom Layer + same-toolchain embed**
(ADR 0018 / 0046). This module lists those capabilities for docs/UI; it does
**not** load third-party code or scan entry points (that is P.DISC / P.LOAD).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

ExtensionKind = Literal[
    "custom_layer_primitives",
    "engine_embed",
    "export_backend_host",
    "future_plugin_runtime",
]


@dataclass(frozen=True)
class ExtensionPoint:
    """One host/engine extension capability."""

    id: str
    kind: ExtensionKind
    title: str
    available_in_first_ship: bool
    notes: str


# Stable catalogue — extend when P.REG lands; do not invent runtime loading here.
BUILTIN_EXTENSION_POINTS: tuple[ExtensionPoint, ...] = (
    ExtensionPoint(
        id="wellplot.custom_layer",
        kind="custom_layer_primitives",
        title="声明式 Custom Layer",
        available_in_first_ship=True,
        notes="ADR 0018/0046 — polylines/triangles/rects/symbols; kernel prepares scene",
    ),
    ExtensionPoint(
        id="wellplot.engine_embed",
        kind="engine_embed",
        title="WellLogEngine 同源嵌入",
        available_in_first_ship=True,
        notes="Python bindings + host canvas; not a third-party plugin ABI",
    ),
    ExtensionPoint(
        id="wellplot.export_dispatch",
        kind="export_backend_host",
        title="宿主导出路由",
        available_in_first_ship=True,
        notes="export_dispatch routes SVG/PDF/PNG/CGM; B1 backends are first-party",
    ),
    ExtensionPoint(
        id="wellplot.plugin_runtime",
        kind="future_plugin_runtime",
        title="完整插件 Runtime",
        available_in_first_ship=False,
        notes="ADR 0055 / docs/plugin-runtime-status.md — discovery/load/sandbox later",
    ),
)


def list_extension_points(*, first_ship_only: bool = False) -> list[ExtensionPoint]:
    """Return built-in extension points (never loads packages)."""
    rows = list(BUILTIN_EXTENSION_POINTS)
    if first_ship_only:
        rows = [p for p in rows if p.available_in_first_ship]
    return rows


def first_ship_extension_ids() -> frozenset[str]:
    return frozenset(p.id for p in BUILTIN_EXTENSION_POINTS if p.available_in_first_ship)
