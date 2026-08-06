# 插件 Runtime 状态（P.SPEC / T17 / #305）

**状态：P.SPEC ✅（首发）**；P.DISC / P.LOAD / P.REG **不在首发**（ADR 0055）。

| 阶段 | 状态 | 说明 |
|------|------|------|
| **P.SPEC** 内置扩展目录 | ✅ 首发 | `extension_points.BUILTIN_EXTENSION_POINTS` + `list_extension_points()`；只读目录，不扫描/不加载 |
| **P.DISC** 发现第三方扩展 | ❌ 不在首发 | 无 entry-point / 目录扫描 |
| **P.LOAD** 加载第三方代码 | ❌ 不在首发 | 无第三方代码执行面（对照 ADR 0042 安全立场） |
| **P.REG** 注册与管理 | ❌ 不在首发 | 无插件管理 UI |

## 首发能力（P.SPEC 目录）

- **Custom Layer**（`wellplot.custom_layer`）：声明式 polyline / triangle /
  rect / symbol 图元，内核预生成场景（ADR 0018 / 0046）；宿主按既有权值渲染。
- **engine embed**（`wellplot.engine_embed`）：WellLogView 引擎画布嵌入（可用时）。
- **export backend host**（`wellplot.export_backend`）：宿主导出后端插槽（PDF/SVG/PNG/Qt paint）。

## 边界

- 目录**不**隐式暗示运行时加载；任何「插件」表述只指向目录能力。
- P.LOAD 开启前必须完成隔离与信任模型设计（ADR 0055 §4）。
