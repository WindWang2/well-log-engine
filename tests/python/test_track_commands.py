"""Track/Data workflow bindings (ADR 0056/0057).

apply_track_command maps one payload to one validated, undoable C++ track
command riding ApplyPatchCommand; presentation_state / selection_state /
set_row_selection expose the live presentation + shared Selection Set to
Python. Runs headless (QT_QPA_PLATFORM=minimal) — session commands need no GL.
"""

import unittest

import numpy as np
from PySide6.QtWidgets import QApplication

from welllog import WellLogValidationError, WellLogView


DOCUMENT = "30000000-0000-4000-8000-000000000001"
AXIS = "30000000-0000-4000-8000-000000000002"
CURVE_GR = "30000000-0000-4000-8000-000000000003"
CURVE_RT = "30000000-0000-4000-8000-000000000004"


def multi_track_payload() -> dict:
    depth = np.arange(1000.0, 1005.0, dtype=np.float64)
    gr = np.array([10.0, 40.0, 90.0, 20.0, 55.0], dtype=np.float64)
    rt = np.array([1.0, 4.0, 9.0, 2.0, 8.0], dtype=np.float64)
    for buffer in (depth, gr, rt):
        buffer.flags.writeable = False
    return {
        "document_id": DOCUMENT,
        "axis_id": AXIS,
        "depth": depth,
        "depth_unit": "m",
        "curves": [
            {"curve_id": CURVE_GR, "mnemonic": "GR", "value_unit": "API",
             "values": gr},
            {"curve_id": CURVE_RT, "mnemonic": "RT", "value_unit": "OHMM",
             "values": rt},
        ],
        # NOTE: submit_multi_track derives its own deterministic track/scale/
        # layer ids (the payload track_id/scale_id strings are positional
        # hints only) — tests read the real ids back via presentation_state.
        "tracks": [
            {"track_id": "30000000-0000-4000-8000-000000000011",
             "width_mm": 40.0,
             "layers": [
                 {"curve_id": CURVE_GR,
                  "scale_id": "30000000-0000-4000-8000-000000000021"}],
             "scales": [
                 {"scale_id": "30000000-0000-4000-8000-000000000021",
                  "minimum": 0.0, "maximum": 100.0, "unit": "API"}]},
            {"track_id": "30000000-0000-4000-8000-000000000012",
             "width_mm": 30.0,
             "layers": [
                 {"curve_id": CURVE_RT,
                  "scale_id": "30000000-0000-4000-8000-000000000022"}],
             "scales": [
                 {"scale_id": "30000000-0000-4000-8000-000000000022",
                  "minimum": 0.0, "maximum": 10.0, "unit": "OHMM"}]},
        ],
    }


class TrackCommandBindingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def _prepared_view(self) -> WellLogView:
        view = WellLogView()
        report = view.submit_multi_track(multi_track_payload())
        self.assertTrue(report.get("render_prepared", True))
        return view

    @staticmethod
    def _bindings(view: WellLogView) -> dict:
        """Actual entity ids, resolved from the live presentation.

        submit_multi_track derives deterministic presentation ids internally,
        so callers read them back instead of assuming payload-side ids. The
        fixture puts GR alone in the first track and RT alone in the second.
        """
        state = view.presentation_state(DOCUMENT)
        assert state is not None
        tracks = sorted(state["tracks"], key=lambda t: t["z_order"])
        track1, track2 = tracks[0]["id"], tracks[1]["id"]
        gr_layer = [l for l in state["curve_layers"]
                    if l["track_id"] == track1
                    and l["curve_id"] == CURVE_GR][0]
        rt_layer = [l for l in state["curve_layers"]
                    if l["track_id"] == track2
                    and l["curve_id"] == CURVE_RT][0]
        return {
            "track1": track1,
            "track2": track2,
            "gr_layer": gr_layer["id"],
            "rt_layer": rt_layer["id"],
            "gr_scale": gr_layer["scale_id"],
            "rt_scale": rt_layer["scale_id"],
        }

    def test_bind_move_unbind_workflow_updates_presentation_state(self) -> None:
        view = self._prepared_view()
        state = view.presentation_state(DOCUMENT)
        self.assertIsNotNone(state)
        self.assertEqual(len(state["tracks"]), 2)
        self.assertEqual(len(state["curve_layers"]), 2)
        ids = self._bindings(view)
        track1, track2 = ids["track1"], ids["track2"]

        # Bind GR into track2 → a layer id is generated and reported.
        report = view.apply_track_command(
            {"op": "bind_curve", "document_id": DOCUMENT,
             "curve_id": CURVE_GR, "track_id": track2})
        self.assertIn("revision", report)
        layer_id = report["layer_id"]
        self.assertTrue(layer_id)
        state = view.presentation_state(DOCUMENT)
        self.assertEqual(len(state["curve_layers"]), 3)
        bound = [l for l in state["curve_layers"] if l["id"] == layer_id]
        self.assertEqual(len(bound), 1)
        self.assertEqual(bound[0]["track_id"], track2)

        # Move the original GR layer track1 → track2 (API scale generated).
        move = view.apply_track_command(
            {"op": "move_curve_layer", "document_id": DOCUMENT,
             "layer_id": ids["gr_layer"], "target_track_id": track2})
        self.assertIn("revision", move)
        state = view.presentation_state(DOCUMENT)
        moved = [l for l in state["curve_layers"]
                 if l["id"] == ids["gr_layer"]]
        self.assertEqual(moved[0]["track_id"], track2)
        # A new API scale exists in track2 now.
        api_scales = [s for s in state["scales"]
                      if s["track_id"] == track2 and s["unit"] == "API"]
        self.assertEqual(len(api_scales), 1)

        # Unbind removes the layer.
        view.apply_track_command(
            {"op": "unbind_curve", "document_id": DOCUMENT,
             "layer_id": layer_id})
        state = view.presentation_state(DOCUMENT)
        self.assertEqual(len(state["curve_layers"]), 2)

    def test_track_editing_ops_round_trip(self) -> None:
        view = self._prepared_view()
        ids = self._bindings(view)
        track1, track2 = ids["track1"], ids["track2"]
        # add_track reports the generated id; resize + reorder + visibility
        # are all visible in presentation_state.
        added = view.apply_track_command(
            {"op": "add_track", "document_id": DOCUMENT, "width_mm": 25.0})
        new_track = added["track_id"]
        view.apply_track_command(
            {"op": "resize_track", "document_id": DOCUMENT,
             "track_id": new_track, "width_mm": 33.0})
        view.apply_track_command(
            {"op": "reorder_tracks", "document_id": DOCUMENT,
             "track_ids": [new_track, track2, track1]})
        view.apply_track_command(
            {"op": "set_track_visibility", "document_id": DOCUMENT,
             "track_id": new_track, "visible": False})
        view.apply_track_command(
            {"op": "set_track_header", "document_id": DOCUMENT,
             "track_id": new_track, "height_mm": 9.0, "font_size_mm": 2.5})
        state = view.presentation_state(DOCUMENT)
        tracks = {t["id"]: t for t in state["tracks"]}
        self.assertEqual(list(tracks), [new_track, track2, track1])
        self.assertFalse(tracks[new_track]["visible"])
        self.assertAlmostEqual(tracks[new_track]["width_mm"], 33.0)
        self.assertAlmostEqual(tracks[new_track]["header_height_mm"], 9.0)

        # remove_track cascades its scales/layers.
        view.apply_track_command(
            {"op": "remove_track", "document_id": DOCUMENT,
             "track_id": new_track})
        state = view.presentation_state(DOCUMENT)
        self.assertEqual(len(state["tracks"]), 2)

    def test_scale_and_style_and_layer_ops(self) -> None:
        view = self._prepared_view()
        ids = self._bindings(view)
        gr_layer, rt_layer = ids["gr_layer"], ids["rt_layer"]
        gr_scale = ids["gr_scale"]

        view.apply_track_command(
            {"op": "set_scale", "document_id": DOCUMENT,
             "scale_id": gr_scale, "minimum": 5.0, "maximum": 95.0})
        state = view.presentation_state(DOCUMENT)
        scale = [s for s in state["scales"] if s["id"] == gr_scale][0]
        self.assertAlmostEqual(scale["minimum"], 5.0)
        self.assertAlmostEqual(scale["maximum"], 95.0)

        view.apply_track_command(
            {"op": "auto_range_scale", "document_id": DOCUMENT,
             "scale_id": gr_scale})
        state = view.presentation_state(DOCUMENT)
        scale = [s for s in state["scales"] if s["id"] == gr_scale][0]
        self.assertAlmostEqual(scale["minimum"], 10.0)
        self.assertAlmostEqual(scale["maximum"], 90.0)

        view.apply_track_command(
            {"op": "set_layer_style", "document_id": DOCUMENT,
             "layer_id": gr_layer, "color": "#ff8800",
             "line_width_mm": 0.6})
        state = view.presentation_state(DOCUMENT)
        layer = [l for l in state["curve_layers"]
                 if l["id"] == gr_layer][0]
        self.assertEqual(layer["color"], "#ff8800ff")
        self.assertAlmostEqual(layer["line_width_mm"], 0.6)

        view.apply_track_command(
            {"op": "set_layer_visibility", "document_id": DOCUMENT,
             "layer_id": gr_layer, "visible": False})
        state = view.presentation_state(DOCUMENT)
        layer = [l for l in state["curve_layers"]
                 if l["id"] == gr_layer][0]
        self.assertFalse(layer["visible"])

        dup = view.apply_track_command(
            {"op": "duplicate_curve_layer", "document_id": DOCUMENT,
             "layer_id": rt_layer})
        self.assertTrue(dup["new_layer_id"])
        view.apply_track_command(
            {"op": "reorder_curve_layers", "document_id": DOCUMENT,
             "track_id": ids["track2"], "layer_ids": [dup["new_layer_id"],
                                                      rt_layer]})

    def test_invalid_ops_raise_typed_errors(self) -> None:
        view = self._prepared_view()
        ids = self._bindings(view)
        with self.assertRaises(WellLogValidationError):
            view.apply_track_command(
                {"op": "bind_curve", "document_id": DOCUMENT,
                 "curve_id": "ffffffff-0000-4000-8000-0000000000ff",
                 "track_id": ids["track1"]})
        with self.assertRaises(WellLogValidationError):
            view.apply_track_command(
                {"op": "remove_track", "document_id": DOCUMENT,
                 "track_id": "ffffffff-0000-4000-8000-0000000000fe"})
        with self.assertRaises(WellLogValidationError):
            view.apply_track_command(
                {"op": "set_scale", "document_id": DOCUMENT,
                 "scale_id": ids["gr_scale"], "minimum": 500.0})
        with self.assertRaises(WellLogValidationError):
            view.apply_track_command({"op": "nonsense",
                                      "document_id": DOCUMENT})

    def test_selection_state_and_row_selection_round_trip(self) -> None:
        view = self._prepared_view()
        self.assertIsNone(view.selection_state())
        report = view.set_row_selection(AXIS, 1, 3)
        self.assertEqual(report["first_row"], 1)
        self.assertEqual(report["last_row"], 3)
        state = view.selection_state()
        self.assertIsNotNone(state)
        self.assertEqual(state["sampling_axis_id"], AXIS)
        self.assertEqual(state["first_row"], 1)
        self.assertEqual(state["last_row"], 3)
        self.assertAlmostEqual(state["top"], 1001.0)
        self.assertAlmostEqual(state["bottom"], 1002.0)
        self.assertTrue(state["valid"])

    def test_hover_info_is_none_without_hover(self) -> None:
        view = self._prepared_view()
        self.assertIsNone(view.hover_info())


if __name__ == "__main__":
    unittest.main()
