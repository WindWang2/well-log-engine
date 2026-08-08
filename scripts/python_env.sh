#!/usr/bin/env bash
# Resolve the controlled WellLogEngine Python runtime for binding build/test
# (Goal env-governance, E1/E2). Prints eval-able exports:
#
#   eval "$(scripts/python_env.sh)"
#
#   WELLLOG_PYTHON        — interpreter to build/test the bindings with
#   WELLLOG_PYTHONPATH    — site-packages containing PySide6/shiboken6/numpy
#   WELLLOG_PYTHON_ENV    — "ok" when a capable runtime was found
#
# Why this exists
# ---------------
# The bindings must not depend on whichever python happens to be on PATH:
#
# 1. conda pythons carry an RPATH to /opt/miniconda3/lib/libstdc++.so.6 whose
#    GLIBCXX ceiling (3.4.34 in the current machine) is older than what the
#    host GCC-16-built native module requires (GLIBCXX_3.4.35+). Importing the
#    native module under such an interpreter fails with "GLIBCXX_x.y.z not
#    found".
# 2. Some sandbox/harness environments resolve /proc/self/exe to a bundled
#    runtime, which breaks venv prefix detection (a venv's own bin/python no
#    longer finds its site-packages).
#
# The policy (see docs/environment-binding-policy.md):
#   * use a *base* interpreter (not a venv shim) that is NOT a conda runtime;
#   * point PYTHONPATH at the workspace venv's site-packages explicitly;
#   * verify PySide6/shiboken6_generator/numpy presence and version match.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- capability probe -------------------------------------------------------
# $1 = interpreter path; $2 = optional site-packages to inject (venv case).
# Prints nothing on success; exits 1 on failure.
probe() {
    local interp="$1"
    local site_packages="$2"
    if [[ ! -x "$interp" ]]; then
        return 1
    fi
    local out
    out="$("$interp" -c '
import sys
if sys.argv[1]:
    sys.path.insert(0, sys.argv[1])
try:
    import pathlib
    import numpy
    import PySide6
    import shiboken6
    import shiboken6_generator
except Exception as exc:  # noqa: BLE001
    sys.exit(f"missing binding deps: {exc}")
if not (3, 12) <= sys.version_info[:2] <= (3, 13):
    sys.exit(f"unsupported CPython {sys.version_info.major}.{sys.version_info.minor}")
if PySide6.__version__ != shiboken6.__version__:
    sys.exit(f"PySide6 {PySide6.__version__} != shiboken6 {shiboken6.__version__}")
print(f"{sys.executable}|{pathlib.Path(PySide6.__file__).parent.parent}")
' "$site_packages" 2>/dev/null)" || return 1
    WELLLOG_PYTHON="${out%%|*}"
    # probe already printed the site-packages directory (PySide6's parent.parent).
    WELLLOG_PYTHONPATH="${out#*|}"
}

# --- candidate discovery ----------------------------------------------------
candidates=()

# 1. Explicit override (must still pass the probe).
if [[ -n "${WELLLOG_PYTHON:-}" ]]; then
    candidates+=("$WELLLOG_PYTHON")
fi

# 2. Workspace venvs: for each, use the *base* interpreter named in
#    pyvenv.cfg (the venv bin/python itself is unreliable in sandboxed
#    harnesses where /proc/self/exe does not resolve to the venv shim).
venv_dirs=()
for d in "$ROOT/.venv" "$ROOT/../.venv" "$ROOT/../paleo_workbench/.venv"; do
    if [[ -f "$d/pyvenv.cfg" ]]; then
        venv_dirs+=("$d")
    fi
done
for vd in "${venv_dirs[@]}"; do
    local_home="$(sed -n 's/^home = //p' "$vd/pyvenv.cfg" 2>/dev/null || true)"
    if [[ -n "$local_home" && -x "$local_home/python3" ]]; then
        site="$(find "$vd/lib" -maxdepth 2 -type d -name site-packages 2>/dev/null | head -1)"
        candidates+=("$local_home/python3|${site:-}")
    fi
done

# 3. PATH python as a last resort.
candidates+=("$(command -v python3 || true)")

# --- pick the first capable candidate --------------------------------------
for cand in "${candidates[@]}"; do
    if [[ -z "$cand" ]]; then
        continue
    fi
    interp="${cand%%|*}"
    site="${cand#*|}"
    [[ "$site" == "$interp" ]] && site=""
    if probe "$interp" "$site"; then
        # Reject conda runtimes: their libstdc++ RPATH shadows the compiler
        # runtime the native module needs (GLIBCXX ceiling).
        if ! "$WELLLOG_PYTHON" -c 'import sys; sys.exit(0 if "conda" not in sys.base_prefix else 1)' 2>/dev/null; then
            echo "python_env: rejecting conda runtime $WELLLOG_PYTHON (GLIBCXX ceiling)" >&2
            WELLLOG_PYTHON=""
            continue
        fi
        echo "export WELLLOG_PYTHON=${WELLLOG_PYTHON@Q}"
        echo "export WELLLOG_PYTHONPATH=${WELLLOG_PYTHONPATH@Q}"
        echo "export WELLLOG_PYTHON_ENV=ok"
        exit 0
    fi
done

echo "python_env: no capable Python runtime found" >&2
echo "python_env: need CPython 3.12/3.13 with PySide6==shiboken6, shiboken6_generator, numpy" >&2
echo "export WELLLOG_PYTHON_ENV=missing"
exit 1
