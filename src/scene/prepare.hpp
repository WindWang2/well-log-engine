#pragma once

#include <optional>
#include <stop_token>
#include <unordered_map>

#include <welllog/scene/curve_lod.hpp>
#include <welllog/scene/image_pyramid.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog::detail {

class WELLLOG_SCENE_API ScenePreparer {
public:
  using CurveLodMap =
      std::unordered_map<EntityId, CurveLodPyramid, EntityIdHash>;
  using ImagePyramidMap =
      std::unordered_map<EntityId, ImagePyramid, EntityIdHash>;

  // Validates every static document/presentation relationship ScenePreparer
  // depends on, without shaping text, walking curve samples, or producing a
  // PreparedScene. Command paths use this to reject invalid declarative edits
  // atomically before scheduling the asynchronous geometry work.
  [[nodiscard]] static std::optional<Error>
  preflight(const WellLogDocument &document,
            const ScenePresentation &presentation) noexcept;
  [[nodiscard]] static Result<PreparedScene>
  prepare(const WellLogDocument &document,
          const ScenePresentation &presentation,
          TextEngine *text_engine = nullptr) noexcept;
  [[nodiscard]] static Result<PreparedScene>
  prepare(const WellLogDocument &document,
          const ScenePresentation &presentation, const CurveLodMap &curve_lods,
          const CurveLodQuery &query, std::stop_token stop_token = {},
          TextEngine *text_engine = nullptr) noexcept;
  [[nodiscard]] static Result<PreparedScene>
  prepare(const WellLogDocument &document,
          const ScenePresentation &presentation, const CurveLodMap &curve_lods,
          const CurveLodQuery &query, const ImagePyramidMap &image_pyramids,
          const ImagePyramidQuery &image_query, std::stop_token stop_token = {},
          TextEngine *text_engine = nullptr) noexcept;

private:
  [[nodiscard]] static Result<PreparedScene>
  prepare_impl(const WellLogDocument &document,
               const ScenePresentation &presentation,
               const CurveLodMap *curve_lods, const CurveLodQuery *query,
               const ImagePyramidMap *image_pyramids,
               const ImagePyramidQuery *image_query,
               std::stop_token stop_token, TextEngine *text_engine) noexcept;
};

} // namespace welllog::detail
