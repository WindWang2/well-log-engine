---
status: accepted
---

# 使用保留式 Prepared Scene 与版本化增量失效

WellLogSession 的文档、布局、样式、Depth Transform 和视口分别版本化，变化只使依赖它的 Track、Layer、文本布局、几何或 GPU 资源失效。后台准备可见内容并形成有序 Frame Plan；纯平移优先复用几何并更新变换，缓存越界时异步补充 LOD。OpenGL 屏幕后端与 PDF/SVG 导出后端共同消费 Prepared Scene，避免在每次 paintGL 或导出时重新查询和解释业务对象。
