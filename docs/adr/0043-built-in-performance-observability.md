---
status: accepted
---

# 性能可观测性是首期正式能力

引擎结构化记录 CPU Prepare、Upload、Draw、Present 阶段和异步 GPU Timer Query，并统计批次、顶点、LOD、缓存命中、上传字节、工作队列与取消任务；提供可关闭的 Profiler Overlay 和 Chrome Trace 兼容导出。Release 默认保留低开销聚合，详细 Trace 显式开启，Python 只读取聚合统计而不接收逐帧回调；指标仅含 Entity ID 与计数，不包含井名或曲线值。
