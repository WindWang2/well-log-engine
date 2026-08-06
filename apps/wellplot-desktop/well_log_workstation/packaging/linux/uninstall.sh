#!/usr/bin/env bash
# Uninstall WellPlot Desktop user/system install created by install.sh.
#
# Usage:
#   bash uninstall.sh [--prefix DIR] [--dry-run] [--purge-config]
#
# Removes:
#   - install prefix (onedir)
#   - bin symlink wellplot-desktop
#   - .desktop launcher
#
# By default keeps user config (QSettings / recent workspaces).
# Pass --purge-config to remove residual config (documented below).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY=0
PURGE=0
PREFIX=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY=1; shift ;;
    --purge-config) PURGE=1; shift ;;
    --prefix) PREFIX="${2:-}"; shift 2 ;;
    -h|--help)
      sed -n '2,16p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

# Prefer meta written by install.sh
META=""
if [[ -n "$PREFIX" && -f "$PREFIX/.wellplot-install-meta" ]]; then
  META="$PREFIX/.wellplot-install-meta"
elif [[ -f "$SCRIPT_DIR/.wellplot-install-meta" ]]; then
  META="$SCRIPT_DIR/.wellplot-install-meta"
  PREFIX="$SCRIPT_DIR"
elif [[ -f "$HOME/.local/opt/WellPlotDesktop/.wellplot-install-meta" ]]; then
  META="$HOME/.local/opt/WellPlotDesktop/.wellplot-install-meta"
  PREFIX="$HOME/.local/opt/WellPlotDesktop"
elif [[ -f /opt/WellPlotDesktop/.wellplot-install-meta ]]; then
  META=/opt/WellPlotDesktop/.wellplot-install-meta
  PREFIX=/opt/WellPlotDesktop
else
  PREFIX="${PREFIX:-$HOME/.local/opt/WellPlotDesktop}"
fi

BIN_LINK="${XDG_BIN_HOME:-$HOME/.local/bin}/wellplot-desktop"
DESKTOP="${XDG_DATA_HOME:-$HOME/.local/share}/applications/wellplot-desktop.desktop"

if [[ -n "$META" && -f "$META" ]]; then
  # shellcheck disable=SC1090
  while IFS='=' read -r k v; do
    case "$k" in
      prefix) PREFIX="$v" ;;
      bin) BIN_LINK="$v" ;;
      desktop) DESKTOP="$v" ;;
    esac
  done < "$META"
fi

run() {
  if [[ $DRY -eq 1 ]]; then
    echo "DRY: $*"
  else
    "$@"
  fi
}

echo "==> Uninstall WellPlot Desktop"
echo "    prefix:  ${PREFIX:-"(none)"}"
echo "    bin:     $BIN_LINK"
echo "    desktop: $DESKTOP"

if [[ -L "$BIN_LINK" || -f "$BIN_LINK" ]]; then
  run rm -f "$BIN_LINK"
fi
if [[ -f "$DESKTOP" ]]; then
  run rm -f "$DESKTOP"
fi
# Also clear common system bin if present
if [[ -L /usr/local/bin/wellplot-desktop ]]; then
  run rm -f /usr/local/bin/wellplot-desktop
fi
if [[ -f /usr/local/share/applications/wellplot-desktop.desktop ]]; then
  run rm -f /usr/local/share/applications/wellplot-desktop.desktop
fi

if [[ -n "$PREFIX" && -d "$PREFIX" ]]; then
  run rm -rf "$PREFIX"
fi

if [[ $PURGE -eq 1 ]]; then
  # Residual config locations (see packaging README).
  CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/paleo-workbench"
  if [[ -d "$CONF_DIR" ]]; then
    echo "    purge config: $CONF_DIR"
    run rm -rf "$CONF_DIR"
  fi
fi

echo "==> Uninstalled."
echo "    Residual (unless --purge-config): QSettings under org paleo-workbench,"
echo "    and any user workspaces you created outside the install prefix."
