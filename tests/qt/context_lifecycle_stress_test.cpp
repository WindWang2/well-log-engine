// #173 — Qt stress: create/reparent/show/hide/destroy churn, multi-view
// isolation on shared session, context recovery, Trace toggle under load.

#include <welllog/qtwidgets/well_log_view.hpp>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

class ContextLifecycleStressTest final : public QObject {
  Q_OBJECT

private slots:
  void create_show_hide_destroy_churn();
  void create_destroy_churn_keeps_session_without_pixel_readback();
  void reparent_and_hide_show_recovers_gpu();
  void multi_view_failure_isolates_current_view();
  void chrome_trace_toggle_during_document_replace();
};

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  Q_ASSERT(parsed.has_value());
  return *parsed;
}

// Q_ASSERT's condition is not evaluated under QT_NO_DEBUG — fixture commands
// executed inside one would silently never run in Release builds. Require the
// command to succeed in every build type and surface the engine error.
template <typename T>
void require_command(const Result<T> &result, std::string_view what) {
  if (!result.has_value()) {
    const auto &error = result.error();
    qFatal("%s failed: code=%d message=%d", std::string{what}.c_str(),
           static_cast<int>(error.code), static_cast<int>(error.message));
  }
}

struct Fixture {
  EntityId document_id;
  std::shared_ptr<WellLogSession> session;
};

Fixture make_fixture(PerformanceBudgets budgets = PerformanceBudgets{}) {
  const auto document_id = id("b7300000-0000-4000-8000-000000000001");
  const auto axis_id = id("b7300000-0000-4000-8000-000000000002");
  const auto curve_id = id("b7300000-0000-4000-8000-000000000003");
  const auto track_id = id("b7300000-0000-4000-8000-000000000004");
  const auto scale_id = id("b7300000-0000-4000-8000-000000000005");
  const auto layer_id = id("b7300000-0000-4000-8000-000000000006");

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
      Millimetres{100.0}, "context-lifecycle-stress");
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
  return Fixture{.document_id = document_id, .session = std::move(session)};
}

bool center_not_white(WellLogView &view) {
  const auto image = view.grabFramebuffer();
  if (image.isNull() || image.width() < 2 || image.height() < 2) {
    return false;
  }
  return image.pixelColor(image.width() / 2, image.height() / 2) !=
         QColor{Qt::white};
}

void ContextLifecycleStressTest::create_show_hide_destroy_churn() {
  auto fixture = make_fixture();
  constexpr int kRounds = 40;
  for (int i = 0; i < kRounds; ++i) {
    QWidget host;
    auto *layout = new QVBoxLayout(&host);
    auto *view = new WellLogView(fixture.session, &host);
    view->set_document_id(fixture.document_id);
    layout->addWidget(view);
    host.resize(160, 120);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().initialization_complete,
                             5000);
    if (view->capability_report().graphics_available) {
      QTRY_VERIFY_WITH_TIMEOUT(center_not_white(*view), 5000);
    }
    view->hide();
    QTest::qWait(2);
    view->show();
    QTest::qWait(2);
    // Destroy via host; session must outlive views.
    host.close();
  }
  // Session still holds prepared scene after view churn.
  QVERIFY(fixture.session->prepared_scene(fixture.document_id) != nullptr);
}

void ContextLifecycleStressTest::
    create_destroy_churn_keeps_session_without_pixel_readback() {
  // Mesa llvmpipe FBO readback is white-center on GHA; this slot still
  // exercises WellLogView create/show/hide/destroy + session lifetime.
  auto fixture = make_fixture();
  constexpr int kRounds = 20;
  for (int i = 0; i < kRounds; ++i) {
    QWidget host;
    auto *layout = new QVBoxLayout(&host);
    auto *view = new WellLogView(fixture.session, &host);
    view->set_document_id(fixture.document_id);
    layout->addWidget(view);
    host.resize(160, 120);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().initialization_complete,
                             5000);
    QVERIFY(view->document_id() == fixture.document_id);
    view->hide();
    QTest::qWait(2);
    view->show();
    QTest::qWait(2);
    host.close();
  }
  QVERIFY(fixture.session->prepared_scene(fixture.document_id) != nullptr);
}

void ContextLifecycleStressTest::reparent_and_hide_show_recovers_gpu() {
  auto fixture = make_fixture();
  QWidget first;
  QWidget second;
  auto *first_layout = new QVBoxLayout(&first);
  auto *second_layout = new QVBoxLayout(&second);
  auto *view = new WellLogView(fixture.session);
  view->set_document_id(fixture.document_id);
  first_layout->addWidget(view);
  first.resize(200, 160);
  first.show();
  QVERIFY(QTest::qWaitForWindowExposed(&first));
  QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().graphics_available, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(center_not_white(*view), 5000);

  for (int i = 0; i < 6; ++i) {
    view->hide();
    QTest::qWait(5);
    view->show();
    QVERIFY(QTest::qWaitForWindowExposed(&first));
    QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().graphics_available,
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(center_not_white(*view), 5000);

    QWidget *target = (i % 2 == 0) ? &second : &first;
    QVBoxLayout *target_layout =
        (i % 2 == 0) ? second_layout : first_layout;
    view->setParent(target);
    target_layout->addWidget(view);
    target->resize(200, 160);
    target->show();
    view->show();
    QVERIFY(QTest::qWaitForWindowExposed(target));
    QTRY_VERIFY_WITH_TIMEOUT(view->capability_report().graphics_available,
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(center_not_white(*view), 5000);
  }
}

void ContextLifecycleStressTest::multi_view_failure_isolates_current_view() {
  // One healthy view must keep painting when a sibling shares the session.
  // Capability failure is per-view (local impl); session scene is shared CPU.
  auto fixture = make_fixture();
  WellLogView primary(fixture.session);
  primary.set_document_id(fixture.document_id);
  primary.resize(200, 160);
  primary.show();
  QVERIFY(QTest::qWaitForWindowExposed(&primary));
  QTRY_VERIFY_WITH_TIMEOUT(primary.capability_report().graphics_available,
                           5000);
  QTRY_VERIFY_WITH_TIMEOUT(center_not_white(primary), 5000);

  WellLogView secondary(fixture.session);
  secondary.set_document_id(fixture.document_id);
  secondary.resize(200, 160);
  secondary.show();
  QVERIFY(QTest::qWaitForWindowExposed(&secondary));
  QTRY_VERIFY_WITH_TIMEOUT(secondary.capability_report().initialization_complete,
                           5000);

  // Both share session; destroy secondary repeatedly while primary paints.
  for (int i = 0; i < 8; ++i) {
    auto *temp = new WellLogView(fixture.session);
    temp->set_document_id(fixture.document_id);
    temp->resize(120, 90);
    temp->show();
    QTest::qWait(10);
    delete temp;
    QCoreApplication::processEvents();
    QVERIFY(primary.capability_report().graphics_available);
    QTRY_VERIFY_WITH_TIMEOUT(center_not_white(primary), 3000);
  }
  QVERIFY(fixture.session->prepared_scene(fixture.document_id) != nullptr);
}

void ContextLifecycleStressTest::
    chrome_trace_toggle_during_document_replace() {
  // AC4: Trace toggle under document traffic must not crash or leave bad JSON.
  auto fixture = make_fixture();
  WellLogView view(fixture.session);
  view.set_document_id(fixture.document_id);
  view.resize(200, 160);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTRY_VERIFY_WITH_TIMEOUT(view.capability_report().graphics_available, 5000);

  view.set_chrome_trace_enabled(true);
  view.set_profiler_overlay_visible(true);
  for (int i = 0; i < 4; ++i) {
    // Force repaint / poll path.
    view.update();
    QTest::qWait(16);
    view.set_chrome_trace_enabled(i % 2 == 0);
    view.set_profiler_overlay_visible(i % 2 == 1);
  }
  view.set_chrome_trace_enabled(true);
  view.update();
  QTest::qWait(32);
  const auto json = view.export_chrome_trace_json();
  // May be empty if no samples captured, but must not throw / corrupt.
  QVERIFY(json.empty() || json.find('[') != std::string::npos ||
          json.find('{') != std::string::npos);
  view.clear_chrome_trace();
  view.set_chrome_trace_enabled(false);
  QVERIFY(center_not_white(view));
}

} // namespace

int main(int argc, char **argv) {
  configure_well_log_surface_format();
  QApplication application(argc, argv);
  ContextLifecycleStressTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "context_lifecycle_stress_test.moc"
