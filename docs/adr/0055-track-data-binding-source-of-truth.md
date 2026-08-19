# ADR 0055 — Track/Data Binding Source of Truth

Status: Accepted (2026-08)

## Context

The professional track workflow needs fast, consistent answers to binding
questions — track → curves/scales/layers, curve → layers, layer → track,
scale → track — for every hover, property-panel refresh, drag target
resolution and command validation. Before this ADR every such question was a
linear scan (`PreparedScene::track_id_for_layer` walked all seven layer
vectors; `patchable_entity_at` walked every collection), and the desktop
host re-sent entire `submit_multi_track` payloads after any edit.

The binding facts themselves were already settled by ADR 0004/0017/0025:
`WellLogDocument` is the raw/domain truth, `ScenePresentation`
(`TrackSpec`/`TrackScaleSpec`/`CurveLayerSpec`) is the visual binding truth,
the session is the mutation truth. What was missing was (a) an efficient
resolver over those facts and (b) high-level, validated, undoable workflow
commands so hosts stop hand-building patches and re-submitting documents.

## Decision

1. **Immutable snapshot indexes, not live caches.**
   - `DocumentBindingIndex` (core) indexes one `WellLogDocument` snapshot:
     axes/curves/masks/intervals/... by id plus `curves_on_axis` in document
     order.
   - `PresentationBindingIndex` (scene) indexes one `ScenePresentation`
     snapshot: tracks in z-order (the authoritative layout order), scales and
     layers by id and by track, curve → layers, layer → track across every
     layer kind, scale → track, and `all_layers_of_track` (the exact set a
     track removal must cascade over).
   Both are built in O(entity count) with zero curve-buffer access and hold
   pointers into the immutable snapshot they were built from. Nothing keeps
   a second copy of binding facts: **there is still no `Track.curves[]`
   anywhere** — the presentation layer remains the only binding truth.

2. **One mutation engine.** The track-data commands (ADR 0056) mutate state
   exclusively through `ApplyPatchCommand`. The session gains no second
   writer, so atomicity, base-revision safety, preflight validation
   (dangling curve/scale/track references, unit compatibility, log
   constraints), undo/redo, events and LOD-cache reuse all apply unchanged.

3. **Duplicate bindings are allowed and meaningful.** The same curve may be
   bound several times (distinct layers, even in one track); each layer is
   an independent visual presentation of the same immutable buffer. The
   generated `DuplicateCurveLayerCommand` relies on this. Duplicate ids
   remain globally invalid (preflight).

4. **Removal cascades.** `RemoveTrackCommand` removes the track together
   with its scales and every layer placed in it as ONE atomic patch. A
   partial removal could never commit (preflight rejects dangling
   references), so the cascade is the only semantics that can succeed —
   there is no configurable "leave dangling" mode by design.
   `UnbindCurveFromTrackCommand` removes only the layer; scales survive
   (a user-tuned range outlives re-binding), and the engine document keeps
   the curve — data truth is never deleted by a presentation edit.

5. **Auto range is an explicit operation.** `BindCurveToTrackCommand` (when
   generating a scale) and `AutoRangeTrackScaleCommand` compute the finite
   extent of the bound curves once, on command. Nothing recomputes a scale
   from visible min/max per frame — the scale a user sees only changes when
   a host/user operation changes it. Log scales fit the positive extent;
   all-non-positive log fits are rejected (`track_scale_range_invalid`).

6. **Moving never copies.** `MoveCurveLayerCommand` keeps the layer id
   (LOD caches and undo history stay continuous) and rewrites only its
   track/scale binding and z-order. Raw curve and sampling-axis buffers are
   untouched by construction — the benchmark
   (`welllog.track-command-benchmark`) gates on the buffer address and
   capacity being bit-for-bit identical after the whole op workload.

## Consequences

- Hover/refresh/command paths resolve bindings in O(1) hash lookups; the
  preparer still builds its own local maps (unchanged) but the session
  command layer and hosts use the shared indexes.
- `WellLogSession::presentation(id)` is now public read-only API: track
  managers, data trees and hover inspectors enumerate the live presentation
  through it plus `PresentationBindingIndex`.
- `EntityId::generate()` (random v4) exists for hosts and commands that
  need an id for a to-be-created entity; hosts wanting reproducible ids
  still supply their own.
- Known cost boundary: a committed patch re-executes the session's existing
  document/presentation replace + scene re-prepare, which scales with the
  presented sample count (~15 ms at a typical 200k-sample well; ~0.8 s at
  10 M samples in the benchmark). The command layer itself is µs-level.
  Incremental scene preparation is future renderer work and out of scope
  here.
