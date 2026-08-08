# Desktop 套件退出段 SIGSEGV —— 诊断记录（E7）

**状态：第三方问题（PySide6/Shiboken × Qt offscreen 平台），非本仓库代码。
留下稳定复现器 + 完整诊断。无产品级修复路径；不影响产品运行。**

## 复现

```bash
# WellPlot Desktop 全量测试（每次必现，exit 139）
cd apps/wellplot-desktop
eval "$(../../scripts/python_env.sh)"
QT_QPA_PLATFORM=offscreen PYTHONFAULTHANDLER=1 \
  "$WELLLOG_PYTHON" ../../scripts/pytest_bootstrap.py tests -q
echo $?   # 139
```

所有测试 100% 通过后，进程在**解释器退出**阶段 SIGSEGV（faulthandler 无输出，
因为崩溃在 C++ 侧）。

## 环境（复现机）

| 项 | 值 |
|---|---|
| 编译器 | GCC 16.1.1 20260430（宿主 AppImage toolchain） |
| Qt | 6.11.1（PySide6 自带 /usr/lib 系统 Qt 均为 6.11.1） |
| Python | CPython 3.12.13（uv 托管 base 解释器） |
| PySide6 / Shiboken6 | 6.11.1 |
| Display backend | `QT_QPA_PLATFORM=offscreen` |
| 归属判定 | 与 welllog 绑定无关：排除全部 welllog 测试后仍 exit 139 |

## 崩溃栈（gdb，三次一致）

```text
#0  PyErr_Fetch ()
#1  Shiboken::Errors::Stash::Stash()            (libshiboken6.abi3.so.6.11)
#2  (trampoline)                                (libshiboken6.abi3.so.6.11)
#3  Shiboken::Object::destroy(SbkObject*, void*) (libshiboken6.abi3.so.6.11)
#4  (Sbk death-callback trampoline)             (PySide6/QtCore.abi3.so)
#5  (Sbk death-callback trampoline)             (PySide6/QtCore.abi3.so)
#6  QPlatformClipboard::~QPlatformClipboard()   (PySide6/Qt/lib/libQt6Gui.so.6)
#7  exit()                                       (libc)
#8  Py_Exit()
#9  handle_system_exit / _PyErr_PrintEx          (CPython)
```

帧 #6 按 load-base 符号化：`libQt6Gui.so.6 + 0x21bb86` →
`QPlatformClipboardD0Ev`（Qt 私有 API）。

## 根因机制

1. Qt 的 offscreen 平台 integration 是 `Q_GLOBAL_STATIC` 单例，其持有的
   `QOffscreenPlatformClipboard`（QPlatformClipboard 子类）**生命周期到进程
   exit()**——QApplication 的提前销毁无法影响它（已实测：sessionfinish 显式
   deleteLater + Shiboken.delete 后仍复现同一栈）。
2. 套件运行中 PySide6 为 clipboard 链创建过 Python wrapper，wrapper 的引用被
   PySide6 C++ 侧持有，**Py_FinalizeEx 之后仍存活**。
3. `exit()` → 静态单例析构 → `QPlatformClipboard::~QPlatformClipboard()` →
   Shiboken death callback → `Shiboken::Object::destroy` → `Stash::Stash` →
   `PyErr_Fetch()` 访问已 finalize 的解释器状态 → SIGSEGV（wrapper 已释放的
   use-after-free 形态）。

栈中**没有任何 welllog / 本仓库帧**；排除 welllog 相关测试文件后
（`--ignore` 全部 10 个 import welllog 的文件）仍 exit 139 —— 与我们的
binding 无关，属 PySide6 + Qt offscreen 的退出期已知脆弱性。

## 已尝试的缓解（均无效，已回退）

- `pytest_sessionfinish` 中 `app.quit() + deleteLater + sendPostedEvents +
  Shiboken.delete(app)`：QApplication 确实被销毁（日志确认），崩溃不变——
  因为崩溃对象不是 QApplication 的成员，而是 Qt 静态单例。

## 建议

- 测试层面：接受该退出段崩溃（exit code 139 不影响 pytest 结果判定，
  测试结果以 pytest 输出为准）；或将 Desktop 测试改用 xcb/offscreen 之外的
  平台时重新验证。
- 升级 PySide6（≥ 修复版本）后重跑 `gdb` 复现确认是否消失。
- 不要用 `os._exit()` / 屏蔽 stderr 规避。
