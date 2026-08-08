#!/usr/bin/env python3
"""Run pytest under the controlled WellLogEngine runtime (E1/E2).

PYTHONPATH alone does not process .pth files, so editable-installed host
packages (geoviz etc.) stay invisible. site.addsitedir() processes them.
Requires WELLLOG_SITE_PACKAGES (set by scripts/welllog_env.sh).

    "$WELLLOG_PYTHON" scripts/pytest_bootstrap.py <pytest args...>
"""

from __future__ import annotations

import os
import site
import sys


def main() -> int:
    site_packages = os.environ.get("WELLLOG_SITE_PACKAGES")
    if site_packages:
        site.addsitedir(site_packages)
    import pytest

    return pytest.main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
