---
status: accepted
---

# 测井内核拥有交互状态

WellLogEngine 不是无状态 `draw()` 函数；每个 WellLogSession 单一持有文档版本、图道布局、深度视口、选择、活动工具及与渲染相关的撤销状态。Qt 与 Python 宿主通过 View Command 请求变化，并订阅 View Event 获取结果，不维护第二份镜像状态；文件管理、权限、项目流程和业务审批仍归宿主应用，以避免跨语言双向状态同步和反馈循环。
