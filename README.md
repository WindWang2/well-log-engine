# WellLogEngine

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)
![Qt](https://img.shields.io/badge/Qt-6_Widgets-41CD52?logo=qt)
![OpenGL](https://img.shields.io/badge/OpenGL-3.3_Core-5586A4?logo=opengl)
![License](https://img.shields.io/badge/License-MIT-green)

Host-independent **C++20 well-log rendering SDK** for high-sample-rate, long-depth, multi-track, and multi-well correlation scenes. Drives standalone Qt desktop apps or embeds as a native `QWidget` in PySide6 hosts (Shiboken bindings optional).

This repository is published as a **standalone open-source project**. Host applications (e.g. [paleo-workbench](https://github.com/WindWang2/paleo-workbench) WellPlot Desktop) depend on it one-way; the engine does **not** depend on Paleo Workbench.

## Features (phase-one scope)

- Immutable owned log buffers; reversible depth transform chain
- Composable track layers, hierarchical curve LOD, retained prepared scene
- OpenGL 3.3 Core interactive rendering (no software interactive fallback)
- Qt 6 Widgets adapter; optional PySide6 / Shiboken6
- Virtualized table projection; PDF / SVG / raster export paths
- Shared semantic selection between graphic and table (ADR 0024)

## Documentation

| Doc | Path |
|-----|------|
| Requirements | [docs/requirements.md](docs/requirements.md) |
| Architecture | [docs/architecture.md](docs/architecture.md) |
| Data model & API | [docs/data-model-and-api.md](docs/data-model-and-api.md) |
| Rendering | [docs/rendering.md](docs/rendering.md) |
| Qt / Python integration | [docs/qt-python-integration.md](docs/qt-python-integration.md) |
| Table & export | [docs/table-and-export.md](docs/table-and-export.md) |
| Quality / security / perf | [docs/quality-security-performance.md](docs/quality-security-performance.md) |
| Implementation roadmap | [docs/implementation-roadmap.md](docs/implementation-roadmap.md) |
| Decision index (ADRs) | [docs/decision-log.md](docs/decision-log.md) |
| ADR bodies | [docs/adr/](docs/adr/) |

## Build (CMake)

Requirements: CMake ≥ 3.24, C++20 compiler, Qt 6 Widgets (for Qt targets), optional vcpkg (`VCPKG_ROOT`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Presets: see [CMakePresets.json](CMakePresets.json). Dependencies: [vcpkg.json](vcpkg.json) (harfbuzz, freetype, icu, zlib).

Python wheel (optional): [pyproject.toml](pyproject.toml) and [python/README.md](python/README.md).

## WellPlot Desktop (`apps/wellplot-desktop`)

Log-first Qt desktop host for this engine: workspace, LAS, multi-track, Graphic|Table, optional `WellLogView` embed.

**Positioning:** an **example** of SDK integration **and/or** a **product**, depending on how far we take polish, packaging, and support. Not “demo-only”; not locked as the only commercial face of the SDK either.

```bash
cd apps/wellplot-desktop
pip install -e ".[dev]"
python -m well_log_workstation
```

Details: [apps/wellplot-desktop/README.md](apps/wellplot-desktop/README.md).

## Layout

```
well-log-engine/
├── include/          # Public C++ headers
├── src/              # core, scene, session, render_gl, qtwidgets, export_*, …
├── apps/
│   └── wellplot-desktop/   # Host app (example and/or product — see its README)
├── tests/            # unit / integration / qt / python
├── benchmarks/
├── schemas/          # e.g. manifest JSON schema
├── cmake/
├── docs/             # design docs + docs/adr/
├── python/           # binding packaging helpers
└── CMakeLists.txt
```

CMake package exports layered targets under `WellLog::*` (see ADR 0035).

## Relationship to hosts

| Component | Role |
|-----------|------|
| **WellLogEngine** (this repo root) | SDK — data model, scene, GL, export, optional Qt/Python adapters |
| **WellPlot Desktop** (`apps/wellplot-desktop`) | Host application that calls the SDK (example path today; product path as it matures) |
| [paleo-workbench](https://github.com/WindWang2/paleo-workbench) | Larger workbench monorepo (maps, seismic, …); consumes this engine as a submodule |

Legacy host QPainter multi-track remains a fallback when the engine wheel is unavailable.

## License

[MIT](LICENSE) — Copyright (c) 2026 Kevin
