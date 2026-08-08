"""E3 — binding-first strict mode for the engine bridge.

WLWS_REQUIRE_NATIVE_BINDING=1 must make probe_engine() prove the *package*
path (welllog.WellLogView). A broken package init must surface as a failure,
never be papered over by the raw-extension fallback. End-user default
(unset) keeps the graceful degrade.
"""

from __future__ import annotations

import importlib.util
import os
import sys
import types

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.engine_bridge import (  # noqa: E402
    probe_engine,
    reset_engine_capability_cache,
)


@pytest.fixture(autouse=True)
def _reset_probe() -> None:
    reset_engine_capability_cache()
    yield
    reset_engine_capability_cache()


def _native_importable() -> bool:
    """True when the welllog package is importable in this test env."""
    try:
        import welllog  # noqa: F401

        return True
    except Exception:  # noqa: BLE001
        return False


def _install_broken_welllog(monkeypatch: pytest.MonkeyPatch) -> None:
    """Replace the welllog package with a stub missing WellLogView."""
    stub = types.ModuleType("welllog")
    monkeypatch.setitem(sys.modules, "welllog", stub)
    # Also evict any already-imported submodule so imports re-enter the stub.
    monkeypatch.delitem(sys.modules, "welllog._QtWidgets", raising=False)


@pytest.mark.skipif(not _native_importable(), reason="welllog not importable")
def test_strict_mode_proves_native_package_path(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.setenv("WLWS_REQUIRE_NATIVE_BINDING", "1")
    reset_engine_capability_cache()

    cap = probe_engine()
    assert cap.available is True
    assert cap.mode == "native", (
        "strict mode must resolve through the welllog package, "
        f"got mode={cap.mode!r} detail={cap.detail!r}"
    )
    assert cap.well_log_view_cls is not None


@pytest.mark.skipif(not _native_importable(), reason="welllog not importable")
def test_strict_mode_fails_fast_on_broken_package_init(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    """Package init broken + a loadable-looking .so on sys.path.

    Strict mode must report unavailable with the *package* error and must
    NOT attempt the raw-extension fallback.
    """
    fake = tmp_path / "fake-site"
    ext_dir = fake / "welllog"
    ext_dir.mkdir(parents=True)
    # Garbage file that would fail to exec if the fallback were attempted.
    (ext_dir / "_QtWidgets.abi3.so").write_bytes(b"not an elf")

    monkeypatch.syspath_prepend(str(fake))
    _install_broken_welllog(monkeypatch)
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.setenv("WLWS_REQUIRE_NATIVE_BINDING", "1")
    reset_engine_capability_cache()

    cap = probe_engine()
    assert cap.available is False
    assert cap.mode is None
    assert "WLWS_REQUIRE_NATIVE_BINDING" in cap.detail
    assert "import failed" in cap.detail
    assert "_QtWidgets" not in cap.detail, (
        "strict mode must not fall through to the raw-extension path"
    )


@pytest.mark.skipif(not _native_importable(), reason="welllog not importable")
def test_non_strict_still_attempts_extension_fallback(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    """Without strict mode the fallback is attempted and its failure is
    visible in the detail (existing end-user degrade behaviour)."""
    fake = tmp_path / "fake-site"
    ext_dir = fake / "welllog"
    ext_dir.mkdir(parents=True)
    (ext_dir / "_QtWidgets.abi3.so").write_bytes(b"not an elf")

    monkeypatch.syspath_prepend(str(fake))
    _install_broken_welllog(monkeypatch)
    monkeypatch.delenv("WLWS_DISABLE_ENGINE", raising=False)
    monkeypatch.delenv("WLWS_REQUIRE_NATIVE_BINDING", raising=False)
    reset_engine_capability_cache()

    cap = probe_engine()
    assert cap.available is False
    # The extension fallback was attempted and failed — detail combines the
    # package error and the extension load error.
    assert "_QtWidgets" in cap.detail


def test_strict_mode_does_not_override_explicit_disable(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("WLWS_DISABLE_ENGINE", "1")
    monkeypatch.setenv("WLWS_REQUIRE_NATIVE_BINDING", "1")
    reset_engine_capability_cache()
    cap = probe_engine()
    assert cap.available is False
    assert "WLWS_DISABLE_ENGINE" in cap.detail
