"""Unit tests for WellSectionDatum multi-mode vertical alignment policy (Ticket 01)."""

from __future__ import annotations

import numpy as np
import pytest
from paleo_workbench.env_bootstrap import ensure_geoviz_on_path

if not ensure_geoviz_on_path():
    # Importing paleo_workbench.viz executes its package __init__ chain
    # (viz.adapter -> viz.prediction_helpers), which imports geoviz at module
    # level. The workstation host job does not put the geo-viz-engine packages
    # on PYTHONPATH (K-F3); skip at collection with a clear reason instead of
    # failing the whole directory run.
    pytest.skip("geoviz not importable in this environment", allow_module_level=True)

from paleo_workbench.viz.well_section_datum import WellSectionDatum


def test_well_section_datum_md_mode():
    engine = WellSectionDatum()
    wells = [{"name": "Well-1"}, {"name": "Well-2"}]
    shifts = engine.compute_shifts(wells, mode="md")

    assert shifts == {"Well-1": 0.0, "Well-2": 0.0}


def test_well_section_datum_tvdss_mode():
    engine = WellSectionDatum()
    wells = [{"name": "Well-1"}, {"name": "Well-2"}]
    kb = {"Well-1": 15.0, "Well-2": 25.0}

    shifts = engine.compute_shifts(wells, mode="tvdss", kb_elevations=kb)

    assert shifts["Well-1"] == -15.0
    assert shifts["Well-2"] == -25.0


def test_well_section_datum_horizon_flattening():
    engine = WellSectionDatum()
    wells = [
        {"name": "Well-1", "tops": [{"name": "H1", "depth": 1200.0}, {"name": "H2", "depth": 1400.0}]},
        {"name": "Well-2", "tops": [{"name": "H1", "depth": 1280.0}, {"name": "H2", "depth": 1490.0}]},
    ]

    shifts = engine.compute_shifts(wells, mode="horizon", target_horizon="H1")

    assert shifts["Well-1"] == -1200.0
    assert shifts["Well-2"] == -1280.0

    # Depth transformation shifts target horizon depth to Z=0
    d1 = np.array([1000.0, 1200.0, 1400.0], dtype=np.float32)
    aligned_d1 = engine.transform_well_depths(d1, shifts["Well-1"])
    assert np.allclose(aligned_d1, [-200.0, 0.0, 200.0])
