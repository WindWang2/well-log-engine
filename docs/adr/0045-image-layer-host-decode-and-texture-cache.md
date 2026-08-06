---
status: accepted
---

# 图像 Layer 由宿主解码并按预算缓存纹理

大型栅格源（岩心照片、井壁成像）作为 ImageSource 注册到 Document，仅声明尺寸、深度范围、DPI、色彩空间与数据源身份；像素字节不由引擎持有或解码。引擎构建多分辨率金字塔、按可见深度加预取选择瓦片、在 GPU 预算内 LRU 缓存与淘汰纹理，并在上下文丢失后从 CPU 元数据与宿主解析器重建。SVG/PDF 导出保留为带明确物理尺寸与 DPI 的栅格对象。

## 背景

rendering.md 第 10 节要求图像按可见深度分块、多分辨率金字塔、只解码上传可见瓦片及有限预取、尺寸/压缩比/像素总量受安全上限；ADR 0042 要求图片按限制解码。引擎现有曲线/轴/空值位图均已采用非拥有、宿主供给字节的模型（ADR 0032）。

## 决策

- **不在引擎内解码图像**。引擎新增 `ImageSource`（含 `BufferSourceReference` 身份）与 `ManifestResolvers::image_tile` 解析缝；宿主按 `ImageTileRequest{source,level,row,col}` 按需返回已解码 `RasterTile` 字节。与曲线 BufferView 同构，避免引入 PNG/JPEG 编解码依赖与解压炸弹面。
- **多分辨率金字塔在内核一次性计算**。`ImagePyramid::build` 只由元数据推导层级/瓦片网格（不解码），`query` 按 viewport 密度与 `prefetch_viewports` 选择可见瓦片；预算耗尽则降级到更粗层级（ADR 0034）。
- **GPU 纹理按预算 LRU**。`GlRenderer` 维护按 `(source,level,row,col)` 索引的纹理缓存与 `frame_stamp` 计时；新增瓦片前淘汰最久未用的离屏瓦片，总量不超过 `PerformanceBudgets.maximum_image_texture_bytes`。
- **上下文丢失可重建**。`release()`/`abandon()` 删除/清零纹理名与缓存；下一帧从保留的 `PreparedImageTile` 元数据重新经解析器请求并上传（ADR 0033：从 PreparedScene + CPU 状态恢复）。
- **资源限制在准备期拒绝**。每边像素上限、总像素上限、DPI/深度合法性校验失败分别返回 `invalid_presentation` / `invalid_image`。
- **导出为栅格对象**。SVG 以 `<image x= y= width= height=>` 发射，带 DPI、色彩空间与数据源 URI，不内联像素；PDF 后端尚未存在，PDF 栅格导出留作后续。

## 后果

- 引擎保持无编解码依赖与可信输入面收窄；渲染前必须由宿主接线 image_tile 解析器（与 manifest 缓冲解析同约束）。
- 纹理缓存粒度为单瓦片，LRU 按 `last_used` 帧戳淘汰；可见瓦片优先保留。
- 上下文丢失重建的正确性取决于宿主解析器幂等可重放；测试以可计数 mock 解析器覆盖。
