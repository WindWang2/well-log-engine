---
status: accepted
---

# 纵向坐标采用可逆深度变换链

WellLogEngine 明确区分源采样轴、Reference Depth、Display Depth、视口相对坐标与屏幕坐标。每口井的 Reference Depth 可为 MD、TVD 或 TVDSS；基准拉平及多标志层对齐通过单调、可逆的 Depth Transform 表达，允许分段线性纵向拉伸，但不得改写原始深度数组。CPU 逻辑坐标使用双精度，GPU 使用相对视口中心的单精度坐标；拾取、提示和导出必须能从显示坐标返回并标明真实参考深度。
