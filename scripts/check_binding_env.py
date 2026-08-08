#!/usr/bin/env python3
"""L1 binding-environment diagnostics (Goal env-governance, E1/E2).

Run with the *controlled* runtime:

    eval "$(scripts/python_env.sh)"
    "$WELLLOG_PYTHON" scripts/check_binding_env.py

Reports (exit 0 = healthy for binding build/test):
  * interpreter identity (executable, version, base_prefix)
  * conda shadowing (base_prefix under conda, libstdc++ loaded from conda)
  * PySide6 / shiboken6 / shiboken6_generator / numpy versions + match
  * GLIBCXX ceiling of the libstdc++.so.6 this interpreter would load vs the
    ceiling the native module requires (>= 3.4.35 on the current toolchain)
  * LD_LIBRARY_PATH / CONDA_PREFIX contamination
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

# The native module built by the current toolchain (GCC 16) needs this symbol
# version. Bump with the toolchain if it moves.
MIN_GLIBCXX = "GLIBCXX_3.4.35"


def glibcxx_ceiling(so_path: Path) -> str | None:
    """Highest GLIBCXX_x symbol version exported by a libstdc++ shared lib."""
    if not so_path.is_file():
        return None
    out = subprocess.run(
        ["strings", str(so_path)],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    versions = sorted(
        {m for m in re.findall(r"GLIBCXX_[0-9.]+", out)},
        key=lambda s: [int(p) for p in s.split("_")[1].split(".")],
    )
    return versions[-1] if versions else None


def libstdcxx_for_interpreter(interp: Path) -> Path:
    """libstdc++ a binding process will actually load.

    The interpreter itself may not link libstdc++ (Clang-built CPython does
    not); it arrives via PySide6/Qt extension modules. When it is loaded, the
    best evidence is the process map; otherwise fall back to ldd / default.
    """
    maps = Path("/proc/self/maps")
    if maps.is_file():
        for line in maps.read_text(errors="replace").splitlines():
            if "libstdc++.so.6" in line:
                parts = line.split()
                for part in parts:
                    if "libstdc++.so.6" in part:
                        return Path(part)
    ldd = subprocess.run(
        ["ldd", str(interp)], capture_output=True, text=True, check=False
    ).stdout
    for line in ldd.splitlines():
        m = re.search(r"libstdc\+\+\.so\.6 => (\S+)", line)
        if m:
            return Path(m.group(1))
    m = re.search(r"libstdc\+\+\.so\.6 \(\S+\)", ldd)
    if m:
        return Path("/lib64/libstdc++.so.6")
    return Path("/nonexistent/libstdc++.so.6")


def main() -> int:
    problems: list[str] = []
    info: list[str] = []

    interp = Path(sys.executable)
    info.append(f"interpreter : {sys.executable}")
    info.append(f"version     : {sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")
    info.append(f"base_prefix : {sys.base_prefix}")
    info.append(f"LD_LIBRARY_PATH = {os.environ.get('LD_LIBRARY_PATH', '(unset)')}")
    info.append(f"CONDA_PREFIX    = {os.environ.get('CONDA_PREFIX', '(unset)')}")

    if "conda" in sys.base_prefix:
        problems.append("conda base_prefix — native module may fail to load (GLIBCXX ceiling)")

    # Import PySide6 *before* resolving libstdc++: the runtime libstdc++ is
    # pulled into the process by the Qt extension modules, not by the
    # interpreter itself. `import PySide6` is lazy — force a Qt module load.
    try:
        import numpy
        import PySide6
        import PySide6.QtWidgets  # noqa: F401 — loads QtCore → libstdc++
        import shiboken6
        import shiboken6_generator  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        problems.append(f"binding deps import failed: {exc}")
        PySide6 = None  # type: ignore[assignment]
        shiboken6 = None  # type: ignore[assignment]
        numpy = None  # type: ignore[assignment]

    so = libstdcxx_for_interpreter(interp)
    ceiling = glibcxx_ceiling(so)
    info.append(f"libstdc++   : {so}")
    info.append(f"GLIBCXX max : {ceiling or 'unknown'}")
    if ceiling is None:
        problems.append("could not determine GLIBCXX ceiling")
    elif _version_key(ceiling) < _version_key(MIN_GLIBCXX):
        problems.append(
            f"{so} supports only {ceiling} but the native module needs {MIN_GLIBCXX}"
        )

    if PySide6 is not None:
        info.append(f"PySide6     : {PySide6.__version__}")
        info.append(f"shiboken6   : {shiboken6.__version__}")  # type: ignore[union-attr]
        info.append(f"numpy       : {numpy.__version__}")  # type: ignore[union-attr]
        if PySide6.__version__ != shiboken6.__version__:  # type: ignore[union-attr]
            problems.append("PySide6 and shiboken6 versions differ")

    if not (3, 12) <= sys.version_info[:2] <= (3, 13):
        problems.append(
            f"CPython {sys.version_info.major}.{sys.version_info.minor} not in the "
            "supported binding matrix (3.12/3.13)"
        )

    print("\n".join(info))
    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("\nOK: runtime is healthy for binding build/test.")
    return 0


def _version_key(tag: str) -> list[int]:
    return [int(p) for p in tag.split("_")[1].split(".")]


if __name__ == "__main__":
    sys.exit(main())
