"""WellPlot Desktop packaging / installer skeleton (T15 / #303)."""

from __future__ import annotations

import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation import __version__
from well_log_workstation.branding import PRODUCT_NAME
from well_log_workstation.main import main as wlws_main

REPO = Path(__file__).resolve().parents[1]
PACK = REPO / "well_log_workstation" / "packaging"


def test_packaging_layout_present() -> None:
    required = [
        PACK / "README.md",
        PACK / "requirements-packaging.txt",
        PACK / "wellplot-desktop.spec",
        PACK / "build.sh",
        PACK / "build.ps1",
        PACK / "linux" / "install.sh",
        PACK / "linux" / "uninstall.sh",
        PACK / "linux" / "wellplot-desktop.desktop.in",
        PACK / "windows" / "install.ps1",
        PACK / "windows" / "uninstall.ps1",
        PACK / "windows" / "wellplot-desktop.iss",
    ]
    missing = [str(p.relative_to(REPO)) for p in required if not p.is_file()]
    assert not missing, f"packaging files missing: {missing}"


def test_readme_documents_build_and_residual() -> None:
    text = (PACK / "README.md").read_text(encoding="utf-8")
    for needle in (
        "build.sh",
        "build.ps1",
        "install.sh",
        "uninstall",
        "Inno Setup",
        "Residual",
        "purge-config",
        "WellPlot Desktop",
    ):
        assert needle in text, f"packaging README missing {needle!r}"


def test_desktop_entry_template() -> None:
    text = (PACK / "linux" / "wellplot-desktop.desktop.in").read_text(encoding="utf-8")
    assert "[Desktop Entry]" in text
    assert "Name=WellPlot Desktop" in text
    assert "Exec=@EXEC@" in text
    assert "Type=Application" in text


def test_inno_setup_has_uninstall() -> None:
    text = (PACK / "windows" / "wellplot-desktop.iss").read_text(encoding="utf-8")
    assert "AppName=" in text or "AppName={" in text
    assert "Uninstall" in text or "uninstallexe" in text
    assert "WellPlotDesktop.exe" in text


def test_main_version_no_gui() -> None:
    assert wlws_main(["--version"]) == 0
    assert wlws_main(["-V"]) == 0
    assert wlws_main(["--help"]) == 0


def test_main_version_subprocess(capsys) -> None:
    code = subprocess.run(
        [sys.executable, "-m", "well_log_workstation", "--version"],
        cwd=str(REPO),
        env={**os.environ, "PYTHONPATH": str(REPO), "QT_QPA_PLATFORM": "offscreen"},
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert code.returncode == 0, code.stderr
    assert PRODUCT_NAME in code.stdout
    assert __version__ in code.stdout


def test_linux_install_uninstall_dry_run(tmp_path: Path) -> None:
    """Simulate a built onedir and exercise install/uninstall dry-run + real prefix."""
    bundle = tmp_path / "WellPlotDesktop"
    bundle.mkdir()
    exe = bundle / "WellPlotDesktop"
    exe.write_text("#!/bin/sh\necho fake\n", encoding="utf-8")
    exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
    # Ship the real install scripts + desktop template into the fake bundle
    for name in ("install.sh", "uninstall.sh", "wellplot-desktop.desktop.in"):
        src = PACK / "linux" / name
        (bundle / name).write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
        if name.endswith(".sh"):
            (bundle / name).chmod(0o755)

    prefix = tmp_path / "opt" / "WellPlotDesktop"
    bin_dir = tmp_path / "bin"
    app_dir = tmp_path / "applications"
    env = {
        **os.environ,
        "HOME": str(tmp_path / "home"),
        "XDG_BIN_HOME": str(bin_dir),
        "XDG_DATA_HOME": str(tmp_path / "xdg"),
    }
    # XDG applications dir is under XDG_DATA_HOME/applications — install.sh uses that
    # unless we only set XDG_BIN_HOME. APP_DIR = XDG_DATA_HOME/applications.

    # Dry-run should not create prefix
    r = subprocess.run(
        ["bash", str(bundle / "install.sh"), "--prefix", str(prefix), "--dry-run"],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert r.returncode == 0, r.stderr + r.stdout
    assert "DRY:" in r.stdout
    assert not prefix.exists()

    # Real install into tmp prefix
    r = subprocess.run(
        ["bash", str(bundle / "install.sh"), "--prefix", str(prefix)],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert r.returncode == 0, r.stderr + r.stdout
    assert (prefix / "WellPlotDesktop").is_file()
    assert (prefix / ".wellplot-install-meta").is_file()
    link = bin_dir / "wellplot-desktop"
    assert link.is_symlink() or link.is_file()
    desktop = tmp_path / "xdg" / "applications" / "wellplot-desktop.desktop"
    assert desktop.is_file()
    desk_text = desktop.read_text(encoding="utf-8")
    assert "WellPlot Desktop" in desk_text
    assert str(prefix / "WellPlotDesktop") in desk_text

    # Uninstall removes launcher + prefix
    r = subprocess.run(
        ["bash", str(prefix / "uninstall.sh"), "--prefix", str(prefix)],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert r.returncode == 0, r.stderr + r.stdout
    assert not prefix.exists()
    assert not link.exists()
    assert not desktop.exists()


def test_pyproject_has_wellplot_desktop_script() -> None:
    text = (REPO / "pyproject.toml").read_text(encoding="utf-8")
    assert 'wellplot-desktop = "well_log_workstation.main:main"' in text
    assert 'well-log-workstation = "well_log_workstation.main:main"' in text


def test_spec_excludes_workbench_stack() -> None:
    text = (PACK / "wellplot-desktop.spec").read_text(encoding="utf-8")
    assert "paleo_workbench" in text
    assert "WellPlotDesktop" in text
    assert "collect_all" in text
