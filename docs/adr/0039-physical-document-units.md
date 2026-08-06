---
status: accepted
---

# 文档物理单位与屏幕交互单位分离

Track 宽度、字体、线宽、图纹和页边距以物理单位定义，Document Scale 决定 PDF/SVG/打印尺寸；屏幕可使用交互适配模式并显示 Effective Screen Scale，或在用户校准 DPI 后预览真实物理比例。设备像素比只影响栅格化精度，拾取容差与交互手柄使用设备无关像素；导出不得复用窗口像素尺寸或未经校准的系统 DPI。
