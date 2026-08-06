# 质量、安全与性能

## 1. 发布质量模型

验证分四层：

1. 数学与数据性质；
2. Prepared Scene 语义快照；
3. 容差图像与矢量结构；
4. 固定硬件性能和平台兼容。

任何层都不能替代另一层。像素相似不证明深度/选择正确，语义正确也不证明驱动可用。

## 2. 测试层次

### 2.1 Core 单元与性质测试

重点：

- Depth Transform 正逆往返；
- 分段变换单调性；
- MD/TVD/TVDSS 域隔离；
- Sampling Axis 递增、递减、重复、乱序拒绝；
- Null/NaN/Infinity 断线；
- 对数 Scale 非正值；
- Track Scale 正反向映射；
- UUID/Runtime Handle 生命周期；
- Document Patch 基础 Revision 冲突；
- Append Batch 原子性；
- Buffer size/stride 溢出；
- Result/Error 稳定码。

性质测试生成随机有效/无效输入，而不只依赖固定案例。

### 2.2 LOD 正确性

对每个有效数据段：

- 输出保持源顺序；
- 不跨 Null；
- 每个摘要范围的 min/max 必须出现；
- 放大到原始层级时值完全一致；
- 递减 Sampling Axis 与递增轴结果语义一致；
- 非线性 Depth Transform 下不遗漏可见范围；
- C++ 标量参考实现与优化实现一致。

需覆盖：

- 单点、双点；
- 全 Null；
- 交替极值；
- 极窄尖峰；
- 重复深度；
- 大幅值与亚米深度步长；
- Append 后尾块。

### 2.3 Prepared Scene 快照

快照存储稳定的语义结构，而不是指针或 GPU Handle：

- Pass 和 Z 顺序；
- Clip 层级；
- Track 物理边界；
- Layer Entity ID；
- 曲线段数量与范围；
- Pattern ID/锚点/物理单元；
- Glyph ID/位置；
- 拾取边界；
- 导出提示。

浮点使用明确容差和规范化输出。

### 2.4 图像回归

固定环境：

- 固定字体文件；
- 固定 Pattern 资产；
- 固定色彩空间；
- Mesa 软件 OpenGL 或受控 GPU；
- 固定物理像素与 DPR。

比较：

- 感知差异；
- 边缘/文字容差；
- 忽略已知驱动级亚像素差异；
- 任何阈值更新必须附视觉审查结果。

Windows/Linux 真 GPU 运行夜间矩阵，不要求跨驱动逐像素相同。

### 2.5 矢量导出测试

- PDF 页面尺寸与 Document Scale；
- SVG viewBox/物理尺寸；
- 字体嵌入或轮廓策略；
- Pattern 相位；
- 裁切；
- 分页深度连续性；
- Marker 页边界去重；
- 纯矢量模式不静默嵌入曲线栅格；
- 混合模式报告被栅格化 Layer。

### 2.6 表格测试

- 同轴宽表；
- 异轴分表；
- 禁止隐式重采样；
- Null/重复深度；
- Selection 往返；
- TSV/HTML Clipboard；
- XLSX 行数拆分；
- XML XSD 校验与往返；
- CSV manifest；
- 大表流式内存上限。

### 2.7 Qt/Python 测试

- Widget 创建/销毁；
- 父子所有权；
- `deleteLater`；
- Python 引用释放；
- NumPy owner pinning；
- Arrow release callback；
- GUI 线程违规；
- 快速更换文档 Last-Revision-Wins；
- Context 丢失/重建；
- Widget 重挂载；
- 信号限频；
- Python 3.12/3.13 wheel 导入。

## 3. Fuzz 与鲁棒性

持续 fuzz 目标：

- Manifest JSON；
- XML；
- XLSX/ZIP container；
- Pattern Definition；
- 字体元数据入口；
- Buffer descriptor；
- LAS/DLIS 等 Source Adapter；
- Document Patch；
- Depth Transform 控制点。

断言：

- 不越界；
- 不整数溢出；
- 不无限循环；
- 不无界分配；
- 不跨边界抛异常；
- 错误只隔离对象/文档/视图。

种子库包含现有项目中已发现的短行、长行、包装 LAS、NaN、Infinity、重复身份和快速换版案例。

## 4. 性能基准

### 4.1 固定场景

主场景：

- 20 wells；
- 100 visible tracks；
- 200 curves；
- 500k max samples/curve；
- 100M total scalar samples；
- 100k discrete objects；
- 3840×2160 physical pixels。

辅助场景：

- 单井 200 曲线；
- 全 Null/高缺测；
- 极密集锯齿；
- 100k 短 Interval；
- 大中文标签；
- 多 Pattern Fill；
- 非线性多控制点 Depth Transform；
- 高频 Append；
- Context 重建。

### 4.2 记录指标

- first interactive frame；
- frame CPU P50/P95/P99；
- GPU Pass P50/P95/P99；
- pick P50/P95/P99；
- GUI max blocking；
- worker latency/cancel ratio；
- CPU/GPU cache hit；
- upload bytes/frame；
- draw batches；
- CPU derived memory；
- GPU resident memory；
- export throughput/peak memory；
- table copy/export throughput。

### 4.3 门禁

- 主 SLO 以需求规格为准。
- 相对于已批准基线回退超过 10% 阻止发布。
- 更新基线必须说明硬件、驱动、工具链、数据集和原因。
- 共享 CI 不判定绝对帧时。
- 固定工作站在干净环境、多次运行后取统计结果。

## 5. Profiler 与 Trace

Profiler Overlay 至少显示：

- FPS 与 frame P95；
- Prepare/Upload/Draw/GPU；
- 可见 Track/Curve；
- 当前 LOD 点数；
- CPU/GPU cache；
- worker queue；
- diagnostics count。

Chrome Trace 导出：

- Session command；
- invalidation；
- worker task；
- upload；
- render pass；
- present；
- append/export。

Trace 只使用 Entity ID 和数量，不包含井名、文本标签和样点值。

## 6. 安全模型

### 6.1 不可信输入

下列均不可信：

- 数据文件；
- Manifest；
- XML/XLSX/ZIP；
- 字体；
- Pattern；
- 图像；
- Custom Layer；
- Python Buffer descriptor。

### 6.2 必须实施

- checked arithmetic；
- 最大样点/对象/文本/图片/嵌套深度；
- ZIP 展开量、压缩比、条目数和路径验证；
- XML DTD/外部实体/网络禁用；
- 图片像素上限与分块；
- 字体解析资源上限；
- 无脚本、无 Shader、无命令执行；
- Export 路径由宿主确认；
- 临时文件同目录、成功后原子替换；
- 不在日志/崩溃报告中记录原始数据。

具体数值上限在实现前由配置 Schema 决定，并具有保守默认值。

## 7. Error 与 Diagnostic

### 同步 Error

- Validation；
- VersionConflict；
- Capability；
- ResourceLimit；
- ThreadViolation；
- Export；
- IO Adapter。

### Diagnostic

- 跳过无效值；
- 对数非正值；
- 缺失字体；
- Pattern 降级；
- Cache Pressure；
- stale background result；
- dropped/coalesced append；
- Context recovery。

Diagnostic 具有稳定码、Severity、Entity ID 和计数，可本地化；高频相同诊断必须聚合。

## 8. OpenGL 兼容

Capability Report 包含：

- vendor/renderer/version；
- GLSL；
- Core Profile；
- stencil bits；
- texture limits；
- buffer limits；
- optional extensions；
- 启用/禁用增强路径。

兼容测试至少覆盖：

- Windows 主流 Intel/AMD/NVIDIA；
- Linux Mesa Intel/AMD；
- Linux NVIDIA 专有驱动；
- Mesa software renderer（测试用途）。

## 9. 发布门禁清单

- [ ] Core/Scene/Export 单元、性质测试通过。
- [ ] Prepared Scene 快照无未审查变化。
- [ ] 固定图像回归通过。
- [ ] Windows/Linux GPU 矩阵通过。
- [ ] 主性能场景达到 SLO。
- [ ] ASan/UBSan/Windows 内存检查通过。
- [ ] Fuzz 无已知崩溃与越界。
- [ ] C++/CMake Package Consumer 测试通过。
- [ ] PySide6 wheels 在支持矩阵安装与运行。
- [ ] PDF/SVG/XLSX/XML/CSV 契约通过。
- [ ] SBOM、许可证、工具链清单完成。
- [ ] Manifest 迁移测试通过。
- [ ] 文档与 API reference 更新。


## Release gate (#174)

See [release-gate.md](release-gate.md) and [sbom-and-licenses.md](sbom-and-licenses.md).
