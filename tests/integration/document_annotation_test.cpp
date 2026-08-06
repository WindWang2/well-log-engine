#include <welllog/core/document.hpp>
#include <welllog/core/utf8.hpp>
#include <welllog/session/session.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  // _Exit, not std::exit: avoid CRT/DLL teardown while LOD/frame worker
  // jthreads are still mid-flight (Windows loader-lock deadlock, #241).
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return parsed.value();
}

struct DocumentFixture {
  EntityId document_id{id("dddddddd-dddd-4ddd-8ddd-dddddddddddd")};
  EntityId axis_id{id("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")};
  EntityId curve_id{id("ffffffff-ffff-4fff-8fff-ffffffffffff")};
  EntityId interval_id{id("11111111-aaaa-4aaa-8aaa-aaaaaaaaaaaa")};
  EntityId marker_id{id("22222222-bbbb-4bbb-8bbb-bbbbbbbbbbbb")};
  EntityId symbol_id{id("33333333-cccc-4ccc-8ccc-cccccccccccc")};
  EntityId annotation_id{id("44444444-dddd-4ddd-8ddd-dddddddddddd")};
  EntityId track_id{id("55555555-eeee-4eee-8eee-eeeeeeeeeeee")};
};

WellLogDocumentBuilder base_builder(const DocumentFixture &fixture) {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0});
  WellLogDocumentBuilder builder(fixture.document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = fixture.axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = fixture.curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = fixture.axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  return builder;
}

Interval valid_interval(const DocumentFixture &fixture) {
  return Interval{
      .id = fixture.interval_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1001.5,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = {},
      .fill_color = RgbaColor{200, 180, 120, 255},
      .label = "砂岩 Sandstone",
  };
}

void utf8_validation_is_strict() {
  require(is_valid_utf8("plain ASCII"), "ASCII must be valid UTF-8");
  require(is_valid_utf8("砂岩 γρ μm³"),
          "CJK, Greek and superscripts must be valid UTF-8");
  require(is_valid_utf8(""), "empty text must be valid UTF-8");
  require(!is_valid_utf8("\xC0\x80"),
          "overlong encodings must be rejected");
  require(!is_valid_utf8("\xED\xA0\x80"),
          "UTF-16 surrogates must be rejected");
  require(!is_valid_utf8("\xF4\x90\x80\x80"),
          "code points above U+10FFFF must be rejected");
  require(!is_valid_utf8("\xE2\x82"), "truncated sequences must be rejected");
  require(!is_valid_utf8("\x80"), "lone continuations must be rejected");
  require(!is_valid_utf8("\xC2"), "truncated two-byte lead must be rejected");
  require(!is_valid_utf8("\xF5\x80\x80\x80"),
          "out-of-range lead bytes must be rejected");
  const std::string embedded_nul{"ab\0cd", 5};
  require(is_valid_utf8(embedded_nul), "embedded NUL is valid UTF-8");
}

void session_accepts_intervals_markers_symbols_and_annotations() {
  const DocumentFixture fixture;
  auto builder = base_builder(fixture);
  builder.add_interval(valid_interval(fixture));
  builder.add_marker(Marker{
      .id = fixture.marker_id,
      .reference_depth = 1001.5,
      .semantic = MarkerSemantic::formation_top,
      .label = "顶面 Top A",
  });
  builder.add_symbol(SymbolOccurrence{
      .id = fixture.symbol_id,
      .reference_depth = 1000.75,
      .track_fraction = 0.25,
      .kind = SymbolKind::diamond,
      .label = "core",
  });
  builder.add_annotation(TextAnnotation{
      .id = fixture.annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1000.5,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "含油砂岩 oil-bearing",
      .language = "zh-Hans",
      .orientation = TextOrientation::vertical,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{3.5},
  });

  WellLogSession session;
  const auto receipt = session.execute(SetDocumentCommand{builder.build()});
  require(receipt.has_value(),
          "document with intervals, markers, symbols and annotations must be "
          "accepted");

  const auto stored = session.document(fixture.document_id);
  require(stored != nullptr, "submitted document must be queryable");
  require(stored->intervals().size() == 1, "interval must round-trip");
  require(stored->intervals().front().label == "砂岩 Sandstone",
          "interval label must round-trip unmodified");
  require(stored->markers().size() == 1, "marker must round-trip");
  require(stored->markers().front().semantic == MarkerSemantic::formation_top,
          "marker semantic must round-trip");
  require(stored->symbols().size() == 1, "symbol must round-trip");
  require(stored->symbols().front().kind == SymbolKind::diamond,
          "symbol kind must round-trip");
  require(stored->annotations().size() == 1, "annotation must round-trip");
  require(stored->annotations().front().orientation ==
              TextOrientation::vertical,
          "annotation orientation must round-trip");
  require(stored->annotations().front().font_size == Millimetres{3.5},
          "annotation font size must round-trip");
}

void session_rejects_reversed_and_degenerate_intervals() {
  const DocumentFixture fixture;

  auto reversed = valid_interval(fixture);
  reversed.top_reference_depth = 1002.0;
  reversed.bottom_reference_depth = 1000.0;
  WellLogSession reversed_session;
  const auto reversed_result = reversed_session.execute(SetDocumentCommand{
      base_builder(fixture).add_interval(reversed).build()});
  require(!reversed_result.has_value(), "reversed interval must be rejected");
  require(reversed_result.error().code == ErrorCode::invalid_document,
          "reversed interval must use the document error code");
  require(reversed_result.error().message ==
              MessageKey::interval_depth_order_invalid,
          "reversed interval must use the depth-order message key");
  require(reversed_result.error().entity_id.has_value() &&
              reversed_result.error().entity_id.value() == fixture.interval_id,
          "reversed interval error must identify the offending entity");

  auto degenerate = valid_interval(fixture);
  degenerate.top_reference_depth = 1001.0;
  degenerate.bottom_reference_depth = 1001.0;
  WellLogSession degenerate_session;
  const auto degenerate_result = degenerate_session.execute(SetDocumentCommand{
      base_builder(fixture).add_interval(degenerate).build()});
  require(!degenerate_result.has_value(),
          "zero-thickness interval must be rejected; it is a marker");

  auto nan_depth = valid_interval(fixture);
  nan_depth.bottom_reference_depth =
      std::numeric_limits<double>::quiet_NaN();
  WellLogSession nan_session;
  const auto nan_result = nan_session.execute(SetDocumentCommand{
      base_builder(fixture).add_interval(nan_depth).build()});
  require(!nan_result.has_value(), "non-finite interval depths must be rejected");
}

void session_rejects_invalid_utf8_labels_and_text() {
  const DocumentFixture fixture;

  auto bad_interval = valid_interval(fixture);
  bad_interval.label = "sand\xC0\x80stone";
  WellLogSession interval_session;
  const auto interval_result = interval_session.execute(SetDocumentCommand{
      base_builder(fixture).add_interval(bad_interval).build()});
  require(!interval_result.has_value(),
          "invalid UTF-8 interval label must be rejected");
  require(interval_result.error().message == MessageKey::text_encoding_invalid,
          "invalid label must use the text-encoding message key");

  Marker bad_marker{
      .id = fixture.marker_id,
      .reference_depth = 1001.0,
      .semantic = MarkerSemantic::fault,
      .label = "\xED\xA0\x80",
  };
  WellLogSession marker_session;
  const auto marker_result = marker_session.execute(SetDocumentCommand{
      base_builder(fixture).add_marker(bad_marker).build()});
  require(!marker_result.has_value(),
          "invalid UTF-8 marker label must be rejected");

  TextAnnotation bad_annotation{
      .id = fixture.annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1000.5,
      .track_fraction = 0.5,
      .track_id = {},
      .text = "bad \xF5\x80\x80\x80 text",
      .language = {},
      .orientation = TextOrientation::horizontal,
      .font_size = Millimetres{3.0},
  };
  WellLogSession annotation_session;
  const auto annotation_result = annotation_session.execute(
      SetDocumentCommand{base_builder(fixture)
                             .add_annotation(bad_annotation)
                             .build()});
  require(!annotation_result.has_value(),
          "invalid UTF-8 annotation text must be rejected");
}

void session_rejects_invalid_annotation_anchors() {
  const DocumentFixture fixture;

  TextAnnotation out_of_range_fraction{
      .id = fixture.annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1000.5,
      .track_fraction = 1.5,
      .track_id = {},
      .text = "note",
      .language = {},
      .orientation = TextOrientation::horizontal,
      .font_size = Millimetres{3.0},
  };
  WellLogSession fraction_session;
  const auto fraction_result = fraction_session.execute(SetDocumentCommand{
      base_builder(fixture).add_annotation(out_of_range_fraction).build()});
  require(!fraction_result.has_value(),
          "track fraction outside [0, 1] must be rejected");
  require(fraction_result.error().message ==
              MessageKey::annotation_anchor_invalid,
          "invalid anchor must use the anchor message key");

  TextAnnotation nil_track{
      .id = fixture.annotation_id,
      .anchor = AnnotationAnchor::track,
      .track_id = {},
      .depth_fraction = 0.5,
      .horizontal_fraction = 0.5,
      .text = "note",
      .language = {},
      .orientation = TextOrientation::horizontal,
      .font_size = Millimetres{3.0},
  };
  WellLogSession track_session;
  const auto track_result = track_session.execute(SetDocumentCommand{
      base_builder(fixture).add_annotation(nil_track).build()});
  require(!track_result.has_value(),
          "track anchor without a track identity must be rejected");

  TextAnnotation zero_font{
      .id = fixture.annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1000.5,
      .track_fraction = 0.5,
      .track_id = {},
      .text = "note",
      .language = {},
      .orientation = TextOrientation::horizontal,
      .font_size = Millimetres{0.0},
  };
  WellLogSession font_session;
  const auto font_result = font_session.execute(SetDocumentCommand{
      base_builder(fixture).add_annotation(zero_font).build()});
  require(!font_result.has_value(), "non-positive font size must be rejected");

  TextAnnotation empty_text{
      .id = fixture.annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1000.5,
      .track_fraction = 0.5,
      .track_id = {},
      .text = "",
      .language = {},
      .orientation = TextOrientation::horizontal,
      .font_size = Millimetres{3.0},
  };
  WellLogSession empty_session;
  const auto empty_result = empty_session.execute(SetDocumentCommand{
      base_builder(fixture).add_annotation(empty_text).build()});
  require(!empty_result.has_value(), "empty annotation text must be rejected");
}

void session_rejects_duplicate_annotation_entity_ids() {
  const DocumentFixture fixture;
  auto builder = base_builder(fixture);
  builder.add_marker(Marker{
      .id = fixture.curve_id,
      .reference_depth = 1001.0,
      .semantic = MarkerSemantic::custom,
      .label = "clash",
  });
  WellLogSession session;
  const auto result = session.execute(SetDocumentCommand{builder.build()});
  require(!result.has_value(),
          "entity identities must be unique across all document collections");
  require(result.error().code == ErrorCode::duplicate_entity_id,
          "identity clash must return the duplicate-entity code");
}

} // namespace

int main() {
  utf8_validation_is_strict();
  session_accepts_intervals_markers_symbols_and_annotations();
  session_rejects_reversed_and_degenerate_intervals();
  session_rejects_invalid_utf8_labels_and_text();
  session_rejects_invalid_annotation_anchors();
  session_rejects_duplicate_annotation_entity_ids();
  std::cout << "PASS: document interval and annotation model\n";
  return EXIT_SUCCESS;
}
