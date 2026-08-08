# Epic C 收尾验收报告（2026-08-08）

两个切片：**SDK marker symbols** + **TVD/TVDSS 域区间道投影**。

## 切片 1 — SDK marker symbols（casing_shoe 等）

| 验收点 | 交付 | 证据 |
|---|---|---|
| 标准符号语义 | `SymbolKind` append `triangle_down`/`shoe`（**append-only**，数值稳定性由 undo_redo 断言继续覆盖） | `core/document.hpp` |
| 权威映射单一来源 | `scene::symbol_for_marker_semantic`：formation_top→triangle_down、fault→cross、fluid_contact→diamond、casing_shoe→shoe、custom→circle | `scene.hpp`/`scene.cpp` + `marker_symbol_test` 映射表断言 |
| 共享字形几何 | `scene::symbol_glyph`（闭包多边形 + kind 透传；vector 后端仍发射精确圆弧/笔画） | `marker_symbol_test` 几何边界/闭合断言 |
| semantic 贯通 PreparedScene | `PreparedMarker.semantic` + `MarkerLayerSpec.draw_symbols`/`symbol_size`（默认 false，**向后兼容**） | prep 拷贝 + `prepared_markers_carry_semantic_and_layer_flags` |
| 三后端渲染 | SVG（`marker-symbol-<id>` + `data-semantic`）、PDF（shoe arch 路径，FlateDecode 流内断言）、raster（像素级：arch 在 marker 线下 6px 内、draw_symbols=false 对照空白） | `marker_symbol_test` 5 场景 |
| binding | payload `markers[].semantic`（宽容解析，未知→custom，缺省→formation_top 历史行为）+ `marker_symbols` 开关（单井/多井两路径） | `test_qt_embedding.test_marker_semantics_reach_the_exported_svg` |
| Desktop | `FormationTop.semantic` 字段 + 三处 marker payload 透传（legacy 无 semantic 不伪造键） | `test_marker_semantic_reaches_the_engine_payload` |

## 切片 2 — TVD/TVDSS 域区间道投影

| 验收点 | 交付 | 证据 |
|---|---|---|
| 递减 display 域（TVDSS） | `DepthTransform` 校验放宽为 display **双向严格单调**（方向翻转仍拒绝）；`map_display_to_reference` 改为方向无关括号搜索（原实现对递减 display 内部值算错）；scene interval/marker culling 与 image tile clamp 方向无关化；显示窗口允许 top>bottom | `tvdss_projection_test`（TVD 递增 + TVDSS 递减投影、逆映射 round-trip、SVG 导出、退化校验）；`depth_transform_overlay_test` fold 语义更新 |
| 区间道按显示域投影 | interval/marker 走 `depth_to_top`（display 线性→mm），递减窗口下更深 MD 仍渲染在页面下方 | `tvdss_projection_test` 区间 20mm/60mm、marker 50mm 断言 |
| binding | 单井 `submit_multi_track` 补 `depth_transform` 解析（与多井对齐；多井兼容旧键 `transform_points`） | `test_tvdss_engine_submission_accepts_decreasing_transform`（端到端：递减 transform 被原生引擎接受） |
| Desktop 接线 | `survey_depth_transform`（轨迹→MD→display 控制点，tvd/tvdss，非有限值过滤）；`presentation_to_multi_track_payload`/`presentations_to_multi_well_payload`/`submit_multi_well_presentations` 透传；shell 对比图 datum tvd/tvdss 时按井构建（TVDSS 无 KB → 显式跳过） | bridge 测试 4 个新场景（含端到端） |

## 回归

- **ctest（Release，build/env-gate）：74/74 通过**（新增 `welllog.marker-symbols`、`welllog.tvdss-projection`）；并行/串行双验。
- **ctest（Debug，build/env-gate-debug）：74/74 通过**。
- **Desktop pytest：838 = 837 passed + 1 环境相关失败**（packaging 子进程测试，harness `sys.executable` 异常，同前轮）；退出段 exit 139 为既有第三方问题（见 `exit-segfault-diagnosis.md`）。
- enum append-only：SymbolKind 仅尾部追加；undo_redo 数值断言在 74/74 内。

## 遗留（两项均已交付，见 epic-d-surface-tst-acceptance-report.md）

- ~~raster 后端暂不渲染 `PreparedSymbol`（SymbolOccurrence）~~ ✅：`rasterize_tile` 新增 symbol pass（interval 与 curve 之间，符号在曲线之下，与 SVG/GL 语义对齐；cross 走 draw_line 双对角线，其余 kind 用 `symbol_glyph` + `fill_polygon`），内容测试 `raster_draws_symbol_layer_pixels`（字形中心像素 + 无杂散色块）。
- ~~单井引擎预览（`open_engine_preview`）暂不应用 tvd/tvdss transform~~ ✅：`submit_multi_track_presentation` / `load_presentation_into_view` 透传 `depth_transform`，单井提交点按多井模式从测斜构建控制点。
