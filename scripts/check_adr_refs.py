#!/usr/bin/env python3
"""Fail-closed ADR reference check (#39).

Scans docs/ + src/ + include/ + apps/ + tests/ + python/ for ``ADR 00xx``
references and asserts every referenced number has a corresponding file in
docs/adr/. Exits non-zero listing the dangling references.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ADR_DIR = REPO / "docs" / "adr"
SCAN_DIRS = ("docs", "src", "include", "apps", "tests", "python", "schemas")
REF_RE = re.compile(r"\bADR[ -]?(\d{3,4})\b")

# References to decision numbers that live outside docs/adr/ (e.g. the
# paleo-workbench context) would be listed here.
EXEMPT: set[str] = set()


def main() -> int:
    existing = {p.name.split("-", 1)[0] for p in ADR_DIR.glob("*.md")}
    existing = {n.lstrip("0") or "0" for n in existing}
    dangling: list[tuple[str, int, str]] = []
    for scan in SCAN_DIRS:
        root = REPO / scan
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix not in {".md", ".cpp", ".hpp", ".h", ".py", ".txt", ".json"} or not path.is_file():
                continue
            if path.name == "check_adr_refs.py":
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                for match in REF_RE.finditer(line):
                    num = match.group(1).lstrip("0") or "0"
                    if num not in existing and match.group(0) not in EXEMPT:
                        dangling.append((str(path.relative_to(REPO)), lineno, match.group(0)))
    if dangling:
        print(f"::error:: {len(dangling)} dangling ADR reference(s):")
        for where, lineno, ref in dangling:
            print(f"  {where}:{lineno}: {ref}")
        return 1
    print(f"ADR references OK ({len(existing)} ADR files on disk)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
