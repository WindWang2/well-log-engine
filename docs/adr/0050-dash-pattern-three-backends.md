---
status: accepted
---

# DashPattern:显式虚线段数组为三后端共同来源

虚线样式以显式的 on/off 段数组(`DashPattern`,场景毫米)表达,是 Custom Polyline(及任何带描边的图元)的唯一虚线真相源;SVG / PDF / GL 三个后端从它各自映射,不改写语义。

## 背景

Custom Layer(ADR 0018)允许扩展声明 `CustomPolyline` 等图元。地质图例常需要虚线(断层线、推测边界、井迹虚段),而三个渲染/导出后端此前各自处理(或忽略)虚线:

- SVG 有 `stroke-dasharray` / `stroke-dashoffset`;
- PDF 内容流有 line dash 数组操作符(`d`);
- GL 没有原生虚线,需要 CPU 侧按弧长把折线细分为 on/off 子段后按 quad ribbon 发射。

若各自为政,同一图元在屏显(GL)与导出(SVG/PDF)间的虚线观感会漂移——#840 即发现三后端分歧(SVG 无 dashoffset、奇数数组 GL 变实线、零和数组 GL 静默删除整条折线),修复后需要一份决策文档固化语义。

## 决策

1. **结构**:`DashPattern{ segments: [on, off, on, off, ...], offset }`,单位场景毫米;`segments` 交替 on/off 并沿折线重复;空数组 = 实线;`offset` 沿虚线周期平移起点。
2. **语义规则(三后端必须一致)**:
   - 空数组 ⇒ 实线;
   - 单元素数组按 `[s, s]` 补齐(周期 = 2s);
   - 零和周期(如 `[0, 0]`)⇒ 不产生可见笔画的**空样式**,但图元本身仍有效(不得删除整条折线——#840 之前的 GL 实现会整条丢弃,已修);
   - `offset` 在三后端都生效(SVG: `stroke-dashoffset`;PDF: dash phase;GL: 细分时按 `offset mod 周期` 预跳过),SVG 侧不得忽略(#840 修复项)。
3. **映射**:SVG → `stroke-dasharray`(毫米转 px 按 96dpi 或导出物理比例);PDF → `[ ] 0 d` 数组 + phase;GL → CPU 按累计弧长切分,虚线相位对整条折线连续(每段接续,不每线段重置)。
4. **归属**:`CustomPolyline.dash_pattern` 为空值即实线;未来其它带描边图元复用同一结构,不得另造平行字段。

## 后果

- 图元的虚线观感在屏显与导出间一致;`offset`/奇偶长度/零和三类边界有共同语义(#840 的回归测试锁定三后端)。
- GL 路径承担细分成本:长折线 × 密虚线的 quad 数上升,属可接受(虚线图元通常稀少且短)。
- 序列化(manifest)必须包含 segments + offset,不得只存一个"样式名"。

## 关联

- `include/welllog/core/document.hpp`:`DashPattern` / `CustomPolyline`(注释即引用本 ADR)
- `src/scene/scene.cpp`、`src/render_gl/renderer.cpp`、`src/export_vector/svg.cpp`(三后端实现)
- ADR 0018(声明式 Custom Layer)、#840(三后端分歧修复)
