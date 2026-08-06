# 表格与导出设计

## 1. 原则

图形、表格和导出是同一 Document Revision 的不同投影：

- 图形使用 Prepared Scene；
- 表格使用 Table Projection；
- 数据导出使用 Table Projection；
- 图形导出使用 Prepared Scene。

它们共享 Entity ID、Reference Depth、Selection Set、单位和 Null 语义，但不共享屏幕像素或 LOD 伪样点。

## 2. Table Projection

### 2.1 同轴宽表

共享同一 Sampling Axis 的曲线可组成：

```text
MD(m) | GR(API) | RT(ohm.m) | RHOB(g/cm3)
```

每一行直接引用源 Axis 索引。曲线 Null 为空单元格，并保留明确 Null 状态。

### 2.2 异轴分表

Sampling Axis Entity ID 不同的曲线默认分表：

```text
Table: curves@axis-a
Table: curves@axis-b
Table: intervals
Table: markers
Table: annotations
```

禁止：

- 按数组序号拼接；
- 按浮点近似深度自动 join；
- 自动插值到第一条曲线；
- 用 Display Depth 替换 Reference Depth。

### 2.3 Resampled Table

用户必须明确提供：

- 目标 Sampling Axis；
- 插值方法；
- 缺测区策略；
- 是否允许外推；
- 输出单位/精度。

输出标为 Derived/Resampled，并记录参数和源 Curve Revision。

## 3. 虚拟化

Table Projection 提供：

- 列元数据；
- 64 位行数；
- 单元/行块按需读取；
- Selection 到行范围映射；
- 流式 Export Cursor。

Qt Model 不把数据复制到 QVariant 矩阵。为降低 per-cell 开销，应支持可见行块缓存和批量格式化。

排序/过滤不能对源数组原地重排；若提供，必须产生视图索引，并明确其内存预算。

## 4. Selection 与 Copy

### 4.1 联动

- 图上选择 Reference Depth Range，表格选择所有落在该范围的源行。
- 表格选择 Curve Sample，图上高亮相应原始点/范围。
- 多轴表分别映射自己的源行。
- 文档替换导致无法映射时，Selection 显式失效。

### 4.2 剪贴板格式

同一次 Copy 至少生成：

- `text/plain`：TSV，包含列头；
- `text/html`：HTML Table；
- 可选内部 MIME：保留 Entity ID、Revision 和单位，用于应用内粘贴。

Copy 读取原始 Buffer，不读取 LOD。大选择集超过剪贴板安全上限时：

- 明确提示；
- 提供导出文件；
- 不在 GUI 线程构造超大字符串。

## 5. XLSX

### 5.1 工作簿组织

默认：

- 每口井一个工作表组；
- 每个 Sampling Axis 一个曲线工作表；
- Interval、Marker、Annotation 独立工作表；
- Metadata/Provenance 独立工作表；
- Resampled Table 在名称和元数据中标识。

Excel 单工作表最多 1,048,576 行，包含表头。超过时自动创建连续工作表，例如：

```text
A-01_curves_01
A-01_curves_02
```

每个连续工作表重复列定义并记录全局起始行。

### 5.2 单元格语义

- 数值写为数值，不预先格式化成字符串；
- Null 写为空单元格，并在元数据记录 Null 规则；
- Depth 保留足够精度；
- 单位写在独立表头行或列元数据；
- 不写宏、公式或外部链接；
- 写出采用流式/constant-memory 模式。

## 6. XML

XML 是版本化测井表格交换格式，不是 SpreadsheetML。

示意：

```xml
<wellLogTables schemaVersion="1.0">
  <well id="..." revision="..." referenceDepth="MD" unit="m">
    <table id="..." kind="curves" samplingAxisId="...">
      <columns>
        <column id="..." name="MD" unit="m" type="float64"/>
        <column id="..." name="GR" unit="API" type="float32"/>
      </columns>
      <rows>
        <row><v>1000.0</v><v>42.5</v></row>
        <row><v>1000.125</v><null/></row>
      </rows>
    </table>
  </well>
</wellLogTables>
```

最终元素和命名空间在 XSD Review 中确定。

要求：

- UTF-8；
- 流式写出；
- XSD 校验；
- 禁用 DTD、外部实体和网络资源；
- 原始/重采样表明确标识；
- 单位、Sampling Axis、Reference Depth 和 Null 不丢失；
- 提供小型、重复深度、异轴、Null、多井等示例和往返测试。

## 7. CSV

- 一个 CSV 只表达一个 Table Projection；
- 多表导出使用目录或 ZIP 包，并附 manifest；
- UTF-8；
- 分隔符、十进制点和换行规则固定或显式配置；
- Null 令牌不得与合法值混淆；
- Metadata 使用伴随 JSON，不塞进任意注释行。

## 8. 图形导出

### 8.1 Snapshot

导出请求捕获：

- Document Revision 集；
- Presentation/Profile Version；
- Depth Transform Version；
- Selection/Overlay 策略；
- Document Scale；
- 页面与字体配置。

导出运行期间宿主继续编辑时，输出仍对应捕获的 Snapshot，并在结果中报告 Revision。

### 8.2 PDF/SVG

- 使用物理尺寸；
- 支持连续长页或分页；
- Track/header/legend 复现；
- 曲线按目标页面精度重新选择包络；
- Pattern 使用矢量定义；
- 文字复用字形布局；
- 字体可嵌入时子集嵌入，否则轮廓；
- 源 Image 保持栅格；
- 颜色空间策略显式。

### 8.3 PNG/TIFF

- 明确输出 DPI、像素大小、背景和色彩空间；
- 大尺寸按 Tile 渲染，避免单张超大内存；
- TIFF 可选压缩不改变数值/视觉语义；
- 不使用 `QOpenGLWidget::grabFramebuffer()` 作为高分辨率正式导出，因为 GPU readback 会阻塞且受窗口分辨率限制。

### 8.4 纯矢量与混合模式

纯矢量：

- 除源栅格 Layer 外全部保持矢量；
- 可能产生较大文件；
- 不因复杂度静默转栅格。

混合高保真：

- 用户显式允许特定复杂 Layer 栅格化；
- 必须报告哪些 Layer 被栅格化、目标 DPI 和原因；
- 文字、坐标、表头等仍尽量保持矢量。

## 9. 比例尺与分页

Document Scale `1:N` 决定深度到页面物理长度：

```text
page_depth_span = printable_height * N
```

实现需要统一单位换算，禁止把屏幕 DPI 代入页面数学。

分页策略至少包括：

- 固定页面自动连续深度；
- 指定页间重叠；
- 每页重复 Header/Legend；
- Interval/Label 跨页裁切；
- Marker 边界去重；
- 页码、井名、深度范围和 Revision 元数据。

## 10. 异步、取消与原子写出

- 导出在 C++ Worker 执行，不阻塞 GUI；
- 进度以低频 Event 报告；
- 支持取消；
- 写到同目录临时文件，成功后原子替换目标；
- 失败或取消清理临时产物；
- 覆盖已有文件必须由宿主显式确认；
- 错误 Result 包含格式、路径、阶段和安全诊断，不包含原始数据。

## 11. CGM

CGM **不在 Desktop 导出 B0**。导出 **B1** 将其作为独立 Export Backend（**ADR 0054**）：

- 消费 Prepared Scene（+ 后续 Export Snapshot 分页）；
- 自研 CGM Version 3 Binary 子集写入器（`welllog_export_cgm`）：曲线/道框/Latin TEXT、区间与交叉填充、多 PICTURE 分页（B1.CGM.3）、花纹 hatch 近似；
- Pattern/透明度降级写入 `CgmExportDiagnostics`；宿主 **导出 → CGM…** 披露；
- 一致性：`welllog.cgm-spike` + 宿主 `TOL_MM_CGM=0.5` 格式维金标；
- 不修改 Core/Scene 语义。

可搜索 PDF 可选路径见 **ADR 0053**（默认轮廓 PDF 仍为 ADR 0047）。

