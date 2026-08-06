---
status: accepted
---

# 拾取使用 CPU 语义索引并回查原始数据

Track 使用有序布局边界定位，区间和 Marker 使用深度索引，文本、符号与 Cross-Well Overlay 使用包围盒或 R-Tree；曲线先在当前 LOD 几何中定位最近线段，再在对应原始样点窗口精确回查。命中结果返回 Entity ID、Reference Depth、Display Depth、原始值和类型，容差按物理像素计算。高频 Hover 禁止同步 `glReadPixels`，GPU ID Buffer 仅保留为未来密集 Custom Layer 的异步后备。
