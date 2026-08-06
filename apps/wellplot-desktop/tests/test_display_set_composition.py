"""T1: Display-set composition seam (dual-layer pure logic).

Seam under test: ``default_checks`` + ``compose`` on
``well_log_workstation.display_set`` — no UI, no HostPresentation binding.
"""

from __future__ import annotations

from well_log_workstation.display_set import (
    DEFAULT_DISPLAY_MAX,
    DisplayableTrackLeaf,
    StyleSource,
    compose,
    default_checks,
)
from well_log_workstation.template_model import PlotTemplate, get_builtin_template


def _leaf(
    leaf_id: str,
    mnemonic: str,
    *,
    source_id: str = "src-a",
    label: str | None = None,
) -> DisplayableTrackLeaf:
    return DisplayableTrackLeaf(
        id=leaf_id,
        mnemonic=mnemonic,
        source_id=source_id,
        label=label if label is not None else mnemonic,
    )


def _a2_like_leaves() -> list[DisplayableTrackLeaf]:
    """Multi-curve source: GR/RT/DEN + extras including independent AT* leaves."""
    return [
        _leaf("a2:GR", "GR"),
        _leaf("a2:CAL", "CAL"),
        _leaf("a2:RT", "RT"),
        _leaf("a2:AT10", "AT10"),
        _leaf("a2:AT20", "AT20"),
        _leaf("a2:AT90", "AT90"),
        _leaf("a2:DEN", "DEN"),
        _leaf("a2:CNL", "CNL"),
    ]


def _std_template() -> PlotTemplate:
    tpl = get_builtin_template("std-gr-rt-den")
    assert tpl is not None
    return tpl


def test_default_checks_includes_template_then_preferred() -> None:
    leaves = _a2_like_leaves()
    checks = default_checks(leaves, _std_template())
    # Template slots first: GR / RT / DEN; then preferred fill (CAL, AT*, CNL)
    assert {"a2:GR", "a2:RT", "a2:DEN"}.issubset(checks)
    assert "a2:CAL" in checks
    assert len(checks) <= DEFAULT_DISPLAY_MAX
    assert len(checks) == len(leaves)  # only 8 leaves here, under cap


def test_default_checks_caps_at_ten() -> None:
    leaves = [_leaf(f"x:C{i}", f"C{i}") for i in range(25)]
    # Also sprinkle common names so priority path is exercised
    leaves[0] = _leaf("x:GR", "GR")
    leaves[1] = _leaf("x:RT", "RT")
    leaves[2] = _leaf("x:RHOB", "RHOB")
    checks = default_checks(leaves, _std_template())
    assert len(checks) == DEFAULT_DISPLAY_MAX
    assert "x:GR" in checks
    assert "x:RT" in checks
    assert "x:RHOB" in checks


def test_default_checks_empty_leaves() -> None:
    assert default_checks([], _std_template()) == frozenset()


def test_default_checks_max_tracks_override() -> None:
    leaves = _a2_like_leaves()
    checks = default_checks(leaves, _std_template(), max_tracks=2)
    assert len(checks) == 2
    # Template order: GR then RT
    assert checks == frozenset({"a2:GR", "a2:RT"})


def test_compose_empty_display_set_yields_empty_list() -> None:
    tracks = compose(_a2_like_leaves(), frozenset(), _std_template())
    assert tracks == []


def test_compose_matched_leaves_use_template_style_in_template_order() -> None:
    leaves = _a2_like_leaves()
    checks = frozenset({"a2:DEN", "a2:GR", "a2:RT"})  # scrambled set
    tracks = compose(leaves, checks, _std_template())
    assert [t.leaf_id for t in tracks] == ["a2:GR", "a2:RT", "a2:DEN"]
    assert all(t.style_source == StyleSource.TEMPLATE for t in tracks)
    assert tracks[0].template_slot_id == "gr"
    assert tracks[1].template_slot_id == "rt"
    assert tracks[2].template_slot_id == "den"
    # Template style hints present
    assert tracks[0].color  # non-empty
    assert tracks[0].width_fraction > 0
    assert tracks[0].scale is not None
    assert tracks[1].scale is not None
    assert tracks[1].scale.mode == "log"  # RT slot is log in std template


def test_compose_unmatched_checked_leaf_gets_default_style() -> None:
    leaves = _a2_like_leaves()
    checks = frozenset({"a2:GR", "a2:CAL"})
    tracks = compose(leaves, checks, _std_template())
    by_id = {t.leaf_id: t for t in tracks}
    assert by_id["a2:GR"].style_source == StyleSource.TEMPLATE
    assert by_id["a2:CAL"].style_source == StyleSource.DEFAULT
    assert by_id["a2:CAL"].template_slot_id is None
    # Matched first (template order), then unmatched in leaf order
    assert [t.leaf_id for t in tracks] == ["a2:GR", "a2:CAL"]


def test_compose_includes_every_checked_leaf_even_if_template_cannot_match() -> None:
    leaves = _a2_like_leaves()
    # AT10/AT20/CNL have no std slot; AT90 is an RT-slot alias in std template
    # so when RT is unchecked, AT90 may consume the RT slot (template style).
    checks = frozenset({"a2:AT10", "a2:AT20", "a2:AT90", "a2:CNL"})
    tracks = compose(leaves, checks, _std_template())
    assert {t.leaf_id for t in tracks} == checks
    by_id = {t.leaf_id: t for t in tracks}
    assert by_id["a2:AT10"].style_source == StyleSource.DEFAULT
    assert by_id["a2:AT20"].style_source == StyleSource.DEFAULT
    assert by_id["a2:CNL"].style_source == StyleSource.DEFAULT
    assert by_id["a2:AT90"].style_source == StyleSource.TEMPLATE
    assert by_id["a2:AT90"].template_slot_id == "rt"
    # Template-matched first (RT slot), then remaining defaults in leaf order
    assert [t.leaf_id for t in tracks] == [
        "a2:AT90",
        "a2:AT10",
        "a2:AT20",
        "a2:CNL",
    ]


def test_compose_ignores_unknown_ids_in_display_set() -> None:
    leaves = _a2_like_leaves()
    tracks = compose(leaves, frozenset({"a2:GR", "ghost"}), _std_template())
    assert [t.leaf_id for t in tracks] == ["a2:GR"]


def test_compose_template_switch_same_display_set_changes_styles_only() -> None:
    leaves = _a2_like_leaves()
    display_set = frozenset({"a2:GR", "a2:CAL"})
    std = compose(leaves, display_set, _std_template())
    gr_only = get_builtin_template("gr-only")
    assert gr_only is not None
    only = compose(leaves, display_set, gr_only)
    # Same membership / leaf ids regardless of template
    assert {t.leaf_id for t in std} == display_set
    assert {t.leaf_id for t in only} == display_set
    # GR still template-styled under gr-only; CAL still default
    only_by = {t.leaf_id: t for t in only}
    assert only_by["a2:GR"].style_source == StyleSource.TEMPLATE
    assert only_by["a2:CAL"].style_source == StyleSource.DEFAULT


def test_at_leaves_are_independent_for_default_and_compose() -> None:
    """AT* are separate leaves — RT slot prefers RT; AT10 may fill as preferred extra."""
    leaves = _a2_like_leaves()
    checks = default_checks(leaves, _std_template())
    assert "a2:RT" in checks
    # Explicitly check AT10 only (independent leaf, not grouped with RT)
    tracks = compose(leaves, frozenset({"a2:AT10"}), _std_template())
    assert len(tracks) == 1
    assert tracks[0].leaf_id == "a2:AT10"
    assert tracks[0].style_source == StyleSource.DEFAULT


def test_single_source_single_leaf_still_composes() -> None:
    leaves = [_leaf("extra:GR", "GR", source_id="src-extra", label="GR_extra")]
    checks = default_checks(leaves, _std_template())
    assert checks == frozenset({"extra:GR"})
    tracks = compose(leaves, checks, _std_template())
    assert len(tracks) == 1
    assert tracks[0].title in ("GR", "GR_extra")
    assert tracks[0].style_source == StyleSource.TEMPLATE


def test_mnemonic_alias_match_case_insensitive() -> None:
    leaves = [_leaf("x:gamma", "GAMMA")]
    checks = default_checks(leaves, _std_template())
    assert checks == frozenset({"x:gamma"})
    tracks = compose(leaves, checks, _std_template())
    assert tracks[0].style_source == StyleSource.TEMPLATE
    assert tracks[0].template_slot_id == "gr"
