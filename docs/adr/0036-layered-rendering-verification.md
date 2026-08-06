---
status: accepted
---

# 渲染验证分为性质、语义、图像与性能四层

核心以性质测试验证深度变换、LOD、Null、Scale、Patch 和身份，以 Prepared Scene 语义快照验证批次、裁切、字形和图纹；固定字体及 Mesa 环境执行容差图像回归，Windows/Linux 真 GPU 进行夜间兼容测试。固定参考工作站记录一亿点场景的帧时分位数、首帧、缓存、上传量和内存并作为发布门禁，超过基线 10% 的回退必须阻止发布或由负责人基于证据更新基线。C++、Qt、PySide6、导出、模糊输入、Context 生命周期和 Python GC 均纳入契约与压力测试。
