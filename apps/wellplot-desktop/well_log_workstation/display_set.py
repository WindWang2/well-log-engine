"""Display Set × plot template dual-layer composition (T1 / #341).

Pure seam: no UI, no HostPresentation binding. Later tickets rebuild the plot
from ``compose`` output; this module only decides *which* leaves participate
and *which style source* each gets.

Product rules (design §4):
- Display Set = checked leaf identities
- Template supplies layout/style for matched slots
- Unmatched checked leaves get default style
- Template never drops a checked leaf; empty set → empty track list
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import AbstractSet, Iterable, Sequence

from well_log_workstation.las_import import ImportedWellDocument
from well_log_workstation.template_model import (
    BoundCurveLayer,
    BoundTrack,
    HostPresentation,
    PlotTemplate,
    ScaleSpec,
    header_spec_from_template,
)


class StyleSource(str, Enum):
    TEMPLATE = "template"
    DEFAULT = "default"


@dataclass(frozen=True, slots=True)
class DisplayableTrackLeaf:
    """Source-side displayable track instance (stable id; first-ship: scalar)."""

    id: str
    mnemonic: str
    source_id: str = ""
    label: str = ""


@dataclass(frozen=True, slots=True)
class StyledTrackDescriptor:
    """One composed curve track for the single-well presentation path."""

    leaf_id: str
    mnemonic: str
    title: str
    style_source: StyleSource
    template_slot_id: str | None
    width_fraction: float
    color: str
    scale: ScaleSpec | None


# Default style for unmatched checked leaves (T1; layout polish is open).
_DEFAULT_WIDTH = 0.25
_DEFAULT_COLOR = "#5b8c5a"
_DEFAULT_SCALE = ScaleSpec(mode="linear", min=0.0, max=100.0, unit="")


def _norm(mnemonic: str) -> str:
    return mnemonic.strip().upper()


def _slot_mnemonics(track: dict) -> list[str]:
    out: list[str] = []
    if str(track.get("role") or "curve") != "curve":
        return out
    for layer in track.get("layers") or []:
        if str(layer.get("type") or "curve") != "curve":
            continue
        for m in layer.get("mnemonics") or []:
            out.append(str(m))
    return out


def _slot_color(track: dict) -> str:
    for layer in track.get("layers") or []:
        if str(layer.get("type") or "curve") != "curve":
            continue
        color = str(layer.get("color") or "").strip()
        if color:
            return color
    return "#1a6fb5"


def _parse_scale(raw: object) -> ScaleSpec | None:
    if not isinstance(raw, dict) or not raw:
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


def _curve_slots(template: PlotTemplate) -> list[dict]:
    return [
        t
        for t in template.tracks
        if str(t.get("role") or "curve") == "curve" and _slot_mnemonics(t)
    ]


def _leaf_matches_slot(leaf: DisplayableTrackLeaf, slot_mnemos: Sequence[str]) -> bool:
    want = _norm(leaf.mnemonic)
    return any(_norm(m) == want for m in slot_mnemos)


def default_checks(
    leaves: Sequence[DisplayableTrackLeaf],
    template: PlotTemplate,
) -> frozenset[str]:
    """Leaf ids the current template can match (one leaf per curve slot)."""
    remaining = list(leaves)
    chosen: list[str] = []
    for track in _curve_slots(template):
        mnemos = _slot_mnemonics(track)
        for i, leaf in enumerate(remaining):
            if _leaf_matches_slot(leaf, mnemos):
                chosen.append(leaf.id)
                del remaining[i]
                break
    return frozenset(chosen)


def compose(
    leaves: Sequence[DisplayableTrackLeaf],
    display_set: AbstractSet[str],
    template: PlotTemplate,
) -> list[StyledTrackDescriptor]:
    """Ordered styled tracks for every known checked leaf.

    Order: template-matched slots (template track order), then remaining
    checked leaves in input leaf order with default style.
    """
    if not display_set:
        return []

    by_id = {leaf.id: leaf for leaf in leaves}
    checked_ids = {lid for lid in display_set if lid in by_id}
    if not checked_ids:
        return []

    used: set[str] = set()
    out: list[StyledTrackDescriptor] = []

    for track in _curve_slots(template):
        mnemos = _slot_mnemonics(track)
        match: DisplayableTrackLeaf | None = None
        for leaf in leaves:
            if leaf.id not in checked_ids or leaf.id in used:
                continue
            if _leaf_matches_slot(leaf, mnemos):
                match = leaf
                break
        if match is None:
            continue
        used.add(match.id)
        slot_id = str(track.get("id") or "") or None
        title = str(track.get("title") or match.label or match.mnemonic)
        out.append(
            StyledTrackDescriptor(
                leaf_id=match.id,
                mnemonic=match.mnemonic,
                title=title,
                style_source=StyleSource.TEMPLATE,
                template_slot_id=slot_id,
                width_fraction=float(track.get("width_fraction") or _DEFAULT_WIDTH),
                color=_slot_color(track),
                scale=_parse_scale(track.get("scale")),
            )
        )

    for leaf in leaves:
        if leaf.id not in checked_ids or leaf.id in used:
            continue
        used.add(leaf.id)
        title = (leaf.label or leaf.mnemonic).strip() or leaf.mnemonic
        out.append(
            StyledTrackDescriptor(
                leaf_id=leaf.id,
                mnemonic=leaf.mnemonic,
                title=title,
                style_source=StyleSource.DEFAULT,
                template_slot_id=None,
                width_fraction=_DEFAULT_WIDTH,
                color=_DEFAULT_COLOR,
                scale=_DEFAULT_SCALE,
            )
        )

    return out


def display_set_from_ids(ids: Iterable[str]) -> frozenset[str]:
    """Normalize an iterable of leaf ids into a Display Set."""
    return frozenset(str(i) for i in ids if str(i))


def leaf_id_for_curve(document_id: str, mnemonic: str) -> str:
    """Stable Displayable Track Leaf id for a scalar curve on a document."""
    return f"{document_id}:{mnemonic}"


def leaves_from_document(document: ImportedWellDocument) -> list[DisplayableTrackLeaf]:
    """Build first-ship scalar leaves for an imported well document (one source)."""
    source_id = document.source_path or document.document_id
    return [
        DisplayableTrackLeaf(
            id=leaf_id_for_curve(document.document_id, curve.mnemonic),
            mnemonic=curve.mnemonic,
            source_id=source_id,
            label=curve.mnemonic,
        )
        for curve in document.curves
    ]


def presentation_from_display_set(
    template: PlotTemplate,
    document: ImportedWellDocument,
    display_set: AbstractSet[str],
) -> HostPresentation:
    """Compile HostPresentation from Display Set × template (dual-layer).

    Always includes a depth track. Empty Display Set yields depth-only
    presentation (no curve tracks) — empty canvas guidance is a UI concern.
    """
    leaves = leaves_from_document(document)
    styled = compose(leaves, display_set, template)

    bound_tracks: list[BoundTrack] = [
        BoundTrack(
            id="depth",
            role="depth",
            title="深度",
            width_fraction=0.12,
            scale=None,
            layers=[],
        )
    ]

    for desc in styled:
        curve = document.curve_by_mnemonic(desc.mnemonic)
        if curve is None:
            continue
        scale = desc.scale
        if scale is not None and not scale.unit and curve.unit:
            scale = ScaleSpec(
                mode=scale.mode,
                min=scale.min,
                max=scale.max,
                unit=curve.unit,
            )
        bound_tracks.append(
            BoundTrack(
                id=desc.template_slot_id or f"leaf-{desc.leaf_id}",
                role="curve",
                title=desc.title,
                width_fraction=desc.width_fraction,
                scale=scale,
                layers=[
                    BoundCurveLayer(
                        mnemonic=curve.mnemonic,
                        color=desc.color,
                        unit=curve.unit,
                        values=curve.values,
                        null_mask=curve.null_mask,
                    )
                ],
            )
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
