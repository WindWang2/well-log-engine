#include <welllog/session/track_commands.hpp>

#include <welllog/core/document_index.hpp>
#include <welllog/scene/presentation_index.hpp>
#include <welllog/session/session.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace welllog {
namespace {

// The track commands resolve the live document + presentation through the
// binding indexes, then delegate to ApplyPatchCommand. This file holds NO
// independent mutation state — the patch engine remains the only writer.

[[nodiscard]] Error entity_missing(EntityId entity_id) {
  return Error{
      .code = ErrorCode::document_not_found,
      .severity = Severity::error,
      .entity_id = entity_id,
      .message = MessageKey::track_entity_missing,
      .arguments = {},
  };
}

[[nodiscard]] Error binding_invalid(EntityId entity_id) {
  return Error{
      .code = ErrorCode::invalid_presentation,
      .severity = Severity::error,
      .entity_id = entity_id,
      .message = MessageKey::track_binding_invalid,
      .arguments = {},
  };
}

[[nodiscard]] Error order_incomplete(EntityId document_id) {
  return Error{
      .code = ErrorCode::invalid_presentation,
      .severity = Severity::error,
      .entity_id = document_id,
      .message = MessageKey::track_order_incomplete,
      .arguments = {},
  };
}

[[nodiscard]] Error scale_range_invalid(EntityId scale_id) {
  return Error{
      .code = ErrorCode::invalid_presentation,
      .severity = Severity::error,
      .entity_id = scale_id,
      .message = MessageKey::track_scale_range_invalid,
      .arguments = {},
  };
}

[[nodiscard]] Error duplicate_id(EntityId entity_id) {
  return Error{
      .code = ErrorCode::duplicate_entity_id,
      .severity = Severity::error,
      .entity_id = entity_id,
      .message = MessageKey::entity_identity_duplicated,
      .arguments = {},
  };
}

// Everything a track command needs: the live document + presentation plus
// their binding indexes. Track commands mutate the presentation, so one must
// already exist (ApplyPatchCommand rejects presentation edits without one;
// failing here gives the precise error instead).
struct BindingContext {
  const WellLogDocument *document{};
  const ScenePresentation *presentation{};
  DocumentBindingIndex document_index;
  PresentationBindingIndex presentation_index;
};

[[nodiscard]] std::optional<Error>
resolve_context(
    const std::unordered_map<EntityId, std::shared_ptr<const WellLogDocument>,
                             EntityIdHash> &documents,
    const std::unordered_map<EntityId, ScenePresentation, EntityIdHash>
        &presentations,
    EntityId document_id, BindingContext &context) {
  const auto document_entry = documents.find(document_id);
  if (document_entry == documents.end()) {
    return Error{
        .code = ErrorCode::document_not_found,
        .severity = Severity::error,
        .entity_id = document_id,
        .message = MessageKey::track_entity_missing,
        .arguments = {},
    };
  }
  const auto presentation_entry = presentations.find(document_id);
  if (presentation_entry == presentations.end()) {
    return binding_invalid(document_id);
  }
  context.document = document_entry->second.get();
  context.presentation = &presentation_entry->second;
  context.document_index = DocumentBindingIndex{*context.document};
  context.presentation_index =
      PresentationBindingIndex{*context.presentation};
  return std::nullopt;
}

// The next z-order slot after the current maximum in a collection (0 for an
// empty collection).
template <typename Entity>
[[nodiscard]] std::int32_t
z_order_after_last(std::span<const Entity *const> entities) noexcept {
  std::int32_t next = 0;
  for (const auto *entity : entities) {
    next = std::max(next, entity->z_order + 1);
  }
  return next;
}

// The next z-order slot after the current maximum among a track's curve
// layers (0 when the track has none).
[[nodiscard]] std::int32_t
top_of_track(const PresentationBindingIndex &index,
             EntityId track_id) noexcept {
  return z_order_after_last<const CurveLayerSpec>(
      index.curve_layers_of_track(track_id));
}

// The first scale in a track whose unit equals `unit` (nullptr when the track
// has none) — the reuse rule for nil scale ids.
[[nodiscard]] const TrackScaleSpec *
compatible_scale(const PresentationBindingIndex &index, EntityId track_id,
                 const std::string &unit) noexcept {
  for (const auto *scale : index.scales_of_track(track_id)) {
    if (scale->unit == unit) {
      return scale;
    }
  }
  return nullptr;
}

struct ValueExtent {
  double minimum{0.0};
  double maximum{1.0};
};

// Finite extent of one curve's values with the documented auto-range
// fallbacks: all non-finite ⇒ 0..1, constant curve ⇒ v..v+1. The scan is
// O(samples) per explicit auto-range operation, never per frame.
[[nodiscard]] ValueExtent curve_value_extent(const Curve &curve) noexcept {
  auto minimum = std::numeric_limits<double>::infinity();
  auto maximum = -std::numeric_limits<double>::infinity();
  const auto length = curve.values.length();
  for (std::uint64_t index = 0; index < length; ++index) {
    const auto value = curve.values.value_as_double(index);
    if (value.has_value() && std::isfinite(*value)) {
      minimum = std::min(minimum, *value);
      maximum = std::max(maximum, *value);
    }
  }
  if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
    return {};
  }
  if (minimum == maximum) {
    return {.minimum = minimum, .maximum = minimum + 1.0};
  }
  return {.minimum = minimum, .maximum = maximum};
}

[[nodiscard]] bool valid_scale_range(double minimum, double maximum,
                                     ScaleMode mode) noexcept {
  return std::isfinite(minimum) && std::isfinite(maximum) &&
         minimum < maximum &&
         (mode != ScaleMode::logarithmic || minimum > 0.0);
}

// The scale edit shared by SetTrackScaleCommand and
// AutoRangeTrackScaleCommand: merges optional fields over the current scale,
// validates the resulting range, and emits one upsert edit.
[[nodiscard]] std::optional<Error>
upsert_scale_edit(const TrackScaleSpec &current, std::optional<ScaleMode> mode,
                  std::optional<double> minimum, std::optional<double> maximum,
                  std::optional<ScaleDirection> direction,
                  std::optional<std::string> unit, DocumentPatch &patch) {
  TrackScaleSpec updated = current;
  if (mode.has_value()) {
    updated.mode = *mode;
  }
  if (minimum.has_value()) {
    updated.minimum = *minimum;
  }
  if (maximum.has_value()) {
    updated.maximum = *maximum;
  }
  if (direction.has_value()) {
    updated.direction = *direction;
  }
  if (unit.has_value()) {
    updated.unit = std::move(*unit);
  }
  if (updated.unit.empty() ||
      !valid_scale_range(updated.minimum, updated.maximum, updated.mode)) {
    return scale_range_invalid(current.id);
  }
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  return std::nullopt;
}

// Outcome of resolving the scale a bind/move targets.
struct ResolvedScale {
  // nullptr when a new scale was generated into `generated_id` (its upsert
  // edit is already appended to the patch).
  const TrackScaleSpec *existing{};
  EntityId generated_id{};
  std::optional<Error> error;
};

// Builds (or reuses) the scale a bind/move targets. `explicit_scale_id` is
// validated against the track; nil resolves the reuse-or-create policy:
// the target track's first scale with the curve's unit, else a generated
// scale (auto-ranged from the curve buffer when `auto_range`).
[[nodiscard]] ResolvedScale
resolve_bind_scale(const BindingContext &context, EntityId track_id,
                   const Curve &curve, EntityId explicit_scale_id,
                   bool auto_range, DocumentPatch &patch) {
  if (!explicit_scale_id.is_nil()) {
    const auto *scale = context.presentation_index.scale(explicit_scale_id);
    if (scale == nullptr) {
      return ResolvedScale{.existing = nullptr, .generated_id = {},
                         .error = entity_missing(explicit_scale_id)};
    }
    if (scale->track_id != track_id || scale->unit != curve.unit) {
      return ResolvedScale{.existing = nullptr, .generated_id = {},
                         .error = binding_invalid(explicit_scale_id)};
    }
    return ResolvedScale{.existing = scale, .generated_id = {},
                         .error = {}};
  }
  if (const auto *reused = compatible_scale(context.presentation_index,
                                            track_id, curve.unit);
      reused != nullptr) {
    return ResolvedScale{.existing = reused, .generated_id = {},
                         .error = {}};
  }
  const auto extent = auto_range ? curve_value_extent(curve) : ValueExtent{};
  auto generated_id = EntityId::generate();
  TrackScaleSpec scale{
      .id = generated_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = extent.minimum,
      .maximum = extent.maximum,
      .direction = ScaleDirection::left_to_right,
      .unit = curve.unit,
  };
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(scale)});
  return ResolvedScale{.existing = nullptr,
                         .generated_id = generated_id, .error = {}};
}

[[nodiscard]] EntityId resolved_scale_id(const ResolvedScale &resolved) {
  return resolved.existing != nullptr ? resolved.existing->id
                                      : resolved.generated_id;
}

} // namespace

Result<CommandReceipt>
WellLogSession::execute(const AddTrackCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  if (!std::isfinite(command.width.value) || command.width.value <= 0.0) {
    return binding_invalid(command.track_id);
  }
  if (!command.track_id.is_nil() &&
      context.presentation_index.track(command.track_id) != nullptr) {
    return duplicate_id(command.track_id);
  }
  const auto z_order =
      command.z_order.has_value()
          ? *command.z_order
          : z_order_after_last<const TrackSpec>(
                context.presentation_index.tracks_in_z_order());
  auto track = TrackSpec{
      .id = command.track_id.is_nil() ? EntityId::generate()
                                      : command.track_id,
      .width = command.width,
      .z_order = z_order,
      .header = command.header,
      .visible = command.visible,
  };
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(track)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const RemoveTrackCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  if (context.presentation_index.track(command.track_id) == nullptr) {
    return entity_missing(command.track_id);
  }
  // Cascade: the track, its scales and every layer placed in it leave
  // atomically, so no dangling reference can survive.
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(RemoveEntity{.id = command.track_id});
  for (const auto *scale :
       context.presentation_index.scales_of_track(command.track_id)) {
    patch.edits.emplace_back(RemoveEntity{.id = scale->id});
  }
  for (const auto layer_id :
       context.presentation_index.all_layers_of_track(command.track_id)) {
    patch.edits.emplace_back(RemoveEntity{.id = layer_id});
  }
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const ReorderTracksCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto &tracks = context.presentation->tracks();
  if (command.ordered_track_ids.size() != tracks.size()) {
    return order_incomplete(command.document_id);
  }
  auto provided = command.ordered_track_ids;
  std::sort(provided.begin(), provided.end());
  auto existing = std::vector<EntityId>{};
  existing.reserve(tracks.size());
  for (const auto &track : tracks) {
    existing.push_back(track.id);
  }
  std::sort(existing.begin(), existing.end());
  if (provided != existing) {
    return order_incomplete(command.document_id);
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  for (std::size_t position = 0;
       position < command.ordered_track_ids.size(); ++position) {
    auto updated = *context.presentation_index.track(
        command.ordered_track_ids[position]);
    updated.z_order = static_cast<std::int32_t>(position);
    patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  }
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const ResizeTrackCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *track = context.presentation_index.track(command.track_id);
  if (track == nullptr) {
    return entity_missing(command.track_id);
  }
  if (!std::isfinite(command.width.value) || command.width.value <= 0.0) {
    return binding_invalid(command.track_id);
  }
  auto updated = *track;
  updated.width = command.width;
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const SetTrackHeaderCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *track = context.presentation_index.track(command.track_id);
  if (track == nullptr) {
    return entity_missing(command.track_id);
  }
  auto updated = *track;
  updated.header = command.header;
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const SetTrackVisibilityCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *track = context.presentation_index.track(command.track_id);
  if (track == nullptr) {
    return entity_missing(command.track_id);
  }
  auto updated = *track;
  updated.visible = command.visible;
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const BindCurveToTrackCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *curve = context.document_index.curve(command.curve_id);
  if (curve == nullptr) {
    return entity_missing(command.curve_id);
  }
  if (context.presentation_index.track(command.track_id) == nullptr) {
    return entity_missing(command.track_id);
  }
  if (!command.layer_id.is_nil() &&
      context.presentation_index.curve_layer(command.layer_id) != nullptr) {
    return duplicate_id(command.layer_id);
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  const auto resolved =
      resolve_bind_scale(context, command.track_id, *curve, command.scale_id,
                         command.auto_range, patch);
  if (resolved.error.has_value()) {
    return *resolved.error;
  }
  CurveLayerSpec layer{
      .id = command.layer_id.is_nil() ? EntityId::generate()
                                      : command.layer_id,
      .track_id = command.track_id,
      .curve_id = command.curve_id,
      .scale_id = resolved_scale_id(resolved),
      .color = command.color,
      .line_width = command.line_width,
      .z_order = command.z_order.has_value()
                     ? *command.z_order
                     : top_of_track(context.presentation_index,
                                    command.track_id),
      .visible = true,
      .qc_display = {},
  };
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(layer)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const UnbindCurveFromTrackCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  if (context.presentation_index.curve_layer(command.layer_id) == nullptr) {
    return entity_missing(command.layer_id);
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(RemoveEntity{.id = command.layer_id});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const MoveCurveLayerCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *layer = context.presentation_index.curve_layer(command.layer_id);
  if (layer == nullptr) {
    return entity_missing(command.layer_id);
  }
  if (context.presentation_index.track(command.target_track_id) == nullptr) {
    return entity_missing(command.target_track_id);
  }
  const auto *curve = context.document_index.curve(layer->curve_id);
  if (curve == nullptr) {
    return entity_missing(layer->curve_id);
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  const auto resolved = resolve_bind_scale(
      context, command.target_track_id, *curve, command.target_scale_id,
      true, patch);
  if (resolved.error.has_value()) {
    return *resolved.error;
  }
  // Moving keeps the layer identity (LOD caches and undo stay continuous);
  // only the track/scale binding and z-order change. Raw curve and axis
  // buffers are untouched by construction — the patch rewrites presentation
  // entities only.
  auto moved = *layer;
  moved.track_id = command.target_track_id;
  moved.scale_id = resolved_scale_id(resolved);
  moved.z_order =
      top_of_track(context.presentation_index, command.target_track_id);
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(moved)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const DuplicateCurveLayerCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *layer = context.presentation_index.curve_layer(command.layer_id);
  if (layer == nullptr) {
    return entity_missing(command.layer_id);
  }
  if (!command.new_layer_id.is_nil() &&
      context.presentation_index.curve_layer(command.new_layer_id) !=
          nullptr) {
    return duplicate_id(command.new_layer_id);
  }
  auto copy = *layer;
  copy.id = command.new_layer_id.is_nil() ? EntityId::generate()
                                          : command.new_layer_id;
  copy.z_order = top_of_track(context.presentation_index, layer->track_id);
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(copy)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const ReorderCurveLayersCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  if (context.presentation_index.track(command.track_id) == nullptr) {
    return entity_missing(command.track_id);
  }
  const auto layers =
      context.presentation_index.curve_layers_of_track(command.track_id);
  if (command.ordered_layer_ids.size() != layers.size()) {
    return order_incomplete(command.document_id);
  }
  auto provided = command.ordered_layer_ids;
  std::sort(provided.begin(), provided.end());
  auto existing = std::vector<EntityId>{};
  existing.reserve(layers.size());
  for (const auto *layer : layers) {
    existing.push_back(layer->id);
  }
  std::sort(existing.begin(), existing.end());
  if (provided != existing) {
    return order_incomplete(command.document_id);
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  for (std::size_t position = 0;
       position < command.ordered_layer_ids.size(); ++position) {
    auto updated = *context.presentation_index.curve_layer(
        command.ordered_layer_ids[position]);
    updated.z_order = static_cast<std::int32_t>(position);
    patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  }
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const SetCurveLayerVisibilityCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *layer = context.presentation_index.curve_layer(command.layer_id);
  if (layer == nullptr) {
    return entity_missing(command.layer_id);
  }
  auto updated = *layer;
  updated.visible = command.visible;
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const SetCurveLayerStyleCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *layer = context.presentation_index.curve_layer(command.layer_id);
  if (layer == nullptr) {
    return entity_missing(command.layer_id);
  }
  auto updated = *layer;
  if (command.color.has_value()) {
    updated.color = *command.color;
  }
  if (command.line_width.has_value()) {
    if (!std::isfinite(command.line_width->value) ||
        command.line_width->value <= 0.0) {
      return binding_invalid(command.layer_id);
    }
    updated.line_width = *command.line_width;
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  patch.edits.emplace_back(UpsertEntity{.entity = std::move(updated)});
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const SetTrackScaleCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *scale = context.presentation_index.scale(command.scale_id);
  if (scale == nullptr) {
    return entity_missing(command.scale_id);
  }
  // A unit change must keep every bound curve compatible — checked here for
  // a precise error; preflight re-verifies atomically.
  if (command.unit.has_value() && *command.unit != scale->unit) {
    for (const auto *layer :
         context.presentation_index.curve_layers_of_track(scale->track_id)) {
      if (layer->scale_id != scale->id) {
        continue;
      }
      const auto *curve = context.document_index.curve(layer->curve_id);
      if (curve != nullptr && curve->unit != *command.unit) {
        return binding_invalid(command.scale_id);
      }
    }
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  if (const auto failure =
          upsert_scale_edit(*scale, command.mode, command.minimum,
                            command.maximum, command.direction, command.unit,
                            patch);
      failure.has_value()) {
    return *failure;
  }
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

Result<CommandReceipt>
WellLogSession::execute(const AutoRangeTrackScaleCommand &command) {
  BindingContext context;
  if (const auto failure =
          resolve_context(documents_view(), presentations_view(),
                          command.document_id, context);
      failure.has_value()) {
    return *failure;
  }
  const auto *scale = context.presentation_index.scale(command.scale_id);
  if (scale == nullptr) {
    return entity_missing(command.scale_id);
  }
  // Fit to the finite extent of every curve bound through this scale. An
  // explicit user/host operation — never a per-frame adjustment. A log scale
  // fits the positive extent (log auto-range is rejected when no bound
  // sample is positive).
  auto minimum = std::numeric_limits<double>::infinity();
  auto maximum = -std::numeric_limits<double>::infinity();
  auto positive_minimum = std::numeric_limits<double>::infinity();
  auto positive_maximum = 0.0;
  auto bound = false;
  for (const auto *layer :
       context.presentation_index.curve_layers_of_track(scale->track_id)) {
    if (layer->scale_id != scale->id) {
      continue;
    }
    const auto *curve = context.document_index.curve(layer->curve_id);
    if (curve == nullptr) {
      continue;
    }
    const auto length = curve->values.length();
    for (std::uint64_t index = 0; index < length; ++index) {
      const auto value = curve->values.value_as_double(index);
      if (!value.has_value() || !std::isfinite(*value)) {
        continue;
      }
      bound = true;
      minimum = std::min(minimum, *value);
      maximum = std::max(maximum, *value);
      if (*value > 0.0) {
        positive_minimum = std::min(positive_minimum, *value);
        positive_maximum = std::max(positive_maximum, *value);
      }
    }
  }
  if (!bound) {
    return scale_range_invalid(command.scale_id);
  }
  auto next_minimum = minimum;
  auto next_maximum = maximum;
  if (scale->mode == ScaleMode::logarithmic) {
    if (!(positive_maximum > 0.0)) {
      return scale_range_invalid(command.scale_id);
    }
    next_minimum = positive_minimum;
    next_maximum = positive_maximum;
  }
  if (next_minimum == next_maximum) {
    next_maximum =
        scale->mode == ScaleMode::logarithmic && next_minimum > 0.0
            ? next_minimum * 10.0
            : next_minimum + 1.0;
  }
  DocumentPatch patch;
  patch.base_revision = context.document->revision();
  if (const auto failure =
          upsert_scale_edit(*scale, std::nullopt, next_minimum, next_maximum,
                            std::nullopt, std::nullopt, patch);
      failure.has_value()) {
    return *failure;
  }
  return execute(ApplyPatchCommand{.document_id = command.document_id,
                                   .patch = std::move(patch)});
}

} // namespace welllog
