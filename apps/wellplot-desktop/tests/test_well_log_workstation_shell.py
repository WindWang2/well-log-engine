"""Smoke tests for WellPlot Desktop L-shell (#216 / brand #290)."""

from __future__ import annotations

import os

import pytest

# Platform must be set before any QApplication in this process when possible.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.branding import (  # noqa: E402
    PRODUCT_NAME,
    about_text,
    window_title,
)
from well_log_workstation.qt_platform import (  # noqa: E402
    configure_qt_platform_for_session,
    effective_qt_platform_hint,
)
from well_log_workstation.shell import WellLogWorkstationWindow  # noqa: E402


def test_configure_clears_xcb_on_wayland(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("WAYLAND_DISPLAY", "wayland-0")
    monkeypatch.setenv("QT_QPA_PLATFORM", "xcb")
    monkeypatch.delenv("WLWS_FORCE_XCB", raising=False)
    monkeypatch.delenv("PALEO_FORCE_XCB", raising=False)
    result = configure_qt_platform_for_session(warn=False)
    assert result is None
    assert "QT_QPA_PLATFORM" not in os.environ


def test_configure_keeps_offscreen(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    assert configure_qt_platform_for_session(warn=False) == "offscreen"


def test_force_xcb_on_wayland(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("WAYLAND_DISPLAY", "wayland-0")
    monkeypatch.setenv("QT_QPA_PLATFORM", "xcb")
    monkeypatch.setenv("WLWS_FORCE_XCB", "1")
    assert configure_qt_platform_for_session(warn=False) == "xcb"


def test_shell_has_l_chrome(qtbot) -> None:
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    win.show()

    assert win.objectName() == "WellLogWorkstationWindow"
    assert win.windowTitle() == PRODUCT_NAME
    assert win.workspace_tree.objectName() == "WorkspaceTree"
    # Unified tree: no dual 工区|井内容 tabs; content is under 井 nodes
    assert getattr(win, "left_tabs", None) is None
    assert win.well_content_tree is win.workspace_tree
    assert win.document_tabs.objectName() == "DocumentTabs"
    assert win.template_list.objectName() == "TemplateList"
    assert win.tops_list.objectName() == "TopsList"

    menu_titles = [a.text().replace("&", "") for a in win.menuBar().actions()]
    for expected in ("文件", "图件", "图版", "导出", "层位", "帮助"):
        assert expected in menu_titles

    from PySide6.QtWidgets import QSplitter

    sp = win.findChild(QSplitter, "ShellSplitter")
    assert sp is not None
    assert sp.count() == 3

    assert win.document_tabs.count() >= 1
    msg = win.statusBar().currentMessage() or ""
    assert "Qt:" in msg
    assert PRODUCT_NAME in msg


def test_product_branding_helpers() -> None:
    assert PRODUCT_NAME == "WellPlot Desktop"
    assert window_title() == PRODUCT_NAME
    assert window_title(workspace_name="Demo") == f"Demo — {PRODUCT_NAME}"
    body = about_text(version="0.1.0")
    assert PRODUCT_NAME in body
    assert "well_log_workstation" in body
    assert "Workstation" in body  # upgrade note


def test_about_action_enabled(qtbot) -> None:
    win = WellLogWorkstationWindow()
    qtbot.addWidget(win)
    about = None
    for action in win.menuBar().actions():
        menu = action.menu()
        if menu is None:
            continue
        for a in menu.actions():
            if a.objectName() == "Action_About":
                about = a
                break
    assert about is not None
    assert about.isEnabled()


def test_effective_hint_nonempty() -> None:
    assert len(effective_qt_platform_hint()) > 0
