#include <welllog/scene/inspect.hpp>

#include <welllog/core/document_index.hpp>
#include <welllog/scene/presentation_index.hpp>

namespace welllog {

std::optional<CurvePickInfo>
resolve_curve_pick(const WellLogDocument &document,
                   const ScenePresentation &presentation,
                   const CurvePick &pick) noexcept {
  const PresentationBindingIndex presentation_index{presentation};
  const DocumentBindingIndex document_index{document};

  const auto *layer = presentation_index.curve_layer(pick.layer_id);
  if (layer == nullptr) {
    return std::nullopt;
  }
  const auto *curve = document_index.curve(layer->curve_id);
  if (curve == nullptr) {
    return std::nullopt;
  }
  const auto *axis = document_index.axis(curve->sampling_axis_id);
  if (axis == nullptr) {
    return std::nullopt;
  }
  CurvePickInfo info;
  info.document_id = pick.document_id;
  info.track_id = layer->track_id;
  info.layer_id = layer->id;
  info.curve_id = curve->id;
  info.scale_id = layer->scale_id;
  info.sampling_axis_id = axis->id;
  info.mnemonic = curve->mnemonic;
  info.display_name = curve->display_name;
  info.unit = curve->unit;
  info.sample_index = pick.sample_index;
  info.reference_depth = pick.reference_depth;
  info.display_depth = pick.display_depth;
  info.raw_value = pick.value;
  info.qc_state = qc_state_at(document, *curve, pick.sample_index);
  if (const auto *scale = presentation_index.scale(layer->scale_id);
      scale != nullptr) {
    info.scale_id = scale->id;
    info.scale_unit = scale->unit;
    info.scale_mode = scale->mode;
    info.scale_minimum = scale->minimum;
    info.scale_maximum = scale->maximum;
    info.scale_direction = scale->direction;
  }
  if (curve->derived.has_value()) {
    info.derived = true;
    info.derived_freshness =
        compute_derived_freshness(document, *curve);
    info.algorithm_id = curve->derived->algorithm_id;
    info.algorithm_version = curve->derived->algorithm_version;
  }
  return info;
}

} // namespace welllog
