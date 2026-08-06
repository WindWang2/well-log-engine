---
status: accepted
---

# 引擎矢量导出器分期接入路线（评估备案，实现暂缓）

引擎已具备完整、可无头运行的 C++ SVG（物理分页）/PDF/PNG/TIFF 导出器，宿主 6 类图件导出仍全部经 Qt paint。本期评估结论：维持 T8 的宿主导出路由不变，同时把分期切换到引擎导出器的路线、前置缺口与绑定方案固化为备案，供后续阶段直接启动。

## 背景

Phase-2 T8 明确决定 "no new engine bindings"：`export_dispatch.py` 按图件类型全量走 Qt paint（SVG/PDF/PNG），fence_3d 仅 PNG（`grabFramebuffer`）。理由：phase-2 要求零 C++ 改动，且 numpy_bridge 无 custom-primitive 解析，section 无法走引擎 Custom Layer。

本评估（Candidate 1 follow-up）核实：

- **引擎导出器是"已实现未接入"**：`SvgExporter`/`PaginatedSvgExporter`（svg.cpp、pagination.cpp）、`PdfSceneExporter`（pdf_scene.cpp，自写写入器、字节确定性）、`RasterExportJob`/`export_raster_sync`（raster_export.cpp）。输入统一为 `PreparedScene + ExportSnapshot`，不依赖 GL/Qt/session，引擎内已有无头测试覆盖。
- **宿主 Qt paint 与 ADR 0021 的要求存在真实差距**：曲线固定 `n//2500` 抽稀无 LOD 包络（export_plot.py:143）；无 pattern fill/interval/marker 层；单页输出（计划的 `paginate_by_depth` 未交付，`depth_per_page_mm` 定义未用）；同步阻塞 GUI 线程；fence_3d 的 `grabFramebuffer` 被 `table-and-export.md` §8.3 明确禁止作正式导出。
- **切换的硬约束**：(a) 引擎只看到 single_well/correlation 的数据（且 numpy_bridge 映射有损：仅曲线道、NaN→0）；section/plane_map/fence_3d/composite 的渲染数据引擎根本不可见；(b) 引擎 PDF 文字为字形轮廓、不可搜索（ADR 0047），宿主 QPdfWriter 路径文本可搜索，切换在这点上是倒退；(c) 绑定本身不是主要成本——注入函数路径（仿现有 5 个 submit 函数）引擎侧为小~中型，真正的成本在保真补齐与缺失的引擎 API。

## 决策

1. **维持 T8 现状**：6 类图件导出路由不变（Qt paint / geoviz / grabFramebuffer）。理由：切换的真实成本在保真补齐而非绑定；section/plane_map/fence_3d/composite 在引擎域外；PDF 可搜索文本语义不应静默倒退。
2. **绑定方案结论备案**（供启动时直接采用）：采用**注入函数路径**（path B）——`typesystem_welllog.xml` 加 add-function + `numpy_bridge.cpp` 新增导出实现（payload dict 解析复用现有 helper，`PreparedScene` 经 `session.prepared_scene()/prepared_surface_scene()` 获取，结果 `PyBytes` 返回）+ CMake 链接导出库。全 shiboken 类型面（path A）与 CLI 子进程（path C）均不取：前者类型面大且逐参数扩 typesystem，后者需新建数据通道。
3. **分期路线备案**：
   - **Stage 1（单井试点，中型）**：引擎绑定 + single_well 引擎 SVG/PDF 导出。前置缺口：numpy_bridge payload 保真补齐（层位 tops/intervals/pattern 进 presentation）；导出密度制备 API（现有仅异步 viewport 重制备，无"按导出聚合密度制备"入口）；session text engine getter（PDF 文字需要）；GIL/异步设计（同步导出持 GIL 在 GUI 线程会卡 UI，大场景走 `RasterExportJob` 式 worker）。
   - **Stage 2（对比图）**：correlation 经 `prepared_surface_scene()`（`submit_multi_well_section` 已布好 layout，路径基本现成）。
   - **Stage 3（剖面图，独立大 epic）**：section custom-primitive 场景通路（numpy_bridge custom-primitive 解析 + host section_geometry 上传），即 T8 blockers 的正面解决。
   - **独立小项**：fence_3d 以离屏渲染替换 `grabFramebuffer`（geoviz 域，不依赖本路线）。
4. **触发条件**：出现深度分页/高保真导出需求或 Qt paint 路径保真投诉时，按 Stage 1 启动；启动前不再重复本评估。

## 后果

- 已知缺口被显式记录而非隐含：Qt paint 路径在 LOD 包络、矢量图层完整性、深度分页、异步/取消/原子写出（`table-and-export.md` §10）上均不满足 ADR 0021/0048，属已接受的临时状态。
- PDF 可搜索文本：宿主路径保持可搜索；Stage 1 落地时需向用户显式披露引擎 PDF 不可搜索的语义变化（纯矢量与混合模式显式选择本是 ADR 0021 要求）。
- plane_map/composite 永久留在 geoviz/Qt 路由（引擎域外）；fence_3d 的正式导出等离屏渲染独立项。
- 未来启动 Stage 1 时，本 ADR 的分期清单即为任务分解，绑定路径无需重新选型。
