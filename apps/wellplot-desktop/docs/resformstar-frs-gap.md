# ResformSTAR FRS — Gap Analysis & Roadmap

**Date:** 2026-08-06
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
| 测斜/定向井轨迹 (MD/Inc/Az → TVD/TVDSS/ΔN/ΔE) | ❌ | `section_geometry/trajectory_2d.py:20-54` 仅直线 head→bottom；`datum` 显式拒绝 `tvd` | 轨迹计算（测斜→TVD/位移）→ SDK(几何)；存储/导入 → Desktop |
| 曲线别名字典（工区级） | ✅ | 工区级 `Workspace.mnemonic_alias`（canonical→[aliases]，存 `workspace.json`）；`mnemonic_alias.py` 双向 expand 接入 `_match_curve`/`default_checks`/`_leaf_matches_slot`；「测井别名字典」对话框编辑 | 已闭环：模板/默认显示按别名命中 |
| 多采样率/多版本曲线 | 🟡 | 模型支持多 curve；多 axis (md/tvd/tvdss) (`core/document.hpp:45-50`) | 版本管理（原始/校正/合成, 非破坏切换）→ Desktop(会话) |
| 统一地层层序字典（界-系-统-组-段-小层-砂层 + 颜色/线型/花纹） | ❌ | `FormationTop` 仅 name+depth+color (`tops_model.py:25-34`); SDK interval 语义含 lithology/stratigraphy (`core/document.hpp:409-416`) 但无层级体系 | 层序字典 schema → Desktop(数据)；渲染语义已可由 SDK interval 表达 |
| 岩心/试油/射孔数据库 | ❌ | 无 | → Desktop(数据) + 单井图道（见 §2） |

### 2.x 单井绘图

| FRS 需求 | 状态 | 现状 | 缺口 → 归属 |
|---|---|---|---|
| MD/TVD/TVDSS/TWT 多深度刻度 | 🟡 | SDK axis md/tvd/tvdss (`core/document.hpp:45-50`); Desktop 画布仅 MD | TWT 无；多轴同时显示 → SDK(渲染) + Desktop(画布) |
| 图道增删/隐藏/顺序拖拽/宽度 | 🟡 | 显示/宽度/比例 via `track_overrides` 右面板 (`template_model.py:309-379`) | 画布内拖拽排序/拉伸 → Desktop |
| 页眉/页脚自定义 | ✅ | `PlotHeaderSpec` (`template_model.py:50-111`) | — |
| 线性/对数刻度 | ✅ | `ScaleSpec` linear/log (`template_model.py:22-26`; `multi_track_canvas._paint_curve:436-497`) | 对数 1-4 数量级、反向刻度(密度) → Desktop(画布) |
| 超量程折叠 (Wrap-around) | ❌ | 无 | → SDK(曲线绘制) + Desktop |
| 基线充填 (如 GR>80) | 🟡 | SDK `IntervalLayerSpec` + `PatternDefinition` (`scene/scene.hpp:210,193`); Desktop host 画布无 fill 图道（role 仅 depth/curve, `template_model.py:39-47`） | Desktop(画布 fill 图道) 复用 SDK interval 语义 |
| 双曲线交叉充填 | 🟡 | SDK `CrossoverFillLayerSpec` (`scene/scene.hpp:232`) 已实现 (crossover 填充) | Desktop host 画布未渲染 → Desktop |
| 岩性描述道 SY/T 5615 花纹库 | 🟡 | SDK `PatternDefinition` 平铺图元（line/polyline/circle）已有；Desktop `litho_patterns/syt5615.json` 内置 7 种核心岩性（砂岩/泥岩/砾岩/灰岩/白云岩/膏盐岩/页岩），`make_qbrush` Qt 真矢量渲染（替换 Dense4 近似），section 四边形 + 井间充填（含楔形）接入；单井岩性道（新 track role）仍缺 | **P0(库+剖面/对比渲染✅)/后续**: 单井岩性道 → Desktop(画布 track role) |
| 试油/解释成果道 | ❌ | 无 | → Desktop(数据+渲染) |
| 射孔/井下工程道 | ❌ | SDK marker 语义含 casing_shoe (`core/document.hpp:433-439`) | → SDK(符号) + Desktop(道) |
| 岩心照片道 + 物性点叠加 | 🟡 | SDK `ImageLayerSpec`/`ImagePyramid` (`scene/image_pyramid.hpp:67`) | Desktop host 画布无 image role → Desktop |
| 交互深度校正 (Depth Shift) | ❌ | 无交互编辑；SDK session 有 patch 编辑 (`session.hpp:272-308`) | → SDK(编辑命令) + Desktop(手势) |
| 曲线编辑 (Despike/手绘/基线平移) | ❌ | 同上 | → SDK(编辑) + Desktop(UI) |
| 公式计算器 (VSH 等) | ❌ | 无；SDK 有 derived curve provenance (`core/document.hpp:328-334`) 供宿主求值 | → Desktop(解析器+派生曲线) |

### 3.x 连井剖面

| FRS 需求 | 状态 | 现状 | 缺口 → 归属 |
|---|---|---|---|
| 剖面选井/排序/增删/镜像 | 🟡 | correlation 列排序/间隙持久化 (`test_..._correlation_layout.py`) | 镜像翻转 → Desktop |
| 平面画线自动取井生成剖面 | ❌ | `plane_map_view` 仅井位散点（无选井/切线） | → Desktop(平面交互) |
| 高程剖面 (MSL) / 拉平剖面 | 🟡 | `datum/well_section_datum.py:19-79` md/tvdss/horizon；tvdss 为 -kb 近似（非真海拔） | 真 TVDSS（需测斜+KB）→ SDK(轨迹) |
| 斜井 TVD 剖面 / 沿轨迹展布 (Unfolded) | ❌ | `trajectory_2d` 直线；`datum` 拒绝 tvd | → SDK(轨迹) + Desktop(展布) |
| 实际井距/等井距、纵横比例尺解耦 | ❌ | correlation 等距列 | → Desktop(画布布局) |
| 分层线拖拽吸附 (Snap Picking) | 🟡 | link 拾取 10px 容差 (`correlation_canvas.py:162-202`)；无曲线极值吸附 | → Desktop(磁吸) |
| 曲线形态自动对比 | 🟡 | 仅名字匹配 (`correlation_links.match_tops_by_name:88-132`) | → Desktop(相似度) + SDK(信号处理) |
| **地层尖灭/透镜体/剥蚀超覆** | 🟡 | `interwell_fill.py` 支持单井区间的线性楔形尖灭（FRS §3.3，P0-C 已交付）；`interwell_fill.py:35-88` 同名分层直四边形；透镜体手绘、剥蚀/超覆截断仍缺 | **P0(楔形✅)/P1(透镜体·截断)**: Desktop(几何 numpy, section_geometry 模式) + SDK(多边形/三角化复用) |
| 断层错断/落差 | ❌ | `section_geometry/fault_2d.py:31-91` 曲线切片存在，但 `PlotDocument` 无字段、shell 读死字段 (`shell.py:2443-2452`) → 恒空；无 UI | **P1: 数据模型+几何 → Desktop；渲染复用 SDK custom layers** |
| 流体界面 OWC/GOC + 复合充填 | ❌ | `contact_2d.py:35-91` 折线几何存在，同样无持久化/UI | **P1: 同上** |
| 磁吸/手绘平滑 | ❌ | 无 | → Desktop(交互) |
| 全局撤销/重做 | 🟡 | SDK session 命令栈 (`session/session.hpp`); Desktop 对 datum/link 有撤销 (`tops_history.py`) | 扩展覆盖新编辑 → Desktop |

### 4.x 联动 & 5.x 出版导出

| FRS 需求 | 状态 | 现状 | 缺口 → 归属 |
|---|---|---|---|
| 平面-剖面双向联动 | ❌ | 无（平面仅散点） | **P2: Desktop 事件总线** |
| 图例栏 (Legend Block) | 🟡 | SDK pagination 有 legend 色带 (`pagination.hpp:64-65`; `pagination.cpp:232-243`) | 动态花纹/颜色图例 → SDK(导出) + Desktop(整饰) |
| 接合图 (Location Map) | ❌ | 无 | **P2: Desktop(整饰)** |
| 责任表/图框 | ❌ | 无 | **P2** |
| 比例尺/指北针 | 🟡 | depth scale 页脚 (`PlotHeaderSpec`); SDK 页脚带 (`pagination.cpp:221`) | → SDK(导出) |
| 长卷多页分切 + 剪切线 | 🟡 | SDK `PaginatedSvgExporter` 连续/分页 + 重复页眉/图例 (`export/pagination.hpp:34,104`); CGM 多 PICTURE (`cgm.hpp:69-74`); 无 crop marks | 剪切线 → SDK(导出) |
| PDF/SVG/CGM 矢量导出 | ✅ | 单井 engine 后端 SVG/PDF/CGM; correlation/section Qt paint SVG/PDF/PNG (`export_dispatch.py:157-200`) | correlation/section 无 CGM → SDK(scene 导出) |
| 分层 PDF（图道开关） | 🟡 | PDF 搜索文本层 (`pdf_scene.hpp:63-69`); 无 per-track layer 开关 | → SDK(导出) |
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
| **P1-A 断层错断** | 多边形切割基元（上/下盘） | `PlotDocument` fault 字段 + 编辑 UI + 渲染（错断多边形、落差） |
| **P1-B 流体界面** | 界面切割多边形 | OWC/GOC 持久化 + UI + 油水双色/过渡带渲染 |
| **P1-C 挂井与展布** | 测斜→TVD/TVDSS/ΔN/ΔE 计算；真 TST | 测斜导入 (MD/Inc/Az) + datum tvd 模式 + Unfolded 展布 |

### P2 — 公式 / 联动 / 出版整饰

| 切片 | SDK | Desktop |
|---|---|---|
| **P2-A 公式计算器** | 表达式求值引擎（头文件独立） | 公式编辑器 + 派生曲线写入会话 |
| **P2-B 平面-剖面联动** | — | 平面切线选井 → 生成剖面；剖面修正 → 平面刷新（事件总线） |
| **P2-C 出版整饰** | 剪切线；per-track PDF layer | 图例栏、接合图、责任表图框生成 |

---

## 4. 建议的首个开发切片

**P0-C 砂体尖灭多边形（含 interwell_fill 接入）**，理由：

1. FRS 明确定位“尖灭多边形算法是决定连井对比图专业度的关键门槛”；
2. 现有 `interwell_fill` / `tie_quads` / `correlation_canvas` 提供了最小接入面（增量而非全新模块）；
3. 几何为纯 numpy（`section_geometry` 已有模式），可无头单测；
4. 与 SDK 边界清晰：先在 Desktop 落地业务几何，多边形布尔/渲染基元需要时再提升进 SDK（避免过早 C++ 绑定成本）。

备选：P0-A 别名字典（最小、自包含、立即提升模板命中率）。
