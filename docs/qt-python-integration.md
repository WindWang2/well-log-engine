# Qt 与 Python 集成

## 1. 边界

Qt/Python 集成由适配层完成：

```text
welllog-core/session/scene/render
              ▲
              │ C++ API
welllog-qtwidgets
              ▲
              │ Shiboken6
welllog-python / PySide6
```

Core 头文件不得包含 Qt 或 Python 头文件。`welllog-qtwidgets` 不包含 Python 逻辑。`welllog-python` 是唯一理解 PySide6、NumPy/Python 生命周期和 Python 异常的模块。

## 2. 原生 Widget

建议 C++ 形状：

```cpp
class WellLogView final : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit WellLogView(QWidget* parent = nullptr);
    ~WellLogView() override;

    WellLogSession& session() noexcept;
    const WellLogSession& session() const noexcept;

signals:
    void viewportChanged(const ViewportEvent&);
    void selectionChanged(const SelectionEvent&);
    void diagnosticPublished(const DiagnosticEvent&);
    void fatalViewError(const ViewErrorEvent&);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    bool event(QEvent* event) override;
};
```

这是契约草案，不是冻结 API。

Widget 负责：

- 获取/验证 QSurfaceFormat；
- GL Context 初始化、current 状态和 FBO；
- Qt 鼠标、滚轮、键盘、触控事件归一化；
- 按需 `update()`；
- View Event 到 Qt Signal；
- Widget 重挂载与 Context cleanup。

Widget 不负责：

- 解析 LAS；
- 保存项目；
- 维护第二份选择/视口；
- 在 Python 中拼接逐帧顶点；
- 以 Qt Model 作为 Core 数据事实来源。

## 3. OpenGL Profile

适配器提供一个可在 QApplication 创建前调用的配置函数，建议请求：

- OpenGL 3.3 Core；
- 24-bit depth（按实际 Pass 需要确认）；
- 8-bit stencil；
- 双缓冲；
- 明确的 sRGB 策略；
- 可配置 MSAA。

实际 Context 可能低于请求或缺少能力，`initializeGL()` 必须读取真实 Format 并形成 Capability Report。

同一顶层窗口的多个 QOpenGLWidget Format 必须兼容。跨顶层窗口共享资源是可选优化，不是正确性前提。

## 4. GUI 线程规则

- 创建、显示、销毁 Widget 必须在 Qt GUI 线程。
- 修改可见 Session 的 View Command 默认要求 GUI 线程；从其他线程调用时由 Qt 适配层排队，或返回 ThreadViolation。
- GL API 只在 `initializeGL`、`resizeGL`、`paintGL` 或显式 `makeCurrent()` 区间调用。
- 后台 worker 不访问 QObject 和 OpenGL。
- `deleteLater()`、父子所有权与 Python GC 必须遵守 QObject 生命周期。

## 5. Shiboken6

选择 Shiboken6 的原因是它与 PySide6 共享 QObject 类型系统、父子所有权和信号槽语义。参考：

- [What is Shiboken](https://doc.qt.io/qtforpython-6/faq/whatisshiboken.html)
- [Shiboken object ownership](https://doc.qt.io/qtforpython-6/shiboken2/typesystem_ownership.html)

绑定规则：

- Python 创建且无 parent 的 Widget 初始由 Python 持有；
- 设置 QWidget parent 后遵循 Qt 父子所有权；
- C++ 删除对象时 Python wrapper 必须失效；
- 不返回所有权不明确的裸 QObject 指针；
- Python callback 不能从渲染线程调用；
- 绑定生成器与目标 PySide6/Qt 使用相同 SDK Profile。

## 6. Python 包建议

```text
welllog/
├── __init__.py
├── QtWidgets.<platform-extension> # Shiboken generated
├── _buffers.<platform-extension>  # 必要时的手写转换辅助
├── typing/
│   └── *.pyi
└── resources/
    └── schema/
```

公开体验建议围绕：

- `WellLogView`
- `WellLogDocumentBuilder`
- `WellLogSession`
- `TableProjectionModel`
- `ExportRequest`
- 类型化 Event/Result

不要把 RenderGL、VBO、Shader 或 Runtime Handle 暴露给 Python。

## 7. NumPy 零拷贝

Python 适配器接收满足以下条件的数组：

- 支持 Buffer Protocol；
- dtype 可映射到 Core ScalarType；
- 一维；
- 长度与 Sampling Axis 一致；
- stride 合法；
- 数组在 Engine 持有期间不可原地修改。

适配器创建 SharedOwner，保持 Python 对象强引用直到最后一个 Document Revision 释放。

建议默认将数组视为只读，并在 Debug/诊断模式检测可写数组。无法安全接入时必须：

1. 明确拒绝；或
2. 经调用方选择后复制；
3. 在接入报告中标为 Converted Copy。

不得悄悄使用 `forcecast` 复制后仍声称 Zero Copy。

## 8. Arrow

Arrow 通过可选 `welllog-arrow`：

- 接受 Arrow C Data Interface；
- 读取类型、长度、offset、validity bitmap；
- 将 Arrow release callback 纳入 SharedOwner；
- 不把 Arrow 类型放入 Core 公共头文件。

Python 可通过 PyArrow 导出 C Data Interface，无需把大型列转成 NumPy。

## 9. Python 调用示例

以下仅展示预期使用形状：

```python
import numpy as np
from PySide6.QtWidgets import QVBoxLayout, QWidget
from welllog import WellLogDocumentBuilder, WellLogView

depth = np.asarray(depth_values, dtype=np.float64)
gr = np.asarray(gr_values, dtype=np.float32)

builder = WellLogDocumentBuilder(well_id=well_id, name="A-01")
axis_id = builder.add_sampling_axis(
    coordinates=depth,
    domain="MD",
    unit="m",
)
curve_id = builder.add_curve(
    mnemonic="GR",
    unit="API",
    sampling_axis=axis_id,
    values=gr,
)
document = builder.commit()

view = WellLogView()
result = view.session.set_documents([document])
result.raise_for_error()

host = QWidget()
layout = QVBoxLayout(host)
layout.addWidget(view)
```

真实 API 在头文件评审与 Python UX 测试后确定。

## 10. Signal 与事件频率

可以直接跨 Python：

- 文档变化；
- Viewport 交互结束；
- Selection 变化；
- Patch 提交/冲突；
- Fatal View Error；
- 低频 Diagnostic。

必须在 C++ 合并/限频：

- Hover；
- 连续滚轮；
- Pointer move；
- Frame stats；
- Cache stats；
- Append 高频刷新。

聚合性能通过主动查询或低频快照提供，不发送逐帧 Python Signal。

## 11. 表格适配

`WellLogTableModel : QAbstractTableModel` 包装 Table Projection：

- `rowCount` 可为大整数，但受 Qt Model 实际限制时使用窗口化分页；
- `data()` 按需访问 Core Buffer；
- Header 提供名称、单位、Sampling Axis 和类型；
- Selection Model 与 Session Selection Set 映射；
- Copy 使用 Core Table Exporter，不循环调用每个单元格的 Python API。

## 12. 异常映射

同步 Result 可映射为：

- `WellLogValidationError`
- `WellLogVersionConflict`
- `WellLogCapabilityError`
- `WellLogThreadError`
- `WellLogExportError`

异常种类保持少量稳定；详细错误码保留在异常对象中。异步错误通过 Qt Signal/View Event，不从未来任意线程直接抛入 Python。

## 13. Wheel 与 Qt 配套

支持矩阵至少包含：

- CPython 3.12；
- CPython 3.13；
- 项目明确支持的 PySide6/Qt 次版本；
- Windows x64；
- Linux x64。

构建时验证：

- PySide6、Shiboken6 与 Qt 库版本；
- 编译器/CRT；
- Core/QtWidgets ABI；
- OpenGL loader；
- wheel 内动态库查找路径。

禁止把来自系统 Qt、vcpkg Qt 和 PySide wheel Qt 的库混在同一进程。

## 14. 宿主迁移

建议新增一个薄的 Paleo Workbench Adapter：

- 把现有 Pydantic/NumPy 数据转换为 Document Builder 调用；
- 把现有图道配置转换为 Presentation Profile；
- 把现有页面命令连接到 Session；
- 把 View Event 连接到项目持久化；
- 保持 Legacy Widget 作为 Feature Flag 回退，直至验收完成。

Adapter 不得复制新内核的布局、LOD、选择或导出逻辑。

## 15. 真实 binding 路径（2026-08 环境治理后）

本文前 14 节是设计契约；本节记录当前代码库中实际生效的构建/加载路径。

### 构件

| 构件 | 位置 |
|---|---|
| Shiboken typesystem | `src/python/typesystem_welllog.xml`（模块 `welllog._QtWidgets`） |
| 绑定头 | `src/python/bindings.hpp`（仅 WellLogView） |
| NumPy/载荷桥 | `src/python/numpy_bridge.cpp`（`welllog::python::submit_curve` 等） |
| Python 包 | `python/welllog/`（`__init__.py` + `errors.py` + `py.typed`） |
| 生成产物 | CMake `shiboken_generator_create_binding` → `welllog/_QtWidgets.abi3.so` |
| wheel | `pyproject.toml`（scikit-build-core，`cp311-abi3` 标签） |

### 构建路径（受控 runtime）

- CMake：`-DWELLLOG_BUILD_PYTHON=ON`（强制 `WELLLOG_BUILD_QT_WIDGETS=ON`），
  显式 `-DPython_EXECUTABLE` 指向受控 base 解释器。
- 环境：`source scripts/welllog_env.sh`（解析非 conda base 解释器 +
  venv site-packages，见 `docs/environment-binding-policy.md`）。
- configure 时 profile 步骤解析 PySide6/shiboken6/shiboken6_generator 的
  cmake 路径并 prepend `CMAKE_PREFIX_PATH`；同时输出 site-packages 注入
  python CTest 的 `PYTHONPATH`（测试不再依赖 shell 环境）。
- 版本一致性由 CMake FATAL 强制：PySide6 == Shiboken6 == Shiboken6Tools ==
  Qt6（同一 SDK profile）。

### 加载路径与 Desktop resolver

`apps/wellplot-desktop/well_log_workstation/engine_bridge.py`：

1. `probe_engine()` 优先 `from welllog import WellLogView`（mode=native）。
2. 失败时（非严格模式）回退直接加载 `welllog._QtWidgets` 扩展
   （mode=extension），再回退 sys.path 扫描 `.abi3.so`。
3. `WLWS_REQUIRE_NATIVE_BINDING=1`（E3 严格模式）下只接受 native 路径；
   package 加载失败即 fail-fast，detail 携带原始错误，禁止 extension
   fallback 掩盖失败。最终用户默认行为不变。

### 已验证（本机）

- `ctest -R welllog.python.` 在受控 runtime 下全绿（qt-embedding +
  qt-lifecycle-stress）。
- wheel 在 clean target 目录安装后 `tests/python/wheel_smoke.py` 通过；
  `cp311-abi3` 标签在 CPython 3.11 与 3.12 双版本实测可加载。
- 退出期崩溃诊断见
  `apps/wellplot-desktop/docs/exit-segfault-diagnosis.md`。
