"""welllog engine bindings — export_scene_pdf crop marks / layered PDF (FRS §5).

Exercises the Shiboken-bound ``WellLogView.export_scene_pdf`` through the
installed welllog wheel: the new trailing optional ``crop_marks`` /
``layered_pdf`` bools must produce the OCG machinery and marked content, and
stay byte-compatible with the default (no OCG) output. Skipped when the
engine wheel is not installed (probe_engine() fails).
"""

from __future__ import annotations

import os
import zlib

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

# Host-only runs (parent workbench job: WLWS_DISABLE_ENGINE=1, no wheel)
# skip at collection instead of aborting the directory with ImportError.
# Strict binding-first runs (WLWS_REQUIRE_NATIVE_BINDING=1) keep the hard
# import so a broken wheel FAILS rather than silently skipping.
if os.environ.get("WLWS_REQUIRE_NATIVE_BINDING") != "1":
    pytest.importorskip(
        "welllog",
        reason="welllog engine wheel not installed (host-only run)",
        exc_type=ModuleNotFoundError,
    )
from welllog import WellLogView  # noqa: E402

from well_log_workstation.engine_bridge import (  # noqa: E402
    engine_available,
    reset_engine_capability_cache,
)

DOC_ID = "60000000-0000-4000-8000-0000000000aa"
AXIS_ID = "60000000-0000-4000-8000-0000000000ab"
CURVE_ID = "60000000-0000-4000-8000-0000000000ac"


def _submit_small_scene(view: WellLogView) -> None:
    depth = np.linspace(1000.0, 1010.0, 11)
    depth.setflags(write=False)
    values = np.linspace(0.0, 100.0, 11)
    values.setflags(write=False)
    result = view.submit_curve(
        depth, values, DOC_ID, AXIS_ID, CURVE_ID, "GR", "m", "API"
    )
    assert result, f"submit_curve failed: {result}"


def _inflate_first_stream(data: bytes) -> bytes:
    start = data.index(b"stream\n") + len(b"stream\n")
    end = data.index(b"\nendstream", start)
    return zlib.decompress(data[start:end])


@pytest.fixture(autouse=True)
def _engine_or_skip(monkeypatch) -> None:
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    reset_engine_capability_cache()
    if not engine_available():
        pytest.skip("welllog engine wheel not installed")


def test_default_pdf_has_no_ocg(qtbot) -> None:
    view = WellLogView()
    qtbot.addWidget(view)
    _submit_small_scene(view)
    plain = view.export_scene_pdf(DOC_ID)
    assert b"/OCProperties" not in plain
    assert b"/Properties" not in plain
    assert b"/Lay0" not in plain


def test_layered_pdf_emits_ocg_per_track(qtbot) -> None:
    view = WellLogView()
    qtbot.addWidget(view)
    _submit_small_scene(view)
    layered = view.export_scene_pdf(DOC_ID, 0, False, True, True)
    assert b"/OCProperties" in layered
    assert b"/Type /OCG /Name (track-0)" in layered
    assert b"/Properties" in layered
    assert b"/Lay0" in layered
    # The marked content lives in the (Flate-compressed) content stream.
    inflated = _inflate_first_stream(layered)
    assert b"/Lay0 OC BMC" in inflated
    assert b"EMC" in inflated


def test_keyword_and_positional_calls_agree(qtbot) -> None:
    view = WellLogView()
    qtbot.addWidget(view)
    _submit_small_scene(view)
    positional = view.export_scene_pdf(DOC_ID, 0, False, True, True)
    keyword = view.export_scene_pdf(
        DOC_ID, searchable_text=False, crop_marks=True, layered_pdf=True
    )
    assert keyword == positional


def test_crop_marks_without_layers(qtbot) -> None:
    view = WellLogView()
    qtbot.addWidget(view)
    _submit_small_scene(view)
    plain = view.export_scene_pdf(DOC_ID)
    marks = view.export_scene_pdf(DOC_ID, 0, False, True, False)
    assert b"/OCProperties" not in marks  # crop marks alone → no OCG
    assert len(marks) > len(plain)  # registration strokes add content
