#!/usr/bin/env bash
# Build WellPlot Desktop onedir bundle with PyInstaller (Linux / macOS host).
# Usage (from monorepo root):
#   bash well_log_workstation/packaging/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SPEC="well_log_workstation/packaging/wellplot-desktop.spec"
OUT_DIR="${WELLPLOT_DIST:-$ROOT/dist}"
WORK_DIR="${WELLPLOT_BUILD:-$ROOT/build/pyinstaller}"

if ! command -v pyinstaller >/dev/null 2>&1; then
  echo "pyinstaller not found. Install build deps:" >&2
  echo "  python -m pip install -r well_log_workstation/packaging/requirements-packaging.txt" >&2
  exit 1
fi

# Prefer host-only engine policy for the first-ship installer.
export WLWS_DISABLE_ENGINE="${WLWS_DISABLE_ENGINE:-1}"

echo "==> PyInstaller → $OUT_DIR/WellPlotDesktop"
mkdir -p "$OUT_DIR" "$WORK_DIR"
pyinstaller \
  --noconfirm \
  --clean \
  --distpath "$OUT_DIR" \
  --workpath "$WORK_DIR" \
  "$SPEC"

APP="$OUT_DIR/WellPlotDesktop"
if [[ ! -x "$APP/WellPlotDesktop" && ! -f "$APP/WellPlotDesktop" ]]; then
  echo "Build failed: missing $APP/WellPlotDesktop" >&2
  exit 1
fi

# Console smoke helper (GUI binary may not print --version on some platforms).
cat > "$APP/wellplot-desktop-cli" <<'EOF'
#!/usr/bin/env bash
# Thin wrapper: run the packaged binary with any CLI args.
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/WellPlotDesktop" "$@"
EOF
chmod +x "$APP/wellplot-desktop-cli"

# Ship install/uninstall helpers next to the bundle for tarball distribution.
cp -f well_log_workstation/packaging/linux/install.sh "$APP/"
cp -f well_log_workstation/packaging/linux/uninstall.sh "$APP/"
cp -f well_log_workstation/packaging/linux/wellplot-desktop.desktop.in "$APP/"
chmod +x "$APP/install.sh" "$APP/uninstall.sh"

echo "==> Bundle ready: $APP"
echo "    Install (user):  bash $APP/install.sh"
echo "    Uninstall:       bash $APP/uninstall.sh"
ls -la "$APP" | head -20
