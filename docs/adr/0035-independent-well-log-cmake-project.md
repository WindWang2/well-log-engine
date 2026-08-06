---
status: accepted
---

# 新引擎使用独立顶层 CMake 子项目

新建顶层 `well-log-engine/` C++20 子项目，独立拥有公共头文件、实现、测试、基准、CMake 包配置、文档与版本，并导出分层 `WellLog::*` Targets；它不依赖 Paleo Workbench、Python 包或 geo-viz-engine，上层只能单向依赖。Shiboken/PySide6 是可选适配产物。现有 `native/well_log_core` 与 ECharts/QPainter 路径作为 Legacy 保留到新引擎通过功能和性能门槛，随后按宿主页面渐进迁移，删除另行决策。
