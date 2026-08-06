---
status: accepted
---

# 导出 B1：CGM 作为独立导出后端（相对 ADR 0021）

**CGM**（Computer Graphics Metafile）列入 **导出 B1**，作为与 PDF/SVG/PNG/TIFF **并列的独立 Export Backend**，消费同一 `PreparedScene + ExportSnapshot`。  
在实现与一致性测试通过前，**不得**在 UI、文档或发行说明中宣称已支持 CGM。

## 背景

- ADR 0021：首期 PDF/SVG/PNG/TIFF；「CGM 延后为可能的企业交换插件」。
- `well-log-engine/docs/table-and-export.md` §11：CGM 不在首期；若确认企业交换需求，应独立后端、自有降级策略与一致性测试，不修改 Core/Scene 语义。
- 技术方案 §15.6 导出 B1：CGM 后端（新 ADR 选型后实现）；§16 愿景含 CGM 一致性检查。
- Desktop 首发 B0 **不含** CGM（epic #288 已结案）。T16 / #304 启动前须完成本 ADR。

## 决策

### 1. 产品定位

- **用途**：企业/存档交换（测井图矢量 metafile），非交互编辑主格式。
- **优先级**：低于可搜索 PDF（ADR 0053）与 0.1 mm 几何矩阵扩容；可与 B1.PDF 并行，但不阻塞 Desktop 补丁版。
- **入口**：导出菜单/API 的显式格式项 `cgm`；无「静默 CGM」。

### 2. 技术选型：自研子集写入器（对齐 PDF/SVG 策略）

| 选项 | 结论 |
|------|------|
| 第三方完整 CGM SDK | **不作为 B1 默认**：许可证、构建与确定性难控；多数库对 Pattern/地质图纹支持差 |
| 宿主 Qt 无 CGM | 不适用 |
| **自研 CGM Version 3 Binary 子集写入器** | **采用**：与 ADR 0047/0048 相同哲学——只映射引擎已有绘制词表 |

要点：

- 输入：`PreparedScene + ExportSnapshot`（与 SVG/PDF 相同）；**不**改 Document/Scene 语义（table-and-export §11）。
- 编码：优先 **CGM:1999 Version 3 Binary**（交换体积与工业工具兼容性）；可选 Clear Text 仅用于调试开关，默认不发行。
- 图元映射（B1 最小集）：
  - 折线/多边形 ← 曲线包络、网格、区间框、Custom 线框；
  - 填充 ← 纯色；**Pattern** 首次可降级为纯色或细密 hatch，并写入 Export Diagnostic；
  - 文本 ← 受限 `TEXT`/`APPEND TEXT`（Unicode 策略：嵌入或平台字，须文档化；复杂整形可降级为路径近似并诊断）；
  - 裁剪 ← 页/道裁切窗。
- 透明度：CGM 能力有限 → **不支持半透明**时扁平为不透明 + 诊断（禁止静默看起来「像 PDF」）。
- 分页：与 SVG 定页模型对齐——多页可多 metafile 或 BEGIN/END PICTURE 每页一图；**推荐每页一个 PICTURE**，文件名或容器约定在实现 ticket 写清。

### 3. 明确不做（B1）

- WebCGM DOM、完整 ISO 附件、智能图形超链接。
- 用 CGM 替换 PDF/SVG 成为默认工程交付。
- 修改 Core 为「CGM 友好」的第二套几何。
- 在无金标测试时宣称 0.1 mm CGM 达标。

### 4. 一致性与验收

- 独立测试目录（建议 `well-log-engine` 无头 + 宿主可选冒烟）：固定 `T14_GOLDEN_V1`（及后续夹具）→ CGM → 解析或参考阅读器度量关键折线端点。
- 与 PDF/SVG **同一布局模型**（ADR 0039 mm）；B1 全矩阵（0.1 mm）将 CGM 列为格式维之一，但 **CGM 可先以较大容差（如 0.5 mm）入门**，再收敛到 0.1 mm（写在几何矩阵 ticket，不在本 ADR 放宽 PDF/SVG 已有 0.1 mm 子集）。
- 安全：外部 CGM **导入**不在 B1（ADR 0042 不可信资产）；本 ADR 仅 **导出**。

### 5. 分期

| 切片 | 内容 |
|------|------|
| **B1.CGM.1** | ✅ 自写 CGM V3 Binary：`CgmBinaryWriter` + `CgmSceneExporter`（曲线折线、道框、Latin TEXT）；`welllog.cgm-spike` 无头烟测；宿主菜单由 **B1.CGM.2** 挂接（见下行） |
| **B1.CGM.2** | ✅ 区间/交叉填充 → 纯色 POLYGON + `CgmExportDiagnostics`；`export_scene_cgm` 绑定；宿主 **导出 → CGM…** + 披露文案 |
| **B1.CGM.3** | ✅ 固定页高多 PICTURE 分页；花纹 = 纯色 + 对角 hatch 近似 + 诊断；`cgm_scene_to_vdc` + 宿主金标 `TOL_MM_CGM=0.5` 格式维 |

### 6. 与既有 ADR 的关系

- **细化** ADR 0021「CGM 延后」：延后终点 = B1，形态 = 独立后端而非含糊「插件」。
- **可**在轨 P 再包一层 `IExportPlugin`（#305），但 B1 **不**阻塞在完整插件 Runtime。
- 与 ADR 0052：CGM 绑定方式同 Stage 1 注入函数路径（`export_scene_cgm` 类 API），在 PDF/SVG 稳定后加。

## 后果

- 选型闭合：自研子集 + PreparedScene，无第三方 CGM 硬依赖。
- 产品承诺边界清晰：未实现前零「支持 CGM」表述。
- Pattern/透明度降级必须可见，避免企业交换「看起来完整实则丢花纹」。
- 实现 ticket 在 #304 下拆分为 B1.CGM.1–3；本 ADR 关闭 T16「启动前 CGM ADR」门禁。
