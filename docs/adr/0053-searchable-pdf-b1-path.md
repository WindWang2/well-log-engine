---
status: accepted
---

# 导出 B1：可搜索 PDF 为显式可选路径（相对 ADR 0047）

引擎 **B0 默认 PDF** 仍走 ADR 0047 自写写入器与**字形轮廓**（不可搜索/不可选中），并须 UI 披露。  
**B1** 增加用户**显式选择**的 **可搜索 PDF** 路径；不得把默认可搜索语义静默替换为轮廓字，也不得在未交付时宣称「引擎 PDF 可搜索」。

## 背景

- ADR 0021 愿景：PDF 文字「优先保留为可搜索文本并嵌入字体子集」。
- ADR 0047 首期 Spike 结论：自写 PDF + 轮廓字，规避 CMap/CID/ToUnicode/CJK 子集复杂度；测井图以图形交付为主，B0 可接受不可搜索。
- Desktop 首发（epic #288 / 轨 E B0）已兑现：单井引擎 SVG/PDF + **不可搜索披露**（#299）、连井矢量、打印预览、几何金标子集。
- 技术方案 §15.6 **导出 B1** 要求：可搜索 PDF 路径（换栈或双后端；与 ADR 0047 **显式修订**）。T16 / #304 启动前须完成本 ADR。

宿主 Qt `QPdfWriter` 路径文本仍可搜索，但与引擎高保真/分页/LOD 包络不是同一后端；B1 目标是**引擎交付物**可选可搜索，而非长期依赖 Qt paint 顶替。

## 决策

### 1. 双模式，显式选择（不静默）

| 模式 | 语义 | 何时 |
|------|------|------|
| **`pdf_outline`（默认 / B0）** | 字形 → 矢量路径；不可搜索；字节确定性优先 | 默认导出、CI 金标、无障碍披露后的工程图形交付 |
| **`pdf_searchable`（B1）** | 视觉路径 + 可检索文本层；可搜索/可选中 | 用户在导出对话框或 API 中**显式**选择 |

- UI / API 必须使用可区分选项（例如「PDF（图形/不可搜索）」与「PDF（可搜索文本）」），禁止仅改内部默认而不提示。
- B0 披露文案在选择 `pdf_outline` 时保留；选择 `pdf_searchable` 时改为说明「含可提取文本；复杂 CJK/自定义字体以本版本支持矩阵为准」。

### 2. 实现策略：扩展自写写入器，不默认引入 PoDoFo

1. **主路径（B1 首选）**：在 ADR 0047 写入器上增加可选 **Text 对象发射**：
   - 输入仍为 `PreparedScene + ExportSnapshot` + session text engine（与 Stage 1 导出一致）。
   - 对每个文本 run：保留现有轮廓绘制以保证与屏幕/SVG **视觉一致**；并叠加 **不可见或同色 PDF 文本算子**（`Tj`/`TJ` + 字体资源），供搜索/复制。
   - 字体：FreeType 已加载的 face → 嵌入 **子集** TrueType/CID；写入 **ToUnicode** CMap，保证复制粘贴为 Unicode。
   - CJK：HarfBuzz 整形结果驱动 glyph 索引；子集失败时该 run **降级为仅轮廓**并记入 Export Diagnostic（不整页失败，除非用户要求 strict）。
2. **后备（仅当主路径在 CJK/子集上阻塞）**：可选动态链接 **PoDoFo**（MPL-2.0，`PODOFO_WITH_AFDKO=OFF`，时间戳/ID 归一化以满足可复现），作为 `pdf_searchable` 的备选后端；**不**替换 `pdf_outline` 默认路径。启用后备须单独 changelog，不得静默。
3. **不采用**：全量改默认轮廓 PDF 为可搜索；或仅用宿主 QPdfWriter 宣称「引擎 B1 已完成」。

### 3. 确定性与测试

- `pdf_outline`：继续要求同输入同字节（ADR 0047）。
- `pdf_searchable`：**不**要求与 outline 模式字节相同；要求 (a) `pdftotext`/`mutool draw -F txt` 可提取期望助记符/井名/深度标签子集；(b) 几何金标（T14 子集 → B1 扩矩阵）在 0.1 mm 容差内与 outline 模式一致（文本框锚点对齐轮廓基线）。
- 金标夹具复用 `T14_GOLDEN_V1` 并扩展标签字符串表。

### 4. 分期

| 切片 | 内容 |
|------|------|
| **B1.PDF.1** | 导出 UI 双选项 + 产品模式名 `outline`/`searchable`；searchable 先经 Qt（过渡） |
| **B1.PDF.2** | 引擎 `searchable_text`：Base-14 **Helvetica** Latin/ASCII 页眉带叠加；`PdfPathStream::draw_standard_text`；Python `export_scene_pdf(..., searchable_text)`；无绑定则回退 Qt |
| **B1.PDF.3** | ✅ Latin-1（WinAnsi Helvetica）+ UTF-8 解码；CJK 从可搜索层丢弃并计数（`SearchableTextStats` / `non_latin_codepoints_dropped`）；完整嵌入字体 ToUnicode 子集与连井矩阵仍为后续增强 |


### 5. 与既有 ADR 的关系

- **修订** ADR 0047 的「永久轮廓、仅未来另案」表述：另案即本 ADR；**0047 对默认路径仍然有效**。
- **落实** ADR 0021「文字优先可搜索」于 B1 可选路径，而非推翻 B0 披露。
- **衔接** ADR 0052 Stage 1：可搜索模式仍走引擎导出绑定，不新开 Qt 独占通道。

## 后果

- 产品可诚实区分「图形 PDF」与「可搜索 PDF」，避免 B0 披露与愿景 0021 长期对立。
- 实现与测试成本集中在字体子集与 CJK；默认路径与现有 CI 不受影响。
- 若主路径不可行再评估 PoDoFo，避免过早引入重依赖。
- 实现 ticket 在 #304 下拆分为 B1.PDF.1–3；本 ADR 关闭 T16「启动前 PDF 文本 ADR」门禁。
