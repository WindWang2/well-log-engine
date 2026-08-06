---
status: accepted
---

# 首期以 Qt Widgets 适配 WellLogEngine

首期交付原生 C++ `welllog-qtwidgets` 适配库，以 `QOpenGLWidget` 承载 OpenGL 上下文、逐帧绘制和输入事件，并提供可加入 PySide6 布局的 Python 绑定。Qt、QObject 和 QWidget 类型只存在于适配层；核心 API 保持 Qt 无关。QML/Qt Quick 不进入首期范围，未来若需要则通过独立的 `welllog-qtquick` 适配器接入同一内核。
