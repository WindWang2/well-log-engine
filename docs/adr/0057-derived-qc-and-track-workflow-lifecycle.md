# ADR 0057 — Derived/QC Lifecycle through the Track/Data Workflow

Status: Accepted (2026-08)

## Context

ADR 0025 fixed the non-destructive editing invariants: raw curve buffers
are immutable, QC masks never rewrite values, derived curves carry
provenance (`input_curve_id`, `input_revision`, `algorithm id/version`,
`parameters`, `output_sampling_axis_id`, buffer identity) and turn stale
when their input changes. `tests/integration/qc_derived_curve_test.cpp`
already covers the core lifecycle (byte-identical buffers, table/graphics
policy, staleness, undo/redo, raw-patch rejection). The track/data
workflow (ADR 0055/0056) now touches the same documents, so this ADR
records how the two interact and what the workflow adds.

## Decision

1. **Presentation edits never touch derived/QC state.** Track commands
   rewrite only TrackSpec/TrackScaleSpec/CurveLayerSpec entities. A move,
   restyle or re-scale of a derived curve's layer re-presents the same
   derived buffer; freshness is unaffected (it depends on the input
   buffer identity, not on presentation bindings).

2. **Inspect exposes the lifecycle.** `resolve_curve_pick`
   (`scene/inspect.hpp`) resolves a hover pick into
   `CurvePickInfo` — mnemonic/display name/unit, raw value, QC state
   (`qc_state_at`, mask-aware), scale context and, for derived curves,
   the algorithm identity plus live `compute_derived_freshness` state.
   `WellLogView.hover_info()` ships the same dict to Python. Desktops
   therefore render "derived / stale" badges from one C++-resolved
   structure instead of re-deriving state host-side.

3. **QC display stays presentation-scoped.** `CurveLayerSpec.qc_display`
   (hide suspect/invalid/user-excluded) already controls drawability per
   layer; the track commands copy it verbatim on move/duplicate, and
   `SetCurveLayerStyleCommand` deliberately does not edit it (QC display
   policy is a layer-visibility concern, reached through patches or
   future commands if hosts ask). Mask edits remain document patches that
   invalidate only the affected curve geometry — never other wells
   (ADR 0024/0025 event scoping).

4. **Binding a curve does not validate its data quality.** A derived
   curve — fresh or stale — can be bound, moved and styled like any
   curve: staleness is a data-state report, not a binding error. Hosts
   read `hover_info()["derived_stale"]` / the document provenance to
   decide whether to recompute; the engine never silently re-runs an
   algorithm to "fix" a stale presentation (recomputation is an explicit
   host operation producing a new revision).

## Consequences

- The workflow composes with the existing derived/QC guarantees without
  new invariants; no lifecycle state is duplicated into presentation
  entities.
- Desktop stale/QC badges have a single source (`resolve_curve_pick` /
  `compute_derived_freshness`), keeping the one-selection-set /
  one-inspect-path discipline of ADR 0024/0030.
