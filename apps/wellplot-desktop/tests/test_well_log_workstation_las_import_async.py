"""LAS import runs off the GUI thread from the interactive path (#511)."""
from __future__ import annotations

import os
import threading

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest

from well_log_workstation.workspace import create_workspace


def _write_las(path, well: str = "A") -> str:
    path.write_text(
        f"""~VERSION INFORMATION
VERS. 2.0
~WELL INFORMATION
STRT.M 1000.0
STOP.M 1002.0
STEP.M 1.0
NULL. -999.25
WELL. {well}
~CURVE INFORMATION
DEPT.M
GR.GAPI
~ASCII
1000 20
1001 30
1002 40
""",
        encoding="utf-8",
    )
    return str(path)


@pytest.fixture(autouse=True)
def _no_modal_boxes(monkeypatch):
    """Modal QMessageBox would block the offscreen event loop forever."""
    from PySide6.QtWidgets import QMessageBox

    shown: list[str] = []
    monkeypatch.setattr(
        QMessageBox, "information",
        staticmethod(lambda *a, **k: shown.append(str(a[-1] if len(a) > 2 else a)) or 0),
    )
    monkeypatch.setattr(
        QMessageBox, "warning",
        staticmethod(lambda *a, **k: shown.append(str(a[-1] if len(a) > 2 else a)) or 0),
    )
    yield shown


@pytest.fixture
def win(qtbot, tmp_path):
    import well_log_workstation.shell as shell_mod
    from well_log_workstation.shell import WellLogWorkstationWindow

    ws = create_workspace(tmp_path / "ws")
    w = WellLogWorkstationWindow()
    qtbot.addWidget(w)
    w.set_workspace(ws)
    return w


def test_las_import_parse_runs_off_gui_thread(win, qtbot, tmp_path, monkeypatch):
    import well_log_workstation.shell as shell_mod

    las = _write_las(tmp_path / "a.las", "ASYNC-1")
    calls: list[str] = []
    real = shell_mod.import_las_into_workspace

    def spy(workspace, path):
        calls.append(threading.current_thread().name)
        return real(workspace, path)

    monkeypatch.setattr(shell_mod, "import_las_into_workspace", spy)

    win._start_las_import(las)
    # Nothing parsed synchronously in the calling (GUI) thread.
    assert calls == []

    def done():
        return bool(win._las_import_thread is None and win._selected_well_id)

    qtbot.waitUntil(done, timeout=15_000)
    assert calls and calls[0] != threading.current_thread().name
    # The result was applied on the GUI thread: document in the session and
    # the well selected.
    doc = win.session.get(win._selected_well_id)
    assert doc is not None and doc.well_name == "ASYNC-1"
    assert len(doc.curves) >= 1


def test_las_import_failure_reports_and_reenables(win, qtbot, tmp_path, monkeypatch, _no_modal_boxes):
    import well_log_workstation.shell as shell_mod
    from well_log_workstation.las_import import LasImportError

    def boom(workspace, path):
        raise LasImportError("corrupt las file")

    monkeypatch.setattr(shell_mod, "import_las_into_workspace", boom)
    las = _write_las(tmp_path / "bad.las", "BAD")

    win._start_las_import(las)
    qtbot.waitUntil(lambda: win._las_import_thread is None, timeout=15_000)
    assert any("corrupt las file" in s for s in _no_modal_boxes)
    assert win._act_import_las.isEnabled()


def test_second_import_while_running_is_rejected(win, qtbot, tmp_path, monkeypatch, _no_modal_boxes):
    import well_log_workstation.shell as shell_mod
    import threading

    started = threading.Event()
    release = threading.Event()
    real = shell_mod.import_las_into_workspace

    def blocker(workspace, path):
        started.set()
        release.wait(10)  # hold the worker until the test releases it
        return real(workspace, path)

    monkeypatch.setattr(shell_mod, "import_las_into_workspace", blocker)
    las = _write_las(tmp_path / "a.las", "HOLD")
    win._start_las_import(las)
    qtbot.waitUntil(started.is_set, timeout=10_000)
    assert win._las_import_thread is not None

    try:
        win._start_las_import(las)  # second while running
        assert any("正在进行" in s for s in _no_modal_boxes)
    finally:
        release.set()
    qtbot.waitUntil(lambda: win._las_import_thread is None, timeout=15_000)
