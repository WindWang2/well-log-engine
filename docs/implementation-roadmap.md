# 实施路线图

## 1. 工程目录建议

```text
well-log-engine/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── cmake/
│   ├── WellLogConfig.cmake.in
│   └── toolchains/
├── include/welllog/
│   ├── core/
│   ├── session/
│   ├── scene/
│   ├── table/
│   └── export/
├── src/
│   ├── core/
│   ├── session/
│   ├── scene/
│   ├── text/
│   ├── pattern/
│   ├── render_gl/
│   ├── export_vector/
│   ├── export_table/
│   ├── io/
│   ├── arrow/
│   └── qtwidgets/
├── bindings/
│   └── python/
├── schemas/
│   ├── manifest/
│   ├── pattern/
│   └── table-xml/
├── assets/
│   ├── fonts/
│   └── patterns/
├── tests/
│   ├── unit/
│   ├── property/
│   ├── snapshot/
│   ├── image/
│   ├── integration/
│   └── fuzz/
├── benchmarks/
│   ├── generators/
│   ├── scenarios/
│   └── runner/
├── examples/
│   ├── cpp_widgets/
│   └── pyside6/
└── docs/
```

实现时可以按 Target 拆分 `src` 子目录；公共 include 不得暴露第三方实现细节。

## 2. CMake Targets

建议：

- `WellLog::Core`
- `WellLog::Session`
- `WellLog::Scene`
- `WellLog::Table`
- `WellLog::RenderGL`
- `WellLog::ExportVector`
- `WellLog::ExportTable`
- `WellLog::IO`
- `WellLog::Arrow`（可选）
- `WellLog::QtWidgets`（可选）
- `WellLog::Python`（打包目标）

构建选项：

```text
WELLLOG_BUILD_SHARED
WELLLOG_BUILD_QT
WELLLOG_BUILD_PYTHON
WELLLOG_BUILD_ARROW
WELLLOG_BUILD_TESTS
WELLLOG_BUILD_BENCHMARKS
WELLLOG_BUILD_FUZZERS
WELLLOG_ENABLE_TRACING
WELLLOG_WARNINGS_AS_ERRORS
```

选项名在 CMake Review 时确定。

## 3. 阶段 1：基础契约

### 交付

- 独立 CMake 工程和 Package Consumer test；
- Entity ID/Runtime Handle；
- Result/Error/Diagnostic；
- Buffer View/SharedOwner/Null Bitmap；
- Sampling Axis/Curve/Interval/Marker；
- Document/Revision/Patch/Append；
- Depth Transform；
- Session Command/Event 骨架；
- 基准数据生成器；
- Manifest/Pattern/XML Schema 仓位。

### 验收

- Core 不依赖 Qt/Python/OpenGL/Arrow；
- checked arithmetic 和输入验证测试通过；
- Depth Transform 性质测试通过；
- NumPy/Arrow 尚可不实现，但 Buffer lifetime 测试可用原生 owner 完成；
- 一亿点合成数据可建立文档而不复制。

## 4. 阶段 2：Headless Prepared Scene

### 交付

- Track/Layer/Scale 模型；
- 多井场景基础布局；
- M4/Min-Max 层次摘要；
- 区间裁切；
- HarfBuzz/FreeType/ICU 文字；
- Pattern Definition；
- Prepared Scene/Frame Plan；
- CPU 语义拾取；
- SVG 最小后端；
- 语义快照测试。

### 验收

- 单井和多井数据模型贯通；
- 缺测、反向 Sampling Axis、重复深度正确；
- 视口查询不全量扫描；
- 文字、Pattern 与布局可在无 Qt/GL 环境测试；
- SVG 与 Prepared Scene 快照一致。

## 5. 阶段 3：单井 OpenGL、Qt 与 Python 纵向切片

### 交付

- OpenGL 3.3 Renderer；
- QOpenGLWidget；
- 平移、缩放、Cursor、Hover、Selection；
- VBO 轮换和预算上传；
- Glyph/Pattern Atlas；
- Qt Table Model；
- Shiboken6；
- NumPy Zero Copy；
- 单井 C++ 与 PySide6 示例；
- Capability Report、Profiler 基础。

### 验收

- Python 加入布局即可显示；
- Python 不参与 frame loop；
- GUI 线程阻塞不超过预算；
- NumPy 被 GC 前后生命周期正确；
- Context 重建恢复；
- 图形/表格选择与复制一致；
- 单井压力场景达到阶段性性能基线。

## 6. 阶段 4：专业制图与导出

### 交付

- 多 Scale Curve Layer；
- Curve Crossover Fill；
- Stencil Pattern Fill；
- Image/Text/Symbol Layer；
- 编辑 Tool、Undo/Redo、Document Patch；
- PDF/SVG/PNG/TIFF；
- XLSX/XML/CSV；
- Document Scale 与分页；
- Font embedding/subsetting；
- Export Snapshot、进度、取消、原子写出。

### 验收

- NPHI–RHOB 异单位/反向 Scale 交会正确；
- Pattern 屏幕与矢量相位一致；
- PDF 物理比例与分页正确；
- XML XSD 与 XLSX 大表拆分通过；
- 原始曲线不能被表格或编辑 Tool 原地修改；
- 导出不阻塞 GUI。

## 7. 阶段 5：多井、变换与追加

### 交付

- 多井横向布局/虚拟化；
- per-well 可逆 Depth Transform；
- 多 Marker 分段拉伸；
- Cross-Well Overlay；
- 多井统一拾取与整体导出；
- Append Batch 与增量 LOD；
- Follow Latest；
- 高频更新合并。

### 验收

- 20 口井/100 Track 场景功能正确；
- 同一 Surface 绘制跨井连接；
- Display Depth 与 Reference Depth 提示/导出不混淆；
- Depth Transform 正逆、控制点编辑、Undo 通过；
- Append 不复制旧块，批次保持原子。

## 8. 阶段 6：数据适配、规模化与迁移

### 交付

- Arrow C Data/IPC/mmap；
- 选定的 LAS/DLIS/LIS/716 Source Adapter；
- 缓存调优；
- Chrome Trace；
- 固定硬件 Runner；
- Paleo Workbench Adapter；
- Legacy/New Feature Flag；
- 对等性报告。

### 验收

- 主一亿点 SLO 全部通过；
- CPU/GPU 内存预算通过；
- Windows/Linux GPU 矩阵通过；
- 现有单井、多井、预测结果和导出页面可切换到新引擎；
- Legacy 与新引擎关键数值/语义对等；
- 删除 Legacy 仍需单独评审。

## 9. 跨阶段持续工作

每个阶段都必须同步：

- 单元/性质/快照测试；
- 性能基准；
- fuzz seeds；
- API reference；
- ADR/Manifest migration；
- SBOM 和许可证；
- 示例程序；
- Windows/Linux CI。

不得把性能、安全、Python 生命周期或矢量一致性推迟到最后补做。

## 10. Legacy 迁移映射

| Legacy 能力 | 新架构目标 |
|---|---|
| `native/well_log_core.minmax_downsample` | Core 分块层次 LOD |
| `fast_las_parse_data` | WellLog::IO Source Adapter |
| QPainter `CurveTrack` | Track + Curve Layer + RenderGL |
| QPainter `LithologyTrack` | Interval/Pattern/Text Layers |
| ECharts JSON payload | Strong typed API + Manifest |
| Python `SyncManager` | Session shared viewport |
| `WellSectionCanvas` | Multi-Well Scene |
| QPainter/SVG export | Prepared Scene Export Backend |
| Pydantic `WellLogData` | Paleo Adapter -> WellLogDocument |

迁移原则：

1. 新旧并存；
2. 同输入对比；
3. 按页面切 Feature Flag；
4. 收集性能与诊断；
5. 达标后默认新引擎；
6. 删除旧路径另开变更。

## 11. 主要风险与控制

| 风险 | 影响 | 控制 |
|---|---|---|
| OpenGL 驱动差异 | 崩溃、视觉异常 | 3.3 基线、真 GPU 矩阵、Context 隔离 |
| 多井非线性变换使 LOD 复杂 | 漏点、卡顿 | 分段逆变换、性质测试、局部层级 |
| 字体/中文布局不一致 | 导出错位 | 固定字体指纹、同一 shaping 结果 |
| Python/Qt 所有权 | UAF/双重释放 | Shiboken ownership、GC/重挂载压力测试 |
| Arrow/NumPy 假零拷贝 | 隐性复制、内存超标 | 接入报告、显式转换策略 |
| Pattern 三套实现 | 屏幕/导出不一致 | 矢量定义单一来源 |
| 过早冻结插件 ABI | 长期无法演进 | 首期声明式 Custom Layer |
| XLSX/XML 超大导出 | UI 卡顿、内存爆炸 | 流式写、取消、资源限制 |
| Legacy 一次性替换 | 回归面过大 | Feature Flag 渐进迁移 |

## 12. 实现前必须关闭的具体项

这些不改变已确认架构，但必须在对应阶段开始前定案：

- 支持的 Qt/PySide6 精确版本矩阵；
- 默认 CJK 字体资产、许可证与包体积；
- PDF 实现库及字体子集方案；
- XLSX 流式写库；
- XML namespace/XSD 1.0 细节；
- GL loader 生成工具；
- vcpkg baseline；
- 参考工作站具体硬件、驱动和 OS 镜像；
- Source Adapter 的优先格式顺序；
- 安全资源上限默认数值。

每项应通过小型技术 Spike 或基准选型，不应改变 Core 依赖方向。

## 13. 第一批实施任务

1. 创建 CMake Targets 和 include 依赖测试。
2. 建立 `EntityId`、`Result`、`BufferView`、`SharedOwner`。
3. 编写 Sampling Axis/Curve 验证性质测试。
4. 实现 Document Revision、Patch Conflict、Append 原子测试。
5. 实现 Depth Transform 正逆和随机单调性测试。
6. 创建一亿点合成基准生成器与指标格式。
7. 定义 Prepared Scene 最小稳定语义。
8. 以 SVG 参考后端验证 Track/Layer/Scale。
9. 实现层次 LOD 标量参考版本，再做 SIMD/并行优化。
10. 完成阶段 1/2 API Review 后再开始 Qt/OpenGL。



## 14. Track/Data 工作流（2026-08，ADR 0055/0056/0057）

已完成（本地测试 `welllog.track-commands` / `welllog.qt-table-selection-sync`
/ `welllog.python.track-commands` / `welllog.track-command-benchmark` +
desktop `tests/test_engine_track_commands.py`）：

- 绑定索引：`DocumentBindingIndex`（core）+ `PresentationBindingIndex`
  （scene），O(1) 解析全部 track/curve/scale/layer 绑定问题。
- Session 工作流命令 15 条（track 增删/排序/宽度/header/可见性、曲线
  bind/unbind/move/duplicate/reorder/可见性/样式、scale 编辑与显式
  auto-range），全部走 ApplyPatchCommand；隐藏 track 语义（保槽位、无
  geometry）；`EntityId::generate()`；`session.presentation()` 只读访问；
  `resolve_curve_pick` 悬停检查。
- TableModel 订阅 session selection 事件自动刷新（图形↔表格闭环无需宿主
  轮询）。
- Python：`apply_track_command` / `presentation_state` / `hover_info` /
  `selection_state` / `set_row_selection`。
- Desktop：`capture_engine_bindings` + `incremental_presentation_sync`
  两阶段镜像（结构 + 值），display-set/属性面板/井道头拖拽全部优先走
  增量命令，结构性变化回退整包重发。
- 已知边界：补丁提交沿用的 document/presentation 替换 + 场景重准备成本
  随呈现样本数缩放（20 万样本井约 15ms/次；1000 万样本基准约 0.8s/次，
  命令层本身为 μs 级）——增量场景准备属后续渲染管线工作；桌面 display-set
  模型一曲一线，跨井道移动暂为引擎 API 能力（已测），待桌面模型支持
  多曲线井道后接入。
