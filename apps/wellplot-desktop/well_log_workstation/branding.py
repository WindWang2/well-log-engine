"""Product identity for WellPlot Desktop (#290 / epic #288 T2).

The Python package and import path remain ``well_log_workstation`` (gradual
rename later). User-visible chrome uses **WellPlot Desktop**.
"""

from __future__ import annotations

# Display name (window title, QApplication, About, status bar).
PRODUCT_NAME = "WellPlot Desktop"

# Qt settings / QSettings organization (unchanged for existing user config).
ORGANIZATION_NAME = "paleo-workbench"

# Importable package name — keep stable for entrypoint ``python -m …``.
PACKAGE_MODULE = "well_log_workstation"

# Short note for About / README (Workstation → Desktop upgrade).
UPGRADE_NOTE = (
    "WellPlot Desktop is the product name for the former Well Log Workstation "
    "shell. The code package remains `well_log_workstation` for now; "
    "start with: python -m well_log_workstation"
)


def window_title(*, workspace_name: str | None = None) -> str:
    """Main-window title with optional open workspace name."""
    if workspace_name:
        return f"{workspace_name} — {PRODUCT_NAME}"
    return PRODUCT_NAME


def about_text(*, version: str) -> str:
    """Plain-text body for the About dialog."""
    return (
        f"{PRODUCT_NAME}\n"
        f"Version {version}\n"
        f"\n"
        f"Standalone well-log plotting desktop (log-first).\n"
        f"Rendering: WellLogEngine when available; host canvas as fallback.\n"
        f"\n"
        f"{UPGRADE_NOTE}\n"
        f"\n"
        f"Not Paleo Workbench — independent product track (epic #288).\n"
        f"Package: {PACKAGE_MODULE}"
    )
