# Qt / lifecycle stress tests (#173)

| CTest | Binary / command | Notes |
|-------|------------------|--------|
| `welllog.qt-widget` | `welllog_qt_widget_tests` | Functional GL embedding |
| `welllog.qt-context-lifecycle-stress` | `welllog_qt_context_lifecycle_stress_tests` | Create/destroy, reparent, multi-view isolation, Trace toggle |
| `welllog.async-lrw-stress` | `welllog_async_lrw_stress_tests` | Headless LRW, session destroy under workers, export cancel, table/SVG without GL |
| `welllog.python.qt-lifecycle-stress` | `tests/python/test_qt_lifecycle_stress.py` | GC off GUI thread, churn (needs `WELLLOG_BUILD_PYTHON`) |

## Run

```bash
ctest -R 'welllog\.(qt-context-lifecycle|async-lrw|python\.qt-lifecycle|qt-widget)' --output-on-failure
```

Linux Mesa software path (matching `welllog.qt-widget`):

```bash
LIBGL_ALWAYS_SOFTWARE=1 ctest -R welllog.qt-context-lifecycle-stress --output-on-failure
```

## Sanitizers (AC8)

Build with host flags (same pattern as `tests/fuzz/README.md`):

```bash
CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
LDFLAGS="-fsanitize=address,undefined" \
  cmake -S . -B build/asan -DWELLLOG_BUILD_QT_WIDGETS=ON -DWELLLOG_BUILD_TESTS=ON
cmake --build build/asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --test-dir build/asan -R 'async-lrw|qt-context-lifecycle' --output-on-failure
```

Windows: use the toolchain’s ASan/UBSan (or Debug CRT) equivalent and re-run the same CTest names. Python 3.12 and 3.13: re-run `welllog.python.qt-lifecycle-stress` against each wheel.
