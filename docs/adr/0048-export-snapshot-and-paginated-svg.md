---
status: accepted
---

# Export Snapshot 与物理分页 SVG

导出的第一步切片：一个不可变的 **Export Snapshot** 捕获让导出可复现、自描述的元数据（#156 准则 1），以及一个**物理比例、可分页的 SVG 导出器**，把单个 PreparedScene 切成连续长页或定页，每页重复页眉/图例/页码/深度范围。本 ADR 仅覆盖 SVG（PDF 由 #187/#188 在 ADR 0047 写入器之上构建）。关闭 #156 准则 1、2、3、4 与准则 8 的 SVG 部分。

## 背景

rendering.md / #156 要求从 Export Snapshot 输出物理比例准确的 SVG：覆盖曲线、区间、Pattern、Unicode 文本、Custom Layer 与源栅格图像，并支持长页与分页。此前 `SvgExporter::write(const PreparedScene&)` 只输出恰好一个场景尺寸的 SVG，没有页模型、没有分页、没有物理比例。#185 Spike（ADR 0047）已落定导出分解的共享决策（自写写入器、OutlineCommand 词表、确定性构造、字形轮廓），本 ADR 在其之上构建 SVG 切片。

侦察发现 Export Snapshot 准则 1 所需的若干字段此前缺失：Document Revision（已有）、Presentation 版本（缺失）、Depth Transform 参数（缺失——仅有 `ReferenceDepthRange`，无 `DepthTransform` 结构）、字体指纹（已有）、Pattern 版本（`PatternDefinition` 此前无 version 字段）。

## 决策

### 1. Export Snapshot = 不可变自描述元数据捕获

`ExportSnapshot`（`include/welllog/export/pagination.hpp`）记录：document revision、presentation 版本、depth transform 描述符、字体资产指纹、pattern 版本集合、以及页规格。导出运行期间宿主继续编辑时，输出仍对应捕获的 Snapshot。

- 新增 `PresentationVersion`（builder 的 `set_presentation_version` 设置，默认 0，既有 presentation 字节不变）。
- 新增 `DepthTransformDescriptor`：域 + 参考深度顶/底 + version tag。**这不是 ADR 0013 的完整可逆深度变换链**（组合 measured/true vertical/sub-sea 变换），仅足以让 snapshot 记录它产生时所基于的深度映射。完整链是更大的后续课题。
- `PatternDefinition` 增加 `version` 字段（默认 0）。
- 通过 **builder setter**（而非扩 ctor）注入这些可选元数据：既有约 16 处 `ScenePresentationBuilder` 调用点（含 Qt 测试、benchmark、python bridge）全部无需改动、既有测试快照字节不变。

### 2. 物理分页：单次 prepare + 按页深度窗裁切发射

主机按 `PaginatedSvgExporter::required_aggregate_pixel_height` 返回的**聚合像素高** prepare 场景**一次**（`pixel_height = 每页深度像素 × 页数`），随后每个定页把同一场景裁切到该页的深度窗 `[y_top, y_bottom]` 发射。这样高效、DRY、完全加性（既有 `SvgExporter::write` 原样保留）。

正确性依据：

- 曲线 LOD 选择在**参考深度空间**进行（`CurveLodQuery` 取深度窗 + 像素高），与场景几何无关；深度→y 是单一全局线性映射 `y = (depth − top)/(bottom − top) × physical_height`。聚合密度 prepare 对均匀采样（测井曲线的主流情形）给出正确的每页包络。
- prepare 已把区间矩形 clamp 到 `[0, physical_height]`（scene.cpp），故跨页几何在按页裁切窗下自然分裂，无需重新 clamp；Marker/区间/Custom 等非曲线 Layer 在各页位置稳定。
- 曲线是唯一的密度相关量。**按页本地密度重查询**（针对非均匀采样）作为改进/延后项记录——聚合密度 prepare 对主导的均匀采样是正确的。

### 3. 页模型：连续 vs 定页

- **连续模式**：一个 SVG，高度保持真实深度→物理长度（场景物理高 × 与宽度相同的缩放因子），可选单一页眉/页脚（深度范围）。
- **定页模式**：`页数 = ceil(场景高 / (可印深度高 × (1 − page_overlap)))`；每页：页尺寸 `<svg>`、页眉带（井名 + "page N of M"）、页脚带（页码 + 该页深度范围 `data-page-depth-top/-bottom`）、可选图例带（每条可见曲线：助记 + 色块 + 比例范围），以及被页深度窗裁切、平移到位的场景 body。Pattern/glyph `<defs>` 每页发射一次（每页是独立 SVG 文档）。

### 4. 复用既有逐层发射

分页器**不重写**逐层发射（svg.cpp 的 interval/marker/symbol/curve/crossover/image/custom/text 块）。重构把 `<defs>` 与逐 track `<g>` 体抽成共享 helper（`svg_internal::append_defs` / `append_layer_body`），单一场景导出器与分页器共用同一份几何发射——保证两种路径逐层一致。逐页页眉/页脚/图例的合成文本用独立 `append_text_element`（普通 ASCII `<text>`，**非**场景字形轮廓 run；场景自身的逐 track curve header run 是场景绝对定位、无法廉价按页重定位，故页眉复用 `PreparedTrackHeaderEntry` 元数据按页重塑为文本）。

所有尺寸用物理毫米，绝不代入屏幕像素或未校准系统 DPI（ADR 0039）。

## 后果

- `SvgExporter::write(const PreparedScene&)` 行为完全保留（分页器是加性的）；既有 SVG 快照测试不变。
- 曲线包络密度依赖主机按 `required_aggregate_pixel_height` 正确 prepare；未按聚合密度 prepare 时曲线仍正确（只是密度可能不是每页最优）。
- 文本经字形轮廓 run 发射（与既有 SVG 一致），不可搜索/不可选中——与 ADR 0047 一致，测井导出是图形交付物。

## 延后项（诚实记录，同 #152/#153）

- PDF 发射（#187/#188）。
- **Document Scale `1:N`**。spec §9 与 requirements EXP-02 把分页建模为比例驱动（`page_depth_span = printable_height × N`），但本切片**不引入显式 `1:N` 参数**：定页深度跨度改为从场景既有的 `physical_height / physical_width` 比例推导（`printable_depth_height_mm`），由宿主在 prepare 时设定。也就是说，本切片的"比例"是宿主烘焙进场景物理维度的，**而非 spec §9 的 `printable_height × N`**。由此连续模式的发射物理长度 = 宿主设定的 `physical_height`（内部几何自洽、深度比例保真），但未实现 §9 的显式 `1:N` 语义。显式 Document Scale 模型作为后续 export ticket 延后。
- **Selection/Overlay 策略 与颜色空间策略**。spec §8.1（Selection/Overlay 作为 snapshot 捕获项）与 §8.2（"颜色空间策略显式"）要求的字段，`ExportSnapshot` 本切片**均未捕获**。二者更自然地属于后续导出流水线阶段（栅格/PDF/选择渲染），故延后而非实现；本次 snapshot 仅捕获准则 1 列出的其余自描述字段。
- ADR 0013 完整可逆深度变换链（本次仅落 version-tagged 描述符）。
- 针对非均匀采样的按页本地密度曲线重查询（聚合密度 prepare 对主导情形正确；改进为后续）。
- 新增 presentation/version/depth-transform/pattern-version 字段的 manifest 序列化（ADR 0032/0037——与 #152/#153 同类缺口）。
- XLSX/CSV（#155）、PNG/TIFF（#156 栅格切片）、CGM。
