---
status: accepted
---

# 首期使用单 GL 上下文与 CPU 工作线程池

Qt GUI 线程独占 QOpenGLWidget、主 OpenGL Context 和全部 GL 调用；工作线程池只执行 LOD、裁切、布局、索引和几何预处理，并返回带 Document Revision 的不可变结果，过期结果直接丢弃。GPU 数据由 GUI 线程按每帧预算分批上传并使用轮换缓冲区避免争用；首期不创建后台共享 GL Context，以规避跨驱动资源共享、合成同步及销毁竞态。Python Widget API 限定在 GUI 线程，耗时绑定调用释放 GIL。
