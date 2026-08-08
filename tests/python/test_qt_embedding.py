import gc
import unittest
import weakref

import numpy as np
from PySide6.QtCore import QCoreApplication, QEvent
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget
from shiboken6 import Shiboken

from welllog import (
    WellLogThreadError,
    WellLogValidationError,
    WellLogView,
)


class WellLogViewEmbeddingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def test_generated_view_follows_qwidget_parent_lifetime(self) -> None:
        host = QWidget()
        layout = QVBoxLayout(host)
        view = WellLogView()
        view_ref = weakref.ref(view)
        destroyed_count = 0

        def record_destruction() -> None:
            nonlocal destroyed_count
            destroyed_count += 1

        view.destroyed.connect(record_destruction)
        layout.addWidget(view)

        self.assertIsInstance(view, QOpenGLWidget)
        self.assertIs(view.parentWidget(), host)
        self.assertTrue(Shiboken.isValid(view))

        host.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()

        self.assertEqual(destroyed_count, 1)
        self.assertFalse(Shiboken.isValid(view))

        del view
        gc.collect()

        self.assertIsNone(view_ref())
        self.assertEqual(destroyed_count, 1)

    def test_numpy_curve_is_zero_copy_and_owned_by_the_document(self) -> None:
        view = WellLogView()
        depth = np.arange(1000.0, 1006.0, dtype=np.float64)
        values_source = np.arange(12.0, dtype=np.float32)
        values = values_source[::2]
        depth_ref = weakref.ref(depth)
        values_ref = weakref.ref(values)
        expected_depth_address = depth.ctypes.data
        expected_value_address = values.ctypes.data
        depth.flags.writeable = False
        values.flags.writeable = False

        report = view.submit_curve(
            depth,
            values,
            "10000000-0000-4000-8000-000000000001",
            "10000000-0000-4000-8000-000000000002",
            "10000000-0000-4000-8000-000000000003",
            "GR",
            "m",
            "API",
        )

        self.assertEqual(report["depth"]["access_mode"], "zero_copy")
        self.assertEqual(report["curve"]["access_mode"], "zero_copy")
        self.assertEqual(report["depth"]["address"], expected_depth_address)
        self.assertEqual(report["curve"]["address"], expected_value_address)
        self.assertEqual(report["curve"]["stride_bytes"], 8)
        self.assertIs(report["render_prepared"], True)

        del depth
        del values
        del values_source
        gc.collect()

        self.assertIsNotNone(depth_ref())
        self.assertIsNotNone(values_ref())
        self.assertEqual(
            view.sample_value("10000000-0000-4000-8000-000000000003", 3),
            6.0,
        )

        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()

        self.assertIsNone(depth_ref())
        self.assertIsNone(values_ref())

    def test_synchronous_failures_use_stable_typed_exceptions(self) -> None:
        view = WellLogView()
        arguments = (
            "20000000-0000-4000-8000-000000000001",
            "20000000-0000-4000-8000-000000000002",
            "20000000-0000-4000-8000-000000000003",
            "GR",
            "m",
            "API",
        )

        with self.assertRaises(WellLogValidationError) as raised:
            view.submit_curve(
                np.ones((2, 2), dtype=np.float64),
                np.ones(4, dtype=np.float32),
                *arguments,
            )

        self.assertEqual(raised.exception.code, "invalid_buffer")
        self.assertTrue(issubclass(WellLogThreadError, RuntimeError))

        writable_depth = np.arange(3, dtype=np.float64)
        writable_values = np.ones(3, dtype=np.float32)
        with self.assertRaises(WellLogValidationError) as writable_error:
            view.submit_curve(writable_depth, writable_values, *arguments)

        self.assertEqual(writable_error.exception.code, "writable_buffer")

        invalid_depth = np.asarray(
            [1000.0, 1002.0, 1001.0], dtype=np.float64
        )
        invalid_values = np.ones(3, dtype=np.float32)
        invalid_depth.flags.writeable = False
        invalid_values.flags.writeable = False
        with self.assertRaises(WellLogValidationError) as result_error:
            view.submit_curve(
                invalid_depth,
                invalid_values,
                *arguments,
            )

        self.assertEqual(result_error.exception.code, "invalid_sampling_axis")

    def test_asynchronous_view_failure_is_a_typed_qt_signal(self) -> None:
        view = WellLogView()
        errors: list[tuple[str, str]] = []
        view.viewError.connect(
            lambda code, message: errors.append((code, message))
        )

        view.resize(160, 120)
        view.show()
        QTest.qWait(650)

        self.assertTrue(errors)
        self.assertEqual(errors[-1][0], "capability_unavailable")
        self.assertTrue(errors[-1][1])
        signal_names = {
            bytes(view.metaObject().method(index).name()).decode()
            for index in range(view.metaObject().methodCount())
        }
        self.assertNotIn("frameReady", signal_names)
        self.assertNotIn("frameStats", signal_names)

    def test_document_session_event_crosses_python_as_typed_fields(self) -> None:
        view = WellLogView()
        events: list[tuple[str, int]] = []
        view.documentChanged.connect(
            lambda document_id, revision: events.append(
                (document_id, revision)
            )
        )
        depth = np.arange(4, dtype=np.float64)
        values = np.arange(4, dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False
        view.submit_curve(
            depth,
            values,
            "40000000-0000-4000-8000-000000000001",
            "40000000-0000-4000-8000-000000000002",
            "40000000-0000-4000-8000-000000000003",
            "GR",
            "m",
            "API",
        )

        QTest.qWait(25)

        self.assertEqual(
            events,
            [("40000000-0000-4000-8000-000000000001", 1)],
        )

    def test_descending_depth_and_caller_ids_prepare_a_scene(self) -> None:
        view = WellLogView()
        depth = np.asarray([1002.0, 1001.0, 1000.0], dtype=np.float64)
        values = np.asarray([1.0, 2.0, 3.0], dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False

        report = view.submit_curve(
            depth,
            values,
            "50000000-0000-4000-8000-000000000001",
            "50000000-0000-4000-8000-000000000010",
            "50000000-0000-4000-8000-000000000003",
            "DEN",
            "m",
            "g/cm3",
        )

        self.assertIs(report["render_prepared"], True)

    def test_rejected_presentation_input_does_not_pin_buffers(self) -> None:
        view = WellLogView()
        depth = np.arange(3, dtype=np.float64)
        values = np.arange(3, dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False
        depth_ref = weakref.ref(depth)
        values_ref = weakref.ref(values)

        with self.assertRaises(WellLogValidationError) as raised:
            view.submit_curve(
                depth,
                values,
                "60000000-0000-4000-8000-000000000001",
                "60000000-0000-4000-8000-000000000002",
                "60000000-0000-4000-8000-000000000003",
                "GR",
                "",
                "API",
            )

        self.assertEqual(raised.exception.code, "invalid_presentation")
        del depth
        del values
        gc.collect()
        self.assertIsNone(depth_ref())
        self.assertIsNone(values_ref())

    def test_export_scene_svg_returns_non_empty_bytes(self) -> None:
        # T1 / #273: the engine SVG exporter is reachable from Python and
        # returns the document as in-memory bytes (the engine never writes
        # to disk). submit_curve prepares a single-well scene we then export.
        view = WellLogView()
        depth = np.arange(1000.0, 1006.0, dtype=np.float64)
        values = np.arange(6.0, dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False
        view.submit_curve(
            depth,
            values,
            "10000000-0000-4000-8000-000000000001",
            "10000000-0000-4000-8000-000000000002",
            "10000000-0000-4000-8000-000000000003",
            "GR",
            "m",
            "API",
        )

        svg = view.export_scene_svg(
            "10000000-0000-4000-8000-000000000001",
        )
        self.assertIsInstance(svg, bytes)
        self.assertGreater(len(svg), 0)
        # SVG documents begin with an XML declaration or the <svg> root.
        self.assertTrue(
            svg.lstrip().startswith(b"<?xml") or
            svg.lstrip().startswith(b"<svg"),
            f"unexpected SVG preamble: {svg[:40]!r}",
        )

        # Error path: an unknown (but valid) document_id surfaces a typed
        # document_not_found error. (A malformed UUID surfaces
        # invalid_document via parse_id — see test_rejected_presentation_input
        # for that parse path.)
        with self.assertRaises(WellLogValidationError) as raised:
            view.export_scene_svg("99999999-0000-4000-8000-000000000099")
        self.assertEqual(raised.exception.code, "document_not_found")

        with self.assertRaises(WellLogValidationError) as raised:
            view.export_scene_svg("not-a-uuid")
        self.assertEqual(raised.exception.code, "invalid_document")

        # Regression (review D-003): a well-formed nil UUID must surface a
        # typed invalid_document error, NOT a SystemError. Previously
        # parse_id returned an engaged-but-nil id, leaving a stale Python
        # error that shiboken surfaced as SystemError.
        with self.assertRaises(WellLogValidationError) as raised:
            view.export_scene_svg("00000000-0000-0000-0000-000000000000")
        self.assertEqual(raised.exception.code, "invalid_document")

        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()

    def test_export_scene_pdf_returns_non_empty_bytes(self) -> None:
        # T2 / #274: the engine PDF exporter is reachable from Python.
        # Text is glyph outlines (non-searchable, ADR 0047). Mirrors the SVG
        # smoke; the PDF path also exercises ExportSnapshot construction.
        view = WellLogView()
        depth = np.arange(1000.0, 1006.0, dtype=np.float64)
        values = np.arange(6.0, dtype=np.float32)
        depth.flags.writeable = False
        values.flags.writeable = False
        view.submit_curve(
            depth,
            values,
            "10000000-0000-4000-8000-000000000001",
            "10000000-0000-4000-8000-000000000002",
            "10000000-0000-4000-8000-000000000003",
            "GR",
            "m",
            "API",
        )

        pdf = view.export_scene_pdf(
            "10000000-0000-4000-8000-000000000001",
        )
        self.assertIsInstance(pdf, bytes)
        self.assertGreater(len(pdf), 0)
        # PDF files begin with the %PDF- header.
        self.assertTrue(
            pdf.startswith(b"%PDF-"),
            f"unexpected PDF header: {pdf[:16]!r}",
        )

        # Error path mirrors the SVG binding.
        with self.assertRaises(WellLogValidationError) as raised:
            view.export_scene_pdf("99999999-0000-4000-8000-000000000099")
        self.assertEqual(raised.exception.code, "document_not_found")

        with self.assertRaises(WellLogValidationError) as raised:
            view.export_scene_pdf("not-a-uuid")
        self.assertEqual(raised.exception.code, "invalid_document")

        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()

    def test_multi_track_intervals_and_patterns_reach_export(self) -> None:
        # T4 / #276: intervals + patterns submitted via submit_multi_track
        # reach the engine PreparedScene and appear in the exported SVG.
        view = WellLogView()
        depth = np.arange(1000.0, 1006.0, dtype=np.float64)
        gr = np.arange(6.0, dtype=np.float32)
        depth.flags.writeable = False
        gr.flags.writeable = False
        doc_id = "10000000-0000-4000-8000-000000000001"
        curve_id = "10000000-0000-4000-8000-000000000010"
        pattern_id = "10000000-0000-4000-8000-000000000020"
        interval_id = "10000000-0000-4000-8000-000000000030"
        payload = {
            "document_id": doc_id,
            "depth": depth,
            "depth_unit": "m",
            "curves": [
                {"curve_id": curve_id, "mnemonic": "GR", "values": gr,
                 "value_unit": "API"},
            ],
            "tracks": [
                {"width_mm": 40.0,
                 "layers": [{"curve_id": curve_id, "color": "#1972b8"}]},
            ],
            "intervals": [
                {"id": interval_id, "top_depth": 1001.0,
                 "bottom_depth": 1004.0, "fill_color": "#cc6633",
                 "pattern_id": pattern_id, "label": "Sand"},
            ],
            "patterns": [
                {"id": pattern_id, "tile_width_mm": 3.0,
                 "tile_height_mm": 3.0, "foreground": "#333333",
                 "primitives": [
                     {"line": {"from_x": 0.0, "from_y": 0.0,
                               "to_x": 3.0, "to_y": 3.0}},
                 ]},
            ],
        }
        report = view.submit_multi_track(payload)
        self.assertIs(report["render_prepared"], True)

        svg = view.export_scene_svg(doc_id)
        self.assertIsInstance(svg, bytes)
        text = svg.decode("utf-8", errors="replace")
        # The engine SVG emits an <rect id="interval-<uuid>"> per interval.
        self.assertIn(f"interval-{interval_id}", text,
                      "exported SVG must contain the submitted interval")

        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()

    def test_marker_semantics_reach_the_exported_svg(self) -> None:
        # SDK marker symbols (Epic C 收尾 slice 1): markers submitted with a
        # semantic render as symbol glyphs in the exported SVG when
        # marker_symbols is enabled; the semantic is recorded verbatim.
        view = WellLogView()
        depth = np.asarray([1000.0, 1000.5, 1001.0, 1001.5])
        gr = np.asarray([1.0, 2.0, 3.0, 4.0], dtype=np.float32)
        for arr in (depth, gr):
            arr.flags.writeable = False
        doc_id = "30000000-0000-4000-8000-000000000101"
        curve_id = "30000000-0000-4000-8000-000000000102"
        shoe_id = "30000000-0000-4000-8000-000000000103"
        top_id = "30000000-0000-4000-8000-000000000104"
        payload = {
            "document_id": doc_id,
            "depth": depth,
            "depth_unit": "m",
            "curves": [
                {"curve_id": curve_id, "mnemonic": "GR", "values": gr,
                 "value_unit": "API"},
            ],
            "tracks": [
                {"width_mm": 40.0, "scale_min": 0.0, "scale_max": 10.0,
                 "layers": [{"curve_id": curve_id, "color": "#1972b8"}]},
            ],
            "markers": [
                {"id": shoe_id, "depth": 1001.0, "label": "Shoe",
                 "semantic": "casing_shoe"},
                {"id": top_id, "depth": 1002.0, "label": "Top",
                 "semantic": "formation_top"},
                {"id": "30000000-0000-4000-8000-000000000105",
                 "depth": 1000.5, "label": "Plain"},
            ],
            "marker_symbols": True,
        }
        report = view.submit_multi_track(payload)
        self.assertIs(report["render_prepared"], True)

        svg = view.export_scene_svg(doc_id).decode("utf-8", errors="replace")
        self.assertIn(f"marker-symbol-{shoe_id}", svg,
                      "casing_shoe marker must render its symbol glyph")
        self.assertIn('data-semantic="casing_shoe"', svg)
        self.assertIn('data-semantic="formation_top"', svg)
        # Legacy marker without semantic keeps the historical behaviour
        # (formation_top) — still rendered as a symbol under marker_symbols.
        self.assertIn('data-semantic="formation_top"', svg)

        # Without marker_symbols the glyph paths are not emitted.
        payload.pop("marker_symbols")
        view2 = WellLogView()
        view2.submit_multi_track(payload)
        svg2 = view2.export_scene_svg(doc_id).decode("utf-8", errors="replace")
        self.assertNotIn("marker-symbol-", svg2,
                         "marker_symbols=false emits no glyph paths")
        self.assertIn(f"marker-{shoe_id}", svg2,
                      "the marker line itself still renders")

        view.deleteLater()
        view2.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()

    def test_multi_rate_curves_with_own_depth_axes(self) -> None:
        # Epic A: a curve may carry its own "depth" (independent sampling
        # axis); curves without one share the document depth. Both must reach
        # the engine document and appear in the exported SVG.
        view = WellLogView()
        depth = np.asarray([1000.0, 1000.5, 1001.0, 1001.5])
        gr = np.asarray([1.0, 2.0, 3.0, 4.0], dtype=np.float32)
        rt = np.asarray([10.0, 20.0, 30.0], dtype=np.float32)
        rt_depth = np.asarray([1000.1, 1000.6, 1001.1])
        for arr in (depth, gr, rt, rt_depth):
            arr.flags.writeable = False
        doc_id = "20000000-0000-4000-8000-000000000001"
        gr_id = "20000000-0000-4000-8000-000000000010"
        rt_id = "20000000-0000-4000-8000-000000000020"
        payload = {
            "document_id": doc_id,
            "depth": depth,
            "depth_unit": "m",
            "curves": [
                {"curve_id": gr_id, "mnemonic": "GR", "values": gr,
                 "value_unit": "API"},
                {"curve_id": rt_id, "mnemonic": "RT", "values": rt,
                 "value_unit": "OHMM", "depth": rt_depth},
            ],
            "tracks": [
                {"width_mm": 40.0, "scale_min": 0.0, "scale_max": 10.0,
                 "layers": [{"curve_id": gr_id, "color": "#1972b8"}]},
                {"width_mm": 40.0, "scale_min": 0.0, "scale_max": 50.0,
                 "layers": [{"curve_id": rt_id, "color": "#d62728"}]},
            ],
        }
        report = view.submit_multi_track(payload)
        self.assertIs(report["render_prepared"], True)
        self.assertEqual(report["curve_count"], 2)

        svg = view.export_scene_svg(doc_id)
        self.assertIsInstance(svg, bytes)
        text = svg.decode("utf-8", errors="replace")
        self.assertIn(gr_id, text, "exported SVG must reference the shared-axis curve")
        self.assertIn(rt_id, text, "exported SVG must reference the per-curve-axis curve")

        # A curve whose values do not match ITS depth axis must be rejected.
        bad_values = np.asarray([1.0, 2.0])
        bad_values.flags.writeable = False
        bad = dict(payload)
        bad["curves"] = [dict(payload["curves"][1], values=bad_values)]
        with self.assertRaises(Exception) as ctx:
            view.submit_multi_track(bad)
        self.assertIn("length", str(ctx.exception).lower(),
                      "per-curve length mismatch must be reported")

        # Shared-depth curves keep the old contract (mismatch still rejected).
        bad2 = dict(payload)
        bad2["curves"] = [dict(payload["curves"][0], values=bad_values)]
        with self.assertRaises(Exception) as ctx2:
            view.submit_multi_track(bad2)
        self.assertIn("length", str(ctx2.exception).lower(),
                      "shared-axis length mismatch must be reported")

        view.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.app.processEvents()
        gc.collect()


if __name__ == "__main__":
    unittest.main()
