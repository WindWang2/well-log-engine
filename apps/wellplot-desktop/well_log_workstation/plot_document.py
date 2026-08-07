"""Single-well / correlation plot documents under ``plots/`` (#220).

Host metadata only; multi-track layout still comes from template apply (H).
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from well_log_workstation.correlation_links import HorizonLink
from well_log_workstation.workspace import (
    PlotCatalogEntry,
    PlotType,
    Workspace,
    WorkspaceError,
    add_plot,
    save_workspace,
)

# v6: display_set (leaf ids on plot; samples stay on well — model A)
# v7: data_bindings — each leaf-on-plot has binding_id + plot/well identity
# v8: track_order — persisted single-well track id order (canvas drag reorder)
# v9: surfaces — section erosion/onlap surfaces (FRS §3.x 尖灭行 P1)
# v10: lenses — section freehand lens bodies (FRS §3.x 透镜体手绘)
PLOT_SCHEMA_VERSION = 10


@dataclass
class PlotDataBinding:
    """One well-track linked into a plot (identity only; no samples).

    ``binding_id`` uniquely identifies this plot↔data association so imports
    and multi-plot sharing can refer to the link without embedding curves.
    """

    binding_id: str
    plot_id: str
    well_id: str
    leaf_id: str
    mnemonic: str = ""
    source_id: str = ""

    def to_json(self) -> dict[str, Any]:
        return {
            "binding_id": self.binding_id,
            "plot_id": self.plot_id,
            "well_id": self.well_id,
            "leaf_id": self.leaf_id,
            "mnemonic": self.mnemonic,
            "source_id": self.source_id,
        }

    @staticmethod
    def from_json(raw: dict[str, Any], *, default_plot_id: str = "") -> PlotDataBinding | None:
        leaf_id = str(raw.get("leaf_id") or "").strip()
        well_id = str(raw.get("well_id") or "").strip()
        if not leaf_id or not well_id:
            return None
        bid = str(raw.get("binding_id") or "").strip() or str(uuid.uuid4())
        return PlotDataBinding(
            binding_id=bid,
            plot_id=str(raw.get("plot_id") or default_plot_id),
            well_id=well_id,
            leaf_id=leaf_id,
            mnemonic=str(raw.get("mnemonic") or ""),
            source_id=str(raw.get("source_id") or ""),
        )


@dataclass
class PanelRef:
    """Composite-figure panel reference (Phase-2 T9 / #253, T7 / #251).

    A composite figure (油藏综合图) lays out several sub-plots in panels.
    T7 adds the paper-side placement + render mode:

    - ``source_plot_type``: the source plot's PlotType (drives live vs
      snapshot - GL/engine plots must snapshot).
    - ``rect_mm``: panel rect on the paper in mm (``[x, y, w, h]``); None
      means the layout window picks a default position.
    - ``render_mode``: ``"live"`` (QGraphicsProxyWidget, non-GL) or
      ``"snapshot"`` (QPixmap via source.grab(), GL/engine plots).
    """

    plot_id: str
    slot: str = "main"
    source_plot_type: str = "single_well"
    rect_mm: list[float] | None = None  # [x, y, w, h] in mm
    render_mode: str = "live"


@dataclass
class PlotDocument:
    id: str
    name: str
    type: PlotType
    well_ids: list[str]
    template_id: str | None
    # Relative path from workspace root
    path: str
    # Correlation horizon links (#229); empty for single-well
    links: list[HorizonLink] = field(default_factory=list)
    # Composite-figure panels (Phase-2 T9); only ``composite`` plots use it.
    panels: list[PanelRef] = field(default_factory=list)
    # Per-plot revision, persisted in plots/<id>.json (schema v3, ADR 0051).
    revision: int = 0
    # Composite free graphics (schema v4): raw shape dicts placed on the
    # paper directly, independent of panels/templates. Empty for all other
    # plot types.
    free_graphics: list[dict] = field(default_factory=list)
    # Single-well track property overrides (schema v5 / #292): track_id → props.
    # Keys: visible, title, width_fraction, scale_min, scale_max, scale_mode.
    track_overrides: dict[str, dict[str, Any]] = field(default_factory=dict)
    # Single-well Display Set (schema v6): leaf ids from the bound well's
    # content tree (import source → tracks). Empty = use template defaults
    # on first open. Does not store curve samples — data lives under wells/.
    display_set: list[str] = field(default_factory=list)
    # Single-well track order (schema v8, FRS §2.x): track ids in presentation
    # order after canvas drag reorder. Empty = template default order.
    track_order: list[str] = field(default_factory=list)
    # Explicit plot↔data links (schema v7). Each entry has binding_id so
    # "imported into this plot" is identifiable; samples remain on the well.
    data_bindings: list[PlotDataBinding] = field(default_factory=list)
    # Correlation column gap in pixels (#295 / T7). Optional field; default 6.
    # Well order is the existing ``well_ids`` list order.
    column_gap_px: int = 6
    # Correlation display datum (#296 / T8): md | tvdss | horizon.
    datum_mode: str = "md"
    datum_horizon: str | None = None
    # Inter-well fill MVP (#297 / T9)
    show_interwell_fill: bool = False
    interwell_fill_color: str = "#93c5fd"
    # Pinchout wedges for unilateral intervals (FRS §3.3). Optional correlation
    # fields; default off. ``pinchout_factor`` is the apex x fraction across the
    # inter-column gap (0.05–1.0).
    pinchout_mode: str = "off"
    pinchout_factor: float = 0.5
    pinchout_smooth: bool = False
    # Lithology pattern map (FRS §2.3 / P0-B): top/horizon name → SY/T 5615
    # pattern id. Optional; empty means solid-color fills only.
    litho_pattern_map: dict[str, str] = field(default_factory=dict)
    # Section faults (FRS §3.3 / P1-A): list of serialized SectionFault2D
    # dicts (position+throw model). Optional; empty for fault-free sections.
    faults: list[dict] = field(default_factory=list)
    # Section fluid contacts (FRS §3.3 / P1-B): list of serialized
    # FluidContact2D dicts (OWC/GOC per-well depth). Optional; empty = none.
    contacts: list[dict] = field(default_factory=list)
    # Section erosion/onlap surfaces (FRS §3.x / P1): list of serialized
    # ErosionSurface2D dicts (per-well depth + mode). Optional; empty = none.
    surfaces: list[dict] = field(default_factory=list)
    # Freehand sand-lens polygons (FRS §3.x 透镜体手绘): LensBody2D dicts.
    lenses: list[dict] = field(default_factory=list)
    # Section well spacing (FRS §3.1 / P1-C): "equal" = fixed column interval,
    # "geographic" = wells placed by survey closure projected on the section
    # azimuth (+ curved trajectory polylines). Optional; default equal.
    well_spacing: str = "equal"
    # Correlation real well distance (FRS §3.x 实际井距): "equal" = fixed
    # column stride, "real" = columns spread to wellhead surface distance.
    # Distinct from section ``well_spacing`` (which uses survey closure).
    # Optional; default equal.
    correlation_spacing: str = "equal"
    # Vertical exaggeration (FRS §3.x 纵横比例尺解耦): depth-axis display
    # stretch factor, 1.0 = unchanged. Optional; default 1.0.
    vertical_exaggeration: float = 1.0
    # Publication ornaments (FRS §5 / P2-C): title block / legend / location
    # map overlay on the section canvas and exports. Default off.
    ornaments: bool = False

    def absolute_path(self, workspace: Workspace) -> Path:
        return workspace.root / self.path


def _plot_rel_path(plot_id: str) -> str:
    return f"plots/{plot_id}.json"


def _to_json(doc: PlotDocument) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "schemaVersion": PLOT_SCHEMA_VERSION,
        "id": doc.id,
        "name": doc.name,
        "type": doc.type,
        "well_ids": list(doc.well_ids),
        "template_id": doc.template_id,
    }
    # Always persist links for correlation docs so clear/remove is durable (#230)
    if doc.type == "correlation" or doc.links:
        payload["links"] = [lk.to_json() for lk in doc.links]
    if doc.type == "composite" or doc.panels:
        payload["panels"] = [
            {
                "plot_id": p.plot_id,
                "slot": p.slot,
                "source_plot_type": p.source_plot_type,
                "rect_mm": p.rect_mm,
                "render_mode": p.render_mode,
            }
            for p in doc.panels
        ]
    # Always persist the revision (ADR 0051); unlike links/panels it is
    # unconditional so a stale file can never look unversioned.
    payload["revision"] = int(doc.revision)
    # Free graphics (schema v4): persist for composite docs, and also for any
    # doc that carries them (mirrors links/panels so stale values never drop).
    if doc.type == "composite" or doc.free_graphics:
        payload["free_graphics"] = list(doc.free_graphics)
    if doc.type == "single_well" or doc.track_overrides:
        payload["track_overrides"] = {
            str(k): dict(v) for k, v in doc.track_overrides.items() if isinstance(v, dict)
        }
    if doc.type == "single_well" or doc.track_order:
        payload["track_order"] = [str(x) for x in doc.track_order]
    if doc.type == "single_well" or doc.display_set:
        payload["display_set"] = [str(x) for x in doc.display_set]
    if doc.type == "single_well" or doc.data_bindings:
        payload["data_bindings"] = [b.to_json() for b in doc.data_bindings]
    # Correlation layout (T7/T8): write when correlation so re-open is stable.
    if doc.type == "correlation" or doc.column_gap_px != 6:
        payload["column_gap_px"] = int(doc.column_gap_px)
    if doc.type == "correlation" or doc.datum_mode != "md" or doc.datum_horizon:
        payload["datum_mode"] = str(doc.datum_mode or "md")
        if doc.datum_horizon:
            payload["datum_horizon"] = str(doc.datum_horizon)
    if doc.type == "correlation" or doc.show_interwell_fill:
        payload["show_interwell_fill"] = bool(doc.show_interwell_fill)
        payload["interwell_fill_color"] = str(
            doc.interwell_fill_color or "#93c5fd"
        )
    # Pinchout wedges (FRS §3.3): optional correlation fields, written together
    # so a stale file never carries a half-set of pinchout options.
    if doc.type == "correlation" or doc.pinchout_mode != "off":
        payload["pinchout_mode"] = str(doc.pinchout_mode or "off")
        payload["pinchout_factor"] = float(doc.pinchout_factor)
        payload["pinchout_smooth"] = bool(doc.pinchout_smooth)
    if doc.type == "correlation" or doc.litho_pattern_map:
        payload["litho_pattern_map"] = {
            str(k): str(v) for k, v in doc.litho_pattern_map.items() if k and v
        }
    if doc.type == "section" or doc.faults:
        payload["faults"] = [dict(f) for f in doc.faults if isinstance(f, dict)]
    if doc.type == "section" or doc.contacts:
        payload["contacts"] = [dict(c) for c in doc.contacts if isinstance(c, dict)]
    if doc.type == "section" or doc.surfaces:
        payload["surfaces"] = [dict(s) for s in doc.surfaces if isinstance(s, dict)]
    if doc.type == "section" or doc.lenses:
        payload["lenses"] = [dict(s) for s in doc.lenses if isinstance(s, dict)]
    if doc.type == "section" or doc.well_spacing != "equal":
        payload["well_spacing"] = str(doc.well_spacing)
    if doc.type == "section" or doc.ornaments:
        payload["ornaments"] = bool(doc.ornaments)
    # Correlation real-distance + vertical exaggeration (FRS §3.x). Written
    # together so stale files never carry a half-set; only emitted for
    # correlation plots or when non-default.
    if doc.type == "correlation" or doc.correlation_spacing != "equal":
        payload["correlation_spacing"] = str(doc.correlation_spacing)
    if doc.type == "correlation" or abs(doc.vertical_exaggeration - 1.0) > 1e-9:
        payload["vertical_exaggeration"] = float(doc.vertical_exaggeration)
    return payload


def _from_json(data: dict[str, Any], *, path: str) -> PlotDocument:
    version = int(data.get("schemaVersion", 0))
    if version == 1:
        # v1 -> v2 additive: panels is new and defaults to empty.
        data = dict(data)
        data.setdefault("panels", [])
        version = 2
    if version == 2:
        # v2 -> v3 additive: revision is new and defaults to 0 (ADR 0051).
        data = dict(data)
        data.setdefault("revision", 0)
        version = 3
    if version == 3:
        # v3 -> v4 additive: free_graphics is new and defaults to empty.
        data = dict(data)
        data.setdefault("free_graphics", [])
        version = 4
    if version == 4:
        # v4 -> v5 additive: track_overrides for single-well layout edits (#292).
        data = dict(data)
        data.setdefault("track_overrides", {})
        version = 5
    if version == 5:
        # v5 -> v6 additive: plot-level display_set (leaf ids; data stays on well).
        data = dict(data)
        data.setdefault("display_set", [])
        version = 6
    if version == 6:
        # v6 -> v7 additive: data_bindings with per-link binding_id.
        data = dict(data)
        data.setdefault("data_bindings", [])
        version = 7
    if version == 7:
        # v7 -> v8 additive: track_order for canvas drag reorder (FRS §2.x).
        data = dict(data)
        data.setdefault("track_order", [])
        version = 8
    if version == 8:
        # v8 -> v9 additive: section erosion/onlap surfaces (FRS §3.x P1).
        data = dict(data)
        data.setdefault("surfaces", [])
        version = 9
    if version == 9:
        # v9 -> v10 additive: freehand section lenses (FRS §3.x 透镜体手绘).
        data = dict(data)
        data.setdefault("lenses", [])
        version = PLOT_SCHEMA_VERSION
    if version != PLOT_SCHEMA_VERSION:
        raise WorkspaceError(
            f"unsupported plot schemaVersion={version} "
            f"(expected {PLOT_SCHEMA_VERSION})"
        )
    ptype = str(data.get("type") or "single_well")
    if ptype not in ("single_well", "correlation", "section", "plane_map", "fence_3d", "composite"):
        ptype = "single_well"
    links: list[HorizonLink] = []
    for raw in data.get("links") or []:
        if isinstance(raw, dict):
            link = HorizonLink.from_json(raw)
            if link is not None:
                links.append(link)
    panels: list[PanelRef] = []
    for raw in data.get("panels") or []:
        if isinstance(raw, dict):
            rect = raw.get("rect_mm")
            panels.append(
                PanelRef(
                    plot_id=str(raw.get("plot_id") or ""),
                    slot=str(raw.get("slot") or "main"),
                    source_plot_type=str(raw.get("source_plot_type") or "single_well"),
                    rect_mm=(
                        [float(v) for v in rect]
                        if isinstance(rect, (list, tuple)) and len(rect) == 4
                        else None
                    ),
                    render_mode=str(raw.get("render_mode") or "live"),
                )
            )
    free_graphics: list[dict] = []
    for raw in data.get("free_graphics") or []:
        if isinstance(raw, dict):
            free_graphics.append(raw)
    track_overrides: dict[str, dict[str, Any]] = {}
    raw_ov = data.get("track_overrides") or {}
    if isinstance(raw_ov, dict):
        for tid, props in raw_ov.items():
            if isinstance(props, dict):
                track_overrides[str(tid)] = dict(props)
    track_order: list[str] = []
    for raw_id in data.get("track_order") or []:
        s = str(raw_id).strip()
        if s:
            track_order.append(s)
    display_set: list[str] = []
    for raw_leaf in data.get("display_set") or []:
        s = str(raw_leaf).strip()
        if s:
            display_set.append(s)
    plot_id_hint = str(data.get("id") or "")
    data_bindings: list[PlotDataBinding] = []
    for raw_b in data.get("data_bindings") or []:
        if isinstance(raw_b, dict):
            b = PlotDataBinding.from_json(raw_b, default_plot_id=plot_id_hint)
            if b is not None:
                data_bindings.append(b)
    # If only display_set present (v6 files), synthesize bindings without ids
    if not data_bindings and display_set:
        well_hint = ""
        wids = data.get("well_ids") or []
        if wids:
            well_hint = str(wids[0])
        for lid in display_set:
            data_bindings.append(
                PlotDataBinding(
                    binding_id=str(uuid.uuid4()),
                    plot_id=plot_id_hint,
                    well_id=well_hint,
                    leaf_id=lid,
                )
            )
    try:
        column_gap_px = int(data.get("column_gap_px", 6))
    except (TypeError, ValueError):
        column_gap_px = 6
    column_gap_px = max(0, min(200, column_gap_px))
    datum_mode = str(data.get("datum_mode") or "md")
    if datum_mode not in ("md", "tvdss", "horizon"):
        datum_mode = "md"
    raw_h = data.get("datum_horizon")
    datum_horizon = str(raw_h).strip() if raw_h else None
    if datum_horizon == "":
        datum_horizon = None
    show_interwell_fill = bool(data.get("show_interwell_fill", False))
    interwell_fill_color = str(data.get("interwell_fill_color") or "#93c5fd")
    pinchout_mode = str(data.get("pinchout_mode") or "off")
    if pinchout_mode not in ("off", "linear"):
        pinchout_mode = "off"
    try:
        pinchout_factor = float(data.get("pinchout_factor", 0.5))
    except (TypeError, ValueError):
        pinchout_factor = 0.5
    pinchout_factor = max(0.05, min(1.0, pinchout_factor))
    pinchout_smooth = bool(data.get("pinchout_smooth", False))
    litho_pattern_map: dict[str, str] = {}
    raw_lpm = data.get("litho_pattern_map") or {}
    if isinstance(raw_lpm, dict):
        for k, v in raw_lpm.items():
            ks, vs = str(k).strip(), str(v).strip()
            if ks and vs:
                litho_pattern_map[ks] = vs
    raw_faults = data.get("faults")
    faults: list[dict] = []
    if isinstance(raw_faults, list):
        for f in raw_faults:
            if isinstance(f, dict):
                faults.append(dict(f))
    raw_contacts = data.get("contacts")
    contacts: list[dict] = []
    if isinstance(raw_contacts, list):
        for c in raw_contacts:
            if isinstance(c, dict):
                contacts.append(dict(c))
    raw_surfaces = data.get("surfaces")
    surfaces: list[dict] = []
    if isinstance(raw_surfaces, list):
        for s in raw_surfaces:
            if isinstance(s, dict):
                surfaces.append(dict(s))
    raw_lenses = data.get("lenses")
    lenses: list[dict] = []
    if isinstance(raw_lenses, list):
        for s in raw_lenses:
            if isinstance(s, dict):
                lenses.append(dict(s))
    well_spacing = str(data.get("well_spacing") or "equal")
    if well_spacing not in ("equal", "geographic"):
        well_spacing = "equal"
    ornaments = bool(data.get("ornaments", False))
    correlation_spacing = str(data.get("correlation_spacing") or "equal")
    if correlation_spacing not in ("equal", "real"):
        correlation_spacing = "equal"
    try:
        vertical_exaggeration = float(data.get("vertical_exaggeration", 1.0))
    except (TypeError, ValueError):
        vertical_exaggeration = 1.0
    vertical_exaggeration = max(0.1, min(20.0, vertical_exaggeration))
    return PlotDocument(
        id=str(data["id"]),
        name=str(data.get("name") or data["id"]),
        type=ptype,  # type: ignore[arg-type]
        well_ids=[str(x) for x in (data.get("well_ids") or [])],
        template_id=data.get("template_id"),
        path=path,
        links=links,
        panels=panels,
        free_graphics=free_graphics,
        revision=max(0, int(data.get("revision") or 0)),
        track_overrides=track_overrides,
        track_order=track_order,
        display_set=display_set,
        data_bindings=data_bindings,
        column_gap_px=column_gap_px,
        datum_mode=datum_mode,
        datum_horizon=datum_horizon,
        show_interwell_fill=show_interwell_fill,
        interwell_fill_color=interwell_fill_color,
        pinchout_mode=pinchout_mode,
        pinchout_factor=pinchout_factor,
        pinchout_smooth=pinchout_smooth,
        litho_pattern_map=litho_pattern_map,
        faults=faults,
        contacts=contacts,
        surfaces=surfaces,
        lenses=lenses,
        well_spacing=well_spacing,
        ornaments=ornaments,
        correlation_spacing=correlation_spacing,
        vertical_exaggeration=vertical_exaggeration,
    )


def save_plot_document(workspace: Workspace, doc: PlotDocument) -> None:
    """Write plots/<id>.json and ensure catalog entry matches."""
    workspace.plots_dir.mkdir(parents=True, exist_ok=True)
    rel = doc.path or _plot_rel_path(doc.id)
    doc.path = rel
    abs_path = workspace.root / rel
    abs_path.parent.mkdir(parents=True, exist_ok=True)
    # Lazy import: keep this schema/persistence module importable without
    # PySide6 (events.py imports QtCore) for /usr/bin/python3 verification.
    from well_log_workstation.events import bump_plot_revision

    # Saving commits a new revision; bump (no emit) before writing the file.
    # Shared by both branches below (file write + catalog upsert).
    doc.revision = bump_plot_revision(doc.id)
    tmp = abs_path.with_suffix(".json.tmp")
    payload = json.dumps(_to_json(doc), indent=2, ensure_ascii=False) + "\n"
    tmp.write_text(payload, encoding="utf-8")
    tmp.replace(abs_path)

    # Upsert catalog
    existing = next((p for p in workspace.plots if p.id == doc.id), None)
    if existing is None:
        add_plot(
            workspace,
            name=doc.name,
            plot_type=doc.type,
            well_ids=doc.well_ids,
            template_id=doc.template_id,
            path=rel,
            plot_id=doc.id,
        )
    else:
        existing.name = doc.name
        existing.type = doc.type
        existing.well_ids = list(doc.well_ids)
        existing.template_id = doc.template_id
        existing.path = rel
        save_workspace(workspace)


def load_plot_document(workspace: Workspace, plot_id: str) -> PlotDocument:
    """Load plot metadata from disk (catalog path or default plots/<id>.json)."""
    entry = next((p for p in workspace.plots if p.id == plot_id), None)
    rel = entry.path if entry and entry.path else _plot_rel_path(plot_id)
    abs_path = workspace.root / rel
    if not abs_path.is_file():
        raise WorkspaceError(f"图件文件不存在: {rel}")
    try:
        data = json.loads(abs_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise WorkspaceError(f"无法读取图件: {exc}") from exc
    if not isinstance(data, dict):
        raise WorkspaceError("图件 JSON 根必须是对象")
    doc = _from_json(data, path=rel)
    if doc.id != plot_id and entry is not None:
        # Prefer catalog id if file was renamed oddly
        doc = PlotDocument(
            id=plot_id,
            name=doc.name,
            type=doc.type,
            well_ids=doc.well_ids,
            template_id=doc.template_id,
            path=rel,
            links=list(doc.links),
            panels=list(doc.panels),
            free_graphics=list(doc.free_graphics),
            revision=doc.revision,
            track_overrides=dict(doc.track_overrides),
            display_set=list(doc.display_set),
            data_bindings=list(doc.data_bindings),
            column_gap_px=doc.column_gap_px,
            datum_mode=doc.datum_mode,
            datum_horizon=doc.datum_horizon,
            show_interwell_fill=doc.show_interwell_fill,
            interwell_fill_color=doc.interwell_fill_color,
            pinchout_mode=doc.pinchout_mode,
            pinchout_factor=doc.pinchout_factor,
            pinchout_smooth=doc.pinchout_smooth,
            litho_pattern_map=dict(doc.litho_pattern_map),
            faults=list(doc.faults),
            contacts=list(doc.contacts),
            surfaces=list(doc.surfaces),
            lenses=list(doc.lenses),
            well_spacing=doc.well_spacing,
            ornaments=doc.ornaments,
            correlation_spacing=doc.correlation_spacing,
            vertical_exaggeration=doc.vertical_exaggeration,
        )
    # Lazy import: keep this module importable without PySide6 (see save).
    from well_log_workstation.events import restore_plot_revision

    # Seed the in-memory counter from the persisted value; both return paths
    # above (with/without id-mismatch rebuild) share this single restore.
    restore_plot_revision(doc.id, doc.revision)
    return doc


def sync_data_bindings(
    doc: PlotDocument,
    *,
    well_id: str,
    leaf_ids: list[str] | tuple[str, ...] | set[str],
    leaf_meta: dict[str, dict[str, str]] | None = None,
) -> None:
    """Align ``display_set`` + ``data_bindings``; preserve binding_id when possible.

    ``leaf_meta[leaf_id]`` may include mnemonic / source_id.
    """
    meta = leaf_meta or {}
    existing = {b.leaf_id: b for b in doc.data_bindings}
    ordered = [str(x) for x in leaf_ids]
    doc.display_set = list(ordered)
    new_bindings: list[PlotDataBinding] = []
    for lid in ordered:
        m = meta.get(lid) or {}
        if lid in existing:
            b = existing[lid]
            new_bindings.append(
                PlotDataBinding(
                    binding_id=b.binding_id,
                    plot_id=doc.id,
                    well_id=well_id or b.well_id,
                    leaf_id=lid,
                    mnemonic=str(m.get("mnemonic") or b.mnemonic),
                    source_id=str(m.get("source_id") or b.source_id),
                )
            )
        else:
            new_bindings.append(
                PlotDataBinding(
                    binding_id=str(uuid.uuid4()),
                    plot_id=doc.id,
                    well_id=well_id,
                    leaf_id=lid,
                    mnemonic=str(m.get("mnemonic") or ""),
                    source_id=str(m.get("source_id") or ""),
                )
            )
    doc.data_bindings = new_bindings


def create_single_well_plot(
    workspace: Workspace,
    *,
    well_id: str,
    well_name: str,
    template_id: str,
    name: str | None = None,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create and persist a 单井分析图 document (multi-track template binding)."""
    if not any(w.id == well_id for w in workspace.wells):
        raise WorkspaceError("井不在工区目录中")
    pid = plot_id or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name or f"{well_name} 单井分析图",
        type="single_well",
        well_ids=[well_id],
        template_id=template_id,
        path=_plot_rel_path(pid),
    )
    save_plot_document(workspace, doc)
    return doc


def create_correlation_plot(
    workspace: Workspace,
    *,
    well_ids: list[str],
    template_id: str,
    name: str | None = None,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create and persist a 地层对比图-lite document (≥2 wells)."""
    if len(well_ids) < 2:
        raise WorkspaceError("地层对比至少需要 2 口井")
    catalog_ids = {w.id for w in workspace.wells}
    for wid in well_ids:
        if wid not in catalog_ids:
            raise WorkspaceError(f"井不在工区目录中: {wid}")
    names = []
    for wid in well_ids:
        entry = next(w for w in workspace.wells if w.id == wid)
        names.append(entry.name)
    pid = plot_id or str(uuid.uuid4())
    label = name or f"{'–'.join(names[:3])} 地层对比"
    doc = PlotDocument(
        id=pid,
        name=label,
        type="correlation",
        well_ids=list(well_ids),
        template_id=template_id,
        path=_plot_rel_path(pid),
    )
    save_plot_document(workspace, doc)
    return doc


def _validate_well_ids(workspace: Workspace, well_ids: list[str], *, min_count: int) -> list[str]:
    """Shared well-id validation for the Phase-2 T9 create_* helpers."""
    if len(well_ids) < min_count:
        raise WorkspaceError(f"至少需要 {min_count} 口井")
    catalog_ids = {w.id for w in workspace.wells}
    for wid in well_ids:
        if wid not in catalog_ids:
            raise WorkspaceError(f"井不在工区目录中: {wid}")
    return list(well_ids)


def create_section_plot(
    workspace: Workspace,
    *,
    well_ids: list[str],
    template_id: str,
    name: str | None = None,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create and persist a 油藏剖面图 document (≥2 wells; Phase-2 T9 / #253)."""
    ids = _validate_well_ids(workspace, well_ids, min_count=2)
    pid = plot_id or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name or f"油藏剖面（{len(ids)} 井）",
        type="section",
        well_ids=ids,
        template_id=template_id,
        path=_plot_rel_path(pid),
    )
    save_plot_document(workspace, doc)
    return doc


def create_plane_map_plot(
    workspace: Workspace,
    *,
    wells: list[str] | None = None,
    template_id: str,
    name: str | None = None,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create and persist a 平面图 document (requires a project CRS; Phase-2 T9).

    ``workspace.coordinate`` must be set (defaults to WGS84) so the map has a
    CRS context for the paleo_map Plate Carrée identity.
    """
    if not workspace.coordinate:
        raise WorkspaceError("平面图需要先设置工区坐标系（workspace.coordinate）")
    ids = _validate_well_ids(workspace, list(wells or []), min_count=1) if wells else []
    pid = plot_id or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name or "平面图",
        type="plane_map",
        well_ids=ids,
        template_id=template_id,
        path=_plot_rel_path(pid),
    )
    save_plot_document(workspace, doc)
    return doc


def create_fence_3d_plot(
    workspace: Workspace,
    *,
    well_ids: list[str],
    template_id: str,
    name: str | None = None,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create and persist a 3D fence plot document (≥2 wells; Phase-2 T9)."""
    ids = _validate_well_ids(workspace, well_ids, min_count=2)
    pid = plot_id or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name or f"三维栅状图（{len(ids)} 井）",
        type="fence_3d",
        well_ids=ids,
        template_id=template_id,
        path=_plot_rel_path(pid),
    )
    save_plot_document(workspace, doc)
    return doc


def create_composite_plot(
    workspace: Workspace,
    *,
    panels: list[PanelRef] | None = None,
    template_id: str,
    name: str | None = None,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create and persist a 油藏综合图 document (≥1 PanelRef; Phase-2 T9)."""
    panel_list = list(panels or [])
    if not panel_list:
        raise WorkspaceError("综合图至少需要 1 个面板（PanelRef）")
    pid = plot_id or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name or "油藏综合图",
        type="composite",
        well_ids=[],
        template_id=template_id,
        path=_plot_rel_path(pid),
        panels=panel_list,
    )
    save_plot_document(workspace, doc)
    return doc


def find_plot_entry(workspace: Workspace, plot_id: str) -> PlotCatalogEntry | None:
    return next((p for p in workspace.plots if p.id == plot_id), None)
