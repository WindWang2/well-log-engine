# 数据模型与 API 契约

## 1. 文档状态

本文定义实现必须遵守的语义契约。类型名和函数名是首版头文件的建议形状，在 API Review 前不构成已发布 ABI。

## 2. 身份、版本与所有权

### 2.1 持久身份

以下对象必须具有 128 位 `EntityId`：

- WellLogDocument / Well；
- Sampling Axis；
- Curve、QC Mask、Derived Curve；
- Interval、Marker、Annotation；
- Track、Layer、Track Scale；
- Pattern Definition；
- Cross-Well Overlay。

显示名称、mnemonic、数组地址、列表序号和运行时句柄均不得替代 Entity ID。

### 2.2 运行时句柄

Session 可将 Entity ID 映射为 32 位 `RuntimeHandle`，用于：

- GPU 批次；
- 空间索引；
- Frame Plan；
- 拾取临时结果。

Runtime Handle 的作用域不超过 Session 生命周期，不进入 Manifest、Document Patch 或 Python 业务 API。

### 2.3 Document Revision

Revision 标识一次不可变提交后的完整文档状态。下列操作产生新 Revision：

- 曲线或区间替换；
- Derived Curve/QC Mask 增删；
- Append Batch；
- 已提交解释 Patch；
- 持久化 Presentation Profile 变化。

纯 Hover、临时 Selection、当前 Viewport 和未提交交互不产生 Document Revision，它们只产生 Session State Version。

## 3. 缓冲区契约

### 3.1 建议接口

```cpp
// 契约草案，不是已冻结头文件。
enum class ScalarType : uint8_t {
    Float32,
    Float64,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64
};

struct BufferView {
    const std::byte* data;
    uint64_t length;
    uint32_t strideBytes;
    ScalarType type;
    NullBitmapView nulls;
    SharedOwner owner;
};
```

`SharedOwner` 是核心可复制的不透明生命周期令牌。它可以封装：

- `std::shared_ptr` 原生数组；
- mmap 区域；
- Arrow Array release callback；
- 由 Shiboken 适配器保持的 Python 对象。

### 3.2 硬约束

- 不接受没有生命周期保证的裸指针。
- 提交后数据只读。
- `data + (length - 1) * strideBytes` 的计算必须检查溢出。
- Null Bitmap 长度必须覆盖 Buffer。
- 适配器不得在没有明确告知调用方时进行类型转换或复制。
- Engine 必须能报告某次接入是 Zero Copy、Shared Copy 还是 Converted Copy。

## 4. Sampling Axis

```cpp
struct SamplingAxis {
    EntityId id;
    BufferView coordinates;
    DepthDomain domain;       // MD, TVD, TVDSS, or source index domain
    Unit unit;
    AxisDirection direction;  // increasing or decreasing
};
```

规则：

- 坐标必须单调递增或递减。
- 相邻坐标可重复，重复项保持原序。
- 局部乱序由 Source Adapter 拆成多个单调数据段。
- 多条曲线可共享同一个 Sampling Axis，避免重复深度数组。
- Sampling Axis 的身份不同，即使数值碰巧相同，也不得自动视为同轴；适配器可显式去重。

## 5. 曲线与数据质量

```cpp
struct Curve {
    EntityId id;
    std::string mnemonic;
    std::string displayName;
    Unit unit;
    EntityId samplingAxisId;
    BufferView values;
    std::optional<EntityId> qcMaskId;
    CurveProvenance provenance;
};
```

### 5.1 Null 与非有限值

- Null Bitmap 是显式缺测来源。
- NaN 和 Infinity 无论 Bitmap 如何均不可绘制。
- 缺测中断折线、填充和插值区间。
- 对数 Scale 中 `value <= 0` 不绘制并增加诊断计数。

### 5.2 QC Mask

QC Mask 与源曲线等长，至少支持：

- Valid；
- Suspect；
- Invalid；
- UserExcluded。

具体位分配在 Schema Review 时确定。Mask 不修改值；显示策略决定是否隐藏、着色或标注不同状态。

### 5.3 Derived Curve

Derived Curve 必须记录：

- 输入 Curve Entity ID 与 Revision；
- 算法标识和版本；
- 参数；
- 输出 Sampling Axis；
- 创建者/时间等可选审计元数据。

若输入 Revision 不再匹配，Derived Curve 进入 Stale 状态而不是静默继续冒充最新结果。

## 6. 区间、Marker 与注释

```cpp
struct Interval {
    EntityId id;
    double topReferenceDepth;
    double bottomReferenceDepth;
    IntervalSemantic semantic;
    PropertyMap properties;
};

struct Marker {
    EntityId id;
    double referenceDepth;
    MarkerSemantic semantic;
    PropertyMap properties;
};
```

规则：

- Interval 必须满足 `top <= bottom`。
- `top == bottom` 不保存为 Interval，使用 Marker。
- 超出井范围的合法对象可以存在，视口只裁切可见部分。
- 反向区间作为输入错误拒绝。
- 文本 Annotation 可锚定 Reference Depth、Track、Curve Sample 或场景位置，锚定类型必须显式。

## 7. WellLogDocument

```cpp
struct WellLogDocument {
    EntityId id;
    DocumentRevision revision;
    WellMetadata metadata;
    Collection<SamplingAxis> axes;
    Collection<Curve> curves;
    Collection<QcMask> qcMasks;
    Collection<Interval> intervals;
    Collection<Marker> markers;
    Collection<Annotation> annotations;
    Collection<PatternDefinition> patterns;
    std::optional<PresentationProfile> defaultPresentation;
};
```

一份 Document 只表示一口井。井轨迹换算和 MD/TVD/TVDSS 映射可以作为不可变 Transform 资源附加，但轨迹计算算法不属于 Render Core。

`defaultPresentation` 是建议初始布局；Session 可拥有未提交的布局覆盖。宿主显式提交后，布局变更才成为 Document Patch/Revision。

## 8. Track、Layer 与 Scale

实现词汇（include/welllog/scene/scene.hpp）：`TrackSpec { id, width,
z_order, header, visible }`、`TrackScaleSpec { id, track_id, mode, minimum,
maximum, direction, unit }`、`CurveLayerSpec { id, track_id, curve_id,
scale_id, color, line_width, z_order, visible, qc_display }` 以及
interval / crossover fill / image / marker / symbol / text / custom 等
Layer 类型。历史草图中 "track 持有 scaleIds/layers 数组" 的形态已被否决：
Track 与 Curve 的绑定事实只存在于 Presentation 的平铺 Layer 集合
（ADR 0055），引擎内不存在第二份 `Track.curves[]`。

`TrackSpec.visible == false` 的隐藏 track 保留自己的布局槽位与全部绑定
（preflight 仍完整校验），但其所有 Layer 与 header 行不产生任何 prepared
geometry——"灰显不重排" 策略。

Layer 是带语义类型的 tagged variant，不是继承自 QWidget 的多态树。首期内置类型：

- Grid Layer；
- Curve Layer；
- Interval Layer；
- Fill Layer；
- Pattern Layer；
- Image Layer；
- Text Layer；
- Symbol Layer。

Custom Layer 只提交受控绘图原语及语义拾取信息。

每个 Curve Layer 显式引用 Curve Entity ID 和 Track Scale Entity ID。不得以当前可见最小/最大值临时自动归一化。

## 9. 深度模型

### 9.1 变换链

```text
Sampling Axis
  -> Reference Depth
  -> per-well Depth Transform
  -> Display Depth
  -> viewport-relative coordinate
  -> Screen Y
```

### 9.2 Depth Transform

Depth Transform 是严格单调、可逆的分段映射：

```cpp
struct DepthControlPoint {
    double referenceDepth;
    double displayDepth;
};
```

约束：

- 控制点按 Reference Depth 严格有序；
- Display Depth 也必须保持同方向单调；
- 区间内首期使用线性插值；
- 区间外的外推策略必须显式配置；
- 逆变换在有效域内唯一；
- 违反单调性时整个 Command 被拒绝。

整体基准平移是两个控制点具有相同偏移的特例。

## 10. Multi-Well Scene

```cpp
struct WellPlacement {
    EntityId documentId;
    PhysicalLength left;
    PhysicalLength width;
    DepthTransform depthTransform;
    Visibility visibility;
};

struct MultiWellScene {
    std::vector<WellPlacement> wells;
    std::vector<CrossWellOverlay> overlays;
};
```

Multi-Well Scene 在一个 Session 和一个 Render Surface 中存在。垂直 Viewport 共享 Display Depth 域；每口井可以有独立 Depth Transform 和 Track 组。

### 10.1 统一 Surface Canvas（ADR 0055）

单井是 one-placement Surface 的特例：无显式 layout 时 `prepared_surface_scene()` 解析隐式单井 surface（focused well，否则唯一已 prepare 的文档），`pick_surface_curve()` 走同一路径。Surface 交互状态：

- `SetFocusedWellCommand` / `focused_well()`：focused well 是 engine 持有的交互状态；
- `PanDepthCommand` / `ZoomDepthAtCommand` / `ResetViewportCommand`：layout 成员上委托 shared Display Depth 视口（单井 per-document，同一条命令）；
- `SetSurfaceHorizontalViewCommand` / `PanSurfaceHorizontalCommand`：水平窗口（surface 毫米）与带边界 clamp 的平移；
- `surface_depth_viewport()` / `surface_crosshair()` / `surface_width_mm()` / `surface_horizontal_view()` / `surface_statistics()`：单井与连井共用的统一访问器（visible/culled wells/tracks 虚拟化计数）。

Compose cache 键只钉真实输入（placements + 高度 + layout/overlay generation）：水平平移不改变剔除集合时组合场景指针不变，GPU 资源全量复用。

## 11. Session、Command 与 Event

### 11.1 Session 持有

- 当前 Document Revision 集合；
- Multi-Well Scene；
- Viewport 与 Effective Screen Scale；
- 工作 Presentation Profile；
- Selection Set；
- 活动 Tool；
- 撤销/重做；
- 任务、缓存和诊断状态。

### 11.2 Command 示例

- `SetDocuments`
- `ApplyDocumentPatch`
- `AppendBatch`
- `SetViewport`
- `PanDepth`
- `ZoomDepthAt`
- `SetTrackWidth`
- `ReorderTrack`
- `SetLayerVisibility`
- `SetTrackScale`
- `SetDepthTransform`
- `SetSelection`
- `BeginEdit` / `CommitEdit` / `CancelEdit`
- `Undo` / `Redo`

每个 Command 返回 `Result<CommandReceipt, Error>`。Receipt 至少包含新状态版本、是否引发异步准备和可能的诊断 ID。

### 11.3 Event 示例

- `DocumentsChanged`
- `ViewportChanged`
- `PresentationChanged`
- `SelectionChanged`
- `HoverChanged`（合并/限频）
- `PatchCommitted`
- `PatchConflict`
- `FrameReady`
- `CapabilityChanged`
- `CachePressure`
- `DiagnosticPublished`
- `FatalViewError`

Event 只陈述应用后的事实，不能要求订阅者回写同一个状态。

## 12. Selection Set

Selection 是 tagged variant，可包含：

- Reference Depth Range；
- Curve Sample Range；
- Interval/Marker/Annotation Entity ID；
- Track/Layer Entity ID；
- Cross-Well Overlay Entity ID。

精确样点至少包含：

```text
Document ID + Revision + Curve ID + Sample Index
```

图形框选、表格选区和 Copy 命令都映射到同一个 Selection Set。复制读取原始 Buffer，不读取 LOD 结果。

## 13. Document Patch

Patch 必须声明：

- Patch ID；
- 基础 Document Revision；
- 操作序列；
- 受影响 Entity ID；
- 可选审计元数据。

首期 Patch 操作覆盖：

- Add/Replace/Remove 解释对象；
- Add/Replace Derived Curve 或 QC Mask；
- 修改 Presentation Profile；
- 修改 Depth Transform 资源；
- 修改 Cross-Well Overlay。

若基础 Revision 不匹配，返回 Conflict；内核不得按名称或当前位置自动套用。未来的三方合并属于独立模块。

### 13.1 Track/Data 工作流命令（ADR 0055/0056）

`include/welllog/session/track_commands.hpp` 在 Document Patch 之上提供专业
工作流命令：`AddTrack` / `RemoveTrack`（级联移除 scale+layer 的原子补丁）/
`ReorderTracks` / `ResizeTrack` / `SetTrackHeader` / `SetTrackVisibility`、
`BindCurveToTrack` / `UnbindCurveFromTrack` / `MoveCurveLayer` /
`DuplicateCurveLayer` / `ReorderCurveLayers` / `SetCurveLayerVisibility` /
`SetCurveLayerStyle`、`SetTrackScale` / `AutoRangeTrackScale`。每条命令通过
绑定索引解析当前 document+presentation，构造一条基于当前 revision 的
DocumentPatch 并委托给 `ApplyPatchCommand`——没有第二套 mutation engine；
原子性、revision 门、preflight、undo/redo、事件与 LOD 复用全部继承。
Move 只改写 layer 的 track/scale 绑定与 z-order，原始曲线与深度缓冲区
地址不变（benchmark `welllog.track-command-benchmark` 以地址/容量不变为
硬门）。Auto-range 是显式操作（log 刻度拟合正有限值域），任何比例尺都不
随可见 min/max 逐帧变化。配套稳定错误键：`track_entity_missing`、
`track_binding_invalid`、`track_order_incomplete`、
`track_scale_range_invalid`。

绑定索引（O(1) 解析、不触碰原始缓冲区）：core 的
`DocumentBindingIndex`（按 id 定位实体 + `curves_on_axis` 文档序）与
scene 的 `PresentationBindingIndex`（z 序 track 列表、track→scales/
layers、curve→layers、layer→track 全类型、`all_layers_of_track` 级联集）。
悬停检查用 `scene/inspect.hpp` 的 `resolve_curve_pick` 得到 CurvePickInfo
（mnemonic/单位/QC 状态/scale 上下文/derived provenance 与 staleness）。

## 14. Append Batch

Append Batch 是一组曲线尾块的原子提交：

- 所有 Curve 必须存在；
- 新坐标延续原 Sampling Axis 的方向；
- Buffer 均有所有权令牌；
- 批次要么全部应用，要么全部失败；
- 应用后创建新 Revision；
- 旧块不复制；
- 尾部 LOD 摘要增量更新。

历史回补和乱序数据必须用 Replace/Patch，不得伪装 Append。

## 15. Result、Error 与诊断

同步 API 不允许异常跨模块边界：

```cpp
template<class T>
class Result;

struct Error {
    ErrorCode code;
    Severity severity;
    std::optional<EntityId> entity;
    MessageKey message;
    PropertyMap arguments;
};
```

输入错误、版本冲突和能力不足通过 Result 返回。持续运行中的局部问题进入 Diagnostic Stream。

渲染帧、析构、Qt 回调和 Python/C 边界不得传播异常。`std::bad_alloc` 等不可恢复异常也必须在最外层边界转为 View 隔离或进程既定故障策略。

## 16. Manifest

Manifest 使用版本化 JSON Schema，只保存：

- 文档/实体身份；
- 数据源 URI 或宿主资产引用；
- Buffer 描述和校验摘要；
- Presentation、Pattern、Patch 与 Session 状态；
- Schema/SDK 版本要求。

大型数值数组不得内嵌为 JSON 数字列表。XLSX/XML/CSV 也不得作为 Manifest 替代品。

