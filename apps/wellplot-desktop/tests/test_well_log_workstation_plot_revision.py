"""Per-plot revision persistence (schema v3, ADR 0051) tests.

The per-plot ``revision`` counter in ``events._revisions`` is in-memory; the
host persists it into ``plots/<id>.json`` (ADR 0025: persistence is the host's
job). ``save_plot_document`` bumps the counter and writes the new committed
revision (no emit); ``load_plot_document`` seeds the counter back from disk
without regressing an already-higher in-memory count; ``emit_plot_changed``
bumps on the shell create/display paths.

Import layout mirrors the codebase's lazy-import rule: tests that call
load/save transitively import ``events`` (PySide6) and run under pytest/CI;
the pure ``_from_json``/``_to_json`` case is importable and verifiable with
plain ``/usr/bin/python3`` (plot_document imports events lazily on purpose).
"""

from __future__ import annotations

import json
from pathlib import Path


def _workspace_with_well(tmp_path: Path):
    """Fresh workspace + one catalog well (same shape as existing tests)."""
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws")
    well = add_well(ws, name="W1", path="wells/w1.las", well_id="well-fixed")
    return ws, well


def test_save_persists_revision_and_schema(tmp_path: Path) -> None:
    """A freshly created plot lands on disk as current schema with revision >= 1."""
    from well_log_workstation.events import reset_revisions
    from well_log_workstation.plot_document import (
        PLOT_SCHEMA_VERSION,
        create_single_well_plot,
    )

    reset_revisions()
    ws, well = _workspace_with_well(tmp_path)
    plot = create_single_well_plot(
        ws, well_id=well.id, well_name=well.name, template_id="std-gr-rt-den"
    )
    data = json.loads((ws.root / plot.path).read_text(encoding="utf-8"))
    assert data["schemaVersion"] == PLOT_SCHEMA_VERSION == 7
    assert "revision" in data
    assert int(data["revision"]) >= 1
    assert plot.revision == data["revision"]


def test_revision_survives_restart(tmp_path: Path) -> None:
    """After a process restart the in-memory counter is re-seeded from disk."""
    from well_log_workstation.events import plot_revision, reset_revisions
    from well_log_workstation.plot_document import (
        create_single_well_plot,
        load_plot_document,
    )

    reset_revisions()
    ws, well = _workspace_with_well(tmp_path)
    plot = create_single_well_plot(
        ws, well_id=well.id, well_name=well.name, template_id="std-gr-rt-den"
    )
    file_rev = int(
        json.loads((ws.root / plot.path).read_text(encoding="utf-8"))["revision"]
    )
    assert file_rev >= 1

    reset_revisions()  # simulate process restart
    assert plot_revision(plot.id) == 0

    loaded = load_plot_document(ws, plot.id)
    assert loaded.revision == file_rev
    assert plot_revision(plot.id) == file_rev


def test_save_bumps_monotonic(tmp_path: Path) -> None:
    """Each save commits a strictly higher revision to disk (bump, no emit)."""
    from well_log_workstation.events import plot_bus, plot_revision, reset_revisions
    from well_log_workstation.plot_document import (
        create_single_well_plot,
        save_plot_document,
    )

    reset_revisions()
    ws, well = _workspace_with_well(tmp_path)
    plot = create_single_well_plot(
        ws, well_id=well.id, well_name=well.name, template_id="std-gr-rt-den"
    )
    first = int(json.loads((ws.root / plot.path).read_text(encoding="utf-8"))["revision"])

    received: list[int] = []

    def _on(plot_id: str, rev: int) -> None:
        received.append(rev)

    plot_bus.plot_changed.connect(_on)
    try:
        save_plot_document(ws, plot)
    finally:
        plot_bus.plot_changed.disconnect(_on)
    assert received == []  # save bumps but does not emit (spec item 9)

    second = int(json.loads((ws.root / plot.path).read_text(encoding="utf-8"))["revision"])
    assert second > first
    assert plot.revision == second  # save mutates the doc to the committed value
    assert plot_revision(plot.id) == second  # in-memory counter matches disk


def test_restore_no_regression(tmp_path: Path) -> None:
    """Loads never pull the in-memory counter back below its current value."""
    from well_log_workstation.events import emit_plot_changed, plot_revision, reset_revisions
    from well_log_workstation.plot_document import (
        create_single_well_plot,
        load_plot_document,
    )

    reset_revisions()
    ws, well = _workspace_with_well(tmp_path)
    plot = create_single_well_plot(
        ws, well_id=well.id, well_name=well.name, template_id="std-gr-rt-den"
    )
    file_rev = int(
        json.loads((ws.root / plot.path).read_text(encoding="utf-8"))["revision"]
    )
    assert file_rev >= 1

    # Emits (depth-range changes / editor dirty hooks) push the counter above disk.
    emit_plot_changed(plot.id)
    emit_plot_changed(plot.id)
    in_memory = plot_revision(plot.id)
    assert in_memory == file_rev + 2

    load_plot_document(ws, plot.id)
    assert plot_revision(plot.id) == in_memory  # restored, never regressed


def test_load_legacy_v2_and_v1_without_revision(tmp_path: Path) -> None:
    """Pre-v3 files (no revision; v1 also without panels) load with revision 0."""
    from well_log_workstation.plot_document import load_plot_document
    from well_log_workstation.workspace import add_plot, create_workspace

    for version in (2, 1):
        ws = create_workspace(tmp_path / f"ws-legacy-v{version}")
        pid = f"legacy-v{version}"
        rel = f"plots/{pid}.json"
        (ws.root / "plots").mkdir(exist_ok=True)
        (ws.root / rel).write_text(
            json.dumps(
                {
                    "schemaVersion": version,  # v1: no panels; v2: no revision
                    "id": pid,
                    "name": "Legacy",
                    "type": "single_well",
                    "well_ids": ["w-a"],
                    "template_id": "t",
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        add_plot(
            ws,
            name="Legacy",
            plot_type="single_well",
            well_ids=["w-a"],
            path=rel,
            plot_id=pid,
        )
        loaded = load_plot_document(ws, pid)
        assert loaded.revision == 0
        assert loaded.panels == []


def test_id_mismatch_branch_preserves_revision(tmp_path: Path) -> None:
    """Catalog id differing from the file id keeps the persisted revision.

    ``load_plot_document`` rebuilds the doc with the catalog id when the file
    was renamed oddly; the rebuilt doc must carry ``revision`` through so the
    persisted count is not lost on load.
    """
    from well_log_workstation.plot_document import PLOT_SCHEMA_VERSION, load_plot_document
    from well_log_workstation.workspace import add_plot, create_workspace

    ws = create_workspace(tmp_path / "ws-mismatch")
    file_id = "file-id"
    catalog_id = "cat-id"
    rel = f"plots/{file_id}.json"
    (ws.root / "plots").mkdir(exist_ok=True)
    (ws.root / rel).write_text(
        json.dumps(
            {
                "schemaVersion": PLOT_SCHEMA_VERSION,
                "id": file_id,
                "name": "Renamed on disk",
                "type": "single_well",
                "well_ids": ["w-a"],
                "template_id": "t",
                "revision": 5,
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    add_plot(ws, name="Cat", plot_type="single_well", path=rel, plot_id=catalog_id)
    loaded = load_plot_document(ws, catalog_id)
    assert loaded.id == catalog_id
    assert loaded.revision == 5


def test_to_json_always_writes_revision_field() -> None:
    """Pure _from_json/_to_json round-trip; no events/PySide6 import.

    Kept separate from the load/save cases so it can be verified with plain
    ``/usr/bin/python3`` (plot_document lazily imports events only in save/load).
    """
    from well_log_workstation.plot_document import (
        PLOT_SCHEMA_VERSION,
        PlotDocument,
        _from_json,
        _to_json,
    )

    doc = PlotDocument(
        id="p1",
        name="P",
        type="single_well",
        well_ids=["w-a"],
        template_id="t",
        path="plots/p1.json",
        revision=4,
    )
    payload = _to_json(doc)
    assert payload["schemaVersion"] == PLOT_SCHEMA_VERSION
    assert payload["revision"] == 4

    again = _from_json(payload, path="plots/p1.json")
    assert again.revision == 4
