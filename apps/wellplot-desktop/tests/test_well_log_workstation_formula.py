"""Formula calculator + derived curves (FRS §2.4 / P2-A).

Covers:
* parser: precedence, parens, right-assoc power, unary minus vs power,
  function nesting, syntax errors, unknown variables;
* evaluator: VSH example, constant broadcast, null propagation,
  case-insensitive variables;
* storage: formulas.json round-trip + missing-file empty;
* integration: _apply_derived_curves attaches derived tracks with correct
  values; canvas render smoke.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from well_log_workstation.formula import (
    Formula,
    FormulaError,
    evaluate,
    evaluate_expression,
    parse_expression,
)


def _scalar(expr: str, context: dict | None = None) -> float:
    values, _ = evaluate_expression(expr, context or {})
    return float(values[0])


# ---------------------------------------------------------------------------
# Parser: precedence & associativity
# ---------------------------------------------------------------------------


def test_precedence_mul_before_add() -> None:
    assert _scalar("1+2*3") == pytest.approx(7.0)


def test_parens_override_precedence() -> None:
    assert _scalar("(1+2)*3") == pytest.approx(9.0)


def test_power_right_associative() -> None:
    assert _scalar("2^3^2") == pytest.approx(512.0)


def test_unary_minus_binds_looser_than_power() -> None:
    # -2^2 = -(2^2) = -4 (mathematical convention).
    assert _scalar("-2^2") == pytest.approx(-4.0)


def test_negative_exponent() -> None:
    assert _scalar("2^-3") == pytest.approx(0.125)


def test_nested_function_call() -> None:
    assert _scalar("sqrt(max(4, 9))") == pytest.approx(3.0)
    assert _scalar("log10(1000)") == pytest.approx(3.0)
    assert _scalar("min(3, 7) * 2") == pytest.approx(6.0)


def test_division_and_float_literals() -> None:
    assert _scalar("7 / 2") == pytest.approx(3.5)
    assert _scalar(".5 + 1e1") == pytest.approx(10.5)


def test_whitespace_tolerated() -> None:
    assert _scalar("  1  +  2  ") == pytest.approx(3.0)


# ---------------------------------------------------------------------------
# Parser: errors
# ---------------------------------------------------------------------------

_PARSE_ERRORS = ["1+", "(1", "1 2", "a @ b", "1 + * 2", "sqrt(1", "max(1)", "()"]


@pytest.mark.parametrize("expr", _PARSE_ERRORS)
def test_syntax_errors_raise(expr: str) -> None:
    with pytest.raises(FormulaError):
        parse_expression(expr)


def test_empty_expression_raises() -> None:
    with pytest.raises(FormulaError):
        parse_expression("")
    with pytest.raises(FormulaError):
        parse_expression("   ")


# ---------------------------------------------------------------------------
# Evaluator
# ---------------------------------------------------------------------------


def test_vsh_example() -> None:
    """VSH = (GR - 25) / (150 - 25): GR 25/100/150 → 0/0.6/1."""
    gr = np.array([25.0, 100.0, 150.0])
    values, mask = evaluate_expression("(GR - 25) / (150 - 25)", {"GR": gr})
    np.testing.assert_allclose(values, [0.0, 0.6, 1.0])
    assert not mask.any()


def test_null_propagates() -> None:
    gr = np.array([25.0, np.nan, 150.0])
    values, mask = evaluate_expression("(GR - 25) / (150 - 25)", {"GR": gr})
    assert mask.tolist() == [False, True, False]
    assert np.isnan(values[1])


def test_null_mask_input_propagates() -> None:
    gr = np.array([25.0, 100.0, 150.0])
    nulls = np.array([True, False, False])
    values, mask = evaluate_expression(
        "GR * 2", {"GR": gr}, {"GR": nulls}
    )
    assert mask.tolist() == [True, False, False]


def test_case_insensitive_variable() -> None:
    values, _ = evaluate_expression("gr * 2", {"GR": np.array([3.0])})
    assert values[0] == pytest.approx(6.0)


def test_constant_broadcast() -> None:
    values, _ = evaluate_expression("GR + 100", {"GR": np.array([1.0, 2.0])})
    np.testing.assert_allclose(values, [101.0, 102.0])


def test_unknown_variable_raises() -> None:
    with pytest.raises(FormulaError, match="未知曲线"):
        evaluate_expression("NOPE + 1", {"GR": np.array([1.0])})


def test_two_curve_expression() -> None:
    context = {"GR": np.array([10.0, 20.0]), "RT": np.array([100.0, 200.0])}
    values, _ = evaluate_expression("GR / RT", context)
    np.testing.assert_allclose(values, [0.1, 0.1])


def test_length_mismatch_truncates() -> None:
    context = {"A": np.array([1.0, 2.0, 3.0]), "B": np.array([1.0, 2.0])}
    values, _ = evaluate_expression("A + B", context)
    assert values.size == 2


def test_evaluate_with_ast() -> None:
    node = parse_expression("GR * 2")
    values, _ = evaluate(node, {"GR": np.array([4.0])})
    assert values[0] == pytest.approx(8.0)


# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------


def test_formula_storage_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.tops_model import (
        load_formulas_for_well,
        save_formulas_for_well,
    )
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="Formula")
    well = add_well(ws, name="W-1", path="wells/w1.las")
    formulas = [
        Formula("VSH", "(GR - 25) / (150 - 25)"),
        Formula("RT_LOG", "log10(RT)"),
    ]
    save_formulas_for_well(ws, well.id, formulas)

    loaded, diags = load_formulas_for_well(ws, well.id)
    assert diags == []
    assert loaded == formulas


def test_formula_storage_missing_returns_empty(tmp_path: Path) -> None:
    from well_log_workstation.tops_model import load_formulas_for_well
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws2", name="NoFormula")
    well = add_well(ws, name="W-2", path="wells/w2.las")
    loaded, diags = load_formulas_for_well(ws, well.id)
    assert loaded == []
    assert diags == []


# ---------------------------------------------------------------------------
# Integration: derived curves on the presentation
# ---------------------------------------------------------------------------


def test_apply_derived_curves_attaches_track(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import save_formulas_for_well
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="Derived")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    las = tmp_path / "d.las"
    las.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1003.0
STEP.M 1.0
NULL. -999.25
WELL. D
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 25
1001 100
1002 150
1003 150
""",
        encoding="utf-8",
    )
    well_id = win.import_las_path(las)
    save_formulas_for_well(
        ws, well_id, [Formula("VSH", "(GR - 25) / (150 - 25)")]
    )
    # Build the single-well presentation (import alone has no tracks).
    win.apply_template_to_well(well_id, "gr-only")
    win._selected_well_id = well_id
    applied, diags = win._apply_derived_curves()
    assert applied == 1
    assert diags == []
    assert win._presentation is not None

    derived = [t for t in win._presentation.tracks if t.id == "derived-VSH"]
    assert len(derived) == 1
    layer = derived[0].layers[0]
    assert layer.mnemonic == "VSH"
    assert layer.color == "#8b5cf6"
    np.testing.assert_allclose(layer.values, [0.0, 0.6, 1.0, 1.0], atol=1e-9)


def test_apply_derived_curves_replaces_previous(qtbot, tmp_path: Path) -> None:
    """Re-applying must not duplicate derived tracks."""
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import save_formulas_for_well
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="Derived2")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    las = tmp_path / "d2.las"
    las.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1001.0
STEP.M 1.0
NULL. -999.25
WELL. D2
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 50
1001 100
""",
        encoding="utf-8",
    )
    well_id = win.import_las_path(las)
    save_formulas_for_well(
        ws, well_id, [Formula("VSH", "(GR - 25) / (150 - 25)")]
    )
    win.apply_template_to_well(well_id, "gr-only")
    win._selected_well_id = well_id
    win._apply_derived_curves()
    n1 = sum(1 for t in win._presentation.tracks if t.id.startswith("derived-"))
    win._apply_derived_curves()
    n2 = sum(1 for t in win._presentation.tracks if t.id.startswith("derived-"))
    assert n1 == 1 and n2 == 1


def test_apply_derived_curves_bad_formula_diagnostic(qtbot, tmp_path: Path) -> None:
    from well_log_workstation.shell import WellLogWorkstationWindow
    from well_log_workstation.tops_model import save_formulas_for_well
    from well_log_workstation.workspace import create_workspace

    ws = create_workspace(tmp_path / "ws", name="BadFormula")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.set_workspace(ws)

    las = tmp_path / "b.las"
    las.write_text(
        """~VERSION INFORMATION
VERS. 2.0
WRAP. NO
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1001.0
STEP.M 1.0
NULL. -999.25
WELL. B
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 50
1001 100
""",
        encoding="utf-8",
    )
    well_id = win.import_las_path(las)
    save_formulas_for_well(
        ws, well_id, [Formula("BAD", "NOPE + 1"), Formula("OK", "GR * 2")]
    )
    win.apply_template_to_well(well_id, "gr-only")
    win._selected_well_id = well_id
    applied, diags = win._apply_derived_curves()
    assert applied == 1  # OK applied
    assert len(diags) == 1  # BAD reported
    assert "BAD" in diags[0]
    assert any(t.id == "derived-OK" for t in win._presentation.tracks)
    assert not any(t.id == "derived-BAD" for t in win._presentation.tracks)
