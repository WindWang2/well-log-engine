---
status: accepted
---

# PySide6 适配使用 Shiboken6

Python Qt 适配器使用 Shiboken6 生成与 PySide6 对象模型兼容的 `WellLogView` 绑定，并显式声明 QWidget 父子所有权及 C++/Python 生命周期。NumPy、Arrow 与核心只读缓冲区之间的零拷贝转换由该绑定层负责；纯 C++ 核心和 Qt Widgets 库不依赖 Python，现有 pybind11 算法扩展也不构成新架构的 ABI。绑定 wheel 按受支持的 Qt/PySide6 与 CPython 版本矩阵构建和验证。
