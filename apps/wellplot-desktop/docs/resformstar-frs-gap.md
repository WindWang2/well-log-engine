# ResformSTAR FRS — Gap Analysis & Roadmap

**Date:** 2026-08-06
**Status:** FRS P0 + P1 + P2 全部交付（12 个切片）；测试套件 394 passed / 0 failed（2026-08-06 清理了 5 个预存在失败：schema 断言 5→7、session 树加载断言、ADR 0055 + plugin-runtime-status 文档补齐）
**Scope:** 《ResformSTAR 工业级对标：单井与连井剖面功能需求规范说明书 (FRS)》 vs
current codebase. Ground-truth established by code inventory (well-log-engine SDK +
WellPlot Desktop), with `file:line` references.

**Ownership rule used throughout:** the **SDK** (well-log-engine) owns reusable
rendering primitives, geometry algorithms, export/IO and the document model —
everything headless-testable. The **Desktop** (well_log_workstation) owns
workspace data management, workflows/UX, canvas interactions and plot-document
schema. Anything with business semantics tied to wells/plots/workspaces lives in
Desktop; pure math/rendering promoted into SDK when reused ≥2 places.

---

## 1. FRS section-by-section status

Legend: ✅ 已有 · 🟡 部分 · ❌ 缺失

### 1.x 数据管理

| FRS 需求 | 状态 | 现状（哪里） | 缺口 → 归属 |
|---|---|---|---|
| 坐标系/投影转换 (GK/UTM, CGCS2000/北京54/西安80) | 🟡 | `workspace.py` coordinate trio (project/display/target CRS); `plane_map_view.coerce_to_project_crs`; `crs_dialog.py` | 投影转换引擎缺失（依赖 geoviz 能力）→ SDK(IO/坐标) + Desktop(UI) |
| 井口坐标 X/Y、KB 补心海拔、GL、Max MD | 🟡 | LAS `LAT/LONG` → lng/lat/crs (`las_import.py:115-143`); `kb_m` on wells (`datum/well_section_datum.py:37-54`) | X/Y (GK/UTM 投影坐标) 未解析；GL 无 → Desktop(数据) + SDK(las.cpp 井头) |
| 测斜/定向井轨迹 (MD/Inc/Az → TVD/TVDSS/ΔN/ΔE) | 🟡 | `survey.py` 最小曲率法（业界标准）从 MD/Inc/Az 算 TVD/TVDSS/位移；测斜存 `wells/<id>/survey.json`（对称 tops）；datum 增 `tvd` 模式（无 survey 降级 0）；编辑对话框；section 画布真轨迹展布（按闭合位移摆列、斜井段弯曲）仍缺 | **P1(计算+tvd datum✅)/后续**: 真轨迹展布画布 → Desktop(画布) |
| 曲线别名字典（工区级） | ✅ | 工区级 `Workspace.mnemonic_alias`（canonical→[aliases]，存 `workspace.json`）；`mnemonic_alias.py` 双向 expand 接入 `_match_curve`/`default_checks`/`_leaf_matches_slot`；「测井别名字典」对话框编辑 | 已闭环：模板/默认显示按别名命中 |
| 多采样率/多版本曲线 | 🟡 | 模型支持多 curve；多 axis (md/tvd/tvdss) (`core/document.hpp:45-50`)；**版本管理（原始/校正 非破坏切换）已交付 Desktop 侧**：会话级「曲线版本」combo（`CurveVersionCombo`）在 校正（含编辑曲线 edited-*）/ 原始（不含）间切换，`_apply_curve_edits(show_edited=...)` 参数化，保存编辑/手绘后自动切回校正版，编辑定义存 `curve_edits.json` 运行时重算不碰原始数据 | 多采样率（同 mnemonic 多版本采样）仍缺 → Desktop(数据) |
| 统一地层层序字典（界-系-统-组-段-小层-砂层 + 颜色/线型/花纹） | ❌ | `FormationTop` 仅 name+depth+color (`tops_model.py:25-34`); SDK interval 语义含 lithology/stratigraphy (`core/document.hpp:409-416`) 但无层级体系 | 层序字典 schema → Desktop(数据)；渲染语义已可由 SDK interval 表达 |
| 岩心/试油/射孔数据库 | ❌ | 无 | → Desktop(数据) + 单井图道（见 §2） |

### 2.x 单井绘图

| FRS 需求 | 状态 | 现状 | 缺口 → 归属 |
|---|---|---|---|
| MD/TVD/TVDSS/TWT 多深度刻度 | 🟡 | SDK axis md/tvd/tvdss (`core/document.hpp:45-50`); Desktop 画布仅 MD | TWT 无；多轴同时显示 → SDK(渲染) + Desktop(画布) |
| 图道增删/隐藏/顺序拖拽/宽度 | 🟡 | 显示/宽度/比例 via `track_overrides` 右面板 (`template_model.py:309-379`) | 画布内拖拽排序/拉伸 → Desktop |
| 页眉/页脚自定义 | ✅ | `PlotHeaderSpec` (`template_model.py:50-111`) | — |
| 线性/对数刻度 | ✅ | `ScaleSpec` linear/log (`template_model.py:22-26`; `multi_track_canvas._paint_curve:436-497`) | 对数 1-4 数量级、反向刻度(密度) → Desktop(画布) |
| 超量程折叠 (Wrap-around) | ✅ | 已交付：`ScaleSpec.wrap` 字段（与 min/max/mode 同组，随 display_set + override snapshot/apply 流转）；4 处渲染点（`multi_track_canvas._paint_curve` / `correlation_canvas.x_map` / `section_canvas.x_map` / `export_plot._paint_curve`）把 `t=clamp(0,1)` 换成 wrap 模式 `t = t - floor(t)`（锯齿折回，连续样本自动画折回对角线，log 模式同样对归一化 t 折回；section 仅线性）；track 属性面板「超量程折叠」checkbox（持久化 + load/handler）；渲染点用 `getattr(scale,'wrap',False)` 防御旧 scale stub | **P3(wrap✅)** |
| 基线充填 (如 GR>80) | ✅ | 已交付：`ScaleSpec.fill_threshold`（None=关）+ `fill_direction`（above/below，如 GR>80 充填）；`baseline_fill.baseline_fill_polygons` 纯几何共享函数（曲线 x → 轨道右缘半透明多边形，alpha 96，null/出窗断段，与曲线循环同采样步长）；4 处渲染点接入（multi_track/export/corr/section）；track-props 面板「基线充填」checkbox + 阈值 spinbox + 方向 combo（override 快照/apply 持久化，2 个 `_parse_scale` 对齐）；花纹填充留后续 | **P3(基线充填✅)** |
| 双曲线交叉充填 | 🟡 | SDK `CrossoverFillLayerSpec` (`scene/scene.hpp:232`) 已实现 (crossover 填充) | Desktop host 画布未渲染 → Desktop |
| 岩性描述道 SY/T 5615 花纹库 | ✅ | SDK `PatternDefinition` 平铺图元（line/polyline/circle）已有；Desktop `litho_patterns/syt5615.json` 内置 7 种核心岩性（砂岩/泥岩/砾岩/灰岩/白云岩/膏盐岩/页岩），`make_qbrush` Qt 真矢量渲染（替换 Dense4 近似），section 四边形 + 井间充填（含楔形）接入；**单井岩性道已交付**：新 track role `litho`（`template_model`/`display_set` 绑定，`multi_track_canvas`/`export_plot` 共用 `paint_litho_bands`），岩性段数据模型 `lithology_model.py`（`wells/<id>/lithology.json`，Qt-free 可无头测）+ `LithologyDialog` 编辑器（含演示数据一键生成）+ 内置图版「标准岩性图」（`templates/std_litho.json`）；验收：单井岩性道可见 | **P0(库+剖面/对比/单井道✅)** |
| 试油/解释成果道 | ❌ | 无 | → Desktop(数据+渲染) |
| 射孔/井下工程道 | ❌ | SDK marker 语义含 casing_shoe (`core/document.hpp:433-439`) | → SDK(符号) + Desktop(道) |
| 岩心照片道 + 物性点叠加 | 🟡 | SDK `ImageLayerSpec`/`ImagePyramid` (`scene/image_pyramid.hpp:67`) | Desktop host 画布无 image role → Desktop |
| 交互深度校正 (Depth Shift) | ✅ | 已交付：单井画布「深度校正」模式（工具栏 toggle）——按住层位线拖拽实时预览（黄色高亮 + `名称 → 深度` 标注），释放后**编辑该 top 真实深度**（`wells/<id>/tops.json`，走 `set_top_depth` + `_tops_history` 撤销，无 schema bump）；`MultiTrackCanvas.hit_test_top` 10px 像素容差（沿用 correlation 惯例）；shift mode 强制 host 画布；非模式拖拽仍为平移 | **P3(交互校正✅)** |
| 曲线编辑 (Despike/手绘/基线平移) | ✅ | 已交付：**Despike**（`curve_edit.despike`：邻域中值 + MAD 阈值去毛刺，|v−med| > threshold×MAD 替换为中值，null 保持）+ **基线平移**（`apply_baseline` 常量偏移）+ **手绘曲线**（`apply_freehand` 按深度插值，`multi_track_canvas` 手绘模式：按住左键在曲线道 body 上绘制 → 红色预览 → 释放提交，Esc 取消，与拾取/深度校正互斥）；**非破坏派生**模式（原始曲线只读冻结不碰，编辑存 `wells/<id>/curve_edits.json`，运行时重算附加绿色 `edited-*` track —— 镜像 formulas 机制，与 `derived-*` 共存，无 schema bump）；「曲线编辑（去毛刺/基线平移）…」+「手绘曲线」菜单 + `CurveEditDialog` 表格编辑（曲线/方法/窗口+阈值/偏移，方法切换参数联动） | **P3(despike+baseline+手绘✅)** |
| 公式计算器 (VSH 等) | ✅ | `formula.py` 手写递归下降解析器（+−×÷^、括号、一元负号、log10/ln/exp/sqrt/abs/round/min/max）+ 逐元素数组求值（null 传播、标量广播、大小写不敏感）；公式存 `wells/<id>/formulas.json`；派生曲线运行时附加单井画布（`derived-*` tracks）；`FormulaDialog` 编辑器 | 已闭环：解析器/求值/集成单测 |

### 3.x 连井剖面

| FRS 需求 | 状态 | 现状 | 缺口 → 归属 |
|---|---|---|---|
| 剖面选井/排序/增删/镜像 | 🟡 | correlation 列排序/间隙持久化 (`test_..._correlation_layout.py`)；**镜像翻转已交付**（`_on_correlation_mirror` 反转 `plot.well_ids`，走 layout undo 快照，画布/导出自动跟随）；增删井 UI 仍缺 | 增删井 → Desktop |
| 平面画线生成剖面 | 🟡 | `section_line.py` 缓冲带选井（点到线段距离 + 沿线投影排序）+ `SectionLineDialog`（端点/缓冲输入 + 井选取端点 + 实时预览井数）+ 一键生成地层对比图（工作流 1 ✅）；平面图切线可视化（PaleoMapCanvas 无 overlay API）、剖面修正刷新平面（需等厚图）、光标同步（工作流 2/3）仍缺 | **P2(工作流1✅)/后续**: 工作流 2/3 → Desktop(联动) |
| 高程剖面 (MSL) / 拉平剖面 | 🟡 | `datum/well_section_datum.py:19-79` md/tvdss/horizon；tvdss 为 -kb 近似（非真海拔） | 真 TVDSS（需测斜+KB）→ SDK(轨迹) |
| 斜井 TVD 剖面 / 沿轨迹展布 (Unfolded) | 🟡 | `survey.py` 最小曲率法 + datum tvd 模式 + **地理井距展布**（井列按测斜闭合位移投影摆放 + 井内弯曲轨迹线，`well_spacing="geographic"`）；Unfolded（沿 MD 展布剖面类型）仍缺 | **P1(计算+datum+展布✅)/后续**: Unfolded 剖面类型 |
| 实际井距/等井距、纵横比例尺解耦 | ✅ | 已交付：correlation 画布**实际井距**（`correlation_spacing=real`，按井口经纬度 haversine 真实地面距离比例摆放井列，缺坐标降级等距；`well_spacing.haversine_m`/`wellhead_offsets` 纯 Python 无 Qt）+ **纵向放大 VE**（`vertical_exaggeration`，0.1–20.0 clamp，独立拉伸深度轴，与滚轮缩放正交）；`CorrelationCanvas._x_well` 统一收口 4 处内联列位（含 offset 线性插值）；`hit_test_top` VE 反投影；导出 `_paint_correlation_export` 同步 gap/offset/VE；画布全局 combo「井距模式」+ spinbox「纵向放大 VE」+ undo 快照 | **P3(实际井距+VE✅)** |
| 分层线拖拽吸附 (Snap Picking) | 🟡 | link 拾取 10px 容差 (`correlation_canvas.py:162-202`)；无曲线极值吸附 | → Desktop(磁吸) |
| 曲线形态自动对比 | 🟡 | 仅名字匹配 (`correlation_links.match_tops_by_name:88-132`) | → Desktop(相似度) + SDK(信号处理) |
| **地层尖灭/透镜体/剥蚀超覆** | 🟡 | 楔形尖灭 P0-C ✅；剥蚀/超覆截断 P1 ✅；**透镜体手绘** ✅；流体过渡带另见流体界面行 | **P0/P1/透镜体✅** |
| 断层错断/落差 | ✅ | `section_geometry/fault_section.py` 2D 位置+落差模型：`SectionFault2D` + `fault_polyline` + `apply_fault_throw_to_quad` + **`split_quad_by_fault`**；`PlotDocument.faults` 持久化；**`split_quad_composite` 对每条断层逐条全切**（非仅第一条） | **P1/P2/复合全切✅** |
| 流体界面 OWC/GOC + 复合充填 | ✅ | `FluidContact2D` + `split_quad_by_contact`；多接触/多断层全切；**过渡带** `transition_m` → 界面上下混色条带 + 对话框「过渡带(m)」列 | **P1/多接触/过渡带✅** |
| 磁吸/手绘平滑 | ✅ | 已交付：透镜体手绘**分层磁吸**（`SectionCanvas._snap_point`，10px 像素容差，命中井列分层深度即吸附，沿用 correlation `hit_test_top` 惯例）+ **手绘平滑**（`section_geometry/lens_body.smooth_ring` 闭合环 Chaikin 角点切平，paint-time、原始顶点保留可逆，对齐 pinchout 平滑惯例）；`LensBody2D.smooth` 字段（per-lens JSON，纯 `data.get` 无 schema bump）；画布全局开关 `吸附分层`/`平滑边缘` + lens 对话框 per-lens 平滑 checkbox；曲线极值磁吸留后续 | **P3(透镜体磁吸+平滑✅)** |
| 全局撤销/重做 | 🟡 | SDK session 命令栈 (`session/session.hpp`); Desktop 对 datum/link 有撤销 (`tops_history.py`) | 扩展覆盖新编辑 → Desktop |

### 4.x 联动 & 5.x 出版导出

| FRS 需求 | 状态 | 现状 | 缺口 → 归属 |
|---|---|---|---|
| 平面-剖面双向联动 | ❌ | 无（平面仅散点） | **P2: Desktop 事件总线** |
| 图例栏/责任表/接合图 | ✅ | `ornament.py`（P2-C）：责任表（图名/工区/比例尺/日期）、图例栏（花纹/断层/接触样本 + 标签，行列布局）、接合图（井位散点 + 剖面井高亮 + 指北针）、比例尺；section 画布交互预览 + 导出叠加（`plot.ornaments` 开关）；**图框边线已交付**（`_draw_qt_frame_border`，Qt 导出 SVG/PDF/PNG，`border_frame` 导出选项，镜像 crop marks wiring）；CGM 整饰件、长卷剪切线、SDK C++ 单井 PDF 图框留后续 | **P2(核心+图框✅)/后续**: SDK 侧 CGM 整饰 / 长卷剪切 / 单井 PDF 图框 |
| 接合图 (Location Map) | ✅ | `ornament.draw_location_map`：井位散点 + 剖面井红点高亮 + 指北针（P2-C） | — |
| 责任表/图框 | ✅ | 责任表 `ornament.draw_title_block`（图名/工区/比例尺/日期，P2-C）；**图框边线** `export_dispatch._draw_qt_frame_border`（页边距矩形边框，Qt 导出 SVG/PDF/PNG，`PdfExportOptionsDialog.border_frame` 选项，镜像 crop marks wiring，默认关） | **P2✅** |
| 比例尺/指北针 | 🟡 | depth scale 页脚 (`PlotHeaderSpec`); SDK 页脚带 (`pagination.cpp:221`) | → SDK(导出) |
| 长卷多页分切 + 剪切线 | ✅ | SDK `PaginatedSvgExporter` 连续/分页 + 重复页眉/图例 (`export/pagination.hpp:34,104`); CGM 多 PICTURE (`cgm.hpp:69-74`); **crop marks 已交付**(FRS §5):`ExportPageSpec.crop_marks` 四角剪切线(每角 2 条 5mm 短线),SVG(`pagination.cpp::append_crop_marks`,fixed+continuous)+ PDF(`pdf_scene.cpp::emit_crop_marks`,PAGE-mm 空间)几何一致,默认关 | — |
| PDF/SVG/CGM 矢量导出 | ✅ | 单井 engine SVG/PDF/CGM；correlation/**section** Qt paint；**PDF 导出选项对话框**对齐单井表面（剪切线；对比/剖面禁用文本模式与 OCG） | 无 CGM → SDK |
| 分层 PDF（图道开关） | ✅ | PDF 搜索文本层 (`pdf_scene.hpp:63-69`); **per-track 图层已交付**(FRS §5):`ExportPageSpec.layered_pdf` → 每图道一个 OCG(Optional Content Group),Catalog `/OCProperties`(默认全开)+ 每页 `/Properties` + 内容流 `/Lay<i> OC BMC…EMC` marked content;`PdfWriter::write` 增全局 `layers` 参数,默认空输出字节不变 | — |
| 岩心/试油等数据 | ❌ | 无 | → Desktop(数据) |

---

## 2. FRS 差距表勘误（FRS 原“当前代码库状态”列已过时）

| FRS 声称 | 实际 |
|---|---|
| “仅支持基础颜色填充，缺少花纹集” | 部分属实：SDK `PatternDefinition` 平铺图元已有，缺**地质花纹库**；Desktop section 有 Qt hatch 近似 |
| “仅有曲线道、充填道、基础图像道” | 单井画布确实仅 depth/curve；但**连井/油藏剖面画布、栅状图、平面图原型、3D 桥**已存在 |
| “仅有直线多边形闭合，无尖灭逻辑” | 属实：`tie_quads`/`interwell_fill` 只有直四边形 |
| “仅支持绘制一条断层直线” | 几何模块 `fault_2d.curtain_slice_fault` 可投影任意断层折线，但**数据模型无字段、UI 无入口**（死代码路径） |
| “支持高程与分层拉平，斜井展布较弱” | 属实：datum md/tvdss/horizon 已可用；轨迹为直线 |
| “无图上公式解析能力” | 属实 |
| “仅有独立平面图原型，无闭环联动” | 属实（平面仅散点） |
| “支持 PDF/SVG/CGM 导出，缺乏整饰件” | 属实：SDK 三个矢量导出齐全 + 分页/页眉/图例带；缺图框/接合图/剪切线 |

---

## 3. 路线图（SDK / Desktop 分工）

### P0 — 决定“像不像专业图件”的门槛（FRS 建议 1-2 个月）

| 切片 | SDK 部分 | Desktop 部分 | 验收 |
|---|---|---|---|
| **P0-A 别名字典** ✅ | — | `Workspace.mnemonic_alias`（canonical→[aliases]，存 workspace.json）；`mnemonic_alias.py` 双向 expand；接入 `_match_curve`/`default_checks`/`_leaf_matches_slot`；「测井别名字典」对话框 | 导入任意别名 GR 曲线可被模板命中；单测 |
| **P0-B SY/T 5615 花纹库** | `PatternDefinition` 扩展（点/斜线/三角/波浪等图元组合）；内置花纹目录 + 测试 golden | 岩性道渲染（host 画布）+ 花纹选择器；section 四边形改用真花纹 | 花纹 golden 单测；单井岩性道可见 |
| **P0-B SY/T 5615 花纹库** ✅ | — | 已交付：`litho_patterns/syt5615.json`（7 种核心岩性）+ `litho_pattern_lib`（加载/`make_qbrush` Qt 真矢量渲染/`pattern_to_engine_payload`）；section 四边形 + 井间充填（含楔形）接入真花纹；`PlotDocument.litho_pattern_map` 持久化 | 花纹加载/payload 契约/Qt 渲染/画布 smoke/持久化往返 |
| **P0-C 砂体尖灭多边形** ✅ | — | 已交付：`interwell_fill.build_interwell_fill_bands(pinchout_mode="linear")` 生成线性楔形；`PlotDocument.pinchout_mode/factor/smooth` 持久化；correlation 画布渲染（直线/贝塞尔平滑）；UI 控件 + 测试 | 纯 numpy 几何单测；画布 render smoke；持久化往返 |

### P1 — 断层 / 流体界面 / 展布

| 切片 | SDK | Desktop |
|---|---|---|
| **P1-A 断层错断** ✅ | — | 已交付：`section_geometry/fault_section.py` 2D 位置+落差模型（替换 3D CRS 死代码）；`SectionFault2D` + `fault_polyline` + `apply_fault_throw_to_quad`（下盘角点位移，正断降/逆断升）；`PlotDocument.faults` 持久化；section 画布渲染断层线 + 错断 quad；「编辑断层」对话框；旧 3D 测试迁移 | 几何单测（位移/反向/边界）；render smoke；持久化往返 |
| **P1-B 流体界面** ✅ | — | 已交付：`section_geometry/contact_section.py` 2D 每井深度模型（替换 3D CRS 死代码）；`FluidContact2D` + `contact_segment_2d` + `split_quad_by_contact`（quad 切上油/气·下水双色填充）；`PlotDocument.contacts` 持久化；section 画布渲染接触线 + 双色 quad；「编辑流体界面」对话框；旧 3D 测试迁移 | 几何单测（切分/边界/缺井）；render smoke；持久化往返 |
| **P1-C 测斜/TVD datum** ✅ | — | 已交付：`survey.py` 最小曲率法（MD/Inc/Az → TVD/TVDSS/ΔN/ΔE/闭合位移）；测斜存 `wells/<id>/survey.json`（对称 tops，load/save）；datum 增 `tvd` 模式（TVD−MD 位移，无 survey 降级 0）；`_show_section` 读 `plot.datum_mode` + tvd 加载 survey；「编辑测斜数据」菜单 + `SurveyDialog` | 计算单测（直井/斜井/边界）；插值；serde；datum tvd；存储往返 |
| **P1-B 流体界面** | 界面切割多边形 | OWC/GOC 持久化 + UI + 油水双色/过渡带渲染 |
| **P1-C 挂井与展布** | 测斜→TVD/TVDSS/ΔN/ΔE 计算；真 TST | 已交付：`survey.py` 最小曲率法 + datum tvd 模式 + 地理井距展布（`PlotDocument.well_spacing`，井列按闭合位移投影摆放 + 井内弯曲轨迹线）；Unfolded（沿 MD 展布）与真 TST 仍缺 | 计算/投影/展布单测 + 画布 smoke + 持久化往返 |

### P2 — 公式 / 联动 / 出版整饰

| 切片 | SDK | Desktop |
|---|---|---|
| **P2-A 公式计算器** ✅ | — | 已交付：`formula.py` 递归下降解析器 + 数组求值（null 传播/标量广播）；`formulas.json` 存储（对称 tops）；`_apply_derived_curves` 运行时附加派生曲线道；`FormulaDialog` 编辑器（语法预校验） | 解析器优先级/错误、VSH 数值、null 传播、集成附加/替换/诊断 |
| **P2-B 平面-剖面联动** | — | 部分交付（工作流 1）：`section_line.py` 缓冲带选井（距离/投影排序）+ `SectionLineDialog` + 「平面画线生成剖面」菜单 → 一键生成沿线排序的地层对比图；工作流 2（剖面修正刷新平面，需等厚图/构造图）与工作流 3（光标同步）留后续 | 几何/排序/截断单测 + dialog 解析 + 集成井序 |
| **P2-C 出版整饰** ✅ | — | 已交付：`ornament.py` 责任表（图名/工区/比例尺/日期）+ 图例栏（花纹/断层/接触样本行列布局）+ 接合图（井位+剖面井高亮+指北针）+ 比例尺；section 画布交互预览 + Qt 导出叠加（`PlotDocument.ornaments` 开关）；`_collect_section_ornaments` 数据收集 | 布局纯函数 + 绘制 smoke + 画布渲染 + 持久化往返 + 数据收集 |

---

## 4. 建议的首个开发切片

**P0-C 砂体尖灭多边形（含 interwell_fill 接入）**，理由：

1. FRS 明确定位“尖灭多边形算法是决定连井对比图专业度的关键门槛”；
2. 现有 `interwell_fill` / `tie_quads` / `correlation_canvas` 提供了最小接入面（增量而非全新模块）；
3. 几何为纯 numpy（`section_geometry` 已有模式），可无头单测；
4. 与 SDK 边界清晰：先在 Desktop 落地业务几何，多边形布尔/渲染基元需要时再提升进 SDK（避免过早 C++ 绑定成本）。

备选：P0-A 别名字典（最小、自包含、立即提升模板命中率）。
