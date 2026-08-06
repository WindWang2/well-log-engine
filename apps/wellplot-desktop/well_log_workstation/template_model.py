"""Host multi-track plot templates → display presentation (decision H / #219).

Templates are versioned JSON. Applying a template binds well curves by mnemonic
aliases into tracks. Runtime layout is a single ``HostPresentation`` (mirrors
engine ScenePresentation ownership until C++ presentation is bound from Python).
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from importlib import resources
from pathlib import Path
from typing import Any, Literal

from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument

ScaleMode = Literal["linear", "log"]


@dataclass
class ScaleSpec:
    mode: ScaleMode = "linear"
    min: float = 0.0
    max: float = 100.0
    unit: str = ""


@dataclass
class BoundCurveLayer:
    mnemonic: str
    color: str
    unit: str
    values: Any  # np.ndarray
    null_mask: Any


@dataclass
class BoundTrack:
    id: str
    role: str  # depth | curve
    title: str
    width_fraction: float
    scale: ScaleSpec | None
    layers: list[BoundCurveLayer] = field(default_factory=list)
    # Runtime layout edit (#292 / T4); default visible.
    visible: bool = True


@dataclass
class PlotHeaderSpec:
    """Minimal single-well plot header/footer (#293 / T5).

    Built from template JSON ``header`` / ``footer`` and resolved at apply time
    with well name, depth unit, and scale summary.
    """

    title: str = ""
    show_well_name: bool = True
    show_depth_scale: bool = True
    show_template_name: bool = True
    extra_lines: list[str] = field(default_factory=list)
    # Footer
    show_depth_range: bool = True
    footer_text: str = ""

    def header_lines(
        self,
        *,
        well_name: str,
        template_name: str,
        depth_unit: str,
        scale_summary: str,
    ) -> list[str]:
        """Resolved display lines for the header band (non-empty)."""
        lines: list[str] = []
        title = (self.title or "").strip() or template_name
        if title:
            lines.append(title)
        meta: list[str] = []
        if self.show_well_name and well_name:
            meta.append(f"井: {well_name}")
        if self.show_depth_scale:
            unit = depth_unit or "m"
            meta.append(f"深度: {unit}")
            if scale_summary:
                meta.append(scale_summary)
        if self.show_template_name and template_name:
            meta.append(f"图版: {template_name}")
        if meta:
            lines.append(" · ".join(meta))
        for raw in self.extra_lines:
            s = str(raw).strip()
            if s:
                lines.append(s)
        return lines or [template_name or "单井图"]

    def footer_line(
        self,
        *,
        depth_range: tuple[float, float] | None,
        depth_unit: str,
    ) -> str:
        parts: list[str] = []
        if self.footer_text.strip():
            parts.append(self.footer_text.strip())
        if self.show_depth_range and depth_range is not None:
            d0, d1 = depth_range
            unit = depth_unit or "m"
            parts.append(f"视窗 {d0:.2f}–{d1:.2f} {unit}")
        return " · ".join(parts)


@dataclass
class HostPresentation:
    """Compiled multi-track layout for one well (single layout owner for UI)."""

    template_id: str
    template_name: str
    well_document_id: str
    well_name: str
    depth: Any
    depth_unit: str
    tracks: list[BoundTrack]
    header: PlotHeaderSpec = field(default_factory=PlotHeaderSpec)

    @property
    def track_count(self) -> int:
        return len(self.tracks)

    @property
    def visible_tracks(self) -> list[BoundTrack]:
        return [t for t in self.tracks if t.visible]

    @property
    def curve_track_count(self) -> int:
        return sum(1 for t in self.tracks if t.role == "curve")

    def scale_summary(self) -> str:
        """Short curve-scale summary for header (e.g. GR 0–150 GAPI)."""
        bits: list[str] = []
        for t in self.tracks:
            if t.role != "curve" or not t.visible or t.scale is None:
                continue
            unit = t.scale.unit or ""
            bits.append(
                f"{t.title} {t.scale.min:g}–{t.scale.max:g}"
                + (f" {unit}" if unit else "")
            )
            if len(bits) >= 3:
                break
        return " · ".join(bits)


@dataclass
class PlotTemplate:
    id: str
    name: str
    tracks: list[dict[str, Any]]
    schema_version: int = 1
    header: dict[str, Any] = field(default_factory=dict)
    footer: dict[str, Any] = field(default_factory=dict)


def _templates_package_dir() -> Path:
    return Path(__file__).resolve().parent / "templates"


def load_template_file(path: Path | str) -> PlotTemplate:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return PlotTemplate(
        id=str(data["id"]),
        name=str(data.get("name") or data["id"]),
        tracks=list(data.get("tracks") or []),
        schema_version=int(data.get("schemaVersion", 1)),
        header=dict(data.get("header") or {}),
        footer=dict(data.get("footer") or {}),
    )


def header_spec_from_template(template: PlotTemplate) -> PlotHeaderSpec:
    """Parse template header/footer dicts into PlotHeaderSpec."""
    h = template.header or {}
    f = template.footer or {}
    extra = h.get("lines") or h.get("extra_lines") or []
    if not isinstance(extra, list):
        extra = []
    return PlotHeaderSpec(
        title=str(h.get("title") or ""),
        show_well_name=bool(h.get("show_well_name", True)),
        show_depth_scale=bool(h.get("show_depth_scale", True)),
        show_template_name=bool(h.get("show_template_name", True)),
        extra_lines=[str(x) for x in extra],
        show_depth_range=bool(f.get("show_depth_range", True)),
        footer_text=str(f.get("text") or f.get("footer_text") or ""),
    )


def list_builtin_templates() -> list[PlotTemplate]:
    root = _templates_package_dir()
    out: list[PlotTemplate] = []
    if not root.is_dir():
        return out
    for path in sorted(root.glob("*.json")):
        try:
            out.append(load_template_file(path))
        except (OSError, json.JSONDecodeError, KeyError):
            continue
    return out


def get_builtin_template(template_id: str) -> PlotTemplate | None:
    for t in list_builtin_templates():
        if t.id == template_id:
            return t
    return None


def _match_curve(
    doc: ImportedWellDocument, mnemonics: list[str]
) -> ImportedCurve | None:
    upper_map = {c.mnemonic.upper(): c for c in doc.curves}
    for m in mnemonics:
        hit = upper_map.get(m.strip().upper())
        if hit is not None:
            return hit
    return None


def _parse_scale(raw: dict[str, Any] | None) -> ScaleSpec | None:
    if not raw:
        return None
    mode = str(raw.get("mode") or "linear")
    if mode not in ("linear", "log"):
        mode = "linear"
    return ScaleSpec(
        mode=mode,  # type: ignore[arg-type]
        min=float(raw.get("min", 0.0)),
        max=float(raw.get("max", 100.0)),
        unit=str(raw.get("unit") or ""),
    )


def apply_template(
    template: PlotTemplate, document: ImportedWellDocument
) -> HostPresentation:
    """Compile template + well data into a multi-track HostPresentation."""
    if not template.tracks:
        raise ValueError("template has no tracks")

    bound_tracks: list[BoundTrack] = []
    for t in template.tracks:
        role = str(t.get("role") or "curve")
        layers_out: list[BoundCurveLayer] = []
        if role == "curve":
            for layer in t.get("layers") or []:
                if str(layer.get("type") or "curve") != "curve":
                    continue
                mnemos = [str(x) for x in (layer.get("mnemonics") or [])]
                curve = _match_curve(document, mnemos)
                if curve is None:
                    continue
                layers_out.append(
                    BoundCurveLayer(
                        mnemonic=curve.mnemonic,
                        color=str(layer.get("color") or "#1a6fb5"),
                        unit=curve.unit,
                        values=curve.values,
                        null_mask=curve.null_mask,
                    )
                )
        bound_tracks.append(
            BoundTrack(
                id=str(t.get("id") or f"track-{len(bound_tracks)}"),
                role=role,
                title=str(t.get("title") or role),
                width_fraction=float(t.get("width_fraction") or 0.25),
                scale=_parse_scale(t.get("scale")),
                layers=layers_out,
            )
        )

    # Ensure at least depth + one curve track for a usable multi-track plot.
    if not any(t.role == "depth" for t in bound_tracks):
        bound_tracks.insert(
            0,
            BoundTrack(
                id="depth",
                role="depth",
                title="深度",
                width_fraction=0.12,
                scale=None,
                layers=[],
            ),
        )

    return HostPresentation(
        template_id=template.id,
        template_name=template.name,
        well_document_id=document.document_id,
        well_name=document.well_name,
        depth=document.depth,
        depth_unit=document.depth_unit,
        tracks=bound_tracks,
        header=header_spec_from_template(template),
    )


def track_overrides_snapshot(presentation: HostPresentation) -> dict[str, dict[str, Any]]:
    """Serialize editable track props for plot-document persistence (#292)."""
    out: dict[str, dict[str, Any]] = {}
    for track in presentation.tracks:
        entry: dict[str, Any] = {
            "visible": bool(track.visible),
            "title": track.title,
            "width_fraction": float(track.width_fraction),
        }
        if track.scale is not None:
            entry["scale_min"] = float(track.scale.min)
            entry["scale_max"] = float(track.scale.max)
            entry["scale_mode"] = str(track.scale.mode)
        out[track.id] = entry
    return out


def apply_track_overrides(
    presentation: HostPresentation,
    overrides: dict[str, dict[str, Any]] | None,
) -> HostPresentation:
    """Mutate presentation tracks in place from saved overrides; return same object."""
    if not overrides:
        return presentation
    for track in presentation.tracks:
        raw = overrides.get(track.id)
        if not isinstance(raw, dict):
            continue
        if "visible" in raw:
            track.visible = bool(raw["visible"])
        if "title" in raw and str(raw["title"]).strip():
            track.title = str(raw["title"])
        if "width_fraction" in raw:
            try:
                wf = float(raw["width_fraction"])
                if wf > 0:
                    track.width_fraction = wf
            except (TypeError, ValueError):
                pass
        if track.scale is not None:
            if "scale_min" in raw:
                try:
                    track.scale.min = float(raw["scale_min"])
                except (TypeError, ValueError):
                    pass
            if "scale_max" in raw:
                try:
                    track.scale.max = float(raw["scale_max"])
                except (TypeError, ValueError):
                    pass
            if "scale_mode" in raw:
                mode = str(raw["scale_mode"] or "linear")
                if mode in ("linear", "log"):
                    track.scale.mode = mode  # type: ignore[assignment]
            # Keep scale valid
            if track.scale.max <= track.scale.min:
                track.scale.max = track.scale.min + 1.0
        elif any(k in raw for k in ("scale_min", "scale_max", "scale_mode")):
            # Curve tracks without scale (unusual) — create one if edited
            try:
                smin = float(raw.get("scale_min", 0.0))
                smax = float(raw.get("scale_max", 100.0))
            except (TypeError, ValueError):
                smin, smax = 0.0, 100.0
            if smax <= smin:
                smax = smin + 1.0
            mode = str(raw.get("scale_mode") or "linear")
            if mode not in ("linear", "log"):
                mode = "linear"
            track.scale = ScaleSpec(mode=mode, min=smin, max=smax)  # type: ignore[arg-type]
    return presentation
