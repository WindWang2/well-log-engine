---
status: accepted
---

# 图形与表格共享语义 Selection Set

WellLogSession 单一持有 Selection Set，以稳定井、曲线或区间身份、Reference Depth、样点索引和 Document Revision 表达，不保存屏幕坐标、Display Depth 或 LOD 包络点。图上范围选择与 Table Projection 双向联动，复制始终读取原始样点；追加数据尽量保持选择，无法安全映射的替换显式使其失效并发布事件。高频悬停在 C++ 内合并限频，避免逐鼠标事件跨越 Python 边界。
