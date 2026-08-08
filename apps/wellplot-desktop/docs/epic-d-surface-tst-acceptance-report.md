# Epic D 高阶扩展验收报告 — Surface-based True TST（2026-08-08）

三维曲面（非平面 mesh）bedding 的 TST 计算 —— `goal-acceptance-report.md` L89
「Epic D 终极扩展」与 `resformstar-frs-gap.md` L60 行尾未决项「三维曲面（非平面）
bedding 后续」的交付。

## 切片 E1 — 引擎：SurfaceGrid 曲面模型 + 几何求交

| 验收点 | 交付 | 证据 |
|---|---|---|
| 曲面数据结构 | `SurfaceGrid`：规则 (x, y) 网格上的 z = f(x, y)（TVD，Z 向下），`y_nodes × x_nodes` 行主序高度数组；契约：每维 ≥ 2 节点、有限正步长、有限原点/高度、矩形覆盖区内定义 | `include/welllog/scene/tst.hpp` |
| 几何求交（非声明边界） | `tst_along_surface_path`：逐腿按覆盖区逐 cell 遍历，cell 内 d(t) = z(t) − f(x(t), y(t)) 为二次型，三样本精确拟合 + 精确求根；**双根（相切）不算穿越**；相邻 cell/腿共享边界的重复根去重 | `src/scene/tst.cpp` |
| 曲面法向插值 | 交点处双线性梯度插值 → 单位法向（Z 分量向下，与 BedNormal3D 约定一致）；单元 TST = Σ 段长·\|ŵ·n̂\|，n̂ 为单元顶/底两曲面在段中点插值法向的归一化平均（**显式局部平行近似**，非静默平面回退） | `tst.cpp` + 头注释 |
| 单元语义（栈模型） | 相邻曲面界定单元（unit i = surfaces[i]…surfaces[i+1]）；组件行走：candidate 两侧单元内/外状态相异才算真穿越（相切/擦过跳过）；**每个 candidate 都做对侧校验**（曲面沿井路径交叉/重叠 = 输入错误，绝不静默合并——反演交点恰好是不改变状态的 candidate，因此校验必须对全部 candidate 执行） | `tst.cpp` 组件行走 + `surfaces_crossing_between_path_points` fixture |
| 契约纪律（不静默回退） | 网格违约/覆盖区外出/倒置或交叉曲面/重合段/路径违约 → `ErrorCode::invalid_geometry`；空栈与单曲面 = 合法零结果（同空层书）；零 TST 合法（井平行 bedding）；顶/底曲面在路径点严格有序（接触 = 错误） | 校验分支 + fixtures |
| 解析几何 fixtures | `tests/integration/tst_surfaces_test.cpp` **18 组**：水平平面×直井（TST=Δz）；x 倾/xy 双向倾×直井非节点（Δz·cos δ）；水平平面×两腿斜井（TST=TVD 经典结果）；正弦折皱网格×节点直井；**三曲面栈**（湾形进出/起点在单元内/终点在单元内/走向平行腿零贡献/单曲面多次穿越）；空栈/单曲面合法零；单元在井下方；走向平行零 TST；折皱脊线相切（非穿越）；路径节点恰在曲面上（候选合并）；网格 7 类违约；覆盖区 2 类；倒置/接触曲面；路径点间交叉（对侧校验）；重合段；路径 5 类违约 | `tst_surfaces_test.cpp`（与切片 1 的 9 组、切片 2 的 12 组同量级） |
| 回归 | Release `build/env-gate` 全量 ctest **75/75**（新增 `welllog.tst-surfaces`，串行防 flake） | ctest 输出 |

## 切片 E2 — Desktop：mirror + resolver + bedding.json v2 + 对话框

| 验收点 | 交付 | 证据 |
|---|---|---|
| Python mirror 精确镜像 | `tst.py`：`SurfaceGrid`/`SurfaceGridSpec`、`_mirror_tst_along_surface_path`（网格校验、cell 遍历求交、二次求根、组件行走、对侧校验、重合段检测逐行镜像 C++） | `well_log_workstation/tst.py` |
| 奇偶校验锁死 | 同 fixtures 值断言：水平平面 200、倾平面 200/√1.0625、折皱节点 200/√1.01、三曲面栈 600/√1.0625（measured 1200、normal_dot 审计）、相切、走向平行 0、路径节点穿越、空/单栈 0；全部 ValueError 分支（网格/覆盖区/倒置/接触/交叉/重合/路径） | `test_well_log_workstation_tst.py` 12 个新测试 |
| binding-first | `tst_along_surface_path` 走既有 resolver（绑定存在即胜出，缺失回退 mirror），`_result_from_bound` 归一化 | fake-welllog dispatch 测试 |
| bedding.json v2 | `BEDDING_SCHEMA_VERSION = 2`；层可携带 `top_surface`/`bottom_surface` 网格（origin/steps/nodes/z_tvd 行主序）；宽容读（v1 文件原样可读）、原子写；**恰好一个曲面 = 不一致 spec**（`layer_surfaces` 抛 ValueError，绝不静默平面回退） | `test_bedding_sidecar_surface_roundtrip` / `test_layer_surfaces_inconsistent_spec` / v1 兼容测试 |
| 对话框 | 「形态」列（平面/曲面，只读）；曲面行 TST = `tst_along_surface_path(path, [顶面, 底面])` 单单元（crossing 几何计算）；不一致曲面行 TST 显式「—」；`value()` roundtrip 原样保留声明的曲面（含部分曲面） | `test_well_log_workstation_tst_dialog.py` 5 个新测试 |
| 回归 | tst + tst_dialog 测试文件全绿（44 项）；全量 Desktop pytest 见集成轮 | pytest |

## 遗留（非阻塞）

- 曲面网格尚无导入 UI（网格数据经 bedding.json v2 边车/测试进入；未来可加网格导入器）。
- 相邻层共享曲面会重复内嵌网格数据（共享 surfaces 数组 + id 引用可后续优化）。
- 相关性图（multi-well）路径未接 surface TST（当前为单井对话框使用）。
