"""Startup page + recent workspaces (#291 / T3)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QSettings

from well_log_workstation.branding import ORGANIZATION_NAME, PRODUCT_NAME
from well_log_workstation.recent_workspaces import (
    add_recent,
    clear_recent,
    load_recent,
    remove_recent,
)
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import create_workspace


@pytest.fixture(autouse=True)
def _isolate_settings(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Keep QSettings out of the real user config for this suite."""
    # Point Qt settings to a temp org/app under tmp
    org = f"paleo-test-{tmp_path.name}"
    app = "WellPlotDesktopTest"
    monkeypatch.setattr(
        "well_log_workstation.recent_workspaces.ORGANIZATION_NAME", org
    )
    monkeypatch.setattr(
        "well_log_workstation.recent_workspaces.PRODUCT_NAME", app
    )
    clear_recent()
    yield
    clear_recent()


def test_recent_add_load_remove(tmp_path: Path) -> None:
    a = tmp_path / "ws-a"
    b = tmp_path / "ws-b"
    a.mkdir()
    b.mkdir()
    add_recent(a)
    add_recent(b)
    recent = load_recent()
    assert recent[0] == str(b.resolve())
    assert str(a.resolve()) in recent
    remove_recent(b)
    recent2 = load_recent()
    assert str(b.resolve()) not in recent2
    assert str(a.resolve()) in recent2


def test_cold_start_shows_startup_page(qtbot) -> None:
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.show()
    assert win._main_stack.currentIndex() == 0
    assert win.startup_page.isVisible() or win._main_stack.currentWidget() is win.startup_page
    assert win.startup_page.objectName() == "StartupPage"
    assert win.startup_page.new_btn.objectName() == "StartupNewWorkspace"
    assert win.workspace is None


def test_open_workspace_switches_to_main_shell(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "live", name="Live")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    assert win._main_stack.currentIndex() == 0
    win.set_workspace(ws)
    assert win._main_stack.currentIndex() == 1
    assert win.workspace is not None
    assert win.workspace.name == "Live"
    # Recent list should include this path
    recent = load_recent()
    assert any(str(ws.root) in p or p.endswith("live") for p in recent)


def test_recent_invalid_path_removes_and_warns(qtbot, tmp_path: Path, monkeypatch) -> None:
    missing = tmp_path / "gone"
    missing.mkdir()
    add_recent(missing)
    missing.rmdir()

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.startup_page.refresh_recent()
    # Double-open path that no longer exists
    warned: list[str] = []

    def fake_warning(parent, title, text):  # noqa: ANN001
        warned.append(text)
        return 0

    monkeypatch.setattr(
        "well_log_workstation.shell.QMessageBox.warning", fake_warning
    )
    win._on_open_recent_workspace(str(missing))
    assert warned
    assert str(missing) not in load_recent()
    assert win.workspace is None
    assert win._main_stack.currentIndex() == 0
