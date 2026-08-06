# WellPlot Desktop

**Product name:** WellPlot Desktop  
**Python package:** `well_log_workstation`  
**Role in this monorepo:** **reference host / sample product** for [WellLogEngine](../../README.md).

A standalone log-first Qt desktop app that:

- Owns workspace, LAS import, Well Content Tree, Display Set, Graphic|Table dual view
- Optionally embeds engine `WellLogView` via `engine_bridge` when the `welllog` wheel is available
- Falls back to host `MultiTrackCanvas` without the engine

This app lives **inside** the engine repository so SDK consumers have a full end-to-end example—not inside Paleo Workbench.

## Run

From this directory (or engine root with `PYTHONPATH`):

```bash
# Install app deps (PySide6, lasio, numpy)
pip install -e ".[dev]"

# Optional: install/build WellLogEngine Python bindings (welllog) for engine canvas
# See ../../python/README.md and ../../docs/qt-python-integration.md

# Prefer Wayland on Linux — do not force xcb
unset QT_QPA_PLATFORM
python -m well_log_workstation
# or
wellplot-desktop
```

## Layout

```
apps/wellplot-desktop/
├── well_log_workstation/   # application package
├── tests/                  # pytest suite for the app
├── packaging/              # (under package) PyInstaller scripts
├── pyproject.toml
└── README.md
```

## Tests

```bash
cd apps/wellplot-desktop
pip install -e ".[dev]"
pytest
```

## Relationship

| Component | Location |
|-----------|----------|
| WellLogEngine SDK | repo root (`include/`, `src/`, CMake) |
| WellPlot Desktop (this) | `apps/wellplot-desktop/` |
| Paleo Workbench | separate host; may embed or omit this app |

## License

MIT — same as WellLogEngine.
