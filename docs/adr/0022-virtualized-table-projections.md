---
status: accepted
---

# 表格模式使用虚拟化且不隐式重采样的投影

共享同一 Sampling Axis 的曲线可形成 `Depth | Curve...` 宽表，不同采样轴默认分表；只有用户明确选择目标轴和插值方法时才生成标识清楚的 Resampled Table。区间、层位和注释保持独立表，多井默认按井组织工作表。核心提供按需访问的 Table Projection，Qt 适配为虚拟化 QAbstractTableModel；选区复制同时提供 TSV 与 HTML Table，表格导出首期支持 XLSX、XML 和 CSV，并保留单位、Null 语义及 Reference Depth 类型。
