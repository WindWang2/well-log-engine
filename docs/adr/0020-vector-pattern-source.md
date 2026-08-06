---
status: accepted
---

# 图纹以矢量定义为共同来源

Pattern Definition 以受限矢量原语、重复单元和物理尺寸表达，并锚定场景坐标以避免平移漂移和相邻区间接缝。OpenGL 后端将其缓存为纹理图集或距离场并通过 Stencil/Mask 裁切，PDF/SVG 后端直接消费原始矢量定义；Procedural Shader 只能作为不改变结果的内置优化，不能成为独立图纹规范。
