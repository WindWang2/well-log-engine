# WellLogEngine release gate (#174, ADR 0014)

Publish gate for the independent C++20 WellLogEngine SDK and Paleo Workbench
default path. **Legacy is retained** and remains switchable.

## Product default

| Surface | Default | Opt-out |
|---------|---------|---------|
| `PALEO_USE_WELLLOG_ENGINE` | **ON** (unset) | `0` / `false` / `no` / `off` / `legacy` |
| Single-well canvas | Engine combo | Explicit "Legacy (QPainter)" |
| Stratigraphy dual-path | Engine combo | Explicit "Legacy (CrossWell)" |

Engine failure still falls back to painting Legacy tracks so the page remains usable.

## Reference scenario (ADR 0014)

| Parameter | Full (publish) | CI (default harness) |
|-----------|----------------|----------------------|
| Wells | 20 | 2 |
| Curves | 200 (10/well) | 8 (4/well) |
| Samples / curve | 500 000 | 8 000 |
| Total samples | **100 000 000** | 64 000 |
| Visible tracks | 100 (5/well) | 4 |
| Discrete markers | 100 000 | 100 |
| Env | `WELLLOG_GATE_SCALE=full` | unset / `ci` |

### SLOs (full / fixed 4K workstation)

| Metric | Gate |
|--------|------|
| Frame P95 / P99 | ≤ 16.7 ms / ≤ 33.3 ms |
| Pick P95 | ≤ 16 ms |
| GUI block | ≤ 8 ms |
| First interactive | ≤ 2 s |
| Engine derived memory | ≤ 50 % of raw curve buffers |
| GPU | no full raw-curve copy |

Shared CI **must not** assert absolute frame times (QSP §4.3). The headless
harness enforces **structure + memory ratio + pick budget + SVG** only.

## How to run

```bash
# Structural publish subset (exact label; does not include full 1e8)
well-log-engine/scripts/run_release_gate.sh
# or:
ctest --test-dir well-log-engine/build/dev-shared -L '^release-gate$' --output-on-failure

# Full 1e8 scale (long; needs RAM) — structural + memory/LOD; reports first-interactive ms
ctest --test-dir … -L '^release-gate-full$' -V
# Absolute ≤2s first-interactive only on ADR 0014 reference HW:
WELLLOG_GATE_ENFORCE_SLO=1 ctest --test-dir … -L '^release-gate-full$' -V

# Workbench default flag
pytest tests/test_welllog_engine_adapter.py tests/test_well_log_canvas_panel.py -q
```

`run_release_gate.sh` additionally runs the environment-governance smoke chain
(E1–E5): L1 binding-environment diagnostics, native binding import +
`welllog.python.*` CTest tests, raster focused tests, wheel smoke in a clean
target dir, and the Desktop binding-first tests under
`WLWS_REQUIRE_NATIVE_BINDING=1` strict mode. Wheel smoke needs a wheel
(`WELLLOG_WHEEL` or `dist/*.whl`); binding smokes need the controlled Python
runtime from `scripts/python_env.sh` (see
`docs/environment-binding-policy.md`). Overrides: `WELLLOG_BUILD_DIR`,
`WELLLOG_WHEEL`, `WELLLOG_SKIP_WHEEL`, `WELLLOG_SKIP_DESKTOP`.

## Platform matrix (process)

Record evidence for each cell before a public release:

| OS | GPU stack | Wheel / install | Status |
|----|-----------|-----------------|--------|
| Linux x64 | Mesa (CI software) | source / wheel | automated structural gate |
| Linux x64 | NVIDIA / AMD / Intel | wheel | manual frame SLO |
| Windows x64 | NVIDIA / AMD / Intel | wheel | manual frame SLO |

Python **3.12** and **3.13** wheel smoke: `welllog.python.qt-embedding` + lifecycle stress when `WELLLOG_BUILD_PYTHON=ON`.

## Package / version gates (already automated)

| CTest | Role |
|-------|------|
| `welllog.package.consumer` | CMake `find_package` consumer |
| `welllog.svg.package.consumer` | SVG export consumer |
| `welllog.qt.package.consumer` | Qt consumer (optional) |
| `welllog.manifest` | Schema v1/v2 accept; v3 reject; SDK requirement |
| CMake package version | `SameMajorVersion` (0.1.0) |

## Security / stress prerequisites

| CTest | Ticket |
|-------|--------|
| `welllog.fuzz-*` | #172 |
| `welllog.container-security` | #171 |
| `welllog.async-lrw-stress` | #173 |
| `welllog.qt-context-lifecycle-stress` | #173 |

## SBOM / licenses / toolchain

See `docs/sbom-and-licenses.md`. Produce SBOM with Syft/CycloneDX at release cut;
commit or attach the generated document to the release notes.

## Explicit non-goals of this gate ticket

- **Deleting Legacy** — requires a separate review ticket.
- Absolute frame P95 in shared CI.
- Replacing the full Windows GPU matrix with Linux software GL alone.
