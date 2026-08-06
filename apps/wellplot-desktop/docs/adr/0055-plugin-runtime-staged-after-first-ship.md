---
status: accepted
---

# 插件运行时（Plugin Runtime）在首发之后分阶段（P.SPEC → P.DISC → P.LOAD → P.REG）

**决策**：WellPlot Desktop 的插件运行时**不在首发**（first ship）提供。首发只
交付**声明式扩展目录**（`extension_points.BUILTIN_EXTENSION_POINTS`，P.SPEC）——
Custom Layer 图元 + 同工具链嵌入——**不加载任何第三方代码**。完整插件运行时
（发现 / 加载 / 注册）作为后续阶段实施。

## 背景

- T17 / #305 要求为「插件 Runtime」定一条产品边界：首发的图件扩展能力是什么、
  什么**明确不在**首发。
- 既有能力：`section_geometry` 声明式 Custom Layer（ADR 0018/0046）由内核
  预生成场景；宿主不执行外部脚本。
- 风险：若首发就做通用插件加载，会引入不可信资产执行面（对照 ADR 0042 的
  安全立场）与跨版本 ABI 契约，远超首发范围。

## 决策

### 1. 分阶段定义（P.SPEC 先行，其余不在首发）

| 阶段 | 内容 | 首发？ |
|------|------|--------|
| **P.SPEC** | 内置扩展目录：声明式 Custom Layer 图元、engine embed、host export backend；`list_extension_points()` 只读目录，不扫描/不加载 | ✅ 首发 |
| **P.DISC** | 发现第三方扩展（entry points / 目录扫描） | ❌ 不在首发 |
| **P.LOAD** | 加载第三方代码（隔离执行） | ❌ 不在首发 |
| **P.REG** | 注册/管理第三方扩展的 UI 与生命周期 | ❌ 不在首发 |

### 2. 首发边界

- `wellplot.custom_layer` / `wellplot.engine_embed` / `wellplot.export_backend`
  在目录中标记 `available_in_first_ship=True`；目录**不**隐式暗示运行时加载。
- Custom Layer 仍是**声明式**：宿主/内核按既有权值渲染，不执行插件字节码。
- 任何「插件」字样在 UI/文档中只指向目录能力，不承诺加载第三方扩展。

### 3. 明确不做（首发）

- 不扫描 entry points、不执行第三方 Python/C++ 扩展。
- 不定义插件 ABI / 版本契约。
- 不在首发 UI 提供插件管理界面。

### 4. 后续阶段开启条件

- 出现真实第三方扩展需求且首发能力被证明不够用时，按 P.DISC → P.LOAD →
  P.REG 顺序推进；P.LOAD 必须先行完成隔离与签名/信任模型设计。

## 后果

- 首发交付边界清晰：能力目录 + 声明式扩展，无第三方代码执行面。
- 后续插件运行时升级为**加性**能力，不破坏首发目录 API（`list_extension_points`
  保持只读兼容）。
- `docs/plugin-runtime-status.md` 跟踪各阶段状态；本 ADR 关闭 T17 的
  「插件 Runtime 边界」门禁。
