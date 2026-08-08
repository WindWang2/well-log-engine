# Goal 验收报告 — Epic A/B/C/D 全清单核对（2026-08-08）

**状态：四个 Epic 全部闭环 ✅**（含 Desktop 接线与 UI）。本文按 Goal 文档的验收点逐条对照代码/测试证据；原始 Goal 文档文本未存档于仓库，验收清单以本报告 + `resformstar-frs-gap.md` 为准。

**测试基线（最后全量验证）：**
- C++ ctest（`build/well-log-engine-148-final`，全量重建后串行）：**64/64 通过（exit 0）**
- Desktop pytest（排除已知 flake `test_well_log_workstation_correlation_add_remove_well.py`）：**829 passed**
- 每阶段（每个切片）均执行完整 sweep：单元 → 回归 → 集成 → Desktop smoke → SDK tests

---

## Epic A — 多采样率曲线数据模型 ✅（commits `232f816` `3c2f781` `8aad1e3` `3470468`）

| Goal 验收点 | 证据 |
|---|---|
| 每条曲线独立采样轴 | SDK `Curve.sampling_axis_id` → `SamplingAxis.coordinates`；per curve-axis 长度不变量保留（多采样率=每曲线一条轴，非共享轴参差）；`multi_rate_curve_test.cpp` 9 场景全链路 |
| 同 mnemonic 多版本 | `ImportedCurve.version`；leaf id `doc:mnemonic:version`（raw 保持历史形态）；display_set 版本感知解析；`leaf_id_for_curve` |
| 显式重采样派生操作 | `curve_resample.py` 线性插值到规则目标轴，null/NaN 间隙传播不造值；`curve_resamples.json` 持久化、加载时重算；菜单「曲线重采样…」 |
| 持久化/绑定/导出/表格投影 | manifest v2 `nominalInterval`（可选写出、宽容读）；engine payload per-curve depth；表格投影 per-curve 轴独立分表（真实 depth 不隐式对齐） |
| 不破坏非破坏 curve-edit/version | 原始/校正曲线编辑机制未动；`edited-*` 与 `derived-*` 共存测试通过 |

## Epic B — SDK Depth/Domain Infrastructure ✅（commits `aae42f5` `36f75b9` `5aa69bb`）

| Goal 验收点 | 证据 |
|---|---|
| 真 TWT domain（禁止 TVD×constant 冒充） | `TimeDepthRelationship`（`scene/time_depth.hpp`）：双向单调显式映射、单位/provenance、空=unavailable 返回 NaN 显式降级 |
| simultaneous multi-axis | `ticks_for_secondary_window`（任一向单调 (ref,display) 点，TVDSS↔MD 递减可用）；单井画布右缘副轴（MD + TVDSS，测斜+KB 控制点，无则显式不绑定） |
| depth/domain projection | `ticks_for_reference_window`/`ticks_for_twt_window`；`reference_to_display_at` 正向映射 |
| 单井 PDF 深度刻度 | `export_scene_pdf` `show_depth_ruler`（默认 false 兼容）；Desktop 引擎导出默认开启 + 旧绑定逐级回退 |
| 从 reversible depth transform chain 扩展，不建第二套体系 | ADR 0013 chain：Sampling Axis → Reference Depth → Display Depth；TWT/副轴均为 chain 的 tick 语义扩展，无新坐标类 |
| 授权 axis/tick 语义单一来源 | `nice_axis_ticks`/`format_axis_tick_label` C++ 权威；Desktop 绑定优先 + Python 回退 + parity 测试锁死（同纪律复用于 TST） |

## Epic C — 地质/工程数据模型 + UI ✅（数据 `cb76e5b` `b4b4515` `8018ed8`；UI `308e5b8` `fc8f18f` `a50e111` `617862d`）

| Goal 验收点 | 证据 |
|---|---|
| 统一地层层序（可扩展层级，非封闭 enum） | `STANDARD_LEVELS` 八级为常量非封闭枚举；`stratigraphy.py` 层级导航 + 环/孤儿/重复校验；`stratigraphy.json` 工区级边车 |
| Core 不重复 core_photo_model | 照片保持 CorePhotoModel 所有权；CoreSample 仅 `photo_segment_id` 引用 |
| Well Test typed payload | `WellTestInterval.payload`（结构化公共字段 + opaque dict 可扩展 schema）；对话框往返不丢 |
| Perforation | `perforation_model.py`（密度/相位/状态/完井引用）+ 对话框 |
| C5 显式 depth+unit+depth_domain | `DEPTH_DOMAINS`（MD/TVD/TVDSS）；所有深度承载对象显式声明 |
| 先数据模型后 UI | 数据模型先交付，UI 三件套后交付（层序字典编辑器、岩心/试油/射孔对话框、试油/射孔道） |

## Epic D — True TST ✅（`c08d3fe` `64eb465` `e395f86` `616cc8e`）

| Goal 验收点 | 证据 |
|---|---|
| 可证明正确的 planar/local 模型优先 | `scene/tst.hpp`：`tst_from_measured_interval`（L·|w·n|）/`tst_from_interval_points`（|Δp·n|）；9 组解析几何 fixtures |
| API 不假装平面永远成立 | 单元向量校验（缺失=输入错误）；分段模型层须有序/不重叠/正厚度；未声明区贡献 0 属调用方责任 |
| ThicknessKind 显式区分 | MD/TVD/apparent/TST 四值枚举 |
| 扩展到复杂 surface（D2） | `tst_through_layers` + `tst_along_path`（surface intersection）；12 组解析几何 fixtures |
| Desktop 接线（D3）+ UI（D4） | `tst.py`（mirror + 奇偶校验 + 绑定优先 resolver + `path_from_trajectory` + `bedding.json` 边车）；`tst_dialog.py`「真地层厚度 TST…」（实时计算列、无测斜「—」显式不可用）；18 + 6 pytest |

## 全局约束检查

| 约束 | 状态 |
|---|---|
| 主 agent 统一架构 | ✅ 全部切片由主 agent 完成 |
| 不重复 depth transform 实现 | ✅ depth transform 仅在 SDK；Desktop 绑定优先 + 回退 |
| 不新增第三套 mapping | ✅ correlation `0.12/0.92` 经验映射**保留 gap**（宿主 Qt 布局近似，SDK transform 无法描述；gap line 38 记录决策，仅当宿主布局统一后替换） |
| 枚举 append-only | ✅ `ErrorCode`/`MessageKey` 新值追加尾部；`undo_redo_test.cpp` 数值稳定性断言（19/20/21）在 ctest 内强制 |
| 只在需要时修改 geo-viz-engine | ✅ 本 Goal 全程未改 geo-viz-engine（git 验证） |
| SDK 不反向依赖 parent | ✅ engine 独立构建；parent 仅 submodule bump |
| 每阶段完整 sweep | ✅ 每个切片后 ctest + Desktop 全量 |

## 保留 gap / 已知问题（均非阻塞）

- correlation 0.12/0.92 经验映射（决策保留，见上）
- SVG 引擎导出为单场景路径（无分页页带，ruler 属分页 PDF 特性）
- **2026-08-08 环境治理后已关闭**：conda libstdc++ 缺 `GLIBCXX_3.4.35` 的
  2 个 python binding ctest（受控 runtime + CMake site-packages 注入，
  现 Release/Debug 均 72/72 全绿）；raster 未进入受控 build 的疑虑
  （raster target 本就在默认 build，现补内容级验证并纳入 release-gate 标签）
- `append-coalescing-stress` 在 -j8 下 flaky（单独跑通过）
- Desktop 套件退出段 segfault（exit 139，全过后发生）——本轮定位为第三方
  （Qt offscreen 静态单例 × Shiboken 退出期 UAF，与 welllog 无关；
  详见 `docs/exit-segfault-diagnosis.md`）
- 系统 `cmake` 损坏 → 构建须用 `.venv/bin/cmake`（已固化到
  `scripts/welllog_env.sh`）
- Desktop `test_main_version_subprocess` 在 harness 下失败
  （`sys.executable` 解析为宿主 AppImage；环境相关，非代码回归）

## 结论与后续

**Goal 四个 Epic 全部验收通过**（+ 2026-08-08 环境治理 Goal 全项验收，见
`docs/environment-binding-acceptance-report.md`）。后续可选方向（按推荐序）：
1. Epic C 收尾：SDK marker 符号（casing_shoe）、TVD/TVDSS 域区间道投影
2. Epic D 终极扩展：三维曲面（非平面 mesh）bedding TST
3. （可选）PySide6 升级后重验 Desktop 退出段 segfault 是否消失
