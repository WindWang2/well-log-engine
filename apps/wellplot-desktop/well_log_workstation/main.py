"""Entry point: ``python -m well_log_workstation`` (#216 / #290).

Must configure Qt platform **before** QApplication is created.
Product display name: WellPlot Desktop (package path unchanged).

Installer smoke (T15 / #303): ``--version`` / ``-V`` print and exit 0
without creating a QApplication.
"""

from __future__ import annotations

import sys

from well_log_workstation import __version__
from well_log_workstation.branding import ORGANIZATION_NAME, PRODUCT_NAME
from well_log_workstation.qt_platform import configure_qt_platform_for_session


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if any(a in ("--version", "-V") for a in args):
        print(f"{PRODUCT_NAME} {__version__}")
        return 0
    if any(a in ("--help", "-h") for a in args):
        print(
            f"{PRODUCT_NAME} {__version__}\n"
            f"Usage: python -m well_log_workstation [--version] [--help]\n"
            f"  or:  wellplot-desktop / well-log-workstation\n"
        )
        return 0

    configure_qt_platform_for_session(warn=True)

    from PySide6.QtWidgets import QApplication

    from well_log_workstation.shell import WellLogWorkstationWindow

    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName(PRODUCT_NAME)
    app.setOrganizationName(ORGANIZATION_NAME)

    window = WellLogWorkstationWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
