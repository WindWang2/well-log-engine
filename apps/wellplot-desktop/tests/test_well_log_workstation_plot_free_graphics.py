"""Composite free-graphics persistence (schema v4) tests.

Task 11 of the free-graphics host work: ``PlotDocument.free_graphics`` is a
list of raw shape dicts (``{"kind": ..., "geometry": ...}``) placed directly
on a composite paper, independent of panels/templates. The schema bumps
v3 -> v4; v4 is additive so pre-v4 files load with ``free_graphics == []``.

Import layout mirrors the codebase's lazy-import rule: these tests exercise
only the pure ``_from_json``/``_to_json`` pair, so the whole module is
importable and verifiable with plain ``/usr/bin/python3`` (no PySide6; the
plot_document schema/persistence module imports ``events`` lazily on purpose).
"""

from __future__ import annotations


def _shape() -> dict:
    """One free-graphics shape dict, matching the shape the editor persists."""
    return {"kind": "rect", "geometry": {"x": 1.0, "y": 1.0, "w": 2.0, "h": 2.0}}


def test_schema_version_current() -> None:
    """Current plot schema is v5 (track_overrides / correlation layout fields)."""
    from well_log_workstation.plot_document import PLOT_SCHEMA_VERSION

    assert PLOT_SCHEMA_VERSION == 5


def test_v3_upgrades_to_v4_with_empty_free_graphics() -> None:
    """A v3 composite file (no free_graphics) loads with an empty list."""
    from well_log_workstation.plot_document import _from_json

    d = _from_json(
        {
            "schemaVersion": 3,
            "id": "x",
            "name": "X",
            "type": "composite",
            "well_ids": [],
            "template_id": None,
            "revision": 1,
        },
        path="plots/x.json",
    )
    assert d.free_graphics == []


def test_v3_file_with_existing_free_graphics_is_parsed() -> None:
    """A hand-edited v3 file that already carries free_graphics keeps them."""
    from well_log_workstation.plot_document import _from_json

    shape = _shape()
    d = _from_json(
        {
            "schemaVersion": 3,
            "id": "x",
            "name": "X",
            "type": "composite",
            "well_ids": [],
            "template_id": None,
            "revision": 2,
            "free_graphics": [shape],
        },
        path="plots/x.json",
    )
    assert d.free_graphics == [shape]


def test_v1_and_v2_chain_upgrade_to_v4() -> None:
    """v1/v2 files chain through the additive upgrades with empty free_graphics."""
    from well_log_workstation.plot_document import _from_json

    for version in (1, 2):
        data: dict = {
            "schemaVersion": version,
            "id": "legacy",
            "name": "Legacy",
            "type": "single_well",
            "well_ids": ["w-a"],
            "template_id": "t",
        }
        d = _from_json(data, path="plots/legacy.json")
        assert d.free_graphics == []
        assert d.revision == 0
        assert d.panels == []  # v1 -> v2 still applies before the v3/v4 chain


def test_to_json_emits_free_graphics_for_composite() -> None:
    """Composite docs persist free_graphics and are stamped schema v4."""
    from well_log_workstation.plot_document import (
        PLOT_SCHEMA_VERSION,
        PlotDocument,
        _to_json,
    )

    shape = _shape()
    doc = PlotDocument(
        id="c1",
        name="C",
        type="composite",
        well_ids=[],
        template_id=None,
        path="plots/c1.json",
        free_graphics=[shape],
    )
    payload = _to_json(doc)
    assert payload["schemaVersion"] == PLOT_SCHEMA_VERSION == 5
    assert payload["free_graphics"] == [shape]


def test_to_json_omits_free_graphics_for_plain_single_well() -> None:
    """Non-composite docs without shapes do not emit the field at all."""
    from well_log_workstation.plot_document import PlotDocument, _to_json

    doc = PlotDocument(
        id="s1",
        name="S",
        type="single_well",
        well_ids=["w"],
        template_id="t",
        path="plots/s1.json",
    )
    assert "free_graphics" not in _to_json(doc)


def test_to_json_persists_free_graphics_even_on_non_composite() -> None:
    """The ``or doc.free_graphics`` branch keeps stale shapes from dropping."""
    from well_log_workstation.plot_document import PlotDocument, _to_json

    shape = _shape()
    doc = PlotDocument(
        id="s2",
        name="S",
        type="single_well",
        well_ids=["w"],
        template_id="t",
        path="plots/s2.json",
        free_graphics=[shape],
    )
    assert _to_json(doc)["free_graphics"] == [shape]


def test_free_graphics_round_trip_and_list_copy() -> None:
    """Shapes survive _to_json -> _from_json and the payload is a copy."""
    from well_log_workstation.plot_document import PlotDocument, _from_json, _to_json

    shape = _shape()
    doc = PlotDocument(
        id="c2",
        name="C",
        type="composite",
        well_ids=[],
        template_id=None,
        path="plots/c2.json",
        free_graphics=[shape],
    )
    payload = _to_json(doc)
    again = _from_json(payload, path="plots/c2.json")
    assert again.free_graphics == [shape]
    # Mutating the serialized payload must not alias the doc's own list.
    payload["free_graphics"].clear()
    assert doc.free_graphics == [shape]


def test_non_dict_free_graphics_entries_are_filtered() -> None:
    """Only dict shapes are loaded; junk entries are skipped."""
    from well_log_workstation.plot_document import _from_json

    shape = _shape()
    d = _from_json(
        {
            "schemaVersion": 4,
            "id": "x",
            "name": "X",
            "type": "composite",
            "well_ids": [],
            "template_id": None,
            "revision": 1,
            "free_graphics": [shape, "junk", 42, None],
        },
        path="plots/x.json",
    )
    assert d.free_graphics == [shape]
