---
status: accepted
---

# 首期采用 OpenGL 3.3 Core 渲染后端

WellLogEngine 首期只交付 Raw OpenGL 渲染后端，以 OpenGL 3.3 Core 为最低能力基线；OpenGL 4.4/4.5 的持久映射等能力只能作为运行时检测后的等价增强路径。内核保留窄的后端边界以便未来增加 Skia、Qt RHI 或软件实现，但首期不承担多后端一致性成本；PDF/SVG 等矢量输出由独立导出后端消费同一场景语义，不通过 OpenGL 截图生成。
