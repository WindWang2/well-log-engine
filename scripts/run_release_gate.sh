#!/usr/bin/env bash
# #174 — publish-gate CTest subset + environment-governance smoke chain
# (E1: binding env diagnostics, E3: binding-first strict mode, E4: wheel,
# E5: raster). Structural; not absolute frame SLOs.
#
# Env overrides:
#   WELLLOG_BUILD_DIR   — ctest build dir (default build/dev-shared)
#   WELLLOG_WHEEL       — wheel to smoke (default dist/*.whl in repo root)
#   WELLLOG_SKIP_WHEEL  — 1 to skip the wheel smoke
#   WELLLOG_SKIP_DESKTOP — 1 to skip the Desktop binding-first smoke
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${WELLLOG_BUILD_DIR:-$ROOT/build/dev-shared}"

if [[ ! -d "$BUILD" ]]; then
  echo "Build dir not found: $BUILD" >&2
  echo "Set WELLLOG_BUILD_DIR or configure build/dev-shared first." >&2
  exit 2
fi

# Controlled Python runtime (E1/E2): resolves a non-conda base interpreter
# with PySide6==shiboken6 + numpy; see docs/environment-binding-policy.md.
WELLLOG_PYTHON="${WELLLOG_PYTHON:-}"
WELLLOG_PYTHONPATH="${WELLLOG_PYTHONPATH:-}"
if [[ -z "$WELLLOG_PYTHON" && -x "$ROOT/scripts/python_env.sh" ]]; then
  eval "$("$ROOT/scripts/python_env.sh")" || true
fi
export WELLLOG_PYTHON WELLLOG_PYTHONPATH

echo "== WellLogEngine release-gate (structural label) =="
# Exact label match: avoid matching release-gate-full via regex substring.
ctest --test-dir "$BUILD" -L '^release-gate$' --output-on-failure "$@"

echo
echo "== Raster (E5) =="
ctest --test-dir "$BUILD" -R 'welllog\.(raster-export|render-raster)' --output-on-failure

if [[ -n "$WELLLOG_PYTHON" ]]; then
  echo
  echo "== L1 binding-environment diagnostics (E1/E2) =="
  PYTHONPATH="${WELLLOG_PYTHONPATH}${PYTHONPATH:+:$PYTHONPATH}" \
    "$WELLLOG_PYTHON" "$ROOT/scripts/check_binding_env.py"

  echo
  echo "== Native binding import + representative tests (E3) =="
  ctest --test-dir "$BUILD" -R 'welllog\.python\.' --output-on-failure

  echo
  echo "== Wheel smoke (E4) =="
  if [[ "${WELLLOG_SKIP_WHEEL:-0}" == "1" ]]; then
    echo "(skipped via WELLLOG_SKIP_WHEEL=1)"
  else
    WHEEL="${WELLLOG_WHEEL:-$(ls "$ROOT"/dist/*.whl 2>/dev/null | head -1 || true)}"
    if [[ -z "$WHEEL" || ! -f "$WHEEL" ]]; then
      echo "No wheel found (looked in \$WELLLOG_WHEEL / $ROOT/dist)." >&2
      echo "Build one with: python -m pip wheel . --no-deps -w dist" >&2
      echo "(in the engine root, under the controlled runtime)" >&2
      exit 2
    fi
    CLEAN_DIR="$(mktemp -d)"
    trap 'rm -rf "$CLEAN_DIR"' EXIT
    "$WELLLOG_PYTHON" -m pip install --quiet --no-deps --target "$CLEAN_DIR" "$WHEEL"
    PYTHONPATH="$CLEAN_DIR:${WELLLOG_PYTHONPATH}" QT_QPA_PLATFORM=minimal \
      "$WELLLOG_PYTHON" "$ROOT/tests/python/wheel_smoke.py"
  fi

  echo
  echo "== Desktop binding-first smoke, strict mode (E3) =="
  if [[ "${WELLLOG_SKIP_DESKTOP:-0}" == "1" ]]; then
    echo "(skipped via WELLLOG_SKIP_DESKTOP=1)"
  elif [[ -d "$ROOT/apps/wellplot-desktop/tests" ]]; then
    (
      cd "$ROOT/apps/wellplot-desktop"
      WLWS_REQUIRE_NATIVE_BINDING=1 QT_QPA_PLATFORM=offscreen \
        "$WELLLOG_PYTHON" "$ROOT/scripts/pytest_bootstrap.py" \
        tests/test_well_log_workstation_engine_bridge.py \
        tests/test_well_log_workstation_engine_bridge_strict.py -q
    )
  else
    echo "(Desktop app not present; skipping)"
  fi
else
  echo
  echo "No controlled Python runtime (WELLLOG_PYTHON unset and"
  echo "scripts/python_env.sh found none) — binding/wheel/Desktop smokes skipped."
  echo "Diagnose with: scripts/python_env.sh && scripts/check_binding_env.py"
fi

echo
echo "Full ADR 0014 1e8 scale (optional; structural by default):"
echo "  ctest --test-dir \$BUILD -L '^release-gate-full\$' -V"
echo "Enforce ≤2s first-interactive on reference HW only:"
echo "  WELLLOG_GATE_ENFORCE_SLO=1 ctest --test-dir \$BUILD -L '^release-gate-full\$' -V"
echo "GL frame P95/P99 (optional, WELLLOG_BUILD_BENCHMARKS=ON + Qt):"
echo "  ./welllog_dense_curve_benchmark  # single-curve 500k @ 4K"
