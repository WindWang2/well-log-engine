# WellPlot Desktop

**Display name:** WellPlot Desktop  
**Python package:** `well_log_workstation`  
**Lives in:** [WellLogEngine](../../README.md) under `apps/wellplot-desktop/`

Standalone log-first Qt desktop host that:

- Owns workspace, LAS import, Well Content Tree, Display Set, Graphic|Table dual view
- Optionally embeds engine `WellLogView` via `engine_bridge` when the `welllog` wheel is available
- Falls back to host `MultiTrackCanvas` without the engine

### Example **or** product

This app is **not** labeled “demo-only.” It is a real host built on the SDK. How we treat it over time depends on completeness:

| Maturity | Role |
|----------|------|
| Integration / early ship | **Example** — complete, runnable path for calling WellLogEngine |
| Polish, packaging, support | **Product** — installable WellPlot Desktop as a first-class offering |

Both readings are valid; ship gates and branding can tighten when the product track is chosen explicitly. It is **not** part of Paleo Workbench’s main package tree.

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
