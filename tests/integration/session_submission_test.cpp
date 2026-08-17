#include <welllog/core/document.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/scene/text_engine.hpp>
#include <welllog/session/session.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

WellLogDocument
single_curve_document(BufferView coordinates, BufferView values,
                      AxisDirection direction = AxisDirection::increasing,
                      NullBitmapView nulls = {}) {
  const auto document_id = id("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
  const auto axis_id = id("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
  const auto curve_id = id("ffffffff-ffff-4fff-8fff-ffffffffffff");
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = std::move(coordinates),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = direction,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = std::move(values),
      .nulls = std::move(nulls),
  });
  return builder.build();
}

void session_accepts_an_increasing_axis_curve() {
  const auto document_id = id("11111111-1111-4111-8111-111111111111");
  const auto axis_id = id("22222222-2222-4222-8222-222222222222");
  const auto curve_id = id("33333333-3333-4333-8333-333333333333");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1000.5, 1000.5, 1001.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});

  WellLogDocumentBuilder builder(document_id, DocumentRevision{7});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths,
                                             BufferSourceReference{
                                                 .uri = "memory://well-a/depth",
                                                 .checksum = "sha256:depths",
                                             }),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values,
                                        BufferSourceReference{
                                            .uri = "memory://well-a/gr",
                                            .checksum = "sha256:gr",
                                        }),
      .nulls = {},
  });

  WellLogSession session;
  auto receipt = session.execute(SetDocumentCommand{builder.build()});

  require(receipt.has_value(), "valid document submission must succeed");
  require(receipt.value().document_id == document_id,
          "receipt must report the submitted document identity");
  require(receipt.value().document_revision == DocumentRevision{7},
          "receipt must report the submitted revision");
  require(!receipt.value().asynchronous_preparation_started,
          "document-only submission must report no asynchronous preparation");
  require(!receipt.value().diagnostic_id.has_value(),
          "clean document submission must report no diagnostics");

  const auto events = session.events();
  require(events.size() == 1, "successful submission must publish one event");
  require(events.front().kind == ViewEventKind::documents_changed,
          "submission event must describe the document change");
  require(events.front().document_id == document_id,
          "event must report the submitted document identity");
  require(events.front().document_revision == DocumentRevision{7},
          "event must report the submitted revision");
}

void session_keeps_decreasing_curve_buffers_alive_without_copying() {
  const auto document_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
  const auto axis_id = id("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
  const auto curve_id = id("cccccccc-cccc-4ccc-8ccc-cccccccccccc");

  std::weak_ptr<const std::vector<double>> values_lifetime;
  const double *original_values_address = nullptr;
  std::vector<double> original_values;

  {
    WellLogSession session;
    {
      auto depths = std::make_shared<const std::vector<double>>(
          std::initializer_list<double>{2000.0, 1999.5, 1999.5, 1999.0});
      auto values = std::make_shared<const std::vector<double>>(
          std::initializer_list<double>{11.0, 22.0, 33.0, 44.0});
      auto null_bytes = std::make_shared<const std::vector<std::uint8_t>>(
          std::initializer_list<std::uint8_t>{0b00000100});
      values_lifetime = values;
      original_values_address = values->data();
      original_values.assign(values->begin(), values->end());
      const auto value_view = BufferView::from_vector(values);

      WellLogDocumentBuilder builder(document_id, DocumentRevision{3});
      builder.add_sampling_axis(SamplingAxis{
          .id = axis_id,
          .coordinates = BufferView::from_vector(depths),
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .direction = AxisDirection::decreasing,
      });
      builder.add_curve(Curve{
          .id = curve_id,
          .mnemonic = "DEN",
          .display_name = "Density",
          .unit = "g/cm3",
          .sampling_axis_id = axis_id,
          .values = value_view,
          .nulls = NullBitmapView::from_raw(null_bytes->data(), 4,
                                            null_bytes->size(),
                                            SharedOwner{null_bytes}),
      });

      auto result = session.execute(SetDocumentCommand{builder.build()});
      require(result.has_value(),
              "decreasing axis with duplicates and nulls must succeed");
      require(result.value().diagnostic_id.has_value(),
              "receipt must report the diagnostic identity");
      require(values->data() ==
                  reinterpret_cast<const double *>(value_view.data()),
              "vector submission must report the original buffer address");
      require(value_view.access_mode() == BufferAccessMode::zero_copy,
              "vector submission must report zero-copy access");
    }

    require(!values_lifetime.expired(),
            "session must pin the submitted buffer owner");
    const auto stored = session.document(document_id);
    require(stored != nullptr, "submitted document must be queryable");
    require(stored->curves().front().values.as_single().data() ==
                reinterpret_cast<const std::byte *>(original_values_address),
            "session must retain the original zero-copy buffer address");
    require(std::vector<double>(original_values_address,
                                original_values_address +
                                    original_values.size()) == original_values,
            "session must not modify submitted values");

    const auto diagnostics = session.diagnostics();
    require(diagnostics.size() == 1,
            "one curve containing missing data must publish one diagnostic");
    require(diagnostics.front().code == DiagnosticCode::missing_samples,
            "diagnostic must identify missing samples");
    require(diagnostics.front().entity_id == curve_id,
            "diagnostic must identify the affected curve");
    require(diagnostics.front().occurrence_count == 1,
            "diagnostic must aggregate the missing sample count");

    const auto events = session.events();
    require(events.size() == 2,
            "document and diagnostic changes must both publish events");
    require(events.back().kind == ViewEventKind::diagnostic_published,
            "the second event must report the diagnostic");
  }

  require(values_lifetime.expired(),
          "buffer owner must be released after the session and snapshots");
}

void session_rejects_invalid_documents_without_changing_state() {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 3.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0});

  const auto missing_owner_values = BufferView::from_raw(
      values->data(), values->size(), sizeof(double), ScalarType::float64,
      values->size() * sizeof(double), SharedOwner{});
  WellLogSession missing_owner_session;
  const auto missing_owner =
      missing_owner_session.execute(SetDocumentCommand{single_curve_document(
          BufferView::from_vector(depths), missing_owner_values)});
  require(!missing_owner.has_value(), "ownerless buffer must be rejected");
  require(missing_owner.error().code == ErrorCode::missing_owner,
          "ownerless buffer must return the stable missing-owner code");
  require(missing_owner_session.events().empty(),
          "rejected command must not publish state-change events");

  const auto overflowing_axis = BufferView::from_raw(
      depths->data(), UINT64_MAX, UINT64_MAX, ScalarType::float64, UINT64_MAX,
      SharedOwner{depths});
  WellLogSession overflow_session;
  const auto overflow =
      overflow_session.execute(SetDocumentCommand{single_curve_document(
          overflowing_axis, BufferView::from_vector(values))});
  require(!overflow.has_value(), "overflowing buffer extent must be rejected");
  require(overflow.error().code == ErrorCode::arithmetic_overflow,
          "overflowing extent must return the stable overflow code");

  auto unordered_depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 3.0, 2.0});
  WellLogSession unordered_session;
  const auto unordered = unordered_session.execute(SetDocumentCommand{
      single_curve_document(BufferView::from_vector(unordered_depths),
                            BufferView::from_vector(values))});
  require(!unordered.has_value(), "locally unordered axis must be rejected");
  require(unordered.error().code == ErrorCode::invalid_sampling_axis,
          "unordered axis must return the stable sampling-axis code");

  auto short_values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0});
  WellLogSession mismatch_session;
  const auto mismatch = mismatch_session.execute(SetDocumentCommand{
      single_curve_document(BufferView::from_vector(depths),
                            BufferView::from_vector(short_values))});
  require(!mismatch.has_value(), "structural length mismatch must be rejected");
  require(mismatch.error().code == ErrorCode::length_mismatch,
          "length mismatch must return the stable structural error code");
}

// Holds the session text-engine lock for longer than drain_workers' 2s
// bound so ~Impl joins the pool while a frame task still references the
// mutex. Issue #610: mutex must outlive TaskExecutor.
class HoldingTextEngine final : public TextEngine {
public:
  explicit HoldingTextEngine(std::chrono::milliseconds hold) : hold_(hold) {}

  Result<ShapedRun> shape(const TextShapeRequest &) noexcept override {
    entered_.store(true, std::memory_order_release);
    std::this_thread::sleep_for(hold_);
    return ShapedRun{};
  }
  Result<GlyphOutline> glyph_outline(std::uint32_t, std::uint32_t) noexcept
      override {
    return GlyphOutline{};
  }
  std::string font_fingerprint(std::uint32_t) const override { return "hold"; }
  std::string font_family_name(std::uint32_t) const override { return "hold"; }

  [[nodiscard]] bool entered() const noexcept {
    return entered_.load(std::memory_order_acquire);
  }

private:
  std::chrono::milliseconds hold_;
  std::atomic<bool> entered_{false};
};

// Issue #610: destroying a session while a frame task is inside shape()
// (holding text_engine_mutex) must join the pool before the mutex dies.
// Declaration order is the lifetime contract: members are destroyed in reverse
// order, so the mutex must appear before TaskExecutor in Impl.
void text_engine_mutex_is_declared_before_task_executor() {
#ifndef WELLLOG_SESSION_SOURCE
  fail("WELLLOG_SESSION_SOURCE must point at src/session/session.cpp");
#else
  std::ifstream in{WELLLOG_SESSION_SOURCE};
  require(static_cast<bool>(in), "session.cpp must be readable");
  const std::string text{std::istreambuf_iterator<char>{in},
                         std::istreambuf_iterator<char>{}};
  auto mutex_pos = text.find("std::shared_ptr<std::mutex> text_engine_mutex{");
  if (mutex_pos == std::string::npos) {
    mutex_pos = text.find("std::mutex text_engine_mutex;");
  }
  const auto executor_pos = text.find("TaskExecutor task_executor{2}");
  require(mutex_pos != std::string::npos,
          "Impl must declare text_engine_mutex");
  require(executor_pos != std::string::npos,
          "Impl must declare TaskExecutor task_executor");
  require(mutex_pos < executor_pos,
          "text_engine_mutex must be declared before task_executor so the "
          "mutex outlives pool join (#610)");
#endif
}

void session_teardown_joins_before_text_mutex_dies() {
  const auto document_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa1");
  const auto axis_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa2");
  const auto curve_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa3");
  const auto track_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa4");
  const auto text_layer_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa5");
  const auto annotation_id = id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa6");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0});

  WellLogDocumentBuilder document_builder(document_id, DocumentRevision{1});
  document_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  document_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  document_builder.add_annotation(TextAnnotation{
      .id = annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1000.5,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "Hold",
      .language = "en",
      .orientation = TextOrientation::horizontal,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });

  auto engine = std::make_shared<HoldingTextEngine>(
      std::chrono::milliseconds{3500});
  {
    WellLogSession session(PerformanceBudgets{
        .maximum_cpu_derived_bytes = 4ULL * 1024ULL * 1024ULL,
        .asynchronous_sample_threshold = 1,
    });
    session.set_text_engine(engine);
    require(session.execute(SetDocumentCommand{document_builder.build()})
                .has_value(),
            "holding-engine document must be accepted");

    ScenePresentationBuilder presentation_builder(
        document_id,
        ReferenceDepthRange{
            .domain = DepthDomain::measured_depth,
            .unit = "m",
            .top = 1000.0,
            .bottom = 1001.0,
        },
        Millimetres{80.0}, "font-fixture-v1");
    presentation_builder.add_track(TrackSpec{
        .id = track_id,
        .width = Millimetres{30.0},
        .z_order = 0,
    });
    presentation_builder.add_text_layer(TextLayerSpec{
        .id = text_layer_id,
        .track_id = track_id,
        .z_order = 0,
        .color = RgbaColor{10, 10, 10, 255},
    });
    require(session.execute(SetPresentationCommand{presentation_builder.build()})
                .has_value(),
            "holding-engine presentation must be accepted");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline && !engine->entered()) {
      session.poll_async();
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    require(engine->entered(),
            "frame task must enter shape() while holding the text mutex");
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
}

void session_compares_integer_sampling_axes_without_precision_loss() {
  auto depths = std::make_shared<const std::vector<std::uint64_t>>(
      std::initializer_list<std::uint64_t>{9007199254740993ULL,
                                           9007199254740992ULL});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0});

  WellLogSession session;
  const auto result = session.execute(SetDocumentCommand{single_curve_document(
      BufferView::from_vector(depths), BufferView::from_vector(values))});

  require(!result.has_value(),
          "integer axis disorder must not be hidden by double conversion");
  require(result.error().code == ErrorCode::invalid_sampling_axis,
          "integer disorder must return the sampling-axis error code");
}

} // namespace

int main() {
  session_accepts_an_increasing_axis_curve();
  session_keeps_decreasing_curve_buffers_alive_without_copying();
  session_rejects_invalid_documents_without_changing_state();
  text_engine_mutex_is_declared_before_task_executor();
  session_teardown_joins_before_text_mutex_dies();
  session_compares_integer_sampling_axes_without_precision_loss();
  std::cout << "PASS: session submission behavior\n";
  return EXIT_SUCCESS;
}
