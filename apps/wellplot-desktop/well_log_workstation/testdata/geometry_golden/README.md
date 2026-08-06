# Geometry golden fixture (T14 + B1.GEOM)

Fixed single-well sample for **WellPlot Desktop** export layout and
multi-format geometry checks.

| Issue | Scope |
|-------|--------|
| **T14 / #302** | Qt-paint export layout mm subset @ **0.1 mm** |
| **B1.GEOM / #304** | Multi-format matrix (page box, depth map, CGM VDC, pagination, cross-format) toward §16 |

## Dataset

| Field | Value |
|-------|--------|
| File | `T14_GOLDEN_V1.las` |
| Well | `T14-GOLDEN-V1` |
| Depth | 1000.0 – 1010.0 m, step 1.0 |
| Curves | GR, RT, RHOB |
| Template | `std-gr-rt-den` |

Committed in-repo (no external download).

## Asserted format dimensions (B1.GEOM)

See `B1_FORMAT_MATRIX` in `well_log_workstation/geometry_golden.py` and
`tests/test_well_log_workstation_geometry_matrix.py`.

| Format / metric | Tolerance |
|-----------------|-----------|
| Qt-paint track left/width, depth→Y | **0.1 mm** |
| PageSpec / SVG viewBox A4 landscape | **0.1 mm** |
| CGM VDC ↔ mm pure round-trip | **0.1 mm** |
| CGM track left (export-path proxy) | **0.5 mm** entry (ADR 0054) |
| Cross-format layout vs CGM VDC | **0.1 mm** |
| Fixed pagination page count | exact |

**Deferred** (not claimed): engine PDF band-text anchors @ 0.1 mm; full CGM
scene clip vs host layout @ 0.1 mm; §16 full multi-well matrix.

## Updating goldens

Change frozen `GOLDEN_*` constants only with intentional layout edits; do not
loosen `TOL_MM` (0.1) without an ADR relative to §16. CGM entry tol may
tighten from 0.5 → 0.1 once scene-clip goldens land.
