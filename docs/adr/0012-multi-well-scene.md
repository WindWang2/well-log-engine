---
status: accepted
---

# 多井是内核场景的一等能力

一份 WellLogDocument 只表示一口井，而一个 WellLogSession 可在同一 OpenGL 渲染表面中排列多份文档，共享深度视口和基准变换，并绘制 Cross-Well Overlay。单井视图是只有一份文档的特例；不以多个独立 QWidget 的滚动同步来拼装多井剖面，从而保证跨井覆盖层、统一拾取、帧一致性和整体导出具有单一坐标与状态来源。
