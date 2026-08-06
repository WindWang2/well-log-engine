---
status: accepted
---

# 交会填充在内核一次性计算几何

Curve Crossover Fill 的边界、交点与三角化在 ScenePreparer 内核一次性计算并写入 PreparedScene（PreparedFillLayer/PreparedFillRegion/PreparedFillVertex/PreparedFillTriangle），GL 与 SVG/PDF 后端只消费已准备好的几何，而不是各自重新求交或栅格化。

## 背景

交会填充的边界由两条曲线经各自 Track Scale 映射后的横坐标决定（rendering.md 第 6 节），可在不同单位、对数与反向刻度下交叉；缺测需中断填充，异轴需在共同深度片段内局部插值且不跨 Null/QC Invalid。若各后端独立求交，会出现语义漂移与 GL/SVG 不一致。

## 决策

- 交会检测与区域构建在内核完成：以上曲线深度为基准网格，对下曲线在共同有效深度片段内做显式局部线性插值，检测上减下符号变化界定封闭区域；缺测或越界即结束当前区域。
- 每个区域同时保留闭合边界环（供 SVG 单条 `<path>` 填充与点在多边形内拾取）与其三角化（供 GL 复用现有 solid/pattern 图元批次，无需新着色器）。
- 拾取 `pick_fill` 基于边界环做点在多边形内测试，返回两条依赖曲线及命中 Reference Depth。
- 上传调度（GpuUploadSchedule）计入填充三角形数，使 GL 与 SVG 的填充几何对等可断言。

## 后果

- 渲染后端保持薄：不掌握交会数学，只绘制内核产物，GL/SVG 一致性由“共享同一已准备几何”结构性保证。
- Stencil/Mask 作为复杂边界的稳健后备路径（rendering.md 第 5、8.2 节）留待后续；当前预三角化对 x-单调的交会区域已足够且更简单。
- LOD 下交点基于当前（已缩减）点集重算，保证区域边界与可见折线一致；不遗漏摘要区间内的交叉这一更严格的稳定性留作后续增强。
