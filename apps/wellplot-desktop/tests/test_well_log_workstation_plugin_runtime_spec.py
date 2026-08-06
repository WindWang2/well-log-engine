"""T17 / #305 — plugin Runtime spec gate + first-ship extension catalogue."""

from __future__ import annotations

from pathlib import Path

from well_log_workstation.command_audit import CommandAuditLog, audit, get_default_audit_log
from well_log_workstation.extension_points import (
    BUILTIN_EXTENSION_POINTS,
    first_ship_extension_ids,
    list_extension_points,
)

REPO = Path(__file__).resolve().parents[1]


def test_adr_0055_and_status_doc_exist() -> None:
    assert (REPO / "docs" / "adr" / "0055-plugin-runtime-staged-after-first-ship.md").is_file()
    status = REPO / "docs" / "plugin-runtime-status.md"
    assert status.is_file()
    text = status.read_text(encoding="utf-8")
    assert "P.SPEC" in text
    assert "不在" in text or "not" in text.lower() or "首发" in text
    assert "Custom Layer" in text


def test_builtin_extension_points_first_ship() -> None:
    all_pts = list_extension_points()
    assert len(all_pts) >= 3
    ship = list_extension_points(first_ship_only=True)
    assert all(p.available_in_first_ship for p in ship)
    ids = first_ship_extension_ids()
    assert "wellplot.custom_layer" in ids
    assert "wellplot.plugin_runtime" not in ids
    runtime = next(p for p in BUILTIN_EXTENSION_POINTS if p.id == "wellplot.plugin_runtime")
    assert runtime.available_in_first_ship is False


def test_command_audit_ring() -> None:
    log = CommandAuditLog(capacity=3)
    log.record("export.pdf", ok=True, target="plot-1")
    log.record("import.las", ok=True, target="well-a")
    log.record("export.cgm", ok=False, detail="engine missing")
    log.record("export.svg", ok=True)
    assert len(log) == 3  # capacity ring
    recent = log.recent(2)
    assert recent[-1].name == "export.svg"
    assert any(r.name == "export.cgm" and r.ok is False for r in log.recent(10))


def test_default_audit_log_record() -> None:
    log = get_default_audit_log()
    before = len(log)
    rec = audit("test.ping", ok=True, detail="unit")
    assert rec.name == "test.ping"
    assert len(log) >= before
