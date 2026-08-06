---
status: accepted
---

# 图道由可组合图层构成

Track 只定义水平布局、表头、横轴、裁切和图层顺序，不再以 CurveTrack、LithologyTrack 等 QWidget 子类表达业务类型。曲线、区间、曲线间或基线填充、地质图纹、栅格图像、文本与符号由可组合 Layer 表达；岩性、地层、层序、体系域和沉积相通过通用图层及不同语义样式组合，跨井对象留在 Cross-Well Overlay。新的专业显示通过注册 Layer Renderer 扩展，而不是扩张 Qt 控件继承树。
