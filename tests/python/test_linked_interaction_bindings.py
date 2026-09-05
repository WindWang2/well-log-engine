"""Linked-interpretation interaction bindings (paleo-workbench L2).

set_crosshair / clear_crosshair / crosshair_state form the external link-cursor
channel: a host drives the crosshair from an outside selection (e.g. a seismic
cursor) and reads it back in REFERENCE depth (the axis coordinate, MD m) —
never display depth. set_depth_selection mirrors the built-in Ctrl+drag
gesture; set_viewport_depth_range jumps the viewport. Runs headless
(QT_QPA_PLATFORM=minimal) — session commands need no GL.
"""

import threading
import unittest

import numpy as np
from PySide6.QtWidgets import QApplication

from welllog import WellLogThreadError, WellLogValidationError, WellLogView


DOCUMENT = "31000000-0000-4000-8000-000000000001"
AXIS = "31000000-0000-4000-8000-000000000002"
CURVE_GR = "31000000-0000-4000-8000-000000000003"
UNKNOWN_DOCUMENT = "31000000-0000-4000-8000-0000000000ff"


def multi_track_payload() -> dict:
    depth = np.arange(2000.0, 2005.0, dtype=np.float64)
    gr = np.array([15.0, 45.0, 95.0, 25.0, 60.0], dtype=np.float64)
    for buffer in (depth, gr):
        buffer.flags.writeable = False
    return {
        "document_id": DOCUMENT,
        "axis_id": AXIS,
        "depth": depth,
        "depth_unit": "m",
        "curves": [
            {"curve_id": CURVE_GR, "mnemonic": "GR", "value_unit": "API",
             "values": gr},
        ],
        "tracks": [
            {"track_id": "31000000-0000-4000-8000-000000000011",
             "width_mm": 40.0,
             "layers": [
                 {"curve_id": CURVE_GR,
                  "scale_id": "31000000-0000-4000-8000-000000000021"}],
             "scales": [
                 {"scale_id": "31000000-0000-4000-8000-000000000021",
                  "minimum": 0.0, "maximum": 100.0, "unit": "API"}]},
        ],
    }


class LinkedInteractionBindingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def _prepared_view(self) -> WellLogView:
        view = WellLogView()
        report = view.submit_multi_track(multi_track_payload())
        self.assertTrue(report.get("render_prepared", True))
        return view

    # --- external link cursor (set/clear/read round trip) -------------------

    def test_crosshair_round_trip_in_reference_depth(self) -> None:
        view = self._prepared_view()
        self.assertIsNone(view.crosshair_state())
        receipt = view.set_crosshair(DOCUMENT, 2002.5)
        self.assertIn("revision", receipt)
        state = view.crosshair_state()
        self.assertIsNotNone(state)
        self.assertEqual(state["document_id"], DOCUMENT)
        self.assertAlmostEqual(state["reference_depth"], 2002.5)
        self.assertAlmostEqual(state["display_depth"], 2002.5)
        self.assertAlmostEqual(state["track_fraction"], 0.5)

    def test_crosshair_track_fraction_is_clamped(self) -> None:
        view = self._prepared_view()
        view.set_crosshair(DOCUMENT, 2001.0, track_fraction=17.0)
        state = view.crosshair_state()
        self.assertIsNotNone(state)
        self.assertAlmostEqual(state["track_fraction"], 1.0)

    def test_clear_crosshair_removes_state(self) -> None:
        view = self._prepared_view()
        view.set_crosshair(DOCUMENT, 2002.0)
        self.assertIsNotNone(view.crosshair_state())
        view.clear_crosshair(DOCUMENT)
        self.assertIsNone(view.crosshair_state())

    def test_set_crosshair_rejects_unknown_document_and_bad_depth(self) -> None:
        view = self._prepared_view()
        with self.assertRaises(WellLogValidationError):
            view.set_crosshair(UNKNOWN_DOCUMENT, 2002.0)
        with self.assertRaises(WellLogValidationError):
            view.set_crosshair(DOCUMENT, float("nan"))

    # --- external depth selection (Ctrl+drag parity) -------------------------

    def test_set_depth_selection_lands_on_primary_axis(self) -> None:
        view = self._prepared_view()
        view.set_depth_selection(DOCUMENT, 2001.0, 2003.0)
        state = view.selection_state()
        self.assertIsNotNone(state)
        self.assertEqual(state["sampling_axis_id"], AXIS)
        self.assertAlmostEqual(state["top"], 2001.0)
        self.assertAlmostEqual(state["bottom"], 2003.0)
        self.assertTrue(state["valid"])

    def test_set_depth_selection_validates_bounds_and_document(self) -> None:
        view = self._prepared_view()
        with self.assertRaises(WellLogValidationError):
            view.set_depth_selection(DOCUMENT, 2003.0, 2001.0)  # inverted
        with self.assertRaises(WellLogValidationError):
            view.set_depth_selection(DOCUMENT, float("inf"), 2004.0)
        with self.assertRaises(WellLogValidationError):
            view.set_depth_selection(UNKNOWN_DOCUMENT, 2001.0, 2003.0)

    # --- viewport jump --------------------------------------------------------

    def test_set_viewport_depth_range_executes(self) -> None:
        view = self._prepared_view()
        receipt = view.set_viewport_depth_range(DOCUMENT, 2000.5, 2004.5)
        self.assertIn("revision", receipt)

    def test_set_viewport_depth_range_validates(self) -> None:
        view = self._prepared_view()
        with self.assertRaises(WellLogValidationError):
            view.set_viewport_depth_range(DOCUMENT, 2004.0, 2004.0)  # empty
        with self.assertRaises(WellLogValidationError):
            view.set_viewport_depth_range(DOCUMENT, 2004.0, 2000.0)  # inverted
        with self.assertRaises(WellLogValidationError):
            view.set_viewport_depth_range(UNKNOWN_DOCUMENT, 2000.0, 2004.0)

    # --- click pick + thread contract ----------------------------------------

    def test_click_pick_info_is_none_without_click(self) -> None:
        view = self._prepared_view()
        self.assertIsNone(view.click_pick_info())

    def test_off_gui_thread_crosshair_write_is_rejected(self) -> None:
        view = self._prepared_view()
        errors: list[BaseException] = []

        def worker() -> None:
            try:
                view.set_crosshair(DOCUMENT, 2002.0)
            except BaseException as exc:  # noqa: BLE001 - collected for assert
                errors.append(exc)

        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        self.assertEqual(len(errors), 1)
        self.assertIsInstance(errors[0], WellLogThreadError)
        # GUI-thread state was untouched by the rejected write.
        self.assertIsNone(view.crosshair_state())


if __name__ == "__main__":
    unittest.main()
