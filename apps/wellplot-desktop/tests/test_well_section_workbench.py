from __future__ import annotations

from PySide6.QtWidgets import QApplication
from geoviz import DatumTransformer, WellLogData, WellSectionCanvas
from paleo_workbench.ui.pages.composite_visualization_panel import CompositeVisualizationPanel
from paleo_workbench.viz.hosts.well_section_host import WellSectionHost
from paleo_workbench.viz.models import VizPayload


def test_well_section_canvas_set_wells_and_datum_modes(qtbot):
    canvas = WellSectionCanvas()
    qtbot.addWidget(canvas)

    w1 = WellLogData(well_name="HZ26-6-1", top_depth=3000.0, bottom_depth=3500.0)
    w2 = WellLogData(well_name="HZ27-10-1", top_depth=3100.0, bottom_depth=3600.0)

    canvas.set_wells([w1, w2])
    assert len(canvas._wells) == 2
    assert canvas.transformer.global_min_depth == 3000.0
    assert canvas.transformer.global_max_depth == 3600.0

    # Test datum shift mode
    canvas.set_datum_mode("datum_shift", datum_name="C6")
    assert canvas.transformer.mode == "datum_shift"
    assert canvas.transformer.datum_name == "C6"

    # Test inter-well spacing
    canvas.set_inter_well_spacing(200)
    assert canvas._inter_well_spacing == 200

    # Test facies fills toggle
    canvas.set_show_facies_fills(False)
    assert canvas._show_facies_fills is False


def test_well_section_host_apply_payload(qtbot):
    host = WellSectionHost()
    qtbot.addWidget(host.widget)

    w1 = WellLogData(well_name="A1", top_depth=1000.0, bottom_depth=2000.0)
    w2 = WellLogData(well_name="A2", top_depth=1100.0, bottom_depth=2100.0)

    payload = VizPayload(
        kind="well_log",
        label="Cross Well Section",
        well_log=w1,
        well_logs=[w1, w2],
    )

    res = host.apply(payload)
    assert res is True
    assert len(host.canvas._wells) == 2

    # Clear
    host.clear()
    assert len(host.canvas._wells) == 0


def test_composite_panel_includes_well_section_tab(qtbot):
    panel = CompositeVisualizationPanel()
    qtbot.addWidget(panel)

    assert hasattr(panel, "well_section_host")
    assert panel.well_section_host is not None
    assert panel.tabs.count() == 7

    tab_titles = [panel.tabs.tabText(i) for i in range(panel.tabs.count())]
    assert "多井对比剖面" in tab_titles


def test_stratigraphy_correlation_page_has_correlation_engine(qtbot):
    from paleo_workbench.ui.pages.stratigraphy_correlation_page import StratigraphyCorrelationPage
    page = StratigraphyCorrelationPage()
    qtbot.addWidget(page)

    assert hasattr(page, "correlation_engine")
    assert page.correlation_engine is not None

