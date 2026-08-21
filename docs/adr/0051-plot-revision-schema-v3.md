---
status: accepted
---

# Plot Revision:宿主侧 per-plot 单调修订计数器(schema v3)

WellPlot Workstation 的每个图件(plot)拥有一个**单调递增的修订计数器**,持久化于 `plots/<id>.json` 顶层字段(plot schema v3)。它解决"复合图如何知道源图变了"的宿主侧问题;与引擎的 DocumentRevision 是**两套概念**,互不替代。

## 背景

复合图(油藏综合图)内嵌多个源图面板,源图变化后需要刷新。引擎的 ViewEvent / DocumentRevision 是 C++ 侧观察者,Workstation(宿主)没有它们的 Python 观察面;宿主在自己的命令式保存/渲染调用之后需要一条进程内广播,且重启后语义不回退。

## 决策

1. **信号总线**:`well_log_workstation/events.py` 的模块级单例 `plot_bus`;信号 `plot_changed(plot_id, revision)`。
2. **计数语义(三个入口,全部只进不退)**:
   - `save_plot_document` 保存时 bump——保存意味着进入新的已提交状态;
   - `load_plot_document` 加载时以 `restore_plot_revision` 恢复持久化值,**max 语义**(取内存与持久化的较大者,任何路径都不回退);
   - `emit_plot_changed` 发射时 bump 并广播。
   - 创建/显示路径经保存 bump,不直接 emit(保存点与发射点不重叠,避免双 bump)。
3. **持久化**:revision 存 `plots/<id>.json` 顶层字段,schema v3;旧 schema 文件按缺省 0 读取,首次保存升级。
4. **与引擎 DocumentRevision 的关系**:DocumentRevision 由内核维护、描述引擎内文档内容版本(经 `WellLogView.documentChanged` 暴露到 Python);Plot Revision 是宿主侧图件保存状态。消费方(如复合面板)应以 Plot Revision 判断刷新/使 snapshot 失效,不把二者混用。

## 后果

- 复合图可按 revision 判断源图是否真的变化(避免重复刷新);崩溃后重载不产生时间倒流。
- 写盘失败时内存计数器前进但文件未提交——单调性保留(可能出现编号空洞),可接受。
- 若未来引擎观察面进入 Python,复合图应迁移到 DocumentRevision 驱动;本机制退化为宿主保存状态的审计痕迹。

## 关联

- `apps/wellplot-desktop/well_log_workstation/events.py`(实现,注释即引用本 ADR)
- `plot_document.py`(save/load 持久化)、`tests/test_well_log_workstation_plot_revision.py`(锁定 save-bump / restore-max / 单调语义)
- #251(Phase-2 T7,引入信号总线)
