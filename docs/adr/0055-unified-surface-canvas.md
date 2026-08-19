---
status: accepted
---

# 统一 Surface Canvas:单井与连井共用一条路径

## 背景

ADR 0012 把多井确立为内核场景能力(WellPlacement + 共享 Display Depth 视口 + Cross-Well Overlay),但实现仍残留两条路径:

- **Qt View 层**在 `paintGL` / `update_pointer` 中以 `well_layout().empty()` 分支选择"单井 per-document 取数"或"多井 surface 取数";crosshair owner 隐式取第一口井。
- **指针交互**在组合场景上用 `PreparedScene::pick_curve` 拾取:document_id 恒为第一口井、`reference_depth = display_depth` 假设 identity DepthTransform、横向映射忽略 surface 水平窗口。
- **事件过滤**只接受 focused document 的事件:非 focused 井的 frame_ready 不触发重绘,连井剖面"部分井永不更新"。
- **单井语义**是"空 layout"特殊模式,`prepared_surface_scene()` 返回空。

## 决策

1. **单井 = one-placement surface。** Session 无显式 layout 时,`prepared_surface_scene()` 解析隐式单井 surface(focused well,否则唯一已 prepare 的文档),直接返回该井场景(共享指针同一性,零拷贝、零 compose)。`pick_surface_curve()` 同样解析。单井不再是特殊模式:`surface.wells.size() == 1`。
2. **统一访问器。** `surface_depth_viewport()` / `surface_crosshair()` / `surface_width_mm()` / `surface_horizontal_view()` / `surface_statistics()` 一组 getter 同时服务单井与连井;`viewport(doc)` 对 layout 成员委托返回 shared viewport。View 层不再有 single/multi 分支。
3. **命令路径唯一。** `PanDepthCommand` / `ZoomDepthAtCommand` / `ResetViewportCommand` 在 layout 成员上经 `SetViewportCommand` 委托到 shared viewport 广播(单井走 per-document,同一条命令)。新增 `SetFocusedWellCommand`(focused well 是 engine 持有的交互状态,ADR 0011)与 `PanSurfaceHorizontalCommand`(水平窗口平移,clamp 到 surface 范围)。
4. **拾取走 `pick_surface_curve()`。** View 的 hover/click 统一调用 per-well 拾取(`pick_curve_multi_well`):每口井左移查询、返回各自 Reference Depth(非 identity DepthTransform 下正确)、document_id 为真实持有井。Ctrl+拖拽选择手势同样用 focused 井的 transform 逆映射 display→reference。
5. **横向视口是渲染参数 + 虚拟化闸门。** `GlRenderFrame::horizontal`({left_mm, span_mm},缺省保持旧 fit-to-width)把水平窗口做成与垂直 Depth Viewport 对等的 render-time uniform:水平 pan 零 re-upload。Session 侧 `SetSurfaceHorizontalViewCommand` 仍是精确剔除窗口(导出语义不变);off-screen 井不参与 compose(`prepared_surface_scene` 井级 culling,`surface_statistics` 上报 visible/culled wells/tracks)。
6. **Compose cache 键只钉真实输入。** cache key = placements(井 id、left、场景指针)+ height + layout/overlay generation;水平窗口不进 key——平移不改变剔除集合时,组合场景指针不变,GPU 资源全量复用。
7. **Surface 事件刷新整块画布。** View 接受 layout 成员(或隐式单井)的 frame_ready/presentation_changed 并重绘;面向 Widget 的信号(documentChanged/diagnosticPublished)仍限定 focused document。`focused_well_changed` 同步 View 的 document_id。

## 兼容性

- `prepared_scene(doc)` / `viewport(doc)` / `PanDepthCommand` 等既有 API 保留,内部委托统一状态。
- 无 `horizontal` 的 `GlRenderFrame` 渲染与旧版逐像素一致(单井与小 surface 保持 fit-to-width)。
- `SetSurfaceHorizontalViewCommand` 语义不变(精确剔除窗口),仅不再 bump surface generation。

## 后果

- 单井/连井交互语义一致(同一 pan/zoom/reset/拾取/十字线代码路径)。
- 水平 pan 在剔除集合不变时零 compose、零 GPU 上传;井滚入视口时一次有界 recompose(可见井集合小)。
- 预取(prefetch margin)暂未实现:窗口跨越井边界时新井几何晚一帧出现,属已知限制。
