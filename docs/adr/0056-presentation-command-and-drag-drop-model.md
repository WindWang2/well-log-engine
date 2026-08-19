# ADR 0056 — Presentation Command Model & Desktop Drag/Drop Command Model

Status: Accepted (2026-08)

## Context

ADR 0025 made presentation edits patchable (`PatchableEntity`:
TrackSpec/TrackScaleSpec/CurveLayerSpec), but hosts had to hand-assemble
`DocumentPatch`es for every workflow action — enumerate the track's layers,
build the inverse edits, keep ids straight. In practice the desktop avoided
the patch path entirely: every tree drop, checkbox, scale spinbox change or
header drag re-sent a complete `submit_multi_track` payload (new document,
new presentation, new curve ids) — an O(all presented samples) round trip
per edit, and a second implicit source of truth (the rebuilt payload)
drifting from the host display-set model.

## Decision

### 1. Workflow commands over the patch engine (session)

`include/welllog/session/track_commands.hpp` adds the professional
vocabulary as first-class session commands, each implemented by resolving
the live document + presentation through the binding indexes (ADR 0055),
building one `DocumentPatch` at the live revision, and delegating to
`execute(ApplyPatchCommand)`:

| Command | Semantics |
|---|---|
| `AddTrackCommand` | append/insert track (nil id → generated; z nullopt → after last) |
| `RemoveTrackCommand` | atomic cascade: track + its scales + all its layers |
| `ReorderTracksCommand` | complete-permutation z-order rewrite (track header drag) |
| `ResizeTrackCommand` | width edit (header width drag) |
| `SetTrackHeaderCommand` / `SetTrackVisibilityCommand` | header spec / hide (hidden track keeps its slot, contributes no geometry/header — preparer skips every layer kind) |
| `BindCurveToTrackCommand` | new layer; nil scale reuses the track's first unit-matching scale, else generates one (auto-ranged on request); explicit scale must belong to the track and match units |
| `UnbindCurveFromTrackCommand` | remove one layer; scale survives; engine document keeps the curve |
| `MoveCurveLayerCommand` | keep layer id, rewrite track/scale binding, land on top of the target track |
| `DuplicateCurveLayerCommand` | second presentation of the same immutable buffer |
| `ReorderCurveLayersCommand` | complete-permutation in-track z rewrite |
| `SetCurveLayerVisibilityCommand` / `SetCurveLayerStyleCommand` | visibility / partial color+width style |
| `SetTrackScaleCommand` / `AutoRangeTrackScaleCommand` | partial scale edit / explicit fit-to-data (log ⇒ positive extent) |

Precise stable error keys: `track_entity_missing`,
`track_binding_invalid`, `track_order_incomplete`,
`track_scale_range_invalid`.

### 2. Python surface (one payload → one command)

`WellLogView.apply_track_command({"op": ..., "document_id": ..., ...})`
maps each op 1:1 onto the C++ commands; ids for created entities are
generated in the bridge and reported back (`track_id`/`layer_id`/
`new_layer_id`) so hosts can address follow-up edits.
`presentation_state(document_id)`, `selection_state()`,
`set_row_selection(axis, first, last)` and `hover_info()` expose the live
presentation, shared Selection Set and resolved hover inspect dict.

### 3. Desktop drag/drop mirrors, never re-sends

The desktop's display set × template model stays the persistence truth.
`engine_bridge.capture_engine_bindings` snapshots the engine entity ids per
host track id right after each full submit (the payload builder's
submission order), and `incremental_presentation_sync` mirrors host edits
into commands in two phases:

- **Phase A (structure):** `reorder_tracks`, `remove_track` (a host track
  that lost all layers leaves — the payload builder would omit it),
  `add_track` + `bind_curve` (a re-checked track whose curves are still in
  the engine document), `move_curve_layer`, `unbind_curve`, re-`bind_curve`
  from the persistent identity→engine-curve-id cache.
- **Phase B (values, against the post-A state):** `resize_track`,
  `set_scale`, `set_layer_style`, `reorder_curve_layers`.

Anything the snapshot cannot express — a NEW curve identity the engine
document cannot hold, an unknown template slot arrangement, a rejected
command — returns "fall back" and the shell takes the full re-submit path.
Commands are atomic, so a failed incremental attempt never leaves the
engine mid-edit. Shell call sites: the track props form, host canvas
header reorder/width drags, and the display-set rebuild path (tree drops /
checkboxes / template switches) all try incremental first.

### 4. Selection stays one set

The C++ TableModel now subscribes to session selection events when a
selection source is attached (marshalled onto the model's thread), so
graphics → table reflection needs no host polling; `set_row_selection`
remains the table → graphics direction over the same Selection Set
(ADR 0024). `WellLogView.set_row_selection` / `selection_state()` expose
the loop to Python hosts.

## Consequences

- Desktop edits become O(changed presentation entities) engine commands;
  no raw buffers are re-sent (ADR 0055 gates this in the benchmark).
- The engine document deliberately accumulates curves the host once
  displayed (unbind keeps data truth); a full re-submit remains the
  compaction point.
- Scope boundary: the desktop display-set model binds one curve per track
  slot, so a host-side "move curve to another track" is not representable
  in its persistence model yet — the engine-level move is complete and
  tested (C++ + bindings), and the desktop incremental sync already
  mirrors multi-layer hosts. The desktop adopts it when its display-set
  model grows multi-curve track slots.
