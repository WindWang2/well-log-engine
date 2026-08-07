"""Virtualized table projection for single-well Display Set (T4 / #344, ADR 0022).

On-demand cell access only — never materializes a full-length wide float grid
for the screen path. Columns = Depth + checked curve leaves (shared depth axis).

## Performance budgets (design §8 / T6 #346)

Reference workstation (ADR 0014 spirit):

| Path | Target |
|------|--------|
| Steady scroll | P95 ≤ 16.7 ms |
| Check / Graphic↔Table | P95 ≤ 100 ms |
| First enter table | P95 ≤ 300 ms first paint; ≤1 s visible feedback |

Acceptance anchors: A2-scale (~1.5×10⁴ rows × ≥20 cols) and stress ≥1×10⁵ rows × 20 cols.

**Hot path:** ``LogTableModel.data()`` / ``TableProjection.cell()`` only.
**Not on hot path:** full-table export, clipboard of non-selection, PDF/XLSX writers.

Export is a separate job API (``export_projection_rows`` below or host export menus);
failures there must not clear an open table view. Clipboard materializes **selected
rows only** (``selection_tsv`` / ``selection_html``).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Sequence

import numpy as np
from PySide6.QtCore import QAbstractTableModel, QModelIndex, Qt

from well_log_workstation.display_set import (
    StyledTrackDescriptor,
    compose,
    leaves_from_document,
)
from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument
from well_log_workstation.template_model import PlotTemplate

# Soft product tip threshold (incl. Depth column) — design §8 / #337.
SOFT_COLUMN_TIP_THRESHOLD = 64


@dataclass(frozen=True, slots=True)
class CurveColumn:
    """One curve column: identity + array views (no copy)."""

    leaf_id: str
    mnemonic: str
    title: str
    values: np.ndarray
    null_mask: np.ndarray


@dataclass(frozen=True, slots=True)
class TableProjection:
    """Logical table on one sampling axis (first-ship: document MD)."""

    axis_id: str
    depth: np.ndarray
    depth_unit: str
    columns: tuple[CurveColumn, ...]

    @property
    def row_count(self) -> int:
        return int(self.depth.size)

    @property
    def column_count(self) -> int:
        """Depth + curve columns."""
        return 1 + len(self.columns)

    @property
    def needs_soft_column_tip(self) -> bool:
        return self.column_count >= SOFT_COLUMN_TIP_THRESHOLD

    def header(self, section: int) -> str:
        if section <= 0:
            unit = self.depth_unit or "m"
            return f"Depth ({unit})" if unit else "Depth"
        col = self.columns[section - 1]
        return col.title or col.mnemonic

    def cell(self, row: int, col: int) -> float | None:
        """On-demand sample; None means null / missing."""
        if row < 0 or row >= self.row_count:
            return None
        if col == 0:
            v = float(self.depth[row])
            return v if np.isfinite(v) else None
        cidx = col - 1
        if cidx < 0 or cidx >= len(self.columns):
            return None
        curve = self.columns[cidx]
        if row >= curve.values.size:
            return None
        if curve.null_mask[row]:
            return None
        v = float(curve.values[row])
        return v if np.isfinite(v) else None


def build_table_projections(
    document: ImportedWellDocument,
    display_set: set[str] | frozenset[str],
    template: PlotTemplate,
) -> list[TableProjection]:
    """Build projection(s) for the Display Set.

    Same sampling axis (shared document depth) → one wide table.
    Curves whose length differs from the depth axis are placed on a **split**
    projection keyed by length (no implicit resample).
    """
    leaves = leaves_from_document(document)
    styled: list[StyledTrackDescriptor] = compose(leaves, display_set, template)
    if not styled:
        return [
            TableProjection(
                axis_id="md",
                depth=np.asarray(document.depth, dtype=np.float64),
                depth_unit=document.depth_unit or "m",
                columns=(),
            )
        ]

    # Group by value length vs depth length for split tables (ADR 0022 spirit).
    # Multi-rate (Epic A): a curve with its own sampling axis gets its own
    # table carrying its REAL depth — never merged into the shared-axis table
    # (no implicit alignment across axes).
    depth = np.asarray(document.depth, dtype=np.float64)
    groups: dict[int, list[CurveColumn]] = {}
    per_axis_tables: list[TableProjection] = []
    for desc in styled:
        curve = document.curve_by_mnemonic(desc.mnemonic)
        if curve is None:
            continue
        n = int(curve.values.size)
        own_depth = getattr(curve, "depth", None)
        if own_depth is not None:
            per_axis_tables.append(
                TableProjection(
                    axis_id=f"curve-{curve.mnemonic}",
                    depth=np.asarray(own_depth, dtype=np.float64),
                    depth_unit=document.depth_unit or "m",
                    columns=(
                        CurveColumn(
                            leaf_id=desc.leaf_id,
                            mnemonic=curve.mnemonic,
                            title=desc.title,
                            values=curve.values,
                            null_mask=curve.null_mask,
                        ),
                    ),
                )
            )
            continue
        groups.setdefault(n, []).append(
            CurveColumn(
                leaf_id=desc.leaf_id,
                mnemonic=curve.mnemonic,
                title=desc.title,
                values=curve.values,
                null_mask=curve.null_mask,
            )
        )

    if not groups:
        return per_axis_tables or [
            TableProjection(
                axis_id="md",
                depth=depth,
                depth_unit=document.depth_unit or "m",
                columns=(),
            )
        ]

    out: list[TableProjection] = list(per_axis_tables)
    for n, cols in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        if n == depth.size:
            axis_depth = depth
            axis_id = "md"
        else:
            # Independent index axis — no resample onto MD
            axis_depth = np.arange(n, dtype=np.float64)
            axis_id = f"len-{n}"
        out.append(
            TableProjection(
                axis_id=axis_id,
                depth=axis_depth,
                depth_unit=document.depth_unit or "m" if n == depth.size else "idx",
                columns=tuple(cols),
            )
        )
    return out


class LogTableModel(QAbstractTableModel):
    """Qt model over a TableProjection — virtualized via data() only."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._proj: TableProjection | None = None

    def set_projection(self, projection: TableProjection | None) -> None:
        self.beginResetModel()
        self._proj = projection
        self.endResetModel()

    def projection(self) -> TableProjection | None:
        return self._proj

    def rowCount(self, parent: QModelIndex = QModelIndex()) -> int:  # noqa: N802
        if parent.isValid() or self._proj is None:
            return 0
        return self._proj.row_count

    def columnCount(self, parent: QModelIndex = QModelIndex()) -> int:  # noqa: N802
        if parent.isValid() or self._proj is None:
            return 0
        return self._proj.column_count

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole) -> Any:
        if not index.isValid() or self._proj is None:
            return None
        if role not in (
            Qt.ItemDataRole.DisplayRole,
            Qt.ItemDataRole.ToolTipRole,
            Qt.ItemDataRole.EditRole,
        ):
            return None
        val = self._proj.cell(index.row(), index.column())
        if val is None:
            return "" if role == Qt.ItemDataRole.DisplayRole else None
        if index.column() == 0:
            return f"{val:.4f}" if role == Qt.ItemDataRole.DisplayRole else val
        return f"{val:.4g}" if role == Qt.ItemDataRole.DisplayRole else val

    def headerData(  # noqa: N802
        self,
        section: int,
        orientation: Qt.Orientation,
        role: int = Qt.ItemDataRole.DisplayRole,
    ) -> Any:
        if role != Qt.ItemDataRole.DisplayRole or self._proj is None:
            return None
        if orientation == Qt.Orientation.Horizontal:
            return self._proj.header(section)
        return str(section + 1)

    def materialize_full_grid(self) -> np.ndarray:
        """Test-only helper — must not be used by the UI hot path.

        Allocates a dense grid for contract tests that assert the model itself
        does not keep such a buffer as state.
        """
        if self._proj is None:
            return np.zeros((0, 0), dtype=np.float64)
        r, c = self._proj.row_count, self._proj.column_count
        grid = np.full((r, c), np.nan, dtype=np.float64)
        for i in range(r):
            for j in range(c):
                v = self._proj.cell(i, j)
                if v is not None:
                    grid[i, j] = v
        return grid


def selection_tsv(projection: TableProjection, rows: Sequence[int]) -> str:
    """Materialize **selected rows only** as TSV (clipboard path)."""
    if not rows:
        return ""
    headers = [projection.header(c) for c in range(projection.column_count)]
    lines = ["\t".join(headers)]
    for r in rows:
        if r < 0 or r >= projection.row_count:
            continue
        cells: list[str] = []
        for c in range(projection.column_count):
            v = projection.cell(r, c)
            cells.append("" if v is None else f"{v:.6g}")
        lines.append("\t".join(cells))
    return "\n".join(lines) + ("\n" if len(lines) > 1 else "")


def selection_html(projection: TableProjection, rows: Sequence[int]) -> str:
    """Materialize **selected rows only** as a simple HTML table."""
    if not rows:
        return ""
    parts = ["<table border='1'><thead><tr>"]
    for c in range(projection.column_count):
        parts.append(f"<th>{projection.header(c)}</th>")
    parts.append("</tr></thead><tbody>")
    for r in rows:
        if r < 0 or r >= projection.row_count:
            continue
        parts.append("<tr>")
        for c in range(projection.column_count):
            v = projection.cell(r, c)
            parts.append(f"<td>{'' if v is None else f'{v:.6g}'}</td>")
        parts.append("</tr>")
    parts.append("</tbody></table>")
    return "".join(parts)


def export_projection_rows(
    projection: TableProjection,
    *,
    row_start: int = 0,
    row_end: int | None = None,
    cancel_flag: list[bool] | None = None,
) -> list[list[float | None]]:
    """Separate **export job** path — not for scroll/data() hot path.

    Materializes a row range into Python lists for writers (CSV/XLSX).
    ``cancel_flag`` is a single-element list ``[False]``; set ``[True]`` to abort.
    Does not touch UI state; callers handle progress.
    """
    end = projection.row_count if row_end is None else min(row_end, projection.row_count)
    start = max(0, row_start)
    out: list[list[float | None]] = []
    for r in range(start, end):
        if cancel_flag is not None and cancel_flag and cancel_flag[0]:
            break
        out.append(
            [projection.cell(r, c) for c in range(projection.column_count)]
        )
    return out


# Test hook: projection build may consult this for injected latency/failure.
# Production always uses defaults (no delay, no force-fail).
class ProjectionBuildHooks:
    """Injectable hooks for T6 cancel/failure tests (not for production UI)."""

    delay_steps: int = 0
    force_fail: bool = False
    cancel_flag: list[bool] | None = None

    def reset(self) -> None:
        self.delay_steps = 0
        self.force_fail = False
        self.cancel_flag = None


PROJECTION_BUILD_HOOKS = ProjectionBuildHooks()


def build_table_projections_guarded(
    document: ImportedWellDocument,
    display_set: set[str] | frozenset[str],
    template: PlotTemplate,
    *,
    hooks: ProjectionBuildHooks | None = None,
    on_progress: Any | None = None,
) -> list[TableProjection]:
    """Like ``build_table_projections`` with cancel/progress/failure hooks (T6)."""
    h = hooks if hooks is not None else PROJECTION_BUILD_HOOKS
    if h.force_fail:
        raise RuntimeError("表格投影构建失败（注入/错误）")
    steps = max(0, int(h.delay_steps))
    for i in range(steps):
        if h.cancel_flag is not None and h.cancel_flag and h.cancel_flag[0]:
            raise InterruptedError("表格投影构建已取消")
        if on_progress is not None:
            on_progress(i + 1, steps)
    return build_table_projections(document, display_set, template)
