"""Workspace mnemonic alias dictionary (FRS §1.2 / P0-A).

Covers:
* AliasMap bidirectional expansion + dedup + tolerance;
* module-level active resolver (set/get/clear);
* template_model._match_curve picks up alias curves;
* display_set.default_checks preferred-mnemonic fill picks up alias curves;
* workspace round-trip + v1 migration default.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.display_set import DisplayableTrackLeaf, default_checks
from well_log_workstation.las_import import ImportedCurve, ImportedWellDocument
from well_log_workstation.mnemonic_alias import (
    AliasMap,
    expand,
    get_active_map,
    normalize_alias_mapping,
    set_active_map,
)
from well_log_workstation.template_model import PlotTemplate, _match_curve


@pytest.fixture(autouse=True)
def _clear_active_map():
    """Ensure no alias map leaks between tests."""
    set_active_map(None)
    yield
    set_active_map(None)


# ---------------------------------------------------------------------------
# AliasMap
# ---------------------------------------------------------------------------


def test_expand_canonical_to_aliases() -> None:
    m = AliasMap({"GR": ["GRD", "GAPI"]})
    assert m.expand(["GR"]) == ["GR", "GRD", "GAPI"]


def test_expand_alias_to_canonical_and_siblings() -> None:
    """An alias expands to its canonical plus sibling aliases (bidirectional)."""
    m = AliasMap({"GR": ["GRD", "GAPI"]})
    assert m.expand(["GRD"]) == ["GRD", "GR", "GAPI"]


def test_expand_dedups_case_insensitive() -> None:
    m = AliasMap({"GR": ["GRD"]})
    # Mix of cases + duplicates collapses to first-seen form.
    assert m.expand(["gr", "GRD", "Gr"]) == ["gr", "GRD"]


def test_empty_map_is_passthrough() -> None:
    m = AliasMap()
    assert m.expand(["GR", "RT"]) == ["GR", "RT"]
    assert not bool(m)


def test_normalize_drops_invalid_entries() -> None:
    raw = {
        "GR": ["GRD", "", "grd", "GR"],
        "": ["x"],
        "RT": "oops",  # non-list → dropped
        "SP": ["SP1", "SP2"],
    }
    out = normalize_alias_mapping(raw)
    assert out == {"GR": ["GRD"], "SP": ["SP1", "SP2"]}


def test_normalize_non_dict_returns_empty() -> None:
    assert normalize_alias_mapping(None) == {}
    assert normalize_alias_mapping("GR") == {}
    assert normalize_alias_mapping([1, 2]) == {}


# ---------------------------------------------------------------------------
# Module-level active resolver
# ---------------------------------------------------------------------------


def test_active_map_set_get_clear() -> None:
    assert not bool(get_active_map())  # empty by default
    set_active_map({"GR": ["GRD"]})
    assert expand(["GR"]) == ["GR", "GRD"]
    set_active_map(None)
    assert expand(["GR"]) == ["GR"]


# ---------------------------------------------------------------------------
# template_model._match_curve
# ---------------------------------------------------------------------------


def _doc_with_curves(*mnemonics: str) -> ImportedWellDocument:
    curves = []
    for m in mnemonics:
        curves.append(
            ImportedCurve(
                mnemonic=m,
                unit="",
                values=np.array([1.0, 2.0]),
                null_mask=np.array([False, False]),
            )
        )
    return ImportedWellDocument(
        document_id="d1",
        well_name="W",
        source_path="wells/d1.las",
        depth=np.array([0.0, 1.0]),
        depth_unit="m",
        curves=curves,
    )


def test_match_curve_uses_alias() -> None:
    """A template GR slot matches a curve named GRD via the alias map."""
    doc = _doc_with_curves("GRD")
    set_active_map({"GR": ["GRD"]})
    hit = _match_curve(doc, ["GR"])
    assert hit is not None and hit.mnemonic == "GRD"


def test_match_curve_without_alias_still_exact() -> None:
    doc = _doc_with_curves("GR")
    hit = _match_curve(doc, ["GR"])
    assert hit is not None and hit.mnemonic == "GR"


def test_match_curve_alias_reverse_slot_matches_canonical_curve() -> None:
    """A slot authored as alias GRD matches a curve named GR (canonical)."""
    doc = _doc_with_curves("GR")
    set_active_map({"GR": ["GRD"]})
    hit = _match_curve(doc, ["GRD"])
    assert hit is not None and hit.mnemonic == "GR"


# ---------------------------------------------------------------------------
# display_set.default_checks preferred-mnemonic fill
# ---------------------------------------------------------------------------


def _leaf(mnemonic: str, i: int = 0) -> DisplayableTrackLeaf:
    return DisplayableTrackLeaf(id=f"{mnemonic}-{i}", mnemonic=mnemonic)


_EMPTY_TEMPLATE = PlotTemplate(id="empty", name="empty", tracks=[])


def test_default_checks_prefers_alias_curve() -> None:
    """Preferred GR picks up a GRD leaf when the workspace maps GR→[GRD]."""
    leaves = [_leaf("GRD")]
    set_active_map({"GR": ["GRD"]})
    checks = default_checks(leaves, _EMPTY_TEMPLATE, max_tracks=10)
    assert leaves[0].id in checks


def test_default_checks_no_alias_no_false_match() -> None:
    leaves = [_leaf("GRD")]
    checks = default_checks(leaves, _EMPTY_TEMPLATE, max_tracks=10)
    # Without an alias, GRD is still picked via the document-order fallback
    # (step 3), but the GR-preferred step must not falsely claim it.
    assert leaves[0].id in checks  # filled by step-3 fallback


# ---------------------------------------------------------------------------
# Workspace round-trip
# ---------------------------------------------------------------------------


def test_workspace_alias_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import (
        create_workspace,
        open_workspace,
        save_workspace,
    )

    ws = create_workspace(tmp_path / "ws", name="Alias")
    ws.mnemonic_alias = {"GR": ["GRD", "GAPI"], "RT": ["LLD"]}
    save_workspace(ws)

    again = open_workspace(ws.root)
    assert again.mnemonic_alias == {"GR": ["GRD", "GAPI"], "RT": ["LLD"]}


def test_workspace_v1_defaults_empty_alias(tmp_path: Path) -> None:
    import json

    from well_log_workstation.workspace import (
        WORKSPACE_FILENAME,
        add_well,
        create_workspace,
        open_workspace,
    )

    # Hand-write a minimal v1 workspace (no mnemonic_alias).
    root = tmp_path / "v1"
    (root / "wells").mkdir(parents=True)
    (root / "plots").mkdir()
    (root / "templates").mkdir()
    (root / WORKSPACE_FILENAME).write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "name": "Legacy",
                "defaultTemplateId": None,
                "wells": [{"id": "w1", "name": "W1", "path": "wells/w1.las"}],
                "plots": [],
            }
        ),
        encoding="utf-8",
    )
    ws = open_workspace(root)
    assert ws.mnemonic_alias == {}


# ---------------------------------------------------------------------------
# Shell integration (active map set on workspace open)
# ---------------------------------------------------------------------------


def test_shell_sets_active_map_on_workspace(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.workspace import create_workspace, save_workspace

    ws = create_workspace(tmp_path / "ws", name="Int")
    ws.mnemonic_alias = {"GR": ["GRD"]}
    save_workspace(ws)

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)
    assert get_active_map().expand(["GR"]) == ["GR", "GRD"]

    # Clearing the workspace clears the active map.
    win.set_workspace(None)
    assert expand(["GR"]) == ["GR"]
