#!/usr/bin/env bash
# Install WellPlot Desktop onedir bundle for the current user (or system with --system).
#
# Usage:
#   bash install.sh [--prefix DIR] [--system] [--dry-run]
#
# Default user prefix: ~/.local/opt/WellPlotDesktop
# Creates:
#   <prefix>/…                 (copied onedir)
#   ~/.local/bin/wellplot-desktop  (or /usr/local/bin with --system)
#   ~/.local/share/applications/wellplot-desktop.desktop
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE_DIR="$SCRIPT_DIR"
# When run from source tree packaging/linux (not from dist bundle), look for dist.
if [[ ! -f "$BUNDLE_DIR/WellPlotDesktop" && ! -x "$BUNDLE_DIR/WellPlotDesktop" ]]; then
  if [[ -f "$BUNDLE_DIR/../../../../dist/WellPlotDesktop/WellPlotDesktop" ]]; then
    BUNDLE_DIR="$(cd "$BUNDLE_DIR/../../../../dist/WellPlotDesktop" && pwd)"
  elif [[ -f "$BUNDLE_DIR/../../../dist/WellPlotDesktop/WellPlotDesktop" ]]; then
    BUNDLE_DIR="$(cd "$BUNDLE_DIR/../../../dist/WellPlotDesktop" && pwd)"
  fi
fi

SYSTEM=0
DRY=0
PREFIX=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --system) SYSTEM=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --prefix) PREFIX="${2:-}"; shift 2 ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

if [[ $SYSTEM -eq 1 ]]; then
  PREFIX="${PREFIX:-/opt/WellPlotDesktop}"
  BIN_DIR="/usr/local/bin"
  APP_DIR="/usr/local/share/applications"
else
  PREFIX="${PREFIX:-$HOME/.local/opt/WellPlotDesktop}"
  BIN_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"
  APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
fi

EXE_SRC="$BUNDLE_DIR/WellPlotDesktop"
if [[ ! -e "$EXE_SRC" ]]; then
  echo "Bundle binary not found: $EXE_SRC" >&2
  echo "Build first: bash well_log_workstation/packaging/build.sh" >&2
  exit 1
fi

run() {
  if [[ $DRY -eq 1 ]]; then
    echo "DRY: $*"
  else
    "$@"
  fi
}

echo "==> Install WellPlot Desktop"
echo "    bundle: $BUNDLE_DIR"
echo "    prefix: $PREFIX"
echo "    bin:    $BIN_DIR/wellplot-desktop"
echo "    desktop:$APP_DIR/wellplot-desktop.desktop"

run mkdir -p "$PREFIX" "$BIN_DIR" "$APP_DIR"
if [[ $DRY -eq 0 ]]; then
  # rsync-like copy without requiring rsync
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "$BUNDLE_DIR"/ "$PREFIX"/
  else
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"
    cp -a "$BUNDLE_DIR"/. "$PREFIX"/
  fi
  chmod +x "$PREFIX/WellPlotDesktop" 2>/dev/null || true
  ln -sfn "$PREFIX/WellPlotDesktop" "$BIN_DIR/wellplot-desktop"
else
  run true  # already printed
fi

DESKTOP_IN="$BUNDLE_DIR/wellplot-desktop.desktop.in"
if [[ ! -f "$DESKTOP_IN" ]]; then
  DESKTOP_IN="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/wellplot-desktop.desktop.in"
fi
ICON_PATH="$PREFIX/WellPlotDesktop"  # no branded icon yet; Exec is enough
if [[ -f "$DESKTOP_IN" ]]; then
  if [[ $DRY -eq 1 ]]; then
    echo "DRY: write desktop entry → $APP_DIR/wellplot-desktop.desktop"
  else
    sed \
      -e "s|@EXEC@|$PREFIX/WellPlotDesktop|g" \
      -e "s|@ICON@|$ICON_PATH|g" \
      "$DESKTOP_IN" > "$APP_DIR/wellplot-desktop.desktop"
    chmod 644 "$APP_DIR/wellplot-desktop.desktop"
  fi
fi

# Record install metadata for uninstall.
META="$PREFIX/.wellplot-install-meta"
if [[ $DRY -eq 0 ]]; then
  cat > "$META" <<EOF
prefix=$PREFIX
bin=$BIN_DIR/wellplot-desktop
desktop=$APP_DIR/wellplot-desktop.desktop
system=$SYSTEM
installed_at=$(date -Iseconds 2>/dev/null || date)
EOF
fi

echo "==> Installed."
echo "    Start: wellplot-desktop   or   $PREFIX/WellPlotDesktop"
echo "    Uninstall: bash $PREFIX/uninstall.sh"
if [[ $SYSTEM -eq 0 ]]; then
  case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo "    Note: add $BIN_DIR to PATH if 'wellplot-desktop' is not found." ;;
  esac
fi
