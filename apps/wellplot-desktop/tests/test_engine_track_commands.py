"""Track/Data workflow: engine bridge wrappers + incremental sync (ADR 0055/0056).

Model+command-layer tests with a fake engine view that mirrors the C++
apply_track_command semantics (validated ops over a presentation-state dict).
The real binding surface is covered by tests/python/test_track_commands.py;
these tests prove the DESKTOP bridge logic: capture_engine_bindings keyed by
host track id, and incremental_presentation_sync emitting exactly the right
op set for value edits, reorders, moves and unbinds — falling back to the
full path for structural changes.
"""

from __future__ import annotations

import os
from typing import Any

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.engine_bridge import (  # noqa: E402
    EngineSubmitError,
    apply_track_op,
    capture_engine_bindings,
    engine_hover_info,
    engine_selection_state,
    engine_set_row_selection,
    incremental_presentation_sync,
    track_command_supported,
)
from well_log_workstation.template_model import (  # noqa: E402
    BoundCurveLayer,
    BoundTrack,
    HostPresentation,
    ScaleSpec,
)

DOC = "31000000-0000-4000-8000-000000000001"


def _layer(mnemonic: str, color: str = "#1972b8") -> BoundCurveLayer:
    values = np.arange(4, dtype=np.float64)
    return BoundCurveLayer(
        mnemonic=mnemonic,
        color=color,
        unit="API",
        values=values,
        null_mask=np.zeros(4, dtype=bool),
    )


def _presentation(
    tracks: list[tuple[str, list[str], float | None]] | None = None,
) -> HostPresentation:
    """Host presentation; tracks = [(track_id, [mnemonics], width_fraction)]."""
    bound: list[BoundTrack] = [
        BoundTrack(
            id="depth",
            role="depth",
            title="深度",
            width_fraction=0.12,
            scale=None,
            layers=[],
        )
    ]
    for track_id, mnemonics, width in tracks or [
        ("track-gr", ["GR"], 0.3),
        ("track-rt", ["RT"], 0.3),
    ]:
        bound.append(
            BoundTrack(
                id=track_id,
                role="curve",
                title=track_id,
                width_fraction=width if width is not None else 0.3,
                scale=ScaleSpec(mode="linear", min=0.0, max=100.0, unit="API"),
                layers=[_layer(m) for m in mnemonics],
            )
        )
    return HostPresentation(
        template_id="t",
        template_name="T",
        well_document_id=DOC,
        well_name="W-1",
        depth=np.arange(4, dtype=np.float64),
        depth_unit="m",
        tracks=bound,
    )


class FakeEngineView:
    """Mirrors the C++ apply_track_command/presentation_state semantics."""

    def __init__(self, *, reject_ops: set[str] | None = None) -> None:
        self.calls: list[dict[str, Any]] = []
        self.reject_ops = reject_ops or set()
        # state mirrors the dict shape of WellLogView.presentation_state.
        self.state: dict[str, Any] = {
            "tracks": [],
            "scales": [],
            "curve_layers": [],
        }
        self._next = 0

    def _id(self) -> str:
        self._next += 1
        return f"32000000-0000-4000-8000-{self._next:012d}"

    # --- submission (simulates what the C++ bridge derives internally) -----
    def submit_snapshot(
        self, presentation: HostPresentation
    ) -> None:
        """Build engine state the way submit_multi_track does (derived ids)."""
        tracks: list[dict[str, Any]] = []
        scales: list[dict[str, Any]] = []
        layers: list[dict[str, Any]] = []
        z = 0
        for track in presentation.tracks:
            if not track.visible or track.role != "curve" or not track.layers:
                continue
            engine_track = self._id()
            tracks.append(
                {
                    "id": engine_track,
                    "width_mm": max(20.0, float(track.width_fraction) * 120.0),
                    "z_order": z,
                    "visible": True,
                    "header_height_mm": 8.0,
                }
            )
            z += 1
            scale_id = self._id()
            scales.append(
                {
                    "id": scale_id,
                    "track_id": engine_track,
                    "minimum": float(track.scale.min) if track.scale else 0.0,
                    "maximum": float(track.scale.max) if track.scale else 100.0,
                    "mode": "linear",
                    "direction": "left_to_right",
                    "unit": "API",
                }
            )
            for li, layer in enumerate(track.layers):
                layers.append(
                    {
                        "id": self._id(),
                        "track_id": engine_track,
                        "curve_id": self._id(),
                        "scale_id": scale_id,
                        "color": (layer.color or "#1972b8").lower(),
                        "line_width_mm": 0.35,
                        "z_order": li,
                        "visible": True,
                    }
                )
        self.state = {
            "tracks": tracks,
            "scales": scales,
            "curve_layers": layers,
        }

    # --- engine API surface -------------------------------------------------
    def apply_track_command(self, payload: dict[str, Any]) -> dict[str, Any]:
        op = str(payload.get("op"))
        self.calls.append(dict(payload))
        if op in self.reject_ops:
            raise RuntimeError(f"{op} rejected")
        # Entity-existence gates mirroring the engine's track_entity_missing.
        if op in ("remove_track", "resize_track"):
            if not any(
                t["id"] == payload.get("track_id")
                for t in self.state["tracks"]
            ):
                raise RuntimeError("track not found")
        if op == "set_scale":
            if not any(
                sc["id"] == payload.get("scale_id")
                for sc in self.state["scales"]
            ):
                raise RuntimeError("scale not found")
        if op in ("move_curve_layer", "unbind_curve", "set_layer_style"):
            if not any(
                l["id"] == payload.get("layer_id")
                for l in self.state["curve_layers"]
            ):
                raise RuntimeError("layer not found")
        if op == "resize_track":
            for t in self.state["tracks"]:
                if t["id"] == payload["track_id"]:
                    t["width_mm"] = float(payload["width_mm"])
        elif op == "set_scale":
            for s in self.state["scales"]:
                if s["id"] == payload["scale_id"]:
                    s.update(
                        {
                            k: payload[k]
                            for k in (
                                "minimum",
                                "maximum",
                                "mode",
                                "direction",
                                "unit",
                            )
                            if k in payload
                        }
                    )
        elif op == "reorder_tracks":
            order = list(payload["track_ids"])
            for position, track_id in enumerate(order):
                for t in self.state["tracks"]:
                    if t["id"] == track_id:
                        t["z_order"] = position
        elif op == "reorder_curve_layers":
            order = list(payload["layer_ids"])
            layers = [
                l
                for l in self.state["curve_layers"]
                if l["track_id"] == payload["track_id"]
            ]
            for position, layer_id in enumerate(order):
                for l in layers:
                    if l["id"] == layer_id:
                        l["z_order"] = position
        elif op == "set_layer_style":
            for layer in self.state["curve_layers"]:
                if layer["id"] == payload["layer_id"]:
                    if "color" in payload:
                        layer["color"] = payload["color"]
        elif op == "move_curve_layer":
            for layer in self.state["curve_layers"]:
                if layer["id"] == payload["layer_id"]:
                    layer["track_id"] = payload["target_track_id"]
                    layer["z_order"] = 99
        elif op == "remove_track":
            removed = payload["track_id"]
            self.state["tracks"] = [
                t for t in self.state["tracks"] if t["id"] != removed
            ]
            self.state["scales"] = [
                sc for sc in self.state["scales"] if sc["track_id"] != removed
            ]
            self.state["curve_layers"] = [
                l
                for l in self.state["curve_layers"]
                if l["track_id"] != removed
            ]
        elif op == "unbind_curve":
            self.state["curve_layers"] = [
                l
                for l in self.state["curve_layers"]
                if l["id"] != payload["layer_id"]
            ]
        elif op == "add_track":
            new_track = {
                "id": self._id(),
                "width_mm": float(payload.get("width_mm", 40.0)),
                "z_order": len(self.state["tracks"]),
                "visible": True,
                "header_height_mm": 0.0,
            }
            self.state["tracks"].append(new_track)
            return {
                "revision": 2,
                "state_version": 2,
                "track_id": new_track["id"],
            }
        elif op == "bind_curve":
            # Mirror the engine's reuse-or-create scale policy: first scale
            # in the track with a matching unit, else a generated one.
            track_scales = [
                sc
                for sc in self.state["scales"]
                if sc["track_id"] == payload["track_id"]
            ]
            if track_scales:
                scale_id = track_scales[0]["id"]
            else:
                scale_id = self._id()
                self.state["scales"].append(
                    {
                        "id": scale_id,
                        "track_id": payload["track_id"],
                        "minimum": 0.0,
                        "maximum": 1.0,
                        "mode": "linear",
                        "direction": "left_to_right",
                        "unit": "API",
                    }
                )
            self.state["curve_layers"].append(
                {
                    "id": self._id(),
                    "track_id": payload["track_id"],
                    "curve_id": payload["curve_id"],
                    "scale_id": scale_id,
                    "color": payload.get("color", "#1f72b8"),
                    "line_width_mm": 0.35,
                    "z_order": 50,
                    "visible": True,
                }
            )
        else:
            raise RuntimeError(f"unknown op {op}")
        return {"revision": 2, "state_version": 2}

    def remove_track(self, track_id: str) -> None:  # helper for tests
        self.apply_track_command(
            {"op": "remove_track", "document_id": DOC, "track_id": track_id}
        )

    def presentation_state(self, document_id: str) -> dict[str, Any]:
        if "-" not in document_id or len(document_id) != 36:
            return None
        return {
            "tracks": [dict(t) for t in self.state["tracks"]],
            "scales": [dict(s) for s in self.state["scales"]],
            "curve_layers": [dict(l) for l in self.state["curve_layers"]],
        }


class NoCommandsView:
    """Older wheel without the track-command surface."""


# ---------------------------------------------------------------------------
# wrappers
# ---------------------------------------------------------------------------


def test_track_command_supported_detects_surface() -> None:
    assert track_command_supported(FakeEngineView())
    assert not track_command_supported(NoCommandsView())
    assert not track_command_supported(object())


def test_apply_track_op_raises_typed_error() -> None:
    view = FakeEngineView()
    with pytest.raises(EngineSubmitError):
        apply_track_op(NoCommandsView(), "add_track", document_id=DOC)
    with pytest.raises(EngineSubmitError):
        apply_track_op(view, "remove_track", document_id=DOC,
                       track_id="ffffffff-0000-4000-8000-0000000000fe")


def test_introspection_helpers_degrade_to_none() -> None:
    view = NoCommandsView()
    assert engine_hover_info(view) is None
    assert engine_selection_state(view) is None
    assert engine_set_row_selection(view, DOC, 0, 1) is None


# ---------------------------------------------------------------------------
# capture + incremental sync
# ---------------------------------------------------------------------------


def test_capture_binds_host_track_ids_to_engine_entities() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)
    assert bindings is not None
    assert set(bindings) == {"__curves__", "track-gr", "track-rt"}
    # The persistent identity → engine curve id cache rides along.
    assert bindings["__curves__"]["GR"]
    gr = bindings["track-gr"]
    assert gr["engine_track"] in {t["id"] for t in view.state["tracks"]}
    assert gr["engine_scale"]
    assert set(gr["layers"]) == {"GR"}
    assert gr["curves"]["GR"]


def test_no_op_sync_emits_nothing() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)
    refreshed = incremental_presentation_sync(view, pres, bindings)
    assert refreshed is not None
    assert view.calls == []


def test_scale_edit_applies_set_scale_command() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    # Host edit: min 10 / max 90 / log / reversed.
    track = next(t for t in pres.tracks if t.id == "track-gr")
    track.scale.min = 10.0
    track.scale.max = 90.0
    track.scale.mode = "log"
    track.scale.reverse = True

    refreshed = incremental_presentation_sync(view, pres, bindings)
    assert refreshed is not None
    ops = [c["op"] for c in view.calls]
    assert ops == ["set_scale"]
    call = view.calls[0]
    assert call["minimum"] == 10.0
    assert call["maximum"] == 90.0
    assert call["mode"] == "logarithmic"
    assert call["direction"] == "right_to_left"
    # Engine state updated.
    scale = next(
        s for s in view.state["scales"] if s["id"] == call["scale_id"]
    )
    assert scale["minimum"] == 10.0
    assert scale["mode"] == "logarithmic"


def test_width_edit_applies_resize_command() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    track = next(t for t in pres.tracks if t.id == "track-rt")
    track.width_fraction = 0.5
    assert incremental_presentation_sync(view, pres, bindings) is not None
    assert [c["op"] for c in view.calls] == ["resize_track"]
    assert view.calls[0]["width_mm"] == pytest.approx(60.0)


def test_host_reorder_applies_reorder_command() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    gr = next(t for t in pres.tracks if t.id == "track-gr")
    rt = next(t for t in pres.tracks if t.id == "track-rt")
    # Move RT before GR (a real order swap).
    pres.tracks.remove(rt)
    pres.tracks.insert(pres.tracks.index(gr), rt)

    assert incremental_presentation_sync(view, pres, bindings) is not None
    assert [c["op"] for c in view.calls] == ["reorder_tracks"]
    want = [bindings["track-rt"]["engine_track"],
            bindings["track-gr"]["engine_track"]]
    assert view.calls[0]["track_ids"] == want
    z = {
        t["id"]: t["z_order"]
        for t in view.state["tracks"]
    }
    assert z[want[0]] < z[want[1]]


def test_curve_removal_removes_empty_track_with_cascade() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    # Host: remove GR from its track (display set uncheck equivalent). The
    # empty track leaves the payload too, so engine parity removes the track
    # — cascading its scale/layer, with no dangling references.
    track = next(t for t in pres.tracks if t.id == "track-gr")
    track.layers = []

    refreshed = incremental_presentation_sync(view, pres, bindings)
    assert refreshed is not None
    assert [c["op"] for c in view.calls] == ["remove_track"]
    removed = bindings["track-gr"]["engine_track"]
    assert all(t["id"] != removed for t in view.state["tracks"])
    assert all(l["track_id"] != removed for l in view.state["curve_layers"])
    assert "track-gr" not in refreshed


def test_layer_removal_from_multi_curve_track_applies_unbind() -> None:
    view = FakeEngineView()
    pres = _presentation([("track-gr", ["GR", "RT"], 0.4), ("track-den", ["DEN"], 0.3)])
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    # Remove ONE of the two layers: the track survives, the layer unbinds.
    track = next(t for t in pres.tracks if t.id == "track-gr")
    track.layers = [l for l in track.layers if l.mnemonic != "GR"]

    refreshed = incremental_presentation_sync(view, pres, bindings)
    assert refreshed is not None
    assert [c["op"] for c in view.calls] == ["unbind_curve"]
    layer_id = bindings["track-gr"]["layers"]["GR"]
    assert all(l["id"] != layer_id for l in view.state["curve_layers"])
    # The refreshed bindings dropped the identity but keep the curve id.
    assert "GR" not in refreshed["track-gr"]["layers"]
    assert refreshed["track-gr"]["curves"]["GR"]


def test_curve_readd_applies_bind_command_with_cached_curve_id() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    track = next(t for t in pres.tracks if t.id == "track-rt")
    track.layers = []
    bindings = incremental_presentation_sync(view, pres, bindings)
    assert bindings is not None

    # Re-check RT (same curve, empty track re-appears): add_track + bind
    # with the cached engine curve id — the engine document still holds the
    # curve, so no raw data is re-sent.
    track.layers = [_layer("RT")]
    refreshed = incremental_presentation_sync(view, pres, bindings)
    assert refreshed is not None
    ops = [c["op"] for c in view.calls]
    assert "add_track" in ops
    assert "bind_curve" in ops
    bind = next(c for c in view.calls if c["op"] == "bind_curve")
    assert bind["track_id"] == refreshed["track-rt"]["engine_track"]
    # The cached engine curve id survived the track removal.
    assert bind["curve_id"] == bindings["__curves__"]["RT"]


def test_cross_track_move_applies_move_command() -> None:
    view = FakeEngineView()
    pres = _presentation(
        [("track-a", ["GR", "NPHI"], 0.4), ("track-b", ["RT", "DEN"], 0.4)]
    )
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    # Move RT into track-a (both tracks keep layers, so both survive).
    track_a = next(t for t in pres.tracks if t.id == "track-a")
    track_b = next(t for t in pres.tracks if t.id == "track-b")
    track_a.layers.append(track_b.layers.pop(0))

    refreshed = incremental_presentation_sync(view, pres, bindings)
    assert refreshed is not None
    ops = [c["op"] for c in view.calls]
    assert ops == ["move_curve_layer"]
    move = view.calls[0]
    assert move["target_track_id"] == bindings["track-a"]["engine_track"]
    layer = next(
        l for l in view.state["curve_layers"]
        if l["id"] == bindings["track-b"]["layers"]["RT"]
    )
    assert layer["track_id"] == bindings["track-a"]["engine_track"]
    # The refreshed bindings moved the identity to track-a.
    assert "RT" in refreshed["track-a"]["layers"]
    assert "RT" not in refreshed["track-b"]["layers"]


def test_new_curve_identity_falls_back_to_full_path() -> None:
    view = FakeEngineView()
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    track = next(t for t in pres.tracks if t.id == "track-gr")
    track.layers.append(_layer("NPHI"))

    assert incremental_presentation_sync(view, pres, bindings) is None
    assert view.calls == []


def test_rejected_command_falls_back_without_partial_damage() -> None:
    view = FakeEngineView(reject_ops={"set_scale"})
    pres = _presentation()
    view.submit_snapshot(pres)
    bindings = capture_engine_bindings(view, DOC, pres)

    track = next(t for t in pres.tracks if t.id == "track-gr")
    track.scale.min = 10.0
    track.scale.max = 90.0

    assert incremental_presentation_sync(view, pres, bindings) is None
    # The attempted op is visible; the caller falls back to the full path
    # (each command is atomic, so the engine is never mid-edit).
    assert any(c["op"] == "set_scale" for c in view.calls)


def test_missing_bindings_or_state_fall_back() -> None:
    view = FakeEngineView()
    pres = _presentation()
    assert incremental_presentation_sync(view, pres, None) is None
    # State not submitted yet → capture returns None, sync falls back.
    assert capture_engine_bindings(view, DOC, pres) is None
    assert incremental_presentation_sync(view, pres, {}) is None
    # Non-UUID document ids never sync incrementally.
    pres2 = _presentation()
    pres2.well_document_id = "not-a-uuid"
    view.submit_snapshot(pres2)
    assert capture_engine_bindings(view, "not-a-uuid", pres2) is None
