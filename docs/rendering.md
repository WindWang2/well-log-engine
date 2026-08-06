# 渲染管线设计

## 1. 目标

渲染管线必须在 4K、一亿总样点、100 个可见 Track 的基准场景中保持稳定交互，同时保证极值、缺测、深度、图纹和导出语义正确。

首期唯一屏幕后端是 OpenGL 3.3 Core。OpenGL 4.4/4.5 特性仅能作为结果一致的增强路径。

## 2. 坐标精度

逻辑计算使用 `double`：

- Reference Depth；
- Display Depth；
- Track 物理布局；
- PDF/SVG 页面坐标；
- 拾取逆变换。

进入 GPU 前：

1. 以当前 Viewport 中心为原点；
2. 将可见坐标减去中心；
3. 转为 `float`；
4. 使用矩阵映射到 NDC。

该策略避免 8,000 m 井深与 0.125 m 采样同时存在时的浮点抖动。

## 3. 帧生命周期

```text
Command/Event
    │
    ▼
Versioned Invalidation
    │
    ├─ immediate state-only update
    └─ background prepare task
             │
             ▼
      immutable prepared result
             │ current?
             ├─ no: discard
             └─ yes: merge into Prepared Scene
                        │
                        ▼
                    Frame Plan
                        │
             budgeted upload + draw
```

渲染按需调度。没有动画、交互或新结果时，不持续空转 60 FPS。

## 4. 曲线 LOD

### 4.1 为什么不使用逐帧全量 Min-Max

每次 Viewport 变化扫描全部样点会把缓存未命中转化为 UI 卡顿。LTTB 也不适合作为工程曲线主路径，因为它不能保证局部极值与缺测边界保真。

### 4.2 分块层次摘要

建议叶块覆盖约 256 个源样点。每个连续有效段的摘要至少包含：

- first sample；
- minimum sample；
- maximum sample；
- last sample；
- 上述样点的源索引与 Reference Depth；
- 有效/缺测边界信息。

输出点必须按源索引排序并去重。高层节点合并子节点，继续保留全局 first/min/max/last 及断点摘要。

叶块大小应通过基准调整，不写死为公开格式。

### 4.3 查询

对每个可见 Curve Layer：

1. 将 Display Depth Viewport 按 Depth Transform 分段逆变换到 Reference Depth；
2. 二分定位源 Sampling Axis 范围；
3. 根据局部显示像素密度选择摘要层级；
4. 目标是一个摘要块约覆盖 0.5–1 个垂直物理像素；
5. 收集按源顺序排列的 M4 点和断点；
6. 放大到足够精细时直接使用原始点；
7. 对 Track Scale 映射并生成屏幕空间线段实例。

非线性 Depth Transform 可使同一曲线可见范围内使用不同 LOD 层级。

### 4.4 缓存键

LOD 查询结果至少依赖：

- Curve Entity ID 与 Revision；
- Sampling Axis Revision；
- QC Mask Revision；
- Depth Transform Version；
- Viewport Range；
- 物理像素高度；
- Scale clipping policy。

过期查询必须可取消。

## 5. 曲线 GPU 绘制

不依赖实现不一致的宽 `GL_LINE_STRIP`。建议把曲线表示为屏幕空间线段实例或扩展三角形：

- 每实例包含端点、宽度、颜色、样式和断点标记；
- Vertex Shader 在屏幕空间扩展线宽；
- Join/Cap 使用受控策略；
- 虚线相位基于连续深度/路径长度，不能随分块跳变；
- Track 使用 Scissor，复杂边界使用 Stencil；
- 同 Shader/Blend/Texture 状态的曲线跨 Layer 批处理。

纯平移且仍在预取范围时，只更新 Transform/Uniform；不重新上传全部顶点。

## 6. Scale 与曲线交会

Curve 值先通过各自 Track Scale 映射到归一化横坐标。线性、对数和反向刻度在 CPU/GPU 必须共享同一数学定义。

Curve Crossover Fill 的边界由两条曲线映射后的横坐标决定：

- 两曲线可有不同单位和 Scale；
- 缺测任一侧时填充中断；
- 在可见深度片段内检测符号变化并求交点；
- 使用足以不遗漏交点的 LOD/摘要信息；
- 生成填充 Mask 或边界几何；
- Pattern/Color 通过 Stencil 区域裁切。

不能仅比较原始数值，也不能假设两条曲线具有同一 Sampling Axis。异轴情况需要在共同深度片段中进行显式、局部插值，且不能越过缺测区。

## 7. Interval、Marker 与 Overlay

- Interval 按 Display Depth 裁切为矩形或声明式形状。
- Marker 在所有缩放级别保持零厚度语义，线宽使用屏幕交互单位或文档物理单位，取决于样式角色。
- Cross-Well Overlay 在井布局完成后计算，引用多井变换结果。
- 井间连接带和层位线在同一 Render Surface 中绘制，保证无 Widget 边界断裂。
- Overlay 的 Z 顺序必须显式，不能依赖注册顺序偶然决定。

## 8. Pattern Fill

### 8.1 Pattern Definition

唯一来源是受限矢量重复单元：

- 线、折线、圆/弧、点和封闭填充；
- 单元物理宽高；
- 前景/背景角色色；
- 场景锚点；
- 可选旋转。

不接受 Shader 源码、脚本或外部网络资源。

### 8.2 屏幕

- Pattern Definition 栅格化到按物理密度分桶的 Atlas；
- 可选 SDF/距离场仅作为质量优化；
- Fragment Shader 根据场景锚点计算平铺坐标；
- Stencil/Mask 限定 Interval 或曲线边界；
- 相邻区间共享相位，滚动不改变 Pattern 原点。

### 8.3 导出

PDF/SVG 直接使用原始矢量单元与物理尺寸。GPU Atlas 不是导出事实来源。

## 9. 文字

文字管线：

```text
UTF-8
 -> ICU segmentation / line breaking
 -> font fallback resolution
 -> HarfBuzz shaping
 -> positioned glyph run
 -> Prepared Scene
 -> Glyph Atlas or vector font output
```

要求：

- Font Resolver 顺序为项目字体、内置回退、系统字体；
- 缓存键包含字体文件指纹、Face、字号、方向、语言和特性；
- 支持水平、旋转和真正的竖排布局；
- 缺失字形产生诊断；
- 每个工作线程使用独立 ICU BreakIterator；
- 屏幕与导出复用相同 glyph positions；
- Atlas 按 Context Share Group 管理。

参考：

- [HarfBuzz shaping](https://harfbuzz.github.io/getting-started.html)
- [ICU Boundary Analysis](https://unicode-org.github.io/icu/userguide/boundaryanalysis/)

## 10. 图像 Layer

用于岩心照片、井壁成像等源栅格内容：

- 读取元数据后按可见深度分块；
- 大图使用多分辨率金字塔；
- 只解码和上传可见瓦片及有限预取；
- 图像尺寸、压缩比和像素总量受安全上限；
- 不允许因导出请求一次性解码超大完整图像；
- 矢量导出中保持为有明确 DPI/色彩空间的栅格对象。

## 11. 拾取

拾取使用 CPU 语义索引：

- Track：有序水平边界；
- Interval/Marker：深度区间索引；
- 文本/符号/Overlay：BBox 或 R-Tree；
- Curve：当前 LOD 最近线段，再回查源样点窗口。

命中结果：

- Entity ID；
- hit kind；
- Reference Depth；
- Display Depth；
- 原始值/样点索引（若适用）；
- 像素距离。

Hover 禁止同步 `glReadPixels`。未来 GPU ID Buffer 只能是异步后备。

## 12. GPU Pass 与状态

建议 Pass：

1. Clear/Background；
2. Grid；
3. Image；
4. Interval/Fill/Pattern；
5. Curve；
6. Text/Symbol；
7. Cross-Well Overlay；
8. Selection/Cursor；
9. Profiler。

每个 Pass 显式设置所需 GL 状态，不假定 `initializeGL()` 状态永久保留。QOpenGLWidget 使用自己的 FBO，不能绑定 framebuffer 0；必须使用当前默认 FBO。

参考：[QOpenGLWidget 文档](https://doc.qt.io/qt-6/qopenglwidget.html)。

## 13. 上传与缓冲

- GUI 线程执行全部 GL 上传。
- 每帧上传有固定时间/字节预算。
- 动态几何使用双重或三重轮换 Buffer。
- GL 3.3 路径可使用 orphaning + `glBufferSubData`。
- GL 4.4+ 可选 persistent mapping，但结果和资源生命周期必须一致。
- 不为每个 Layer 创建独立 VBO；按资源类型和更新频率聚合。
- 全部原始曲线不得常驻 GPU。

## 14. 缓存与内存

默认：

- CPU 派生缓存 ≤ 原始缓冲区 25%；
- GPU 缓存 ≤ `min(可查询显存 20%, 512 MB)`；
- 无显存查询时为 256 MB；
- 上下各约两个 Viewport 预取。

成本 LRU 需要考虑：

- 重建 CPU 时间；
- 上传字节；
- 最近可见性；
- 是否被多个 View 共享；
- 当前交互方向。

内存压力顺序：

1. 缩小预取；
2. 淘汰不可见 GPU 几何；
3. 淘汰可重建 CPU 查询结果；
4. 保留层次摘要和当前可见结果；
5. 发布 Cache Pressure。

## 15. Context 生命周期

- GL 资源只在 Context current 时创建/销毁。
- 监听 `aboutToBeDestroyed` 并显式 cleanup。
- Widget 重挂载或 Context 丢失后，GPU Handle 全部视为失效。
- Prepared Scene、CPU LOD 和源 Buffer 保持有效，用于重建。
- 同顶层 Widget 的 Share Group 可共享字体/图纹资源。
- 连续恢复失败将 View 隔离，不终止宿主。

## 16. 性能指标

每帧至少统计：

- prepare CPU time；
- upload CPU time/bytes；
- draw submit time；
- GPU pass time（异步查询）；
- batches、vertices/instances；
- LOD level distribution；
- cache hits/misses/evictions；
- worker queue/cancel counts；
- frame P50/P95/P99。

详细 Trace 兼容 Chrome Trace Event 格式，默认关闭。

