"""Main window chrome for WellPlot Desktop — L layout (#216–#222, brand #290)."""

from __future__ import annotations

import math
import uuid
import zipfile
from pathlib import Path
from typing import AbstractSet, Any, Iterable
from xml.etree import ElementTree as ET

import numpy as np
from PySide6.QtCore import QRectF, Qt, QTimer
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QSplitter,
    QStackedWidget,
    QStatusBar,
    QTableView,
    QTabWidget,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from well_log_workstation import __version__
from well_log_workstation.branding import (
    PRODUCT_NAME,
    about_text,
    window_title,
)
from well_log_workstation.correlation_canvas import CorrelationCanvas
from well_log_workstation.correlation_links import (
    HorizonLink,
    make_horizon_link,
    match_tops_by_name,
)
from well_log_workstation.composite_view import CompositeView
from well_log_workstation.crs_dialog import CoordinateReferenceDialog
from well_log_workstation.datum.well_section_datum import WellSectionDatum
from well_log_workstation.engine_bridge import (
    EngineSubmitError,
    EngineUnavailable,
    create_well_log_view,
    engine_available,
    load_presentation_into_view,
    probe_engine,
    submit_multi_well_presentations,
)
from well_log_workstation.events import emit_plot_changed
from well_log_workstation.command_audit import audit as audit_command
from well_log_workstation.export_dispatch import (
    CGM_EXPORT_DISCLOSURE,
    ENGINE_PDF_NONSEARCHABLE_DISCLOSURE,
    PDF_SEARCHABLE_MODE_NOTE,
    ExportFormat,
    PageSpec,
    PdfTextMode,
    UnsupportedFormatError,
    engine_pdf_needs_disclosure,
    export_plot,
    prefer_engine_for_single_well,
    resolve_single_well_pdf_export,
)
from well_log_workstation.print_preview import (
    PrintPreviewDialog,
    compute_print_preview,
    depth_range_from_presentation,
)
from well_log_workstation.export_plot import (
    ExportError,
    export_presentation_pdf,
    export_presentation_svg,
)
from well_log_workstation.fence_view import FenceView
from well_log_workstation.las_import import LasImportError, import_las_into_workspace
from well_log_workstation.multi_track_canvas import MultiTrackCanvas
from well_log_workstation.plane_map_view import PlaneMapView
from well_log_workstation.plot_document import (
    PanelRef,
    PlotDocument,
    create_composite_plot,
    create_correlation_plot,
    create_fence_3d_plot,
    create_plane_map_plot,
    create_section_plot,
    create_single_well_plot,
    load_plot_document,
    save_plot_document,
    sync_data_bindings,
)
from well_log_workstation.plot_io import (
    export_plot_excel,
    export_plot_xml,
    import_plot_excel,
    import_plot_xml,
)
from well_log_workstation.qt_platform import effective_qt_platform_hint
from well_log_workstation.section_canvas import SectionCanvas
from well_log_workstation.section_geometry import (
    ContactSegment2D,
    FaultSegment2D,
    TieQuad2D,
    contact_polyline,
    curtain_slice_fault,
    tie_quads,
)
from well_log_workstation.session_store import HostSessionStore
from well_log_workstation.display_set import (
    default_checks,
    leaf_id_for_curve,
    leaves_from_document,
    presentation_from_display_set,
)
from well_log_workstation.semantic_selection import (
    SemanticSelection,
    selection_from_depth,
    selection_from_row,
)
from well_log_workstation.table_projection import (
    LogTableModel,
    PROJECTION_BUILD_HOOKS,
    SOFT_COLUMN_TIP_THRESHOLD,
    build_table_projections_guarded,
    export_projection_rows,
    selection_html,
    selection_tsv,
)
from well_log_workstation.template_model import (
    HostPresentation,
    PlotTemplate,
    apply_template,
    apply_track_overrides,
    get_builtin_template,
    list_builtin_templates,
    track_overrides_snapshot,
)
from well_log_workstation.three_d_bridge import probe_3d
from well_log_workstation.tops_history import TopsHistoryBook
from well_log_workstation.tops_model import (
    FormationTop,
    TopsError,
    import_tops_from_json_file,
    load_tops_for_well,
    make_stub_tops,
    save_tops_for_well,
)
from well_log_workstation.recent_workspaces import add_recent, remove_recent
from well_log_workstation.startup_page import StartupPage
from well_log_workstation.workspace import (
    Workspace,
    WorkspaceError,
    create_workspace,
    open_workspace,
)


class WellLogWorkstationWindow(QMainWindow):
    """Log-first shell: left tree · center document tabs · right inspector."""

    def __init__(self) -> None:
        super().__init__()
        self.setObjectName("WellLogWorkstationWindow")
        self.setWindowTitle(window_title())
        self.resize(1280, 800)

        self._workspace: Workspace | None = None
        self.session = HostSessionStore()
        self._selected_well_id: str | None = None
        self._active_plot_id: str | None = None
        self._active_plot_type: str | None = None
        self._presentation: HostPresentation | None = None
        self._correlation_presentations: list[HostPresentation] = []
        self._correlation_links: list[HorizonLink] = []
        self._link_pick_first: tuple[str, FormationTop] | None = None
        self._active_tops: list[FormationTop] = []
        self._tops_diagnostics: list[str] = []
        self._tops_history = TopsHistoryBook()
        self._templates: list[PlotTemplate] = list_builtin_templates()
        # Display Set session cache. Keys: "plot:<id>" (preferred for single-well
        # plots) or "well:<id>" (preview without a plot). Model A: data on well,
        # checks belong to the plot document when a plot is active.
        self._display_sets: dict[str, frozenset[str]] = {}
        # Per-well view mode: "graphic" | "table" (T4 #344); default graphic.
        self._view_modes: dict[str, str] = {}
        self._view_mode: str = "graphic"
        # Semantic selection (T5 #345 / ADR 0024) — not screen coordinates
        self._semantic_selection: SemanticSelection | None = None
        self._selection_guard = False
        self._content_tree_guard = False
        # Table UX (T6 #346)
        self._table_build_cancel: list[bool] = [False]
        self._table_performance_mode = False
        self._table_last_error: str | None = None
        # #227: prefer native WellLogView as primary single-well surface when
        # welllog is available. Host MultiTrackCanvas is always the fallback.
        self._prefer_engine_canvas = self._default_prefer_engine()
        self._primary_surface: str = "host"  # "host" | "engine"
        self._engine_last_error: str | None = None

        self._build_menus()
        self._build_body()
        self._build_status()
        self._populate_templates()
        self._refresh_tree()
        self._refresh_tops_list()

    @property
    def workspace(self) -> Workspace | None:
        return self._workspace

    @property
    def active_presentation(self) -> HostPresentation | None:
        return self._presentation

    @property
    def active_plot_id(self) -> str | None:
        return self._active_plot_id

    @property
    def active_plot_type(self) -> str | None:
        return self._active_plot_type

    def _build_menus(self) -> None:
        bar = self.menuBar()
        file_menu = bar.addMenu("文件")
        file_menu.setObjectName("Menu_文件")
        act_new = file_menu.addAction("新建工区…")
        act_new.setObjectName("Action_NewWorkspace")
        act_new.triggered.connect(self._on_new_workspace)
        act_open = file_menu.addAction("打开工区…")
        act_open.setObjectName("Action_OpenWorkspace")
        act_open.triggered.connect(self._on_open_workspace)
        file_menu.addSeparator()
        self._act_import_las = file_menu.addAction("导入 LAS 到井…")
        self._act_import_las.setObjectName("Action_ImportLas")
        self._act_import_las.triggered.connect(self._on_import_las)
        self._act_import_las.setEnabled(False)
        self._act_import_plot_xml = file_menu.addAction("导入井图定义 XML…")
        self._act_import_plot_xml.setObjectName("Action_ImportPlotXml")
        self._act_import_plot_xml.triggered.connect(self._on_import_plot_xml)
        self._act_import_plot_xml.setEnabled(False)
        self._act_import_plot_xlsx = file_menu.addAction("导入井图定义 Excel…")
        self._act_import_plot_xlsx.setObjectName("Action_ImportPlotXlsx")
        self._act_import_plot_xlsx.triggered.connect(self._on_import_plot_xlsx)
        self._act_import_plot_xlsx.setEnabled(False)
        file_menu.addSeparator()
        act_quit = file_menu.addAction("退出")
        act_quit.triggered.connect(self.close)

        plot_menu = bar.addMenu("图件")
        plot_menu.setObjectName("Menu_图件")
        self._act_new_single_plot = plot_menu.addAction("新建单井分析图…")
        self._act_new_single_plot.setObjectName("Action_NewSingleWellPlot")
        self._act_new_single_plot.triggered.connect(self._on_new_single_well_plot)
        self._act_new_single_plot.setEnabled(False)
        self._act_new_correlation = plot_menu.addAction("新建地层对比图…")
        self._act_new_correlation.setObjectName("Action_NewCorrelationPlot")
        self._act_new_correlation.triggered.connect(self._on_new_correlation_plot)
        self._act_new_correlation.setEnabled(False)
        # Phase-2 PR-C: plane_map + fence_3d (section/composite land in
        # later PR-C batches; their menu items are stubs until then).
        self._act_new_plane_map = plot_menu.addAction("新建平面图…")
        self._act_new_plane_map.setObjectName("Action_NewPlaneMapPlot")
        self._act_new_plane_map.triggered.connect(self._on_new_plane_map_plot)
        self._act_new_plane_map.setEnabled(False)
        self._act_new_fence_3d = plot_menu.addAction("新建三维栅状图…")
        self._act_new_fence_3d.setObjectName("Action_NewFence3dPlot")
        self._act_new_fence_3d.triggered.connect(self._on_new_fence_3d_plot)
        self._act_new_fence_3d.setEnabled(False)
        self._act_new_section = plot_menu.addAction("新建油藏剖面…")
        self._act_new_section.setObjectName("Action_NewSectionPlot")
        self._act_new_section.triggered.connect(self._on_new_section_plot)
        self._act_new_section.setEnabled(False)
        self._act_new_composite = plot_menu.addAction("新建油藏综合图…")
        self._act_new_composite.setObjectName("Action_NewCompositePlot")
        self._act_new_composite.triggered.connect(self._on_new_composite_plot)
        self._act_new_composite.setEnabled(False)
        plot_menu.addSeparator()
        self._act_add_leaf_to_plot = plot_menu.addAction("将选中井道加入当前井图")
        self._act_add_leaf_to_plot.setObjectName("Action_AddLeafToPlot")
        self._act_add_leaf_to_plot.triggered.connect(self._on_add_selected_leaf_to_plot)
        self._act_add_leaf_to_plot.setEnabled(False)
        self._act_export_plot_xml = plot_menu.addAction("导出井图定义 XML…")
        self._act_export_plot_xml.setObjectName("Action_ExportPlotXml")
        self._act_export_plot_xml.triggered.connect(self._on_export_plot_xml)
        self._act_export_plot_xml.setEnabled(False)
        self._act_export_plot_xlsx = plot_menu.addAction("导出井图定义 Excel…")
        self._act_export_plot_xlsx.setObjectName("Action_ExportPlotXlsx")
        self._act_export_plot_xlsx.triggered.connect(self._on_export_plot_xlsx)
        self._act_export_plot_xlsx.setEnabled(False)
        plot_menu.addSeparator()
        self._act_set_crs = plot_menu.addAction("坐标系设置（CRS）…")
        self._act_set_crs.setObjectName("Action_SetCoordinateReference")
        self._act_set_crs.triggered.connect(self._on_set_coordinate_reference)
        self._act_set_crs.setEnabled(False)
        self._act_auto_links = plot_menu.addAction("按层位名自动连线")
        self._act_auto_links.setObjectName("Action_AutoHorizonLinks")
        self._act_auto_links.triggered.connect(self._on_auto_horizon_links)
        self._act_auto_links.setEnabled(False)
        self._act_clear_links = plot_menu.addAction("清除对比连线")
        self._act_clear_links.setObjectName("Action_ClearHorizonLinks")
        self._act_clear_links.triggered.connect(self._on_clear_horizon_links)
        self._act_clear_links.setEnabled(False)
        self._act_pick_links = plot_menu.addAction("点选层位连线")
        self._act_pick_links.setObjectName("Action_PickHorizonLinks")
        self._act_pick_links.setCheckable(True)
        self._act_pick_links.triggered.connect(self._on_toggle_pick_links)
        self._act_pick_links.setEnabled(False)
        plot_menu.addSeparator()
        self._act_prefer_engine = plot_menu.addAction("优先使用引擎画布")
        self._act_prefer_engine.setObjectName("Action_PreferEngineCanvas")
        self._act_prefer_engine.setCheckable(True)
        self._act_prefer_engine.setChecked(self._prefer_engine_canvas)
        self._act_prefer_engine.triggered.connect(self._on_toggle_prefer_engine)
        self._act_engine_preview = plot_menu.addAction("刷新/打开引擎视图…")
        self._act_engine_preview.setObjectName("Action_EnginePreview")
        self._act_engine_preview.triggered.connect(self._on_engine_preview)
        self._act_engine_preview.setEnabled(False)
        self._act_engine_corr = plot_menu.addAction("引擎对比预览…")
        self._act_engine_corr.setObjectName("Action_EngineCorrelationPreview")
        self._act_engine_corr.triggered.connect(self._on_engine_correlation_preview)
        self._act_engine_corr.setEnabled(False)

        template_menu = bar.addMenu("图版")
        template_menu.setObjectName("Menu_图版")
        self._act_apply_template = template_menu.addAction("应用当前图版到选中井")
        self._act_apply_template.setObjectName("Action_ApplyTemplate")
        self._act_apply_template.triggered.connect(self._on_apply_template)
        self._act_apply_template.setEnabled(False)

        export_menu = bar.addMenu("导出")
        export_menu.setObjectName("Menu_导出")
        self._act_export_svg = export_menu.addAction("导出 SVG…")
        self._act_export_svg.setObjectName("Action_ExportSvg")
        self._act_export_svg.triggered.connect(self._on_export_svg)
        self._act_export_svg.setEnabled(False)
        self._act_export_pdf = export_menu.addAction("导出 PDF…")
        self._act_export_pdf.setObjectName("Action_ExportPdf")
        self._act_export_pdf.triggered.connect(self._on_export_pdf)
        self._act_export_pdf.setEnabled(False)
        self._act_export_png = export_menu.addAction("导出 PNG…")
        self._act_export_png.setObjectName("Action_ExportPng")
        self._act_export_png.triggered.connect(self._on_export_png)
        self._act_export_png.setEnabled(False)
        self._act_export_cgm = export_menu.addAction("导出 CGM…")
        self._act_export_cgm.setObjectName("Action_ExportCgm")
        self._act_export_cgm.triggered.connect(self._on_export_cgm)
        self._act_export_cgm.setEnabled(False)
        export_menu.addSeparator()
        self._act_print_preview = export_menu.addAction("打印预览…")
        self._act_print_preview.setObjectName("Action_PrintPreview")
        self._act_print_preview.triggered.connect(self._on_print_preview)
        self._act_print_preview.setEnabled(False)

        tops_menu = bar.addMenu("层位")
        tops_menu.setObjectName("Menu_层位")
        self._act_import_tops = tops_menu.addAction("导入层位 JSON…")
        self._act_import_tops.setObjectName("Action_ImportTops")
        self._act_import_tops.triggered.connect(self._on_import_tops)
        self._act_import_tops.setEnabled(False)
        self._act_stub_tops = tops_menu.addAction("生成示例层位")
        self._act_stub_tops.setObjectName("Action_StubTops")
        self._act_stub_tops.triggered.connect(self._on_stub_tops)
        self._act_stub_tops.setEnabled(False)
        tops_menu.addSeparator()
        self._act_pick_tops = tops_menu.addAction("拾取层位（单击图道）")
        self._act_pick_tops.setObjectName("Action_PickTops")
        self._act_pick_tops.setCheckable(True)
        self._act_pick_tops.triggered.connect(self._on_toggle_pick_tops)
        self._act_pick_tops.setEnabled(False)
        self._act_add_top = tops_menu.addAction("按深度添加层位…")
        self._act_add_top.setObjectName("Action_AddTopByDepth")
        self._act_add_top.triggered.connect(self._on_add_top_by_depth)
        self._act_add_top.setEnabled(False)
        self._act_edit_top_depth = tops_menu.addAction("修改选中层位深度…")
        self._act_edit_top_depth.setObjectName("Action_EditTopDepth")
        self._act_edit_top_depth.triggered.connect(self._on_edit_top_depth)
        self._act_edit_top_depth.setEnabled(False)
        self._act_remove_top = tops_menu.addAction("删除选中层位")
        self._act_remove_top.setObjectName("Action_RemoveTop")
        self._act_remove_top.triggered.connect(self._on_remove_top)
        self._act_remove_top.setEnabled(False)
        tops_menu.addSeparator()
        self._act_undo_tops = tops_menu.addAction("撤销层位编辑")
        self._act_undo_tops.setObjectName("Action_UndoTops")
        self._act_undo_tops.setShortcut("Ctrl+Z")
        self._act_undo_tops.triggered.connect(self._on_undo_tops)
        self._act_undo_tops.setEnabled(False)
        self._act_redo_tops = tops_menu.addAction("重做层位编辑")
        self._act_redo_tops.setObjectName("Action_RedoTops")
        self._act_redo_tops.setShortcut("Ctrl+Shift+Z")
        self._act_redo_tops.triggered.connect(self._on_redo_tops)
        self._act_redo_tops.setEnabled(False)

        help_menu = bar.addMenu("帮助")
        help_menu.setObjectName("Menu_帮助")
        act_about = help_menu.addAction("关于…")
        act_about.setObjectName("Action_About")
        act_about.triggered.connect(self._on_about)

    def _build_body(self) -> None:
        # Stack: [0] startup welcome (no workspace) · [1] main L-shell (#291)
        self._main_stack = QStackedWidget()
        self._main_stack.setObjectName("MainStack")

        self.startup_page = StartupPage()
        self.startup_page.new_requested.connect(self._on_new_workspace)
        self.startup_page.open_requested.connect(self._on_open_workspace)
        self.startup_page.recent_open_requested.connect(self._on_open_recent_workspace)
        self._main_stack.addWidget(self.startup_page)

        shell_root = QWidget()
        shell_root.setObjectName("ShellRoot")
        outer = QVBoxLayout(shell_root)
        outer.setContentsMargins(4, 4, 4, 4)
        outer.setSpacing(4)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.setObjectName("ShellSplitter")
        split.addWidget(self._build_left())
        split.addWidget(self._build_center())
        split.addWidget(self._build_right())
        split.setSizes([240, 760, 280])
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setStretchFactor(2, 0)
        outer.addWidget(split, 1)

        self._main_stack.addWidget(shell_root)
        self.setCentralWidget(self._main_stack)
        self._main_stack.setCurrentIndex(0)  # cold start: welcome

    def _build_left(self) -> QWidget:
        """Single left tree: 工区 → 井 → 导入源 → 井道(可勾选) + 图件.

        No dual tabs — data tracks live under each well so a new plot uses
        the same tree (model A: data on well, checks drive current plot).
        """
        pane = QWidget()
        pane.setObjectName("LeftPane")
        layout = QVBoxLayout(pane)
        layout.setContentsMargins(4, 4, 4, 4)

        self.left_title = QLabel("工区")
        self.left_title.setObjectName("LeftPaneTitle")
        layout.addWidget(self.left_title)

        self.well_content_hint = QLabel(
            "树：井 → 数据源 → 井道；勾选=进当前井图（数据在井上，不在图里）"
        )
        self.well_content_hint.setObjectName("WellContentHint")
        self.well_content_hint.setWordWrap(True)
        layout.addWidget(self.well_content_hint)

        self.workspace_tree = QTreeWidget()
        self.workspace_tree.setObjectName("WorkspaceTree")
        # Alias: content checks live on the same tree (no second tab/tree).
        self.well_content_tree = self.workspace_tree
        self.workspace_tree.setHeaderLabels(["名称"])
        self.workspace_tree.setRootIsDecorated(True)
        self.workspace_tree.currentItemChanged.connect(self._on_tree_selection)
        self.workspace_tree.itemDoubleClicked.connect(self._on_tree_double_click)
        self.workspace_tree.itemChanged.connect(self._on_well_content_item_changed)
        layout.addWidget(self.workspace_tree, 1)

        self._content_tree_guard = False
        self._content_apply_pending = False
        return pane

    def _build_center(self) -> QWidget:
        pane = QWidget()
        pane.setObjectName("CenterPane")
        layout = QVBoxLayout(pane)

        self.document_tabs = QTabWidget()
        self.document_tabs.setObjectName("DocumentTabs")
        self.document_tabs.setTabsClosable(False)
        self.document_tabs.setDocumentMode(True)

        host = QWidget()
        host.setObjectName("SingleWellPlotHost")
        hl = QVBoxLayout(host)
        self.plot_caption = QLabel("单井分析图 · 多图道（选择井并应用图版）")
        self.plot_caption.setObjectName("PlotCaption")
        hl.addWidget(self.plot_caption)

        # Graphic | Table mode switch (T4 #344)
        mode_row = QHBoxLayout()
        self.btn_mode_graphic = QPushButton("图形")
        self.btn_mode_graphic.setObjectName("Button_ViewModeGraphic")
        self.btn_mode_graphic.setCheckable(True)
        self.btn_mode_graphic.setChecked(True)
        self.btn_mode_table = QPushButton("表格")
        self.btn_mode_table.setObjectName("Button_ViewModeTable")
        self.btn_mode_table.setCheckable(True)
        self.btn_mode_graphic.clicked.connect(lambda: self.set_view_mode("graphic"))
        self.btn_mode_table.clicked.connect(lambda: self.set_view_mode("table"))
        mode_row.addWidget(self.btn_mode_graphic)
        mode_row.addWidget(self.btn_mode_table)
        mode_row.addStretch(1)
        hl.addLayout(mode_row)

        self.table_column_tip = QLabel("")
        self.table_column_tip.setObjectName("TableColumnSoftTip")
        self.table_column_tip.setWordWrap(True)
        self.table_column_tip.setStyleSheet("color: #8a6d00; background: #fff8e1;")
        self.table_column_tip.hide()
        hl.addWidget(self.table_column_tip)

        # Outer stack: 0 = graphic (host/engine), 1 = table
        self.view_mode_stack = QStackedWidget()
        self.view_mode_stack.setObjectName("ViewModeStack")

        # Host vs engine primary surface (#227)
        self.single_well_stack = QStackedWidget()
        self.single_well_stack.setObjectName("SingleWellStack")
        self.multi_track_canvas = MultiTrackCanvas()
        self.multi_track_canvas.top_pick_requested.connect(self._on_canvas_top_pick)
        self.multi_track_canvas.sample_selected.connect(
            self._on_canvas_sample_selected
        )
        self.single_well_stack.addWidget(self.multi_track_canvas)  # index 0 host

        self._engine_page = QWidget()
        self._engine_page.setObjectName("SingleWellEnginePage")
        ep = QVBoxLayout(self._engine_page)
        ep.setContentsMargins(0, 0, 0, 0)
        self.engine_caption = QLabel(
            "引擎画布 · 需 welllog · 应用图版后自动提交 multi-track"
        )
        self.engine_caption.setObjectName("EngineCaption")
        ep.addWidget(self.engine_caption)
        self._engine_view = None  # WellLogView | None
        self._engine_placeholder = QLabel(
            "引擎未激活。勾选「优先使用引擎画布」并应用图版，"
            "或将 welllog 加入 PYTHONPATH。"
        )
        self._engine_placeholder.setObjectName("EnginePlaceholder")
        self._engine_placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        ep.addWidget(self._engine_placeholder, 1)
        self.single_well_stack.addWidget(self._engine_page)  # index 1 engine page
        self.view_mode_stack.addWidget(self.single_well_stack)  # 0 graphic

        # Table mode page (virtualized QTableView; multi-axis via tabs)
        table_page = QWidget()
        table_page.setObjectName("SingleWellTablePage")
        tl = QVBoxLayout(table_page)
        tl.setContentsMargins(0, 0, 0, 0)
        self.table_axis_tabs = QTabWidget()
        self.table_axis_tabs.setObjectName("TableAxisTabs")
        self.table_axis_tabs.setDocumentMode(True)
        self._table_models: list[LogTableModel] = []
        self._primary_table_model = LogTableModel()
        primary_view = QTableView()
        primary_view.setObjectName("LogTableView")
        primary_view.setModel(self._primary_table_model)
        primary_view.setAlternatingRowColors(True)
        primary_view.setSelectionBehavior(
            QAbstractItemView.SelectionBehavior.SelectRows
        )
        primary_view.setSelectionMode(
            QAbstractItemView.SelectionMode.SingleSelection
        )
        primary_view.verticalHeader().setDefaultSectionSize(20)
        primary_view.selectionModel().selectionChanged.connect(
            self._on_table_selection_changed
        )
        self._primary_table_view = primary_view
        self.table_axis_tabs.addTab(primary_view, "主表")
        tl.addWidget(self.table_axis_tabs, 1)

        # Progress / cancel / error / performance (T6)
        prog_row = QHBoxLayout()
        self.table_progress = QProgressBar()
        self.table_progress.setObjectName("TableBuildProgress")
        self.table_progress.setRange(0, 0)  # indeterminate until stepped
        self.table_progress.hide()
        self.table_cancel_btn = QPushButton("取消")
        self.table_cancel_btn.setObjectName("Button_TableBuildCancel")
        self.table_cancel_btn.clicked.connect(self._on_cancel_table_build)
        self.table_cancel_btn.hide()
        prog_row.addWidget(self.table_progress, 1)
        prog_row.addWidget(self.table_cancel_btn)
        tl.addLayout(prog_row)

        self.table_error_banner = QLabel("")
        self.table_error_banner.setObjectName("TableErrorBanner")
        self.table_error_banner.setWordWrap(True)
        self.table_error_banner.setStyleSheet(
            "color: #7a1f1f; background: #fdecea; padding: 6px;"
        )
        self.table_error_banner.hide()
        tl.addWidget(self.table_error_banner)

        err_btn_row = QHBoxLayout()
        self.table_retry_btn = QPushButton("重试表格")
        self.table_retry_btn.setObjectName("Button_TableRetry")
        self.table_retry_btn.clicked.connect(self._on_retry_table_build)
        self.table_retry_btn.hide()
        self.table_back_graphic_btn = QPushButton("返回图形")
        self.table_back_graphic_btn.setObjectName("Button_TableBackGraphic")
        self.table_back_graphic_btn.clicked.connect(
            lambda: self.set_view_mode("graphic")
        )
        self.table_back_graphic_btn.hide()
        err_btn_row.addWidget(self.table_retry_btn)
        err_btn_row.addWidget(self.table_back_graphic_btn)
        err_btn_row.addStretch(1)
        tl.addLayout(err_btn_row)

        perf_row = QHBoxLayout()
        self.table_perf_check = QCheckBox("性能模式（关闭装饰）")
        self.table_perf_check.setObjectName("Check_TablePerformanceMode")
        self.table_perf_check.toggled.connect(self._on_table_performance_mode)
        self.table_copy_btn = QPushButton("复制选区")
        self.table_copy_btn.setObjectName("Button_TableCopySelection")
        self.table_copy_btn.clicked.connect(self.copy_table_selection_to_clipboard)
        perf_row.addWidget(self.table_perf_check)
        perf_row.addWidget(self.table_copy_btn)
        perf_row.addStretch(1)
        tl.addLayout(perf_row)

        self.table_empty_label = QLabel("显示集为空 · 勾选井道以显示表格")
        self.table_empty_label.setObjectName("TableEmptyLabel")
        self.table_empty_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.table_empty_label.hide()
        tl.addWidget(self.table_empty_label)
        self.view_mode_stack.addWidget(table_page)  # 1 table

        hl.addWidget(self.view_mode_stack, 1)

        corr_host = QWidget()
        corr_host.setObjectName("CorrelationPlotHost")
        cl = QVBoxLayout(corr_host)
        self.correlation_caption = QLabel(
            "地层对比图-lite · 多井并列 · 共享深度（需 ≥2 口井）"
        )
        self.correlation_caption.setObjectName("CorrelationCaption")
        cl.addWidget(self.correlation_caption)

        self.correlation_stack = QStackedWidget()
        self.correlation_stack.setObjectName("CorrelationStack")
        self.correlation_canvas = CorrelationCanvas()
        self.correlation_canvas.top_clicked.connect(self._on_correlation_top_clicked)
        self.correlation_stack.addWidget(self.correlation_canvas)  # 0 host

        self._corr_engine_page = QWidget()
        self._corr_engine_page.setObjectName("CorrelationEnginePage")
        cel = QVBoxLayout(self._corr_engine_page)
        cel.setContentsMargins(0, 0, 0, 0)
        self.correlation_engine_caption = QLabel(
            "引擎对比画布 · submit_multi_well_section · 共享深度"
        )
        self.correlation_engine_caption.setObjectName("CorrelationEngineCaption")
        cel.addWidget(self.correlation_engine_caption)
        self._corr_engine_placeholder = QLabel(
            "引擎对比未激活。勾选「优先使用引擎画布」并打开对比图。"
        )
        self._corr_engine_placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        cel.addWidget(self._corr_engine_placeholder, 1)
        self.correlation_stack.addWidget(self._corr_engine_page)  # 1 engine
        cl.addWidget(self.correlation_stack, 1)

        self.document_tabs.addTab(host, "单井分析图（多图道）")
        self.document_tabs.addTab(corr_host, "地层对比图-lite")

        # Phase-2 PR-C: plane_map + fence_3d host surfaces (section/composite
        # tabs are added in later PR-C batches).
        self._plane_map_host = QWidget()
        self._plane_map_host.setObjectName("PlaneMapHost")
        pml = QVBoxLayout(self._plane_map_host)
        self.plane_map_caption = QLabel(
            "平面图 · 井位（需井含 lng/lat/crs）· 按工区坐标系投影"
        )
        self.plane_map_caption.setObjectName("PlaneMapCaption")
        pml.addWidget(self.plane_map_caption)
        self.plane_map_view = PlaneMapView()
        pml.addWidget(self.plane_map_view, 1)
        self.document_tabs.addTab(self._plane_map_host, "平面图")

        self._fence_3d_host = QWidget()
        self._fence_3d_host.setObjectName("Fence3dHost")
        f3l = QVBoxLayout(self._fence_3d_host)
        self.fence_3d_caption = QLabel(
            "三维栅状图 · pyqtgraph 3D · 拖拽旋转 / 滚轮缩放"
        )
        self.fence_3d_caption.setObjectName("Fence3dCaption")
        f3l.addWidget(self.fence_3d_caption)
        self.fence_3d_view = FenceView()
        f3l.addWidget(self.fence_3d_view, 1)
        self.document_tabs.addTab(self._fence_3d_host, "三维栅状图")

        # Phase-2 PR-C3: 油藏剖面 host surface.
        self._section_host = QWidget()
        self._section_host.setObjectName("SectionHost")
        sl = QVBoxLayout(self._section_host)
        self.section_caption = QLabel(
            "油藏剖面 · 井列 + 断层/接触/充填覆盖（host QPainter 渲染）"
        )
        self.section_caption.setObjectName("SectionCaption")
        sl.addWidget(self.section_caption)
        self.section_canvas = SectionCanvas()
        sl.addWidget(self.section_canvas, 1)
        self.document_tabs.addTab(self._section_host, "油藏剖面")

        # Phase-2 PR-C4: 油藏综合图 host surface (cartography paper).
        self._composite_host = QWidget()
        self._composite_host.setObjectName("CompositeHost")
        cl2 = QVBoxLayout(self._composite_host)
        self.composite_caption = QLabel(
            "油藏综合图 · 纸面排版 · 图件面板（live / snapshot）"
        )
        self.composite_caption.setObjectName("CompositeCaption")
        cl2.addWidget(self.composite_caption)
        self.composite_view = CompositeView()
        cl2.addWidget(self.composite_view, 1)
        self.document_tabs.addTab(self._composite_host, "油藏综合图")

        layout.addWidget(self.document_tabs, 1)
        return pane

    def _build_right(self) -> QWidget:
        pane = QWidget()
        pane.setObjectName("RightPane")
        layout = QVBoxLayout(pane)

        layout.addWidget(QLabel("属性 / 图版 / 层位"))
        layout.addWidget(QLabel("图版模板（库 · 只应用）"))
        self.template_list = QListWidget()
        self.template_list.setObjectName("TemplateList")
        self.template_list.currentItemChanged.connect(
            lambda *_: self._sync_apply_enabled()
        )
        layout.addWidget(self.template_list)

        btn_row = QHBoxLayout()
        self.apply_btn = QPushButton("应用到选中井")
        self.apply_btn.setObjectName("Button_ApplyTemplate")
        self.apply_btn.clicked.connect(self._on_apply_template)
        self.apply_btn.setEnabled(False)
        btn_row.addWidget(self.apply_btn)
        layout.addLayout(btn_row)

        self.track_list_label = QLabel("当前显示图道（派生 · 勾选在左栏井内容）")
        self.track_list_label.setObjectName("TrackListLabel")
        layout.addWidget(self.track_list_label)
        self.track_list = QListWidget()
        self.track_list.setObjectName("TrackList")
        self.track_list.currentItemChanged.connect(self._on_track_list_selection)
        layout.addWidget(self.track_list)

        props = QWidget()
        props.setObjectName("TrackPropsForm")
        form = QFormLayout(props)
        form.setContentsMargins(0, 0, 0, 0)
        self.track_visible = QCheckBox("可见")
        self.track_visible.setObjectName("TrackVisible")
        self.track_visible.toggled.connect(self._on_track_props_changed)
        form.addRow(self.track_visible)
        self.track_scale_min = QDoubleSpinBox()
        self.track_scale_min.setObjectName("TrackScaleMin")
        self.track_scale_min.setRange(-1e9, 1e9)
        self.track_scale_min.setDecimals(4)
        self.track_scale_min.valueChanged.connect(self._on_track_props_changed)
        form.addRow("比例最小", self.track_scale_min)
        self.track_scale_max = QDoubleSpinBox()
        self.track_scale_max.setObjectName("TrackScaleMax")
        self.track_scale_max.setRange(-1e9, 1e9)
        self.track_scale_max.setDecimals(4)
        self.track_scale_max.valueChanged.connect(self._on_track_props_changed)
        form.addRow("比例最大", self.track_scale_max)
        self.track_scale_mode = QComboBox()
        self.track_scale_mode.setObjectName("TrackScaleMode")
        self.track_scale_mode.addItem("线性", "linear")
        self.track_scale_mode.addItem("对数", "log")
        self.track_scale_mode.currentIndexChanged.connect(self._on_track_props_changed)
        form.addRow("比例类型", self.track_scale_mode)
        layout.addWidget(props)
        self._track_props_guard = False
        self._set_track_props_enabled(False)

        layout.addWidget(QLabel("层位"))
        self.tops_list = QListWidget()
        self.tops_list.setObjectName("TopsList")
        self.tops_list.addItem("（无层位）")
        self.tops_list.currentItemChanged.connect(
            lambda *_: self._sync_apply_enabled()
        )
        layout.addWidget(self.tops_list)

        layout.addWidget(QLabel("对比井序 / 间距"))
        self.corr_well_list = QListWidget()
        self.corr_well_list.setObjectName("CorrelationWellList")
        layout.addWidget(self.corr_well_list)
        corr_order_row = QHBoxLayout()
        self.corr_well_up_btn = QPushButton("上移")
        self.corr_well_up_btn.setObjectName("Button_CorrWellUp")
        self.corr_well_up_btn.clicked.connect(lambda: self._move_correlation_well(-1))
        self.corr_well_down_btn = QPushButton("下移")
        self.corr_well_down_btn.setObjectName("Button_CorrWellDown")
        self.corr_well_down_btn.clicked.connect(lambda: self._move_correlation_well(1))
        corr_order_row.addWidget(self.corr_well_up_btn)
        corr_order_row.addWidget(self.corr_well_down_btn)
        layout.addLayout(corr_order_row)
        gap_row = QHBoxLayout()
        gap_row.addWidget(QLabel("井间距(px)"))
        self.corr_gap_spin = QSpinBox()
        self.corr_gap_spin.setObjectName("CorrelationColumnGap")
        self.corr_gap_spin.setRange(0, 200)
        self.corr_gap_spin.setValue(6)
        self.corr_gap_spin.valueChanged.connect(self._on_correlation_gap_changed)
        gap_row.addWidget(self.corr_gap_spin)
        layout.addLayout(gap_row)
        layout.addWidget(QLabel("对比基准 / 拉平"))
        datum_row = QHBoxLayout()
        self.corr_datum_mode = QComboBox()
        self.corr_datum_mode.setObjectName("CorrelationDatumMode")
        self.corr_datum_mode.addItem("MD（原始深度）", "md")
        self.corr_datum_mode.addItem("层位拉平", "horizon")
        self.corr_datum_mode.currentIndexChanged.connect(self._on_correlation_datum_changed)
        datum_row.addWidget(self.corr_datum_mode)
        layout.addLayout(datum_row)
        self.corr_datum_horizon = QLineEdit()
        self.corr_datum_horizon.setObjectName("CorrelationDatumHorizon")
        self.corr_datum_horizon.setPlaceholderText("拉平层位名，如 T1")
        self.corr_datum_horizon.editingFinished.connect(self._on_correlation_datum_changed)
        layout.addWidget(self.corr_datum_horizon)
        self.corr_undo_btn = QPushButton("撤销连线/拉平")
        self.corr_undo_btn.setObjectName("Button_CorrLayoutUndo")
        self.corr_undo_btn.clicked.connect(self._on_correlation_layout_undo)
        self.corr_undo_btn.setEnabled(False)
        layout.addWidget(self.corr_undo_btn)
        self.corr_fill_check = QCheckBox("显示井间充填")
        self.corr_fill_check.setObjectName("CorrelationInterwellFill")
        self.corr_fill_check.toggled.connect(self._on_correlation_fill_toggled)
        layout.addWidget(self.corr_fill_check)
        self.corr_refresh_btn = QPushButton("刷新对比图（层位）")
        self.corr_refresh_btn.setObjectName("Button_RefreshCorrelationTops")
        self.corr_refresh_btn.setToolTip(
            "从磁盘重载各井层位并更新连线深度/充填/拉平。"
            "单井层位编辑后会自动刷新；此按钮为显式重载。"
        )
        self.corr_refresh_btn.clicked.connect(self._on_refresh_correlation_tops)
        layout.addWidget(self.corr_refresh_btn)
        self._corr_layout_guard = False
        self._corr_layout_undo: list[dict[str, Any]] = []
        self._set_correlation_layout_enabled(False)

        layout.addWidget(QLabel("对比连线"))
        self.links_list = QListWidget()
        self.links_list.setObjectName("LinksList")
        self.links_list.addItem("（无连线）")
        layout.addWidget(self.links_list)
        link_btns = QHBoxLayout()
        self.clear_links_btn = QPushButton("清除连线")
        self.clear_links_btn.setObjectName("Button_ClearLinks")
        self.clear_links_btn.clicked.connect(self._on_clear_horizon_links)
        self.clear_links_btn.setEnabled(False)
        self.remove_link_btn = QPushButton("删除选中")
        self.remove_link_btn.setObjectName("Button_RemoveLink")
        self.remove_link_btn.clicked.connect(self._on_remove_selected_link)
        self.remove_link_btn.setEnabled(False)
        link_btns.addWidget(self.clear_links_btn)
        link_btns.addWidget(self.remove_link_btn)
        layout.addLayout(link_btns)

        layout.addStretch(1)
        return pane

    def _build_status(self) -> None:
        status = QStatusBar(self)
        status.setObjectName("MainStatusBar")
        self.setStatusBar(status)
        self._update_status()

    def _populate_templates(self) -> None:
        self.template_list.clear()
        if not self._templates:
            self.template_list.addItem("（无内置图版）")
            return
        # Prefer multi-track standard template first (not alphabetically
        # gr-only). Users importing multi-curve LAS (e.g. data/井曲线) otherwise
        # only see GR when the list defaulted to "简化 GR-only".
        preferred_id = "std-gr-rt-den"
        ordered = sorted(
            self._templates,
            key=lambda t: (0 if t.id == preferred_id else 1, t.name),
        )
        preferred_row = 0
        for i, t in enumerate(ordered):
            item = QListWidgetItem(t.name)
            item.setData(Qt.ItemDataRole.UserRole, t.id)
            self.template_list.addItem(item)
            if t.id == preferred_id:
                preferred_row = i
        self.template_list.setCurrentRow(preferred_row)
        self._templates = ordered

    def _sync_apply_enabled(self) -> None:
        has_well = self._workspace is not None and self._selected_well_id is not None
        # Session may reload from disk on open; enable if well is in catalog.
        in_catalog = False
        if self._workspace and self._selected_well_id:
            in_catalog = any(
                w.id == self._selected_well_id for w in self._workspace.wells
            )
        ok = (
            has_well
            and in_catalog
            and self.template_list.currentItem() is not None
            and bool(self._templates)
        )
        self.apply_btn.setEnabled(ok)
        self._act_apply_template.setEnabled(ok)
        self._act_new_single_plot.setEnabled(ok)
        has_sw_plot = (
            self._workspace is not None
            and self._active_plot_id is not None
            and self._active_plot_type == "single_well"
        )
        self._act_add_leaf_to_plot.setEnabled(has_sw_plot and has_well)
        self._act_export_plot_xml.setEnabled(has_sw_plot)
        self._act_export_plot_xlsx.setEnabled(has_sw_plot)

        n_wells = len(self._workspace.wells) if self._workspace else 0
        can_corr = (
            self._workspace is not None
            and n_wells >= 2
            and self.template_list.currentItem() is not None
            and bool(self._templates)
        )
        self._act_new_correlation.setEnabled(can_corr)

        can_export_single = (
            self._presentation is not None and self._presentation.track_count > 0
        )
        can_export_corr = len(self._correlation_presentations) >= 2
        can_export = can_export_single or can_export_corr
        self._act_export_svg.setEnabled(can_export)
        self._act_export_pdf.setEnabled(can_export)
        if hasattr(self, "_act_export_png"):
            self._act_export_png.setEnabled(can_export)
        if hasattr(self, "_act_export_cgm"):
            # CGM is single-well engine-only (B1.CGM.2)
            self._act_export_cgm.setEnabled(can_export_single)
        if hasattr(self, "_act_print_preview"):
            # Preview primarily for single-well export stream (T13); correlation OK too
            self._act_print_preview.setEnabled(can_export)

        can_tops = (
            self._workspace is not None
            and self._selected_well_id is not None
            and any(w.id == self._selected_well_id for w in self._workspace.wells)
        )
        self._act_import_tops.setEnabled(can_tops)
        self._act_stub_tops.setEnabled(can_tops)
        has_selected_top = can_tops and bool(self._active_tops) and (
            self.tops_list.currentItem() is not None
            and self.tops_list.currentItem().data(Qt.ItemDataRole.UserRole)
        )
        self._act_edit_top_depth.setEnabled(bool(has_selected_top))
        self._act_remove_top.setEnabled(bool(has_selected_top))
        self._act_undo_tops.setEnabled(
            can_tops and self._tops_history.can_undo(self._selected_well_id)
        )
        self._act_redo_tops.setEnabled(
            can_tops and self._tops_history.can_redo(self._selected_well_id)
        )
        can_pick = (
            can_tops
            and self._presentation is not None
            and self._presentation.well_document_id == self._selected_well_id
        )
        self._act_pick_tops.setEnabled(can_pick)
        self._act_add_top.setEnabled(can_pick)
        if not can_pick and self._act_pick_tops.isChecked():
            self._act_pick_tops.setChecked(False)
            self.multi_track_canvas.set_pick_mode(False)

        self._act_engine_preview.setEnabled(
            self._presentation is not None and self._presentation.curve_track_count > 0
        )
        self._act_engine_corr.setEnabled(
            len(self._correlation_presentations) >= 2
        )
        has_corr = len(self._correlation_presentations) >= 2
        self._act_auto_links.setEnabled(has_corr)
        self._act_pick_links.setEnabled(has_corr)
        has_links = has_corr and len(self._correlation_links) > 0
        self._act_clear_links.setEnabled(has_links)
        self.clear_links_btn.setEnabled(has_links)
        self.remove_link_btn.setEnabled(has_links)
        if not has_corr and self._act_pick_links.isChecked():
            self._act_pick_links.setChecked(False)
            self.correlation_canvas.set_link_pick_mode(False)
            self._link_pick_first = None

    def _on_about(self) -> None:
        QMessageBox.about(
            self,
            f"关于 {PRODUCT_NAME}",
            about_text(version=__version__),
        )

    def _update_status(self) -> None:
        hint = effective_qt_platform_hint()
        if self._workspace is None:
            msg = f"{PRODUCT_NAME} · 未打开工区 · Qt: {hint}"
        else:
            well = self._selected_well_id or "—"
            tracks = (
                self._presentation.track_count if self._presentation else 0
            )
            corr_n = self.correlation_canvas.column_count()
            plot_kind = self._active_plot_type or "—"
            tops_n = len(self._active_tops)
            surface = self._primary_surface
            msg = (
                f"工区: {self._workspace.name} · "
                f"井 {len(self._workspace.wells)} · "
                f"选中 {well[:8]}… · "
                f"图道 {tracks} · "
                f"层位 {tops_n} · "
                f"对比列 {corr_n} · "
                f"图件 {plot_kind} · "
                f"画布 {surface} · "
                f"Qt: {hint}"
            )
        self.statusBar().showMessage(msg)

    @staticmethod
    def _default_prefer_engine() -> bool:
        import os

        if os.environ.get("WLWS_DISABLE_ENGINE", "").strip().lower() in (
            "1",
            "true",
            "yes",
        ):
            return False
        if os.environ.get("WLWS_FORCE_HOST_CANVAS", "").strip().lower() in (
            "1",
            "true",
            "yes",
        ):
            return False
        return True

    @property
    def primary_surface(self) -> str:
        """Active single-well surface: ``host`` or ``engine``."""
        return self._primary_surface

    def set_prefer_engine_canvas(self, prefer: bool) -> None:
        """Prefer WellLogView when available; always falls back to host."""
        self._prefer_engine_canvas = bool(prefer)
        if hasattr(self, "_act_prefer_engine"):
            self._act_prefer_engine.setChecked(self._prefer_engine_canvas)
        if self._active_plot_type == "correlation":
            self._sync_primary_correlation_surface()
        else:
            self._sync_primary_single_well_surface()

    def _ensure_engine_view(self, parent: QWidget | None = None) -> Any:
        """Create WellLogView once; optionally reparent onto ``parent`` page."""
        host = parent or self._engine_page
        if self._engine_view is None:
            view = create_well_log_view(host)
            self._engine_view = view
        else:
            view = self._engine_view
            if view.parent() is not host:
                view.setParent(host)
        # Attach to layout of host page
        layout = host.layout()
        if layout is not None:
            # Avoid double-add: only add if not already in this layout
            found = False
            for i in range(layout.count()):
                item = layout.itemAt(i)
                if item is not None and item.widget() is view:
                    found = True
                    break
            if not found:
                layout.addWidget(view, 1)
        if host is self._engine_page:
            self._engine_placeholder.hide()
        if host is getattr(self, "_corr_engine_page", None):
            self._corr_engine_placeholder.hide()
        view.show()
        return view

    def _sync_primary_single_well_surface(self) -> None:
        """Show host or engine as primary based on preference + availability."""
        if self._presentation is None:
            self._primary_surface = "host"
            self.single_well_stack.setCurrentIndex(0)
            return

        want_engine = self._prefer_engine_canvas and engine_available()
        # Tops pick mode requires host canvas hit-testing
        if self.multi_track_canvas.pick_mode():
            want_engine = False

        if not want_engine:
            self._primary_surface = "host"
            self.single_well_stack.setCurrentIndex(0)
            return

        try:
            view = self._ensure_engine_view(self._engine_page)
            report = load_presentation_into_view(
                view, self._presentation, tops=self._active_tops
            )
            self._engine_last_error = None
            self._primary_surface = "engine"
            self.single_well_stack.setCurrentIndex(1)
            tracks = report.get("track_count", "?")
            curves = report.get("curve_count", "?")
            cap = probe_engine()
            self.engine_caption.setText(
                f"引擎画布 · {self._presentation.well_name} · "
                f"{cap.detail} · tracks={tracks} curves={curves}"
            )
            if self._presentation.template_name:
                self.plot_caption.setText(
                    f"单井分析图 · {self._presentation.well_name} · "
                    f"{self._presentation.template_name} · "
                    f"{self._presentation.track_count} 图道 · 引擎"
                )
        except (EngineUnavailable, EngineSubmitError, Exception) as exc:  # noqa: BLE001
            self._engine_last_error = str(exc)
            self._primary_surface = "host"
            self.single_well_stack.setCurrentIndex(0)
            self.engine_caption.setText(f"引擎不可用，已回退主机画布 · {exc}")

    def _sync_primary_correlation_surface(self) -> None:
        """Prefer engine multi-well for correlation; fall back to host canvas (#228)."""
        if not self._correlation_presentations:
            self.correlation_stack.setCurrentIndex(0)
            return

        want_engine = self._prefer_engine_canvas and engine_available()
        if not want_engine:
            self._primary_surface = "host"
            self.correlation_stack.setCurrentIndex(0)
            return

        try:
            view = self._ensure_engine_view(self._corr_engine_page)
            tops_cols = self.correlation_canvas.tops_per_column()
            depth = self.correlation_canvas.depth_range()
            report = submit_multi_well_presentations(
                view,
                self._correlation_presentations,
                tops_per_well=tops_cols,
                shared_depth=depth,
                links=self._correlation_links,
            )
            self._engine_last_error = None
            self._primary_surface = "engine"
            self.correlation_stack.setCurrentIndex(1)
            n = report.get("well_count", len(self._correlation_presentations))
            cap = probe_engine()
            self.correlation_engine_caption.setText(
                f"引擎对比 · {n} 井 · 共享深度 · {cap.detail}"
            )
            # Reflect engine mode in host caption too
            base = self.correlation_caption.text()
            if "· 引擎" not in base:
                self.correlation_caption.setText(f"{base} · 引擎")
        except (EngineUnavailable, EngineSubmitError, Exception) as exc:  # noqa: BLE001
            self._engine_last_error = str(exc)
            self._primary_surface = "host"
            self.correlation_stack.setCurrentIndex(0)
            self.correlation_engine_caption.setText(
                f"引擎对比不可用，已回退主机画布 · {exc}"
            )

    def set_workspace(self, ws: Workspace | None) -> None:
        self._workspace = ws
        if ws is None:
            self.session.clear()
            self._selected_well_id = None
            self._active_plot_id = None
            self._active_plot_type = None
            self._presentation = None
            self._correlation_presentations = []
            self._correlation_links = []
            self._active_tops = []
            self._tops_diagnostics = []
            self._tops_history.clear_all()
            self.multi_track_canvas.set_presentation(None)
            self._refresh_track_list()
            self.multi_track_canvas.set_tops(None)
            self.correlation_canvas.set_columns([])
            self.correlation_canvas.set_links(None)
            self._refresh_links_list()
            self._refresh_correlation_well_list(None)
            self._primary_surface = "host"
            if hasattr(self, "single_well_stack"):
                self.single_well_stack.setCurrentIndex(0)
            if hasattr(self, "correlation_stack"):
                self.correlation_stack.setCurrentIndex(0)
            self.plot_caption.setText("单井分析图 · 多图道（选择井并应用图版）")
            self.correlation_caption.setText(
                "地层对比图-lite · 多井并列 · 共享深度（需 ≥2 口井）"
            )
            self.document_tabs.setTabText(0, "单井分析图（多图道）")
            self.document_tabs.setTabText(1, "地层对比图-lite")
            self.document_tabs.setCurrentIndex(0)
        self._act_import_las.setEnabled(ws is not None)
        self._act_import_plot_xml.setEnabled(ws is not None)
        self._act_import_plot_xlsx.setEnabled(ws is not None)
        # Phase-2 PR-C: new plot-type menu items (fence_3d needs 3D).
        self._act_new_plane_map.setEnabled(ws is not None)
        self._act_new_fence_3d.setEnabled(
            ws is not None and probe_3d().available
        )
        if ws is not None and not probe_3d().available:
            self._act_new_fence_3d.setToolTip(
                "三维栅状图需要 pyqtgraph + OpenGL"
            )
        self._act_new_section.setEnabled(ws is not None)
        self._act_new_composite.setEnabled(ws is not None)
        self._act_set_crs.setEnabled(ws is not None)
        self._refresh_tree()
        self._refresh_tops_list()
        self._sync_apply_enabled()
        self._update_status()
        if ws is not None:
            self.setWindowTitle(window_title(workspace_name=ws.name))
            try:
                add_recent(ws.root)
            except OSError:
                pass
            if hasattr(self, "_main_stack"):
                self._main_stack.setCurrentIndex(1)
            if hasattr(self, "startup_page"):
                self.startup_page.refresh_recent()
        else:
            self.setWindowTitle(window_title())
            if hasattr(self, "_main_stack"):
                self._main_stack.setCurrentIndex(0)
            if hasattr(self, "startup_page"):
                self.startup_page.refresh_recent()
    def import_las_path(self, las_path: Path | str) -> str:
        if self._workspace is None:
            raise WorkspaceError("请先打开或新建工区")
        result = import_las_into_workspace(self._workspace, las_path)
        self.session.put(result.document)
        self._selected_well_id = result.catalog_well_id
        self._refresh_tree()
        self._select_well_in_tree(result.catalog_well_id)
        self._refresh_well_content_tree()
        self._refresh_tops_list()
        self._sync_apply_enabled()
        self._update_status()
        return result.catalog_well_id

    def load_tops_for_selected_well(self) -> list[FormationTop]:
        """Load tops for selection; update inspector + single-well canvas."""
        return self._load_and_apply_tops(self._selected_well_id)

    def generate_stub_tops_for_well(self, well_id: str) -> list[FormationTop]:
        """Write demo tops from well depth range; show on canvas/inspector."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        doc = self.session.ensure_well_loaded(self._workspace, well_id)
        depth = doc.depth
        if depth.size:
            d0, d1 = float(np.nanmin(depth)), float(np.nanmax(depth))
        else:
            d0, d1 = 0.0, 100.0
        tops = make_stub_tops(d0, d1, unit=doc.depth_unit or "m")
        self._commit_tops_edit(well_id, tops, [])
        return tops

    def import_tops_json_for_well(
        self, well_id: str, path: Path | str
    ) -> list[FormationTop]:
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        before, _ = load_tops_for_well(self._workspace, well_id)
        tops, diags = import_tops_from_json_file(self._workspace, well_id, path)
        # import writes disk; record pre-import state for undo.
        self._tops_history.record_before_commit(well_id, before)
        self._selected_well_id = well_id
        self._apply_tops_to_ui(well_id, tops, diags)
        self._sync_apply_enabled()
        return tops

    def add_top_at_depth(
        self,
        well_id: str,
        name: str,
        depth: float,
        *,
        color: str = "#c0392b",
        unit: str | None = None,
    ) -> FormationTop:
        """Add a formation top, persist, and refresh inspector/canvas (#226/#294)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        label = (name or "").strip()
        if not label:
            raise TopsError("层位名称不能为空")
        if not math.isfinite(depth):
            raise TopsError("深度无效")
        tops, diags = load_tops_for_well(self._workspace, well_id)
        depth_unit = unit or "m"
        if self._presentation is not None:
            depth_unit = self._presentation.depth_unit or depth_unit
        top = FormationTop(
            id=str(uuid.uuid4()),
            name=label,
            depth=float(depth),
            unit=depth_unit,
            color=color,
        )
        new_tops = list(tops)
        new_tops.append(top)
        new_tops.sort(key=lambda t: t.depth)
        self._commit_tops_edit(well_id, new_tops, diags)
        return top

    def remove_top_by_id(self, well_id: str, top_id: str) -> bool:
        """Remove one top by id; returns False if not found."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        tops, diags = load_tops_for_well(self._workspace, well_id)
        key = (top_id or "").strip()
        kept = [t for t in tops if (t.id or t.name) != key]
        if len(kept) == len(tops):
            return False
        self._commit_tops_edit(well_id, kept, diags)
        return True

    def set_top_depth(self, well_id: str, top_id: str, depth: float) -> FormationTop:
        """Change depth of an existing top (command + undoable)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        if not math.isfinite(depth):
            raise TopsError("深度无效")
        tops, diags = load_tops_for_well(self._workspace, well_id)
        key = (top_id or "").strip()
        updated: list[FormationTop] = []
        found: FormationTop | None = None
        for t in tops:
            if (t.id or t.name) == key:
                found = FormationTop(
                    id=t.id,
                    name=t.name,
                    depth=float(depth),
                    unit=t.unit,
                    color=t.color,
                )
                updated.append(found)
            else:
                updated.append(t)
        if found is None:
            raise TopsError("未找到选中层位")
        updated.sort(key=lambda t: t.depth)
        self._commit_tops_edit(well_id, updated, diags)
        return found

    def undo_tops_edit(self, well_id: str | None = None) -> bool:
        """Undo last tops edit for the well; restore disk + UI + engine markers."""
        wid = well_id or self._selected_well_id
        if self._workspace is None or not wid:
            return False
        current, _ = load_tops_for_well(self._workspace, wid)
        previous = self._tops_history.undo(wid, current)
        if previous is None:
            return False
        save_tops_for_well(self._workspace, wid, previous)
        self._selected_well_id = wid
        self._apply_tops_to_ui(wid, previous, [])
        self._sync_apply_enabled()
        return True

    def redo_tops_edit(self, well_id: str | None = None) -> bool:
        """Redo last undone tops edit."""
        wid = well_id or self._selected_well_id
        if self._workspace is None or not wid:
            return False
        current, _ = load_tops_for_well(self._workspace, wid)
        nxt = self._tops_history.redo(wid, current)
        if nxt is None:
            return False
        save_tops_for_well(self._workspace, wid, nxt)
        self._selected_well_id = wid
        self._apply_tops_to_ui(wid, nxt, [])
        self._sync_apply_enabled()
        return True

    def _commit_tops_edit(
        self,
        well_id: str,
        new_tops: list[FormationTop],
        diagnostics: list[str] | None = None,
    ) -> None:
        """Single write path for tops: history → tops.json → UI/engine refresh.

        Does not mutate curve buffers. Engine markers update via multi-track
        resubmit when the engine canvas is primary (#294 / T6).
        """
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        before, _ = load_tops_for_well(self._workspace, well_id)
        self._tops_history.record_before_commit(well_id, before)
        save_tops_for_well(self._workspace, well_id, new_tops)
        self._selected_well_id = well_id
        self._apply_tops_to_ui(well_id, new_tops, list(diagnostics or []))
        self._sync_apply_enabled()

    def _load_and_apply_tops(self, well_id: str | None) -> list[FormationTop]:
        if self._workspace is None or not well_id:
            self._apply_tops_to_ui(None, [], [])
            return []
        try:
            tops, diags = load_tops_for_well(self._workspace, well_id)
        except Exception as exc:  # noqa: BLE001 — degrade, never crash shell
            tops, diags = [], [f"层位加载异常: {exc}"]
        self._apply_tops_to_ui(well_id, tops, diags)
        return tops

    def _apply_tops_to_ui(
        self,
        well_id: str | None,
        tops: list[FormationTop],
        diagnostics: list[str],
    ) -> None:
        self._active_tops = list(tops)
        self._tops_diagnostics = list(diagnostics)
        self._refresh_tops_list_items(tops, diagnostics)
        # Single-well canvas: only if presentation matches this well
        if (
            self._presentation is not None
            and well_id is not None
            and self._presentation.well_document_id == well_id
        ):
            self.multi_track_canvas.set_tops(tops)
            # Refresh engine markers when primary surface is engine
            if self._prefer_engine_canvas and not self.multi_track_canvas.pick_mode():
                self._sync_primary_single_well_surface()
        elif well_id is None:
            self.multi_track_canvas.set_tops(None)
        # Correlation: auto-refresh tops/links/fill when a correlation plot
        # is loaded (T10 strategy: automatic on tops commit).
        if self._correlation_presentations and self._workspace is not None:
            self.refresh_correlation_from_sources(reason="auto")
        self._sync_apply_enabled()
        self._update_status()

    def _refresh_tops_list(self) -> None:
        self._load_and_apply_tops(self._selected_well_id)

    def _refresh_tops_list_items(
        self, tops: list[FormationTop], diagnostics: list[str]
    ) -> None:
        self.tops_list.clear()
        if not tops:
            label = "（无层位）"
            if diagnostics:
                label = f"（无层位 · {diagnostics[0][:40]}）"
            self.tops_list.addItem(label)
            return
        for t in tops:
            item = QListWidgetItem(t.display_label())
            item.setData(Qt.ItemDataRole.UserRole, t.id or t.name)
            item.setForeground(Qt.GlobalColor.darkRed)
            self.tops_list.addItem(item)
        if diagnostics:
            for d in diagnostics[:3]:
                hint = QListWidgetItem(f"⚠ {d}")
                hint.setDisabled(True)
                self.tops_list.addItem(hint)

    def _current_template_id(self) -> str | None:
        item = self.template_list.currentItem()
        if item is None:
            return None
        tid = item.data(Qt.ItemDataRole.UserRole)
        return str(tid) if tid else None

    def _set_track_props_enabled(self, enabled: bool) -> None:
        self.track_visible.setEnabled(enabled)
        self.track_scale_min.setEnabled(enabled)
        self.track_scale_max.setEnabled(enabled)
        self.track_scale_mode.setEnabled(enabled)

    def _selected_track_id(self) -> str | None:
        item = self.track_list.currentItem()
        if item is None:
            return None
        tid = item.data(Qt.ItemDataRole.UserRole)
        return str(tid) if tid else None

    def _find_bound_track(self, track_id: str | None):
        if self._presentation is None or not track_id:
            return None
        for t in self._presentation.tracks:
            if t.id == track_id:
                return t
        return None

    def _refresh_track_list(self) -> None:
        """Populate right-pane track list from active single-well presentation."""
        if not hasattr(self, "track_list"):
            return
        prev = self._selected_track_id()
        self.track_list.blockSignals(True)
        self.track_list.clear()
        if self._presentation is None:
            self.track_list.addItem("（无图道 · 请应用图版）")
            self.track_list.blockSignals(False)
            self._load_track_props_form(None)
            return
        select_row = 0
        for i, track in enumerate(self._presentation.tracks):
            vis = "" if track.visible else " [隐藏]"
            role = "深度" if track.role == "depth" else "曲线"
            label = f"{track.title} ({role}){vis}"
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, track.id)
            self.track_list.addItem(item)
            if prev is not None and track.id == prev:
                select_row = i
        self.track_list.setCurrentRow(select_row)
        self.track_list.blockSignals(False)
        self._on_track_list_selection(self.track_list.currentItem(), None)

    def _load_track_props_form(self, track) -> None:
        self._track_props_guard = True
        try:
            if track is None:
                self.track_visible.setChecked(True)
                self.track_scale_min.setValue(0.0)
                self.track_scale_max.setValue(100.0)
                self.track_scale_mode.setCurrentIndex(0)
                self._set_track_props_enabled(False)
                return
            self.track_visible.setChecked(bool(track.visible))
            has_scale = track.scale is not None and track.role == "curve"
            self.track_scale_min.setEnabled(has_scale)
            self.track_scale_max.setEnabled(has_scale)
            self.track_scale_mode.setEnabled(has_scale)
            self.track_visible.setEnabled(True)
            if track.scale is not None:
                self.track_scale_min.setValue(float(track.scale.min))
                self.track_scale_max.setValue(float(track.scale.max))
                mode = track.scale.mode
                idx = self.track_scale_mode.findData(mode)
                self.track_scale_mode.setCurrentIndex(idx if idx >= 0 else 0)
            else:
                self.track_scale_min.setValue(0.0)
                self.track_scale_max.setValue(100.0)
                self.track_scale_mode.setCurrentIndex(0)
        finally:
            self._track_props_guard = False

    def _on_track_list_selection(self, current, _previous) -> None:
        if current is None or self._presentation is None:
            self._load_track_props_form(None)
            return
        tid = current.data(Qt.ItemDataRole.UserRole)
        track = self._find_bound_track(str(tid) if tid else None)
        self._load_track_props_form(track)

    def _on_track_props_changed(self, *_args) -> None:
        if self._track_props_guard or self._presentation is None:
            return
        track = self._find_bound_track(self._selected_track_id())
        if track is None:
            return
        track.visible = self.track_visible.isChecked()
        if track.scale is not None and track.role == "curve":
            smin = float(self.track_scale_min.value())
            smax = float(self.track_scale_max.value())
            if smax <= smin:
                smax = smin + 1.0
                self._track_props_guard = True
                try:
                    self.track_scale_max.setValue(smax)
                finally:
                    self._track_props_guard = False
            track.scale.min = smin
            track.scale.max = smax
            mode = self.track_scale_mode.currentData()
            if mode in ("linear", "log"):
                track.scale.mode = mode  # type: ignore[assignment]
        # Refresh list labels (hidden marker) without losing selection
        self._refresh_track_list_labels_only()
        self.multi_track_canvas.set_presentation(self._presentation)
        self._sync_primary_single_well_surface()
        self._persist_track_overrides()

    def _refresh_track_list_labels_only(self) -> None:
        if self._presentation is None or not hasattr(self, "track_list"):
            return
        for i, track in enumerate(self._presentation.tracks):
            item = self.track_list.item(i)
            if item is None:
                continue
            vis = "" if track.visible else " [隐藏]"
            role = "深度" if track.role == "depth" else "曲线"
            item.setText(f"{track.title} ({role}){vis}")

    def _persist_track_overrides(self) -> None:
        """Write current track props into the active single-well plot document."""
        if (
            self._workspace is None
            or self._presentation is None
            or not self._active_plot_id
            or self._active_plot_type != "single_well"
        ):
            return
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        plot.track_overrides = track_overrides_snapshot(self._presentation)
        save_plot_document(self._workspace, plot)

    def _display_set_key(
        self, well_id: str, *, plot_id: str | None = None
    ) -> str:
        """Storage key: plot-scoped when a single-well plot is active."""
        pid = plot_id if plot_id is not None else self._active_plot_id
        if (
            pid
            and (
                self._active_plot_type == "single_well"
                or plot_id is not None
            )
        ):
            return f"plot:{pid}"
        return f"well:{well_id}"

    def display_set_for(self, well_id: str) -> frozenset[str] | None:
        """Session Display Set for the active plot (or well preview)."""
        return self._display_sets.get(self._display_set_key(well_id))

    def view_mode_for(self, well_id: str) -> str:
        """Session view mode for a well (default graphic)."""
        return self._view_modes.get(well_id, "graphic")

    def set_view_mode(self, mode: str) -> None:
        """Switch single-well Graphic | Table without changing Display Set.

        Semantic Selection is preserved across mode switches (T5).
        """
        mode = "table" if mode == "table" else "graphic"
        self._view_mode = mode
        if self._selected_well_id:
            self._view_modes[self._selected_well_id] = mode
        if hasattr(self, "btn_mode_graphic"):
            self.btn_mode_graphic.setChecked(mode == "graphic")
            self.btn_mode_table.setChecked(mode == "table")
        if hasattr(self, "view_mode_stack"):
            self.view_mode_stack.setCurrentIndex(1 if mode == "table" else 0)
        if mode == "table":
            self._refresh_table_projection()
            self._apply_selection_to_table()
        # Table mode always uses host surface; engine stays for graphic path.
        if mode == "graphic":
            self._sync_primary_single_well_surface()
            self._apply_selection_to_canvas()

    @property
    def semantic_selection(self) -> SemanticSelection | None:
        """Current shared Semantic Selection (graph↔table); not screen coords."""
        return self._semantic_selection

    def apply_semantic_selection(self, selection: SemanticSelection | None) -> None:
        """Set shared semantic selection and sync graphic + table highlights."""
        self._semantic_selection = selection
        self._apply_selection_to_canvas()
        self._apply_selection_to_table()

    def _primary_curve_hint(self) -> tuple[str | None, str | None]:
        """Optional curve/leaf identity from current presentation / track list."""
        if self._presentation is None:
            return None, None
        for tr in self._presentation.tracks:
            if tr.role != "curve" or not tr.visible or not tr.layers:
                continue
            mnemo = tr.layers[0].mnemonic
            leaf = None
            if self._selected_well_id and self._workspace is not None:
                try:
                    doc = self.session.get(self._presentation.well_document_id)
                    if doc is None:
                        doc = self.session.ensure_well_loaded(
                            self._workspace, self._selected_well_id
                        )
                    leaf = leaf_id_for_curve(doc.document_id, mnemo)
                except Exception:  # noqa: BLE001
                    leaf = None
            return mnemo, leaf
        return None, None

    def _on_canvas_sample_selected(self, depth: float) -> None:
        if self._selected_well_id is None or self._presentation is None:
            return
        mnemo, leaf = self._primary_curve_hint()
        sel = selection_from_depth(
            well_id=self._selected_well_id,
            depth=np.asarray(self._presentation.depth, dtype=np.float64),
            reference_depth=float(depth),
            curve_mnemonic=mnemo,
            leaf_id=leaf,
        )
        self.apply_semantic_selection(sel)

    def _on_table_selection_changed(self, *_args) -> None:
        if self._selection_guard or self._selected_well_id is None:
            return
        if not hasattr(self, "_primary_table_view"):
            return
        indexes = self._primary_table_view.selectionModel().selectedRows()
        if not indexes:
            return
        row = indexes[0].row()
        proj = self._primary_table_model.projection()
        if proj is None:
            return
        mnemo, leaf = self._primary_curve_hint()
        if proj.columns:
            mnemo = proj.columns[0].mnemonic
            leaf = proj.columns[0].leaf_id
        sel = selection_from_row(
            well_id=self._selected_well_id,
            depth=proj.depth,
            sample_index=row,
            curve_mnemonic=mnemo,
            leaf_id=leaf,
        )
        self.apply_semantic_selection(sel)

    def _apply_selection_to_canvas(self) -> None:
        if not hasattr(self, "multi_track_canvas"):
            return
        sel = self._semantic_selection
        if sel is None:
            self.multi_track_canvas.set_selection_depth(None)
            return
        if (
            self._selected_well_id is not None
            and sel.well_id != self._selected_well_id
        ):
            self.multi_track_canvas.set_selection_depth(None)
            return
        self.multi_track_canvas.set_selection_depth(sel.reference_depth)

    def _apply_selection_to_table(self) -> None:
        if not hasattr(self, "_primary_table_view"):
            return
        sel = self._semantic_selection
        model = self._primary_table_model
        proj = model.projection()
        if sel is None or proj is None:
            self._selection_guard = True
            try:
                self._primary_table_view.clearSelection()
            finally:
                self._selection_guard = False
            return
        if (
            self._selected_well_id is not None
            and sel.well_id != self._selected_well_id
        ):
            return
        # Prefer sample_index when it lands on this projection's axis
        row = sel.sample_index
        if row < 0 or row >= proj.row_count:
            # Map by reference depth
            from well_log_workstation.semantic_selection import (  # local avoid cycle noise
                nearest_sample_index,
            )

            mapped = nearest_sample_index(proj.depth, sel.reference_depth)
            if mapped is None:
                return
            row = mapped
        if row >= model.rowCount():
            return
        self._selection_guard = True
        try:
            index = model.index(row, 0)
            self._primary_table_view.selectRow(row)
            self._primary_table_view.scrollTo(index)
        finally:
            self._selection_guard = False

    def _show_table_progress(self, message: str = "正在构建表格投影…") -> None:
        if not hasattr(self, "table_progress"):
            return
        self.table_progress.setFormat(message + " %p%")
        self.table_progress.setRange(0, 0)
        self.table_progress.show()
        self.table_cancel_btn.show()
        self.table_cancel_btn.setEnabled(True)

    def _hide_table_progress(self) -> None:
        if not hasattr(self, "table_progress"):
            return
        self.table_progress.hide()
        self.table_cancel_btn.hide()

    def _show_table_error(self, message: str) -> None:
        self._table_last_error = message
        if not hasattr(self, "table_error_banner"):
            return
        self.table_error_banner.setText(
            f"表格不可用：{message}\n图形模式仍可使用；可重试或返回图形。"
        )
        self.table_error_banner.show()
        self.table_retry_btn.show()
        self.table_back_graphic_btn.show()

    def _hide_table_error(self) -> None:
        self._table_last_error = None
        if not hasattr(self, "table_error_banner"):
            return
        self.table_error_banner.hide()
        self.table_retry_btn.hide()
        self.table_back_graphic_btn.hide()

    def _on_cancel_table_build(self) -> None:
        self._table_build_cancel[0] = True
        PROJECTION_BUILD_HOOKS.cancel_flag = self._table_build_cancel

    def _on_retry_table_build(self) -> None:
        self._hide_table_error()
        self._refresh_table_projection()

    def _on_table_performance_mode(self, on: bool) -> None:
        """Performance mode: drop nonessential decorations; never truncates data."""
        self._table_performance_mode = bool(on)
        alt = not self._table_performance_mode
        if hasattr(self, "_primary_table_view"):
            self._primary_table_view.setAlternatingRowColors(alt)
        for i in range(1, self.table_axis_tabs.count()):
            w = self.table_axis_tabs.widget(i)
            if isinstance(w, QTableView):
                w.setAlternatingRowColors(alt)
        if self._table_performance_mode:
            self.statusBar().showMessage("性能模式：已关闭表格装饰（数据未截断）", 4000)

    def copy_table_selection_to_clipboard(self) -> str:
        """Copy **selected rows only** as TSV (+ HTML) — not the whole table."""
        proj = self._primary_table_model.projection()
        if proj is None:
            return ""
        indexes = self._primary_table_view.selectionModel().selectedRows()
        rows = sorted({ix.row() for ix in indexes})
        tsv = selection_tsv(proj, rows)
        html = selection_html(proj, rows)
        if not tsv:
            return ""
        cb = QGuiApplication.clipboard()
        if cb is not None:
            from PySide6.QtCore import QMimeData

            mime = QMimeData()
            mime.setText(tsv)
            if html:
                mime.setHtml(html)
            cb.setMimeData(mime)
        return tsv

    def export_table_rows_job(
        self,
        *,
        row_start: int = 0,
        row_end: int | None = None,
    ) -> list[list[float | None]]:
        """Separate export job — never call from LogTableModel.data()."""
        proj = self._primary_table_model.projection()
        if proj is None:
            return []
        cancel = [False]
        return export_projection_rows(
            proj, row_start=row_start, row_end=row_end, cancel_flag=cancel
        )

    def _refresh_table_projection(self) -> None:
        """Rebuild virtualized table model(s) from current Display Set.

        Shows immediate progress shell; supports cancel and error recovery (T6).
        Never silently truncates Display Set rows/columns.
        """
        if not hasattr(self, "_primary_table_model"):
            return
        # Immediate feedback (≤1s without feedback is a bug)
        self._show_table_progress("正在构建表格投影…")
        self._table_build_cancel = [False]
        PROJECTION_BUILD_HOOKS.cancel_flag = self._table_build_cancel
        from PySide6.QtWidgets import QApplication

        QApplication.processEvents()

        try:
            if (
                self._workspace is None
                or self._selected_well_id is None
                or self._presentation is None
            ):
                self._primary_table_model.set_projection(None)
                self.table_column_tip.hide()
                if hasattr(self, "table_empty_label"):
                    self.table_empty_label.show()
                self._hide_table_error()
                return

            try:
                doc = self.session.ensure_well_loaded(
                    self._workspace, self._selected_well_id
                )
            except Exception as exc:  # noqa: BLE001
                self._primary_table_model.set_projection(None)
                self._show_table_error(str(exc))
                return

            tid = self._presentation.template_id or self._current_template_id()
            template = get_builtin_template(tid or "std-gr-rt-den")
            if template is None:
                self._primary_table_model.set_projection(None)
                self._show_table_error("未知图版，无法构建表格")
                return

            display_set = self._display_sets.get(
                self._display_set_key(self._selected_well_id), frozenset()
            )

            def _progress(step: int, total: int) -> None:
                if total > 0 and hasattr(self, "table_progress"):
                    self.table_progress.setRange(0, total)
                    self.table_progress.setValue(step)
                    QApplication.processEvents()

            try:
                projections = build_table_projections_guarded(
                    doc,
                    display_set,
                    template,
                    hooks=PROJECTION_BUILD_HOOKS,
                    on_progress=_progress,
                )
            except InterruptedError:
                self._hide_table_progress()
                # Cancel → return to graphic; Display Set unchanged
                self.set_view_mode("graphic")
                self.statusBar().showMessage("已取消表格构建 · 显示集未改动", 4000)
                return
            except Exception as exc:  # noqa: BLE001
                self._primary_table_model.set_projection(None)
                self._show_table_error(str(exc))
                return

            self._hide_table_error()

            # Soft tip for many columns (never auto-uncheck)
            max_cols = max((p.column_count for p in projections), default=0)
            if max_cols >= SOFT_COLUMN_TIP_THRESHOLD:
                self.table_column_tip.setText(
                    f"列较多（{max_cols}，含 Depth）· 请横向滚动查看；"
                    f"不会自动取消勾选（阈值 ≥{SOFT_COLUMN_TIP_THRESHOLD}）"
                )
                self.table_column_tip.show()
            else:
                self.table_column_tip.hide()

            # Reset axis tabs: first projection on primary view; extras as new tabs
            while self.table_axis_tabs.count() > 1:
                w = self.table_axis_tabs.widget(1)
                self.table_axis_tabs.removeTab(1)
                if w is not None:
                    w.deleteLater()
            self._table_models = [self._primary_table_model]

            if not projections or (
                len(projections) == 1 and len(projections[0].columns) == 0
            ):
                self._primary_table_model.set_projection(
                    projections[0] if projections else None
                )
                self.table_axis_tabs.setTabText(0, "主表")
                self.table_empty_label.show()
                return

            self.table_empty_label.hide()
            # Full logical rows/cols — no silent truncation under load
            self._primary_table_model.set_projection(projections[0])
            label0 = (
                f"轴 {projections[0].axis_id}"
                if len(projections) > 1
                else "主表"
            )
            self.table_axis_tabs.setTabText(0, label0)

            for proj in projections[1:]:
                model = LogTableModel()
                model.set_projection(proj)
                view = QTableView()
                view.setModel(model)
                view.setAlternatingRowColors(not self._table_performance_mode)
                view.setSelectionBehavior(
                    QAbstractItemView.SelectionBehavior.SelectRows
                )
                view.setSelectionMode(
                    QAbstractItemView.SelectionMode.SingleSelection
                )
                view.verticalHeader().setDefaultSectionSize(20)
                self.table_axis_tabs.addTab(view, f"轴 {proj.axis_id}")
                self._table_models.append(model)
            self._apply_selection_to_table()
        finally:
            self._hide_table_progress()
            PROJECTION_BUILD_HOOKS.cancel_flag = None

    def set_display_set(
        self,
        well_id: str,
        leaf_ids: AbstractSet[str] | Iterable[str],
        *,
        template_id: str | None = None,
        plot_id: str | None = None,
    ) -> HostPresentation:
        """Set Display Set for the active plot (or well preview) and rebuild."""
        effective_plot_id = (
            plot_id if plot_id is not None else self._active_plot_id
        )
        key = self._display_set_key(well_id, plot_id=effective_plot_id)
        checked = frozenset(str(x) for x in leaf_ids)
        self._display_sets[key] = checked
        # Persist display_set + data_bindings on plot (samples stay on well).
        if effective_plot_id and self._workspace is not None:
            try:
                plot_doc = load_plot_document(self._workspace, effective_plot_id)
                if plot_doc.type == "single_well":
                    meta: dict[str, dict[str, str]] = {}
                    try:
                        wdoc = self.session.ensure_well_loaded(
                            self._workspace, well_id
                        )
                        for leaf in leaves_from_document(wdoc):
                            meta[leaf.id] = {
                                "mnemonic": leaf.mnemonic,
                                "source_id": leaf.source_id,
                            }
                    except Exception:  # noqa: BLE001
                        pass
                    sync_data_bindings(
                        plot_doc,
                        well_id=well_id,
                        leaf_ids=sorted(checked),
                        leaf_meta=meta,
                    )
                    save_plot_document(self._workspace, plot_doc)
            except WorkspaceError:
                pass
        tid = template_id or self._current_template_id()
        if not tid:
            tid = "std-gr-rt-den"
        return self.apply_template_to_well(
            well_id,
            tid,
            plot_id=effective_plot_id,
        )

    def apply_template_to_well(
        self, well_id: str, template_id: str, *, plot_id: str | None = None
    ) -> HostPresentation:
        """Apply builtin template to a session well; show multi-track plot.

        Presentation is rebuilt from **Display Set × template** (T2).
        Display Set is **plot-scoped** when a single-well plot is open (model A:
        import→well data tree; plot checks which leaves to show). Template
        switches keep the Display Set and only restyle.
        Empty Display Set is allowed (guidance on canvas; not an error).
        """
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        doc = self.session.ensure_well_loaded(self._workspace, well_id)
        template = get_builtin_template(template_id)
        if template is None:
            raise WorkspaceError(f"未知图版: {template_id}")

        leaves = leaves_from_document(doc)
        effective_plot_id = plot_id if plot_id is not None else self._active_plot_id
        key = self._display_set_key(well_id, plot_id=effective_plot_id)
        if key not in self._display_sets:
            loaded: frozenset[str] | None = None
            if effective_plot_id is not None:
                try:
                    plot_doc = load_plot_document(
                        self._workspace, effective_plot_id
                    )
                    if plot_doc.display_set:
                        loaded = frozenset(str(x) for x in plot_doc.display_set)
                except WorkspaceError:
                    loaded = None
            if loaded is not None:
                self._display_sets[key] = loaded
            else:
                self._display_sets[key] = default_checks(leaves, template)
                # Seed plot document so re-open is stable
                if effective_plot_id is not None:
                    try:
                        plot_doc = load_plot_document(
                            self._workspace, effective_plot_id
                        )
                        if plot_doc.type == "single_well" and not plot_doc.display_set:
                            meta = {
                                leaf.id: {
                                    "mnemonic": leaf.mnemonic,
                                    "source_id": leaf.source_id,
                                }
                                for leaf in leaves
                            }
                            sync_data_bindings(
                                plot_doc,
                                well_id=well_id,
                                leaf_ids=sorted(self._display_sets[key]),
                                leaf_meta=meta,
                            )
                            save_plot_document(self._workspace, plot_doc)
                    except WorkspaceError:
                        pass
        display_set = self._display_sets[key]

        presentation = presentation_from_display_set(template, doc, display_set)
        # Restore layout edits from plot document when reopening (#292).
        if effective_plot_id is not None:
            try:
                plot_doc = load_plot_document(self._workspace, effective_plot_id)
                apply_track_overrides(presentation, plot_doc.track_overrides)
            except WorkspaceError:
                pass
        self._selected_well_id = well_id
        self._presentation = presentation
        self._active_plot_type = "single_well"
        if plot_id is not None:
            self._active_plot_id = plot_id
        self.multi_track_canvas.set_presentation(presentation)
        tops, diags = load_tops_for_well(self._workspace, well_id)
        self._active_tops = tops
        self._tops_diagnostics = diags
        self.multi_track_canvas.set_tops(tops)
        self._refresh_tops_list_items(tops, diags)
        self._refresh_track_list()
        bound = [
            ly.mnemonic
            for tr in presentation.tracks
            if tr.role == "curve" and tr.visible
            for ly in tr.layers
        ]
        if presentation.curve_track_count < 1:
            self.plot_caption.setText(
                f"单井分析图 · {presentation.well_name} · "
                f"{presentation.template_name} · "
                f"显示集为空 · 勾选井道以显示"
            )
        else:
            bound_s = "、".join(bound[:8]) if bound else "（无曲线）"
            if len(bound) > 8:
                bound_s += f"…(+{len(bound) - 8})"
            self.plot_caption.setText(
                f"单井分析图 · {presentation.well_name} · "
                f"{presentation.template_name} · "
                f"{presentation.track_count} 图道 · 绑定 {bound_s}"
            )
        tab = f"单井·多图道 · {presentation.well_name}"
        if self._active_plot_id:
            tab = f"{tab} · {self._active_plot_id[:8]}"
        self.document_tabs.setTabText(0, tab)
        self.document_tabs.setCurrentIndex(0)
        self._sync_primary_single_well_surface()
        self._sync_apply_enabled()
        self._refresh_correlation_well_list(None)
        self._refresh_well_content_tree()
        if self._view_mode == "table":
            self._refresh_table_projection()
        self._apply_selection_to_canvas()
        self._update_status()
        return presentation

    def create_single_well_plot_document(
        self, well_id: str, template_id: str
    ) -> PlotDocument:
        """Create plots/<id>.json, catalog entry, open multi-track view."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        doc = self.session.ensure_well_loaded(self._workspace, well_id)
        plot = create_single_well_plot(
            self._workspace,
            well_id=well_id,
            well_name=doc.well_name,
            template_id=template_id,
        )
        self._active_plot_id = plot.id
        self._active_plot_type = "single_well"
        self.apply_template_to_well(well_id, plot.template_id, plot_id=plot.id)
        emit_plot_changed(plot.id)
        self._refresh_tree()
        return plot

    def create_correlation_plot_document(
        self,
        well_ids: list[str],
        template_id: str,
        *,
        name: str | None = None,
    ) -> PlotDocument:
        """Create plots/<id>.json correlation doc and show shared-depth canvas."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        plot = create_correlation_plot(
            self._workspace,
            well_ids=well_ids,
            template_id=template_id,
            name=name,
        )
        self._active_plot_id = plot.id
        self._active_plot_type = "correlation"
        self._show_correlation(plot)
        # Initial auto-match once on create (not on every reopen/undo).
        if not plot.links:
            try:
                self.auto_link_correlation_tops()
            except WorkspaceError:
                pass
            try:
                plot = load_plot_document(self._workspace, plot.id)
            except WorkspaceError:
                pass
        emit_plot_changed(plot.id)
        self._refresh_tree()
        return plot

    def _show_correlation(self, plot: PlotDocument) -> None:
        """Load wells, apply template per column, shared depth on canvas."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        if not plot.template_id:
            raise WorkspaceError("图件未绑定图版")
        template = get_builtin_template(plot.template_id)
        if template is None:
            raise WorkspaceError(f"未知图版: {plot.template_id}")

        presentations: list[HostPresentation] = []
        tops_cols: list[list[FormationTop]] = []
        all_diags: list[str] = []
        for well_id in plot.well_ids:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
            pres = apply_template(template, doc)
            if pres.curve_track_count < 1:
                raise WorkspaceError(
                    f"井 {doc.well_name} 图版未能绑定任何曲线"
                )
            presentations.append(pres)
            t, diags = load_tops_for_well(self._workspace, well_id)
            tops_cols.append(t)
            all_diags.extend(diags)

        self._correlation_presentations = presentations
        self._active_plot_id = plot.id
        self._active_plot_type = "correlation"
        if plot.well_ids:
            self._selected_well_id = plot.well_ids[0]
            self._active_tops = tops_cols[0] if tops_cols else []
            self._tops_diagnostics = all_diags
            self._refresh_tops_list_items(self._active_tops, all_diags)
        # Links from document only. Auto-match on first create is done in
        # create_correlation_plot_document so intentional empty (clear/undo)
        # is not refilled (#296).
        links = list(plot.links)
        self._correlation_links = links
        gap = getattr(plot, "column_gap_px", 6) or 6
        self.correlation_canvas.set_column_gap(gap)
        self.correlation_canvas.set_columns(presentations, tops_cols, links)
        self.correlation_canvas.set_show_interwell_fill(
            bool(getattr(plot, "show_interwell_fill", False))
        )
        self.correlation_canvas.set_fill_color(
            str(getattr(plot, "interwell_fill_color", None) or "#93c5fd")
        )
        self._apply_correlation_datum_shifts(plot, presentations, tops_cols)
        self._refresh_links_list()
        self._refresh_correlation_well_list(plot)
        names = " · ".join(p.well_name for p in presentations[:4])
        tops_n = sum(len(t) for t in tops_cols)
        datum = getattr(plot, "datum_mode", None) or "md"
        horiz = getattr(plot, "datum_horizon", None) or ""
        datum_note = f"基准 {datum}" + (f"/{horiz}" if horiz else "")
        self.correlation_caption.setText(
            f"地层对比图-lite · {names} · "
            f"共享深度 · 图版 {template.name} · 层位 {tops_n} · "
            f"连线 {len(links)} · 间距 {gap}px · {datum_note}"
        )
        tab = f"对比 · {len(presentations)}井"
        if self._active_plot_id:
            tab = f"{tab} · {self._active_plot_id[:8]}"
        self.document_tabs.setTabText(1, tab)
        self.document_tabs.setCurrentIndex(1)
        # Keep template selection in sync
        for i in range(self.template_list.count()):
            item = self.template_list.item(i)
            if item and item.data(Qt.ItemDataRole.UserRole) == plot.template_id:
                self.template_list.setCurrentRow(i)
                break
        self._sync_primary_correlation_surface()
        self._sync_apply_enabled()
        self._update_status()

    def _show_plane_map(self, plot: PlotDocument) -> None:
        """Render well positions on the paleo_map canvas (Phase-2 T2)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        self._active_plot_id = plot.id
        self._active_plot_type = "plane_map"
        self.plane_map_view.set_coordinate(self._workspace.coordinate)
        self.plane_map_view.set_plot_data(self._workspace.wells)
        drawn = len(PlaneMapView.filter_wells_with_coords(self._workspace.wells))
        total = len(self._workspace.wells)
        self.plane_map_caption.setText(
            f"平面图 · 绘制 {drawn}/{total} 口井（含坐标）· "
            f"项目坐标系 {self._workspace.coordinate.project_crs}"
        )
        self.document_tabs.setCurrentWidget(self._plane_map_host)
        emit_plot_changed(plot.id)
        self._refresh_correlation_well_list(None)
        self._update_status()

    def _show_fence_3d(self, plot: PlotDocument) -> None:
        """Render the 3D fence surface from well x/y/depth (Phase-2 T6)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        if not probe_3d().available:
            raise WorkspaceError(
                "三维栅状图需要 pyqtgraph + OpenGL（3D 能力探测失败）"
            )
        self._active_plot_id = plot.id
        self._active_plot_type = "fence_3d"
        wells = []
        for wid in plot.well_ids:
            entry = next(
                (w for w in self._workspace.wells if w.id == wid), None
            )
            if entry is None:
                continue
            doc = self.session.ensure_well_loaded(self._workspace, wid)
            depth = float(np.nanmax(doc.depth)) if doc.depth.size else 1000.0
            # x/y from catalog coords (project CRS), depth from LAS.
            lng = float(entry.lng) if entry.lng is not None else 0.0
            lat = float(entry.lat) if entry.lat is not None else 0.0
            wells.append(
                {"name": entry.name, "x": lng, "y": lat, "depth": depth}
            )
        self.fence_3d_view.set_wells(wells)
        self.fence_3d_caption.setText(
            f"三维栅状图 · {len(wells)} 井 · 拖拽旋转 / 滚轮缩放"
        )
        self.document_tabs.setCurrentWidget(self._fence_3d_host)
        emit_plot_changed(plot.id)
        self._update_status()

    def _show_section(self, plot: PlotDocument) -> None:
        """Render the reservoir section (host-side, Phase-2 T4/T5)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        if not plot.template_id:
            raise WorkspaceError("图件未绑定图版")
        template = get_builtin_template(plot.template_id)
        if template is None:
            raise WorkspaceError(f"未知图版: {plot.template_id}")

        presentations: list[HostPresentation] = []
        tops_cols: list[list[FormationTop]] = []
        well_positions: list[tuple[float, float]] = []
        for well_id in plot.well_ids:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
            pres = apply_template(template, doc)
            presentations.append(pres)
            t, _diags = load_tops_for_well(self._workspace, well_id)
            tops_cols.append(t)
            entry = next(
                (w for w in self._workspace.wells if w.id == well_id), None
            )
            lng = float(entry.lng) if entry and entry.lng is not None else float(len(well_positions))
            lat = float(entry.lat) if entry and entry.lat is not None else 0.0
            well_positions.append((lng, lat))

        # Datum shifts (T5): md / tvdss / horizon flatten.
        datum_mode = "md"
        target_horizon = None
        datum = WellSectionDatum(mode=datum_mode, target_horizon=target_horizon)
        well_dicts = [
            {
                "name": pres.well_name,
                "tops": [
                    {"name": ft.name, "depth": ft.depth}
                    for ft in (tops_cols[i] if i < len(tops_cols) else [])
                ],
            }
            for i, pres in enumerate(presentations)
        ]
        shifts = datum.compute_shifts(well_dicts)

        # Section geometry (T4): faults / contacts / tie quads from user
        # annotations in the catalog (defaults: none -> empty overlays).
        faults: list[FaultSegment2D] = []
        contacts: list[ContactSegment2D] = []
        quads: list[TieQuad2D] = []
        fault_pts = getattr(plot, "fault_annotations", None)
        if fault_pts:
            faults = curtain_slice_fault(fault_pts, well_positions, shifts)
        contact_depths = getattr(plot, "contact_annotations", None)
        if contact_depths:
            contacts = contact_polyline(
                [{"depth": d} for d in contact_depths],
                well_positions,
                fluid_type="owc",
                datum_shifts=shifts,
            )
        tops_as_dicts = [
            [{"name": ft.name, "depth": ft.depth} for ft in col]
            for col in tops_cols
        ]
        quads = tie_quads(tops_as_dicts, well_positions, datum_shifts=shifts)

        self._active_plot_id = plot.id
        self._active_plot_type = "section"
        self.section_canvas.set_section(
            presentations,
            tops_cols,
            faults=faults,
            contacts=contacts,
            tie_quads=quads,
        )
        names = " · ".join(p.well_name for p in presentations[:4])
        self.section_caption.setText(
            f"油藏剖面 · {names} · 基准 {datum_mode} · "
            f"断层 {len(faults)} · 接触 {len(contacts)} · 充填 {len(quads)}"
        )
        self.document_tabs.setCurrentWidget(self._section_host)
        emit_plot_changed(plot.id)
        self._update_status()

    def _show_composite(self, plot: PlotDocument) -> None:
        """Render the composite figure paper layout (Phase-2 T7)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        self._active_plot_id = plot.id
        self._active_plot_type = "composite"
        self.composite_view.set_workspace(self._workspace)
        # Register the active source widgets for snapshot grabs.
        if self._presentation is not None:
            self.composite_view.register_source_widget(
                self._active_plot_id or "", self.multi_track_canvas
            )
        # Recreate panels from the persisted PanelRef list.
        for panel in plot.panels:
            self.composite_view.add_panel_ref(panel)
        self.composite_view.set_active_plot_id(plot.id)
        failed = self.composite_view.restore_free_graphics(plot.free_graphics)
        if failed > 0:
            self.statusBar().showMessage(
                f"{failed} 条图形记录无法恢复（未知类型或格式错误）", 5000
            )
        self.composite_caption.setText(
            f"油藏综合图 · {len(plot.panels)} 个面板 · 纸面排版"
        )
        self.document_tabs.setCurrentWidget(self._composite_host)
        emit_plot_changed(plot.id)
        self._update_status()

    def _set_correlation_layout_enabled(self, enabled: bool) -> None:
        if not hasattr(self, "corr_well_list"):
            return
        self.corr_well_list.setEnabled(enabled)
        self.corr_well_up_btn.setEnabled(enabled)
        self.corr_well_down_btn.setEnabled(enabled)
        self.corr_gap_spin.setEnabled(enabled)
        if hasattr(self, "corr_datum_mode"):
            self.corr_datum_mode.setEnabled(enabled)
            self.corr_datum_horizon.setEnabled(enabled)
            self.corr_undo_btn.setEnabled(
                enabled and bool(self._corr_layout_undo)
            )
        if hasattr(self, "corr_fill_check"):
            self.corr_fill_check.setEnabled(enabled)
            self.corr_refresh_btn.setEnabled(enabled)

    def _refresh_correlation_well_list(self, plot: PlotDocument | None = None) -> None:
        """Fill well-order list for active correlation plot (#295)."""
        if not hasattr(self, "corr_well_list"):
            return
        self.corr_well_list.clear()
        if (
            plot is None
            or plot.type != "correlation"
            or self._workspace is None
            or len(plot.well_ids) < 2
        ):
            self._set_correlation_layout_enabled(False)
            self._corr_layout_guard = True
            try:
                self.corr_gap_spin.setValue(6)
            finally:
                self._corr_layout_guard = False
            return
        name_by_id = {w.id: w.name for w in self._workspace.wells}
        for i, wid in enumerate(plot.well_ids):
            label = f"{i + 1}. {name_by_id.get(wid, wid[:8])}"
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, wid)
            self.corr_well_list.addItem(item)
        self._corr_layout_guard = True
        try:
            self.corr_gap_spin.setValue(int(getattr(plot, "column_gap_px", 6) or 6))
            if hasattr(self, "corr_datum_mode"):
                mode = str(getattr(plot, "datum_mode", None) or "md")
                idx = self.corr_datum_mode.findData(mode)
                self.corr_datum_mode.setCurrentIndex(idx if idx >= 0 else 0)
                self.corr_datum_horizon.setText(
                    str(getattr(plot, "datum_horizon", None) or "")
                )
            if hasattr(self, "corr_fill_check"):
                self.corr_fill_check.setChecked(
                    bool(getattr(plot, "show_interwell_fill", False))
                )
        finally:
            self._corr_layout_guard = False
        self._set_correlation_layout_enabled(True)
        if self.corr_well_list.count() > 0:
            self.corr_well_list.setCurrentRow(0)

    def _move_correlation_well(self, delta: int) -> None:
        """Reorder well_ids on the active correlation plot and refresh canvas."""
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
        ):
            return
        row = self.corr_well_list.currentRow()
        if row < 0:
            return
        new_row = row + int(delta)
        if new_row < 0 or new_row >= self.corr_well_list.count():
            return
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        ids = list(plot.well_ids)
        if row >= len(ids) or new_row >= len(ids):
            return
        self._push_correlation_layout_undo(plot)
        ids[row], ids[new_row] = ids[new_row], ids[row]
        plot.well_ids = ids
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存井序失败", str(exc))
            return
        self._show_correlation(plot)
        self.corr_well_list.setCurrentRow(new_row)

    def _on_correlation_gap_changed(self, value: int) -> None:
        if self._corr_layout_guard:
            return
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
        ):
            return
        gap = max(0, min(200, int(value)))
        self.correlation_canvas.set_column_gap(gap)
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        self._push_correlation_layout_undo(plot)
        plot.column_gap_px = gap
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            return
        # Refresh caption spacing note without full rebuild
        base = self.correlation_caption.text()
        if "· 间距" in base:
            head = base.split("· 间距")[0].rstrip()
            self.correlation_caption.setText(f"{head} · 间距 {gap}px")
        self._sync_primary_correlation_surface()
        self._set_correlation_layout_enabled(True)

    def _correlation_layout_snapshot(self, plot: PlotDocument) -> dict[str, Any]:
        return {
            "links": [lk.to_json() for lk in plot.links],
            "column_gap_px": int(getattr(plot, "column_gap_px", 6) or 6),
            "datum_mode": str(getattr(plot, "datum_mode", None) or "md"),
            "datum_horizon": getattr(plot, "datum_horizon", None),
            "well_ids": list(plot.well_ids),
        }

    def _push_correlation_layout_undo(self, plot: PlotDocument) -> None:
        self._corr_layout_undo.append(self._correlation_layout_snapshot(plot))
        if len(self._corr_layout_undo) > 32:
            self._corr_layout_undo = self._corr_layout_undo[-32:]
        if hasattr(self, "corr_undo_btn"):
            self.corr_undo_btn.setEnabled(True)

    def _apply_correlation_datum_shifts(
        self,
        plot: PlotDocument,
        presentations: list[HostPresentation],
        tops_cols: list[list[FormationTop]],
    ) -> None:
        """Compute WellSectionDatum shifts and apply to correlation canvas (#296)."""
        mode = str(getattr(plot, "datum_mode", None) or "md")
        horizon = getattr(plot, "datum_horizon", None)
        if mode not in WellSectionDatum.VALID_MODES:
            mode = "md"
        datum = WellSectionDatum(mode=mode, target_horizon=horizon)
        well_dicts: list[dict[str, Any]] = []
        id_by_name: dict[str, str] = {}
        for i, pres in enumerate(presentations):
            tops = tops_cols[i] if i < len(tops_cols) else []
            well_dicts.append(
                {
                    "name": pres.well_name,
                    "tops": [{"name": t.name, "depth": t.depth} for t in tops],
                }
            )
            id_by_name[pres.well_name] = pres.well_document_id
        shifts_by_name = datum.compute_shifts(well_dicts)
        shifts_by_id = {
            id_by_name.get(name, name): float(shift)
            for name, shift in shifts_by_name.items()
        }
        self.correlation_canvas.set_depth_shifts(shifts_by_id)

    def set_correlation_datum(
        self,
        *,
        mode: str = "md",
        horizon: str | None = None,
        persist: bool = True,
    ) -> None:
        """Set correlation display datum and refresh canvas (undoable when persist)."""
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
        ):
            return
        if mode not in WellSectionDatum.VALID_MODES:
            mode = "md"
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        if persist:
            self._push_correlation_layout_undo(plot)
        plot.datum_mode = mode
        plot.datum_horizon = (horizon or "").strip() or None
        if persist:
            try:
                save_plot_document(self._workspace, plot)
            except WorkspaceError:
                pass
        if self._correlation_presentations:
            tops_cols = self.correlation_canvas.tops_per_column()
            self._apply_correlation_datum_shifts(
                plot, self._correlation_presentations, tops_cols
            )
        self._corr_layout_guard = True
        try:
            if hasattr(self, "corr_datum_mode"):
                idx = self.corr_datum_mode.findData(mode)
                self.corr_datum_mode.setCurrentIndex(idx if idx >= 0 else 0)
                self.corr_datum_horizon.setText(plot.datum_horizon or "")
        finally:
            self._corr_layout_guard = False
        self._sync_primary_correlation_surface()
        self._set_correlation_layout_enabled(True)

    def _on_correlation_datum_changed(self, *_args) -> None:
        if self._corr_layout_guard:
            return
        if not hasattr(self, "corr_datum_mode"):
            return
        mode = self.corr_datum_mode.currentData() or "md"
        horizon = self.corr_datum_horizon.text().strip() or None
        self.set_correlation_datum(mode=str(mode), horizon=horizon, persist=True)

    def undo_correlation_layout(self) -> bool:
        """Restore previous links / gap / datum / well order for active correlation."""
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
            or not self._corr_layout_undo
        ):
            return False
        snap = self._corr_layout_undo.pop()
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return False
        from well_log_workstation.correlation_links import HorizonLink as HL

        restored: list[HorizonLink] = []
        for raw in snap.get("links") or []:
            if isinstance(raw, dict):
                lk = HL.from_json(raw)
                if lk is not None:
                    restored.append(lk)
        plot.links = restored
        plot.column_gap_px = int(snap.get("column_gap_px", 6) or 6)
        plot.datum_mode = str(snap.get("datum_mode") or "md")
        plot.datum_horizon = snap.get("datum_horizon")
        if snap.get("well_ids"):
            plot.well_ids = [str(x) for x in snap["well_ids"]]
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            return False
        self._show_correlation(plot)
        if hasattr(self, "corr_undo_btn"):
            self.corr_undo_btn.setEnabled(bool(self._corr_layout_undo))
        return True

    def _on_correlation_layout_undo(self) -> None:
        if not self.undo_correlation_layout():
            self.statusBar().showMessage("无可撤销的对比布局编辑", 3000)
            return
        self.statusBar().showMessage("已撤销连线/拉平/井序编辑", 3000)

    def refresh_correlation_from_sources(self, *, reason: str = "manual") -> None:
        """Reload tops for open correlation columns; update links & fill (T10).

        **Strategy (documented):**

        - **auto** — invoked after single-well tops commits (add/remove/move/
          stub/import/undo/redo) when a correlation plot is loaded.
        - **manual** — 右栏「刷新对比图（层位）」button for explicit reload.

        Does not re-run auto-match of *new* links; only refreshes depths of
        existing links that still have matching top names, and repaints fills.
        """
        if self._workspace is None or not self._correlation_presentations:
            return
        tops_cols: list[list[FormationTop]] = []
        for pres in self._correlation_presentations:
            t, _ = load_tops_for_well(self._workspace, pres.well_document_id)
            tops_cols.append(t)
        self.correlation_canvas.set_tops_per_column(tops_cols)

        # Update link depths from current tops (by name match)
        tops_by_well: dict[str, dict[str, FormationTop]] = {}
        for i, pres in enumerate(self._correlation_presentations):
            tops_by_well[pres.well_document_id] = {
                t.name.strip(): t for t in tops_cols[i] if t.name.strip()
            }
        updated_links: list[HorizonLink] = []
        for lk in self._correlation_links:
            left_map = tops_by_well.get(lk.left_well_id, {})
            right_map = tops_by_well.get(lk.right_well_id, {})
            lt = left_map.get(lk.name.strip())
            rt = right_map.get(lk.name.strip())
            if lt is None or rt is None:
                # Drop links whose tops vanished
                continue
            updated_links.append(
                HorizonLink(
                    id=lk.id,
                    left_well_id=lk.left_well_id,
                    right_well_id=lk.right_well_id,
                    name=lk.name,
                    left_depth=float(lt.depth),
                    right_depth=float(rt.depth),
                    left_marker_id=lt.id or lk.left_marker_id,
                    right_marker_id=rt.id or lk.right_marker_id,
                    color=lk.color,
                )
            )
        self._correlation_links = updated_links
        self.correlation_canvas.set_links(updated_links)
        self._refresh_links_list()

        # Re-apply datum + keep fill flag
        if self._active_plot_id and self._active_plot_type == "correlation":
            try:
                plot = load_plot_document(self._workspace, self._active_plot_id)
            except WorkspaceError:
                plot = None
            if plot is not None:
                self._apply_correlation_datum_shifts(
                    plot, self._correlation_presentations, tops_cols
                )
                self.correlation_canvas.set_show_interwell_fill(
                    bool(plot.show_interwell_fill)
                )
                # Persist updated link depths (auto path only if links changed)
                if reason == "auto" and plot.links != updated_links:
                    plot.links = list(updated_links)
                    try:
                        save_plot_document(self._workspace, plot)
                    except WorkspaceError:
                        pass
        if reason == "manual":
            self.statusBar().showMessage(
                f"已刷新对比图层位（{sum(len(t) for t in tops_cols)} 个标记 · "
                f"{len(updated_links)} 条连线）",
                4000,
            )
        if (
            self._prefer_engine_canvas
            and self._active_plot_type == "correlation"
        ):
            self._sync_primary_correlation_surface()

    def _on_refresh_correlation_tops(self) -> None:
        self.refresh_correlation_from_sources(reason="manual")

    def _on_correlation_fill_toggled(self, checked: bool = False) -> None:
        if self._corr_layout_guard:
            return
        enabled = self.corr_fill_check.isChecked()
        self.correlation_canvas.set_show_interwell_fill(enabled)
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
        ):
            return
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        plot.show_interwell_fill = enabled
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            pass

    def auto_link_correlation_tops(self) -> list[HorizonLink]:
        """Match tops by name across adjacent wells; persist on active plot."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        if len(self._correlation_presentations) < 2:
            raise WorkspaceError("请先打开地层对比图（≥2 井）")
        well_ids = [p.well_document_id for p in self._correlation_presentations]
        tops_by_well: dict[str, list[FormationTop]] = {}
        for i, wid in enumerate(well_ids):
            tops_by_well[wid] = (
                self.correlation_canvas.tops_per_column()[i]
                if i < len(self.correlation_canvas.tops_per_column())
                else []
            )
        links = match_tops_by_name(well_ids, tops_by_well)
        self._set_correlation_links(links, persist=True)
        return links

    def clear_correlation_links(self) -> None:
        """Remove all horizon links from the active correlation plot."""
        self._set_correlation_links([], persist=True)

    def remove_correlation_link(self, link_id: str) -> bool:
        """Remove one link by id; return True if removed."""
        before = len(self._correlation_links)
        kept = [lk for lk in self._correlation_links if lk.id != link_id]
        if len(kept) == before:
            return False
        self._set_correlation_links(kept, persist=True)
        return True

    def create_horizon_link(
        self,
        left_well_id: str,
        left_top: FormationTop,
        right_well_id: str,
        right_top: FormationTop,
        *,
        name: str | None = None,
    ) -> HorizonLink:
        """Manually link two tops on different wells (#231)."""
        if left_well_id == right_well_id:
            raise WorkspaceError("连线两端必须是不同井")
        # Order by column index so left is leftward on canvas
        order = {
            p.well_document_id: i for i, p in enumerate(self._correlation_presentations)
        }
        li = order.get(left_well_id, 0)
        ri = order.get(right_well_id, 1)
        if li > ri:
            left_well_id, right_well_id = right_well_id, left_well_id
            left_top, right_top = right_top, left_top
        try:
            link = make_horizon_link(
                left_well_id, left_top, right_well_id, right_top, name=name
            )
        except ValueError as exc:
            raise WorkspaceError(str(exc)) from exc
        # Avoid exact duplicate ends
        for existing in self._correlation_links:
            if (
                existing.left_well_id == link.left_well_id
                and existing.right_well_id == link.right_well_id
                and abs(existing.left_depth - link.left_depth) < 1e-6
                and abs(existing.right_depth - link.right_depth) < 1e-6
            ):
                return existing
        links = list(self._correlation_links) + [link]
        self._set_correlation_links(links, persist=True)
        return link

    def _set_correlation_links(
        self, links: list[HorizonLink], *, persist: bool
    ) -> None:
        self._correlation_links = list(links)
        self.correlation_canvas.set_links(self._correlation_links)
        self._refresh_links_list()
        if persist and self._workspace is not None and self._active_plot_id:
            try:
                plot = load_plot_document(self._workspace, self._active_plot_id)
                if plot.type == "correlation":
                    self._push_correlation_layout_undo(plot)
                    plot.links = list(self._correlation_links)
                    save_plot_document(self._workspace, plot)
            except WorkspaceError:
                pass
        cap = self.correlation_caption.text().split(" · 连线")[0]
        self.correlation_caption.setText(
            f"{cap} · 连线 {len(self._correlation_links)}"
        )
        if (
            self._prefer_engine_canvas
            and self._active_plot_type == "correlation"
            and self._correlation_presentations
        ):
            self._sync_primary_correlation_surface()
        self._sync_apply_enabled()
        self._update_status()

    def _refresh_links_list(self) -> None:
        self.links_list.clear()
        if not self._correlation_links:
            self.links_list.addItem("（无连线）")
            return
        # Map well ids to short names when presentations available
        name_by_id = {
            p.well_document_id: p.well_name for p in self._correlation_presentations
        }
        for lk in self._correlation_links:
            left = name_by_id.get(lk.left_well_id, lk.left_well_id[:8])
            right = name_by_id.get(lk.right_well_id, lk.right_well_id[:8])
            label = (
                f"{lk.name}  {left}@{lk.left_depth:g} → "
                f"{right}@{lk.right_depth:g}"
            )
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, lk.id)
            self.links_list.addItem(item)

    def open_plot_document(self, plot_id: str) -> PlotDocument:
        """Load plot metadata and open the matching plot-type view."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        plot = load_plot_document(self._workspace, plot_id)
        # plane_map / composite may legitimately have no wells bound.
        if not plot.well_ids and plot.type not in ("plane_map", "composite"):
            raise WorkspaceError("图件未绑定井")
        if not plot.template_id:
            raise WorkspaceError("图件未绑定图版")

        if plot.type == "correlation":
            self._show_correlation(plot)
            if plot.well_ids:
                self._select_well_in_tree(plot.well_ids[0])
            self._refresh_tree()
            return plot

        if plot.type == "plane_map":
            self._show_plane_map(plot)
            self._refresh_tree()
            return plot

        if plot.type == "fence_3d":
            self._show_fence_3d(plot)
            self._refresh_tree()
            return plot

        if plot.type == "section":
            self._show_section(plot)
            self._refresh_tree()
            return plot

        if plot.type == "composite":
            self._show_composite(plot)
            self._refresh_tree()
            return plot

        if plot.type != "single_well":
            raise WorkspaceError(f"未知图件类型: {plot.type}")
        well_id = plot.well_ids[0]
        self._active_plot_id = plot.id
        self._active_plot_type = "single_well"
        self._selected_well_id = well_id
        self.apply_template_to_well(well_id, plot.template_id, plot_id=plot.id)
        # Keep template selection in sync
        for i in range(self.template_list.count()):
            item = self.template_list.item(i)
            if item and item.data(Qt.ItemDataRole.UserRole) == plot.template_id:
                self.template_list.setCurrentRow(i)
                break
        self._select_well_in_tree(well_id)
        self._refresh_tree()
        return plot

    def export_active_plot_svg(self, path: Path | str) -> Path:
        """Export active single-well multi-track to SVG (engine default when available)."""
        return self._export_single_well_file(path, "svg")

    def export_active_plot_pdf(self, path: Path | str) -> Path:
        """Export active single-well multi-track to PDF (engine default when available).

        Engine PDF is non-searchable (ADR 0047); callers that need UI disclosure
        should use the menu export path which shows the warning dialog.
        """
        return self._export_single_well_file(path, "pdf")

    def _export_single_well_file(
        self,
        path: Path | str,
        fmt: ExportFormat,
        *,
        force_backend: str | None = None,
    ) -> Path:
        """Programmatic single_well export; prefers engine SVG/PDF when available."""
        if self._presentation is None or self._presentation.track_count < 1:
            raise ExportError("无活动单井分析图可导出（请先应用图版）")
        if self._workspace is None or not self._active_plot_id:
            # No plot document — fall back to presentation-only Qt paint
            if fmt == "svg":
                return export_presentation_svg(self._presentation, path)
            if fmt == "pdf":
                return export_presentation_pdf(self._presentation, path)
            raise ExportError(f"不支持的格式: {fmt}")

        plot = load_plot_document(self._workspace, self._active_plot_id)
        backend = prefer_engine_for_single_well(
            fmt,
            engine_available=engine_available(),
            force_backend=force_backend,  # type: ignore[arg-type]
        )
        kwargs: dict[str, Any] = {
            "path": str(path),
            "page_spec": PageSpec(),
            "backend": backend,
            "paint_fn": self._paint_active_plot,
        }
        if backend == "engine":
            view, doc_id = self._prepare_engine_export_view()
            kwargs["view"] = view
            kwargs["document_id"] = doc_id
        try:
            return export_plot(plot, fmt, **kwargs)
        except (ExportError, Exception):
            if backend != "engine":
                raise
            # Graceful fallback to Qt paint
            kwargs["backend"] = "qt"
            kwargs.pop("view", None)
            kwargs.pop("document_id", None)
            return export_plot(plot, fmt, **kwargs)

    def _prepare_engine_export_view(self) -> tuple[Any, str]:
        """Ensure WellLogView has the current single-well scene for export."""
        if self._presentation is None:
            raise ExportError("无活动单井图版")
        if not engine_available():
            raise EngineUnavailable("WellLogEngine 不可用")
        prev = self._prefer_engine_canvas
        self._prefer_engine_canvas = True
        try:
            if self.multi_track_canvas.pick_mode():
                self._act_pick_tops.setChecked(False)
                self.multi_track_canvas.set_pick_mode(False)
            self._sync_primary_single_well_surface()
            if self._engine_view is None or self._primary_surface != "engine":
                # Force submit even if user preferred host
                view = self._ensure_engine_view(self._engine_page)
                load_presentation_into_view(
                    view, self._presentation, tops=self._active_tops
                )
                self._engine_view = view
            doc_id = self._presentation.well_document_id
            return self._engine_view, doc_id
        finally:
            self._prefer_engine_canvas = prev

    def open_engine_preview(self) -> dict[str, object]:
        """Force engine primary surface and submit multi-track presentation."""
        if self._presentation is None:
            raise EngineUnavailable("无活动单井图版展示")
        if not engine_available():
            cap = probe_engine()
            raise EngineUnavailable(
                f"WellLogEngine 不可用: {cap.detail}\n"
                "请安装 welllog-engine wheel 或将 build 产物加入 PYTHONPATH。"
            )
        # Temporarily prefer engine even if user had host forced via menu
        prev = self._prefer_engine_canvas
        self._prefer_engine_canvas = True
        self._act_prefer_engine.setChecked(True)
        # Exit pick mode so engine can show
        if self.multi_track_canvas.pick_mode():
            self._act_pick_tops.setChecked(False)
            self.multi_track_canvas.set_pick_mode(False)
        self._sync_primary_single_well_surface()
        self.document_tabs.setCurrentIndex(0)
        self._update_status()
        if self._primary_surface != "engine" or self._engine_view is None:
            self._prefer_engine_canvas = prev
            self._act_prefer_engine.setChecked(prev)
            raise EngineSubmitError(
                self._engine_last_error or "引擎提交失败，已保持主机画布"
            )
        # Re-submit to return report to caller
        return load_presentation_into_view(
            self._engine_view,
            self._presentation,
            tops=self._active_tops,
        )

    def open_engine_correlation_preview(self) -> dict[str, object]:
        """Force engine multi-well surface for the active correlation plot."""
        if len(self._correlation_presentations) < 2:
            raise EngineUnavailable("请先创建/打开 ≥2 井的地层对比图")
        if not engine_available():
            cap = probe_engine()
            raise EngineUnavailable(
                f"WellLogEngine 不可用: {cap.detail}\n"
                "请安装 welllog-engine wheel 或将 build 产物加入 PYTHONPATH。"
            )
        prev = self._prefer_engine_canvas
        self._prefer_engine_canvas = True
        self._act_prefer_engine.setChecked(True)
        self.document_tabs.setCurrentIndex(1)
        self._sync_primary_correlation_surface()
        self._update_status()
        if self._primary_surface != "engine" or self._engine_view is None:
            self._prefer_engine_canvas = prev
            self._act_prefer_engine.setChecked(prev)
            raise EngineSubmitError(
                self._engine_last_error or "引擎对比提交失败，已保持主机画布"
            )
        tops_cols = self.correlation_canvas.tops_per_column()
        depth = self.correlation_canvas.depth_range()
        return submit_multi_well_presentations(
            self._engine_view,
            self._correlation_presentations,
            tops_per_well=tops_cols,
            shared_depth=depth,
            links=self._correlation_links,
        )

    def _select_well_in_tree(self, well_id: str) -> None:
        def walk(item: QTreeWidgetItem) -> QTreeWidgetItem | None:
            data = item.data(0, Qt.ItemDataRole.UserRole) or {}
            if data.get("kind") == "well" and data.get("id") == well_id:
                return item
            for i in range(item.childCount()):
                hit = walk(item.child(i))
                if hit is not None:
                    return hit
            return None

        for i in range(self.workspace_tree.topLevelItemCount()):
            hit = walk(self.workspace_tree.topLevelItem(i))
            if hit is not None:
                self.workspace_tree.setCurrentItem(hit)
                return

    def _on_tree_selection(
        self, cur: QTreeWidgetItem | None, _prev: QTreeWidgetItem | None
    ) -> None:
        if cur is None:
            return
        data = cur.data(0, Qt.ItemDataRole.UserRole) or {}
        kind = data.get("kind")
        # Selecting well / its data source / a track leaf focuses that well
        if kind in ("well", "source", "leaf"):
            well_id = data.get("well_id") or data.get("id")
            if kind == "well":
                well_id = data.get("id")
            if well_id:
                self._selected_well_id = str(well_id)
                self.set_view_mode(self.view_mode_for(self._selected_well_id))
                self._refresh_tops_list()
                self._sync_apply_enabled()
                self._update_status()

    def _on_tree_double_click(self, item: QTreeWidgetItem, _column: int) -> None:
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        kind = data.get("kind")
        # Double-click a track leaf → import that data into the current plot
        if kind == "leaf":
            try:
                self.import_leaf_to_active_plot(str(data.get("id") or ""))
            except WorkspaceError as exc:
                QMessageBox.warning(self, "加入井图失败", str(exc))
            return
        if kind != "plot":
            return
        plot_id = str(data.get("id") or "")
        if not plot_id:
            return
        try:
            self.open_plot_document(plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "打开图件失败", str(exc))

    def import_leaf_to_active_plot(self, leaf_id: str) -> HostPresentation:
        """从数据区把井道挂到当前井图（写 binding_id，不复制样点）。"""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        if (
            not self._active_plot_id
            or self._active_plot_type != "single_well"
            or not self._selected_well_id
        ):
            raise WorkspaceError("请先打开一张单井分析图，再从数据区加入井道")
        leaf_id = str(leaf_id).strip()
        if not leaf_id:
            raise WorkspaceError("未指定井道")
        well_id = self._selected_well_id
        current = set(self.display_set_for(well_id) or frozenset())
        current.add(leaf_id)
        tid = self._current_template_id() or "std-gr-rt-den"
        return self.set_display_set(
            well_id,
            current,
            template_id=tid,
            plot_id=self._active_plot_id,
        )

    def _on_add_selected_leaf_to_plot(self) -> None:
        item = self.workspace_tree.currentItem()
        if item is None:
            QMessageBox.information(self, "加入井图", "请在树中选中一个井道叶子。")
            return
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if data.get("kind") != "leaf":
            QMessageBox.information(self, "加入井图", "请选中井道叶子（数据源下的曲线）。")
            return
        try:
            self.import_leaf_to_active_plot(str(data.get("id") or ""))
            QMessageBox.information(
                self,
                "已加入井图",
                f"井道已绑定到当前图（含 binding 标识）。\n"
                f"leaf={data.get('id')}",
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加入井图失败", str(exc))

    def _on_export_plot_xml(self) -> None:
        if self._workspace is None or not self._active_plot_id:
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "导出井图定义 XML", "", "XML (*.xml)"
        )
        if not path:
            return
        try:
            doc = load_plot_document(self._workspace, self._active_plot_id)
            export_plot_xml(doc, path)
            QMessageBox.information(self, "导出完成", f"已写入\n{path}")
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "导出失败", str(exc))

    def _on_export_plot_xlsx(self) -> None:
        if self._workspace is None or not self._active_plot_id:
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "导出井图定义 Excel", "", "Excel (*.xlsx)"
        )
        if not path:
            return
        try:
            doc = load_plot_document(self._workspace, self._active_plot_id)
            if not path.lower().endswith(".xlsx"):
                path = path + ".xlsx"
            export_plot_excel(doc, path)
            QMessageBox.information(self, "导出完成", f"已写入\n{path}")
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "导出失败", str(exc))

    def _on_import_plot_xml(self) -> None:
        if self._workspace is None:
            QMessageBox.information(self, "导入井图", "请先打开工区。")
            return
        path, _ = QFileDialog.getOpenFileName(
            self, "导入井图定义 XML", "", "XML (*.xml)"
        )
        if not path:
            return
        try:
            doc = import_plot_xml(self._workspace, path)
            self.open_plot_document(doc.id)
            self._refresh_tree()
            QMessageBox.information(
                self,
                "导入完成",
                f"已创建/更新井图「{doc.name}」\n"
                f"绑定井道 {len(doc.data_bindings)} 条（样点仍在井数据中）",
            )
        except (WorkspaceError, OSError, ET.ParseError) as exc:
            QMessageBox.warning(self, "导入失败", str(exc))

    def _on_import_plot_xlsx(self) -> None:
        if self._workspace is None:
            QMessageBox.information(self, "导入井图", "请先打开工区。")
            return
        path, _ = QFileDialog.getOpenFileName(
            self,
            "导入井图定义 Excel",
            "",
            "Excel/CSV (*.xlsx *.csv)",
        )
        if not path:
            return
        try:
            doc = import_plot_excel(self._workspace, path)
            self.open_plot_document(doc.id)
            self._refresh_tree()
            QMessageBox.information(
                self,
                "导入完成",
                f"已创建/更新井图「{doc.name}」\n"
                f"绑定井道 {len(doc.data_bindings)} 条",
            )
        except (WorkspaceError, OSError, KeyError, zipfile.BadZipFile) as exc:
            QMessageBox.warning(self, "导入失败", str(exc))

    def _refresh_tree(self) -> None:
        """Single tree: workspace → wells (with data→tracks) → plots."""
        tree = self.workspace_tree
        self._content_tree_guard = True
        try:
            tree.clear()
            if self._workspace is None:
                self.left_title.setText("工区")
                self.well_content_hint.setText("请先打开或新建工区")
                root = QTreeWidgetItem(["（未打开工区）"])
                root.addChild(QTreeWidgetItem(["井"]))
                root.addChild(QTreeWidgetItem(["图件"]))
                tree.addTopLevelItem(root)
                tree.expandAll()
                return

            ws = self._workspace
            self.left_title.setText(f"工区 · {ws.name}")
            root = QTreeWidgetItem([ws.name])
            root.setData(0, Qt.ItemDataRole.UserRole, {"kind": "workspace"})

            wells_node = QTreeWidgetItem(["井（数据）"])
            wells_node.setData(0, Qt.ItemDataRole.UserRole, {"kind": "wells_folder"})
            total_leaves = 0
            for well in ws.wells:
                item = QTreeWidgetItem([well.name])
                item.setData(
                    0,
                    Qt.ItemDataRole.UserRole,
                    {
                        "kind": "well",
                        "id": well.id,
                        "well_id": well.id,
                        "path": well.path,
                    },
                )
                item.setToolTip(0, well.path or well.id)
                n = self._attach_well_data_branch(item, well.id)
                total_leaves += n
                wells_node.addChild(item)
            if not ws.wells:
                empty = QTreeWidgetItem(["（无井 · 请导入 LAS）"])
                empty.setDisabled(True)
                wells_node.addChild(empty)

            plots_node = QTreeWidgetItem(["图件（绘图）"])
            plots_node.setData(0, Qt.ItemDataRole.UserRole, {"kind": "plots_folder"})
            for plot in ws.plots:
                label = plot.name
                type_labels = {
                    "single_well": "[单井·多图道]",
                    "correlation": "[对比]",
                    "section": "[剖面]",
                    "plane_map": "[平面图]",
                    "fence_3d": "[栅状图]",
                    "composite": "[综合图]",
                }
                suffix = type_labels.get(plot.type, "")
                if suffix:
                    label = f"{plot.name} {suffix}"
                item = QTreeWidgetItem([label])
                item.setData(
                    0,
                    Qt.ItemDataRole.UserRole,
                    {"kind": "plot", "id": plot.id, "type": plot.type},
                )
                plots_node.addChild(item)
            if not ws.plots:
                empty = QTreeWidgetItem(["（无图件 · 新建单井分析图）"])
                empty.setDisabled(True)
                plots_node.addChild(empty)

            root.addChild(wells_node)
            root.addChild(plots_node)
            tree.addTopLevelItem(root)
            tree.expandToDepth(3)
            if self._selected_well_id:
                self._select_well_in_tree(self._selected_well_id)
            plot_note = (
                f"当前井图 {self._active_plot_id[:8]}…"
                if self._active_plot_id and self._active_plot_type == "single_well"
                else "勾选作用于当前打开的单井图"
            )
            self.well_content_hint.setText(
                f"井→数据源→井道 · 共 {total_leaves} 井道叶子 · {plot_note}"
            )
        finally:
            self._content_tree_guard = False

    def _visual_display_set_for_well(self, well_id: str) -> frozenset[str]:
        """Display Set for tree checkboxes (plot doc / session / template defaults)."""
        key = self._display_set_key(well_id)
        existing = self._display_sets.get(key)
        if existing is not None:
            return existing
        if self._workspace is None:
            return frozenset()
        # Prefer plot document when a single-well plot is active
        if self._active_plot_id and self._active_plot_type == "single_well":
            try:
                plot_doc = load_plot_document(self._workspace, self._active_plot_id)
                if plot_doc.display_set:
                    return frozenset(str(x) for x in plot_doc.display_set)
            except WorkspaceError:
                pass
        try:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
        except Exception:  # noqa: BLE001
            return frozenset()
        tid = self._current_template_id() or "std-gr-rt-den"
        template = get_builtin_template(tid)
        if template is None:
            return frozenset()
        return default_checks(leaves_from_document(doc), template)

    def _attach_well_data_branch(self, well_item: QTreeWidgetItem, well_id: str) -> int:
        """Under a well node: import source → checkable track leaves. Returns leaf count."""
        if self._workspace is None:
            return 0
        try:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
        except Exception as exc:  # noqa: BLE001
            err = QTreeWidgetItem([f"（数据未加载: {exc}）"])
            err.setDisabled(True)
            well_item.addChild(err)
            return 0

        leaves = leaves_from_document(doc)
        checked = self._visual_display_set_for_well(well_id)
        source_label = Path(doc.source_path).name if doc.source_path else "导入源"
        source_item = QTreeWidgetItem([source_label])
        source_item.setData(
            0,
            Qt.ItemDataRole.UserRole,
            {
                "kind": "source",
                "source_id": doc.source_path or doc.document_id,
                "well_id": well_id,
            },
        )
        source_item.setFlags(
            Qt.ItemFlag.ItemIsEnabled
            | Qt.ItemFlag.ItemIsSelectable
            | Qt.ItemFlag.ItemIsUserCheckable
        )
        source_item.setToolTip(
            0,
            f"导入源 · {doc.depth_unit or 'm'} · "
            f"{int(doc.depth.size)} 样点 · {len(leaves)} 井道",
        )

        n_checked = 0
        for leaf in leaves:
            child = QTreeWidgetItem([leaf.label or leaf.mnemonic])
            child.setData(
                0,
                Qt.ItemDataRole.UserRole,
                {
                    "kind": "leaf",
                    "id": leaf.id,
                    "mnemonic": leaf.mnemonic,
                    "well_id": well_id,
                },
            )
            child.setFlags(
                Qt.ItemFlag.ItemIsEnabled
                | Qt.ItemFlag.ItemIsSelectable
                | Qt.ItemFlag.ItemIsUserCheckable
            )
            on = leaf.id in checked
            child.setCheckState(
                0, Qt.CheckState.Checked if on else Qt.CheckState.Unchecked
            )
            if on:
                n_checked += 1
            child.setToolTip(0, f"井道 {leaf.mnemonic}")
            source_item.addChild(child)

        if not leaves:
            none = QTreeWidgetItem(["（无井道）"])
            none.setDisabled(True)
            source_item.addChild(none)
        elif n_checked == 0:
            source_item.setCheckState(0, Qt.CheckState.Unchecked)
        elif n_checked == len(leaves):
            source_item.setCheckState(0, Qt.CheckState.Checked)
        else:
            source_item.setCheckState(0, Qt.CheckState.PartiallyChecked)

        well_item.addChild(source_item)
        return len(leaves)

    def _refresh_well_content_tree(self) -> None:
        """Compat: content is embedded in workspace_tree — full rebuild."""
        self._refresh_tree()

    def _collect_content_tree_checked_leaves(
        self, well_id: str | None = None
    ) -> frozenset[str]:
        """Collect checked track leaves from the unified workspace tree."""
        wid = well_id or self._selected_well_id
        if not wid:
            return frozenset()
        ids: set[str] = set()

        def walk(item: QTreeWidgetItem) -> None:
            data = item.data(0, Qt.ItemDataRole.UserRole) or {}
            if (
                data.get("kind") == "leaf"
                and str(data.get("well_id") or "") == wid
                and item.checkState(0) == Qt.CheckState.Checked
            ):
                ids.add(str(data["id"]))
            for i in range(item.childCount()):
                walk(item.child(i))

        tree = self.workspace_tree
        for i in range(tree.topLevelItemCount()):
            walk(tree.topLevelItem(i))
        return frozenset(ids)

    def _on_well_content_item_changed(
        self, item: QTreeWidgetItem, _column: int
    ) -> None:
        """Checkbox toggles on well→source→track leaves (unified tree)."""
        if self._content_tree_guard:
            return
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        kind = data.get("kind")
        if kind not in ("leaf", "source"):
            return
        well_id = str(data.get("well_id") or "")
        if not well_id:
            return
        self._selected_well_id = well_id

        if kind == "source":
            state = item.checkState(0)
            if state == Qt.CheckState.PartiallyChecked:
                return
            self._content_tree_guard = True
            try:
                for j in range(item.childCount()):
                    child = item.child(j)
                    cdata = child.data(0, Qt.ItemDataRole.UserRole) or {}
                    if cdata.get("kind") == "leaf":
                        child.setCheckState(0, state)
            finally:
                self._content_tree_guard = False

        if not self._content_apply_pending:
            self._content_apply_pending = True
            QTimer.singleShot(0, self._flush_content_tree_checks)

    def _flush_content_tree_checks(self) -> None:
        self._content_apply_pending = False
        if self._selected_well_id is None or self._workspace is None:
            return
        well_id = self._selected_well_id
        checked = self._collect_content_tree_checked_leaves(well_id)
        try:
            self.set_display_set(well_id, checked)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "更新显示集失败", str(exc))
            self._refresh_tree()

    def _on_new_workspace(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "选择空目录作为新工区")
        if not path:
            return
        try:
            ws = create_workspace(Path(path))
            self.set_workspace(ws)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "新建工区失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "新建工区失败", str(exc))

    def _on_open_workspace(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "打开工区目录")
        if not path:
            return
        try:
            ws = open_workspace(path)
            self.set_workspace(ws)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "打开工区失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "打开工区失败", str(exc))

    def _on_open_recent_workspace(self, path: str) -> None:
        """Open a path from the startup recent list (#291)."""
        p = Path(path).expanduser()
        if not p.is_dir():
            QMessageBox.warning(
                self,
                "无法打开",
                f"路径不存在或不是目录：\n{path}\n\n已从最近列表移除。",
            )
            remove_recent(path)
            if hasattr(self, "startup_page"):
                self.startup_page.refresh_recent()
            return
        try:
            ws = open_workspace(p)
            self.set_workspace(ws)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "打开工区失败", str(exc))
            # Keep in list if still a dir but invalid workspace? remove if hard fail
            if "不存在" in str(exc) or "workspace" in str(exc).lower():
                remove_recent(path)
                if hasattr(self, "startup_page"):
                    self.startup_page.refresh_recent()
        except OSError as exc:
            QMessageBox.warning(self, "打开工区失败", str(exc))
            remove_recent(path)
            if hasattr(self, "startup_page"):
                self.startup_page.refresh_recent()
    def _on_import_las(self) -> None:
        if self._workspace is None:
            QMessageBox.information(self, "导入 LAS", "请先打开或新建工区。")
            return
        path, _ = QFileDialog.getOpenFileName(
            self,
            "选择 LAS 文件",
            "",
            "LAS (*.las *.LAS);;All (*.*)",
        )
        if not path:
            return
        try:
            well_id = self.import_las_path(path)
            doc = self.session.get(well_id)
            n_curves = len(doc.curves) if doc else 0
            extra = ""
            if doc and doc.diagnostics:
                extra = "\n\n提示:\n- " + "\n- ".join(doc.diagnostics[:8])
            QMessageBox.information(
                self,
                "导入成功",
                f"已导入井「{doc.well_name if doc else well_id}」\n"
                f"曲线数: {n_curves}\n"
                f"路径: {doc.source_path if doc else ''}"
                f"{extra}\n\n"
                f"请在右栏选择图版并「应用到选中井」以显示多图道。",
            )
        except (LasImportError, WorkspaceError) as exc:
            QMessageBox.warning(self, "导入 LAS 失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "导入 LAS 失败", str(exc))

    def _on_apply_template(self) -> None:
        if self._selected_well_id is None:
            QMessageBox.information(self, "应用图版", "请先在左树选择一口井。")
            return
        template_id = self._current_template_id()
        if not template_id:
            QMessageBox.information(self, "应用图版", "请选择图版模板。")
            return
        try:
            # Update open plot document template if one is active for this well
            if (
                self._active_plot_id
                and self._workspace is not None
            ):
                try:
                    plot = load_plot_document(self._workspace, self._active_plot_id)
                    if plot.well_ids == [self._selected_well_id]:
                        plot.template_id = template_id
                        save_plot_document(self._workspace, plot)
                except WorkspaceError:
                    pass
            pres = self.apply_template_to_well(
                self._selected_well_id,
                template_id,
                plot_id=self._active_plot_id,
            )
            if pres.curve_track_count < 1:
                detail = "显示集为空 · 勾选井道以显示"
            else:
                detail = (
                    f"图道数 {pres.track_count}（曲线道 {pres.curve_track_count}）"
                )
            QMessageBox.information(
                self,
                "图版已应用",
                f"井 {pres.well_name}\n"
                f"图版 {pres.template_name}\n"
                f"{detail}",
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "应用图版失败", str(exc))

    def _on_new_single_well_plot(self) -> None:
        if self._selected_well_id is None:
            QMessageBox.information(self, "新建单井分析图", "请先选择一口井。")
            return
        template_id = self._current_template_id()
        if not template_id:
            QMessageBox.information(self, "新建单井分析图", "请选择图版模板。")
            return
        try:
            plot = self.create_single_well_plot_document(
                self._selected_well_id, template_id
            )
            QMessageBox.information(
                self,
                "图件已创建",
                f"已保存 {plot.path}\n"
                f"井绑定 {', '.join(plot.well_ids)}\n"
                f"图版 {plot.template_id}\n"
                f"双击左树图件可重新打开。",
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "新建图件失败", str(exc))

    def _pick_wells_for_correlation(self) -> list[str] | None:
        """Multi-select dialog; returns well ids or None if cancelled."""
        if self._workspace is None or len(self._workspace.wells) < 2:
            return None
        dlg = QDialog(self)
        dlg.setWindowTitle("选择对比井（≥2）")
        dlg.setObjectName("Dialog_PickCorrelationWells")
        layout = QVBoxLayout(dlg)
        layout.addWidget(QLabel("按住 Ctrl 多选井；至少 2 口："))
        lst = QListWidget()
        lst.setObjectName("List_CorrelationWells")
        lst.setSelectionMode(QAbstractItemView.SelectionMode.MultiSelection)
        for well in self._workspace.wells:
            item = QListWidgetItem(well.name)
            item.setData(Qt.ItemDataRole.UserRole, well.id)
            lst.addItem(item)
            item.setSelected(True)
        layout.addWidget(lst)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        layout.addWidget(buttons)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return None
        ids = [
            str(it.data(Qt.ItemDataRole.UserRole))
            for it in lst.selectedItems()
            if it.data(Qt.ItemDataRole.UserRole)
        ]
        if len(ids) < 2:
            QMessageBox.information(
                self, "新建地层对比图", "请至少选择 2 口井。"
            )
            return None
        return ids

    def _on_new_correlation_plot(self) -> None:
        if self._workspace is None:
            QMessageBox.information(self, "新建地层对比图", "请先打开工区。")
            return
        if len(self._workspace.wells) < 2:
            QMessageBox.information(
                self, "新建地层对比图", "工区至少需要 2 口井。"
            )
            return
        template_id = self._current_template_id()
        if not template_id:
            QMessageBox.information(self, "新建地层对比图", "请选择图版模板。")
            return
        well_ids = self._pick_wells_for_correlation()
        if not well_ids:
            return
        try:
            plot = self.create_correlation_plot_document(well_ids, template_id)
            QMessageBox.information(
                self,
                "对比图已创建",
                f"已保存 {plot.path}\n"
                f"井 {len(plot.well_ids)} 口 · 图版 {plot.template_id}\n"
                f"滚轮缩放 / 拖动平移共享深度。\n"
                f"双击左树图件可重新打开。",
            )
        except (WorkspaceError, ExportError) as exc:
            QMessageBox.warning(self, "新建对比图失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "新建对比图失败", str(exc))

    def _on_new_plane_map_plot(self) -> None:
        """Create + open a plane_map plot (Phase-2 T2 / #246)."""
        if self._workspace is None:
            QMessageBox.information(self, "新建平面图", "请先打开工区。")
            return
        if not self.plane_map_view.mapping_available():
            QMessageBox.warning(
                self, "新建平面图",
                "平面图需要 geoviz mapping 表面（geoviz_paleo_map + CRS helpers）。"
                "请确认 geo-viz-engine 已安装。",
            )
            return
        template_id = self._current_template_id() or "std-gr-rt-den"
        try:
            plot = create_plane_map_plot(
                self._workspace,
                wells=[w.id for w in self._workspace.wells],
                template_id=template_id,
            )
            self._active_plot_id = plot.id
            self._active_plot_type = "plane_map"
            self._show_plane_map(plot)
            self._refresh_tree()
            QMessageBox.information(
                self, "平面图已创建",
                f"已保存 {plot.path}\n"
                f"绘制 {len(PlaneMapView.filter_wells_with_coords(self._workspace.wells))}"
                f"/{len(self._workspace.wells)} 口含坐标井。",
            )
        except (WorkspaceError, ExportError) as exc:
            QMessageBox.warning(self, "新建平面图失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "新建平面图失败", str(exc))

    def _on_new_fence_3d_plot(self) -> None:
        """Create + open a fence_3d plot (Phase-2 T6 / #250)."""
        if self._workspace is None:
            QMessageBox.information(self, "新建三维栅状图", "请先打开工区。")
            return
        if not probe_3d().available:
            cap = probe_3d()
            QMessageBox.warning(
                self, "新建三维栅状图",
                f"三维栅状图需要 pyqtgraph + OpenGL。\n{cap.detail}",
            )
            return
        wells_with_coords = [
            w for w in self._workspace.wells
            if w.lng is not None and w.lat is not None
        ]
        if len(wells_with_coords) < 2:
            QMessageBox.information(
                self, "新建三维栅状图",
                "三维栅状图至少需要 2 口含坐标（lng/lat）的井。",
            )
            return
        template_id = self._current_template_id() or "std-gr-rt-den"
        try:
            plot = create_fence_3d_plot(
                self._workspace,
                well_ids=[w.id for w in wells_with_coords],
                template_id=template_id,
            )
            self._active_plot_id = plot.id
            self._active_plot_type = "fence_3d"
            self._show_fence_3d(plot)
            self._refresh_tree()
            QMessageBox.information(
                self, "三维栅状图已创建",
                f"已保存 {plot.path}\n"
                f"{len(plot.well_ids)} 口井 · 拖拽旋转 / 滚轮缩放。",
            )
        except (WorkspaceError, ExportError) as exc:
            QMessageBox.warning(self, "新建三维栅状图失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "新建三维栅状图失败", str(exc))

    def _on_new_section_plot(self) -> None:
        """Create + open a reservoir section plot (Phase-2 T4/T5)."""
        if self._workspace is None:
            QMessageBox.information(self, "新建油藏剖面", "请先打开工区。")
            return
        if len(self._workspace.wells) < 2:
            QMessageBox.information(
                self, "新建油藏剖面", "油藏剖面至少需要 2 口井。"
            )
            return
        template_id = self._current_template_id()
        if not template_id:
            QMessageBox.information(self, "新建油藏剖面", "请选择图版模板。")
            return
        well_ids = self._pick_wells_for_correlation()
        if not well_ids or len(well_ids) < 2:
            QMessageBox.information(
                self, "新建油藏剖面", "请至少选择 2 口井。"
            )
            return
        try:
            plot = create_section_plot(
                self._workspace,
                well_ids=well_ids,
                template_id=template_id,
            )
            self._active_plot_id = plot.id
            self._active_plot_type = "section"
            self._show_section(plot)
            self._refresh_tree()
            QMessageBox.information(
                self, "油藏剖面已创建",
                f"已保存 {plot.path}\n"
                f"{len(plot.well_ids)} 口井 · 滚轮缩放 / 拖动平移。",
            )
        except (WorkspaceError, ExportError) as exc:
            QMessageBox.warning(self, "新建油藏剖面失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "新建油藏剖面失败", str(exc))

    def _on_new_composite_plot(self) -> None:
        """Create + open a composite figure (Phase-2 T7)."""
        if self._workspace is None:
            QMessageBox.information(self, "新建油藏综合图", "请先打开工区。")
            return
        template_id = self._current_template_id() or "std-gr-rt-den"
        source_ids = [
            p.id for p in self._workspace.plots if p.type != "composite"
        ]
        if not source_ids:
            QMessageBox.information(
                self, "新建油藏综合图",
                "请先创建其他图件（单井/对比/剖面/平面图/栅状图）作为面板来源。",
            )
            return
        try:
            plot = create_composite_plot(
                self._workspace,
                panels=[PanelRef(plot_id=sid) for sid in source_ids[:4]],
                template_id=template_id,
            )
            self._active_plot_id = plot.id
            self._active_plot_type = "composite"
            self._show_composite(plot)
            self._refresh_tree()
            QMessageBox.information(
                self, "油藏综合图已创建",
                f"已保存 {plot.path}\n"
                f"{len(plot.panels)} 个面板 · 可在纸面拖拽排版。",
            )
        except (WorkspaceError, ExportError) as exc:
            QMessageBox.warning(self, "新建油藏综合图失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "新建油藏综合图失败", str(exc))

    def _on_set_coordinate_reference(self) -> None:
        """Open the CRS trio editor (Phase-2 T2 / #246)."""
        if self._workspace is None:
            QMessageBox.information(self, "坐标系设置", "请先打开工区。")
            return
        dlg = CoordinateReferenceDialog(self._workspace.coordinate, parent=self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        self._workspace.coordinate = dlg.result_coordinate()
        save_workspace(self._workspace)
        # Re-project the active plane map if open.
        if self._active_plot_type == "plane_map":
            self.plane_map_view.set_coordinate(self._workspace.coordinate)
            self.plane_map_view.set_plot_data(self._workspace.wells)
        QMessageBox.information(
            self, "坐标系已更新",
            f"项目坐标系: {self._workspace.coordinate.project_crs}\n"
            f"目标坐标系: {self._workspace.coordinate.target_crs or '—'}\n"
            f"显示坐标系: {self._workspace.coordinate.display_crs}",
        )

    def _choose_single_well_pdf_text_mode(self) -> PdfTextMode | None:
        """ADR 0053 dual option: outline (engine) vs searchable (Qt B1.PDF.1).

        Returns None if the user cancels.
        """
        box = QMessageBox(self)
        box.setWindowTitle("导出 PDF — 文本模式")
        box.setIcon(QMessageBox.Icon.Question)
        box.setText(
            "请选择单井 PDF 文本模式（ADR 0053 / 导出 B1）："
        )
        box.setInformativeText(
            "• 引擎图形 PDF：高保真矢量，文字为轮廓、不可搜索（B0 默认）。\n"
            "• 可搜索 PDF：文字可选中/可复制（B1.PDF.1，当前 Qt 路径）。"
        )
        btn_outline = box.addButton(
            "引擎图形 PDF（不可搜索）", QMessageBox.ButtonRole.AcceptRole
        )
        btn_search = box.addButton(
            "可搜索 PDF", QMessageBox.ButtonRole.AcceptRole
        )
        btn_cancel = box.addButton(QMessageBox.StandardButton.Cancel)
        box.setDefaultButton(btn_outline)
        box.exec()
        clicked = box.clickedButton()
        if clicked is None or clicked == btn_cancel:
            return None
        if clicked == btn_search:
            return "searchable"
        return "outline"

    def _export_active_plot(self, fmt: ExportFormat) -> None:
        """Route the active plot's export through the T8 dispatcher.

        Single-well SVG/PDF default to the engine backend when available (T11).
        Single-well PDF offers outline vs searchable dual mode (ADR 0053).
        """
        if self._workspace is None or not self._active_plot_id:
            QMessageBox.information(
                self, "导出", "请先打开/创建图件。"
            )
            return
        plot = load_plot_document(self._workspace, self._active_plot_id)
        default = f"{plot.name or 'plot'}.{fmt}"
        path, _ = QFileDialog.getSaveFileName(
            self,
            f"导出 {fmt.upper()}",
            default,
            f"{fmt.upper()} (*.{fmt});;All (*.*)",
        )
        if not path:
            return
        kwargs: dict = {"path": path, "page_spec": PageSpec()}
        backend_note = ""
        try:
            if plot.type == "single_well" and fmt in ("svg", "pdf"):
                eng_ok = engine_available()
                pdf_text_mode: PdfTextMode = "outline"
                if fmt == "pdf":
                    chosen = self._choose_single_well_pdf_text_mode()
                    if chosen is None:
                        return
                    pdf_text_mode = chosen
                    if pdf_text_mode == "searchable":
                        QMessageBox.information(
                            self, "可搜索 PDF", PDF_SEARCHABLE_MODE_NOTE
                        )
                    backend, backend_note = resolve_single_well_pdf_export(
                        engine_available=eng_ok,
                        pdf_text_mode=pdf_text_mode,
                    )
                else:
                    backend = prefer_engine_for_single_well(
                        "svg", engine_available=eng_ok
                    )
                    backend_note = (
                        "（引擎）" if backend == "engine" else "（Qt）"
                    )
                if backend == "engine":
                    # Outline engine PDF still needs the non-searchable disclosure.
                    if (
                        pdf_text_mode == "outline"
                        and engine_pdf_needs_disclosure("engine", fmt)
                    ):
                        reply = QMessageBox.warning(
                            self,
                            "引擎 PDF 说明",
                            ENGINE_PDF_NONSEARCHABLE_DISCLOSURE
                            + "\n\n是否继续使用引擎图形 PDF 导出？",
                            QMessageBox.StandardButton.Ok
                            | QMessageBox.StandardButton.Cancel,
                            QMessageBox.StandardButton.Ok,
                        )
                        if reply != QMessageBox.StandardButton.Ok:
                            return
                    try:
                        view, doc_id = self._prepare_engine_export_view()
                        kwargs["backend"] = "engine"
                        kwargs["view"] = view
                        kwargs["document_id"] = doc_id
                        if fmt == "pdf":
                            kwargs["pdf_text_mode"] = pdf_text_mode
                    except (EngineUnavailable, EngineSubmitError, ExportError):
                        kwargs["backend"] = "qt"
                        kwargs["paint_fn"] = self._paint_active_plot
                        backend_note = "（引擎不可用，已回退 Qt）"
                else:
                    kwargs["backend"] = "qt"
                    kwargs["paint_fn"] = self._paint_active_plot
            elif plot.type == "correlation":
                # B0 (#300): Qt paint for SVG/PDF; PNG prefers widget grab for
                # links/datum fidelity, with paint_fn fallback.
                if fmt == "png":
                    out = self._export_correlation_png(Path(path))
                    QMessageBox.information(
                        self,
                        "导出成功",
                        f"PNG 已写入（对比图 · Qt 画布抓取）:\n{out}\n"
                        f"大小 {out.stat().st_size} 字节",
                    )
                    return
                kwargs["paint_fn"] = self._paint_active_plot
                kwargs["backend"] = "qt"
                backend_note = "（对比图 · Qt 矢量）"
            elif plot.type == "section":
                kwargs["paint_fn"] = self._paint_active_plot
            elif plot.type == "plane_map":
                kwargs["canvas"] = self.plane_map_view._canvas
            elif plot.type == "fence_3d":
                kwargs["view"] = self.fence_3d_view
            elif plot.type == "composite":
                kwargs["window"] = self.composite_view._layout_window
            out = export_plot(plot, fmt, **kwargs)
            audit_command(
                f"export.{fmt}",
                ok=True,
                target=str(self._active_plot_id or ""),
                detail=backend_note,
            )
            QMessageBox.information(
                self,
                "导出成功",
                f"{fmt.upper()} 已写入{backend_note}:\n{out}\n"
                f"大小 {out.stat().st_size} 字节",
            )
        except (ExportError, UnsupportedFormatError) as exc:
            audit_command(
                f"export.{fmt}",
                ok=False,
                target=str(getattr(self, "_active_plot_id", "") or ""),
                detail=str(exc)[:200],
            )
            QMessageBox.warning(self, f"导出 {fmt.upper()} 失败", str(exc))
        except OSError as exc:
            audit_command(
                f"export.{fmt}",
                ok=False,
                target=str(getattr(self, "_active_plot_id", "") or ""),
                detail=str(exc)[:200],
            )
            QMessageBox.warning(self, f"导出 {fmt.upper()} 失败", str(exc))

    def _paint_active_plot(self, painter, rect) -> None:
        """Paint callback for the T8 Qt-paint export path (single/corr/section)."""
        if self._active_plot_type == "single_well" and self._presentation is not None:
            from well_log_workstation.export_plot import _paint_presentation
            _paint_presentation(painter, self._presentation, rect)
        elif self._active_plot_type == "correlation":
            self._paint_correlation_export(painter, rect)
        elif self._active_plot_type == "section":
            # Section: paint the section canvas into the rect.
            self.section_canvas.render(painter)

    def _paint_correlation_export(self, painter, rect) -> None:
        """Vector export for correlation: columns + links (Qt paint, B0 #300)."""
        from well_log_workstation.export_plot import _paint_presentation

        presentations = self._correlation_presentations
        n = max(1, len(presentations))
        gap = 6.0
        col_w = (rect.width() - gap * (n - 1)) / n if n else rect.width()
        for i, pres in enumerate(presentations):
            sub = QRectF(
                rect.x() + i * (col_w + gap),
                rect.y(),
                col_w,
                rect.height(),
            )
            _paint_presentation(painter, pres, sub)
        # Overlay horizon links in shared display depth if available
        links = self._correlation_links
        if not links or n < 2:
            return
        from PySide6.QtGui import QColor, QPen

        # Approximate shared depth band (middle 80% of column height)
        top = rect.y() + rect.height() * 0.12
        bottom = rect.y() + rect.height() * 0.92
        # Collect depth ranges from presentations
        import numpy as np

        shifts = self.correlation_canvas.depth_shifts()
        d0s: list[float] = []
        d1s: list[float] = []
        for pres in presentations:
            depth = np.asarray(pres.depth, dtype=np.float64)
            if depth.size:
                s = float(shifts.get(pres.well_document_id, 0.0))
                d0s.append(float(np.nanmin(depth)) + s)
                d1s.append(float(np.nanmax(depth)) + s)
        if not d0s:
            return
        d0, d1 = min(d0s), max(d1s)
        if d1 <= d0:
            return
        id_to_i = {p.well_document_id: i for i, p in enumerate(presentations)}

        def y_of(d_disp: float) -> float:
            return top + ((d_disp - d0) / (d1 - d0)) * (bottom - top)

        for link in links:
            li = id_to_i.get(link.left_well_id)
            ri = id_to_i.get(link.right_well_id)
            if li is None or ri is None:
                continue
            ld = float(link.left_depth) + float(
                shifts.get(link.left_well_id, 0.0)
            )
            rd = float(link.right_depth) + float(
                shifts.get(link.right_well_id, 0.0)
            )
            x_l = rect.x() + li * (col_w + gap) + col_w - 4
            x_r = rect.x() + ri * (col_w + gap) + 4
            painter.setPen(QPen(QColor(link.color or "#c0392b"), 1.2))
            painter.drawLine(
                int(x_l), int(y_of(ld)), int(x_r), int(y_of(rd))
            )

    def export_active_correlation(
        self, path: Path | str, fmt: ExportFormat
    ) -> Path:
        """Export active correlation plot to SVG/PDF/PNG (B0 #300).

        Backend: **Qt paint** for SVG/PDF (host multi-column + links);
        **PNG** via correlation canvas grab when possible.
        """
        if self._workspace is None or not self._active_plot_id:
            raise ExportError("请先打开地层对比图")
        if len(self._correlation_presentations) < 2:
            raise ExportError("对比图至少需要 2 口井")
        plot = load_plot_document(self._workspace, self._active_plot_id)
        if plot.type != "correlation":
            raise ExportError("当前图件不是对比图")
        out = Path(path)
        if fmt == "png":
            return self._export_correlation_png(out)
        return export_plot(
            plot,
            fmt,
            path=str(out),
            page_spec=PageSpec(),
            backend="qt",
            paint_fn=self._paint_active_plot,
        )

    def _export_correlation_png(self, path: Path) -> Path:
        """Raster export of the correlation canvas (links + datum included)."""
        from PySide6.QtGui import QImage

        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        canvas = self.correlation_canvas
        # Ensure visible size for grab
        if canvas.width() < 100 or canvas.height() < 100:
            canvas.resize(960, 720)
        img = canvas.grab().toImage()
        if img.isNull() or img.width() < 2:
            # Fallback: paint into pixmap via export_plot path
            plot = load_plot_document(self._workspace, self._active_plot_id)  # type: ignore[arg-type]
            return export_plot(
                plot,
                "png",
                path=str(path),
                page_spec=PageSpec(),
                paint_fn=self._paint_active_plot,
            )
        if not img.save(str(path), "PNG"):
            raise ExportError(f"无法保存 PNG: {path}")
        if not path.is_file() or path.stat().st_size < 50:
            raise ExportError("PNG 导出为空")
        return path

    def _on_export_svg(self) -> None:
        self._export_active_plot("svg")

    def _on_export_pdf(self) -> None:
        self._export_active_plot("pdf")

    def _on_export_png(self) -> None:
        self._export_active_plot("png")

    def _on_export_cgm(self) -> None:
        """Single-well CGM export (B1.CGM.2/3 / ADR 0054)."""
        if self._workspace is None or not self._active_plot_id:
            QMessageBox.information(self, "导出 CGM", "请先打开/创建单井图件。")
            return
        plot = load_plot_document(self._workspace, self._active_plot_id)
        if plot.type != "single_well":
            QMessageBox.information(
                self, "导出 CGM", "CGM 目前仅支持单井分析图（引擎路径）。"
            )
            return
        QMessageBox.information(self, "导出 CGM", CGM_EXPORT_DISCLOSURE)
        # Continuous (default) vs multi-page (B1.CGM.3 fixed page height).
        page_choice = QMessageBox.question(
            self,
            "CGM 分页",
            "选择导出方式：\n\n"
            "• 是：连续单 PICTURE（整井深度一段）\n"
            "• 否：多页分页（固定页高，多 PICTURE）\n"
            "• 取消：中止导出",
            QMessageBox.StandardButton.Yes
            | QMessageBox.StandardButton.No
            | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Yes,
        )
        if page_choice == QMessageBox.StandardButton.Cancel:
            return
        page_height_mm: float | None = None
        if page_choice == QMessageBox.StandardButton.No:
            # Proxy page height from presentation depth span (half span, min 1 mm),
            # matching print-preview skeleton; fallback 50 mm if no presentation.
            if self._presentation is not None:
                d0, d1 = depth_range_from_presentation(self._presentation)
                vr = self.multi_track_canvas.depth_range()
                if vr is not None:
                    d0, d1 = vr
                depth_span = max(float(d1) - float(d0), 0.0)
                page_height_mm = max(depth_span / 2.0, 1.0)
            else:
                page_height_mm = 50.0
        path, _ = QFileDialog.getSaveFileName(
            self,
            "导出 CGM",
            f"{plot.name or 'plot'}.cgm",
            "CGM (*.cgm);;All (*.*)",
        )
        if not path:
            return
        try:
            if not engine_available():
                raise ExportError("WellLogEngine 不可用，无法导出 CGM")
            view, doc_id = self._prepare_engine_export_view()
            kwargs: dict[str, Any] = {
                "backend": "engine",
                "view": view,
                "document_id": doc_id,
                "path": path,
            }
            if page_height_mm is not None:
                kwargs["page_height_mm"] = page_height_mm
            out = export_plot(plot, "cgm", **kwargs)
            mode_note = (
                f"多页分页（page_height_mm={page_height_mm:.1f}）"
                if page_height_mm is not None
                else "连续单 PICTURE"
            )
            audit_command(
                "export.cgm",
                ok=True,
                target=str(self._active_plot_id or ""),
                detail=mode_note,
            )
            QMessageBox.information(
                self,
                "导出成功",
                f"CGM 已写入（引擎 · {mode_note}）:\n{out}\n"
                f"大小 {out.stat().st_size} 字节\n\n{CGM_EXPORT_DISCLOSURE}",
            )
        except (ExportError, UnsupportedFormatError, EngineUnavailable, EngineSubmitError) as exc:
            audit_command(
                "export.cgm",
                ok=False,
                target=str(getattr(self, "_active_plot_id", "") or ""),
                detail=str(exc)[:200],
            )
            QMessageBox.warning(self, "导出 CGM 失败", str(exc))
        except OSError as exc:
            audit_command(
                "export.cgm",
                ok=False,
                target=str(getattr(self, "_active_plot_id", "") or ""),
                detail=str(exc)[:200],
            )
            QMessageBox.warning(self, "导出 CGM 失败", str(exc))

    def open_print_preview(self, *, show: bool = True):
        """Open print-preview skeleton for active single-well or correlation (T13).

        Returns PrintPreviewInfo for tests, or None if nothing to preview.
        ``show=False`` builds info (and optionally dialog) without modal exec.
        """
        if self._presentation is not None and self._presentation.track_count > 0:
            d0, d1 = depth_range_from_presentation(self._presentation)
            vr = self.multi_track_canvas.depth_range()
            if vr is not None:
                d0, d1 = vr
            span = max(d1 - d0, 1e-9)
            # Skeleton multi-page plan: two pages by default for long spans
            page_spec = PageSpec(depth_per_page_mm=max(span / 2.0, 1.0))
            info = compute_print_preview(
                plot_name=(
                    self._presentation.well_name
                    or self._presentation.template_name
                    or "单井图"
                ),
                depth_top=d0,
                depth_bottom=d1,
                depth_unit=self._presentation.depth_unit or "m",
                page_spec=page_spec,
            )
            if show:
                dlg = PrintPreviewDialog(
                    info,
                    paint_fn=self._paint_active_plot,
                    page_spec=page_spec,
                    parent=self,
                )
                dlg.exec()
            return info

        if len(self._correlation_presentations) >= 2:
            d0s: list[float] = []
            d1s: list[float] = []
            unit = "m"
            for pres in self._correlation_presentations:
                a, b = depth_range_from_presentation(pres)
                d0s.append(a)
                d1s.append(b)
                unit = pres.depth_unit or unit
            page_spec = PageSpec()
            info = compute_print_preview(
                plot_name="地层对比图",
                depth_top=min(d0s),
                depth_bottom=max(d1s),
                depth_unit=unit,
                page_spec=page_spec,
            )
            if show:
                dlg = PrintPreviewDialog(
                    info,
                    paint_fn=self._paint_active_plot,
                    page_spec=page_spec,
                    parent=self,
                )
                dlg.exec()
            return info
        return None

    def _on_print_preview(self) -> None:
        info = self.open_print_preview()
        if info is None:
            QMessageBox.information(
                self,
                "打印预览",
                "请先打开单井分析图或地层对比图。",
            )

    def _on_import_tops(self) -> None:
        if self._selected_well_id is None or self._workspace is None:
            QMessageBox.information(self, "导入层位", "请先选择一口井。")
            return
        path, _ = QFileDialog.getOpenFileName(
            self,
            "选择层位 JSON",
            "",
            "JSON (*.json);;All (*.*)",
        )
        if not path:
            return
        try:
            tops = self.import_tops_json_for_well(self._selected_well_id, path)
            QMessageBox.information(
                self,
                "层位已导入",
                f"已关联 {len(tops)} 个层位到选中井。\n"
                f"单井/对比图会以虚线标记深度。",
            )
        except (TopsError, WorkspaceError) as exc:
            QMessageBox.warning(self, "导入层位失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "导入层位失败", str(exc))

    def _on_stub_tops(self) -> None:
        if self._selected_well_id is None or self._workspace is None:
            QMessageBox.information(self, "示例层位", "请先选择一口井。")
            return
        try:
            tops = self.generate_stub_tops_for_well(self._selected_well_id)
            QMessageBox.information(
                self,
                "示例层位",
                f"已生成 {len(tops)} 个示例层位（深度均分）。\n"
                f"正式数据请用「导入层位 JSON…」。",
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "生成层位失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "生成层位失败", str(exc))

    def _on_auto_horizon_links(self) -> None:
        try:
            links = self.auto_link_correlation_tops()
            QMessageBox.information(
                self,
                "自动连线",
                f"已按层位名生成 {len(links)} 条相邻井连线。\n"
                f"主机画布已绘制；引擎路径将作为 horizon_line 提交。",
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "自动连线失败", str(exc))

    def _on_clear_horizon_links(self) -> None:
        if not self._correlation_links:
            return
        self.clear_correlation_links()
        self.statusBar().showMessage("已清除全部对比连线", 4000)

    def _on_remove_selected_link(self) -> None:
        item = self.links_list.currentItem()
        if item is None:
            QMessageBox.information(self, "删除连线", "请先在右栏选中一条连线。")
            return
        link_id = item.data(Qt.ItemDataRole.UserRole)
        if not link_id:
            return
        if self.remove_correlation_link(str(link_id)):
            self.statusBar().showMessage("已删除选中连线", 4000)

    def _on_toggle_pick_links(self) -> None:
        enabled = self._act_pick_links.isChecked()
        if enabled and len(self._correlation_presentations) < 2:
            self._act_pick_links.setChecked(False)
            QMessageBox.information(self, "点选连线", "请先打开 ≥2 井对比图。")
            return
        self._link_pick_first = None
        self.correlation_canvas.set_pick_highlight(None, None)
        self.correlation_canvas.set_link_pick_mode(enabled)
        # Prefer host stack for hit testing
        if enabled:
            self.correlation_stack.setCurrentIndex(0)
            self.document_tabs.setCurrentIndex(1)
            self.statusBar().showMessage(
                "点选连线：依次点击两口井的层位虚线 · 再次点菜单退出",
                8000,
            )

    def _on_correlation_top_clicked(
        self, well_id: str, top_name: str, depth: float, top_id: str
    ) -> None:
        top = FormationTop(
            name=top_name,
            depth=depth,
            id=top_id or str(uuid.uuid4()),
        )
        if self._link_pick_first is None:
            self._link_pick_first = (well_id, top)
            self.correlation_canvas.set_pick_highlight(well_id, top_name)
            self.statusBar().showMessage(
                f"已选 {top_name}@{depth:g} · 请点击另一口井的层位", 6000
            )
            return
        first_well, first_top = self._link_pick_first
        self._link_pick_first = None
        self.correlation_canvas.set_pick_highlight(None, None)
        if first_well == well_id:
            # Restart with this pick as first
            self._link_pick_first = (well_id, top)
            self.correlation_canvas.set_pick_highlight(well_id, top_name)
            self.statusBar().showMessage(
                "请选择不同井上的层位 · 已将当前点设为起点", 5000
            )
            return
        try:
            link = self.create_horizon_link(first_well, first_top, well_id, top)
            self.statusBar().showMessage(
                f"已连线 {link.name}: {first_top.name} ↔ {top.name}", 5000
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "连线失败", str(exc))

    def _on_toggle_prefer_engine(self) -> None:
        self._prefer_engine_canvas = self._act_prefer_engine.isChecked()
        if self._active_plot_type == "correlation":
            self._sync_primary_correlation_surface()
            kind = "对比"
        else:
            self._sync_primary_single_well_surface()
            kind = "单井"
        self._update_status()
        mode = "引擎" if self._primary_surface == "engine" else "主机"
        self.statusBar().showMessage(f"{kind}画布: {mode}", 4000)

    def _on_toggle_pick_tops(self, checked: bool = False) -> None:
        # Prefer checked state from action
        enabled = self._act_pick_tops.isChecked()
        if enabled and (
            self._presentation is None or self._selected_well_id is None
        ):
            self._act_pick_tops.setChecked(False)
            QMessageBox.information(
                self, "拾取层位", "请先应用图版到选中井，再开启拾取。"
            )
            return
        self.multi_track_canvas.set_pick_mode(enabled)
        # Pick needs host canvas; switch stack to host while picking
        self._sync_primary_single_well_surface()
        if enabled:
            self.document_tabs.setCurrentIndex(0)
            self.single_well_stack.setCurrentIndex(0)
            self.statusBar().showMessage(
                "拾取层位：主机画布单击；Shift+单击也可 · 关闭菜单项退出",
                8000,
            )

    def _on_canvas_top_pick(self, depth: float) -> None:
        if self._selected_well_id is None or self._workspace is None:
            return
        if (
            self._presentation is None
            or self._presentation.well_document_id != self._selected_well_id
        ):
            return
        name, ok = QInputDialog.getText(
            self,
            "新建层位",
            f"深度 {depth:.3f} 的层位名称：",
        )
        if not ok:
            return
        try:
            top = self.add_top_at_depth(self._selected_well_id, name, depth)
            self.statusBar().showMessage(
                f"已添加层位 {top.name} @ {top.depth:.3f}", 5000
            )
        except (TopsError, WorkspaceError) as exc:
            QMessageBox.warning(self, "添加层位失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "添加层位失败", str(exc))

    def _selected_top_key(self) -> str | None:
        item = self.tops_list.currentItem()
        if item is None:
            return None
        key = item.data(Qt.ItemDataRole.UserRole)
        return str(key) if key else None

    def _on_undo_tops(self) -> None:
        if not self.undo_tops_edit():
            self.statusBar().showMessage("无可撤销的层位编辑", 3000)
            return
        self.statusBar().showMessage("已撤销层位编辑", 3000)

    def _on_redo_tops(self) -> None:
        if not self.redo_tops_edit():
            self.statusBar().showMessage("无可重做的层位编辑", 3000)
            return
        self.statusBar().showMessage("已重做层位编辑", 3000)

    def _on_remove_top(self) -> None:
        if self._selected_well_id is None:
            return
        key = self._selected_top_key()
        if not key:
            QMessageBox.information(self, "删除层位", "请先在层位列表中选择一项。")
            return
        try:
            if not self.remove_top_by_id(self._selected_well_id, key):
                QMessageBox.information(self, "删除层位", "未找到选中层位。")
                return
            self.statusBar().showMessage("已删除层位", 3000)
        except (TopsError, WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "删除层位失败", str(exc))

    def _on_edit_top_depth(self) -> None:
        if self._selected_well_id is None:
            return
        key = self._selected_top_key()
        if not key:
            QMessageBox.information(self, "修改深度", "请先在层位列表中选择一项。")
            return
        current = next(
            (t for t in self._active_tops if (t.id or t.name) == key),
            None,
        )
        default = float(current.depth) if current is not None else 0.0
        depth, ok = QInputDialog.getDouble(
            self,
            "修改层位深度",
            "新深度：",
            default,
            -1e7,
            1e7,
            3,
        )
        if not ok:
            return
        try:
            top = self.set_top_depth(self._selected_well_id, key, depth)
            self.statusBar().showMessage(
                f"已更新 {top.name} → {top.depth:.3f}", 4000
            )
        except (TopsError, WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "修改深度失败", str(exc))

    def _on_add_top_by_depth(self) -> None:
        if self._selected_well_id is None or self._presentation is None:
            QMessageBox.information(self, "添加层位", "请先选择井并应用图版。")
            return
        depth, ok = QInputDialog.getDouble(
            self,
            "按深度添加层位",
            "深度：",
            0.0,
            -1e9,
            1e9,
            3,
        )
        if not ok:
            return
        name, ok2 = QInputDialog.getText(self, "新建层位", "层位名称：")
        if not ok2:
            return
        try:
            top = self.add_top_at_depth(self._selected_well_id, name, depth)
            QMessageBox.information(
                self,
                "层位已添加",
                f"{top.display_label()}\n已写入 wells/…/tops.json",
            )
        except (TopsError, WorkspaceError) as exc:
            QMessageBox.warning(self, "添加层位失败", str(exc))
        except OSError as exc:
            QMessageBox.warning(self, "添加层位失败", str(exc))

    def _on_engine_preview(self) -> None:
        if self._presentation is None:
            QMessageBox.information(
                self, "引擎预览", "请先应用图版或打开单井分析图。"
            )
            return
        try:
            report = self.open_engine_preview()
            prepared = report.get("render_prepared")
            QMessageBox.information(
                self,
                "引擎预览",
                "已提交多图道 presentation 到 WellLogView（#225）。\n"
                f"render_prepared={prepared} · "
                f"tracks={report.get('track_count')} · "
                f"curves={report.get('curve_count')}\n"
                "主机多图道画布仍为默认路径；无引擎时自动降级。",
            )
        except EngineUnavailable as exc:
            QMessageBox.warning(self, "引擎不可用", str(exc))
        except EngineSubmitError as exc:
            QMessageBox.warning(self, "引擎提交失败", str(exc))

    def _on_engine_correlation_preview(self) -> None:
        if len(self._correlation_presentations) < 2:
            QMessageBox.information(
                self, "引擎对比预览", "请先新建/打开地层对比图（≥2 井）。"
            )
            return
        try:
            report = self.open_engine_correlation_preview()
            QMessageBox.information(
                self,
                "引擎对比预览",
                "已提交 multi-well section（共享深度）。\n"
                f"well_count={report.get('well_count')} · "
                f"render_prepared={report.get('render_prepared')}",
            )
        except EngineUnavailable as exc:
            QMessageBox.warning(self, "引擎不可用", str(exc))
        except EngineSubmitError as exc:
            QMessageBox.warning(self, "引擎提交失败", str(exc))
