---
status: accepted
---

# 声明式 Custom Layer 数据模型与原语分解

Custom Layer 以纯数据原语（折线、三角形、矩形、符号）和可选的层局部裁切路径表达宿主自定义专业 Layer，原语由内核统一分解进现有 PreparedScene 流，复用已有 GL/SVG/拾取管线；扩展不直接发出渲染调用，也不承诺第三方 C++ 二进制插件 ABI。

## 背景

ADR 0018 确立首期扩展接口为声明式 Custom Layer，禁止扩展直接修改渲染状态或提交 Shader/脚本/网络资源；ADR 0042 要求 Pattern 与 Custom Layer 视为不可信输入，按尺寸/数量/几何合法性拒绝。本 ADR 落实 #153（rendering.md 第 11 节）的具体数据模型与渲染分解方式。

## 决策

- **原语为纯数据，约束由类型系统强制**。`CustomPrimitive` 为 `std::variant<CustomPolyline, CustomTriangle, CustomQuad, CustomSymbolOccurrence>`，仅含场景毫米点与颜色；不存在 Shader/脚本/命令/网络字段，因此 ADR 0042 的"不接受脚本/Shader/命令"约束在类型层面成立，而非依赖解析期规则。
- **原语归属 Document，Presentation 仅引用**。`CustomLayerSource`（`EntityId` + `content_revision` + 原语向量 + 可选 `CustomClipPath`）注册到 `WellLogDocument`，`CustomLayerSpec` 仅以 id 引用——与 #152 的 `ImageSource`/`ImageLayerSpec` 同构，便于后续 manifest 往返与按内容版本失效。
- **内核分解原语进共享流，不新增 GL batch kind**。`PrimitiveKind`（solid/pattern/glyph/image）是渲染器私有；Custom Layer 不引入第五种。准备期将折线点存入 `custom_vertices` 并记一条 `PreparedCustomPrimitive`，三角形/矩形/符号同理；GL 上传循环把每条原语拆成现有 `solid` 三角形（折线按法线偏移生成带状四边形，符号复用 `append_symbol_geometry` 圆扇），SVG 直接发射 `<path>`/`<polygon>`/圆弧。两端消费同一份场景毫米数据，语义一致。
- **裁切为层局部**。`CustomClipPath` 只掩蔽本 Custom Layer 自身原语，不触碰 track 裁切与其它 Layer。命中测试先做点在多边形包含；SVG 不强制 `<clipPath>`（原语已按裁切几何外不命中丢弃，简化为按层选择）。
- **资源限制在准备期局部拒绝**。空原语集返回 `invalid_custom_source` + `custom_source_empty`；原语数超上限返回 `custom_source_primitives_exceed_limit`；顶点总量超上限返回 `custom_source_points_exceed_limit`；几何非有限/不合法返回 `invalid_presentation`。与 #151/#152 同构，拒绝以 `prepare` 返回的致命 `Error` 表达（引擎的 session-level `Diagnostic` 通道保留给非致命的 value/text 问题）；"局部"指失败隔离到当前 source 的几何校验，不产生部分场景。
- **层局部裁切在准备期执行**。当 `CustomLayerSource` 声明 `CustomClipPath` 时，准备期用 Sutherland–Hodgman（`detail::clip_polygon_to_polygon`）把三角形/矩形裁切到该路径并重新三角化，折线按段剔除两端皆外的线段，符号按中心点剔除。GL/SVG/拾取因此看到同一份已裁切几何，无需 per-backend 多边形裁切或 stencil shader。
- **拾取返回原语身份**。`pick_custom` 按 z 序逆序命中最近的可见原语，返回 `layer_id`/`source_id`/`source_primitive_index`/`kind` 与从场景毫米反演的 `reference_depth`；宿主可按索引从自身 source 取回自定义拾取 metadata。
- **不承诺第三方 C++ 二进制插件 ABI**（ADR 0018）。接口为 C++ 结构体 API；高性能专用 Layer 可随源码或同工具链编译，闭源二进制插件若成真实需求再以版本化 C ABI 单独设计。

## 后果

- 新增一种 Layer 不需要修改 shader、程序或 `PrimitiveKind`；扩展作者只提交纯数据。
- 层局部裁切在准备期一次性完成（CPU Sutherland–Hodgman + 重三角化），因此 GL/SVG/拾取几何一致，且裁切后的三角形数可变；`GpuUploadSchedule::custom_triangle_count()` 按 `vertex_count/3` 精确统计三角形/矩形（符号仍按圆形扇 24 近似，非圆形符号的精确计数与 SVG 一致性留作后续）。
- 折线在 GL 以双三角带状四边形近似描边（按段法线偏移），与曲线 LOD 描边语义不完全一致（曲线走独立 `CurveBatch` 带线宽 shader），但对自定义标注级折线足够；若需逐段斜接，后续可引入共享描边器。折线在层局部裁切下按段剔除两端皆外线段，段内裁切留作后续。
- 自定义顶点字节的 GPU 预算核算尚未并入 `GpuUploadSchedule::total_bytes()`（曲线边字节校验是独立路径）；当前以准备期硬上限与 `custom_triangle_count()` 核对覆盖，完整字节预算集成留作后续。
- 自定义图像/文字原语不在本期范围：宿主已有 `ImageLayer`（#152）与 `TextLayer`（#150）承担；本期 Custom Layer 不重复其纹理缓存/字形管线。
- `CustomLayerSource` 的 manifest 序列化与 schema 版本升级留作后续（与 #152 的 manifest 缺口同性质）。
