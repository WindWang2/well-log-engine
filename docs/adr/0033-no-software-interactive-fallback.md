---
status: accepted
---

# 首期不提供软件交互渲染回退

图形视图要求 OpenGL 3.3 Core 并在初始化时发布结构化 Capability Report；创建失败时明确禁用图形视图，但 Table Projection、复制和 CPU 矢量导出继续可用，不静默切换 QPainter。Context 丢失或 Widget 重建时从 Prepared Scene 与 CPU 缓存恢复 GPU 资源，连续恢复失败则隔离该视图并发布错误事件，不能使 Python Qt 宿主进程崩溃。
