---
status: accepted
---

# PDF 导出采用自写极简写入器

PDF 导出后端由引擎自写的极简写入器生成，不引入第三方 PDF 库；仅新增 zlib（FlateDecode 流压缩）依赖。字体通过字形轮廓转矢量路径嵌入，不做子集化。输出按构造确定性（无 CreationDate/ModDate/ID）。

> **B1 修订指针（不废止本 ADR）**：默认路径仍为轮廓字不可搜索（Desktop B0 披露）。**可搜索 PDF** 为显式可选模式，见 **ADR 0053**；不得将本 ADR 的默认语义静默改为可搜索。

## 背景

rendering.md / #156 要求从 Export Snapshot 输出物理比例准确的 PDF/SVG，覆盖曲线、区间、Pattern、Unicode 文本、Custom Layer 与源栅格图像，并支持长页与分页。PRD #143 的"实现前必须关闭的具体项"列出"PDF 实现库及字体子集方案"需经技术 Spike 定案。本 ADR 落实 #185 Spike 的结论。

Spike 调研了 vcpkg 可得的候选（PoDoFo、libharu、qpdf；PDFium/muPDF/PDFHummus 均非 vcpkg port 或许可证不兼容），结论：所有库都需手写 PDF **tiling pattern**——而 PatternDefinition 是地质图纹的唯一矢量真值（ADR 0020），tiling pattern 是强制特性。因此即便引入库，最关键的 Pattern 仍要自写，库带来的收益被抵消。

## 决策

- **自写极简 PDF 写入器**（`include/welllog/export/pdf.hpp`、`src/export_vector/pdf.cpp`）。结构为标准间接对象：Catalog/Pages/每页 Page + content stream + xref + trailer。Spike 已证明可生成 qpdf --check / pdfinfo 通过、字节确定性、Flate 压缩的多页 PDF。
- **仅新增 zlib 依赖**（`vcpkg.json` 增加 `zlib`，`find_package(ZLIB)`，`ZLIB::ZLIB` 私有链接）。用于 content stream 的 FlateDecode；图像/字体流后续亦复用。
- **路径算子复用 OutlineCommand 词表**：`OutlineVerb{move_to,line_to,quadratic_to,cubic_to,close}` → PDF `m/l/c/c/h`，`quadratic_to` 按 `c1=p0+2/3(c-p0), c2=c+1/3(p1-c)` 提升为三次（PDF 无二次算子）。与 SVG 路径发射同构（`svg.cpp append_outline_path_data`），两端共享同一份几何真值。
- **字体经字形轮廓转矢量路径，不做子集化**。引擎已有 `PreparedGlyphOutline`（FreeType+HarfBuzz 产出的 OutlineCommand 流）；PDF 文本即图形路径，不可搜索/选中。规避了 CMap/CIDToGIDMap/ToUnicode/CJK 子集的复杂度与无生产级 C++ 子集库的现实。测井导出是图形交付物（`docs/table-and-export.md` §8.2），可接受。
- **确定性按构造保证**：不写 CreationDate/ModDate；不写非确定性 /ID；相同输入产出字节相同（Spike 测试断言两次构建相等）。这直接满足可复现构建约束，而 PoDoFo/libharu 默认写入时间戳/ID 需事后归一化。

## 后果

- 无重型 PDF 依赖、无构建期网络（规避 PDFium/AFDKO/muPDF 的构建/许可证问题）；攻击面相对第三方库大幅收窄。注：当前写入器尚未对内容流/算子串施加硬性字节上限（#185 Spike 输入为引擎产物而非原始不可信输入），完整资源限制（含超限拒绝）随 #186/#187 落地。
- 文本不可搜索/不可选中——若未来需要可检索文本，再以版本化 C ABI 或 PoDoFo（MPL-2.0 动态链接 + `PODOFO_WITH_AFDKO=OFF` + 手动时间戳覆盖）单独设计，不影响本写入器。
- 本 ADR 仅记录写入器与字体策略；Export Snapshot、物理分页、各 Layer 类型的 PDF 发射（曲线/文本/图像/Pattern/Custom）由后续 ticket（#186/#187/#188/#189）在此写入器之上构建。
- Spike 的 `welllog_export_pdf` 库为后续 ticket 的基础；其 `PdfPathStream`（OutlineCommand→算子）与 `PdfWriter::write`（多页装配）是 #187/#188 直接复用的稳定面。
