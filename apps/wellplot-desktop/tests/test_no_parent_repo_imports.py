"""CI guard (workbench #734): engine-repo tests must not import the parent repo.

``paleo_workbench`` and the bare ``geoviz`` namespace only exist in the
parent monorepo checkout. Test modules that import them can never run in
this repository (module-level skip or collection error) and silently add
zero coverage — the exact failure mode that stranded
test_well_section_datum.py / test_well_section_workbench.py here.
"""

from __future__ import annotations

import ast
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent

# Top-level modules that only exist in the parent workbench checkout.
_PARENT_ONLY_TOPLEVEL = {"paleo_workbench", "geoviz"}


def _imported_toplevel_modules(tree: ast.Module) -> set[str]:
    """Only module-level imports break collection. Nested try/import is OK."""
    names: set[str] = set()
    for node in tree.body:
        if isinstance(node, ast.Import):
            names.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            if node.level == 0 and node.module:
                names.add(node.module.split(".")[0])
    return names


def test_no_test_module_imports_the_parent_repo() -> None:
    offenders: dict[str, set[str]] = {}
    for path in sorted(TESTS_DIR.glob("test_*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        bad = _imported_toplevel_modules(tree) & _PARENT_ONLY_TOPLEVEL
        if bad:
            offenders[path.name] = bad

    assert not offenders, (
        "test modules importing parent-repo packages (they can never run "
        f"here; port them to the paleo-workbench suite): {offenders}"
    )
