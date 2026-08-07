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
from well_log_workstation.lithology_model import LithologySegment

ScaleMode = Literal["linear", "log"]


@dataclass
class ScaleSpec:
    mode: ScaleMode = "linear"
    min: float = 0.0
    max: float = 100.0
    unit: str = ""
    # Wrap-around (FRS §2.x 超量程折叠): when set, curve values beyond the
    # scale range fold back via modulo instead of clipping at the edge —
    # classic wireline-log rendering. Optional; default off.
    wrap: bool = False
    # Baseline fill (FRS §2.x 基线充填, e.g. GR>80): when ``fill_threshold``
    # is not None, the area between the curve and the track's right edge is
    # filled (semi-transparent) wherever the value is above (direction
    # "above") or below ("below") the threshold. Optional; default off.
    fill_threshold: float | None = None
    fill_direction: str = "above"
    # Crossover fill (FRS §2.x 双曲线交叉充填): when ``crossover_fill`` is
    # set on a track with ≥2 curve layers, the region where layers[0]'s
    # mapped x lies to the right of layers[1]'s is filled (SDK
    # upper_minus_lower semantics). ``crossover_color`` overrides the
    # default (layers[1] color, semi-transparent). Optional; default off.
    crossover_fill: bool = False
    crossover_color: str = ""


@dataclass
class BoundCurveLayer:
    mnemonic: str
    color: str
    unit: str
    values: Any  # np.ndarray
    null_mask: Any
    # Per-layer scale (FRS §2.x 双曲线交叉充填 / 对道): when set, this layer
    # maps values with its own scale instead of the track's. None = fall
    # back to the track scale. Optional; default None.
    scale: ScaleSpec | None = None


@dataclass
class BoundTrack:
    id: str
    role: str  # depth | curve | litho
    title: str
    width_fraction: float
    scale: ScaleSpec | None
    layers: list[BoundCurveLayer] = field(default_factory=list)
    # Lithology depth bands (role == "litho", FRS §2.x): SY/T 5615 segments.
    litho_segments: list[LithologySegment] = field(default_factory=list)
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
            line = (
                f"{t.title} {t.scale.min:g}–{t.scale.max:g}"
                + (f" {unit}" if unit else "")
            )
            if t.scale.wrap:
                line += " 折叠"
            bits.append(line)
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
    # Expand candidates through the workspace alias dictionary (FRS §1.2):
    # a GR slot can match a GRD curve when the workspace maps GR→[GRD].
    from well_log_workstation.mnemonic_alias import expand as _alias_expand

    upper_map = {c.mnemonic.upper(): c for c in doc.curves}
    for m in _alias_expand(mnemonics):
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
    fill_raw = raw.get("fill_threshold")
    try:
        fill_threshold = (
            float(fill_raw) if fill_raw is not None and str(fill_raw) != "" else None
        )
    except (TypeError, ValueError):
        fill_threshold = None
    fill_direction = str(raw.get("fill_direction") or "above")
    if fill_direction not in ("above", "below"):
        fill_direction = "above"
    return ScaleSpec(
        mode=mode,  # type: ignore[arg-type]
        min=float(raw.get("min", 0.0)),
        max=float(raw.get("max", 100.0)),
        unit=str(raw.get("unit") or ""),
        wrap=bool(raw.get("wrap", False)),
        fill_threshold=fill_threshold,
        fill_direction=fill_direction,
        crossover_fill=bool(raw.get("crossover_fill", False)),
        crossover_color=str(raw.get("crossover_color") or ""),
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
        litho_segments_out: list[LithologySegment] = []
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
                        scale=_parse_scale(layer.get("scale"))
                        if isinstance(layer.get("scale"), dict)
                        else None,
                    )
                )
        elif role == "litho" and document.lithology is not None:
            litho_segments_out = list(document.lithology.segments)
        bound_tracks.append(
            BoundTrack(
                id=str(t.get("id") or f"track-{len(bound_tracks)}"),
                role=role,
                title=str(t.get("title") or role),
                width_fraction=float(t.get("width_fraction") or 0.25),
                scale=_parse_scale(t.get("scale")),
                layers=layers_out,
                litho_segments=litho_segments_out,
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


def track_order_from_presentation(presentation: HostPresentation) -> list[str]:
    """Serialize the visible track order as an id list (canvas drag reorder).

    Persisted alongside track_overrides (plot document schema v8) so the
    presentation order survives reopens.
    """
    return [t.id for t in presentation.tracks]


def apply_track_order(
    presentation: HostPresentation,
    order: list[str] | None,
) -> HostPresentation:
    """Reorder presentation.tracks in place from a persisted id list.

    Only tracks whose id appears in ``order`` move (in the order given);
    unknown ids keep their relative order and trail the reordered ones.
    ``None`` / empty → no change. Returns the same object (like
    apply_track_overrides).
    """
    if not order:
        return presentation
    by_id = {t.id: t for t in presentation.tracks}
    moved: list[BoundTrack] = []
    seen: set[str] = set()
    for track_id in order:
        track = by_id.get(track_id)
        if track is None or track_id in seen:
            continue
        moved.append(track)
        seen.add(track_id)
    if not moved:
        return presentation
    remaining = [t for t in presentation.tracks if t.id not in seen]
    presentation.tracks = moved + remaining
    return presentation


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
            entry["scale_wrap"] = bool(track.scale.wrap)
            if track.scale.fill_threshold is not None:
                entry["scale_fill_threshold"] = float(track.scale.fill_threshold)
                entry["scale_fill_direction"] = str(track.scale.fill_direction)
            if track.scale.crossover_fill:
                entry["scale_crossover_fill"] = True
                entry["scale_crossover_color"] = str(track.scale.crossover_color)
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
            if "scale_wrap" in raw:
                track.scale.wrap = bool(raw["scale_wrap"])
            if "scale_fill_threshold" in raw:
                try:
                    track.scale.fill_threshold = float(raw["scale_fill_threshold"])
                except (TypeError, ValueError):
                    track.scale.fill_threshold = None
            if "scale_fill_direction" in raw:
                fd = str(raw["scale_fill_direction"] or "above")
                track.scale.fill_direction = (
                    fd if fd in ("above", "below") else "above"
                )
            if "scale_crossover_fill" in raw:
                track.scale.crossover_fill = bool(raw["scale_crossover_fill"])
            if "scale_crossover_color" in raw:
                track.scale.crossover_color = str(raw["scale_crossover_color"] or "")
            # Keep scale valid
            if track.scale.max <= track.scale.min:
                track.scale.max = track.scale.min + 1.0
        elif any(
            k in raw
            for k in (
                "scale_min",
                "scale_max",
                "scale_mode",
                "scale_wrap",
                "scale_fill_threshold",
                "scale_crossover_fill",
            )
        ):
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
            try:
                ft = (
                    float(raw["scale_fill_threshold"])
                    if "scale_fill_threshold" in raw
                    else None
                )
            except (TypeError, ValueError):
                ft = None
            fd = str(raw.get("scale_fill_direction") or "above")
            track.scale = ScaleSpec(
                mode=mode,  # type: ignore[arg-type]
                min=smin,
                max=smax,
                wrap=bool(raw.get("scale_wrap", False)),
                fill_threshold=ft,
                fill_direction=fd if fd in ("above", "below") else "above",
                crossover_fill=bool(raw.get("scale_crossover_fill", False)),
                crossover_color=str(raw.get("scale_crossover_color") or ""),
            )
    return presentation
