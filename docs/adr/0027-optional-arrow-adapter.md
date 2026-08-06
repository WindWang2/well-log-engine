---
status: accepted
---

# Arrow 是可选适配与存储实现

`welllog-core` 只公开轻量的类型化只读缓冲区、Null Bitmap、Sampling Axis 和生命周期令牌，不暴露 Arrow 类型、分配器或头文件。`welllog-arrow` 通过 Arrow C Data Interface 零拷贝适配数组，Python 层通过 Buffer Protocol 适配 NumPy，`welllog-io/store` 可使用 Arrow IPC 与 mmap 缓存大型列式数据；由此保留 GB 级零拷贝能力而不将 Arrow 的体积、ABI 和发布约束施加给所有宿主。
