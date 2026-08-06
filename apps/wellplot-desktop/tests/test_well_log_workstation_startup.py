"""Cold-start main shell + recent workspaces (no startup chooser)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.recent_workspaces import (
    add_recent,
    clear_recent,
    load_recent,
    remove_recent,
)
from well_log_workstation.shell import WellLogWorkstationWindow
from well_log_workstation.workspace import (
    create_workspace,
    default_workspace_root,
    ensure_startup_workspace,
    open_or_create_workspace,
)


@pytest.fixture(autouse=True)
def _isolate_settings(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Keep QSettings out of the real user config for this suite."""
    org = f"paleo-test-{tmp_path.name}"
    app = "WellPlotDesktopTest"
    monkeypatch.setattr(
        "well_log_workstation.recent_workspaces.ORGANIZATION_NAME", org
    )
    monkeypatch.setattr(
        "well_log_workstation.recent_workspaces.PRODUCT_NAME", app
    )
    # Isolate silent default session path under tmp
    monkeypatch.setattr(
        "well_log_workstation.workspace.default_workspace_root",
        lambda: tmp_path / "default-workspace",
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


def test_cold_start_main_shell_no_startup_page(qtbot) -> None:
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.open_default_session()
    win.show()
    assert win._main_stack.currentIndex() == 0
    assert getattr(win, "startup_page", None) is None
    assert win.workspace is not None
    assert win._main_stack.currentWidget().objectName() == "ShellRoot"
    # Top-level tree: 数据 + 图件 only
    tree = win.workspace_tree
    assert tree.topLevelItemCount() == 2
    assert tree.topLevelItem(0).text(0) == "数据"
    assert tree.topLevelItem(1).text(0) == "图件"


def test_open_workspace_stays_on_main_shell(qtbot, tmp_path: Path) -> None:
    ws = create_workspace(tmp_path / "live", name="Live")
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    assert win._main_stack.currentIndex() == 0
    win.set_workspace(ws)
    assert win._main_stack.currentIndex() == 0
    assert win.workspace is not None
    assert win.workspace.name == "Live"
    recent = load_recent()
    assert any(str(ws.root) in p or p.endswith("live") for p in recent)


def test_ensure_startup_prefers_recent(tmp_path: Path) -> None:
    custom = create_workspace(tmp_path / "custom", name="Custom")
    add_recent(custom.root)
    ws = ensure_startup_workspace()
    assert ws.root == custom.root
    assert ws.name == "Custom"


def test_open_or_create_idempotent(tmp_path: Path) -> None:
    root = tmp_path / "once"
    a = open_or_create_workspace(root, name="Once")
    b = open_or_create_workspace(root, name="Ignored")
    assert a.root == b.root
    assert b.name == "Once"


def test_recent_invalid_path_removes_and_warns(qtbot, tmp_path: Path, monkeypatch) -> None:
    missing = tmp_path / "gone"
    missing.mkdir()
    add_recent(missing)
    missing.rmdir()

    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
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
    assert win._main_stack.currentIndex() == 0