# WellLogEngine 总体架构

## 1. 架构结论

WellLogEngine 是有状态、宿主无关的 C++20 SDK。它拥有测井场景和交互语义，但不拥有产品窗口、项目文件、权限和业务工作流。

```text
┌──────────────────────── Host Applications ────────────────────────┐
│ C++ Qt Desktop        PySide6 Workbench       Headless Tools      │
└───────────┬──────────────────┬──────────────────────┬─────────────┘
            │                  │                      │
     welllog-qtwidgets   welllog-python         CLI / service adapter
            └──────────────────┴──────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │   welllog-session   │
                    │ commands / events  │
                    │ state / undo / pick│
                    └──────────┬──────────┘
                               │
          ┌────────────────────┼──────────────────────┐
          │                    │                      │
┌─────────▼────────┐ ┌─────────▼────────┐  ┌─────────▼────────┐
│ welllog-core     │ │ welllog-scene    │  │ welllog-table    │
│ document/buffer │ │ layout/prepare   │  │ projections      │
│ ids/revisions   │ │ text/pattern     │  │ selection        │
└─────────┬────────┘ └─────────┬────────┘  └─────────┬────────┘
          │                    │                      │
          │         ┌──────────▼───────────┐          │
          │         │ Prepared Scene       │          │
          │         └──────┬────────┬──────┘          │
          │                │        │                 │
      ┌───▼────┐   ┌───────▼───┐ ┌──▼─────────┐ ┌────▼─────────┐
      │ IO/store│   │ render-gl │ │ export-vec │ │ export-table │
      │ adapters│   │ OpenGL 3.3│ │ PDF / SVG  │ │ XLSX/XML/CSV │
      └────┬────┘   └───────────┘ └────────────┘ └──────────────┘
           │
    LAS/DLIS/...  NumPy  Arrow C Data  mmap  live append
```

## 2. 模块职责

### `WellLog::Core`

负责：

- Entity ID（含 `EntityId::generate()` 随机 v4）、Document Revision、
  Result/Error；
- DocumentBindingIndex（按 id 的 O(1) 实体解析、curve→axis、
  curves_on_axis，ADR 0056）；
- Buffer View、Null Bitmap、Sampling Axis；
- WellLogDocument、Curve、Interval、Marker、QC Mask、Derived Curve；
- Depth Transform 数学；
- 文档验证和不可变 Patch/Append 契约。

禁止依赖：Qt、Python、OpenGL、Arrow、具体文件格式。

### `WellLog::Session`

负责：

- Multi-Well Scene 的当前状态；
- Track/Layer 布局、视口、Selection Set、活动工具；
- View Command、View Event、撤销/重做；
- Document Patch 暂存与提交事件；
- Track/Data 工作流命令（ADR 0056/0057：track 增删/排序/宽度/可见性、
  曲线 bind/unbind/move/duplicate/reorder、样式与比例尺编辑、显式
  auto-range）——全部构造为 DocumentPatch 走 `ApplyPatchCommand`，无第二
  mutation engine；
- 任务调度、版本校验和 Diagnostic Stream。

### `WellLog::Scene`

负责：

- Track/Layer 布局和水平可见性裁切（隐藏 track 保留槽位、不产生
  geometry/header）；
- PresentationBindingIndex（track→scales/layers、curve→layers、
  layer→track、scale→track 的 O(1) 绑定解析，ADR 0056）与
  `resolve_curve_pick` 悬停检查（inspect.hpp）；
- LOD 查询、区间裁切、标签布局；
- Pattern 与文字整形；
- 空间拾取索引；
- 生成不可变 Prepared Scene 和 Frame Plan。

### `WellLog::RenderGL`

负责：

- OpenGL 3.3 Core 能力检测；
- Shader、VBO/IBO、纹理、Glyph/Pattern Atlas；
- Frame Plan 批处理、Stencil/Mask 与 Overlay；
- GPU 资源预算、Context 恢复和计时查询。

它不解释井、曲线、地层等业务对象。

### `WellLog::Export`

负责：

- PDF、SVG、PNG、TIFF；
- 物理页面、比例尺和深度分页；
- 字体嵌入/轮廓、矢量 Pattern；
- 纯矢量与混合高保真策略。

### `WellLog::Table`

负责：

- Table Projection；
- 同轴宽表、异轴分表、显式重采样；
- Selection Set 与表格行列间映射；
- 流式数据读取接口。

### `WellLog::IO`

负责：

- LAS、DLIS、LIS、716 等 Source Adapter；
- 版本化 Manifest 与便携包；
- Arrow IPC/mmap 存储实现；
- XLSX/XML/CSV 表格写出；
- 不可信文件解析和资源限制。

文件格式扩展不得反向修改 Core。

### `WellLog::Arrow`

通过 Arrow C Data Interface 将 Arrow Array 转为 Core Buffer View。该模块可选构建。

### `WellLog::QtWidgets`

负责：

- 原生 `WellLogView : QOpenGLWidget`；
- GUI 线程 GL Context 生命周期；
- Qt 输入事件到 View Command 的归一化；
- Qt 信号与 View Event 的桥接；
- `QAbstractTableModel` 适配。

### `WellLog::Python`

使用 Shiboken6：

- 暴露 PySide6 兼容的 Widget/QObject；
- 转换 NumPy、Arrow 与 Python 所有权；
- 映射 Result、异常和 Qt 信号；
- 在耗时同步操作中释放 GIL。

## 3. 依赖规则

| 调用方 | 可以依赖 | 不得依赖 |
|---|---|---|
| Core | 标准库、最小公共工具 | Qt、Python、OpenGL、Arrow、IO |
| Session | Core、Scene、Table 接口 | Qt、Python、具体文件格式 |
| Scene | Core、文字/图纹内部服务 | Qt Widget、Python |
| RenderGL | Prepared Scene、OpenGL loader | WellLogDocument 业务查询、Qt |
| Export | Prepared Scene、字体/图纹 | OpenGL Context、Qt Widget |
| IO/Arrow | Core | Session、RenderGL |
| QtWidgets | Session、RenderGL、Qt 6 | Python |
| Python | QtWidgets、Core 适配、Shiboken | 直接发出 GL 调用 |

依赖检查应作为 CMake Target 和 include-path 测试执行。

## 4. 状态与事件

一个 Session 是唯一可变状态所有者：

```text
Host/User Input
      │
      ▼
  View Command ── validate ── apply ── new State Version
                                      │
                   ┌──────────────────┼──────────────────┐
                   ▼                  ▼                  ▼
             View Event        Invalidation       Background Tasks
                                                       │
                                             tagged immutable result
                                                       │
                                      current version? ─┤
                                          yes: apply   no: discard
```

约束：

- Host 不维护第二份 viewport/selection/layout 镜像。
- Command 明确成功、拒绝或冲突。
- Event 是状态变化后的事实，不是修改请求。
- 高频 Hover/Profiler 数据先在 C++ 合并，再低频跨 Python。

## 5. 线程模型

### GUI 线程

- 拥有 QOpenGLWidget 和主 OpenGL Context；
- 执行所有 GL API 调用；
- 应用已完成的后台结果；
- 每帧按预算上传 GPU 数据；
- 处理 Qt 输入并调度 `update()`。

### C++ 工作线程池

- LOD 摘要和视口查询；
- 区间/空间索引；
- 布局、文字整形和几何预处理；
- 表格导出与 CPU 矢量准备；
- 过期任务取消。

首期不创建后台共享 OpenGL Context。每个后台结果必须带输入版本集合，GUI 线程只接受仍然匹配的结果。

## 6. Prepared Scene

Prepared Scene 是屏幕和导出的共同中间语义，包含：

- 物理布局与裁切层级；
- 已映射或可映射的曲线/区间几何；
- 文字的字形与位置；
- Pattern Definition 引用与锚点；
- Z 顺序、样式、语义 Entity ID；
- 拾取边界和导出提示。

它不包含 Qt 对象、Python 对象、原始业务查询回调或特定 GL Handle。

Frame Plan 将 Prepared Scene 转为有序 Pass：

1. 背景与网格；
2. 区间、图像和填充；
3. 曲线；
4. 文本和符号；
5. Selection、Cursor、Cross-Well Overlay；
6. 可选诊断与 Profiler。

## 7. 失效模型

| 变化 | 最小失效范围 |
|---|---|
| 纯视口平移且仍在预取范围 | 变换矩阵、Overlay |
| 跨过 LOD 缓存边界 | 对应曲线可见查询 |
| Track 宽度 | 该 Track 布局、Scale、文字、裁切 |
| Curve Revision | 该曲线 LOD 与依赖填充 |
| Depth Transform | 对应井的纵向几何、标签、拾取 |
| Pattern 样式 | 引用该 Pattern 的 Layer/Atlas |
| 字体变化 | 相关文本布局与 Glyph Atlas |
| Selection Set | Selection Pass |
| Document Patch | Patch 影响图中的最小依赖闭包 |

任何失效都必须可以通过 Entity ID 和版本解释，不能使用“全部重建”作为日常路径。

## 8. 兼容与扩展

- SDK 使用语义化版本。
- Patch 保 C++ ABI，Minor 保源码兼容，Major 才破坏。
- 公共类使用 PIMPL 和隐藏符号。
- Custom Layer 只能提交声明式原语。
- 内部 Renderer 扩展只保证同一源码/工具链，不承诺第三方二进制 ABI。
- Manifest Schema 独立版本化并支持当前及前两个版本迁移。

## 9. OpenGL 不可用

初始化生成 Capability Report。若 OpenGL 3.3 Core 不可用：

- 图形 Widget 进入明确不可用状态；
- Table Projection、复制和 CPU 矢量导出继续工作；
- 不静默使用 QPainter；
- Context 丢失时从 Prepared Scene 和 CPU 缓存重建；
- 连续恢复失败隔离当前 View，不终止宿主进程。

## 10. 相关决策

全部已确认 ADR 见 [架构决策索引](decision-log.md)。

