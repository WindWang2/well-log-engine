# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for WellPlot Desktop (T15 / #303).

Build (from monorepo root)::

    pyinstaller well_log_workstation/packaging/wellplot-desktop.spec

Output: dist/WellPlotDesktop/  (onedir; preferred for uninstall + Qt plugins)
"""

from __future__ import annotations

import sys
from pathlib import Path

from PyInstaller.utils.hooks import collect_all, collect_data_files

block_cipher = None

REPO_ROOT = Path(SPECPATH).resolve().parents[1]  # packaging/ → well_log_workstation/ → root
# When SPECPATH is well_log_workstation/packaging, parents[1] is repo root.
if (REPO_ROOT / "well_log_workstation").is_dir():
    ROOT = REPO_ROOT
else:
    ROOT = Path(SPECPATH).resolve().parents[2]

PKG = ROOT / "well_log_workstation"

# PySide6 needs plugins / translations collected.
pyside_datas, pyside_binaries, pyside_hidden = collect_all("PySide6")

datas = [
    (str(PKG / "templates"), "well_log_workstation/templates"),
]
datas += pyside_datas
# Optional golden fixture (small; harmless if present)
_golden = PKG / "testdata" / "geometry_golden"
if _golden.is_dir():
    datas.append((str(_golden), "well_log_workstation/testdata/geometry_golden"))

hiddenimports = list(pyside_hidden) + [
    "well_log_workstation",
    "well_log_workstation.main",
    "well_log_workstation.shell",
    "well_log_workstation.branding",
    "well_log_workstation.template_model",
    "well_log_workstation.export_dispatch",
    "well_log_workstation.export_plot",
    "well_log_workstation.engine_bridge",
    "well_log_workstation.las_import",
    "well_log_workstation.workspace",
    "well_log_workstation.startup_page",
    "well_log_workstation.print_preview",
    "well_log_workstation.geometry_golden",
    "numpy",
    "lasio",
    "shiboken6",
]

binaries = list(pyside_binaries)

a = Analysis(
    [str(PKG / "main.py")],
    pathex=[str(ROOT)],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        # Keep the host bundle free of the full Paleo Workbench / geo stack.
        "paleo_workbench",
        "geoviz",
        "matplotlib",
        "scipy",
        "rasterio",
        "osgeo",
        "tkinter",
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="WellPlotDesktop",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,  # GUI app; use --version via a thin console wrapper if needed
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name="WellPlotDesktop",
)
