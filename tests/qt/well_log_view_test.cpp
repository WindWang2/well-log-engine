#include <welllog/qtwidgets/well_log_view.hpp>

#include <welllog/text/harfbuzz_text_engine.hpp>

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QTest>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace welllog;

class WellLogViewTest final : public QObject {
  Q_OBJECT

private slots:
  void native_view_embeds_and_reports_capabilities();
  void prepared_curve_renders_into_the_widget_fbo();
  void layered_scene_renders_intervals_patterns_symbols_and_text();
  void pointer_interaction_updates_session_and_semantic_picks();
  void pointer_pan_zoom_and_reset_use_session_commands();
  void widget_rebuild_restores_curve_from_session_cpu_state();
  void top_level_reparent_restores_curve_after_context_recreation();
  void unavailable_context_publishes_a_capability_report();
  void external_session_events_refresh_and_coalesce_qt_signals();
  void asynchronous_failures_keep_their_diagnostic_identity();
  void cross_thread_public_mutations_are_queued_to_gui();
};

struct PreparedViewFixture {
  EntityId document_id;
  EntityId curve_id;
  std::shared_ptr<WellLogSession> session;
};

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  Q_ASSERT(parsed.has_value());
  return *parsed;
}

// Fixture commands must succeed in every build type: a bare Q_ASSERT is
// compiled out under QT_NO_DEBUG and silently leaves the session without a
// document/presentation (Release-only cascade failures that Debug never
// sees). Surface the engine error instead.
template <typename T>
void require_command(const Result<T> &result, std::string_view what) {
  if (!result.has_value()) {
    const auto &error = result.error();
    qFatal("%s failed: code=%d message=%d", std::string{what}.c_str(),
           static_cast<int>(error.code), static_cast<int>(error.message));
  }
}

PreparedViewFixture
prepared_view_fixture(PerformanceBudgets budgets = PerformanceBudgets{}) {
  const auto document_id = id("60000000-0000-4000-8000-000000000001");
  const auto axis_id = id("60000000-0000-4000-8000-000000000002");
  const auto curve_id = id("60000000-0000-4000-8000-000000000003");
  const auto track_id = id("60000000-0000-4000-8000-000000000004");
  const auto scale_id = id("60000000-0000-4000-8000-000000000005");
  const auto layer_id = id("60000000-0000-4000-8000-000000000006");
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 50.0, 100.0});

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

  auto session = std::make_shared<WellLogSession>(budgets);
  require_command(session->execute(SetDocumentCommand{document_builder.build()}),
                  "fixture SetDocumentCommand");
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1100.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation_builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 0});
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = 0.0,
      .maximum = 100.0,
      .direction = ScaleDirection::left_to_right,
      .unit = "API",
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{
              .red = 0x12,
              .green = 0x34,
              .blue = 0x56,
              .alpha = 0x80,
          },
      .line_width = Millimetres{0.5},
      .z_order = 0,
      .visible = true,
  });
  require_command(
      session->execute(SetPresentationCommand{presentation_builder.build()}),
      "fixture SetPresentationCommand");
  return PreparedViewFixture{
      .document_id = document_id,
      .curve_id = curve_id,
      .session = std::move(session),
  };
}

void WellLogViewTest::native_view_embeds_and_reports_capabilities() {
  QWidget host;
  auto *layout = new QVBoxLayout(&host);
  auto *view = new WellLogView(&host);
  layout->addWidget(view);
  view->resize(320, 240);

  const auto requested = view->format();
  QCOMPARE(requested.renderableType(), QSurfaceFormat::OpenGL);
  QCOMPARE(requested.profile(), QSurfaceFormat::CoreProfile);
  QVERIFY((requested.version() >= std::pair{3, 3}));
  QVERIFY(requested.stencilBufferSize() >= 8);

  host.resize(320, 240);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().initialization_complete,
                           5000);

  const auto &report = view->capability_report();
  QVERIFY2(report.graphics_available, report.unavailable_reason.c_str());
  QVERIFY(report.core_profile);
  QVERIFY(report.open_gl_major > 3 ||
          (report.open_gl_major == 3 && report.open_gl_minor >= 3));
  QVERIFY(report.stencil_bits >= 8);
  QVERIFY(report.maximum_texture_size > 0);
  QVERIFY(report.maximum_combined_texture_units > 0);
  QVERIFY(report.maximum_vertex_attributes >= 2);
  QVERIFY(report.maximum_uniform_block_size > 0);
  QVERIFY(!report.persistent_mapping_enabled);
  QVERIFY(!report.active_upload_path.empty());
  QVERIFY(!report.vendor.empty());
  QVERIFY(!report.renderer.empty());
  QVERIFY(!report.open_gl_version.empty());
  QVERIFY(!report.glsl_version.empty());
  QCOMPARE(view->parentWidget(), &host);
}

void WellLogViewTest::prepared_curve_renders_into_the_widget_fbo() {
  auto fixture = prepared_view_fixture();
  WellLogView view(fixture.session);
  view.set_document_id(fixture.document_id);
  view.resize(200, 200);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().initialization_complete,
                           5000);
  QVERIFY2(view.capability_report().graphics_available,
           view.capability_report().unavailable_reason.c_str());
  QTRY_COMPARE_WITH_TIMEOUT(
      fixture.session->viewport_pixel_height(fixture.document_id),
      std::optional<std::uint32_t>{static_cast<std::uint32_t>(
          static_cast<double>(view.height()) * view.devicePixelRatioF())},
      5000);

  QImage image;
  QTRY_VERIFY_WITH_TIMEOUT(
      (image = view.grabFramebuffer(),
       image.pixelColor(image.width() / 2, image.height() / 2) !=
           QColor{Qt::white}),
      5000);
  const auto center_color =
      image.pixelColor(image.width() / 2, image.height() / 2);
  const auto center_description = center_color.name(QColor::HexArgb);
  QVERIFY2(center_color.red() >= 125 && center_color.red() <= 150,
           qPrintable(center_description));
  QVERIFY2(center_color.green() >= 145 && center_color.green() <= 170,
           qPrintable(center_description));
  QVERIFY2(center_color.blue() >= 160 && center_color.blue() <= 195,
           qPrintable(center_description));
  QCOMPARE(image.pixelColor(image.width() - 20, 20), QColor{Qt::white});

  qsizetype curve_pixel_count{};
  for (int top = 0; top < image.height(); ++top) {
    for (int left = 0; left < image.width(); ++left) {
      const auto color = image.pixelColor(left, top);
      if (color.red() < 200 && color.green() < 200 && color.blue() < 210) {
        ++curve_pixel_count;
      }
    }
  }
  QVERIFY(curve_pixel_count >= 200);
  QVERIFY(curve_pixel_count <= 2000);

  auto matched_rows = 0;
  for (int top = 0; top < image.height(); ++top) {
    const auto expected_left = static_cast<int>(std::lround(
        static_cast<double>(top) / static_cast<double>(image.height() - 1) *
        static_cast<double>(image.width() - 1)));
    auto row_matches = false;
    for (auto offset = -4; offset <= 4; ++offset) {
      const auto left =
          std::clamp(expected_left + offset, 0, image.width() - 1);
      const auto color = image.pixelColor(left, top);
      if (color.red() < 200 && color.green() < 200 && color.blue() < 210) {
        row_matches = true;
        break;
      }
    }
    matched_rows += row_matches ? 1 : 0;
  }
  QVERIFY(matched_rows >= image.height() * 9 / 10);

  view.set_document_id(EntityId{});
  QTRY_COMPARE_WITH_TIMEOUT(
      (image = view.grabFramebuffer(),
       image.pixelColor(image.width() / 2, image.height() / 2)),
      QColor{Qt::white}, 5000);
}

void WellLogViewTest::pointer_interaction_updates_session_and_semantic_picks() {
  auto fixture = prepared_view_fixture();
  WellLogView view(fixture.session);
  view.set_document_id(fixture.document_id);
  view.resize(200, 200);
  QSignalSpy hover_spy(&view, &WellLogView::hoverChanged);
  QSignalSpy click_spy(&view, &WellLogView::curveClicked);
  QSignalSpy crosshair_spy(&view, &WellLogView::crosshairChanged);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().graphics_available, 5000);

  const auto local_position = QPointF{100.0, 100.0};
  QMouseEvent hover_event(QEvent::MouseMove, local_position,
                          QPointF{view.mapToGlobal(local_position.toPoint())},
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&view, &hover_event);
  QTRY_VERIFY_WITH_TIMEOUT(hover_spy.count() >= 1, 5000);
  QVERIFY(crosshair_spy.count() >= 1);
  const auto crosshair = fixture.session->crosshair(fixture.document_id);
  QVERIFY(crosshair.has_value());
  QVERIFY(std::abs(crosshair->track_fraction - 0.5) < 0.01);
  QVERIFY(std::abs(crosshair->display_depth - 1050.0) < 1.0);

  const auto hover = view.hover_pick();
  QVERIFY(hover.has_value());
  QCOMPARE(hover->curve_id, fixture.curve_id);
  QCOMPARE(hover->sample_index, std::uint64_t{1});
  QVERIFY(std::abs(hover->reference_depth - 1050.0) < 1.0);
  QVERIFY(std::abs(hover->display_depth - 1050.0) < 1.0);
  QVERIFY(std::abs(hover->value - 50.0) < 1.0);
  QVERIFY(hover->distance.value <= 2.0);

  QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint{100, 100});
  QTRY_COMPARE_WITH_TIMEOUT(click_spy.count(), 1, 5000);
  const auto clicked = view.click_pick();
  QVERIFY(clicked.has_value());
  QCOMPARE(clicked->curve_id, fixture.curve_id);

  const auto image = view.grabFramebuffer();
  const auto center = image.pixelColor(image.width() / 2, image.height() / 2);
  QVERIFY(center.red() > center.green() * 2);
}

void WellLogViewTest::pointer_pan_zoom_and_reset_use_session_commands() {
  auto fixture = prepared_view_fixture();
  WellLogView view(fixture.session);
  view.set_document_id(fixture.document_id);
  view.resize(200, 200);
  QSignalSpy viewport_spy(&view, &WellLogView::viewportChanged);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().graphics_available, 5000);

  QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint{100, 100});
  QTest::mouseMove(&view, QPoint{100, 120}, 20);
  QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint{100, 120});
  auto viewport = fixture.session->viewport(fixture.document_id);
  QVERIFY(viewport.has_value());
  QVERIFY(std::abs(viewport->top - 990.0) < 1.0);
  QVERIFY(std::abs(viewport->bottom - 1090.0) < 1.0);

  const auto local_position = QPointF{100.0, 100.0};
  QWheelEvent wheel_event(
      local_position, view.mapToGlobal(local_position.toPoint()), QPoint{},
      QPoint{0, 120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&view, &wheel_event);
  viewport = fixture.session->viewport(fixture.document_id);
  QVERIFY(viewport->bottom - viewport->top < 100.0);
  QTRY_VERIFY_WITH_TIMEOUT(viewport_spy.count() >= 1, 5000);

  view.reset_viewport();
  viewport = fixture.session->viewport(fixture.document_id);
  QCOMPARE(viewport->top, 1000.0);
  QCOMPARE(viewport->bottom, 1100.0);
}

void WellLogViewTest::widget_rebuild_restores_curve_from_session_cpu_state() {
  auto fixture = prepared_view_fixture();
  const auto render_once = [&fixture]() {
    WellLogView view(fixture.session);
    view.set_document_id(fixture.document_id);
    view.resize(200, 200);
    view.show();
    if (!QTest::qWaitForWindowExposed(&view)) {
      return false;
    }
    QImage image;
    for (auto attempt = 0; attempt < 50; ++attempt) {
      QApplication::processEvents();
      if (view.capability_report().graphics_available) {
        image = view.grabFramebuffer();
        if (image.pixelColor(image.width() / 2, image.height() / 2) !=
            QColor{Qt::white}) {
          return true;
        }
      }
      QTest::qWait(20);
    }
    return false;
  };

  QVERIFY(render_once());
  QVERIFY(fixture.session->prepared_scene(fixture.document_id) != nullptr);
  QVERIFY(render_once());
}

void WellLogViewTest::
    top_level_reparent_restores_curve_after_context_recreation() {
  auto fixture = prepared_view_fixture();
  QWidget first_host;
  QWidget second_host;
  auto *first_layout = new QVBoxLayout(&first_host);
  auto *second_layout = new QVBoxLayout(&second_host);
  auto *view = new WellLogView(fixture.session);
  view->set_document_id(fixture.document_id);
  first_layout->addWidget(view);
  first_host.resize(200, 200);
  first_host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&first_host));
  QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().graphics_available, 5000);
  auto image = view->grabFramebuffer();
  QVERIFY(image.pixelColor(image.width() / 2, image.height() / 2) !=
          QColor{Qt::white});

  view->setParent(&second_host);
  second_layout->addWidget(view);
  second_host.resize(200, 200);
  second_host.show();
  view->show();
  QVERIFY(QTest::qWaitForWindowExposed(&second_host));
  QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().graphics_available, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(
      (image = view->grabFramebuffer(),
       image.pixelColor(image.width() / 2, image.height() / 2) !=
           QColor{Qt::white}),
      5000);
}

void WellLogViewTest::unavailable_context_publishes_a_capability_report() {
  if (QGuiApplication::platformName() != QStringLiteral("minimal")) {
    QSKIP("the unavailable-context contract runs with the minimal QPA plugin");
  }
  WellLogView view;
  QSignalSpy capability_spy(&view, &WellLogView::capabilityChanged);
  QSignalSpy fatal_spy(&view, &WellLogView::fatalViewError);
  view.resize(200, 200);
  view.show();

  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().initialization_complete,
                           3000);
  QVERIFY(!view.capability_report().graphics_available);
  QVERIFY(!view.capability_report().unavailable_reason.empty());
  QVERIFY(capability_spy.count() >= 1);
  QVERIFY(fatal_spy.count() >= 1);
}

void WellLogViewTest::
    external_session_events_refresh_and_coalesce_qt_signals() {
  auto fixture = prepared_view_fixture();
  WellLogView view(fixture.session);
  view.set_document_id(fixture.document_id);
  view.resize(200, 200);
  QSignalSpy viewport_spy(&view, &WellLogView::viewportChanged);
  QSignalSpy crosshair_spy(&view, &WellLogView::crosshairChanged);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().graphics_available, 5000);

  for (auto offset = 1; offset <= 3; ++offset) {
    QVERIFY(fixture.session
                ->execute(SetViewportCommand{
                    .document_id = fixture.document_id,
                    .viewport =
                        DepthViewport{
                            .top = 1000.0 + offset,
                            .bottom = 1100.0 + offset,
                        },
                })
                .has_value());
  }
  QTRY_COMPARE_WITH_TIMEOUT(viewport_spy.count(), 1, 5000);

  for (auto offset = 1; offset <= 3; ++offset) {
    QVERIFY(fixture.session
                ->execute(SetCrosshairCommand{
                    .document_id = fixture.document_id,
                    .crosshair =
                        CrosshairState{
                            .track_fraction = 0.25,
                            .display_depth = 1050.0 + offset,
                        },
                })
                .has_value());
  }
  QTRY_COMPARE_WITH_TIMEOUT(crosshair_spy.count(), 1, 5000);
  const auto image = view.grabFramebuffer();
  const auto cursor_pixel =
      image.pixelColor(image.width() / 4, image.height() / 2);
  QVERIFY(cursor_pixel.red() > cursor_pixel.green() * 2);
}

void WellLogViewTest::asynchronous_failures_keep_their_diagnostic_identity() {
  auto fixture = prepared_view_fixture(PerformanceBudgets{
      .maximum_cpu_derived_bytes = 1,
      .maximum_gpu_cache_bytes = 8 * 1024 * 1024,
      .maximum_upload_bytes_per_frame = 256 * 1024,
      .prefetch_viewports = 2.0,
      .asynchronous_sample_threshold = 1,
  });
  WellLogView view(fixture.session);
  view.set_document_id(fixture.document_id);
  view.resize(200, 200);
  QSignalSpy diagnostic_spy(&view, &WellLogView::diagnosticPublished);
  QSignalSpy error_spy(&view, &WellLogView::viewError);
  view.show();

  QTRY_VERIFY_WITH_TIMEOUT(diagnostic_spy.count() >= 1, 5000);
  QCOMPARE(diagnostic_spy.last().at(0).toString(),
           QStringLiteral("asynchronous_preparation_failed"));
  QTRY_VERIFY_WITH_TIMEOUT(error_spy.count() >= 1, 5000);
  QCOMPARE(error_spy.last().at(0).toString(),
           QStringLiteral("asynchronous_preparation_failed"));
  QVERIFY(error_spy.last().at(1).toString().contains(
      QStringLiteral("resource_exhausted")));
}

void WellLogViewTest::cross_thread_public_mutations_are_queued_to_gui() {
  auto fixture = prepared_view_fixture();
  WellLogView view(fixture.session);

  std::thread document_worker([&view, document_id = fixture.document_id]() {
    view.set_document_id(document_id);
  });
  document_worker.join();
  QTRY_VERIFY_WITH_TIMEOUT(view.document_id() == fixture.document_id, 5000);

  QVERIFY(fixture.session
              ->execute(SetViewportCommand{
                  .document_id = fixture.document_id,
                  .viewport = DepthViewport{.top = 1020.0, .bottom = 1080.0},
              })
              .has_value());
  std::thread reset_worker([&view]() { view.reset_viewport(); });
  reset_worker.join();
  const auto default_viewport = std::optional<DepthViewport>{
      DepthViewport{.top = 1000.0, .bottom = 1100.0}};
  QTRY_VERIFY_WITH_TIMEOUT(
      fixture.session->viewport(fixture.document_id) == default_viewport, 5000);
}

void WellLogViewTest::layered_scene_renders_intervals_patterns_symbols_and_text() {
  const auto document_id = id("60000000-0000-4000-8000-000000000101");
  const auto axis_id = id("60000000-0000-4000-8000-000000000102");
  const auto curve_id = id("60000000-0000-4000-8000-000000000103");
  const auto track_id = id("60000000-0000-4000-8000-000000000104");
  const auto pattern_id = id("60000000-0000-4000-8000-000000000105");
  const auto interval_layer_id = id("60000000-0000-4000-8000-000000000106");
  const auto marker_layer_id = id("60000000-0000-4000-8000-000000000107");
  const auto symbol_layer_id = id("60000000-0000-4000-8000-000000000108");
  const auto text_layer_id = id("60000000-0000-4000-8000-000000000109");
  const auto interval_id = id("60000000-0000-4000-8000-00000000010a");
  const auto marker_id = id("60000000-0000-4000-8000-00000000010b");
  const auto symbol_id = id("60000000-0000-4000-8000-00000000010c");
  const auto annotation_id = id("60000000-0000-4000-8000-00000000010d");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1050.0, 1100.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{0.0, 50.0, 100.0});
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
  document_builder.add_interval(Interval{
      .id = interval_id,
      .top_reference_depth = 1000.0,
      .bottom_reference_depth = 1050.0,
      .semantic = IntervalSemantic::lithology,
      .pattern_id = pattern_id,
      .fill_color = RgbaColor{240, 230, 180, 255},
      .label = {},
  });
  document_builder.add_marker(Marker{
      .id = marker_id,
      .reference_depth = 1050.0,
      .semantic = MarkerSemantic::formation_top,
      .label = {},
  });
  document_builder.add_symbol(SymbolOccurrence{
      .id = symbol_id,
      .reference_depth = 1025.0,
      .track_fraction = 0.5,
      .kind = SymbolKind::diamond,
      .label = {},
  });
  document_builder.add_annotation(TextAnnotation{
      .id = annotation_id,
      .anchor = AnnotationAnchor::reference_depth,
      .reference_depth = 1010.0,
      .track_fraction = 0.5,
      .track_id = {},
      .depth_fraction = 0.0,
      .horizontal_fraction = 0.0,
      .scene_point = {},
      .text = "Sand",
      .language = "en",
      .orientation = TextOrientation::horizontal,
      .rotation_degrees = 0.0,
      .font_size = Millimetres{4.0},
  });

  auto session = std::make_shared<WellLogSession>();
  session->set_text_engine(std::make_shared<HarfBuzzTextEngine>());
  require_command(session->execute(SetDocumentCommand{document_builder.build()}),
                  "layered fixture SetDocumentCommand");
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = "m",
          .top = 1000.0,
          .bottom = 1100.0,
      },
      Millimetres{100.0}, "font-fixture-v1");
  presentation_builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{30.0}, .z_order = 0});
  presentation_builder.add_pattern(PatternDefinition{
      .id = pattern_id,
      .tile_width = Millimetres{4.0},
      .tile_height = Millimetres{4.0},
      .rotation_degrees = 0.0,
      .foreground = RgbaColor{120, 100, 60, 255},
      .background = RgbaColor{240, 230, 180, 255},
      .stroke_width = Millimetres{0.4},
      .scene_anchor = PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
      .primitives =
          {
              PatternLine{PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
                          PhysicalPoint{Millimetres{4.0}, Millimetres{4.0}}},
          },
  });
  presentation_builder.add_interval_layer(IntervalLayerSpec{
      .id = interval_layer_id,
      .track_id = track_id,
      .z_order = 0,
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
  });
  presentation_builder.add_marker_layer(MarkerLayerSpec{
      .id = marker_layer_id,
      .track_id = track_id,
      .z_order = 1,
      .line_color = RgbaColor{220, 20, 20, 255},
      .line_width = Millimetres{0.8},
      .draw_labels = false,
      .label_font_size = Millimetres{3.0},
      .label_color = RgbaColor{0, 0, 0, 255},
  });
  presentation_builder.add_symbol_layer(SymbolLayerSpec{
      .id = symbol_layer_id,
      .track_id = track_id,
      .z_order = 2,
      .color = RgbaColor{20, 20, 200, 255},
      .symbol_size = Millimetres{6.0},
  });
  presentation_builder.add_text_layer(TextLayerSpec{
      .id = text_layer_id,
      .track_id = track_id,
      .z_order = 3,
      .color = RgbaColor{0, 0, 0, 255},
  });
  require_command(
      session->execute(SetPresentationCommand{presentation_builder.build()}),
      "fixture SetPresentationCommand");
  QVERIFY(session->prepared_scene(document_id) != nullptr);
  QVERIFY(!session->prepared_scene(document_id)->intervals().empty());
  QVERIFY(!session->prepared_scene(document_id)->text_runs().empty());

  WellLogView view(session);
  view.set_document_id(document_id);
  view.resize(240, 240);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().initialization_complete,
                           5000);
  QVERIFY2(view.capability_report().graphics_available,
           view.capability_report().unavailable_reason.c_str());

  QImage image;
  QTRY_VERIFY_WITH_TIMEOUT(
      (image = view.grabFramebuffer(),
       image.pixelColor(image.width() / 2, image.height() / 4) !=
           QColor{Qt::white}),
      5000);

  // The patterned interval fills the top half with its tile background.
  const auto interval_color =
      image.pixelColor(image.width() / 8, image.height() / 8);
  QVERIFY2(interval_color.red() > 180 && interval_color.blue() < 220,
           qPrintable(interval_color.name(QColor::HexArgb)));

  // The red marker line crosses the middle of the view.
  auto marker_found = false;
  for (int row = image.height() / 2 - 4; row <= image.height() / 2 + 4;
       ++row) {
    for (int left = 0; left < image.width(); ++left) {
      const auto color = image.pixelColor(left, row);
      if (color.red() > 150 && color.green() < 100 && color.blue() < 100) {
        marker_found = true;
        break;
      }
    }
  }
  QVERIFY2(marker_found, "the red marker line must be visible");

  // The blue diamond symbol sits at the quarter-height mark.
  auto symbol_found = false;
  for (int row = image.height() / 5; row < image.height() * 3 / 10; ++row) {
    for (int left = image.width() / 3; left < image.width() * 2 / 3;
         ++left) {
      const auto color = image.pixelColor(left, row);
      if (color.blue() > 150 && color.red() < 100) {
        symbol_found = true;
        break;
      }
    }
  }
  QVERIFY2(symbol_found, "the blue symbol must be visible");

  // The built-in fallback font renders the annotation as dark pixels.
  auto text_found = false;
  for (int row = image.height() / 20; row < image.height() / 6; ++row) {
    for (int left = 0; left < image.width(); ++left) {
      const auto color = image.pixelColor(left, row);
      if (color.red() < 100 && color.green() < 100 && color.blue() < 100) {
        text_found = true;
        break;
      }
    }
  }
  QVERIFY2(text_found, "the annotation text must be visible");

  // Phase continuity: samples one tile period apart vertically share the
  // same pattern phase.
  const auto tile_pixels = std::max(
      4, static_cast<int>(std::lround(4.0 / 100.0 * image.height())));
  auto phase_matches = 0;
  auto phase_samples = 0;
  for (int row = tile_pixels + 8; row < image.height() / 2 - tile_pixels;
       row += 3) {
    for (int left = 8; left < image.width() - 8; left += 3) {
      ++phase_samples;
      if (image.pixelColor(left, row) ==
          image.pixelColor(left, row - tile_pixels)) {
        ++phase_matches;
      }
    }
  }
  QVERIFY(phase_samples > 0);
  QVERIFY2(phase_matches * 2 >= phase_samples,
           "pattern phase must repeat with the tile period");
}

} // namespace

int main(int argc, char **argv) {
  configure_well_log_surface_format();
  QApplication application(argc, argv);
  WellLogViewTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "well_log_view_test.moc"
