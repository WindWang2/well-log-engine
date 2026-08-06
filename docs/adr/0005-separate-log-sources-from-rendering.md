---
status: accepted
---

# 测井数据源与渲染内核分离

`WellLogEngine` 只消费与来源格式无关的 `WellLogDocument`，不直接解析 LAS、DLIS、LIS、716 或其他外部格式。文件解析器组成独立的 C++ `welllog-io` 模块，Python 数组、Arrow 数据和实时流也可通过各自的 Log Source Adapter 产生同一文档模型；这使渲染语义不受文件格式、存储策略和数据到达方式牵制。
