#include <welllog/export/svg.hpp>
#include <welllog/session/session.hpp>

#include <memory>
#include <string_view>
#include <vector>

int main() {
  using namespace welllog;

  const auto document_id =
      EntityId::parse("30101010-1010-4010-8010-101010101010");
  const auto axis_id = EntityId::parse("30202020-2020-4020-8020-202020202020");
  const auto curve_id = EntityId::parse("30303030-3030-4030-8030-303030303030");
  const auto track_id = EntityId::parse("30404040-4040-4040-8040-404040404040");
  const auto scale_id = EntityId::parse("30505050-5050-4050-8050-505050505050");
  const auto layer_id = EntityId::parse("30606060-6060-4060-8060-606060606060");
  if (!document_id || !axis_id || !curve_id || !track_id || !scale_id ||
      !layer_id) {
    return 1;
  }

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{100.0, 101.0});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{0.0F, 100.0F});
  WellLogDocumentBuilder document_builder(*document_id, DocumentRevision{1});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = *axis_id,
      .coordinates = BufferView::from_vector(depths),
      .unit = "m",
  });
  document_builder.add_curve(Curve{
      .id = *curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = *axis_id,
      .values = BufferView::from_vector(values),
  });

  WellLogSession session;
  if (!session.execute(SetDocumentCommand{document_builder.build()})) {
    return 2;
  }

  ScenePresentationBuilder presentation_builder(
      *document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 100.0,
          .bottom = 101.0,
      },
      Millimetres{50.0}, "headless-consumer-font");
  presentation_builder.add_track(TrackSpec{
      .id = *track_id,
      .width = Millimetres{20.0},
  });
  presentation_builder.add_scale(TrackScaleSpec{
      .id = *scale_id,
      .track_id = *track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = *layer_id,
      .track_id = *track_id,
      .curve_id = *curve_id,
      .scale_id = *scale_id,
      .color = RgbaColor{.red = 0xff, .alpha = 0xff},
      .line_width = Millimetres{0.2},
      .visible = true,
  });
  if (!session.execute(SetPresentationCommand{presentation_builder.build()})) {
    return 3;
  }

  const auto scene = session.prepared_scene(*document_id);
  if (scene == nullptr) {
    return 4;
  }
  const auto svg = SvgExporter::write(*scene);
  return svg && svg.value().text().starts_with(std::string_view{"<svg xmlns="})
             ? 0
             : 5;
}
