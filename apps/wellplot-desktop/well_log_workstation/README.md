# WellPlot Desktop

**Product name:** WellPlot Desktop (epic #288 / T2 #290).  
**Python package / entrypoint:** `well_log_workstation` (unchanged for now).

Standalone **log-first** desktop product (wayfinder #207).  
Formerly branded “Well Log Workstation”; same shell, product identity upgraded toward an installable Desktop (轨 D).  

**Lives in WellLogEngine** as the reference host / SDK sample (`apps/wellplot-desktop`).  
Not Paleo Workbench. Rendering uses **WellLogEngine** (`welllog`) when available.

## Run

```bash
# From monorepo root, with PySide6 installed (e.g. repo .venv)
# Prefer Wayland: do NOT set QT_QPA_PLATFORM=xcb
unset QT_QPA_PLATFORM
unset PALEO_FORCE_XCB
unset WLWS_FORCE_XCB

python -m well_log_workstation
# or (after pip install -e .):
# wellplot-desktop
# wellplot-desktop --version   # no GUI; installer smoke (T15)
```

Window title and **帮助 → 关于…** show **WellPlot Desktop**.

## Extensions / plugins (T17 / #305)

First-ship extension surface is **declarative Custom Layer + engine embed**
(ADR 0018 / 0046). Full plugin Runtime is **out of first ship** — see
**[docs/plugin-runtime-status.md](../docs/plugin-runtime-status.md)** and
ADR 0055. Host helpers: `extension_points.list_extension_points()`,
`command_audit` (in-memory ring only).

## Installers (T15 / #303)

Independent Windows / Linux install packages (PyInstaller onedir + install/uninstall scripts).

See **[`packaging/README.md`](packaging/README.md)** for:

- `build.sh` / `build.ps1` (and optional Inno Setup `.iss`)
- install → start → uninstall acceptance steps
- residual config after uninstall (`--purge-config` / `-PurgeConfig`)

### Workspace (#217)

**文件 → 新建工区…** / **打开工区…** 选择目录。

```
<workspace>/
  workspace.json   # catalog (not engine Manifest)
  wells/
  plots/
  templates/
```

### Import LAS (#218)

With a workspace open: **文件 → 导入 LAS…**

- Copies file under `wells/<id>/`
- Updates `workspace.json` catalog
- Loads curves into the host session store (readable via `session.sample_value`)

### Multi-track template (#219)

1. Select a well in the left tree  
2. Choose a template in the right list (e.g. **标准三轨 GR-RT-DEN**)  
3. **应用到选中井** / 图版 → 应用当前图版  

Center canvas shows **one well, multiple tracks** (depth + GR/RT/DEN when present).

Depth viewport (host): **scroll wheel zoom**, **drag pan**, **double-click** reset to full range (same gestures on 对比-lite).

### 单井分析图文档 (#220)

- **图件 → 新建单井分析图…** — writes `plots/<id>.json` + catalog entry, opens multi-track view  
- **Double-click** a plot under 图件 in the left tree — reloads well from `wells/` and re-applies template

### 导出 SVG/PDF/PNG (#221, B0 #299/#300; B1 #304)

| 图件 | SVG | PDF | PNG | 后端 |
|---|---|---|---|---|
| **单井分析图** | ✓ | ✓ 双模式 | ✓ | + **CGM**（B1.CGM.2 引擎） | SVG/PDF 默认引擎。PDF 双选项（轮廓 / 可搜索 Latin-1）。**CGM**：导出菜单，花纹降纯色（披露） |
| **地层对比图** | ✓ | ✓ | ✓ | **Qt paint**（多列 + 连线）；PNG 优先抓取对比画布（含拉平/连线） |
| 其他类型 | 视类型 | 视类型 | 视类型 | 见 `export_dispatch.py` |

菜单：**导出 → 导出 SVG… / PDF… / PNG…**（有活动单井或对比图时启用）；单井另有 **CGM…**。  
单井 PDF 会先询问文本模式（引擎图形 vs 可搜索 Latin-1）；完整 CJK ToUnicode 子集仍未交付。  
**B1 关闭状态**（已交付切片 / 明确未宣称项）：[`docs/export-b1-status.md`](../docs/export-b1-status.md)。

### 地层对比图-lite (#222)

Need **≥2 wells** in the workspace catalog:

1. Choose a template in the right list  
2. **图件 → 新建地层对比图…** — multi-select wells → `plots/<id>.json` type `correlation`  
3. Center tab **地层对比图-lite**: side-by-side columns, **shared depth** pan (drag) / zoom (wheel)  
4. Double-click catalog entry under 图件 to reopen

**层位刷新（T10 / #298）：** 单井层位增删改后，若对比图已打开，**自动**重载各井 `tops.json`、更新连线深度与充填。右栏 **刷新对比图（层位）** 为显式重载。

**井间充填（T9 / #297）：** 右栏勾选 **显示井间充填** — 在相邻井之间、对共有且深度相邻的层位对绘制半透明四边形（不复制单井渲染）。状态写入图件 JSON；PNG 导出抓画布时一并可见。

### 层位 / Formation tops (#223)

Tops are host JSON next to the well LAS (`wells/<id>/tops.json`):

```json
{
  "schemaVersion": 1,
  "well_id": "…",
  "tops": [
    { "name": "T1", "depth": 1001.0, "unit": "m", "color": "#c0392b" }
  ]
}
```

- Right pane **层位** list for the selected well  
- Dashed depth markers on **单井** multi-track and **对比** columns  
- **层位 → 导入层位 JSON…** / **生成示例层位**  
- **层位 → 拾取层位（单击图道）** / **按深度添加层位…** (#226)  
  - Pick mode: click on multi-track canvas (or **Shift+click** without mode)  
  - Name dialog → save `tops.json` + inspector + markers  
- Missing or corrupt files → empty list + diagnostics (no crash)

Headless / CI:

```bash
QT_QPA_PLATFORM=offscreen python -m well_log_workstation
# or tests:
QT_QPA_PLATFORM=offscreen pytest tests/test_well_log_workstation_shell.py -q
```

XWayland debug only:

```bash
WLWS_FORCE_XCB=1 QT_QPA_PLATFORM=xcb python -m well_log_workstation
```

### WellLogEngine primary canvas (#224 / #225 / #227)

When the `welllog` package is on `PYTHONPATH` / installed, **applying a multi-track
template** prefers native `WellLogView` (`submit_multi_track`) as the single-well
surface. Host `MultiTrackCanvas` remains automatic fallback.

```bash
export PYTHONPATH="build/well-log-engine-python/python:well-log-engine/python${PYTHONPATH:+:$PYTHONPATH}"
# Force host canvas:
# export WLWS_DISABLE_ENGINE=1
# export WLWS_FORCE_HOST_CANVAS=1
python -m well_log_workstation
# 图件 → 优先使用引擎画布   (toggle; single-well + correlation-lite)
# 图件 → 刷新/打开引擎视图… / 引擎对比预览…
# 层位拾取 switches to host canvas for click hit-testing, then can return to engine
```

Correlation-lite (#228 / #232): with prefer-engine, create/open 对比图 calls
`submit_multi_well_section` (shared depth) with **multi-track per well** when
the template has curve tracks; host `CorrelationCanvas` is fallback.

Horizon links (#229–#231): tops with the **same name** on adjacent wells
auto-link when opening a contrast plot (**图件 → 按层位名自动连线**).  
**图件 → 点选层位连线** (or Shift+click tops): pick two tops on different wells.  
Right pane **对比连线** lists them; **清除连线** / **删除选中** update canvas +
`links[]`. Engine path submits `horizon_line` overlays.

Gap notes: `docs/research/2026-08-03-welllogengine-python-bindings-225.md`

## Phase-1 scope (locked)

| Decision | Choice |
|----------|--------|
| Shell | L — left tree · center tabs · right inspector |
| Workspace | F — directory + `workspace.json` |
| Templates | H — host JSON → Engine presentation (multi-track) |
| Documents | S1 — 单井多图道 + 对比-lite |

## Ticket chain

Wayfinder map **#207 closed** (phase-1 path delivered): host `#216`–`#223`,
engine bridge `#224`–`#228`, links `#229`–`#231`, multi-track multi-well `#232`.
