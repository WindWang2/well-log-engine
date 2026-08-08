#!/usr/bin/env bash
# Enter the controlled WellLogEngine environment (E1/E2).
#
#   source scripts/welllog_env.sh
#
# Exports:
#   WELLLOG_PYTHON      — base interpreter for binding build/test
#   WELLLOG_PYTHONPATH  — site-packages with PySide6/shiboken6/numpy
#   WELLLOG_CMAKE       — working CMake (system cmake may be broken in
#                         sandboxed harnesses; prefer the venv one)
#   WELLLOG_NINJA       — ninja generator (optional)
#
# Fails loudly (set -e) when no capable Python runtime is found.
# Works when sourced from bash or zsh.
set -euo pipefail

if [[ -n "${BASH_SOURCE:-}" ]]; then
    SELF="${BASH_SOURCE[0]}"
else
    # zsh: path of the currently sourced file.
    SELF="${(%):-%x}"
fi

ROOT="$(cd "$(dirname "$SELF")/.." && pwd)"

eval "$("$ROOT/scripts/python_env.sh")"

# Python only reads PYTHONPATH — bridge the controlled site-packages in.
export PYTHONPATH="${WELLLOG_PYTHONPATH}${PYTHONPATH:+:$PYTHONPATH}"
export WELLLOG_SITE_PACKAGES="${WELLLOG_PYTHONPATH}"

# Prefer the venv-bundled CMake (the harness's system cmake can be broken,
# e.g. "Could not find CMAKE_ROOT"), else fall back to PATH.
WELLLOG_CMAKE="$(command -v cmake || true)"
for vd in "$ROOT/.venv" "$ROOT/../.venv"; do
    if [[ -x "$vd/bin/cmake" ]]; then
        WELLLOG_CMAKE="$vd/bin/cmake"
        break
    fi
done
export WELLLOG_CMAKE

WELLLOG_NINJA="$(command -v ninja || true)"
export WELLLOG_NINJA

echo "welllog env: python=${WELLLOG_PYTHON}" >&2
echo "welllog env: cmake=${WELLLOG_CMAKE}" >&2
