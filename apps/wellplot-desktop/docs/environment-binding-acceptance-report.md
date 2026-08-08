# Environment & Binding 验收报告（2026-08-08）

Goal：环境治理 —— 修复/硬化 C++ → Python binding 环境、消除 conda libstdc++
干扰、真正启用 binding-first 验证路径，补齐 raster 构建/测试链。

**结论：本 Goal 全部验收点完成**（详见各节证据）。

---

## 1. Environment before

| 项 | 审计结果（本轮实测，未采信上轮口头结论） |
|---|---|
| ctest 基线 | `build/well-log-engine-148-final`（Debug）：**70/72**，失败恰为 2 个 python binding 测试（`python.qt-embedding` / `python.qt-lifecycle-stress`） |
| GLIBCXX | **确认仍存在**：`/opt/miniconda3/lib/libstdc++.so.6: version 'GLIBCXX_3.4.35' not found (required by _QtWidgets.abi3.so)` |
| raster | **与上轮报告相反**：`welllog_export_raster` target 已无条件进入默认 build，`welllog.raster-export` 在 70/72 中已通过 |
| 系统 cmake | 确认损坏（`Could not find CMAKE_ROOT`）；须用 `.venv/bin/cmake`（4.4.2） |
| Desktop | 829 passed 基线；退出段 exit 139（全过后发生） |
| 额外发现 | **Release-only qt 测试失败**（Debug 通过 / Release 失败），上轮未暴露 |

## 2. Root causes

1. **GLIBCXX（确认根因）**：conda python 二进制 RPATH 把
   `/opt/miniconda3/lib/libstdc++.so.6` 拉进进程，其 GLIBCXX 上限 **3.4.34**；
   宿主 GCC 16 构建的原生模块需要 **3.4.35+** → 模块加载失败。
   同机系统 libstdc++ 上限 3.4.36（可用）。
2. **venv python 前缀检测失效**：harness 环境下 `/proc/self/exe` 解析为宿主
   runtime（AppImage），venv shim 无法定位自身 site-packages；conda python
   更是连 `encodings` 都导入失败。
3. **CMake 测试环境依赖**：python CTest 的 `PYTHONPATH` 只含 build 目录，
   不含 PySide6/shiboken site-packages → 换个解释器就跑不动。
4. **Release-only qt 测试失败**：fixture 用 `Q_ASSERT(session->execute(...))`
   做带副作用的初始化；`QT_NO_DEBUG` 下 Q_ASSERT **不执行条件** →
   document/presentation 从未提交 → session 空 → 全线连锁失败。
   Debug 构建因 Q_ASSERT 生效而掩盖。
5. **Desktop 退出段 segfault**：第三方（见 §Lifecycle）。

## 3. Toolchain/runtime policy

见 `docs/environment-binding-policy.md`。核心：

- **base 解释器 + 显式 PYTHONPATH**（`scripts/python_env.sh` 从
  `pyvenv.cfg` 解析 base 解释器，拒绝 conda runtime，能力探测
  PySide6==shiboken6 + numpy + CPython 3.12/3.13）。
- 统一入口 `scripts/welllog_env.sh`（导出 `WELLLOG_PYTHON` /
  `WELLLOG_PYTHONPATH` / `WELLLOG_CMAKE`，并把 site-packages 桥接为
  `PYTHONPATH`）。
- L1 诊断 `scripts/check_binding_env.py`：解释器身份、libstdc++ 实际加载
  路径与 GLIBCXX 上限、版本一致性、LD_LIBRARY_PATH/CONDA_PREFIX 污染。
- 测试用 `scripts/pytest_bootstrap.py`（`site.addsitedir` 处理 editable
  安装的 .pth）。

本机解析结果：uv CPython 3.12.13（base）+ workspace venv site-packages
（PySide6 6.11.1 / shiboken6 6.11.1 / numpy 2.5.1）；libstdc++ =
`/usr/lib/libstdc++.so.6.0.36`（GLIBCXX 3.4.36 ≥ 需求 3.4.35）。

## 4. GLIBCXX resolution

不是"unset LD_LIBRARY_PATH"式的临时处理：**构建与测试固定使用无 conda RPATH
的 base 解释器**；CMake configure 从 Python profile 步骤解析 site-packages
并注入 python CTest 的 `PYTHONPATH`（`CMakeLists.txt`），测试不依赖 shell
环境。同一 `_QtWidgets.abi3.so`：conda python 下失败（GLIBCXX_3.4.35 not
found）、受控 runtime 下导入成功 —— 差异完全由 runtime 决定，已固化。

## 5. Native binding build path

- CMake：`WELLLOG_BUILD_PYTHON=ON` + 显式 `-DPython_EXECUTABLE`（受控
  解释器）+ `source scripts/welllog_env.sh` 提供的 PYTHONPATH。
- Shiboken6 生成 `welllog/_QtWidgets.abi3.so`；configure 时版本一致性
  FATAL（PySide6==Shiboken6==Shiboken6Tools==Qt6）。
- 详见 `docs/qt-python-integration.md` §15（真实路径文档化）。

## 6. Binding-first verification

- **Engine bridge 严格模式**（E3）：`engine_bridge.py` 新增
  `WLWS_REQUIRE_NATIVE_BINDING=1` —— package 路径失败即 fail-fast（detail
  携带原始错误），禁止 extension fallback 掩盖；`EngineCapability.mode`
  区分 `native` / `extension`。最终用户默认降级体验不变。
- 新增测试 `test_well_log_workstation_engine_bridge_strict.py`（4 个）：
  严格模式走 native 包、broken-init fail-fast、非严格模式仍尝试 extension
  fallback、DISABLE 优先。
- 测试分类（本轮全部实测）：

| 测试 | 状态 |
|---|---|
| `welllog.python.qt-embedding` | ✅ 真实 binding（受控 runtime） |
| `welllog.python.qt-lifecycle-stress` | ✅ 真实 binding |
| Desktop engine_bridge 套件 | ✅ native mode（本环境绑定可导入） |
| Desktop 其余测试 | ✅ 通过（1 个环境相关失败见 §Skipped） |

## 7. Wheel verification

- 构建：scikit-build-core（PEP 517 hook 直调，绕过 pip 的 /tmp 后端通信
  缺陷；`cmake.define.Python_EXECUTABLE` 显式传入受控解释器）。
- 产物：`welllog_engine-0.1.0-cp311-abi3-linux_x86_64.whl`。
- clean env：`pip install --no-deps --target <clean-dir>` 后
  `tests/python/wheel_smoke.py` 通过。
- **abi3 真实性**：同一 wheel 在 CPython **3.11 与 3.12** 双版本实测可
  加载（3.11 单独安装 PySide6 6.11.1 依赖）；`cp311-abi3` 标签非虚标。
- 布局：`welllog/_QtWidgets.abi3.so` + py.typed + 错误类型。

## 8. Raster verification

- `welllog_export_raster` 已在默认 build（无 option）；`welllog.raster-export`
  在 ctest 全量中执行。
- **新增内容级验证**（`tests/integration/raster_export_test.cpp`）：
  - 测试侧 PNG 解码器（zlib inflate + filter-0 行重构）；
  - 尺寸与请求一致、四角 = background、曲线像素存在（非背景）；
  - gray 色彩空间按文档 luma（红 200,20,20 → 58）逐像素校验；
  - **E6 parity**：像素尺寸由 scene 物理尺寸 × DPI 推导（与 PDF/SVG 同一
    `PreparedScene + ExportSnapshot` 语义；实测 scene 100mm @100dpi → 394px）。
- `welllog.raster-export` / `welllog.export-parity` 已加入 release-gate 标签。

## 9. Lifecycle / segfault status

**第三方问题，已留下稳定复现器 + 完整诊断**
（`apps/wellplot-desktop/docs/exit-segfault-diagnosis.md`）：

- 复现：Desktop 全量 pytest 每次 exit 139（测试 100% 通过后、解释器退出期）。
- 栈（gdb 三次一致）：`exit()` → `QPlatformClipboard::~QPlatformClipboard`
  （Qt offscreen 平台 **静态单例**，符号化确认）→ Shiboken death callback →
  `Shiboken::Object::destroy` → `PyErr_Fetch()`（解释器已 finalize）→ SEGV。
- 与 welllog 无关：排除全部 10 个 import welllog 的测试文件后仍 exit 139；
  栈中无我方帧。
- 缓解尝试（QApplication 提前销毁 / Shiboken.delete）无效并已回退 ——
  崩溃对象属 Qt 静态单例，生命周期到 exit()。
- 环境记录：GCC 16.1.1 / Qt 6.11.1 / CPython 3.12.13 / PySide6+Shiboken6
  6.11.1 / offscreen。

## 10. Sanitizer findings

- ASan+UBSan（RelWithDebInfo，`-fsanitize=address,undefined
  -fno-sanitize-recover=all`）构建完成；核心子集（raster、session、undo/redo、
  manifest、container-security、TST、scene-snapshot、export-parity、
  headless-svg、time-depth、axis-ticks、multi-rate）**全部通过，零报告**。
- qt-widget 在 sanitize 构建下失败与 sanitizer 无关（即 §2.4 的 Release
  Q_ASSERT 问题，修复后 Release 全绿）。

## 11. CMake / presets

新增 presets（沿用现有 Ninja 风格）：`dev-python`（bindings+tests）、
`dev-qt`（QtWidgets adapter）、`sanitize`（ASan+UBSan）、test preset
`raster`（raster 聚焦）。python 构建仍需受控 runtime
（`source scripts/welllog_env.sh`），文档已写明。

## 12. Release gate

`scripts/run_release_gate.sh` 扩展为完整链：ctest structural label →
raster 聚焦测试 → L1 binding 环境诊断 → native binding import +
`welllog.python.*` → wheel smoke（clean dir）→ Desktop binding-first 严格
模式测试。环境变量：`WELLLOG_BUILD_DIR` / `WELLLOG_WHEEL` /
`WELLLOG_SKIP_WHEEL` / `WELLLOG_SKIP_DESKTOP`。`docs/release-gate.md` 同步。

## 13. ctest result

- **Release（build/env-gate）**：**72/72 通过**（含 2 个 python binding +
  qt-widget + qt-context-lifecycle-stress + raster 全部）。
- **Debug（build/env-gate-debug）**：72/72 通过（最终回归）。
- 基线对比：上轮 70/72（2 python 失败）→ 本轮 72/72；新增内容 = binding
  测试真实运行 + Release qt 测试修复。

## 14. Desktop pytest result

- 全量（排除既有 flake `correlation_add_remove_well`）：
  **828 passed, 1 failed, exit 139**（退出段 segfault 见 §9，第三方）。
- 1 个失败为**环境相关**：`test_main_version_subprocess` 用
  `sys.executable` 启子进程，harness 下 `sys.executable` 解析为宿主
  AppImage → 输出不同。非代码回归。
- 新增 4 个严格模式测试全部通过（native mode 实测）。

## 15. Skipped / fallback / not built

| 项 | 说明 |
|---|---|
| `welllog.qt-unavailable` | 设计上以 minimal 平台运行，通过 |
| wheel smoke 于 3.13 | 本机无可用 3.13 runtime（conda 损坏、无 uv 3.13）；以 3.11+3.12 双版本覆盖 abi3 声称 |
| `release-gate-full`（1e8） | 结构化默认；full 为可选（reference HW） |
| Desktop correlation flake | 既有已知 flake，沿用排除 |
| fallback 路径 | `engine_bridge` extension fallback 仍存在但仅非严格模式；严格模式禁用 |

## 16. Remaining environment-specific limitations

1. Desktop 套件退出段 exit 139（第三方，见 §9；升级 PySide6 后应重验）。
2. harness 下 `sys.executable` 异常 → 子进程类测试（packaging）失败。
3. conda runtime 在本机整体不可用（encodings 崩溃）——与 binding 无关的
   既有问题；受控 runtime 已规避。
4. 系统 cmake 损坏 —— 用 `.venv/bin/cmake`（已固化到 welllog_env.sh）。

## 17. Next recommendation

按 Goal §20 默认顺序：**先做 Epic C 收尾（SDK marker symbols、
TVD/TVDSS 域区间道投影）**，再进入 3D mesh bedding TST。理由不变：Epic C
收尾规模小，可进一步验证本轮硬化的 binding/export/release-gate 链。
