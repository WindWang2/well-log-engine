---
status: accepted
---

# 同步错误返回 Result，运行期问题进入诊断流

公共 C++ 操作以 `Result<T, Error>` 返回稳定错误码、严重程度、实体上下文和本地化参数，异常不得跨越 DLL、C ABI、Qt 回调、帧循环或析构边界；局部持续运行问题进入 Diagnostic Stream。Debug 断言捕获程序员不变量，Release 将可恢复问题隔离为受控错误。PySide6 将同步失败映射为少量类型化异常，异步失败发布 Qt 信号/View Event；日志只记录身份、规模与摘要，不记录原始曲线或敏感井数据。
