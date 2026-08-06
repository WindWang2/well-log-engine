---
status: accepted
---

# SDK Patch 保 ABI、Minor 保源码

WellLogEngine 使用语义化版本：Patch 版本保证 C++ ABI、行为与 manifest schema 兼容，Minor 版本保证源码兼容但允许重新编译，Major 版本才允许破坏公共接口。公共类采用 PIMPL、隐藏符号和导出宏，跨模块对象明确创建销毁方并避免跨 Windows CRT 释放内存；Shiboken wheel 精确匹配引擎、Qt/PySide6 与 CPython 支持矩阵。Manifest 独立版本化，读取当前及前两个版本并显式迁移，CMake 对不兼容组合在配置期失败。
