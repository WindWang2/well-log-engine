"""Directory workspace + workspace.json catalog (decision F / #213 / #217).

Engine Manifest is per-well data only — never the whole-project container.
"""

from __future__ import annotations

import json
import logging
import os
import tempfile
import threading
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Literal

from well_log_workstation.mnemonic_alias import normalize_alias_mapping

logger = logging.getLogger(__name__)

WORKSPACE_FILENAME = "workspace.json"
WELLS_DIRNAME = "wells"
PLOTS_DIRNAME = "plots"
TEMPLATES_DIRNAME = "templates"
SCHEMA_VERSION = 2

# Phase-2 T9 (#253): PlotType expanded from 2 to 6 classes for the four
# Phase-2 figure types (section / plane_map / fence_3d / composite).
PlotType = Literal[
    "single_well",
    "correlation",
    "section",
    "plane_map",
    "fence_3d",
    "composite",
]


@dataclass
class CoordinateReference:
    """Workstation-side CRS trio (same shape as the Workbench type, independent).

    Phase-2 T2 (#246): the Workstation holds its own ``CoordinateReference``
    instead of sharing ``paleo_workbench.project.models.CoordinateReference``
    (T10: ``project/models.py`` is NOT promoted). The default is WGS84 lng/lat
    (EPSG:4326) - the paleo_map Plate Carrée identity.
    """

    project_crs: str = "EPSG:4326"
    target_crs: str | None = None
    display_crs: str = "EPSG:4326"
    transform_history: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class WellCatalogEntry:
    id: str
    name: str
    # Relative to workspace root (posix-style preferred in JSON).
    path: str = ""
    # Phase-2 T2 (#246): optional wellhead coordinates + CRS (from LAS headers).
    lng: float | None = None
    lat: float | None = None
    crs: str | None = "EPSG:4326"
    # Wellhead elevations + total depth (FRS §1.x): KB (kelly-bushing /
    # 补心海拔), GL (ground level / 地面海拔), MaxMD. Captured from LAS
    # headers and editable via the well-header dialog; KB feeds the
    # tvdss section datum (shift = -kb).
    kb_m: float | None = None
    gl_m: float | None = None
    max_md: float | None = None


@dataclass
class PlotCatalogEntry:
    id: str
    name: str
    type: PlotType = "single_well"
    well_ids: list[str] = field(default_factory=list)
    template_id: str | None = None
    # Relative path under plots/ for future metadata file (#220).
    path: str = ""


@dataclass
class Workspace:
    """In-memory catalog bound to a filesystem root."""

    root: Path
    name: str
    wells: list[WellCatalogEntry] = field(default_factory=list)
    plots: list[PlotCatalogEntry] = field(default_factory=list)
    default_template_id: str | None = None
    schema_version: int = SCHEMA_VERSION
    # Phase-2 T2 (#246): project/target/display CRS trio held by the
    # Workstation. Defaults to WGS84 (paleo_map Plate Carrée identity).
    coordinate: CoordinateReference = field(default_factory=CoordinateReference)
    # Mnemonic alias dictionary (FRS §1.2 / P0-A): canonical → [aliases].
    # Lets a template slot ``GR`` match a curve named ``GRD`` etc. Optional;
    # empty dict means match by exact (case-insensitive) mnemonic only.
    mnemonic_alias: dict[str, list[str]] = field(default_factory=dict)
    # #35: serializes save_workspace calls on this instance — the GUI save
    # points and the background LAS-import worker share one Workspace.
    # Never persisted; excluded from repr/equality.
    _save_lock: threading.Lock = field(
        default_factory=threading.Lock, repr=False, compare=False
    )

    @property
    def wells_dir(self) -> Path:
        return self.root / WELLS_DIRNAME

    @property
    def plots_dir(self) -> Path:
        return self.root / PLOTS_DIRNAME

    @property
    def templates_dir(self) -> Path:
        return self.root / TEMPLATES_DIRNAME

    @property
    def catalog_path(self) -> Path:
        return self.root / WORKSPACE_FILENAME


class WorkspaceError(Exception):
    """User-facing workspace I/O or validation error."""


def _new_id() -> str:
    return str(uuid.uuid4())


def _to_json_dict(ws: Workspace) -> dict[str, Any]:
    return {
        "schemaVersion": ws.schema_version,
        "name": ws.name,
        "defaultTemplateId": ws.default_template_id,
        "coordinate": asdict(ws.coordinate),
        "mnemonic_alias": {
            str(k): [str(x) for x in v]
            for k, v in ws.mnemonic_alias.items()
            if k and v
        },
        "wells": [asdict(w) for w in ws.wells],
        "plots": [asdict(p) for p in ws.plots],
    }


# Valid plot types across all schema versions (v1 whitelist + v2 additions).
_ALL_PLOT_TYPES = (
    "single_well",
    "correlation",
    "section",
    "plane_map",
    "fence_3d",
    "composite",
)


def _coerce_plot_type(raw: Any) -> str:
    ptype = str(raw or "single_well")
    return ptype if ptype in _ALL_PLOT_TYPES else "single_well"


def _upgrade_v1_to_v2(data: dict[str, Any]) -> dict[str, Any]:
    """Additive v1 -> v2 migration: well coords + CRS defaults, coordinate trio.

    v1 workspaces serialize wells without ``lng``/``lat``/``crs`` and have no
    ``coordinate`` block. This migration fills the new fields with defaults
    (WGS84 lng/lat; unknown plot types fall back to ``single_well``) and
    returns a v2-shaped dict for the common parser.
    """
    out = dict(data)
    wells: list[dict[str, Any]] = []
    for w in data.get("wells") or []:
        wd = dict(w)
        wd.setdefault("lng", None)
        wd.setdefault("lat", None)
        wd.setdefault("crs", "EPSG:4326")
        wells.append(wd)
    out["wells"] = wells
    plots: list[dict[str, Any]] = []
    for p in data.get("plots") or []:
        pd = dict(p)
        pd["type"] = _coerce_plot_type(pd.get("type"))
        plots.append(pd)
    out["plots"] = plots
    out.setdefault("coordinate", asdict(CoordinateReference()))
    # P0-A (FRS §1.2): mnemonic_alias is new in v2; v1 files default to empty.
    out.setdefault("mnemonic_alias", {})
    return out


def _from_json_dict(root: Path, data: dict[str, Any]) -> Workspace:
    version = int(data.get("schemaVersion", 0))
    if version == 1:
        data = _upgrade_v1_to_v2(data)
        version = SCHEMA_VERSION
    if version != SCHEMA_VERSION:
        raise WorkspaceError(
            f"unsupported workspace schemaVersion={version} "
            f"(expected {SCHEMA_VERSION})"
        )
    coord_raw = data.get("coordinate") or {}
    coordinate = CoordinateReference(
        project_crs=str(coord_raw.get("project_crs") or "EPSG:4326"),
        target_crs=coord_raw.get("target_crs"),
        display_crs=str(coord_raw.get("display_crs") or "EPSG:4326"),
        transform_history=list(coord_raw.get("transform_history") or []),
    )
    wells = [
        WellCatalogEntry(
            id=str(w["id"]),
            name=str(w.get("name") or w["id"]),
            path=str(w.get("path") or ""),
            lng=_opt_float(w.get("lng")),
            lat=_opt_float(w.get("lat")),
            crs=str(w.get("crs") or "EPSG:4326"),
            kb_m=_opt_float(w.get("kb_m")),
            gl_m=_opt_float(w.get("gl_m")),
            max_md=_opt_float(w.get("max_md")),
        )
        for w in data.get("wells") or []
    ]
    plots: list[PlotCatalogEntry] = []
    for p in data.get("plots") or []:
        plots.append(
            PlotCatalogEntry(
                id=str(p["id"]),
                name=str(p.get("name") or p["id"]),
                type=_coerce_plot_type(p.get("type")),  # type: ignore[arg-type]
                well_ids=[str(x) for x in (p.get("well_ids") or [])],
                template_id=p.get("template_id"),
                path=str(p.get("path") or ""),
            )
        )
    return Workspace(
        root=root.resolve(),
        name=str(data.get("name") or root.name),
        wells=wells,
        plots=plots,
        default_template_id=data.get("defaultTemplateId"),
        schema_version=version,
        coordinate=coordinate,
        mnemonic_alias=normalize_alias_mapping(data.get("mnemonic_alias")),
    )


def _opt_float(raw: Any) -> float | None:
    """Coerce a JSON number to float, tolerating None / invalid values."""
    if raw is None:
        return None
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def create_workspace(path: Path | str, *, name: str | None = None) -> Workspace:
    """Create skeleton directories and an empty ``workspace.json``."""
    root = Path(path).expanduser().resolve()
    if root.exists() and any(root.iterdir()):
        # Allow empty dir; reject non-empty without catalog to avoid clobber.
        if (root / WORKSPACE_FILENAME).exists():
            raise WorkspaceError(f"workspace already exists: {root}")
        # non-empty without catalog
        raise WorkspaceError(f"directory is not empty: {root}")

    root.mkdir(parents=True, exist_ok=True)
    (root / WELLS_DIRNAME).mkdir(exist_ok=True)
    (root / PLOTS_DIRNAME).mkdir(exist_ok=True)
    (root / TEMPLATES_DIRNAME).mkdir(exist_ok=True)

    ws = Workspace(root=root, name=name or root.name)
    save_workspace(ws)
    return ws


def open_workspace(path: Path | str) -> Workspace:
    """Load catalog from an existing workspace directory."""
    root = Path(path).expanduser().resolve()
    catalog = root / WORKSPACE_FILENAME
    if not catalog.is_file():
        raise WorkspaceError(f"not a workspace (missing {WORKSPACE_FILENAME}): {root}")
    try:
        data = json.loads(catalog.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise WorkspaceError(f"failed to read catalog: {exc}") from exc
    if not isinstance(data, dict):
        raise WorkspaceError("workspace.json root must be an object")
    ws = _from_json_dict(root, data)
    # Ensure skeleton dirs exist (tolerant open).
    ws.wells_dir.mkdir(exist_ok=True)
    ws.plots_dir.mkdir(exist_ok=True)
    ws.templates_dir.mkdir(exist_ok=True)
    return ws


def open_or_create_workspace(path: Path | str, *, name: str | None = None) -> Workspace:
    """Open an existing catalog or create a new empty workspace at ``path``."""
    root = Path(path).expanduser().resolve()
    if (root / WORKSPACE_FILENAME).is_file():
        return open_workspace(root)
    return create_workspace(root, name=name)


def default_workspace_root() -> Path:
    """User-data path for the silent default session storage (no chooser UI)."""
    base = ""
    try:
        from PySide6.QtCore import QStandardPaths

        base = QStandardPaths.writableLocation(
            QStandardPaths.StandardLocation.AppDataLocation
        )
    except Exception:
        base = ""
    if not base:
        # Fallback when Qt is unavailable or AppDataLocation is empty.
        base = str(Path.home() / ".local" / "share" / "WellPlot Desktop")
    return Path(base) / "default-workspace"


def ensure_startup_workspace() -> Workspace:
    """Pick last valid recent workspace, else open/create the default session dir.

    Product cold-start goes straight to the main shell — no workspace chooser.
    Storage still uses a directory + workspace.json under the hood.
    """
    try:
        from well_log_workstation.recent_workspaces import load_recent

        for raw in load_recent():
            candidate = Path(raw).expanduser()
            try:
                if candidate.is_dir() and (candidate / WORKSPACE_FILENAME).is_file():
                    return open_workspace(candidate)
            except (WorkspaceError, OSError):
                continue
    except Exception:
        logger.warning("load_recent failed; falling back to default workspace", exc_info=True)
    return open_or_create_workspace(default_workspace_root(), name="默认")


def save_workspace(ws: Workspace) -> None:
    """Write ``workspace.json`` atomically (unique temp + os.replace).

    #35: the GUI save points and the background LAS-import worker save the
    same ``Workspace`` from two threads. A fixed ``workspace.json.tmp`` name
    let one thread replace (or unlink) the file the other was still writing
    into — FileNotFoundError storms, torn catalog reads, and false
    "import failed" reports after the well had already been appended.
    Same-instance saves are serialized by the instance lock, and every save
    writes its own uniquely named temp file so writers never collide on the
    temp path; a failed save leaves no temp file behind.
    """
    ws.root.mkdir(parents=True, exist_ok=True)
    path = ws.catalog_path
    payload = json.dumps(_to_json_dict(ws), indent=2, ensure_ascii=False) + "\n"
    with ws._save_lock:
        fd, name = tempfile.mkstemp(
            dir=str(ws.root), prefix=f"{WORKSPACE_FILENAME}.", suffix=".tmp"
        )
        tmp = Path(name)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write(payload)
            os.replace(tmp, path)
        except BaseException:
            # Never leave an orphaned temp file on any failure path; a
            # failing cleanup must not mask the original error.
            try:
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
            raise


def add_well(
    ws: Workspace,
    *,
    name: str,
    path: str = "",
    well_id: str | None = None,
    lng: float | None = None,
    lat: float | None = None,
    crs: str | None = "EPSG:4326",
    kb_m: float | None = None,
    gl_m: float | None = None,
    max_md: float | None = None,
) -> WellCatalogEntry:
    """Append a well catalog entry and persist."""
    entry = WellCatalogEntry(
        id=well_id or _new_id(),
        name=name,
        path=path,
        lng=lng,
        lat=lat,
        crs=crs,
        kb_m=kb_m,
        gl_m=gl_m,
        max_md=max_md,
    )
    ws.wells.append(entry)
    save_workspace(ws)
    return entry


def add_plot(
    ws: Workspace,
    *,
    name: str,
    plot_type: PlotType = "single_well",
    well_ids: list[str] | None = None,
    template_id: str | None = None,
    path: str = "",
    plot_id: str | None = None,
) -> PlotCatalogEntry:
    """Append a plot catalog entry and persist."""
    entry = PlotCatalogEntry(
        id=plot_id or _new_id(),
        name=name,
        type=plot_type,
        well_ids=list(well_ids or []),
        template_id=template_id,
        path=path,
    )
    ws.plots.append(entry)
    save_workspace(ws)
    return entry
