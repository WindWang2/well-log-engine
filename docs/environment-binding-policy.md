# Python binding 构建/运行环境策略（E1/E2）

本文件是 WellLogEngine C++ → Python (Shiboken) binding 链的受控环境策略。
目标：**binding 的构建、测试、wheel 验证不依赖"开发者 shell 里恰好激活了什么"**，
并且把已知的 conda libstdc++ / GLIBCXX 干扰变成可诊断、可复现的问题。

## 1. 已知环境风险（本机 2026-08 实测）

| 风险 | 现象 | 根因 |
|---|---|---|
| conda libstdc++ 封顶 | `GLIBCXX_3.4.35 not found (required by _QtWidgets.abi3.so)` | conda python 的 RPATH 把 `/opt/miniconda3/lib/libstdc++.so.6` 拉进进程，其 GLIBCXX 上限 3.4.34；宿主 GCC 16 构建的原生模块需要 3.4.35+ |
| venv python 前缀检测失效 | `Could not find platform dependent libraries <exec_prefix>`，venv site-packages 不可见 | 沙箱/harness 下 `/proc/self/exe` 解析为宿主运行时（如 AppImage），venv shim 无法定位自身 |
| conda python 本身不可用 | `Failed to import encodings` | 同上（exec_prefix 解析失败） |

## 2. 策略：构建环境显式化 + runtime 隔离

1. **用 base 解释器，不用 venv shim**：`scripts/python_env.sh` 从 `pyvenv.cfg`
   的 `home` 字段解析出真正的 base 解释器，并通过显式 `PYTHONPATH` 注入 venv
   site-packages。
2. **拒绝 conda runtime**：`sys.base_prefix` 含 conda 即拒绝并给出原因
   （libstdc++ 封顶会破坏原生模块加载）。
3. **版本一致性强制**：PySide6 == shiboken6，CPython 3.12/3.13，
   PySide6/shiboken6/Qt 同 SDK profile（CMake 侧已有 FATAL_ERROR 检查）。
4. **GLIBCXX 可诊断**：`scripts/check_binding_env.py` 报告进程实际加载的
   libstdc++ 与 GLIBCXX 上限（≥ 3.4.35 才健康）。

## 3. 使用方式

```bash
# 解析受控 runtime（打印 eval-able 导出）
eval "$(well-log-engine/scripts/python_env.sh)"

# L1 诊断
"$WELLLOG_PYTHON" well-log-engine/scripts/check_binding_env.py

# 构建（configure/build 均需上述 PYTHONPATH）
cmake -S . -B build/env-gate \
  -DCMAKE_BUILD_TYPE=Release \
  -DWELLLOG_BUILD_TESTS=ON \
  -DWELLLOG_BUILD_PYTHON=ON \
  -DWELLLOG_BUILD_SHARED=OFF
cmake --build build/env-gate -j
ctest --test-dir build/env-gate --output-on-failure
```

`WELLLOG_PYTHON` 环境变量可显式覆盖解释器（仍需通过能力探测）。

## 4. 禁止事项

- 复制系统 libstdc++ 到源码树 / 提交二进制 libstdc++。
- 把宿主机绝对路径（`/opt/miniconda3`、`/usr/lib/x86_64-linux-gnu`、
  `/home/...`）写进 CMakeLists / Python 包 / Desktop 代码。
- 把 `unset LD_LIBRARY_PATH` 当作"修复"而不记录根因。
- 把 `fallback passed` 当作 `binding path passed`（见 E3 严格模式）。

## 5. CI / 干净环境

- CI 的 binding 步骤应显式传入 `WELLLOG_PYTHON`/`WELLLOG_PYTHONPATH`（或运行
  `python_env.sh`），并执行 `check_binding_env.py` 作为前置断言。
- wheel smoke 必须在独立 target 目录 + 无 conda 的 base 解释器上执行
  （见 E4）。
