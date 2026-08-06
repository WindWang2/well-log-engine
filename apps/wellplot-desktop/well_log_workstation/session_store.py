"""Host session store for imported wells (#218).

Holds loaded documents until full Python WellLogSession bindings exist.
Curves are readable via ``get_document`` / ``sample_value`` (engine-like access).
"""

from __future__ import annotations

from pathlib import Path

from well_log_workstation.las_import import ImportedWellDocument, LasImportError, parse_las_file
from well_log_workstation.workspace import Workspace, WorkspaceError


class HostSessionStore:
    """In-memory well documents keyed by catalog / document id."""

    def __init__(self) -> None:
        self._docs: dict[str, ImportedWellDocument] = {}

    def put(self, document: ImportedWellDocument) -> None:
        self._docs[document.document_id] = document

    def get(self, document_id: str) -> ImportedWellDocument | None:
        return self._docs.get(document_id)

    def remove(self, document_id: str) -> None:
        self._docs.pop(document_id, None)

    def clear(self) -> None:
        self._docs.clear()

    def document_ids(self) -> list[str]:
        return list(self._docs.keys())

    def sample_value(
        self, document_id: str, mnemonic: str, index: int
    ) -> float | None:
        doc = self.get(document_id)
        if doc is None:
            return None
        return doc.sample_value(mnemonic, index)

    def ensure_well_loaded(
        self, workspace: Workspace, well_id: str
    ) -> ImportedWellDocument:
        """Return session document, reloading from workspace LAS path if needed."""
        existing = self.get(well_id)
        if existing is not None:
            return existing
        entry = next((w for w in workspace.wells if w.id == well_id), None)
        if entry is None:
            raise WorkspaceError("井不在工区目录中")
        if not entry.path:
            raise WorkspaceError("井没有数据路径，无法重新加载")
        abs_path = workspace.root / entry.path
        if not abs_path.is_file():
            raise WorkspaceError(f"井数据文件缺失: {entry.path}")
        try:
            doc = parse_las_file(abs_path)
        except LasImportError as exc:
            raise WorkspaceError(str(exc)) from exc
        # Keep catalog id as document id for stable joins with plot docs.
        doc.document_id = well_id
        doc.well_name = entry.name or doc.well_name
        doc.source_path = entry.path
        self.put(doc)
        return doc
