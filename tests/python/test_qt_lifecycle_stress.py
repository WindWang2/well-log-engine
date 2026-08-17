"""#173 — Python lifecycle stress: create/destroy churn, GC off GUI thread.

Requires a built welllog package on PYTHONPATH and a Qt GUI thread. Runs under
QT_QPA_PLATFORM=minimal for widget ownership (capability may be unavailable).
"""

from __future__ import annotations

import gc
import threading
import unittest
import weakref

import numpy as np
from PySide6.QtCore import QCoreApplication, QEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget
from shiboken6 import Shiboken

from welllog import WellLogThreadError, WellLogView


def _app() -> QApplication:
    existing = QApplication.instance()
    if existing is not None:
        return existing  # type: ignore[return-value]
    return QApplication([])


class QtLifecycleStressTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = _app()

    def test_create_destroy_churn_invalidates_wrappers(self) -> None:
        for _ in range(30):
            host = QWidget()
            layout = QVBoxLayout(host)
            view = WellLogView()
            view_ref = weakref.ref(view)
            layout.addWidget(view)
            self.assertTrue(Shiboken.isValid(view))
            host.deleteLater()
            QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
            self.app.processEvents()
            gc.collect()
            self.assertFalse(Shiboken.isValid(view))
            del view
            gc.collect()
            self.assertIsNone(view_ref())

    def test_gc_from_worker_thread_does_not_free_pinned_buffers(self) -> None:
        """AC3: GC off the GUI thread must not free buffers owned by Document."""
        view = WellLogView()
        depth = np.arange(1000.0, 1010.0, dtype=np.float64)
        values = np.linspace(0.0, 90.0, num=10, dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False
        depth_ref = weakref.ref(depth)
        values_ref = weakref.ref(values)

        report = view.submit_curve(
            depth,
            values,
            "c7300000-0000-4000-8000-000000000001",
            "c7300000-0000-4000-8000-000000000002",
            "c7300000-0000-4000-8000-000000000003",
            "GR",
            "m",
            "API",
        )
        self.assertIs(report["render_prepared"], True)
        self.assertEqual(report["depth"]["access_mode"], "zero_copy")
        self.assertEqual(report["curve"]["access_mode"], "zero_copy")

        del depth
        del values
        gc.collect()
        self.assertIsNotNone(depth_ref())
        self.assertIsNotNone(values_ref())

        errors: list[BaseException] = []
        stop = threading.Event()

        def worker_gc() -> None:
            try:
                while not stop.is_set():
                    gc.collect()
            except BaseException as exc:  # noqa: BLE001 — surface any crash
                errors.append(exc)

        thread = threading.Thread(target=worker_gc, name="welllog-gc-stress")
        thread.start()
        try:
            # GUI thread continues to use the view while worker GCs.
            for _ in range(20):
                sample = view.sample_value(
                    "c7300000-0000-4000-8000-000000000003", 3
                )
                self.assertEqual(sample, float(np.linspace(0.0, 90.0, 10)[3]))
                self.app.processEvents()
                QTest.qWait(5)
            self.assertIsNotNone(depth_ref())
            self.assertIsNotNone(values_ref())
            self.assertTrue(Shiboken.isValid(view))
        finally:
            stop.set()
            thread.join(timeout=5.0)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])

        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()
        self.assertIsNone(depth_ref())
        self.assertIsNone(values_ref())

    def test_not_on_gui_thread_rejects_submit(self) -> None:
        """Public submit must stay on the GUI thread (queued/reject, not UAF)."""
        view = WellLogView()
        depth = np.arange(3, dtype=np.float64)
        values = np.ones(3, dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False
        errors: list[BaseException] = []
        done = threading.Event()

        def off_thread() -> None:
            try:
                view.submit_curve(
                    depth,
                    values,
                    "c7300000-0000-4000-8000-000000000011",
                    "c7300000-0000-4000-8000-000000000012",
                    "c7300000-0000-4000-8000-000000000013",
                    "GR",
                    "m",
                    "API",
                )
            except BaseException as exc:  # expected typed error
                errors.append(exc)
            finally:
                done.set()

        threading.Thread(target=off_thread).start()
        self.assertTrue(done.wait(5.0))
        # Must reject (not silently accept) an off-GUI-thread submit.
        # QThread.currentThread() is never None — do not use it as a fallback.
        self.assertTrue(errors, "off-thread submit_curve must raise")
        self.assertIsInstance(errors[0], WellLogThreadError)
        self.assertEqual(getattr(errors[0], "code", ""), "thread_violation")
        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
