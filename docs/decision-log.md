# WellLogEngine 架构决策索引

本索引列出需求访谈中已确认的决策。ADR 原文位于本仓库 [`docs/adr/`](./adr/)。

## 边界与平台

- [ADR 0004：独立且可嵌入的测井渲染内核](./adr/0004-independent-embeddable-well-log-engine.md)
- [ADR 0005：测井数据源与渲染内核分离](./adr/0005-separate-log-sources-from-rendering.md)
- [ADR 0007：首期正式支持 Windows x64 与 Linux x64](./adr/0007-phase-one-platform-support.md)
- [ADR 0035：新引擎使用独立顶层 CMake 子项目](./adr/0035-independent-well-log-cmake-project.md)

## 数据、身份与状态

- [ADR 0006：不可变且有所有权保证的共享缓冲区](./adr/0006-immutable-owned-log-buffers.md)
- [ADR 0011：测井内核拥有交互状态](./adr/0011-engine-owns-interaction-state.md)
- [ADR 0012：多井是内核场景的一等能力](./adr/0012-multi-well-scene.md)
- [ADR 0013：可逆深度变换链](./adr/0013-reversible-depth-transform-chain.md)
- [ADR 0024：图形与表格共享语义 Selection Set](./adr/0024-shared-semantic-selection.md)
- [ADR 0025：解释可编辑而原始曲线不可变](./adr/0025-nondestructive-log-editing.md)
- [ADR 0026：持久 UUID 与运行时 Handle 分层](./adr/0026-persistent-uuid-runtime-handles.md)
- [ADR 0028：严格验证并局部容错](./adr/0028-strict-validation-local-tolerance.md)
- [ADR 0031：原子分块追加](./adr/0031-append-only-streaming-contract.md)
- [ADR 0032：强类型 API 配合版本化 Manifest](./adr/0032-versioned-manifest-external-buffers.md)
- [ADR 0049：LIS ResForm 兼容 v1 身份与规则快照](./adr/0049-lis-resform-v1-profile.md)
- [ADR 0051：宿主侧 per-plot 单调修订计数器（schema v3）](./adr/0051-plot-revision-schema-v3.md)

## 渲染与扩展

- [ADR 0008：OpenGL 3.3 Core 渲染后端](./adr/0008-opengl-rendering-backend.md)
- [ADR 0015：分块层次包络 LOD](./adr/0015-hierarchical-curve-envelope-lod.md)
- [ADR 0016：单 GL Context 与 CPU Worker](./adr/0016-single-gl-context-with-cpu-workers.md)
- [ADR 0017：图道由可组合图层构成](./adr/0017-composable-track-layers.md)
- [ADR 0018：声明式 Custom Layer](./adr/0018-declarative-custom-layer-extension.md)
- [ADR 0019：保留式 Prepared Scene](./adr/0019-retained-prepared-scene.md)
- [ADR 0020：矢量 Pattern Definition 为共同来源](./adr/0020-vector-pattern-source.md)
- [ADR 0023：多曲线显式绑定 Track Scale](./adr/0023-explicit-multi-scale-curve-tracks.md)
- [ADR 0029：Qt 无关 Unicode 文字管线](./adr/0029-platform-neutral-text-pipeline.md)
- [ADR 0030：CPU 语义拾取并回查原始数据](./adr/0030-cpu-semantic-picking.md)
- [ADR 0033：不提供软件交互回退](./adr/0033-no-software-interactive-fallback.md)
- [ADR 0034：派生 CPU/GPU 缓存预算化](./adr/0034-budgeted-derived-caches.md)
- [ADR 0039：文档物理单位与屏幕单位分离](./adr/0039-physical-document-units.md)
- [ADR 0050：显式虚线段数组为三后端共同来源（DashPattern）](./adr/0050-dash-pattern-three-backends.md)

## Qt、Python 与数据适配

- [ADR 0009：首期 Qt Widgets 适配](./adr/0009-qt-widgets-first-adapter.md)
- [ADR 0010：PySide6 使用 Shiboken6](./adr/0010-shiboken-pyside-binding.md)
- [ADR 0027：Arrow 是可选适配与存储实现](./adr/0027-optional-arrow-adapter.md)

## 表格与导出

- [ADR 0021：首期图形导出格式](./adr/0021-first-phase-export-formats.md)
- [ADR 0022：虚拟化且不隐式重采样的表格投影](./adr/0022-virtualized-table-projections.md)
- [ADR 0040：版本化测井表格 XML](./adr/0040-versioned-well-log-table-xml.md)
- [ADR 0047：PDF 自写写入器（默认轮廓字）](./adr/0047-pdf-via-hand-rolled-writer.md)
- [ADR 0048：Export Snapshot 与物理分页 SVG](./adr/0048-export-snapshot-and-paginated-svg.md)
- [ADR 0052：引擎矢量导出器分期路线](./adr/0052-engine-vector-exporters-staged-roadmap.md)
- [ADR 0053：B1 可搜索 PDF 可选路径](./adr/0053-searchable-pdf-b1-path.md)
- [ADR 0054：B1 CGM 独立导出后端](./adr/0054-cgm-export-backend-b1.md)（B1.CGM.1 骨架已实现：`welllog_export_cgm`）

## 性能、质量、安全与交付

- [ADR 0014：性能验收基线](./adr/0014-performance-acceptance-baseline.md)
- [ADR 0036：分层渲染验证](./adr/0036-layered-rendering-verification.md)
- [ADR 0037：SDK 兼容策略](./adr/0037-sdk-compatibility-policy.md)
- [ADR 0038：Result 与 Diagnostic 错误模型](./adr/0038-result-and-diagnostic-error-model.md)
- [ADR 0041：可复现 CMake 依赖](./adr/0041-reproducible-cmake-dependencies.md)
- [ADR 0042：不可信外部资产](./adr/0042-untrusted-external-assets.md)
- [ADR 0043：内置性能可观测性](./adr/0043-built-in-performance-observability.md)

