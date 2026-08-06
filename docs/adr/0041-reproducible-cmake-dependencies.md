---
status: accepted
---

# CMake Presets、锁定 vcpkg 与独立 Qt Profile

独立工程使用 CMake Presets 和可安装的 Package Config，默认以锁定 baseline 的 vcpkg manifest 管理非 Qt 依赖；Qt 与 Shiboken 来自版本一致并可校验的 Qt/PySide SDK Profile，不与其他 Qt 构建混用。Release 构建禁止隐式联网，OpenGL 加载代码固定生成，Core 可构建静态或动态库，适配器发布平台动态库与 wheel。CI 覆盖 MSVC/GCC/Clang 及内存检查，每个发布包附 SBOM、许可证和完整工具链清单。
