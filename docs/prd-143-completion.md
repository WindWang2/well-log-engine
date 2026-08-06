# PRD #143 completion brief — Independent C++20 WellLogEngine

**PRD:** [PRD: 独立高性能 C++20 WellLogEngine](https://github.com/WindWang2/paleo-workbench/issues/143)  
**Status:** Implemented (phase-one SDK + Workbench default path)  
**Branch evidence:** `agent/welllog-pdf-spike-185` (includes through `#174` / `b8b3232`)  
**Date:** 2026-08-03

This document closes the parent PRD by mapping **delivered capability** to the
PRD solution, recording **explicit deferred items** (still out of scope or
process-only), and pointing at the **release gate**.

---

## Outcome

| Goal | Result |
|------|--------|
| Independent embeddable C++20 SDK | `well-log-engine/` CMake project, package consumers, semantic version 0.1.0 |
| Session-owned interaction state | `WellLogSession` + View Commands / Events (no host mirror) |
| OpenGL 3.3 Core interactive path | `WellLogView` (Qt Widgets) + RenderGL |
| Hierarchical LOD / Prepared Scene | Curve LOD + retained scene; budgeted caches |
| NumPy / Arrow zero-copy | Shiboken view + optional `WellLog::Arrow` |
| Multi-well surface + depth transform | Layout, shared viewport, marker align, overlays |
| Tables + export | Table Projection; PDF/SVG/PNG/TIFF; XLSX/XML/CSV |
| Patch / undo | DocumentPatch, history, interpretation + layout coverage |
| Streaming append | Composite buffers, AppendBatch, coalesce, fixed/follow viewport |
| Source adapters | LAS, DLIS, LIS, Format716 |
| Security / fuzz / stress | Container security, asset URI policy, fuzz corpora, Qt/GC/LRW stress |
| Workbench default | `PALEO_USE_WELLLOG_ENGINE` **default ON**; Legacy retained |

**Legacy paths are not deleted** (PRD out-of-scope; #174 AC).

---

## Ticket map (tracer bullets → closed)

| Area | Issues (all CLOSED) |
|------|---------------------|
| Foundation / headless | #144 #145 |
| Qt / Python / NumPy | #146 #147 |
| Dense LOD / multi-scale / fill | #148 #149 #151 |
| Layers (interval, pattern, text, image, custom) | #150 #152 #153 |
| Table + selection | #154 |
| Export tables / vector / raster | #155 #156 #157 #185–#189 |
| Patch / undo | #158 #202–#206 |
| QC / derived | #159 |
| Multi-well / transform | #160 #161 |
| Append stream | #162 #196–#201 |
| Arrow | #163 |
| Sources | #164–#167 |
| Observability | #168 |
| Workbench migration | #169 #170 |
| Security | #171 #172 |
| Lifecycle stress | #173 |
| Release gate + default flag | #174 |
| Hygiene | #183 #184 |

Sub-tickets under #158 / #162 are included above.

---

## PRD user-story themes → delivery

| Theme (US#) | Delivery highlights |
|-------------|---------------------|
| Dense interactive curves (1–7) | LOD, null breaks, axis direction, diagnostics |
| Multi-well correlation (8–15) | Surface layout, shared depth, markers, overlays |
| Multi-scale tracks / fill (16–20) | Explicit scales, crossover fill, headers via presentation |
| Composable layers / pattern / text / image (21–29) | Interval/pattern/text/image layers; tile pyramid |
| Pick / selection / table (30–41) | CPU pick, selection sync, table projection + copy/export |
| Physical export (42–49) | PDF/SVG pagination, raster async cancel, snapshot revision |
| Edit / QC / patch (50–56) | Patch, undo/redo, QC mask, derived curve, UUID/revision |
| Append stream (57–59) | Composite buffer, atomic batch, fixed/follow |
| Host embed (60–66) | QWidget + Shiboken, commands/events, custom layer |
| Capability / no SW interactive fallback (67–69) | Capability report; table/SVG without GL |
| Perf / observability (70+) | Budgets, profiler overlay, Chrome Trace, release-gate harness |

Detailed API and ADRs: `docs/architecture.md`, `docs/decision-log.md`, repo `docs/adr/0004+`.

---

## Release gate (#174)

| Layer | How |
|-------|-----|
| Structural | `ctest -L release-gate` / `scripts/run_release_gate.sh` |
| Full 1e8 scene | `WELLLOG_GATE_SCALE=full` (fixed workstation) |
| Frame P95 absolute | Optional GL benchmark on ADR 0014 hardware — **not** shared CI |
| Docs | `docs/release-gate.md`, `docs/sbom-and-licenses.md` |

---

## Explicitly still deferred (PRD Out of Scope or process)

These do **not** block PRD close; they remain product/process follow-ups:

| Item | Notes |
|------|--------|
| macOS / ARM / mobile | Phase-one platforms: Win/Linux x64 only |
| QML / Qt Quick | Widgets-first |
| CGM; Skia/RHI/Metal; QPainter interactive fallback | ADR 0033 |
| WITSML/MQTT/live protocol clients | Adapters only for disk sources |
| Delete Legacy geoviz paths | Separate review ticket required |
| Full 3-way patch merge | Not in phase one |
| Absolute frame P95 in cloud CI | Forbidden by QSP §4.3 |
| Full Windows GPU matrix + generated SBOM artifact | Process at release cut (recipes documented) |
| Continuous libFuzzer in CI | Deterministic corpus fuzz shipped (#172) |

---

## How to verify a green tree

```bash
# Engine structural gate (exact label)
cmake --build well-log-engine/build/dev-shared -j
ctest --test-dir well-log-engine/build/dev-shared -L '^release-gate$' --output-on-failure

# Workbench default engine flag
pytest tests/test_welllog_engine_adapter.py tests/test_well_log_canvas_panel.py -q
```

---

## Conclusion

PRD **#143** phase-one solution is **delivered** as an independent WellLogEngine
SDK with Workbench dual-path migration **defaulting to the engine**. Remaining
items are either **documented out of scope** or **release-process evidence**
(fixed hardware frame SLOs, multi-GPU matrix, SBOM generation), not open
implementation tickets under this PRD.
