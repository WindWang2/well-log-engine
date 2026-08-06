# WellPlot Desktop — packaging (T15 / #303)

Independent installable Desktop product (轨 D). Package module remains
`well_log_workstation`; user-facing name is **WellPlot Desktop**.

## What you get

| Platform | Artifact | Install | Uninstall |
|----------|----------|---------|-----------|
| **Linux** | `dist/WellPlotDesktop/` onedir (+ scripts) | `install.sh` → `~/.local/opt/…` + `.desktop` + `wellplot-desktop` | `uninstall.sh` |
| **Windows** | `dist/WellPlotDesktop/` onedir (+ scripts) | `install.ps1` → `%LOCALAPPDATA%\WellPlotDesktop` + Start Menu | `uninstall.ps1` |
| **Windows (optional)** | `WellPlotDesktop-Setup.exe` | Inno Setup from `.iss` | Windows Apps & Features |

First-ship installer is **host canvas** by default (`WLWS_DISABLE_ENGINE=1` at
build time). WellLogEngine can be added later as an optional side-by-side
runtime; it is not required to start the app.

## Prerequisites

- Python 3.12 or 3.13
- Runtime deps for the host product: `PySide6`, `numpy`, `lasio` (and the
  `well_log_workstation` package on `PYTHONPATH` / installed editable)
- Build-only: PyInstaller

```bash
python -m pip install "PySide6==6.11.1" "numpy>=1.26" "lasio>=0.31"
python -m pip install -r well_log_workstation/packaging/requirements-packaging.txt
```

## Build

From the **monorepo root**:

### Linux

```bash
bash well_log_workstation/packaging/build.sh
# → dist/WellPlotDesktop/
```

Optional tarball for distribution:

```bash
tar -C dist -czf dist/WellPlotDesktop-linux-$(uname -m).tar.gz WellPlotDesktop
```

### Windows (PowerShell)

```powershell
powershell -ExecutionPolicy Bypass -File well_log_workstation\packaging\build.ps1
# → dist\WellPlotDesktop\
```

Optional classic installer (requires [Inno Setup 6](https://jrsoftware.org/isinfo.php)):

```powershell
iscc well_log_workstation\packaging\windows\wellplot-desktop.iss
# → dist\WellPlotDesktop-Setup.exe
```

## Install → start → uninstall

### Linux (user install)

```bash
# After build:
bash dist/WellPlotDesktop/install.sh
# Start:
wellplot-desktop
# or:
~/.local/opt/WellPlotDesktop/WellPlotDesktop

# Uninstall (removes launcher + prefix; keeps config by default):
bash ~/.local/opt/WellPlotDesktop/uninstall.sh
# Also drop QSettings:
bash ~/.local/opt/WellPlotDesktop/uninstall.sh --purge-config
```

System-wide (needs write access to `/opt` and `/usr/local`):

```bash
sudo bash dist/WellPlotDesktop/install.sh --system
sudo bash /opt/WellPlotDesktop/uninstall.sh
```

### Windows (user install)

```powershell
cd dist\WellPlotDesktop
powershell -ExecutionPolicy Bypass -File .\install.ps1
# Start: Start Menu → WellPlot Desktop
# Uninstall:
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\WellPlotDesktop\uninstall.ps1"
# Also drop HKCU QSettings:
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\WellPlotDesktop\uninstall.ps1" -PurgeConfig
```

With Inno Setup installer: use the generated `WellPlotDesktop-Setup.exe`, then
uninstall via **Settings → Apps** (or the Start Menu uninstall entry).

### Dev / no freeze (smoke)

```bash
python -m well_log_workstation --version   # no GUI
python -m well_log_workstation             # GUI
# console scripts (after pip install -e .):
wellplot-desktop --version
well-log-workstation --version
```

## Residual after uninstall (documented)

| Item | Location | Removed by default? |
|------|----------|---------------------|
| App files + launcher / Start Menu | install prefix, `.desktop` / `.lnk` | **Yes** |
| QSettings org `paleo-workbench` | Linux: `~/.config/paleo-workbench`; Windows: `HKCU\Software\paleo-workbench` | Only with `--purge-config` / `-PurgeConfig` |
| User workspaces (LAS, plots) | Paths the user chose | **Never** (user data) |

Critical entry points (desktop launcher, Start Menu, `wellplot-desktop` symlink)
are always removed by the uninstall scripts / Inno uninstaller.

## CI / verification

Automated tests (no full PyInstaller freeze required):

```bash
QT_QPA_PLATFORM=offscreen pytest -q tests/test_well_log_workstation_packaging.py
```

Manual acceptance on a real desktop:

1. Build on the target OS.
2. Install with the platform script (or Setup.exe).
3. Start from the menu / `wellplot-desktop` — window title **WellPlot Desktop**.
4. Uninstall — launcher gone; optional purge for settings.

## Layout

```
well_log_workstation/packaging/
  README.md                 ← this file
  requirements-packaging.txt
  wellplot-desktop.spec     ← PyInstaller
  build.sh / build.ps1
  linux/
    install.sh
    uninstall.sh
    wellplot-desktop.desktop.in
  windows/
    install.ps1
    uninstall.ps1
    wellplot-desktop.iss    ← optional Inno Setup
```
