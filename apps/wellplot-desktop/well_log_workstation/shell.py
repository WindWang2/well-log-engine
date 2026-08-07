"""Main window chrome for WellPlot Desktop — L layout (#216–#222, brand #290)."""

from __future__ import annotations

import math
import uuid
import zipfile
from pathlib import Path
from typing import AbstractSet, Any, Iterable
from xml.etree import ElementTree as ET

import numpy as np
from PySide6.QtCore import QPoint, QRectF, Qt, QTimer
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
    QMenu,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QSplitter,
    QStackedWidget,
    QStatusBar,
    QTableView,
    QTabWidget,
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
    FluidContact2D,
    SectionFault2D,
    TieQuad2D,
    contacts_from_json,
    faults_from_json,
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
    apply_track_order,
    apply_track_overrides,
    get_builtin_template,
    list_builtin_templates,
    track_order_from_presentation,
    track_overrides_snapshot,
)
from well_log_workstation.three_d_bridge import probe_3d
from well_log_workstation.tops_history import TopsHistoryBook
from well_log_workstation.tops_model import (
    FormationTop,
    TopsError,
    import_tops_from_json_file,
    load_formulas_for_well,
    load_survey_for_well,
    load_tops_for_well,
    make_stub_tops,
    save_formulas_for_well,
    save_survey_for_well,
    save_tops_for_well,
)
from well_log_workstation.nav_tree import NavTreeWidget
from well_log_workstation.recent_workspaces import add_recent, remove_recent
from well_log_workstation.workspace import (
    Workspace,
    WorkspaceError,
    create_workspace,
    ensure_startup_workspace,
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
        self._act_alias_dict = file_menu.addAction("测井别名字典…")
        self._act_alias_dict.setObjectName("Action_MnemonicAliasDict")
        self._act_alias_dict.triggered.connect(self._on_open_alias_dialog)
        self._act_alias_dict.setEnabled(False)
        self._act_survey = file_menu.addAction("编辑测斜数据…")
        self._act_survey.setObjectName("Action_EditSurvey")
        self._act_survey.triggered.connect(self._on_edit_survey)
        self._act_survey.setEnabled(False)
        self._act_formula = file_menu.addAction("公式计算器…")
        self._act_formula.setObjectName("Action_FormulaCalc")
        self._act_formula.triggered.connect(self._on_formula_calculator)
        self._act_formula.setEnabled(False)
        self._act_curve_edit = file_menu.addAction("曲线编辑（去毛刺/基线平移）…")
        self._act_curve_edit.setObjectName("Action_CurveEdit")
        self._act_curve_edit.triggered.connect(self._on_curve_edit)
        self._act_curve_edit.setEnabled(False)
        self._act_draw_curve = file_menu.addAction("手绘曲线")
        self._act_draw_curve.setObjectName("Action_DrawCurve")
        self._act_draw_curve.setCheckable(True)
        self._act_draw_curve.triggered.connect(self._on_toggle_draw_curve)
        self._act_draw_curve.setEnabled(False)
        self._act_litho = file_menu.addAction("岩性道编辑…")
        self._act_litho.setObjectName("Action_EditLithology")
        self._act_litho.triggered.connect(self._on_edit_lithology)
        self._act_litho.setEnabled(False)
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
        # P2-B (FRS §4.2): section line → buffer pick → correlation plot.
        self._act_section_from_line = plot_menu.addAction("平面画线生成剖面…")
        self._act_section_from_line.setObjectName("Action_NewSectionFromLine")
        self._act_section_from_line.triggered.connect(self._on_section_from_line)
        self._act_section_from_line.setEnabled(False)
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
        self._act_depth_shift = tops_menu.addAction("深度校正（拖拽层位）")
        self._act_depth_shift.setObjectName("Action_DepthShift")
        self._act_depth_shift.setCheckable(True)
        self._act_depth_shift.triggered.connect(self._on_toggle_depth_shift)
        self._act_depth_shift.setEnabled(False)
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
        # Main L-shell only — no startup workspace chooser.
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

        # Keep a single-page stack for API stability with older tests.
        self._main_stack = QStackedWidget()
        self._main_stack.setObjectName("MainStack")
        self._main_stack.addWidget(shell_root)
        self.setCentralWidget(self._main_stack)
        self._main_stack.setCurrentIndex(0)

    def _build_left(self) -> QWidget:
        """Single left tree: 数据 (wells→sources→tracks) + 图件.

        No dual tabs, no workspace root node — only the two product concepts.
        """
        pane = QWidget()
        pane.setObjectName("LeftPane")
        layout = QVBoxLayout(pane)
        layout.setContentsMargins(4, 4, 4, 4)

        self.left_title = QLabel("数据与图件")
        self.left_title.setObjectName("LeftPaneTitle")
        layout.addWidget(self.left_title)

        self.well_content_hint = QLabel(
            "数据→图件可拖放；右键菜单 · 双击井道加入当前图"
        )
        self.well_content_hint.setObjectName("WellContentHint")
        self.well_content_hint.setWordWrap(True)
        layout.addWidget(self.well_content_hint)

        self.workspace_tree = NavTreeWidget()
        self.workspace_tree.setObjectName("WorkspaceTree")
        # Alias: content checks live on the same tree (no second tab/tree).
        self.well_content_tree = self.workspace_tree
        self.workspace_tree.setHeaderLabels(["名称"])
        self.workspace_tree.setRootIsDecorated(True)
        self.workspace_tree.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu
        )
        self.workspace_tree.customContextMenuRequested.connect(
            self._on_tree_context_menu
        )
        self.workspace_tree.nav_drop.connect(self._on_nav_drop)
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
        # Interactive depth-shift (FRS §2.x 交互深度校正): commit dragged
        # top depths as undoable tops edits.
        self.multi_track_canvas.depth_shift_committed.connect(
            self._on_canvas_depth_shift_committed
        )
        # Freehand curve drawing (FRS §2.x 手绘曲线): merge the stroke into
        # the well's curve_edits.json and reapply.
        self.multi_track_canvas.curve_drawn_committed.connect(
            self._on_curve_drawn_committed
        )
        # Track-header drag reorder (FRS §2.x): persist the new order.
        self.multi_track_canvas.track_order_changed.connect(
            lambda _order: self._persist_track_overrides()
        )
        # Track-header width drag (FRS §2.x): width_fraction persists via the
        # same track_overrides path.
        self.multi_track_canvas.track_width_changed.connect(
            lambda _track_id, _frac: self._persist_track_overrides()
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
        # Section fault editor (FRS §3.3 / P1-A): edit the plot's faults.
        self.section_fault_btn = QPushButton("编辑断层…")
        self.section_fault_btn.setObjectName("Button_EditSectionFaults")
        self.section_fault_btn.setToolTip(
            "为当前油藏剖面添加/编辑断层（位置 + 落差，正断>0 下盘降 / 逆断<0 下盘升）。"
        )
        self.section_fault_btn.setEnabled(False)
        self.section_fault_btn.clicked.connect(self._on_edit_section_faults)
        sl.addWidget(self.section_fault_btn)
        # Section fluid-contact editor (FRS §3.3 / P1-B): OWC/GOC per-well.
        self.section_contact_btn = QPushButton("编辑流体界面…")
        self.section_contact_btn.setObjectName("Button_EditSectionContacts")
        self.section_contact_btn.setToolTip(
            "为当前油藏剖面添加/编辑油水/气油界面（OWC/GOC，各井深度）。"
            "界面切割充填多边形：上部油/气、下部水。"
        )
        self.section_contact_btn.setEnabled(False)
        self.section_contact_btn.clicked.connect(self._on_edit_section_contacts)
        sl.addWidget(self.section_contact_btn)
        # Section erosion/onlap surface editor (FRS §3.x / P1): per-well
        # unconformity depth; erosion keeps below, onlap keeps above.
        self.section_surface_btn = QPushButton("编辑剥蚀/超覆面…")
        self.section_surface_btn.setObjectName("Button_EditSectionSurfaces")
        self.section_surface_btn.setToolTip(
            "为当前油藏剖面添加/编辑不整合/剥蚀面（各井深度 + 模式）。"
            "剥蚀保留面下部、超覆保留面上部；先于断层/界面作用于充填多边形。"
        )
        self.section_surface_btn.setEnabled(False)
        self.section_surface_btn.clicked.connect(self._on_edit_section_surfaces)
        sl.addWidget(self.section_surface_btn)
        # Freehand sand lenses (FRS §3.x 透镜体手绘).
        lens_row = QHBoxLayout()
        self.section_lens_draw_btn = QPushButton("绘制透镜体")
        self.section_lens_draw_btn.setObjectName("Button_DrawSectionLens")
        self.section_lens_draw_btn.setCheckable(True)
        self.section_lens_draw_btn.setEnabled(False)
        self.section_lens_draw_btn.setToolTip(
            "手绘砂体透镜体：左键加点，双击或 Enter 闭合，右键/Esc 取消。"
        )
        self.section_lens_draw_btn.toggled.connect(self._on_draw_section_lens_toggled)
        lens_row.addWidget(self.section_lens_draw_btn)
        self.section_lens_edit_btn = QPushButton("编辑透镜体…")
        self.section_lens_edit_btn.setObjectName("Button_EditSectionLenses")
        self.section_lens_edit_btn.setEnabled(False)
        self.section_lens_edit_btn.setToolTip(
            "列表编辑已有透镜体顶点，或添加椭圆示例。"
        )
        self.section_lens_edit_btn.clicked.connect(self._on_edit_section_lenses)
        lens_row.addWidget(self.section_lens_edit_btn)
        sl.addLayout(lens_row)
        # Freehand lens snap / smooth toggles (FRS §3.x 磁吸 / 手绘平滑).
        # Snap is interaction-only; smooth also seeds newly drawn lenses with
        # smooth=True so they round on every paint (reversible per-lens).
        lens_opt_row = QHBoxLayout()
        self.section_lens_snap_check = QCheckBox("吸附分层")
        self.section_lens_snap_check.setObjectName("SectionLensSnapTops")
        self.section_lens_snap_check.setEnabled(False)
        self.section_lens_snap_check.setToolTip(
            "手绘透镜体时，光标靠近井列分层（10px 内）自动吸附到该分层深度。"
        )
        self.section_lens_snap_check.toggled.connect(
            self._on_section_lens_snap_toggled
        )
        lens_opt_row.addWidget(self.section_lens_snap_check)
        self.section_lens_smooth_check = QCheckBox("平滑边缘")
        self.section_lens_smooth_check.setObjectName("SectionLensSmooth")
        self.section_lens_smooth_check.setEnabled(False)
        self.section_lens_smooth_check.setToolTip(
            "手绘透镜体边缘 Chaikin 圆角平滑（绘制预览 + 新透镜体默认平滑）。"
        )
        self.section_lens_smooth_check.toggled.connect(
            self._on_section_lens_smooth_toggled
        )
        lens_opt_row.addWidget(self.section_lens_smooth_check)
        sl.addLayout(lens_opt_row)
        # Section well spacing (FRS §3.1 / P1-C): equal vs geographic (survey).
        spacing_row = QHBoxLayout()
        spacing_row.addWidget(QLabel("井距模式"))
        self.section_spacing_combo = QComboBox()
        self.section_spacing_combo.setObjectName("SectionWellSpacing")
        self.section_spacing_combo.addItem("等井距", "equal")
        self.section_spacing_combo.addItem("地理井距（测斜闭合位移）", "geographic")
        self.section_spacing_combo.setEnabled(False)
        self.section_spacing_combo.currentIndexChanged.connect(
            self._on_section_spacing_changed
        )
        spacing_row.addWidget(self.section_spacing_combo, 1)
        sl.addLayout(spacing_row)
        # Publication ornaments (FRS §5 / P2-C): legend/location/title block.
        self.section_ornament_check = QCheckBox("出版整饰（图例/接合图/责任表）")
        self.section_ornament_check.setObjectName("SectionOrnaments")
        self.section_ornament_check.setEnabled(False)
        self.section_ornament_check.toggled.connect(
            self._on_section_ornaments_toggled
        )
        sl.addWidget(self.section_ornament_check)
        self.section_canvas = SectionCanvas()
        self.section_canvas.lens_completed.connect(self._on_section_lens_completed)
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
        # Curve version toggle (FRS §1.x 多版本曲线): session-level display
        # preference — "corrected" shows the green edited-* tracks on top of
        # the template curves, "original" hides them. Not persisted (Desktop
        # session scope per FRS).
        self.curve_version_combo = QComboBox()
        self.curve_version_combo.setObjectName("CurveVersionCombo")
        self.curve_version_combo.addItem("校正（含编辑曲线）", "corrected")
        self.curve_version_combo.addItem("原始（不含编辑）", "original")
        self.curve_version_combo.setCurrentIndex(0)
        self.curve_version_combo.currentIndexChanged.connect(
            self._on_curve_version_changed
        )
        layout.addWidget(self.curve_version_combo)
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
        self.track_scale_wrap = QCheckBox("超量程折叠（折回而非裁顶）")
        self.track_scale_wrap.setObjectName("TrackScaleWrap")
        self.track_scale_wrap.toggled.connect(self._on_track_props_changed)
        form.addRow("", self.track_scale_wrap)
        # Baseline fill (FRS §2.x 基线充填, e.g. GR>80).
        self.track_fill_enable = QCheckBox("基线充填（曲线到右缘）")
        self.track_fill_enable.setObjectName("TrackFillEnable")
        self.track_fill_enable.toggled.connect(self._on_track_props_changed)
        form.addRow("", self.track_fill_enable)
        self.track_fill_threshold = QDoubleSpinBox()
        self.track_fill_threshold.setObjectName("TrackFillThreshold")
        self.track_fill_threshold.setRange(-1e9, 1e9)
        self.track_fill_threshold.setDecimals(2)
        self.track_fill_threshold.setValue(80.0)
        self.track_fill_threshold.valueChanged.connect(self._on_track_props_changed)
        form.addRow("充填阈值", self.track_fill_threshold)
        self.track_fill_direction = QComboBox()
        self.track_fill_direction.setObjectName("TrackFillDirection")
        self.track_fill_direction.addItem("超过阈值充填", "above")
        self.track_fill_direction.addItem("低于阈值充填", "below")
        self.track_fill_direction.currentIndexChanged.connect(
            self._on_track_props_changed
        )
        form.addRow("充填方向", self.track_fill_direction)
        # Crossover fill (FRS §2.x 双曲线交叉充填): dual-curve track region.
        self.track_crossover_fill = QCheckBox("双曲线交叉充填")
        self.track_crossover_fill.setObjectName("TrackCrossoverFill")
        self.track_crossover_fill.toggled.connect(self._on_track_props_changed)
        form.addRow("", self.track_crossover_fill)
        layout.addWidget(props)
        self._track_props_guard = False
        self._set_track_props_enabled(False)
        # Session-level curve version preference (FRS §1.x 多版本曲线).
        self._show_curve_edits = True
        self._curve_version_guard = False

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
        self.corr_mirror_btn = QPushButton("镜像翻转")
        self.corr_mirror_btn.setObjectName("Button_CorrMirror")
        self.corr_mirror_btn.clicked.connect(self._on_correlation_mirror)
        corr_order_row.addWidget(self.corr_well_up_btn)
        corr_order_row.addWidget(self.corr_well_down_btn)
        corr_order_row.addWidget(self.corr_mirror_btn)
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
        # Real well distance + vertical exaggeration (FRS §3.x).
        spacing_row = QHBoxLayout()
        spacing_row.addWidget(QLabel("井距模式"))
        self.corr_spacing_combo = QComboBox()
        self.corr_spacing_combo.setObjectName("CorrelationWellSpacing")
        self.corr_spacing_combo.addItem("等井距", "equal")
        self.corr_spacing_combo.addItem("实际井距（井口经纬度）", "real")
        self.corr_spacing_combo.setEnabled(False)
        self.corr_spacing_combo.setToolTip(
            "实际井距：按井口经纬度真实地面距离比例摆放井列（缺坐标的井降级为等距）。"
        )
        self.corr_spacing_combo.currentIndexChanged.connect(
            self._on_correlation_spacing_changed
        )
        spacing_row.addWidget(self.corr_spacing_combo, 1)
        layout.addLayout(spacing_row)
        ve_row = QHBoxLayout()
        ve_row.addWidget(QLabel("纵向放大 VE"))
        self.corr_ve_spin = QDoubleSpinBox()
        self.corr_ve_spin.setObjectName("CorrelationVE")
        self.corr_ve_spin.setRange(0.1, 20.0)
        self.corr_ve_spin.setSingleStep(0.1)
        self.corr_ve_spin.setDecimals(2)
        self.corr_ve_spin.setValue(1.0)
        self.corr_ve_spin.setSuffix(" ×")
        self.corr_ve_spin.setEnabled(False)
        self.corr_ve_spin.setToolTip(
            "纵向放大倍数：单独拉伸深度轴显示（1.0 = 不变，与滚轮缩放正交）。"
        )
        self.corr_ve_spin.valueChanged.connect(self._on_correlation_ve_changed)
        ve_row.addWidget(self.corr_ve_spin)
        layout.addLayout(ve_row)
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
        # Pinchout wedges (FRS §3.3): unilateral intervals wedge out toward
        # the neighbour column instead of being dropped from the fill.
        self.corr_pinch_check = QCheckBox("尖灭楔形")
        self.corr_pinch_check.setObjectName("CorrelationPinchout")
        self.corr_pinch_check.setToolTip(
            "单井存在的层位区间向缺失侧收拢为楔形（厚度归零外推）。"
        )
        self.corr_pinch_check.toggled.connect(self._on_correlation_pinch_toggled)
        layout.addWidget(self.corr_pinch_check)
        pinch_row = QHBoxLayout()
        pinch_row.addWidget(QLabel("尖灭位置"))
        self.corr_pinch_factor = QDoubleSpinBox()
        self.corr_pinch_factor.setObjectName("CorrelationPinchoutFactor")
        self.corr_pinch_factor.setRange(0.05, 1.0)
        self.corr_pinch_factor.setSingleStep(0.05)
        self.corr_pinch_factor.setValue(0.5)
        self.corr_pinch_factor.setSuffix("（0=邻井 1=本井）")
        self.corr_pinch_factor.valueChanged.connect(self._on_correlation_pinch_factor_changed)
        pinch_row.addWidget(self.corr_pinch_factor)
        layout.addLayout(pinch_row)
        self.corr_pinch_smooth = QCheckBox("平滑边缘")
        self.corr_pinch_smooth.setObjectName("CorrelationPinchoutSmooth")
        self.corr_pinch_smooth.toggled.connect(self._on_correlation_pinch_smooth_toggled)
        layout.addWidget(self.corr_pinch_smooth)
        # Lithology pattern (SY/T 5615) assignment for the selected horizon.
        from well_log_workstation.litho_pattern_lib import load_builtin_patterns

        self._litho_patterns = load_builtin_patterns()
        layout.addWidget(QLabel("层位岩性花纹"))
        litho_row = QHBoxLayout()
        self.corr_litho_combo = QComboBox()
        self.corr_litho_combo.setObjectName("CorrelationLithoPattern")
        self.corr_litho_combo.addItem("（纯色充填）", "")
        for pid, pat in sorted(self._litho_patterns.items(), key=lambda kv: kv[1].name):
            self.corr_litho_combo.addItem(f"{pat.name} ({pat.name_en})", pid)
        self.corr_litho_combo.currentIndexChanged.connect(self._on_correlation_litho_changed)
        litho_row.addWidget(self.corr_litho_combo, 1)
        layout.addLayout(litho_row)
        self.corr_litho_target = QLineEdit()
        self.corr_litho_target.setObjectName("CorrelationLithoTarget")
        self.corr_litho_target.setPlaceholderText("目标层位名，如 T1")
        layout.addWidget(self.corr_litho_target)
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
        self._act_depth_shift.setEnabled(can_pick)
        self._act_draw_curve.setEnabled(can_pick)
        if not can_pick and self._act_pick_tops.isChecked():
            self._act_pick_tops.setChecked(False)
            self.multi_track_canvas.set_pick_mode(False)
        if not can_pick and self._act_depth_shift.isChecked():
            self._act_depth_shift.setChecked(False)
            self.multi_track_canvas.set_shift_mode(False)
        if not can_pick and self._act_draw_curve.isChecked():
            self._act_draw_curve.setChecked(False)
            self.multi_track_canvas.set_draw_curve_mode(False)

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
        # Interactive depth-shift drag also requires host canvas hit-testing.
        if self.multi_track_canvas.shift_mode():
            want_engine = False
        # Freehand curve drawing likewise requires host canvas hit-testing.
        if self.multi_track_canvas.draw_curve_mode():
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
        # Activate the workspace mnemonic alias dictionary so template/display
        # matching picks up alias curves (FRS §1.2 / P0-A).
        from well_log_workstation.mnemonic_alias import set_active_map

        set_active_map(ws.mnemonic_alias if ws is not None else None)
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
        self._act_alias_dict.setEnabled(ws is not None)
        self._act_survey.setEnabled(ws is not None)
        self._act_formula.setEnabled(ws is not None)
        self._act_curve_edit.setEnabled(ws is not None)
        self._act_draw_curve.setEnabled(ws is not None)
        self._act_litho.setEnabled(ws is not None)
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
        # P2-B: section-from-line needs ≥2 wells with coordinates.
        coord_wells = sum(
            1 for w in (ws.wells if ws is not None else [])
            if w.lng is not None and w.lat is not None
        )
        self._act_section_from_line.setEnabled(
            ws is not None and coord_wells >= 2
        )
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
        else:
            self.setWindowTitle(window_title())
        if hasattr(self, "_main_stack"):
            self._main_stack.setCurrentIndex(0)

    def open_default_session(self) -> Workspace:
        """Cold-start: open last/default storage and stay on main shell."""
        ws = ensure_startup_workspace()
        self.set_workspace(ws)
        return ws

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
        if hasattr(self, "track_fill_enable"):
            self.track_fill_enable.setEnabled(enabled)
            self.track_fill_threshold.setEnabled(False)
            self.track_fill_direction.setEnabled(False)
            self.track_crossover_fill.setEnabled(False)

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
        # Sync the curve-version combo to the session preference (guarded).
        if hasattr(self, "curve_version_combo"):
            self._curve_version_guard = True
            try:
                want = "corrected" if self._show_curve_edits else "original"
                idx = self.curve_version_combo.findData(want)
                self.curve_version_combo.setCurrentIndex(idx if idx >= 0 else 0)
            finally:
                self._curve_version_guard = False
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
                self.track_scale_wrap.setChecked(False)
                self.track_fill_enable.setChecked(False)
                self.track_crossover_fill.setChecked(False)
                self._set_track_props_enabled(False)
                return
            self.track_visible.setChecked(bool(track.visible))
            has_scale = track.scale is not None and track.role == "curve"
            self.track_scale_min.setEnabled(has_scale)
            self.track_scale_max.setEnabled(has_scale)
            self.track_scale_mode.setEnabled(has_scale)
            self.track_scale_wrap.setEnabled(has_scale)
            self.track_fill_enable.setEnabled(has_scale)
            fill_on = has_scale and bool(getattr(track.scale, "fill_threshold", None) is not None)
            self.track_fill_threshold.setEnabled(fill_on)
            self.track_fill_direction.setEnabled(fill_on)
            crossover_ok = has_scale and len(track.layers) >= 2
            self.track_crossover_fill.setEnabled(crossover_ok)
            self.track_visible.setEnabled(True)
            if track.scale is not None:
                self.track_scale_min.setValue(float(track.scale.min))
                self.track_scale_max.setValue(float(track.scale.max))
                mode = track.scale.mode
                idx = self.track_scale_mode.findData(mode)
                self.track_scale_mode.setCurrentIndex(idx if idx >= 0 else 0)
                self.track_scale_wrap.setChecked(bool(track.scale.wrap))
                ft = getattr(track.scale, "fill_threshold", None)
                self.track_fill_enable.setChecked(ft is not None)
                if ft is not None:
                    self.track_fill_threshold.setValue(float(ft))
                fd = str(getattr(track.scale, "fill_direction", "above"))
                fidx = self.track_fill_direction.findData(fd)
                self.track_fill_direction.setCurrentIndex(fidx if fidx >= 0 else 0)
                self.track_crossover_fill.setChecked(
                    bool(getattr(track.scale, "crossover_fill", False))
                )
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
            track.scale.wrap = self.track_scale_wrap.isChecked()
            if self.track_fill_enable.isChecked():
                track.scale.fill_threshold = float(self.track_fill_threshold.value())
                fd = self.track_fill_direction.currentData() or "above"
                track.scale.fill_direction = (
                    fd if fd in ("above", "below") else "above"
                )
                # Keep the threshold/direction enabled while fill is on.
                self.track_fill_threshold.setEnabled(True)
                self.track_fill_direction.setEnabled(True)
            else:
                track.scale.fill_threshold = None
                self.track_fill_threshold.setEnabled(False)
                self.track_fill_direction.setEnabled(False)
            track.scale.crossover_fill = self.track_crossover_fill.isChecked()
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
        # Canvas drag reorder (FRS §2.x): persist the id order too.
        plot.track_order = track_order_from_presentation(self._presentation)
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
        # Attach per-well lithology segments (FRS §2.x) so template binding
        # and canvas rendering always see the latest data.
        from well_log_workstation.lithology_model import load_lithology_for_well

        doc.lithology = load_lithology_for_well(self._workspace, well_id)[0]
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
                    # User / prior explicit display_set wins (may exceed default cap).
                    if plot_doc.display_set:
                        loaded = frozenset(str(x) for x in plot_doc.display_set)
                except WorkspaceError:
                    loaded = None
            if loaded is not None:
                self._display_sets[key] = loaded
            else:
                # First open / empty plot: batch of preferred tracks, ≤10.
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
                apply_track_order(presentation, plot_doc.track_order)
            except WorkspaceError:
                pass
        self._selected_well_id = well_id
        self._presentation = presentation
        self._active_plot_type = "single_well"
        if plot_id is not None:
            self._active_plot_id = plot_id
        self.multi_track_canvas.set_presentation(presentation)
        # Attach derived curves from the well's formulas (FRS §2.4 / P2-A).
        self._apply_derived_curves()
        # Attach non-destructive curve edits (FRS §2.x despike/baseline)
        # honoring the session curve version preference (FRS §1.x).
        self._apply_curve_edits(show_edited=self._show_curve_edits)
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
        self.correlation_canvas.set_pinchout(
            str(getattr(plot, "pinchout_mode", "off") or "off"),
            float(getattr(plot, "pinchout_factor", 0.5)),
            bool(getattr(plot, "pinchout_smooth", False)),
        )
        self.correlation_canvas.set_fill_pattern_map(
            dict(getattr(plot, "litho_pattern_map", None) or {})
        )
        # Real well distance + vertical exaggeration (FRS §3.x).
        corr_spacing = str(getattr(plot, "correlation_spacing", None) or "equal")
        if corr_spacing not in ("equal", "real"):
            corr_spacing = "equal"
        well_x_offsets: list[float] | None = None
        if corr_spacing == "real" and self._workspace is not None:
            from well_log_workstation.well_spacing import wellhead_offsets

            positions: list[tuple[float | None, float | None]] = []
            for wid in plot.well_ids:
                entry = next(
                    (w for w in self._workspace.wells if w.id == wid), None
                )
                positions.append(
                    (
                        float(entry.lng) if entry and entry.lng is not None else None,
                        float(entry.lat) if entry and entry.lat is not None else None,
                    )
                )
            offsets, _spacing_m = wellhead_offsets(positions)
            well_x_offsets = offsets
        self.correlation_canvas.set_well_x_offsets(well_x_offsets)
        ve = float(getattr(plot, "vertical_exaggeration", 1.0) or 1.0)
        self.correlation_canvas.set_vertical_exaggeration(ve)
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

    def _section_trajectory_data(
        self,
        plot: PlotDocument,
        presentations: list[HostPresentation],
        well_positions: list[tuple[float, float]],
        shifts: dict[str, float],
        surveys: dict[str, list],
    ) -> tuple[list[float], list[np.ndarray | None]]:
        """Compute geographic-spacing well offsets + trajectory polylines.

        Offsets (well-index units) are the projected closure of each well's
        survey on the section azimuth, normalised by the average inter-well
        spacing; trajectory polylines are ``[offset_units, md + shift]``.

        Wells without a survey contribute a 0 offset and no trajectory line.
        """
        from well_log_workstation.section_geometry import (
            section_trajectory_polyline,
        )
        from well_log_workstation.survey import compute_trajectory

        # Section azimuth: bearing of the first→last wellhead; fall back to
        # 0 (N–S) when coordinates are missing.
        azimuth_deg = 0.0
        if len(well_positions) >= 2:
            dx = well_positions[-1][0] - well_positions[0][0]
            dy = well_positions[-1][1] - well_positions[0][1]
            if dx or dy:
                azimuth_deg = math.degrees(math.atan2(dx, dy))
        # Average inter-well spacing (metres-ish in the lng/lat plane); when
        # unavailable use the max survey closure so offsets stay reasonable.
        dists = [
            math.hypot(
                well_positions[i + 1][0] - well_positions[i][0],
                well_positions[i + 1][1] - well_positions[i][1],
            )
            for i in range(len(well_positions) - 1)
        ]
        spacing_m = sum(dists) / len(dists) if dists else 0.0

        offsets: list[float] = []
        trajectories: list[np.ndarray | None] = []
        max_closure = 0.0
        for well_id, pres in zip(plot.well_ids, presentations, strict=False):
            stations = surveys.get(pres.well_name) or []
            if not stations:
                offsets.append(0.0)
                trajectories.append(None)
                continue
            traj = compute_trajectory(stations)
            closure = float(traj.closure_dist[-1]) if traj.closure_dist.size else 0.0
            max_closure = max(max_closure, closure)
        # Second pass once the max closure is known (for degenerate spacing).
        for well_id, pres in zip(plot.well_ids, presentations, strict=False):
            stations = surveys.get(pres.well_name) or []
            if not stations:
                continue
            traj = compute_trajectory(stations)
            eff_spacing = spacing_m or (max_closure or 1.0)
            pl = section_trajectory_polyline(
                traj, azimuth_deg, eff_spacing, shift=shifts.get(pres.well_name, 0.0)
            )
            trajectories.append(pl)
            offsets.append(float(pl[-1, 0]) if pl.shape[0] else 0.0)
        # Pad any well that had no survey (already appended in first pass).
        while len(offsets) < len(presentations):
            offsets.append(0.0)
            trajectories.append(None)
        return offsets, trajectories

    def _collect_section_ornaments(
        self,
        plot: PlotDocument,
        well_positions: list[tuple[float, float]],
    ) -> Any:
        """Build the ornament layer data for a section (FRS §5 / P2-C).

        Legend items: lithology patterns (from litho_pattern_map), faults
        (red dashed) and fluid contacts (OWC blue / GOC orange). Location map:
        every well with coordinates, highlighting the section's wells. Title
        block: plot name / workspace / scale text.
        """
        from PySide6.QtGui import QBrush, QColor

        from well_log_workstation.litho_pattern_lib import get_pattern, make_qbrush
        from well_log_workstation.ornament import (
            OrnamentData,
            title_block_fields,
        )

        legend: list[tuple[Any, str]] = []
        lpm = getattr(plot, "litho_pattern_map", None) or {}
        for name, pid in lpm.items():
            pat = get_pattern(pid)
            if pat is not None:
                legend.append((make_qbrush(pat, "#cbd5e1"), name))
            else:
                legend.append(("#cbd5e1", name))
        faults = faults_from_json(getattr(plot, "faults", None) or [])
        if faults:
            legend.append(("#dc2626", "断层"))
        contacts = contacts_from_json(getattr(plot, "contacts", None) or [])
        for c in contacts:
            fluid = str(c.fluid_type)
            legend.append(
                ("#2563eb" if fluid == "owc" else "#f59e0b",
                 "油水界面" if fluid == "owc" else "气油界面")
            )

        # Location map points: wells with coordinates; highlight = section wells.
        points: list[tuple[float, float]] = []
        highlight: list[int] = []
        section_ids = set(plot.well_ids)
        for w in self._workspace.wells:
            if w.lng is None or w.lat is None:
                continue
            idx = len(points)
            points.append((float(w.lng), float(w.lat)))
            if w.id in section_ids:
                highlight.append(idx)

        scale_text = "1:500（示意）" if not well_positions else "1:500"
        fields = title_block_fields(
            plot.name,
            self._workspace.name,
            scale_text=scale_text,
        )
        return OrnamentData(
            title_fields=fields,
            legend_items=legend,
            map_points=points,
            map_highlight=highlight,
            scale_text=scale_text,
        )

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

        # Datum shifts (T5 / P1-C): md / tvd / tvdss / horizon flatten.
        datum_mode = str(getattr(plot, "datum_mode", None) or "md")
        if datum_mode not in ("md", "tvd", "tvdss", "horizon"):
            datum_mode = "md"
        target_horizon = getattr(plot, "datum_horizon", None)
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
        surveys: dict[str, list] = {}
        if datum_mode == "tvd":
            for well_id, pres in zip(plot.well_ids, presentations, strict=False):
                stations, _diags = load_survey_for_well(self._workspace, well_id)
                if stations:
                    surveys[pres.well_name] = stations
        shifts = datum.compute_shifts(well_dicts, surveys=surveys or None)

        # Well trajectory display (P1-C / FRS §3.1): geographic spacing places
        # each well by its survey closure projected on the section azimuth,
        # plus a curved trajectory polyline per deviated well.
        well_spacing = str(getattr(plot, "well_spacing", None) or "equal")
        if well_spacing not in ("equal", "geographic"):
            well_spacing = "equal"
        well_x_offsets: list[float] | None = None
        well_trajectories: list[np.ndarray | None] | None = None
        if well_spacing == "geographic":
            well_x_offsets, well_trajectories = self._section_trajectory_data(
                plot, presentations, well_positions, shifts, surveys
            )
        self.section_canvas.set_well_x_offsets(well_x_offsets)
        self.section_canvas.set_well_trajectories(well_trajectories)

        # Section geometry (T4): faults / contacts / surfaces / tie quads.
        faults: list[SectionFault2D] = faults_from_json(
            getattr(plot, "faults", None) or []
        )
        contacts: list[FluidContact2D] = contacts_from_json(
            getattr(plot, "contacts", None) or []
        )
        from well_log_workstation.section_geometry.erosion_surface import (
            surfaces_from_json,
        )

        surfaces = surfaces_from_json(getattr(plot, "surfaces", None) or [])
        from well_log_workstation.section_geometry import lenses_from_json

        lenses = lenses_from_json(getattr(plot, "lenses", None) or [])
        quads: list[TieQuad2D] = []
        tops_as_dicts = [
            [{"name": ft.name, "depth": ft.depth} for ft in col]
            for col in tops_cols
        ]
        quads = tie_quads(tops_as_dicts, well_positions, datum_shifts=shifts)
        # Attach lithology patterns (SY/T 5615) by matching the quad's top
        # names against the plot's litho_pattern_map (name → pattern id).
        lpm = getattr(plot, "litho_pattern_map", None) or {}
        if lpm:
            from dataclasses import replace

            updated: list[TieQuad2D] = []
            for q in quads:
                pid = ""
                for name in str(q.label).split(","):
                    name = name.strip()
                    if name and name in lpm:
                        pid = lpm[name]
                        break
                updated.append(replace(q, pattern_id=pid or q.pattern_id))
            quads = updated

        self._active_plot_id = plot.id
        self._active_plot_type = "section"
        self.section_canvas.set_section(
            presentations,
            tops_cols,
            faults=faults,
            contacts=contacts,
            surfaces=surfaces,
            lenses=lenses,
            tie_quads=quads,
        )
        names = " · ".join(p.well_name for p in presentations[:4])
        self.section_caption.setText(
            f"油藏剖面 · {names} · 基准 {datum_mode} · "
            f"断层 {len(faults)} · 接触 {len(contacts)} · "
            f"剥蚀/超覆 {len(surfaces)} · 透镜体 {len(lenses)} · 充填 {len(quads)}"
        )
        self.section_fault_btn.setEnabled(True)
        self.section_contact_btn.setEnabled(True)
        self.section_surface_btn.setEnabled(True)
        self.section_lens_draw_btn.setEnabled(True)
        self.section_lens_edit_btn.setEnabled(True)
        self.section_lens_snap_check.setEnabled(True)
        self.section_lens_smooth_check.setEnabled(True)
        # Sync the spacing combo from the plot doc (guarded against re-entry).
        self._section_spacing_guard = True
        try:
            want = str(getattr(plot, "well_spacing", None) or "equal")
            idx = self.section_spacing_combo.findData(want)
            self.section_spacing_combo.setCurrentIndex(idx if idx >= 0 else 0)
        finally:
            self._section_spacing_guard = False
        self.section_spacing_combo.setEnabled(True)
        # Publication ornaments (FRS §5 / P2-C): collect data + sync toggle.
        self.section_canvas.set_ornament_data(
            self._collect_section_ornaments(plot, well_positions)
        )
        self.section_ornament_check.setChecked(
            bool(getattr(plot, "ornaments", False))
        )
        self.section_ornament_check.setEnabled(True)
        self.section_canvas.set_show_ornaments(
            bool(getattr(plot, "ornaments", False))
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
        self.corr_mirror_btn.setEnabled(enabled)
        self.corr_gap_spin.setEnabled(enabled)
        if hasattr(self, "corr_spacing_combo"):
            self.corr_spacing_combo.setEnabled(enabled)
            self.corr_ve_spin.setEnabled(enabled)
        if hasattr(self, "corr_datum_mode"):
            self.corr_datum_mode.setEnabled(enabled)
            self.corr_datum_horizon.setEnabled(enabled)
            self.corr_undo_btn.setEnabled(
                enabled and bool(self._corr_layout_undo)
            )
        if hasattr(self, "corr_fill_check"):
            self.corr_fill_check.setEnabled(enabled)
            self.corr_refresh_btn.setEnabled(enabled)
        if hasattr(self, "corr_pinch_check"):
            # Pinchout only renders when inter-well fill is on.
            fill_on = self.corr_fill_check.isChecked() if enabled else False
            self.corr_pinch_check.setEnabled(enabled and fill_on)
            self.corr_pinch_factor.setEnabled(enabled and fill_on)
            self.corr_pinch_smooth.setEnabled(enabled and fill_on)
        if hasattr(self, "corr_litho_combo"):
            fill_on = self.corr_fill_check.isChecked() if enabled else False
            self.corr_litho_combo.setEnabled(enabled and fill_on)
            self.corr_litho_target.setEnabled(enabled and fill_on)

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
            if hasattr(self, "corr_spacing_combo"):
                sp = str(getattr(plot, "correlation_spacing", None) or "equal")
                sidx = self.corr_spacing_combo.findData(sp)
                self.corr_spacing_combo.setCurrentIndex(sidx if sidx >= 0 else 0)
                self.corr_ve_spin.setValue(
                    float(getattr(plot, "vertical_exaggeration", 1.0) or 1.0)
                )
            if hasattr(self, "corr_fill_check"):
                self.corr_fill_check.setChecked(
                    bool(getattr(plot, "show_interwell_fill", False))
                )
            if hasattr(self, "corr_pinch_check"):
                self.corr_pinch_check.setChecked(
                    str(getattr(plot, "pinchout_mode", "off") or "off") == "linear"
                )
                self.corr_pinch_factor.setValue(
                    float(getattr(plot, "pinchout_factor", 0.5))
                )
                self.corr_pinch_smooth.setChecked(
                    bool(getattr(plot, "pinchout_smooth", False))
                )
            if hasattr(self, "corr_litho_combo"):
                # Combo reflects the pattern for whichever horizon name is in
                # the target field; default to first entry when unset.
                target = self.corr_litho_target.text().strip()
                lpm = dict(getattr(plot, "litho_pattern_map", None) or {})
                want_pid = lpm.get(target, "")
                idx = self.corr_litho_combo.findData(want_pid)
                self.corr_litho_combo.setCurrentIndex(idx if idx >= 0 else 0)
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

    def _on_correlation_mirror(self) -> None:
        """Mirror (reverse) the well-column order on the active correlation."""
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
        if len(plot.well_ids) < 2:
            return
        self._push_correlation_layout_undo(plot)
        plot.well_ids = list(reversed(plot.well_ids))
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存井序失败", str(exc))
            return
        self._show_correlation(plot)
        self.corr_well_list.setCurrentRow(0)
        self.statusBar().showMessage("已镜像翻转井序", 4000)

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

    def _on_correlation_spacing_changed(self) -> None:
        """Toggle real vs equal well-distance spacing for the correlation."""
        if self._corr_layout_guard:
            return
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
        ):
            return
        spacing = self.corr_spacing_combo.currentData() or "equal"
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        self._push_correlation_layout_undo(plot)
        plot.correlation_spacing = str(spacing)
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            return
        self._show_correlation(plot)
        self.statusBar().showMessage(
            f"井距模式：{'实际井距' if spacing == 'real' else '等井距'}", 4000
        )

    def _on_correlation_ve_changed(self, value: float) -> None:
        """Set vertical exaggeration for the correlation depth axis."""
        if self._corr_layout_guard:
            return
        if (
            self._workspace is None
            or self._active_plot_type != "correlation"
            or not self._active_plot_id
        ):
            return
        ve = max(0.1, min(20.0, float(value)))
        self.correlation_canvas.set_vertical_exaggeration(ve)
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        self._push_correlation_layout_undo(plot)
        plot.vertical_exaggeration = ve
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            return
        self._set_correlation_layout_enabled(True)

    def _correlation_layout_snapshot(self, plot: PlotDocument) -> dict[str, Any]:
        return {
            "links": [lk.to_json() for lk in plot.links],
            "column_gap_px": int(getattr(plot, "column_gap_px", 6) or 6),
            "datum_mode": str(getattr(plot, "datum_mode", None) or "md"),
            "datum_horizon": getattr(plot, "datum_horizon", None),
            "well_ids": list(plot.well_ids),
            "correlation_spacing": str(
                getattr(plot, "correlation_spacing", None) or "equal"
            ),
            "vertical_exaggeration": float(
                getattr(plot, "vertical_exaggeration", 1.0) or 1.0
            ),
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
        plot.correlation_spacing = str(snap.get("correlation_spacing") or "equal")
        try:
            plot.vertical_exaggeration = float(
                snap.get("vertical_exaggeration", 1.0) or 1.0
            )
        except (TypeError, ValueError):
            plot.vertical_exaggeration = 1.0
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
                self.correlation_canvas.set_pinchout(
                    str(getattr(plot, "pinchout_mode", "off") or "off"),
                    float(getattr(plot, "pinchout_factor", 0.5)),
                    bool(getattr(plot, "pinchout_smooth", False)),
                )
                self.correlation_canvas.set_fill_pattern_map(
                    dict(getattr(plot, "litho_pattern_map", None) or {})
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
        # Pinchout wedges are a sub-mode of inter-well fill.
        self._set_correlation_layout_enabled(
            self._active_plot_type == "correlation"
            and bool(self._active_plot_id)
        )
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

    def _on_correlation_pinch_toggled(self, checked: bool = False) -> None:
        if self._corr_layout_guard:
            return
        mode = "linear" if self.corr_pinch_check.isChecked() else "off"
        self._apply_pinchout_to_canvas(mode)

    def _on_correlation_pinch_factor_changed(self, _value: float) -> None:
        if self._corr_layout_guard:
            return
        # The factor is only meaningful when wedges are on; mirror it to canvas
        # so the live preview updates even before persistence.
        mode = "linear" if self.corr_pinch_check.isChecked() else "off"
        self._apply_pinchout_to_canvas(mode)

    def _on_correlation_pinch_smooth_toggled(self, _checked: bool = False) -> None:
        if self._corr_layout_guard:
            return
        mode = "linear" if self.corr_pinch_check.isChecked() else "off"
        self._apply_pinchout_to_canvas(mode)

    def _apply_pinchout_to_canvas(self, mode: str) -> None:
        """Push current pinchout controls to the canvas and persist on the plot."""
        factor = float(self.corr_pinch_factor.value())
        smooth = self.corr_pinch_smooth.isChecked()
        self.correlation_canvas.set_pinchout(mode, factor, smooth)
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
        plot.pinchout_mode = mode
        plot.pinchout_factor = factor
        plot.pinchout_smooth = smooth
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            pass

    def _on_correlation_litho_changed(self, _idx: int = -1) -> None:
        """Assign a lithology pattern to the named horizon on the active plot."""
        if self._corr_layout_guard:
            return
        name = self.corr_litho_target.text().strip()
        pid = str(self.corr_litho_combo.currentData() or "")
        if not name or (
            self._active_plot_type != "correlation" or not self._active_plot_id
        ):
            return
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        lpm = dict(plot.litho_pattern_map)
        if pid:
            lpm[name] = pid
        else:
            lpm.pop(name, None)
        plot.litho_pattern_map = lpm
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            pass
        self.correlation_canvas.set_fill_pattern_map(lpm)

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
            kind = data.get("kind")
            # Data tree is data-unit (source) based; also match legacy well nodes.
            if kind == "well" and data.get("id") == well_id:
                return item
            if kind == "source" and str(data.get("well_id") or "") == well_id:
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
        # Selecting well / source / data leaf / plot track focuses that well
        if kind in ("well", "source", "leaf", "plot_track", "plot_well"):
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
        if not self._selected_well_id:
            raise WorkspaceError("请先选择数据井道所属井")
        return self.import_leaves_to_plot(
            [leaf_id],
            well_id=self._selected_well_id,
            plot_id=self._active_plot_id,
        )

    def import_leaves_to_plot(
        self,
        leaf_ids: Iterable[str],
        *,
        well_id: str,
        plot_id: str | None = None,
        open_plot: bool = True,
    ) -> HostPresentation:
        """Bind one or more data leaves onto a single-well plot (no sample copy)."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        well_id = str(well_id).strip()
        if not well_id:
            raise WorkspaceError("未指定井")
        ordered = [str(x).strip() for x in leaf_ids if str(x).strip()]
        if not ordered:
            raise WorkspaceError("未指定井道")
        pid = str(plot_id or self._active_plot_id or "").strip()
        if not pid:
            raise WorkspaceError("请先指定或打开一张单井分析图")
        try:
            pdoc = load_plot_document(self._workspace, pid)
        except WorkspaceError as exc:
            raise WorkspaceError(f"无法打开图件: {exc}") from exc
        if pdoc.type != "single_well":
            raise WorkspaceError("仅单井分析图支持拖入/绑定井道")
        if open_plot and self._active_plot_id != pid:
            self.open_plot_document(pid)
        key = self._display_set_key(well_id, plot_id=pid)
        current = set(self._display_sets.get(key) or frozenset())
        if not current and pdoc.display_set:
            current = {str(x) for x in pdoc.display_set}
        current.update(ordered)
        tid = pdoc.template_id or self._current_template_id() or "std-gr-rt-den"
        self._selected_well_id = well_id
        return self.set_display_set(
            well_id,
            current,
            template_id=tid,
            plot_id=pid,
        )

    def remove_leaves_from_plot(
        self,
        leaf_ids: Iterable[str],
        *,
        well_id: str,
        plot_id: str,
    ) -> HostPresentation:
        """Remove bound leaves from a single-well plot display set."""
        if self._workspace is None:
            raise WorkspaceError("请先打开工区")
        pid = str(plot_id).strip()
        well_id = str(well_id).strip()
        drop = {str(x).strip() for x in leaf_ids if str(x).strip()}
        pdoc = load_plot_document(self._workspace, pid)
        if pdoc.type != "single_well":
            raise WorkspaceError("仅单井分析图支持移出井道")
        key = self._display_set_key(well_id, plot_id=pid)
        current = set(self._display_sets.get(key) or frozenset())
        if not current:
            current = {str(x) for x in pdoc.display_set}
        current -= drop
        if self._active_plot_id != pid:
            self.open_plot_document(pid)
        tid = pdoc.template_id or self._current_template_id() or "std-gr-rt-den"
        return self.set_display_set(
            well_id, current, template_id=tid, plot_id=pid
        )

    def _on_nav_drop(self, payload: dict[str, Any], plot_id: str) -> None:
        """Handle data→plot drag/drop from NavTreeWidget."""
        try:
            well_id = str(payload.get("well_id") or "").strip()
            kind = payload.get("kind")
            if kind == "leaf":
                lids = [str(payload.get("leaf_id") or "")]
            elif kind == "data":
                lids = [str(x) for x in (payload.get("leaf_ids") or [])]
            else:
                return
            self.import_leaves_to_plot(lids, well_id=well_id, plot_id=plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "拖放到图件失败", str(exc))

    def _on_tree_context_menu(self, pos: QPoint) -> None:
        tree = self.workspace_tree
        item = tree.itemAt(pos)
        menu = QMenu(self)
        menu.setObjectName("NavTreeContextMenu")
        data = (item.data(0, Qt.ItemDataRole.UserRole) or {}) if item else {}
        kind = data.get("kind")

        def _add(label: str, slot, *, object_name: str = "") -> None:
            act = menu.addAction(label)
            if object_name:
                act.setObjectName(object_name)
            act.triggered.connect(slot)

        if kind in (None, "wells_folder"):
            _add(
                "导入 LAS…",
                self._on_import_las,
                object_name="Ctx_ImportLas",
            )
        elif kind == "source":
            well_id = str(data.get("well_id") or "")
            _add(
                "新建单井分析图…",
                lambda: self._ctx_new_plot_for_well(well_id),
                object_name="Ctx_NewPlotFromData",
            )
            _add(
                "全部井道加入当前井图",
                lambda: self._ctx_add_all_leaves_of_source(item),
                object_name="Ctx_AddAllLeavesToActive",
            )
            self._add_plot_target_submenu(
                menu, "全部井道加入到图件", item, mode="data"
            )
        elif kind == "leaf":
            leaf_id = str(data.get("id") or "")
            well_id = str(data.get("well_id") or "")
            _add(
                "加入当前井图",
                lambda: self._ctx_add_leaf(leaf_id, well_id, None),
                object_name="Ctx_AddLeafToActive",
            )
            self._add_plot_target_submenu(
                menu,
                "加入到图件",
                item,
                mode="leaf",
                leaf_id=leaf_id,
                well_id=well_id,
            )
            _add(
                "新建单井分析图并加入此井道",
                lambda: self._ctx_new_plot_with_leaf(leaf_id, well_id),
                object_name="Ctx_NewPlotWithLeaf",
            )
        elif kind == "plots_folder":
            _add(
                "新建单井分析图…",
                self._on_new_single_well_plot,
                object_name="Ctx_NewPlotFromFolder",
            )
        elif kind == "plot":
            plot_id = str(data.get("id") or "")
            _add(
                "打开",
                lambda: self._ctx_open_plot(plot_id),
                object_name="Ctx_OpenPlot",
            )
            _add(
                "导出井图定义 XML…",
                lambda: self._ctx_export_plot(plot_id, "xml"),
                object_name="Ctx_ExportPlotXml",
            )
            _add(
                "导出井图定义 Excel…",
                lambda: self._ctx_export_plot(plot_id, "xlsx"),
                object_name="Ctx_ExportPlotXlsx",
            )
        elif kind == "plot_track":
            leaf_id = str(data.get("id") or "")
            well_id = str(data.get("well_id") or "")
            plot_id = str(data.get("plot_id") or "")
            _add(
                "从本图移除",
                lambda: self._ctx_remove_plot_track(leaf_id, well_id, plot_id),
                object_name="Ctx_RemovePlotTrack",
            )
        else:
            return

        if menu.isEmpty():
            return
        menu.exec(tree.global_pos_for_menu(pos))

    def _add_plot_target_submenu(
        self,
        menu: QMenu,
        title: str,
        item: QTreeWidgetItem,
        *,
        mode: str,
        leaf_id: str = "",
        well_id: str = "",
    ) -> None:
        if self._workspace is None:
            return
        single = [p for p in self._workspace.plots if p.type == "single_well"]
        if not single:
            return
        sub = menu.addMenu(title)
        sub.setObjectName("Ctx_PlotTargetSubmenu")
        for plot in single:
            act = sub.addAction(plot.name)
            pid = plot.id
            if mode == "leaf":
                act.triggered.connect(
                    lambda _=False, l=leaf_id, w=well_id, p=pid: self._ctx_add_leaf(
                        l, w, p
                    )
                )
            else:
                act.triggered.connect(
                    lambda _=False, it=item, p=pid: self._ctx_add_all_leaves_of_source(
                        it, plot_id=p
                    )
                )

    def _ctx_add_leaf(
        self, leaf_id: str, well_id: str, plot_id: str | None
    ) -> None:
        try:
            self.import_leaves_to_plot(
                [leaf_id], well_id=well_id, plot_id=plot_id
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加入井图失败", str(exc))

    def _ctx_add_all_leaves_of_source(
        self, item: QTreeWidgetItem | None, *, plot_id: str | None = None
    ) -> None:
        if item is None:
            return
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        well_id = str(data.get("well_id") or "")
        leaf_ids: list[str] = []
        for i in range(item.childCount()):
            cdata = item.child(i).data(0, Qt.ItemDataRole.UserRole) or {}
            if cdata.get("kind") == "leaf":
                lid = str(cdata.get("id") or "")
                if lid:
                    leaf_ids.append(lid)
        try:
            self.import_leaves_to_plot(
                leaf_ids, well_id=well_id, plot_id=plot_id
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加入井图失败", str(exc))

    def _ctx_new_plot_for_well(self, well_id: str) -> None:
        if not well_id:
            QMessageBox.information(self, "新建单井分析图", "未指定井。")
            return
        self._selected_well_id = well_id
        self._on_new_single_well_plot()

    def _ctx_new_plot_with_leaf(self, leaf_id: str, well_id: str) -> None:
        if not well_id or not leaf_id:
            return
        self._selected_well_id = well_id
        template_id = self._current_template_id() or "std-gr-rt-den"
        try:
            plot = self.create_single_well_plot_document(well_id, template_id)
            self.import_leaves_to_plot(
                [leaf_id], well_id=well_id, plot_id=plot.id
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "新建图件失败", str(exc))

    def _ctx_open_plot(self, plot_id: str) -> None:
        try:
            self.open_plot_document(plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "打开图件失败", str(exc))

    def _ctx_export_plot(self, plot_id: str, fmt: str) -> None:
        if self._workspace is None or not plot_id:
            return
        try:
            self.open_plot_document(plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "导出失败", str(exc))
            return
        if fmt == "xml":
            self._on_export_plot_xml()
        else:
            self._on_export_plot_xlsx()

    def _ctx_remove_plot_track(
        self, leaf_id: str, well_id: str, plot_id: str
    ) -> None:
        try:
            self.remove_leaves_from_plot(
                [leaf_id], well_id=well_id, plot_id=plot_id
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "移出井道失败", str(exc))

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
            self.import_leaves_to_plot(
                [str(data.get("id") or "")],
                well_id=str(data.get("well_id") or self._selected_well_id or ""),
                plot_id=self._active_plot_id,
            )
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
        """Top-level: 数据 (inventory + refs) and 图件 (plot → tracks)."""
        tree = self.workspace_tree
        self._content_tree_guard = True
        try:
            tree.clear()
            self.left_title.setText("数据与图件")
            data_node = QTreeWidgetItem(["数据"])
            data_node.setData(0, Qt.ItemDataRole.UserRole, {"kind": "wells_folder"})
            plots_node = QTreeWidgetItem(["图件"])
            plots_node.setData(0, Qt.ItemDataRole.UserRole, {"kind": "plots_folder"})

            if self._workspace is None:
                self.well_content_hint.setText("导入 LAS 到井，或新建图件")
                empty_w = QTreeWidgetItem(["（无井 · 请导入 LAS）"])
                empty_w.setDisabled(True)
                data_node.addChild(empty_w)
                empty_p = QTreeWidgetItem(["（无图件 · 新建单井分析图）"])
                empty_p.setDisabled(True)
                plots_node.addChild(empty_p)
                tree.addTopLevelItem(data_node)
                tree.addTopLevelItem(plots_node)
                tree.expandAll()
                return

            ws = self._workspace
            leaf_refs = self._leaf_to_plot_names()
            total_leaves = 0
            # 以数据为单位：每条导入数据一条节点（井道列表无引用标注）
            for well in ws.wells:
                n = self._attach_data_unit(data_node, well, leaf_refs)
                total_leaves += n
            if not ws.wells:
                empty = QTreeWidgetItem(["（无数据 · 请导入 LAS）"])
                empty.setDisabled(True)
                data_node.addChild(empty)

            type_labels = {
                "single_well": "[单井·多图道]",
                "correlation": "[对比]",
                "section": "[剖面]",
                "plane_map": "[平面图]",
                "fence_3d": "[栅状图]",
                "composite": "[综合图]",
            }
            for plot in ws.plots:
                label = plot.name
                suffix = type_labels.get(plot.type, "")
                if suffix:
                    label = f"{plot.name} {suffix}"
                item = QTreeWidgetItem([label])
                item.setData(
                    0,
                    Qt.ItemDataRole.UserRole,
                    {"kind": "plot", "id": plot.id, "type": plot.type},
                )
                self._attach_plot_tracks_branch(item, plot)
                plots_node.addChild(item)
            if not ws.plots:
                empty = QTreeWidgetItem(["（无图件 · 新建单井分析图）"])
                empty.setDisabled(True)
                plots_node.addChild(empty)

            tree.addTopLevelItem(data_node)
            tree.addTopLevelItem(plots_node)
            tree.expandToDepth(3)
            if self._selected_well_id:
                self._select_well_in_tree(self._selected_well_id)
            plot_note = (
                f"当前井图 {self._active_plot_id[:8]}…"
                if self._active_plot_id and self._active_plot_type == "single_well"
                else "双击数据井道加入当前井图"
            )
            self.well_content_hint.setText(
                f"数据=数据单元（名后→标注引用图）· 图件=图→井道 · "
                f"{total_leaves} 井道 · {plot_note}"
            )
        finally:
            self._content_tree_guard = False

    def _leaf_to_plot_names(self) -> dict[str, list[str]]:
        """Map leaf_id → plot display names that reference it (ordered, unique)."""
        out: dict[str, list[str]] = {}
        if self._workspace is None:
            return out
        for plot in self._workspace.plots:
            try:
                pdoc = load_plot_document(self._workspace, plot.id)
            except WorkspaceError:
                continue
            leaf_ids: list[str] = []
            if pdoc.data_bindings:
                leaf_ids.extend(str(b.leaf_id) for b in pdoc.data_bindings if b.leaf_id)
            leaf_ids.extend(str(x) for x in pdoc.display_set if x)
            for lid in leaf_ids:
                bucket = out.setdefault(lid, [])
                if plot.name not in bucket:
                    bucket.append(plot.name)
        return out

    def _format_data_unit_label(self, base: str, plot_names: list[str]) -> str:
        """Data-unit name with optional reference marker (not on track leaves)."""
        if not plot_names:
            return base
        quoted = "、".join(f"「{n}」" for n in plot_names)
        return f"{base} → {quoted}"

    def _attach_data_unit(
        self,
        data_node: QTreeWidgetItem,
        well: Any,
        leaf_refs: dict[str, list[str]] | None = None,
    ) -> int:
        """One node per imported data unit; children = plain track list.

        Reference annotations go only on the data unit name (→ 「图名」),
        never on individual track leaves.
        """
        if self._workspace is None:
            return 0
        well_id = str(well.id)
        leaf_refs = leaf_refs if leaf_refs is not None else self._leaf_to_plot_names()
        try:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
        except Exception as exc:  # noqa: BLE001
            err = QTreeWidgetItem([f"{well.name}（数据未加载: {exc}）"])
            err.setDisabled(True)
            err.setData(
                0,
                Qt.ItemDataRole.UserRole,
                {"kind": "well", "id": well_id, "well_id": well_id},
            )
            data_node.addChild(err)
            return 0

        leaves = leaves_from_document(doc)
        source_name = Path(doc.source_path).name if doc.source_path else "导入源"
        # Data unit title: prefer file name; keep well name when different
        if well.name and well.name != source_name and source_name != "导入源":
            base = f"{source_name}（{well.name}）"
        else:
            base = source_name if source_name != "导入源" else (well.name or source_name)

        # Aggregate plot refs at data-unit level (any track from this data)
        plot_names: list[str] = []
        seen_plots: set[str] = set()
        for leaf in leaves:
            for pname in leaf_refs.get(leaf.id) or []:
                if pname not in seen_plots:
                    seen_plots.add(pname)
                    plot_names.append(pname)

        unit_label = self._format_data_unit_label(base, plot_names)
        unit_item = QTreeWidgetItem([unit_label])
        unit_item.setData(
            0,
            Qt.ItemDataRole.UserRole,
            {
                "kind": "source",
                "source_id": doc.source_path or doc.document_id,
                "well_id": well_id,
                "id": well_id,
            },
        )
        unit_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
        tip = (
            f"数据 · 井 {well.name} · {doc.depth_unit or 'm'} · "
            f"{int(doc.depth.size)} 样点 · {len(leaves)} 井道"
        )
        if plot_names:
            tip += " · 已被图件引用: " + "、".join(plot_names)
        unit_item.setToolTip(0, tip)

        for leaf in leaves:
            mnemo = leaf.label or leaf.mnemonic
            child = QTreeWidgetItem([mnemo])
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
            child.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            child.setToolTip(0, f"井道 {leaf.mnemonic} · 双击加入当前井图")
            unit_item.addChild(child)

        if not leaves:
            none = QTreeWidgetItem(["（无井道）"])
            none.setDisabled(True)
            unit_item.addChild(none)

        data_node.addChild(unit_item)
        return len(leaves)

    def _attach_plot_tracks_branch(
        self, plot_item: QTreeWidgetItem, plot_entry: Any
    ) -> None:
        """图件 subtree always ends at tracks: 图 → (井) → 井道."""
        if self._workspace is None:
            return
        try:
            pdoc = load_plot_document(self._workspace, plot_entry.id)
        except WorkspaceError as exc:
            err = QTreeWidgetItem([f"（无法加载: {exc}）"])
            err.setDisabled(True)
            plot_item.addChild(err)
            return

        well_name = {w.id: w.name for w in self._workspace.wells}

        if plot_entry.type == "single_well" or pdoc.display_set or pdoc.data_bindings:
            # Ordered leaf list: prefer data_bindings order, else display_set
            ordered: list[tuple[str, str, str]] = []  # leaf_id, well_id, mnemonic
            seen: set[str] = set()
            for b in pdoc.data_bindings:
                lid = str(b.leaf_id or "")
                if not lid or lid in seen:
                    continue
                seen.add(lid)
                ordered.append(
                    (lid, str(b.well_id or ""), str(b.mnemonic or ""))
                )
            for lid in pdoc.display_set:
                s = str(lid)
                if not s or s in seen:
                    continue
                seen.add(s)
                wid = ""
                if pdoc.well_ids:
                    wid = str(pdoc.well_ids[0])
                ordered.append((s, wid, ""))

            if not ordered and pdoc.well_ids:
                # No bindings yet: show wells as placeholders under the plot
                for wid in pdoc.well_ids:
                    witem = QTreeWidgetItem(
                        [well_name.get(wid, wid) + "（未绑定井道）"]
                    )
                    witem.setData(
                        0,
                        Qt.ItemDataRole.UserRole,
                        {"kind": "plot_well", "well_id": wid, "plot_id": plot_entry.id},
                    )
                    witem.setDisabled(True)
                    plot_item.addChild(witem)
                return

            if not ordered:
                empty = QTreeWidgetItem(["（无井道 · 从数据区双击加入）"])
                empty.setDisabled(True)
                plot_item.addChild(empty)
                return

            # Group by well when multiple wells appear
            wells_in_order: list[str] = []
            by_well: dict[str, list[tuple[str, str]]] = {}
            for lid, wid, mnemo in ordered:
                key = wid or (pdoc.well_ids[0] if pdoc.well_ids else "")
                if key not in by_well:
                    by_well[key] = []
                    wells_in_order.append(key)
                by_well[key].append((lid, mnemo))

            multi = len(wells_in_order) > 1

            def _resolve_mnemo(lid: str, mnemo: str, wid: str) -> str:
                if mnemo:
                    return mnemo
                if ":" in lid:
                    return lid.rsplit(":", 1)[-1]
                if wid and self._workspace is not None:
                    try:
                        doc = self.session.ensure_well_loaded(self._workspace, wid)
                        for leaf in leaves_from_document(doc):
                            if leaf.id == lid:
                                return leaf.mnemonic
                    except Exception:  # noqa: BLE001
                        pass
                return lid

            for wid in wells_in_order:
                parent = plot_item
                if multi:
                    wnode = QTreeWidgetItem([well_name.get(wid, wid or "井")])
                    wnode.setData(
                        0,
                        Qt.ItemDataRole.UserRole,
                        {
                            "kind": "plot_well",
                            "well_id": wid,
                            "plot_id": plot_entry.id,
                        },
                    )
                    plot_item.addChild(wnode)
                    parent = wnode
                for lid, mnemo in by_well[wid]:
                    label = _resolve_mnemo(lid, mnemo, wid)
                    child = QTreeWidgetItem([label])
                    child.setData(
                        0,
                        Qt.ItemDataRole.UserRole,
                        {
                            "kind": "plot_track",
                            "id": lid,
                            "mnemonic": label,
                            "well_id": wid,
                            "plot_id": plot_entry.id,
                        },
                    )
                    child.setFlags(
                        Qt.ItemFlag.ItemIsEnabled
                        | Qt.ItemFlag.ItemIsSelectable
                        | Qt.ItemFlag.ItemIsUserCheckable
                    )
                    child.setCheckState(0, Qt.CheckState.Checked)
                    child.setToolTip(0, f"图内井道 {label} · 取消勾选可移出本图")
                    parent.addChild(child)
            return

        # Other plot types: 图 → 井
        if pdoc.well_ids:
            for wid in pdoc.well_ids:
                witem = QTreeWidgetItem([well_name.get(wid, wid)])
                witem.setData(
                    0,
                    Qt.ItemDataRole.UserRole,
                    {
                        "kind": "plot_well",
                        "well_id": wid,
                        "plot_id": plot_entry.id,
                    },
                )
                plot_item.addChild(witem)
        else:
            empty = QTreeWidgetItem(["（未绑定井）"])
            empty.setDisabled(True)
            plot_item.addChild(empty)

    def _refresh_well_content_tree(self) -> None:
        """Compat: content is embedded in workspace_tree — full rebuild."""
        self._refresh_tree()

    def _collect_plot_tree_checked_leaves(self, plot_id: str) -> frozenset[str]:
        """Collect checked plot_track leaves under a plot node."""
        ids: set[str] = set()

        def walk(item: QTreeWidgetItem) -> None:
            data = item.data(0, Qt.ItemDataRole.UserRole) or {}
            if (
                data.get("kind") == "plot_track"
                and str(data.get("plot_id") or "") == plot_id
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
        """Checkbox toggles on 图件 → 井道 (remove/add within that plot)."""
        if self._content_tree_guard:
            return
        data = item.data(0, Qt.ItemDataRole.UserRole) or {}
        if data.get("kind") != "plot_track":
            return
        plot_id = str(data.get("plot_id") or "")
        well_id = str(data.get("well_id") or "")
        if not plot_id:
            return
        if well_id:
            self._selected_well_id = well_id
        # Defer apply so multi-toggle batches (parent won't batch on plot side)
        self._pending_plot_check_id = plot_id
        self._pending_plot_check_well = well_id
        if not self._content_apply_pending:
            self._content_apply_pending = True
            QTimer.singleShot(0, self._flush_plot_tree_checks)

    def _flush_plot_tree_checks(self) -> None:
        self._content_apply_pending = False
        if self._workspace is None:
            return
        plot_id = getattr(self, "_pending_plot_check_id", None) or self._active_plot_id
        well_id = getattr(self, "_pending_plot_check_well", None) or self._selected_well_id
        if not plot_id or not well_id:
            return
        checked = self._collect_plot_tree_checked_leaves(str(plot_id))
        try:
            # Keep session active plot aligned when editing that plot's tree
            if self._active_plot_id != plot_id:
                try:
                    self.open_plot_document(str(plot_id))
                except WorkspaceError:
                    pass
            self.set_display_set(
                str(well_id),
                checked,
                plot_id=str(plot_id),
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "更新图件井道失败", str(exc))
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

    def _on_open_alias_dialog(self) -> None:
        """Edit the workspace mnemonic alias dictionary (FRS §1.2 / P0-A)."""
        if self._workspace is None:
            return
        from well_log_workstation.mnemonic_alias import set_active_map
        from well_log_workstation.mnemonic_alias_dialog import MnemonicAliasDialog

        dlg = MnemonicAliasDialog(self._workspace.mnemonic_alias, self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_map = dlg.value()
        self._workspace.mnemonic_alias = new_map
        try:
            save_workspace(self._workspace)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存别名字典失败", str(exc))
            return
        set_active_map(new_map)
        self.statusBar().showMessage(
            f"已更新测井别名字典（{len(new_map)} 条规范名）", 4000
        )

    def _on_edit_survey(self) -> None:
        """Edit the deviation survey for a well (FRS §1.1 / P1-C)."""
        if self._workspace is None or not self._workspace.wells:
            QMessageBox.information(self, "测斜数据", "请先打开含井的工区。")
            return
        from well_log_workstation.survey_dialog import SurveyDialog

        well_ids = self._pick_wells_for_correlation()
        if not well_ids:
            return
        well_id = well_ids[0]
        entry = next((w for w in self._workspace.wells if w.id == well_id), None)
        if entry is None:
            return
        current, _diags = load_survey_for_well(self._workspace, well_id)
        dlg = SurveyDialog(entry.name, current, self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        stations = dlg.value()
        try:
            save_survey_for_well(self._workspace, well_id, stations)
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "保存测斜失败", str(exc))
            return
        self.statusBar().showMessage(
            f"已保存 {entry.name} 测斜（{len(stations)} 站）", 4000
        )
        # If a section using TVD datum is open, re-render with the new survey.
        if self._active_plot_type == "section" and self._active_plot_id:
            try:
                plot = load_plot_document(self._workspace, self._active_plot_id)
                self._show_section(plot)
            except WorkspaceError:
                pass

    def _on_edit_lithology(self) -> None:
        """Edit the selected well's lithology segments (FRS §2.x)."""
        if self._workspace is None or not self._workspace.wells:
            QMessageBox.information(self, "岩性道编辑", "请先打开含井的工区。")
            return
        from well_log_workstation.lithology_dialog import LithologyDialog
        from well_log_workstation.lithology_model import (
            load_lithology_for_well,
            save_lithology_for_well,
        )

        well_ids = self._pick_wells_for_correlation()
        if not well_ids:
            return
        well_id = well_ids[0]
        entry = next((w for w in self._workspace.wells if w.id == well_id), None)
        if entry is None:
            return
        doc = self.session.ensure_well_loaded(self._workspace, well_id)
        depth = np.asarray(doc.depth, dtype=np.float64)
        if depth.size >= 2:
            depth_range = (float(np.nanmin(depth)), float(np.nanmax(depth)))
        else:
            depth_range = None
        current, _diags = load_lithology_for_well(self._workspace, well_id)
        dlg = LithologyDialog(current, self, depth_range=depth_range)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        model = dlg.value()
        try:
            save_lithology_for_well(self._workspace, model)
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "保存岩性失败", str(exc))
            return
        self._selected_well_id = well_id
        # Re-apply the current template so the litho track shows new segments.
        if self._active_plot_type == "single_well" and self._presentation is not None:
            self.apply_template_to_well(well_id, self._presentation.template_id)
        else:
            self.multi_track_canvas.update()
        self.statusBar().showMessage(
            f"已保存 {entry.name} 岩性（{len(model.segments)} 段）", 4000
        )

    def _on_formula_calculator(self) -> None:
        """Edit derived-curve formulas for the selected well (FRS §2.4 / P2-A)."""
        if self._workspace is None or not self._workspace.wells:
            QMessageBox.information(self, "公式计算器", "请先打开含井的工区。")
            return
        from well_log_workstation.formula_dialog import FormulaDialog

        well_ids = self._pick_wells_for_correlation()
        if not well_ids:
            return
        well_id = well_ids[0]
        entry = next((w for w in self._workspace.wells if w.id == well_id), None)
        if entry is None:
            return
        current, _diags = load_formulas_for_well(self._workspace, well_id)
        dlg = FormulaDialog(current, self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        formulas = dlg.value()
        try:
            save_formulas_for_well(self._workspace, well_id, formulas)
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "保存公式失败", str(exc))
            return
        self._selected_well_id = well_id
        ok_n, diags = self._apply_derived_curves()
        note = f"已保存 {entry.name} 公式（{len(formulas)} 条）"
        if diags:
            note += f" · {len(diags)} 条求值失败"
        self.statusBar().showMessage(note, 4000)
        if diags:
            QMessageBox.warning(
                self, "公式求值提示",
                "\n".join(diags[:5]) + ("\n…" if len(diags) > 5 else ""),
            )

    def _on_curve_version_changed(self, index: int = 0) -> None:
        """Toggle the session curve version (original vs corrected)."""
        if self._curve_version_guard:
            return
        mode = self.curve_version_combo.itemData(index) or "corrected"
        self._show_curve_edits = mode == "corrected"
        _ok_n, diags = self._apply_curve_edits(
            show_edited=self._show_curve_edits
        )
        label = "校正（含编辑曲线）" if self._show_curve_edits else "原始（不含编辑）"
        self.statusBar().showMessage(f"曲线版本：{label}", 4000)
        if diags:
            QMessageBox.warning(
                self, "曲线编辑提示", "\n".join(diags[:5])
            )

    def _on_curve_edit(self) -> None:
        """Non-destructive curve edits (despike / baseline) for a well."""
        if self._workspace is None:
            return
        from well_log_workstation.curve_edit import (
            load_curve_edits_for_well,
            save_curve_edits_for_well,
        )
        from well_log_workstation.curve_edit_dialog import CurveEditDialog

        well_ids = self._pick_wells_for_correlation()
        if not well_ids:
            return
        well_id = well_ids[0]
        entry = next((w for w in self._workspace.wells if w.id == well_id), None)
        if entry is None:
            return
        try:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加载井数据失败", str(exc))
            return
        mnemonics = [c.mnemonic for c in doc.curves]
        current, _diags = load_curve_edits_for_well(self._workspace, well_id)
        dlg = CurveEditDialog(current, self, curve_mnemonics=mnemonics)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        edits = dlg.value()
        try:
            save_curve_edits_for_well(self._workspace, well_id, edits)
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "保存曲线编辑失败", str(exc))
            return
        self._selected_well_id = well_id
        # Show the edited version after saving so the user sees the result.
        self._show_curve_edits = True
        self._curve_version_guard = True
        try:
            idx = self.curve_version_combo.findData("corrected")
            self.curve_version_combo.setCurrentIndex(idx if idx >= 0 else 0)
        finally:
            self._curve_version_guard = False
        ok_n, diags = self._apply_curve_edits()
        note = f"已保存 {entry.name} 曲线编辑（{len(edits)} 条）"
        if diags:
            note += f" · {len(diags)} 条应用失败"
        self.statusBar().showMessage(note, 4000)
        if diags:
            QMessageBox.warning(
                self, "曲线编辑提示",
                "\n".join(diags[:5]) + ("\n…" if len(diags) > 5 else ""),
            )

    def _apply_curve_edits(
        self, *, show_edited: bool = True
    ) -> tuple[int, list[str]]:
        """(Re)attach edited curve tracks to the current single-well plot.

        Removes any previous ``edited-*`` tracks, applies the well's curve
        edits (despike / baseline / freehand) to the read-only source arrays,
        and appends a green ``edited-<mnemonic>`` track per edited curve —
        unless ``show_edited`` is False (original version: edits are still
        validated for diagnostics but not displayed). Returns
        ``(applied_count, diagnostics)``.
        """
        if self._presentation is None or self._workspace is None:
            return 0, []
        well_id = self._selected_well_id or ""
        if not well_id:
            return 0, []
        self._presentation.tracks = [
            t for t in self._presentation.tracks
            if not str(t.id).startswith("edited-")
        ]
        from well_log_workstation.curve_edit import (
            apply_curve_edits,
            load_curve_edits_for_well,
        )

        edits, _ = load_curve_edits_for_well(self._workspace, well_id)
        if not edits:
            self.multi_track_canvas.set_presentation(self._presentation)
            return 0, []
        if not show_edited:
            # Original version: keep diagnostics (missing curves etc.) but
            # don't attach the edited tracks.
            diags: list[str] = []
            try:
                doc = self.session.ensure_well_loaded(self._workspace, well_id)
            except Exception:  # noqa: BLE001
                return 0, ["井数据未加载"]
            seen: set[str] = set()
            for edit in edits:
                if edit.mnemonic in seen:
                    continue
                seen.add(edit.mnemonic)
                if doc.curve_by_mnemonic(edit.mnemonic) is None:
                    diags.append(f"{edit.mnemonic}: 井中无此曲线")
            self.multi_track_canvas.set_presentation(self._presentation)
            return 0, diags
        try:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
        except Exception:  # noqa: BLE001
            return 0, ["井数据未加载"]
        from well_log_workstation.template_model import (
            BoundCurveLayer,
            BoundTrack,
            ScaleSpec,
        )

        diags: list[str] = []
        applied = 0
        seen: set[str] = set()
        for edit in edits:
            mnemonic = edit.mnemonic
            if mnemonic in seen:
                continue
            seen.add(mnemonic)
            curve = doc.curve_by_mnemonic(mnemonic)
            if curve is None:
                diags.append(f"{mnemonic}: 井中无此曲线")
                continue
            values, null_mask = apply_curve_edits(
                doc.depth, curve.values, curve.null_mask,
                [e for e in edits if e.mnemonic == mnemonic],
            )
            finite = values[np.isfinite(values)]
            vmin = float(np.min(finite)) if finite.size else 0.0
            vmax = float(np.max(finite)) if finite.size else 1.0
            if vmax <= vmin:
                vmin, vmax = vmin - 1.0, vmax + 1.0
            self._presentation.tracks.append(
                BoundTrack(
                    id=f"edited-{mnemonic}",
                    role="curve",
                    title=f"{mnemonic} 校正",
                    width_fraction=0.25,
                    scale=ScaleSpec(mode="linear", min=vmin, max=vmax, unit=curve.unit),
                    layers=[
                        BoundCurveLayer(
                            mnemonic=mnemonic,
                            color="#10b981",
                            unit=curve.unit,
                            values=values,
                            null_mask=null_mask,
                        )
                    ],
                )
            )
            applied += 1
        self.multi_track_canvas.set_presentation(self._presentation)
        return applied, diags

    def _apply_derived_curves(self) -> tuple[int, list[str]]:
        """(Re)attach derived curve tracks to the current single-well plot.

        Removes any previous ``derived-*`` tracks, re-evaluates the well's
        formulas against its curves, and refreshes the canvas. Returns
        ``(applied_count, diagnostics)``.
        """
        if self._presentation is None or self._workspace is None:
            return 0, []
        well_id = self._selected_well_id or ""
        if not well_id:
            return 0, []
        self._presentation.tracks = [
            t for t in self._presentation.tracks
            if not str(t.id).startswith("derived-")
        ]
        formulas, _ = load_formulas_for_well(self._workspace, well_id)
        if not formulas:
            self.multi_track_canvas.set_presentation(self._presentation)
            return 0, []
        try:
            doc = self.session.ensure_well_loaded(self._workspace, well_id)
        except Exception:  # noqa: BLE001
            return 0, ["井数据未加载"]
        context = {c.mnemonic: c.values for c in doc.curves}
        nulls = {c.mnemonic: c.null_mask for c in doc.curves}
        from well_log_workstation.formula import evaluate_expression
        from well_log_workstation.template_model import BoundCurveLayer, BoundTrack, ScaleSpec

        diags: list[str] = []
        applied = 0
        for f in formulas:
            try:
                values, mask = evaluate_expression(f.expression, context, nulls)
            except Exception as exc:  # noqa: BLE001 — FormulaError and friends
                diags.append(f"{f.name}: {exc}")
                continue
            finite = values[np.isfinite(values)]
            vmin = float(np.min(finite)) if finite.size else 0.0
            vmax = float(np.max(finite)) if finite.size else 1.0
            if vmax <= vmin:
                vmin, vmax = vmin - 1.0, vmax + 1.0
            self._presentation.tracks.append(
                BoundTrack(
                    id=f"derived-{f.name}",
                    role="curve",
                    title=f"{f.name}",
                    width_fraction=0.25,
                    scale=ScaleSpec(mode="linear", min=vmin, max=vmax, unit=""),
                    layers=[
                        BoundCurveLayer(
                            mnemonic=f.name,
                            color="#8b5cf6",
                            unit="",
                            values=values,
                            null_mask=mask,
                        )
                    ],
                )
            )
            applied += 1
        self.multi_track_canvas.set_presentation(self._presentation)
        return applied, diags

    def _on_open_recent_workspace(self, path: str) -> None:
        """Open a path from the recent list (menu / API; no startup page)."""
        p = Path(path).expanduser()
        if not p.is_dir():
            QMessageBox.warning(
                self,
                "无法打开",
                f"路径不存在或不是目录：\n{path}\n\n已从最近列表移除。",
            )
            remove_recent(path)
            return
        try:
            ws = open_workspace(p)
            self.set_workspace(ws)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "打开工区失败", str(exc))
            if "不存在" in str(exc) or "workspace" in str(exc).lower():
                remove_recent(path)
        except OSError as exc:
            QMessageBox.warning(self, "打开工区失败", str(exc))
            remove_recent(path)

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

    def _on_section_from_line(self) -> None:
        """Pick wells along a section line and create a correlation plot.

        FRS §4.2 workflow 1: the user defines a section line (endpoints +
        buffer); wells inside the buffer are ordered by their projection along
        the line and fed into a correlation plot.
        """
        if self._workspace is None:
            QMessageBox.information(self, "平面画线生成剖面", "请先打开工区。")
            return
        from well_log_workstation.section_line import pick_wells_along_line
        from well_log_workstation.section_line_dialog import SectionLineDialog

        dlg = SectionLineDialog(self._workspace.wells, parent=self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        value = dlg.value()
        if value is None:
            return
        p0, p1, buffer_deg = value
        picked = pick_wells_along_line(
            self._workspace.wells, p0, p1, buffer_deg=buffer_deg
        )
        if len(picked) < 2:
            QMessageBox.information(
                self,
                "平面画线生成剖面",
                f"缓冲带内仅 {len(picked)} 口井，需要 ≥2 口才能生成剖面。",
            )
            return
        template_id = self._current_template_id() or "std-gr-rt-den"
        well_ids = [w.id for w in picked]
        try:
            plot = self.create_correlation_plot_document(
                well_ids, template_id,
                name=f"沿线剖面 {len(well_ids)}井",
            )
        except WorkspaceError as exc:
            QMessageBox.warning(self, "生成剖面失败", str(exc))
            return
        names = "、".join(w.name for w in picked[:4])
        if len(picked) > 4:
            names += "…"
        self.statusBar().showMessage(
            f"已沿剖面线选取 {len(picked)} 口井生成对比图：{names}", 5000
        )

    def _on_edit_section_faults(self) -> None:
        """Edit the active section plot's faults (FRS §3.3 / P1-A)."""
        if (
            self._workspace is None
            or self._active_plot_type != "section"
            or not self._active_plot_id
        ):
            return
        from well_log_workstation.section_geometry.fault_dialog import (
            SectionFaultDialog,
        )
        from well_log_workstation.section_geometry import faults_to_json

        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加载图件失败", str(exc))
            return
        well_count = max(2, len(plot.well_ids))
        dlg = SectionFaultDialog(
            current=faults_from_json(plot.faults),
            well_count=well_count,
            parent=self,
        )
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_faults = dlg.value()
        plot.faults = faults_to_json(new_faults)
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存断层失败", str(exc))
            return
        # Re-render with the updated faults.
        self._show_section(plot)
        self.statusBar().showMessage(f"已更新断层（{len(new_faults)} 条）", 4000)

    def _on_edit_section_contacts(self) -> None:
        """Edit the active section plot's fluid contacts (FRS §3.3 / P1-B)."""
        if (
            self._workspace is None
            or self._active_plot_type != "section"
            or not self._active_plot_id
        ):
            return
        from well_log_workstation.section_geometry import contacts_to_json
        from well_log_workstation.section_geometry.contact_dialog import (
            SectionContactDialog,
        )

        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加载图件失败", str(exc))
            return
        well_count = max(2, len(plot.well_ids))
        well_names = [
            (next((w.name for w in self._workspace.wells if w.id == wid), wid[:8]))
            for wid in plot.well_ids
        ]
        dlg = SectionContactDialog(
            current=contacts_from_json(plot.contacts),
            well_count=well_count,
            well_names=well_names,
            parent=self,
        )
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_contacts = dlg.value()
        plot.contacts = contacts_to_json(new_contacts)
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存流体界面失败", str(exc))
            return
        self._show_section(plot)
        self.statusBar().showMessage(
            f"已更新流体界面（{len(new_contacts)} 条）", 4000
        )

    def _on_draw_section_lens_toggled(self, checked: bool = False) -> None:
        """Toggle freehand lens capture on the section canvas."""
        self.section_canvas.set_draw_lens_mode(bool(checked))
        if checked:
            snap = "吸附" if self.section_lens_snap_check.isChecked() else ""
            smooth = "平滑" if self.section_lens_smooth_check.isChecked() else ""
            extras = " · ".join(s for s in (snap, smooth) if s)
            msg = "透镜体绘制：左键加点 · 双击/Enter 闭合 · 右键/Esc 取消"
            if extras:
                msg = f"{msg} · {extras}"
            self.statusBar().showMessage(msg, 6000)

    def _on_section_lens_snap_toggled(self, checked: bool = False) -> None:
        """Toggle snap-to-formation-tops for freehand lens vertices."""
        self.section_canvas.set_snap_tops(bool(checked))

    def _on_section_lens_smooth_toggled(self, checked: bool = False) -> None:
        """Toggle Chaikin smoothing for the live lens draft preview.

        Also seeds newly drawn lenses with ``smooth=True`` (the canvas passes
        this into ``finalize_draft``); existing lenses keep their own per-lens
        flag editable via the lens dialog.
        """
        self.section_canvas.set_lens_smooth(bool(checked))

    def _on_section_lens_completed(self, lens: object) -> None:
        """Persist a newly closed freehand lens onto the active section plot."""
        if (
            self._workspace is None
            or self._active_plot_type != "section"
            or not self._active_plot_id
        ):
            return
        from well_log_workstation.section_geometry import lenses_to_json

        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存透镜体失败", str(exc))
            return
        plot.lenses = lenses_to_json(self.section_canvas.lenses())
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存透镜体失败", str(exc))
            return
        label = getattr(lens, "label", "") or "透镜体"
        self.statusBar().showMessage(
            f"已添加透镜体「{label}」（共 {len(plot.lenses)} 个）", 4000
        )
        # Keep draw mode on for consecutive lenses.
        self.section_lens_draw_btn.setChecked(True)

    def _on_edit_section_lenses(self) -> None:
        """Dialog edit of freehand section lenses."""
        if (
            self._workspace is None
            or self._active_plot_type != "section"
            or not self._active_plot_id
        ):
            return
        from well_log_workstation.section_geometry import (
            lenses_from_json,
            lenses_to_json,
        )
        from well_log_workstation.section_geometry.lens_dialog import (
            SectionLensDialog,
        )

        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加载图件失败", str(exc))
            return
        dlg = SectionLensDialog(
            current=lenses_from_json(plot.lenses),
            parent=self,
        )
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_lenses = dlg.value()
        plot.lenses = lenses_to_json(new_lenses)
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存透镜体失败", str(exc))
            return
        self._show_section(plot)
        self.statusBar().showMessage(
            f"已更新透镜体（{len(new_lenses)} 个）", 4000
        )

    def _on_edit_section_surfaces(self) -> None:
        """Edit the active section plot's erosion/onlap surfaces (FRS §3.x P1)."""
        if (
            self._workspace is None
            or self._active_plot_type != "section"
            or not self._active_plot_id
        ):
            return
        from well_log_workstation.section_geometry.erosion_surface import (
            surfaces_from_json,
            surfaces_to_json,
        )
        from well_log_workstation.section_geometry.erosion_surface_dialog import (
            SectionErosionSurfaceDialog,
        )

        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "加载图件失败", str(exc))
            return
        well_count = max(2, len(plot.well_ids))
        well_names = [
            (next((w.name for w in self._workspace.wells if w.id == wid), wid[:8]))
            for wid in plot.well_ids
        ]
        dlg = SectionErosionSurfaceDialog(
            current=surfaces_from_json(plot.surfaces),
            well_count=well_count,
            well_names=well_names,
            parent=self,
        )
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_surfaces = dlg.value()
        plot.surfaces = surfaces_to_json(new_surfaces)
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError as exc:
            QMessageBox.warning(self, "保存剥蚀/超覆面失败", str(exc))
            return
        self._show_section(plot)
        self.statusBar().showMessage(
            f"已更新剥蚀/超覆面（{len(new_surfaces)} 条）", 4000
        )

    def _on_section_spacing_changed(self, _idx: int = -1) -> None:
        """Toggle equal vs geographic well spacing (FRS §3.1 / P1-C)."""
        guard = getattr(self, "_section_spacing_guard", False)
        if guard or self._workspace is None or self._active_plot_type != "section":
            return
        if not self._active_plot_id:
            return
        spacing = str(self.section_spacing_combo.currentData() or "equal")
        if spacing not in ("equal", "geographic"):
            spacing = "equal"
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        plot.well_spacing = spacing
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            pass
        self._show_section(plot)
        self.statusBar().showMessage(
            f"井距模式：{'地理井距' if spacing == 'geographic' else '等井距'}",
            4000,
        )

    def _on_section_ornaments_toggled(self, checked: bool = False) -> None:
        """Toggle publication ornaments on the section (FRS §5 / P2-C)."""
        if self._workspace is None or self._active_plot_type != "section":
            return
        if not self._active_plot_id:
            return
        try:
            plot = load_plot_document(self._workspace, self._active_plot_id)
        except WorkspaceError:
            return
        plot.ornaments = bool(checked)
        try:
            save_plot_document(self._workspace, plot)
        except WorkspaceError:
            pass
        self.section_canvas.set_show_ornaments(bool(checked))
        self.statusBar().showMessage(
            f"出版整饰：{'开' if checked else '关'}", 3000
        )

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

    def _choose_single_well_pdf_options(
        self,
    ) -> tuple[str, bool, bool, bool] | None:
        """PDF export options for engine single-well exports (ADR 0053 + FRS §5).

        Returns ``(text_mode, crop_marks, layered_pdf, border_frame)`` or None.
        """
        return self._choose_pdf_export_options(plot_type="single_well")

    def _choose_pdf_export_options(
        self,
        *,
        plot_type: str = "single_well",
    ) -> tuple[str, bool, bool, bool] | None:
        """Shared PDF options dialog for single-well and correlation menu export.

        Returns ``(text_mode, crop_marks, layered_pdf, border_frame)`` or None
        if cancelled. Correlation disables engine-only controls (text mode /
        layered OCG); crop marks and frame border remain available and are
        honoured by the Qt paint path.
        """
        from well_log_workstation.export_options_dialog import (
            PdfExportOptionsDialog,
        )

        dlg = PdfExportOptionsDialog(self, plot_type=plot_type)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return None
        return dlg.value()

    def _export_active_plot(self, fmt: ExportFormat) -> None:
        """Route the active plot's export through the T8 dispatcher.

        Single-well SVG/PDF default to the engine backend when available (T11).
        Single-well PDF offers outline vs searchable dual mode (ADR 0053).
        Correlation PDF uses the same options dialog surface (crop marks on
        Qt path; engine-only controls disabled).
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
                pdf_crop_marks = False
                pdf_layered = False
                pdf_border_frame = False
                if fmt == "pdf":
                    chosen = self._choose_single_well_pdf_options()
                    if chosen is None:
                        return
                    (
                        pdf_text_mode,
                        pdf_crop_marks,
                        pdf_layered,
                        pdf_border_frame,
                    ) = chosen
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
                            # FRS §5 export options (engine): crop marks +
                            # layered PDF. layered_pdf is engine-only (the Qt
                            # fallback has no OCG); crop_marks is honoured by
                            # the Qt path too (see below).
                            kwargs["crop_marks"] = pdf_crop_marks
                            kwargs["layered_pdf"] = pdf_layered
                    except (EngineUnavailable, EngineSubmitError, ExportError):
                        kwargs["backend"] = "qt"
                        kwargs["paint_fn"] = self._paint_active_plot
                        backend_note = "（引擎不可用，已回退 Qt）"
                        if fmt == "pdf":
                            # Qt fallback honours crop marks + frame border.
                            kwargs["crop_marks"] = pdf_crop_marks
                            kwargs["border_frame"] = pdf_border_frame
                else:
                    kwargs["backend"] = "qt"
                    kwargs["paint_fn"] = self._paint_active_plot
                    if fmt == "pdf":
                        kwargs["crop_marks"] = pdf_crop_marks
                        kwargs["border_frame"] = pdf_border_frame
            elif plot.type == "correlation":
                # B0 (#300): Qt paint for SVG/PDF; PNG prefers widget grab for
                # links/datum fidelity, with paint_fn fallback.
                # PDF: same options dialog as single-well (crop marks apply;
                # text mode / layered disabled — no engine correlation backend).
                if fmt == "png":
                    out = self._export_correlation_png(Path(path))
                    QMessageBox.information(
                        self,
                        "导出成功",
                        f"PNG 已写入（对比图 · Qt 画布抓取）:\n{out}\n"
                        f"大小 {out.stat().st_size} 字节",
                    )
                    return
                pdf_crop_marks = False
                pdf_border_frame = False
                if fmt == "pdf":
                    chosen = self._choose_pdf_export_options(
                        plot_type="correlation"
                    )
                    if chosen is None:
                        return
                    _mode, pdf_crop_marks, _layered, pdf_border_frame = chosen
                kwargs["paint_fn"] = self._paint_active_plot
                kwargs["backend"] = "qt"
                if fmt == "pdf":
                    kwargs["crop_marks"] = pdf_crop_marks
                    kwargs["border_frame"] = pdf_border_frame
                backend_note = "（对比图 · Qt 矢量）"
            elif plot.type == "section":
                # Same options surface as correlation (Qt paint + crop marks).
                pdf_crop_marks = False
                pdf_border_frame = False
                if fmt == "pdf":
                    chosen = self._choose_pdf_export_options(plot_type="section")
                    if chosen is None:
                        return
                    _mode, pdf_crop_marks, _layered, pdf_border_frame = chosen
                kwargs["paint_fn"] = self._paint_active_plot
                kwargs["backend"] = "qt"
                if fmt == "pdf":
                    kwargs["crop_marks"] = pdf_crop_marks
                    kwargs["border_frame"] = pdf_border_frame
                backend_note = "（油藏剖面 · Qt 矢量）"
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

    def _paint_active_plot(
        self, painter, rect, *, depth_range: tuple[float, float] | None = None
    ) -> None:
        """Paint callback for the T8 Qt-paint export path (single/corr/section).

        ``depth_range`` optionally restricts the painted depth window — the
        WYSIWYG print preview uses it for per-page pagination.
        """
        if self._active_plot_type == "single_well" and self._presentation is not None:
            from well_log_workstation.export_plot import _paint_presentation
            _paint_presentation(
                painter, self._presentation, rect, depth_range=depth_range
            )
        elif self._active_plot_type == "correlation":
            self._paint_correlation_export(
                painter, rect, depth_range=depth_range
            )
        elif self._active_plot_type == "section":
            # Section: paint the section canvas into the rect (WYSIWYG
            # export; depth_range enables per-page print-preview slices).
            self.section_canvas.render_to(
                painter, rect, depth_range=depth_range
            )
            # Publication ornaments (FRS §5 / P2-C): full-size layer on export.
            if self.section_canvas.show_ornaments():
                from well_log_workstation.ornament import draw_ornaments

                data = self.section_canvas.ornament_data()
                if data is not None:
                    from PySide6.QtCore import QRectF

                    w = rect.width()
                    h = rect.height()
                    ow = min(w * 0.45, 420.0)
                    oh = min(h * 0.5, 260.0)
                    draw_ornaments(
                        painter,
                        QRectF(rect.right() - ow, rect.bottom() - oh, ow, oh),
                        data,
                    )

    def _paint_correlation_export(
        self, painter, rect, *, depth_range: tuple[float, float] | None = None
    ) -> None:
        """Vector export for correlation: columns + links (Qt paint, B0 #300).

        ``depth_range`` makes every column share the same depth window (print
        preview pagination); otherwise each column fits its own depth range.
        """
        from well_log_workstation.export_plot import _paint_presentation

        presentations = self._correlation_presentations
        n = max(1, len(presentations))
        # Match the interactive canvas: read its column gap + per-well
        # offsets + vertical exaggeration so the export is WYSIWYG (B0 #300).
        gap = float(getattr(self.correlation_canvas, "_column_gap", 6) or 6)
        offsets = getattr(self.correlation_canvas, "_well_x_offsets", None)
        ve = float(getattr(self.correlation_canvas, "_vertical_exaggeration", 1.0) or 1.0)
        stride = (rect.width() - gap * (n - 1)) / n if n else float(rect.width())
        col_w = stride

        def _col_center(i: int) -> float:
            base = rect.x() + i * (col_w + gap) + col_w / 2
            if offsets and 0 <= i < len(offsets):
                return base + offsets[i] * (col_w + gap)
            return base

        for i, pres in enumerate(presentations):
            cx = _col_center(i)
            sub = QRectF(
                cx - col_w / 2,
                rect.y(),
                col_w,
                rect.height(),
            )
            _paint_presentation(painter, pres, sub, depth_range=depth_range)
        # Overlay horizon links in shared display depth if available
        links = self._correlation_links
        if not links or n < 2:
            return
        from PySide6.QtGui import QColor, QPen

        # Approximate shared depth band (middle 80% of column height)
        top = rect.y() + rect.height() * 0.12
        bottom = rect.y() + rect.height() * 0.92
        shifts = self.correlation_canvas.depth_shifts()
        if depth_range is not None:
            d0, d1 = float(depth_range[0]), float(depth_range[1])
        else:
            # Collect depth ranges from presentations
            import numpy as np

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
            return top + ((d_disp - d0) / (d1 - d0)) * (bottom - top) * ve

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
            x_l = _col_center(li) + col_w / 2 - 4
            x_r = _col_center(ri) - col_w / 2 + 4
            painter.setPen(QPen(QColor(link.color or "#c0392b"), 1.2))
            painter.drawLine(
                int(x_l), int(y_of(ld)), int(x_r), int(y_of(rd))
            )

    def export_active_correlation(
        self,
        path: Path | str,
        fmt: ExportFormat,
        *,
        crop_marks: bool = False,
        border_frame: bool = False,
    ) -> Path:
        """Export active correlation plot to SVG/PDF/PNG (B0 #300).

        Backend: **Qt paint** for SVG/PDF (host multi-column + links);
        **PNG** via correlation canvas grab when possible.
        ``crop_marks`` and ``border_frame`` are honoured for SVG/PDF on the Qt
        paint path. Menu PDF export prompts via
        :meth:`_choose_pdf_export_options` then passes these flags.
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
            crop_marks=bool(crop_marks),
            border_frame=bool(border_frame),
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
        """Open WYSIWYG print preview for the active plot (single-well /
        correlation / section).

        Returns PrintPreviewInfo for tests, or None if nothing to preview.
        ``show=False`` builds info (and optionally dialog) without modal exec.
        """
        # Section first: opening a section may leave a stale single-well
        # _presentation behind, so the active plot type wins.
        if (
            self._active_plot_type == "section"
            and self.section_canvas.column_count() >= 2
        ):
            dr = self.section_canvas.depth_range()
            if dr is not None:
                d0, d1 = dr
                unit = "m"
                columns = self.section_canvas.columns()
                if columns:
                    unit = columns[0].depth_unit or unit
                page_spec = PageSpec(orientation="landscape")
                info = compute_print_preview(
                    plot_name="油藏剖面",
                    depth_top=d0,
                    depth_bottom=d1,
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
        if self._presentation is not None and self._presentation.track_count > 0:
            d0, d1 = depth_range_from_presentation(self._presentation)
            vr = self.multi_track_canvas.depth_range()
            if vr is not None:
                d0, d1 = vr
            # WYSIWYG default: A4 portrait, whole visible depth on one page;
            # the dialog can paginate by depth / change paper interactively.
            page_spec = PageSpec(orientation="portrait")
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
        # Mutually exclusive with the other single-well gestures.
        if enabled:
            self._act_draw_curve.setChecked(False)
            self.multi_track_canvas.set_draw_curve_mode(False)
            self._act_depth_shift.setChecked(False)
            self.multi_track_canvas.set_shift_mode(False)
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

    def _on_toggle_depth_shift(self, checked: bool = False) -> None:
        """Toggle interactive depth-shift mode on the single-well canvas."""
        enabled = self._act_depth_shift.isChecked()
        if enabled and (
            self._presentation is None or self._selected_well_id is None
        ):
            self._act_depth_shift.setChecked(False)
            QMessageBox.information(
                self, "深度校正", "请先应用图版到选中井，再开启深度校正。"
            )
            return
        # Mutually exclusive with the other single-well gestures.
        if enabled:
            self._act_draw_curve.setChecked(False)
            self.multi_track_canvas.set_draw_curve_mode(False)
            self._act_pick_tops.setChecked(False)
            self.multi_track_canvas.set_pick_mode(False)
        self.multi_track_canvas.set_shift_mode(enabled)
        # Shift needs host canvas hit-testing; force host like pick mode.
        self._sync_primary_single_well_surface()
        if enabled:
            self.document_tabs.setCurrentIndex(0)
            self.single_well_stack.setCurrentIndex(0)
            self.statusBar().showMessage(
                "深度校正：拖拽层位线调整深度 · 释放即保存（可撤销）",
                8000,
            )

    def _on_canvas_depth_shift_committed(self, top_id: str, new_depth: float) -> None:
        """Commit a dragged top depth as an undoable tops edit."""
        if self._selected_well_id is None or self._workspace is None:
            return
        try:
            top = self.set_top_depth(self._selected_well_id, top_id, new_depth)
        except (TopsError, WorkspaceError) as exc:
            QMessageBox.warning(self, "深度校正失败", str(exc))
            return
        except OSError as exc:
            QMessageBox.warning(self, "深度校正失败", str(exc))
            return
        self.statusBar().showMessage(
            f"已调整层位 {top.name} → {top.depth:.2f}（可撤销）", 5000
        )

    def _on_toggle_draw_curve(self, checked: bool = False) -> None:
        """Toggle freehand curve drawing mode on the single-well canvas."""
        enabled = self._act_draw_curve.isChecked()
        if enabled and (
            self._presentation is None or self._selected_well_id is None
        ):
            self._act_draw_curve.setChecked(False)
            QMessageBox.information(
                self, "手绘曲线", "请先应用图版到选中井，再开启手绘曲线。"
            )
            return
        # Mutually exclusive with tops pick / depth shift (gesture clash).
        if enabled:
            self._act_pick_tops.setChecked(False)
            self.multi_track_canvas.set_pick_mode(False)
            self._act_depth_shift.setChecked(False)
            self.multi_track_canvas.set_shift_mode(False)
        self.multi_track_canvas.set_draw_curve_mode(enabled)
        # Freehand needs host canvas hit-testing; force host like pick mode.
        self._sync_primary_single_well_surface()
        if enabled:
            self.document_tabs.setCurrentIndex(0)
            self.single_well_stack.setCurrentIndex(0)
            self.statusBar().showMessage(
                "手绘曲线：按住左键在曲线道上绘制 · 释放保存 · Esc 取消",
                8000,
            )

    def _on_curve_drawn_committed(self, mnemonic: str, points: object) -> None:
        """Merge a drawn stroke into the well's freehand edit and reapply."""
        if self._selected_well_id is None or self._workspace is None:
            return
        from well_log_workstation.curve_edit import (
            CurveEdit,
            append_freehand_points,
            load_curve_edits_for_well,
            save_curve_edits_for_well,
        )

        pts = [(float(d), float(v)) for d, v in (points or [])]
        if len(pts) < 2:
            return
        edits, _diags = load_curve_edits_for_well(
            self._workspace, self._selected_well_id
        )
        existing = next(
            (e for e in edits if e.method == "freehand" and e.mnemonic == mnemonic),
            None,
        )
        merged = append_freehand_points(existing, mnemonic, pts)
        # Keep the other edits; replace any prior freehand for this curve.
        kept = [
            e
            for e in edits
            if not (e.method == "freehand" and e.mnemonic == mnemonic)
        ]
        kept.append(merged)
        try:
            save_curve_edits_for_well(self._workspace, self._selected_well_id, kept)
        except (WorkspaceError, OSError) as exc:
            QMessageBox.warning(self, "保存手绘曲线失败", str(exc))
            return
        # Show the edited version after a stroke so the user sees the result.
        self._show_curve_edits = True
        self._curve_version_guard = True
        try:
            idx = self.curve_version_combo.findData("corrected")
            self.curve_version_combo.setCurrentIndex(idx if idx >= 0 else 0)
        finally:
            self._curve_version_guard = False
        _ok_n, diags = self._apply_curve_edits()
        note = f"已手绘曲线 {mnemonic}（{len(pts)} 点）"
        if diags:
            note += f" · {len(diags)} 条应用失败"
        self.statusBar().showMessage(note, 4000)
        if diags:
            QMessageBox.warning(
                self, "手绘曲线提示", "\n".join(diags[:5])
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
