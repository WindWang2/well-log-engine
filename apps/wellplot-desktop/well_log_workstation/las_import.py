"""LAS file import for the host catalog (#218).

Uses **lasio** (host dependency) to normalize ASCII LAS into curve arrays.
Engine ``LasSourceAdapter`` remains the C++ path; Python bindings do not expose
it yet, so the workstation host owns this adapter seam until bindings land.
"""

from __future__ import annotations

import shutil
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

import lasio
import numpy as np

from well_log_workstation.workspace import Workspace, add_well

if TYPE_CHECKING:
    from well_log_workstation.core_photo_model import CorePhotoModel
    from well_log_workstation.lithology_model import LithologyModel
    from well_log_workstation.perforation_model import PerforationModel
    from well_log_workstation.well_test_model import WellTestModel


class LasImportError(Exception):
    """User-visible LAS import failure."""


@dataclass(frozen=True)
class ImportedCurve:
    mnemonic: str
    unit: str
    values: np.ndarray  # float64, read-only
    null_mask: np.ndarray  # bool, True where null
    # Optional per-curve sampling axis (multi-rate, Epic A): when the curve's
    # sample count differs from the document depth axis, it keeps its own
    # depth coordinates instead of being truncated/padded onto the shared
    # axis. None = shares ``ImportedWellDocument.depth`` (index-aligned).
    depth: np.ndarray | None = None
    # Version identity (Epic A): same mnemonic may exist in several versions
    # (e.g. "raw" vs "resample-0.5m"); identity = mnemonic + version, never
    # the mnemonic alone. "raw" is the historic default for imported curves.
    version: str = "raw"


@dataclass
class ImportedWellDocument:
    """Host-side well document after LAS import (curves readable without UI)."""

    document_id: str
    well_name: str
    source_path: str  # relative path under workspace
    depth: np.ndarray  # float64 MD, read-only
    depth_unit: str
    curves: list[ImportedCurve] = field(default_factory=list)
    diagnostics: list[str] = field(default_factory=list)
    # Phase-2 T2 (#246): wellhead coordinates from LAS headers
    # (WELL-X/WELL-Y in ft, or LAT/LONG/LONGITUDE/LATITUDE in decimal degrees).
    lng: float | None = None
    lat: float | None = None
    crs: str | None = "EPSG:4326"
    # Wellhead elevations + total depth (FRS §1.x): KB/GL/MaxMD from LAS headers.
    kb_m: float | None = None
    gl_m: float | None = None
    max_md: float | None = None
    # Per-well lithology segments (FRS §2.x): the shell attaches the loaded
    # ``wells/<id>/lithology.json`` model before applying templates.
    lithology: LithologyModel | None = None
    # Per-well core-photo segments (FRS §2.x): the shell attaches the loaded
    # ``wells/<id>/core_photos.json`` model before applying templates.
    core_photos: CorePhotoModel | None = None
    # Epic C (C3/C4): per-well engineering data attached the same way —
    # well-test intervals (试油/解释成果道) and perforations (射孔/井下工程道).
    well_tests: WellTestModel | None = None
    perforations: PerforationModel | None = None

    def curve_by_mnemonic(
        self, mnemonic: str, version: str | None = None
    ) -> ImportedCurve | None:
        """Find a curve by mnemonic (+ optional version).

        ``version=None`` returns the first match (historic behaviour);
        otherwise only curves whose ``version`` equals the requested one
        qualify (multi-rate versions, Epic A).
        """
        key = mnemonic.strip().upper()
        for c in self.curves:
            if c.mnemonic.upper() != key:
                continue
            if version is not None and getattr(c, "version", "raw") != version:
                continue
            return c
        return None

    def sample_value(self, mnemonic: str, index: int) -> float | None:
        curve = self.curve_by_mnemonic(mnemonic)
        if curve is None or index < 0 or index >= curve.values.size:
            return None
        if curve.null_mask[index]:
            return None
        return float(curve.values[index])


@dataclass
class LasImportResult:
    catalog_well_id: str
    document: ImportedWellDocument


def _freeze(arr: np.ndarray) -> np.ndarray:
    out = np.ascontiguousarray(arr, dtype=np.float64)
    out.setflags(write=False)
    return out


def parse_las_file(path: Path | str) -> ImportedWellDocument:
    """Parse a LAS file into an in-memory document (no workspace side effects)."""
    src = Path(path).expanduser().resolve()
    if not src.is_file():
        raise LasImportError(f"LAS 文件不存在: {src}")

    try:
        las = lasio.read(str(src), ignore_header_errors=True)
    except Exception as exc:  # lasio raises various errors
        raise LasImportError(f"无法解析 LAS: {exc}") from exc

    if not las.curves:
        raise LasImportError("LAS 中没有曲线")

    # Depth: first curve is conventionally DEPT/DEPTH/MD
    depth_curve = las.curves[0]
    depth = np.asarray(depth_curve.data, dtype=np.float64)
    if depth.size == 0:
        raise LasImportError("深度曲线为空")

    diagnostics: list[str] = []
    well_name = ""
    try:
        well_name = str(las.well.WELL.value).strip() if las.well.WELL.value else ""
    except Exception:
        well_name = ""
    if not well_name:
        well_name = src.stem
        diagnostics.append("缺少 WELL 头；使用文件名作为井名")

    # Phase-2 T2 (#246): wellhead coordinates from LAS headers. All reads are
    # tolerant of absence - wells without headers stay in the catalog but
    # with lng/lat = None (marked, not drawn on the plane map).
    lng: float | None = None
    lat: float | None = None
    crs: str | None = "EPSG:4326"

    def _header_float(*mnemonics: str) -> float | None:
        for mnem in mnemonics:
            try:
                item = getattr(las.well, mnem, None)
                if item is not None and item.value not in (None, ""):
                    val = float(str(item.value).strip())
                    if np.isfinite(val):
                        return val
            except Exception:
                continue
        return None

    # WELL-X / WELL-Y (ft) take precedence; fall back to LAT / LONG /
    # LONGITUDE / LATITUDE (decimal degrees).
    well_x = _header_float("WELL_X", "WELLX", "X")
    well_y = _header_float("WELL_Y", "WELLY", "Y")
    if well_x is not None and well_y is not None:
        lng, lat = well_x, well_y
        crs = "EPSG:32650"  # UTM zone 50N (conventional Chinese onshore default)
        diagnostics.append("使用 WELL-X/WELL-Y（ft，假定 UTM 50N）作为井位")
    else:
        lat_deg = _header_float("LAT", "LATITUDE")
        lng_deg = _header_float("LONG", "LONGITUDE")
        if lat_deg is not None and lng_deg is not None:
            lng, lat = lng_deg, lat_deg
            crs = "EPSG:4326"
            diagnostics.append("使用 LAT/LONG（十进制度，WGS84）作为井位")
        else:
            diagnostics.append("缺少 WELL-X/Y/LAT/LONG 井位头；井位标记为 None")

    # Wellhead elevations + total depth (FRS §1.x): KB (kelly-bushing /
    # 补心海拔), GL (ground level / 地面海拔), STOP (max measured depth).
    # KB feeds the tvdss section datum (shift = -kb). All tolerant of absence.
    kb_m = _header_float("KB", "KBELEV", "KOELEV")
    gl_m = _header_float("GL", "GLELEV", "GRDELEV")
    max_md = _header_float("STOP", "MAXDEP", "MAXMD")

    depth_unit = (depth_curve.unit or "m").strip() or "m"
    depth_out = _freeze(depth)

    curves: list[ImportedCurve] = []
    for curve in las.curves[1:]:
        mnemonic = (curve.mnemonic or "").strip()
        if not mnemonic:
            diagnostics.append("跳过无名曲线")
            continue
        data = np.asarray(curve.data, dtype=np.float64)
        curve_depth: np.ndarray | None = None
        if data.size != depth_out.size:
            # Multi-rate (Epic A): keep the curve's own sampling instead of
            # truncating + NaN-padding onto the shared axis — its samples
            # align with the first N depth rows of the file.
            n = min(data.size, depth_out.size)
            diagnostics.append(
                f"曲线 {mnemonic} 长度 {data.size} 与深度 {depth_out.size} 不一致，"
                f"保留独立采样轴（{n} 个样本）"
            )
            data = data[:n]
            curve_depth = _freeze(depth_out[:n])
        null_val = getattr(las.well, "NULL", None)
        try:
            null_num = float(null_val.value) if null_val is not None else -999.25
        except Exception:
            null_num = -999.25
        null_mask = ~np.isfinite(data) | np.isclose(data, null_num, rtol=0.0, atol=1e-3)
        values = data.copy()
        values[null_mask] = np.nan
        curves.append(
            ImportedCurve(
                mnemonic=mnemonic,
                unit=(curve.unit or "").strip(),
                values=_freeze(values),
                null_mask=np.ascontiguousarray(null_mask, dtype=bool),
                depth=curve_depth,
            )
        )

    if not curves:
        raise LasImportError("LAS 仅有深度轴，没有可导入的测井曲线")

    return ImportedWellDocument(
        document_id=str(uuid.uuid4()),
        well_name=well_name,
        source_path="",
        depth=depth_out,
        depth_unit=depth_unit,
        curves=curves,
        diagnostics=diagnostics,
        lng=lng,
        lat=lat,
        crs=crs,
        kb_m=kb_m,
        gl_m=gl_m,
        max_md=max_md,
    )


def import_las_into_workspace(
    workspace: Workspace,
    las_path: Path | str,
    *,
    well_name: str | None = None,
) -> LasImportResult:
    """Copy LAS into ``wells/``, update catalog, return loaded document."""
    src = Path(las_path).expanduser().resolve()
    document = parse_las_file(src)
    if well_name:
        document.well_name = well_name

    well_id = str(uuid.uuid4())
    dest_dir = workspace.wells_dir / well_id
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest_name = src.name if src.suffix.lower() == ".las" else f"{src.name}.las"
    dest = dest_dir / dest_name
    try:
        shutil.copy2(src, dest)
    except OSError as exc:
        raise LasImportError(f"无法复制 LAS 到工区: {exc}") from exc

    rel = dest.relative_to(workspace.root).as_posix()
    document.source_path = rel
    # Stable catalog id == folder id; document_id separate for engine-facing id
    document.document_id = well_id

    add_well(
        workspace,
        name=document.well_name,
        path=rel,
        well_id=well_id,
        lng=document.lng,
        lat=document.lat,
        crs=document.crs,
        kb_m=document.kb_m,
        gl_m=document.gl_m,
        max_md=document.max_md,
    )
    return LasImportResult(catalog_well_id=well_id, document=document)
