#include <welllog/core/document.hpp>
#include <welllog/session/session.hpp>

#include <memory>
#include <vector>

int main() {
  using namespace welllog;

  const auto document_id =
      EntityId::parse("10101010-1010-4010-8010-101010101010");
  const auto axis_id = EntityId::parse("20202020-2020-4020-8020-202020202020");
  const auto curve_id = EntityId::parse("30303030-3030-4030-8030-303030303030");
  if (!document_id || !axis_id || !curve_id) {
    return 1;
  }

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{100.0, 100.5});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{1.0F, 2.0F});

  WellLogDocumentBuilder builder(*document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = *axis_id,
      .coordinates = BufferView::from_vector(depths),
      .unit = "m",
  });
  builder.add_curve(Curve{
      .id = *curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = *axis_id,
      .values = BufferView::from_vector(values),
  });

  WellLogSession session;
  const auto result = session.execute(SetDocumentCommand{builder.build()});
  return result && result.value().document_id == *document_id ? 0 : 2;
}
