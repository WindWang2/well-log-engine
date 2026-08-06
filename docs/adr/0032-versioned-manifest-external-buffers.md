---
status: accepted
---

# 强类型运行时 API 配合版本化 Manifest

C++ 调用方通过强类型 Builder/API 构造文档、图道、图层和会话；布局、样式、Pattern Definition、Document Patch 与会话状态使用带 `schemaVersion` 的 JSON Schema。大型数值缓冲区不进入 JSON，manifest 仅保存 Entity ID、数据源引用、校验摘要和缓冲区描述，实际数据由源资产、Arrow IPC/mmap 缓存或宿主系统持有。便携包由 `welllog-io` 可选封装；XLSX、XML、CSV 和图形输出不是无损回读格式，FlatBuffers 仅在未来跨进程瓶颈得到实测证明后考虑。
