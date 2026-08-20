"""Optional WellLogEngine (welllog) bridge for the workstation host (#224/#225).

Host MultiTrackCanvas remains the default multi-track path. When the ``welllog``
Shiboken package is importable, the shell can embed ``WellLogView`` and submit:

- multi-track presentation via ``submit_multi_track`` (#225)
- multi-well shared-depth section via ``submit_multi_well_section`` (#225)
- single-curve fallback via ``submit_curve`` (#224)

Missing engine never crashes the host.
"""

from __future__ import annotations

import os
import uuid
from dataclasses import dataclass
from typing import Any

import numpy as np

from well_log_workstation.correlation_links import (
    HorizonLink,
    links_to_engine_overlays,
)
from well_log_workstation.las_import import ImportedWellDocument
from well_log_workstation.template_model import HostPresentation
from well_log_workstation.tops_model import FormationTop


class EngineUnavailable(Exception):
    """welllog package / WellLogView not available."""


class EngineSubmitError(Exception):
    """submit_curve or related engine call failed."""


@dataclass(frozen=True)
class EngineCapability:
    available: bool
    detail: str
    well_log_view_cls: type | None = None
    # How the capability was reached:
    #   "native"    — the welllog package imported normally (WellLogView).
    #   "extension" — package __init__ failed; the raw _QtWidgets extension
    #                 was loaded directly (still native code, degraded init).
    #   None        — unavailable.
    mode: str | None = None


_cached: EngineCapability | None = None


def reset_engine_capability_cache() -> None:
    global _cached
    _cached = None


def _strict_native() -> bool:
    """True when the bridge must prove the *package* path (native binding).

    WLWS_REQUIRE_NATIVE_BINDING=1 gates test/CI runs so a broken package
    init or a degraded raw-extension load cannot masquerade as the native
    binding. End-user default (unset) keeps the existing graceful degrade.
    """
    return os.environ.get("WLWS_REQUIRE_NATIVE_BINDING", "").strip() in (
        "1",
        "true",
        "yes",
    )


def probe_engine() -> EngineCapability:
    """Detect welllog without raising. Result is cached until reset."""
    global _cached
    if _cached is not None:
        return _cached

    # Allow forced disable for tests / CI
    if os.environ.get("WLWS_DISABLE_ENGINE", "").strip() in ("1", "true", "yes"):
        _cached = EngineCapability(False, "WLWS_DISABLE_ENGINE set")
        return _cached

    try:
        from welllog import WellLogView  # type: ignore

        _cached = EngineCapability(
            True, "welllog.WellLogView", WellLogView, mode="native"
        )
        return _cached
    except Exception as exc_pkg:  # noqa: BLE001
        pkg_err = str(exc_pkg)

    # Strict mode (E3): the package path is the only acceptable proof of the
    # native binding. A broken __init__ or missing wrapper must surface as a
    # failure instead of being papered over by the raw-extension fallback.
    if _strict_native():
        _cached = EngineCapability(
            False,
            f"WLWS_REQUIRE_NATIVE_BINDING set; welllog package import failed: "
            f"{pkg_err}",
        )
        return _cached

    # Fallback: load extension without package __init__ (e.g. missing TableModel)
    try:
        import importlib
        import importlib.util

        # Prefer already-loaded module, else import welllog._QtWidgets carefully
        try:
            ext = importlib.import_module("welllog._QtWidgets")
        except Exception:
            # Last resort: find shared library on sys.path
            import sys
            from pathlib import Path

            ext = None
            for entry in sys.path:
                for name in ("_QtWidgets.abi3.so", "_QtWidgets.so",
                             "_QtWidgets.abi3.pyd", "_QtWidgets.pyd"):
                    candidate = Path(entry) / "welllog" / name
                    if candidate.is_file():
                        spec = importlib.util.spec_from_file_location(
                            "welllog._QtWidgets", candidate
                        )
                        if spec and spec.loader:
                            ext = importlib.util.module_from_spec(spec)
                            spec.loader.exec_module(ext)
                            break
                if ext is not None:
                    break
            if ext is None:
                raise ImportError(pkg_err)

        view_cls = getattr(getattr(ext, "welllog", None), "WellLogView", None)
        if view_cls is None:
            _cached = EngineCapability(
                False, f"welllog._QtWidgets has no WellLogView ({pkg_err})"
            )
            return _cached
        _cached = EngineCapability(
            True,
            f"welllog._QtWidgets.WellLogView ({pkg_err})",
            view_cls,
            mode="extension",
        )
        return _cached
    except Exception as exc_ext:  # noqa: BLE001
        _cached = EngineCapability(
            False, f"welllog unavailable: {pkg_err}; {exc_ext}"
        )
        return _cached


def engine_available() -> bool:
    return probe_engine().available


@dataclass(frozen=True)
class MappingCapability:
    """Capability probe for the geoviz mapping surface (Phase-2 T2 / #246).

    ``probe_mapping()`` checks that the ``geoviz`` facade exposes the
    Phase-2 mapping helpers (CRS coercion + FilledContourLayer) the
    Workstation draws with at plane-map time.
    """

    available: bool
    detail: str


_cached_mapping: MappingCapability | None = None


def reset_mapping_capability_cache() -> None:
    """Clear the cached mapping-capability probe (test/CI hook)."""
    global _cached_mapping
    _cached_mapping = None


def probe_mapping() -> MappingCapability:
    """Detect the geoviz mapping facade without raising; result is cached.

    Same cached-probe shape as :func:`probe_engine`. Fails closed (available
    False) when the geoviz package is missing or the Phase-2 mapping names
    are absent - callers degrade to list-only UI.
    """
    global _cached_mapping
    if _cached_mapping is not None:
        return _cached_mapping

    if os.environ.get("WLWS_DISABLE_MAPPING", "").strip() in ("1", "true", "yes"):
        _cached_mapping = MappingCapability(False, "WLWS_DISABLE_MAPPING set")
        return _cached_mapping

    try:
        from geoviz import (  # noqa: F401
            FilledContourLayer,
            coerce_to_project_crs,
            list_known_crs,
            set_project_crs,
        )
        _cached_mapping = MappingCapability(
            True, "geoviz mapping surface (FilledContourLayer + CRS helpers)"
        )
    except Exception as exc:  # noqa: BLE001
        _cached_mapping = MappingCapability(False, f"geoviz mapping unavailable: {exc}")
    return _cached_mapping


def mapping_available() -> bool:
    """True when the geoviz mapping surface can be used."""
    return probe_mapping().available


def create_well_log_view(parent=None) -> Any:
    """Instantiate native WellLogView or raise EngineUnavailable."""
    cap = probe_engine()
    if not cap.available or cap.well_log_view_cls is None:
        raise EngineUnavailable(cap.detail)
    return cap.well_log_view_cls(parent)


def _readonly_f64(arr: np.ndarray) -> np.ndarray:
    out = np.ascontiguousarray(arr, dtype=np.float64)
    if not out.flags.writeable:
        return out
    # Copy then freeze — submit_curve rejects writable buffers
    frozen = np.array(out, dtype=np.float64, copy=True)
    frozen.setflags(write=False)
    return frozen


def primary_curve_from_presentation(
    presentation: HostPresentation,
) -> tuple[np.ndarray, np.ndarray, str, str] | None:
    """Return (depth, values, mnemonic, value_unit) for first bound curve layer."""
    depth = _readonly_f64(np.asarray(presentation.depth, dtype=np.float64))
    for track in presentation.tracks:
        if not track.visible:
            continue
        if track.role != "curve" or not track.layers:
            continue
        layer = track.layers[0]
        vals = np.asarray(layer.values, dtype=np.float64).copy()
        nulls = np.asarray(layer.null_mask, dtype=bool)
        if nulls.size == vals.size:
            vals[nulls] = np.nan
        # The C++ bridge accepts NaN and breaks polylines at non-finite
        # samples (valid_sample checks isfinite); substituting 0.0 fabricated
        # fake zero segments that were also baked into exports (#586).
        values = _readonly_f64(vals)
        n = min(depth.size, values.size)
        if n < 2:
            return None
        return depth[:n], values[:n], layer.mnemonic, layer.unit or "unit"
    return None


def primary_curve_from_document(
    document: ImportedWellDocument,
) -> tuple[np.ndarray, np.ndarray, str, str] | None:
    depth = _readonly_f64(np.asarray(document.depth, dtype=np.float64))
    if not document.curves:
        return None
    curve = document.curves[0]
    vals = np.asarray(curve.values, dtype=np.float64).copy()
    nulls = np.asarray(curve.null_mask, dtype=bool)
    if nulls.size == vals.size:
        vals[nulls] = np.nan
    values = _readonly_f64(vals)
    n = min(depth.size, values.size)
    if n < 2:
        return None
    return depth[:n], values[:n], curve.mnemonic, curve.unit or "unit"


def submit_primary_curve(
    view: Any,
    *,
    depth: np.ndarray,
    values: np.ndarray,
    mnemonic: str,
    depth_unit: str,
    value_unit: str,
    document_id: str | None = None,
) -> dict[str, object]:
    """Call WellLogView.submit_curve with UUID entity ids."""
    doc_id = document_id or str(uuid.uuid4())
    # EntityId parse requires non-nil UUID strings
    try:
        uuid.UUID(doc_id)
    except ValueError:
        doc_id = str(uuid.uuid4())
    axis_id = str(uuid.uuid4())
    curve_id = str(uuid.uuid4())
    depth_r = _readonly_f64(depth)
    values_r = _readonly_f64(values)
    if depth_r.size != values_r.size:
        raise EngineSubmitError("depth and values length mismatch")
    try:
        report = view.submit_curve(
            depth_r,
            values_r,
            doc_id,
            axis_id,
            curve_id,
            mnemonic,
            depth_unit or "m",
            value_unit or "unit",
        )
    except Exception as exc:  # noqa: BLE001
        raise EngineSubmitError(str(exc)) from exc
    if not isinstance(report, dict):
        return {"raw": report, "curve_id": curve_id}
    out = dict(report)
    out.setdefault("curve_id", curve_id)
    out.setdefault("document_id", doc_id)
    return out  # type: ignore[return-value]


def load_presentation_into_view(
    view: Any,
    presentation: HostPresentation,
    *,
    tops: list[FormationTop] | None = None,
    depth_transform: list[dict[str, float]] | None = None,
) -> dict[str, object]:
    """Submit multi-track presentation to the engine.

    Prefer ``submit_multi_track`` so all bound curves (GR/RT/DEN, …) appear.
    Do **not** silently fall back to single-curve ``submit_curve`` (that only
    shows the first layer, usually GR, and hides other tracks). If multi-track
    is unavailable or fails, raise ``EngineSubmitError`` so the host canvas
    (full multi-track paint) is used instead.

    ``depth_transform`` (optional) is the MD→display (TVD/TVDSS) control-point
    list forwarded to ``submit_multi_track_presentation``; None keeps the MD
    default.
    """
    n_curve_tracks = sum(
        1
        for t in presentation.tracks
        if t.visible and t.role == "curve" and t.layers
    )
    if hasattr(view, "submit_multi_track"):
        try:
            return submit_multi_track_presentation(
                view, presentation, tops=tops, depth_transform=depth_transform
            )
        except EngineSubmitError as exc:
            # Multi-track preferred: re-raise so shell falls back to host canvas
            # rather than a single-GR engine view.
            if n_curve_tracks > 1:
                raise EngineSubmitError(
                    f"多图道引擎提交失败（已保留主机多图道画布）: {exc}"
                ) from exc
            # Single bound curve — legacy single-curve path is acceptable.
            pass
    elif n_curve_tracks > 1:
        raise EngineSubmitError(
            "WellLogView 不支持 submit_multi_track；使用主机多图道画布"
        )
    primary = primary_curve_from_presentation(presentation)
    if primary is None:
        raise EngineSubmitError("图版未绑定可提交的曲线")
    depth, values, mnemonic, value_unit = primary
    doc_id = presentation.well_document_id
    return submit_primary_curve(
        view,
        depth=depth,
        values=values,
        mnemonic=mnemonic,
        depth_unit=presentation.depth_unit or "m",
        value_unit=value_unit,
        document_id=doc_id if _is_uuid(doc_id) else None,
    )

def presentation_to_multi_track_payload(
    presentation: HostPresentation,
    *,
    tops: list[FormationTop] | None = None,
    intervals: list[dict[str, Any]] | None = None,
    patterns: list[dict[str, Any]] | None = None,
    depth_transform: list[dict[str, float]] | None = None,
) -> dict[str, Any]:
    """Build ``submit_multi_track`` payload from a host multi-track presentation.

    ``intervals`` (T4 / #276) is an optional list of depth-span dicts each
    shaped ``{id, top_depth, bottom_depth, fill_color?, pattern_id?, label?}``;
    ``patterns`` an optional list of vector-tile pattern dicts
    ``{id, tile_width_mm, tile_height_mm, foreground?, background?, primitives?}``.
    Both are passed through to the engine so exports match the screen's
    interval/pattern fills. The host sources these from the section view's
    TieQuad2D fills or other interval providers.
    """
    depth = _readonly_f64(np.asarray(presentation.depth, dtype=np.float64))
    if depth.size < 2:
        raise EngineSubmitError("深度样本不足")

    doc_id = presentation.well_document_id
    if not _is_uuid(doc_id):
        doc_id = str(uuid.uuid4())

    # Collect unique curve layers → curves list + map mnemonic→curve_id.
    # Multi-rate (Epic A): a layer with its own sampling axis carries its
    # "depth" in the curve entry; shared-axis layers keep the historic
    # truncate-to-shared-depth behaviour.
    curves: list[dict[str, Any]] = []
    curve_ids: dict[str, str] = {}  # layer identity -> curve_id (#585)
    n = depth.size

    for track in presentation.tracks:
        if not track.visible:
            continue
        if track.role != "curve":
            continue
        for layer in track.layers:
            # Identity = mnemonic + version (document invariant). The plain
            # mnemonic collapses edited-* correction tracks and multi-rate
            # resample leaves onto the first curve; layers without an explicit
            # identity keep the historic mnemonic-only behaviour (#585).
            key = layer.identity or layer.mnemonic.upper()
            if key in curve_ids:
                continue
            vals = np.asarray(layer.values, dtype=np.float64).copy()
            nulls = np.asarray(layer.null_mask, dtype=bool)
            if nulls.size == vals.size:
                vals[nulls] = np.nan
            values = _readonly_f64(vals)
            cid = str(uuid.uuid4())
            layer_depth = getattr(layer, "depth", None)
            if layer_depth is not None:
                ld = _readonly_f64(np.asarray(layer_depth, dtype=np.float64))
                if ld.size < 2 or values.size != ld.size:
                    continue  # per-curve axis must match its own values
                entry: dict[str, Any] = {
                    "curve_id": cid,
                    "mnemonic": layer.mnemonic,
                    "values": values,
                    "value_unit": layer.unit or "unit",
                    "depth": ld,
                }
            else:
                m = min(n, values.size)
                if m < 2:
                    continue
                entry = {
                    "curve_id": cid,
                    "mnemonic": layer.mnemonic,
                    "values": values[:m],
                    "value_unit": layer.unit or "unit",
                }
            curve_ids[key] = cid
            curves.append(entry)

    if not curves:
        raise EngineSubmitError("图版未绑定可提交的曲线")

    # Shared-axis curves must all match the shared depth exactly: truncate
    # every shared curve + the depth to the shortest shared length (per-curve
    # axis entries are untouched).
    shared = [c for c in curves if "depth" not in c]
    if shared:
        shared_len = min(int(c["values"].size) for c in shared)
        for c in shared:
            c["values"] = c["values"][:shared_len]
        depth = depth[:shared_len]

    tracks_payload: list[dict[str, Any]] = []
    for track in presentation.tracks:
        if not track.visible:
            continue
        if track.role != "curve" or not track.layers:
            continue
        layers_out: list[dict[str, Any]] = []
        for layer in track.layers:
            cid = curve_ids.get(layer.identity or layer.mnemonic.upper())
            if not cid:
                continue
            layers_out.append({"curve_id": cid, "color": layer.color or "#1972b8"})
        if not layers_out:
            continue
        width_mm = max(20.0, float(track.width_fraction) * 120.0)
        entry: dict[str, Any] = {
            "width_mm": width_mm,
            "layers": layers_out,
        }
        if track.scale is not None:
            entry["scale_min"] = float(track.scale.min)
            entry["scale_max"] = float(track.scale.max)
            entry["scale_mode"] = (
                "log" if track.scale.mode == "log" else "linear"
            )
            # FRS §2.x 反向刻度: the C++ renderer applies right_to_left as
            # ``1.0 - normalized_value`` (scene.cpp); payload-only plumbing.
            if getattr(track.scale, "reverse", False):
                entry["scale_reverse"] = True
        tracks_payload.append(entry)

    if not tracks_payload:
        raise EngineSubmitError("无曲线道可提交")

    payload: dict[str, Any] = {
        "document_id": doc_id,
        "depth": depth,
        "depth_unit": presentation.depth_unit or "m",
        "curves": curves,
        "tracks": tracks_payload,
    }
    if depth_transform:
        # TVD/TVDSS 显示域: reference=MD → display=TVD/TVDSS control points
        # (engine accepts either monotonic direction).
        payload["depth_transform"] = depth_transform
    if tops:
        markers = []
        for t in tops:
            mid = t.id if t.id and _is_uuid(t.id) else str(uuid.uuid4())
            entry: dict[str, object] = {
                "id": mid,
                "depth": float(t.depth),
                "label": t.name,
            }
            semantic = getattr(t, "semantic", "") or ""
            if semantic:
                entry["semantic"] = semantic
            markers.append(entry)
        if markers:
            payload["markers"] = markers
    if intervals:
        payload["intervals"] = [
            {
                "id": iv["id"],
                "top_depth": float(iv["top_depth"]),
                "bottom_depth": float(iv["bottom_depth"]),
                **({"fill_color": iv["fill_color"]} if iv.get("fill_color") else {}),
                **({"pattern_id": iv["pattern_id"]} if iv.get("pattern_id") else {}),
                **({"label": iv["label"]} if iv.get("label") else {}),
            }
            for iv in intervals
            if iv.get("id") and float(iv.get("bottom_depth", 0)) > float(iv.get("top_depth", 0))
        ]
    if patterns:
        payload["patterns"] = patterns
    return payload


def submit_multi_track_presentation(
    view: Any,
    presentation: HostPresentation,
    *,
    tops: list[FormationTop] | None = None,
    depth_transform: list[dict[str, float]] | None = None,
) -> dict[str, object]:
    """Call WellLogView.submit_multi_track with a host presentation.

    ``depth_transform`` (optional) is forwarded to the payload builder
    (MD→display TVD/TVDSS control points); None keeps the MD default.
    """
    if not hasattr(view, "submit_multi_track"):
        raise EngineSubmitError("WellLogView 不支持 submit_multi_track（请重建 welllog 绑定）")
    payload = presentation_to_multi_track_payload(
        presentation, tops=tops, depth_transform=depth_transform
    )
    try:
        report = view.submit_multi_track(payload)
    except Exception as exc:  # noqa: BLE001
        raise EngineSubmitError(str(exc)) from exc
    if not isinstance(report, dict):
        return {"raw": report}
    return dict(report)


def presentations_to_multi_well_payload(
    presentations: list[HostPresentation],
    *,
    tops_per_well: list[list[FormationTop]] | None = None,
    gap_mm: float = 5.0,
    shared_depth: tuple[float, float] | None = None,
    links: list[HorizonLink] | None = None,
    multi_track: bool = True,
    depth_transform_per_well: dict[str, list[dict[str, float]]] | None = None,
) -> dict[str, Any]:
    """Build multi-well payload; prefer multi-track columns when possible (#232)."""
    if len(presentations) < 1:
        raise EngineSubmitError("至少需要一口井")
    wells: list[dict[str, Any]] = []
    d0_global: float | None = None
    d1_global: float | None = None
    # well_document_id → payload document_id (usually same)
    doc_ids: dict[str, str] = {}
    # Ensure marker ids from tops match link marker ids when possible
    marker_id_by_well_name: dict[tuple[str, str], str] = {}

    for i, pres in enumerate(presentations):
        depth = _readonly_f64(np.asarray(pres.depth, dtype=np.float64))
        if depth.size < 2:
            raise EngineSubmitError(f"井 {pres.well_name} 深度不足")
        dmin, dmax = float(np.nanmin(depth)), float(np.nanmax(depth))
        d0_global = dmin if d0_global is None else min(d0_global, dmin)
        d1_global = dmax if d1_global is None else max(d1_global, dmax)
        doc_id = (
            pres.well_document_id
            if _is_uuid(pres.well_document_id)
            else str(uuid.uuid4())
        )
        doc_ids[pres.well_document_id] = doc_id

        use_mt = multi_track and pres.curve_track_count >= 1
        well: dict[str, Any]
        if use_mt:
            try:
                mt = presentation_to_multi_track_payload(
                    pres,
                    tops=(
                        tops_per_well[i]
                        if tops_per_well is not None and i < len(tops_per_well)
                        else None
                    ),
                )
            except EngineSubmitError:
                use_mt = False
                mt = None
            if use_mt and mt is not None:
                well = {
                    "document_id": doc_id,
                    "depth": mt["depth"],
                    "depth_unit": mt["depth_unit"],
                    "axis_id": mt.get("axis_id") or str(uuid.uuid4()),
                    "curves": mt["curves"],
                    "tracks": mt["tracks"],
                    "width_mm": max(
                        40.0, 30.0 * max(1, len(mt["tracks"]))
                    ),
                }
                if mt.get("markers"):
                    well["markers"] = mt["markers"]
                    for m in mt["markers"]:
                        label = str(m.get("label") or "")
                        mid = str(m.get("id") or "")
                        if label and mid:
                            marker_id_by_well_name[
                                (pres.well_document_id, label)
                            ] = mid

        if not use_mt:
            primary = primary_curve_from_presentation(pres)
            if primary is None:
                raise EngineSubmitError(f"井 {pres.well_name} 无绑定曲线")
            depth_p, values, mnemonic, value_unit = primary
            well = {
                "document_id": doc_id,
                "axis_id": str(uuid.uuid4()),
                "curve_id": str(uuid.uuid4()),
                "mnemonic": mnemonic,
                "depth_unit": pres.depth_unit or "m",
                "value_unit": value_unit or "unit",
                "depth": depth_p,
                "values": values,
                "width_mm": 35.0,
            }
            if tops_per_well is not None and i < len(tops_per_well):
                markers = []
                for t in tops_per_well[i]:
                    mid = t.id if t.id and _is_uuid(t.id) else str(uuid.uuid4())
                    marker_id_by_well_name[(pres.well_document_id, t.name)] = mid
                    entry: dict[str, object] = {
                        "id": mid,
                        "depth": float(t.depth),
                        "label": t.name,
                    }
                    semantic = getattr(t, "semantic", "") or ""
                    if semantic:
                        entry["semantic"] = semantic
                    markers.append(entry)
                if markers:
                    well["markers"] = markers
        elif tops_per_well is not None and i < len(tops_per_well) and "markers" not in well:
            markers = []
            for t in tops_per_well[i]:
                mid = t.id if t.id and _is_uuid(t.id) else str(uuid.uuid4())
                marker_id_by_well_name[(pres.well_document_id, t.name)] = mid
                entry: dict[str, object] = {
                    "id": mid,
                    "depth": float(t.depth),
                    "label": t.name,
                }
                semantic = getattr(t, "semantic", "") or ""
                if semantic:
                    entry["semantic"] = semantic
                markers.append(entry)
            if markers:
                well["markers"] = markers
        if depth_transform_per_well:
            xform = depth_transform_per_well.get(pres.well_name)
            if xform:
                # TVD/TVDSS 显示域: per-well MD→display control points.
                well["depth_transform"] = xform
        wells.append(well)

    if shared_depth is not None:
        shared_top, shared_bottom = shared_depth
    else:
        shared_top = d0_global if d0_global is not None else 0.0
        shared_bottom = d1_global if d1_global is not None else 1.0
    if not (shared_bottom > shared_top):
        shared_bottom = shared_top + 1.0

    payload: dict[str, Any] = {
        "gap_mm": float(gap_mm),
        "shared_top": float(shared_top),
        "shared_bottom": float(shared_bottom),
        "wells": wells,
    }
    if links:
        # Align marker ids with those just assigned from tops
        fixed: list[HorizonLink] = []
        for lk in links:
            left_m = marker_id_by_well_name.get(
                (lk.left_well_id, lk.name), lk.left_marker_id
            )
            right_m = marker_id_by_well_name.get(
                (lk.right_well_id, lk.name), lk.right_marker_id
            )
            fixed.append(
                HorizonLink(
                    id=lk.id,
                    left_well_id=lk.left_well_id,
                    right_well_id=lk.right_well_id,
                    name=lk.name,
                    left_depth=lk.left_depth,
                    right_depth=lk.right_depth,
                    left_marker_id=left_m or lk.left_marker_id,
                    right_marker_id=right_m or lk.right_marker_id,
                    color=lk.color,
                )
            )
        overlays = links_to_engine_overlays(fixed, well_document_ids=doc_ids)
        if overlays:
            payload["overlays"] = overlays
    return payload


def submit_multi_well_presentations(
    view: Any,
    presentations: list[HostPresentation],
    *,
    tops_per_well: list[list[FormationTop]] | None = None,
    gap_mm: float = 5.0,
    shared_depth: tuple[float, float] | None = None,
    links: list[HorizonLink] | None = None,
    depth_transform_per_well: dict[str, list[dict[str, float]]] | None = None,
) -> dict[str, object]:
    """Call WellLogView.submit_multi_well_section for correlation-lite."""
    if not hasattr(view, "submit_multi_well_section"):
        raise EngineSubmitError(
            "WellLogView 不支持 submit_multi_well_section（请重建 welllog 绑定）"
        )
    payload = presentations_to_multi_well_payload(
        presentations,
        tops_per_well=tops_per_well,
        gap_mm=gap_mm,
        shared_depth=shared_depth,
        links=links,
        depth_transform_per_well=depth_transform_per_well,
    )
    try:
        report = view.submit_multi_well_section(payload)
    except Exception as exc:  # noqa: BLE001
        raise EngineSubmitError(str(exc)) from exc
    if not isinstance(report, dict):
        return {"raw": report}
    return dict(report)


def clear_multi_well(view: Any) -> None:
    if hasattr(view, "clear_multi_well_section"):
        try:
            view.clear_multi_well_section()
        except Exception:  # noqa: BLE001
            pass


def _is_uuid(text: str) -> bool:
    try:
        uuid.UUID(text)
        return True
    except (ValueError, TypeError, AttributeError):
        return False


def survey_depth_transform(
    trajectory: Any, mode: str
) -> list[dict[str, float]]:
    """(reference=MD, display) control points for the engine depth transform.

    TVD/TVDSS 域区间道投影 (Epic C 收尾 slice 2): from a survey trajectory
    (survey.compute_trajectory output) build the control-point list the
    engine submits as ``depth_transform``. TVD display increases with MD;
    TVDSS display decreases (deeper → smaller subsea depth) — the engine
    accepts either monotonic direction. Returns [] when the mode is not
    tvd/tvdss or fewer than two finite stations exist.
    """
    if mode not in ("tvd", "tvdss"):
        return []
    md = np.asarray(trajectory.md, dtype=np.float64)
    if mode == "tvd":
        display = np.asarray(trajectory.tvd, dtype=np.float64)
    else:
        display = np.asarray(trajectory.tvdss, dtype=np.float64)
    points = [
        (float(a), float(b))
        for a, b in zip(md, display)
        if np.isfinite(a) and np.isfinite(b)
    ]
    if len(points) < 2:
        return []
    points.sort(key=lambda p: p[0])
    return [{"reference": r, "display": d} for r, d in points]


# ---------------------------------------------------------------------------
# Track/Data workflow commands (ADR 0056/0057)
#
# The host presentation (display set + template) stays the persistence truth;
# these wrappers mirror value-level edits into the engine session as one
# validated, undoable command each instead of a full ``submit_multi_track``
# re-submission. Structural changes (track/layer set or order changed) still
# go through the full path.


def track_command_supported(view: Any) -> bool:
    """True when the engine view exposes apply_track_command."""
    return hasattr(view, "apply_track_command")


def apply_track_op(view: Any, op: str, **fields: Any) -> dict[str, Any]:
    """Apply one engine track command; raises EngineSubmitError on failure."""
    if not track_command_supported(view):
        raise EngineSubmitError(
            "WellLogView 不支持 apply_track_command（请升级 welllog 绑定）"
        )
    payload: dict[str, Any] = {"op": op, **fields}
    try:
        report = view.apply_track_command(payload)
    except Exception as exc:  # noqa: BLE001
        raise EngineSubmitError(str(exc)) from exc
    return dict(report) if isinstance(report, dict) else {"raw": report}


def engine_presentation_state(view: Any, document_id: str) -> dict | None:
    """Live engine presentation {tracks, scales, curve_layers} or None."""
    getter = getattr(view, "presentation_state", None)
    if getter is None:
        return None
    try:
        return getter(document_id)
    except Exception:  # noqa: BLE001 — degrade to full re-submit
        return None


def engine_selection_state(view: Any) -> dict | None:
    """The view document's shared Selection Set entry (ADR 0024) or None."""
    getter = getattr(view, "selection_state", None)
    if getter is None:
        return None
    try:
        return getter()
    except Exception:  # noqa: BLE001
        return None


def engine_set_row_selection(
    view: Any, axis_id: str, first_row: int, last_row: int
) -> dict | None:
    """Drive the engine selection from table rows; None when unsupported."""
    setter = getattr(view, "set_row_selection", None)
    if setter is None:
        return None
    try:
        return setter(axis_id, int(first_row), int(last_row))
    except Exception:  # noqa: BLE001
        return None


def engine_hover_info(view: Any) -> dict | None:
    """Resolved hover inspect dict (mnemonic/unit/QC/scale context) or None."""
    getter = getattr(view, "hover_info", None)
    if getter is None:
        return None
    try:
        return getter()
    except Exception:  # noqa: BLE001
        return None


def _host_submission_tracks(presentation: HostPresentation) -> list:
    """Visible curve tracks with layers, in ``submit_multi_track`` order."""
    return [
        track
        for track in presentation.tracks
        if track.visible and track.role == "curve" and track.layers
    ]


def _host_submission_width_mm(track: Any) -> float:
    # Mirrors presentation_to_multi_track_payload's width rule exactly.
    return max(20.0, float(track.width_fraction) * 120.0)


def _layer_identity(layer: Any) -> str:
    # Same identity rule presentation_to_multi_track_payload uses to bind
    # curves: identity first, mnemonic fallback.
    return getattr(layer, "identity", None) or layer.mnemonic.upper()


def capture_engine_bindings(
    view: Any,
    document_id: str,
    presentation: HostPresentation,
    previous: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Snapshot engine entity ids keyed by host track id after a full submit.

    ``submit_multi_track`` derives deterministic engine ids internally, so
    right after a full submission this reads them back from
    ``presentation_state`` and maps each host ``BoundTrack.id`` to its engine
    track/scale ids, each layer identity to its engine layer id, and each
    identity to the engine CURVE id (the engine document keeps curves across
    unbinds, so a re-displayed curve can re-bind incrementally). When
    ``previous`` is given, identities are matched through its curve-id cache
    (exact); otherwise by submission position. The snapshot feeds
    :func:`incremental_presentation_sync`; None when the state cannot be read
    or the shapes do not line up (the caller then never syncs incrementally).
    """
    if not track_command_supported(view) or not _is_uuid(document_id):
        return None
    state = engine_presentation_state(view, document_id)
    if state is None:
        return None
    host_tracks = _host_submission_tracks(presentation)
    engine_tracks = sorted(
        list(state.get("tracks", [])), key=lambda t: t.get("z_order", 0)
    )
    if len(host_tracks) != len(engine_tracks):
        return None
    engine_layers_by_track: dict[str, list[dict]] = {}
    engine_curves: dict[str, str] = {}  # engine curve id → engine layer id
    for layer in state.get("curve_layers", []):
        engine_layers_by_track.setdefault(
            str(layer.get("track_id", "")), []
        ).append(layer)
        engine_curves[str(layer.get("curve_id", ""))] = str(layer.get("id", ""))
    scales_by_track: dict[str, list[dict]] = {}
    for scale in state.get("scales", []):
        scales_by_track.setdefault(scale.get("track_id", ""), []).append(scale)

    # Identity → engine curve id, from the previous snapshot when available.
    identity_to_curve: dict[str, str] = {}
    if previous:
        for entry in previous.values():
            for identity, curve_id in entry.get("curves", {}).items():
                identity_to_curve[identity] = curve_id

    bindings: dict[str, Any] = {}
    if previous and previous.get("__curves__"):
        # Persistent identity → engine curve id cache (survives track
        # removals; the engine document keeps the curves).
        bindings["__curves__"] = dict(previous["__curves__"])
    for host_track, engine_track in zip(host_tracks, engine_tracks):
        engine_id = engine_track.get("id", "")
        entry: dict[str, Any] = {
            "engine_track": engine_id,
            "engine_scale": None,
            "layers": {},
            "curves": {},
        }
        engine_layers = sorted(
            engine_layers_by_track.get(engine_id, []),
            key=lambda item: item.get("z_order", 0),
        )
        if len(engine_layers) != len(host_track.layers):
            return None
        matched: list[tuple[str, dict]] = []
        unmatched: list[dict] = list(engine_layers)
        for host_layer in host_track.layers:
            identity = _layer_identity(host_layer)
            curve_id = identity_to_curve.get(identity)
            engine_layer = None
            if curve_id and engine_curves.get(curve_id) in {
                str(l.get("id", "")) for l in unmatched
            }:
                engine_layer = next(
                    l
                    for l in unmatched
                    if str(l.get("id", "")) == engine_curves.get(curve_id)
                )
            if engine_layer is not None:
                unmatched.remove(engine_layer)
            else:
                engine_layer = unmatched.pop(0) if unmatched else None
            if engine_layer is None:
                return None
            entry["layers"][identity] = engine_layer.get("id", "")
            entry["curves"][identity] = engine_layer.get("curve_id", "")
            bindings.setdefault("__curves__", {})[identity] = (
                engine_layer.get("curve_id", "")
            )
            matched.append((identity, engine_layer))
        engine_scales = scales_by_track.get(engine_id, [])
        if engine_scales:
            entry["engine_scale"] = engine_scales[0].get("id", "")
        bindings[str(host_track.id)] = entry
    return bindings


def incremental_presentation_sync(
    view: Any,
    presentation: HostPresentation,
    bindings: dict[str, Any] | None,
) -> dict[str, Any] | None:
    """Mirror presentation edits into engine track commands.

    Uses the ``capture_engine_bindings`` snapshot (host track id → engine
    ids) and runs in two phases:

    Phase A (structure) — ``reorder_tracks``, ``remove_track`` (a host track
    that lost all layers leaves, cascading its scales/layers), ``add_track``
    + ``bind_curve`` (a known track id reappearing with layers; curves are
    still in the engine document, so binds reuse them), ``move_curve_layer``
    and ``unbind_curve`` for membership changes.

    Phase B (values, computed against the post-A engine state) —
    ``resize_track``, ``set_scale``, ``set_layer_style``,
    ``reorder_curve_layers``.

    All commands ride the engine's ApplyPatchCommand engine — atomic,
    undoable, O(changed presentation entities), never re-sending curve
    buffers (ADR 0056/0057). Returns the refreshed bindings snapshot when
    every difference was applied (the caller keeps it for the next sync);
    None when the change is outside what the snapshot can express (an
    unknown host track id, a NEW curve identity the engine document cannot
    hold, no engine state, commands unsupported, or a rejected command) —
    the caller then falls back to the full ``submit_multi_track`` path. A
    rejected command changes nothing, so falling back is always safe.
    """
    if bindings is None or not track_command_supported(view):
        return None
    doc_id = presentation.well_document_id
    if not _is_uuid(doc_id):
        return None
    state = engine_presentation_state(view, doc_id)
    if state is None:
        return None

    host_by_id = {str(t.id): t for t in presentation.tracks}
    host_tracks = _host_submission_tracks(presentation)
    host_track_ids = {str(t.id) for t in host_tracks}

    # Engine curve ids per identity, from the WHOLE snapshot (including
    # tracks a previous sync removed — the engine document keeps their
    # curves, so a re-checked track can re-bind them incrementally).
    engine_curve_of_identity: dict[str, str] = dict(
        bindings.get("__curves__", {})
    )
    for key, entry in bindings.items():
        if key == "__curves__":
            continue
        for identity, curve_id in entry.get("curves", {}).items():
            engine_curve_of_identity[identity] = curve_id

    # Where each layer identity lives now (host truth).
    host_track_of_identity: dict[str, Any] = {}
    host_layer_of_identity: dict[str, Any] = {}
    host_layer_order: dict[str, list[str]] = {}
    for host_track in host_tracks:
        identities = [_layer_identity(layer) for layer in host_track.layers]
        host_layer_order[str(host_track.id)] = identities
        for identity, host_layer in zip(identities, host_track.layers):
            host_track_of_identity[identity] = host_track
            host_layer_of_identity[identity] = host_layer
    # A host layer the snapshot never bound = a NEW curve → the engine
    # document may not hold it; the full path must submit it.
    for identity in host_track_of_identity:
        if identity not in engine_curve_of_identity:
            return None
    # A host track the snapshot never bound can only be handled when every
    # one of its curves is already in the engine document (add_track +
    # bind); otherwise the arrangement is structural for this snapshot.
    for track_id_key in host_track_ids:
        if track_id_key in bindings:
            continue
        identities = host_layer_order.get(track_id_key, [])
        if not all(i in engine_curve_of_identity for i in identities):
            return None
    # Every engine track must be one the snapshot knows.
    snapshot_engine_tracks = {
        entry["engine_track"]
        for key, entry in bindings.items()
        if key != "__curves__"
    }
    for engine_track in state.get("tracks", []):
        if str(engine_track.get("id", "")) not in snapshot_engine_tracks:
            return None

    # --- Phase A: structure -------------------------------------------------
    phase_a: list[tuple[str, dict[str, Any]]] = []
    # Working copy of the bindings this sync will keep updating. The
    # "__curves__" cache (identity → engine curve id) survives track
    # removals so a re-checked curve re-binds without a full submit.
    live_bindings: dict[str, Any] = {
        "__curves__": dict(engine_curve_of_identity),
    }
    for key, entry in bindings.items():
        if key == "__curves__":
            continue
        live_bindings[key] = {
            "engine_track": entry["engine_track"],
            "engine_scale": entry.get("engine_scale"),
            "layers": dict(entry.get("layers", {})),
            "curves": dict(entry.get("curves", {})),
        }

    # Tracks to remove: snapshot tracks the host no longer submits (left the
    # display set, hidden, or lost every layer — the payload builder skips
    # them, so engine parity removes them, cascading scales/layers).
    engine_state_track_ids = {
        str(t.get("id", "")) for t in state.get("tracks", [])
    }
    removed_engine_tracks: set[str] = set()
    for track_id_key, entry in bindings.items():
        if track_id_key == "__curves__" or track_id_key in host_track_ids:
            continue
        removed_engine_tracks.add(entry["engine_track"])
        if entry["engine_track"] in engine_state_track_ids:
            phase_a.append((
                "remove_track",
                {"track_id": entry["engine_track"]},
            ))
        live_bindings.pop(track_id_key, None)

    # Tracks to add: host submission tracks whose engine track is gone.
    for host_track in host_tracks:
        entry = live_bindings.get(str(host_track.id))
        if entry is not None and entry["engine_track"] in engine_state_track_ids:
            continue
        width_mm = _host_submission_width_mm(host_track)
        report = apply_track_op(
            view,
            "add_track",
            document_id=doc_id,
            width_mm=width_mm,
        )
        new_engine_track = str(report.get("track_id", ""))
        if not new_engine_track:
            return None
        if entry is None:
            # Brand-new slot: remember it so future syncs know the track.
            entry = {
                "engine_track": new_engine_track,
                "engine_scale": None,
                "layers": {},
                "curves": {},
            }
            live_bindings[str(host_track.id)] = entry
            for host_layer in host_track.layers:
                identity = _layer_identity(host_layer)
                entry["curves"][identity] = (
                    engine_curve_of_identity.get(identity, "")
                )
                live_bindings["__curves__"][identity] = (
                    engine_curve_of_identity.get(identity, "")
                )
        else:
            entry["engine_track"] = new_engine_track
        # Bind every layer (the engine document still holds the curves).
        for host_layer in host_track.layers:
            identity = _layer_identity(host_layer)
            curve_id = engine_curve_of_identity.get(identity, "")
            if not curve_id:
                return None
            bind_report = apply_track_op(
                view,
                "bind_curve",
                document_id=doc_id,
                curve_id=curve_id,
                track_id=new_engine_track,
                color=(host_layer.color or "#1972b8").lower(),
            )
            entry["layers"][identity] = str(bind_report.get("layer_id", ""))
            entry["curves"][identity] = curve_id
        entry["engine_scale"] = None  # generated scale, resolved in phase B

    # Layer membership on surviving tracks: moves + unbinds + re-binds.
    engine_layers_by_id = {
        str(l.get("id", "")): l for l in state.get("curve_layers", [])
    }
    for track_id_key, entry in live_bindings.items():
        if track_id_key == "__curves__":
            continue
        engine_track_id = entry["engine_track"]
        if engine_track_id in removed_engine_tracks:
            continue
        for identity, engine_layer_id in list(entry.get("layers", {}).items()):
            engine_layer = engine_layers_by_id.get(str(engine_layer_id))
            host_track_now = host_track_of_identity.get(identity)
            if host_track_now is None:
                # Curve left the display set → remove the layer only; the
                # engine document keeps the curve (data truth).
                if engine_layer is not None:
                    phase_a.append((
                        "unbind_curve",
                        {"layer_id": engine_layer_id},
                    ))
                entry["layers"].pop(identity, None)
                continue
            target = live_bindings.get(str(host_track_now.id))
            if target is None:
                return None
            if engine_layer is None:
                # Layer was unbound earlier; the engine document still holds
                # the curve — re-bind with the host style.
                curve_id = entry.get("curves", {}).get(identity, "")
                if not curve_id:
                    return None
                host_layer = host_layer_of_identity[identity]
                bind_report = apply_track_op(
                    view,
                    "bind_curve",
                    document_id=doc_id,
                    curve_id=curve_id,
                    track_id=target["engine_track"],
                    color=(host_layer.color or "#1972b8").lower(),
                )
                target["layers"][identity] = str(
                    bind_report.get("layer_id", "")
                )
                target["curves"][identity] = curve_id
                live_bindings["__curves__"][identity] = curve_id
                entry["layers"].pop(identity, None)
                continue
            if str(engine_layer.get("track_id", "")) != target["engine_track"]:
                phase_a.append((
                    "move_curve_layer",
                    {
                        "layer_id": engine_layer_id,
                        "target_track_id": target["engine_track"],
                    },
                ))
                # The identity now belongs to the target track's entry.
                target["layers"][identity] = engine_layer_id
                if identity not in target.get("curves", {}):
                    target["curves"][identity] = entry.get("curves", {}).get(
                        identity, ""
                    )
                entry["layers"].pop(identity, None)

    # Track order after adds/removals: engine z-order mirrors host order.
    want_order = [
        live_bindings[str(t.id)]["engine_track"]
        for t in host_tracks
        if str(t.id) in live_bindings
    ]
    for op, fields in phase_a:
        fields = {"document_id": doc_id, **fields}
        try:
            apply_track_op(view, op, **fields)
        except EngineSubmitError:
            return None

    # --- Phase B: values against the refreshed state ------------------------
    state = engine_presentation_state(view, doc_id)
    if state is None:
        return None
    engine_tracks_by_id = {
        str(t.get("id", "")): t for t in state.get("tracks", [])
    }
    engine_layers_by_id = {
        str(l.get("id", "")): l for l in state.get("curve_layers", [])
    }
    engine_order_now = [
        t.get("id", "")
        for t in sorted(
            state.get("tracks", []), key=lambda item: item.get("z_order", 0)
        )
    ]
    if engine_order_now != want_order:
        try:
            apply_track_op(
                view,
                "reorder_tracks",
                document_id=doc_id,
                track_ids=want_order,
            )
        except EngineSubmitError:
            return None

    phase_b: list[tuple[str, dict[str, Any]]] = []
    for host_track in host_tracks:
        entry = live_bindings.get(str(host_track.id))
        if entry is None:
            return None
        engine_track = engine_tracks_by_id.get(entry["engine_track"])
        if engine_track is None:
            return None
        engine_id = entry["engine_track"]

        want_width = _host_submission_width_mm(host_track)
        if abs(float(engine_track.get("width_mm", 0.0)) - want_width) > 1.0e-6:
            phase_b.append((
                "resize_track",
                {"track_id": engine_id, "width_mm": want_width},
            ))

        if host_track.scale is not None:
            engine_scale = next(
                (
                    sc
                    for sc in state.get("scales", [])
                    if sc.get("track_id") == engine_id
                ),
                None,
            )
            if engine_scale is not None:
                entry["engine_scale"] = engine_scale.get("id")
                fields: dict[str, Any] = {
                    "scale_id": engine_scale.get("id", "")
                }
                changed = False
                if (
                    abs(
                        float(engine_scale.get("minimum", 0.0))
                        - float(host_track.scale.min)
                    )
                    > 1.0e-9
                ):
                    fields["minimum"] = float(host_track.scale.min)
                    changed = True
                if (
                    abs(
                        float(engine_scale.get("maximum", 0.0))
                        - float(host_track.scale.max)
                    )
                    > 1.0e-9
                ):
                    fields["maximum"] = float(host_track.scale.max)
                    changed = True
                want_mode = (
                    "logarithmic"
                    if host_track.scale.mode == "log"
                    else "linear"
                )
                if engine_scale.get("mode") != want_mode:
                    fields["mode"] = want_mode
                    changed = True
                want_reverse = bool(
                    getattr(host_track.scale, "reverse", False)
                )
                have_reverse = (
                    engine_scale.get("direction") == "right_to_left"
                )
                if have_reverse != want_reverse:
                    fields["direction"] = (
                        "right_to_left" if want_reverse else "left_to_right"
                    )
                    changed = True
                if changed:
                    phase_b.append(("set_scale", fields))

        # In-track layer order + colors.
        identities = host_layer_order.get(str(host_track.id), [])
        engine_layer_ids_in_track = [
            l.get("id", "")
            for l in sorted(
                [
                    l
                    for l in state.get("curve_layers", [])
                    if str(l.get("track_id", "")) == engine_id
                ],
                key=lambda item: item.get("z_order", 0),
            )
        ]
        want_layer_ids = [
            entry["layers"][identity]
            for identity in identities
            if identity in entry.get("layers", {})
            and str(entry["layers"].get(identity)) in engine_layers_by_id
        ]
        if [str(x) for x in engine_layer_ids_in_track] != [
            str(x) for x in want_layer_ids
        ]:
            if want_layer_ids:
                phase_b.append((
                    "reorder_curve_layers",
                    {"track_id": engine_id, "layer_ids": want_layer_ids},
                ))
        for host_layer in host_track.layers:
            identity = _layer_identity(host_layer)
            engine_layer_id = entry["layers"].get(identity)
            engine_layer = engine_layers_by_id.get(str(engine_layer_id))
            if engine_layer is None:
                continue
            host_color = (host_layer.color or "#1972b8").lower()
            if str(engine_layer.get("color", "")).lower() != host_color:
                phase_b.append((
                    "set_layer_style",
                    {"layer_id": engine_layer_id, "color": host_color},
                ))

    for op, fields in phase_b:
        fields = {"document_id": doc_id, **fields}
        try:
            apply_track_op(view, op, **fields)
        except EngineSubmitError:
            return None
    return live_bindings
