# SBOM, licenses, and toolchain inventory (#174)

Release packages for WellLogEngine should include:

1. **SBOM** (Software Bill of Materials) — SPDX or CycloneDX JSON  
2. **License notice** for the engine and bundled third parties  
3. **Toolchain / driver / dependency list** used for the acceptance build  
4. **Benchmark report** from ADR 0014 full gate (when run)

## Generate SBOM (recommended)

From a configured build tree or installed prefix:

```bash
# Example with Syft (install separately)
syft dir:well-log-engine -o cyclonedx-json > welllog-sbom.cdx.json
# or
syft dir:well-log-engine/build/dev-shared -o spdx-json > welllog-sbom.spdx.json
```

If Syft is unavailable, ship a **manual dependency inventory** from:

- `well-log-engine/vcpkg.json` (when used)
- CMake `find_package` results (Qt6, ZLIB, Arrow optional, FreeType/HarfBuzz/ICU for text)
- Python wheel metadata (`pyproject.toml` / `RECORD`)

## First-party license

WellLogEngine follows the repository root license for the Paleo Workbench project
unless a separate `well-log-engine/LICENSE` is added at packaging time.

Bundled test fonts (not shipped in runtime wheels by default):

- `tests/assets/fonts/NOTO-LICENSE.txt`
- `tests/assets/fonts/SOURCE-HAN-LICENSE.txt`

## Toolchain template (fill at release cut)

| Field | Example |
|-------|---------|
| OS | Ubuntu 24.04 / Windows 11 |
| CPU | 8-core x64 |
| RAM | 32 GB |
| GPU / driver | Intel Iris Xe / NVIDIA xxx / Mesa yy |
| Compiler | GCC 14 / MSVC 19.4x / Clang 18 |
| CMake | 3.28+ |
| Qt | 6.x |
| Python | 3.12 / 3.13 |
| Engine version | 0.1.0 |
| Git SHA | _(fill)_ |
| Gate report | `WELLLOG_GATE_SCALE=full` log + dense_curve_benchmark JSON |

## Benchmark report

Attach:

- stdout from `welllog_release_gate_scenario_tests` with `WELLLOG_GATE_SCALE=full`
- optional `welllog_dense_curve_benchmark` JSON (`schema: welllog.dense-curve-benchmark.v1`)
