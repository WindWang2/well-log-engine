---
status: accepted
---

# 文字使用 Qt 无关的 Unicode 管线

核心以 UTF-8、HarfBuzz、FreeType 和 ICU 完成文本整形、字形轮廓及 Unicode 换行，Font Resolver 按项目、内置回退、系统字体顺序解析并记录字体指纹。整形后的字形位置进入 Prepared Scene，OpenGL 使用按字体与字号分桶的 Glyph Atlas，PDF/SVG 复用同一布局并嵌入字体子集或输出轮廓；水平、旋转和竖排布局均由该管线表达，缺失字形必须诊断且只能使用明确回退。
