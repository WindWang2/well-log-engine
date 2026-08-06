// Qt test for bidirectional graphics↔table selection sync (ADR 0024, #154 Phase
// B). No GL needed — a TableModel has no OpenGL and the session is headless —
// so this runs under QT_QPA_PLATFORM=minimal. Asserts: a session depth-range
// selection is reflected by the model as a [first_row,last_row) span; a table-
// driven row selection drives the session selection back; a model on one axis
// ignores a selection made on another axis; and the clipboard internal MIME now
// carries the axis id + per-column curve ids + units (ADR 0024 identity).

#include <welllog/qtwidgets/table_model.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

#include <QApplication>
#include <QByteArray>
#include <QMimeData>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QStringList>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  return parsed.value_or(EntityId{});
}

const auto document_id = id("77000000-0000-4000-8000-000000000001");
const auto axis_a_id = id("77000000-0000-4000-8000-000000000002");
const auto axis_b_id = id("77000000-0000-4000-8000-000000000003");
const auto curve_gr_id = id("77000000-0000-4000-8000-000000000004");
const auto curve_rt_id = id("77000000-0000-4000-8000-000000000005");
const auto curve_rhob_id = id("77000000-0000-4000-8000-000000000006");

// A document with axis A (4 samples [1000,1001,1002,1003], curves GR+RT) and
// axis B (3 samples [2000,2050,2100], curve RHOB). Used to test multi-axis
// selection isolation.
WellLogDocument make_document() {
  auto depths_a = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  auto rt = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1.0, 2.0, 4.0, 8.0});
  auto depths_b = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2000.0, 2050.0, 2100.0});
  auto rhob = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{2.1, 2.3, 2.5});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_a_id, .coordinates = BufferView::from_vector(depths_a),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_b_id, .coordinates = BufferView::from_vector(depths_b),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(gr), .nulls = {}});
  builder.add_curve(Curve{
      .id = curve_rt_id, .mnemonic = "RT", .display_name = "Resistivity",
      .unit = "ohm.m", .sampling_axis_id = axis_a_id,
      .values = BufferView::from_vector(rt), .nulls = {}});
  builder.add_curve(Curve{
      .id = curve_rhob_id, .mnemonic = "RHOB", .display_name = "Bulk Density",
      .unit = "g/cm3", .sampling_axis_id = axis_b_id,
      .values = BufferView::from_vector(rhob), .nulls = {}});
  return builder.build();
}

// Locates the projected table for an axis (nullptr if none).
const TableProjection *find_table(const std::vector<TableProjection> &tables,
                                  EntityId axis_id) {
  for (const auto &t : tables) {
    if (t.sampling_axis_id() == axis_id) {
      return &t;
    }
  }
  return nullptr;
}

class TableSelectionSyncTest final : public QObject {
  Q_OBJECT

private slots:
  void session_selection_reflected_as_row_span();
  void table_row_selection_drives_session();
  void model_on_one_axis_ignores_other_axis_selection();
  void refresh_emits_data_changed_over_spans();
  void clipboard_internal_mime_carries_axis_and_curve_identity();
  void set_projection_clears_stale_selection_source();
};

// A session depth-range selection on axis A is reflected by a model built on
// axis A as the matching [first_row,last_row) span after refresh.
void TableSelectionSyncTest::session_selection_reflected_as_row_span() {
  WellLogSession session;
  QVERIFY(session.execute(SetDocumentCommand{make_document()}).has_value());
  session.clear_events();

  const auto tables = TableProjectionBuilder::from_document(*session.document(document_id));
  const auto *axis_a_table = find_table(tables, axis_a_id);
  QVERIFY(axis_a_table != nullptr);

  TableModel model;
  model.set_projection(*axis_a_table);
  model.set_session_selection_source(&session, document_id, axis_a_id);

  // Select [1001, 1002] on axis A → rows [1, 3).
  QVERIFY(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1001.0, .bottom = 1002.0},
              })
              .has_value());
  model.refresh_session_selection();
  const auto reflected = model.current_row_selection();
  QCOMPARE(reflected.first_row, std::uint64_t{1});
  QCOMPARE(reflected.last_row, std::uint64_t{3});
}

// A table-driven row selection (model.set_row_selection) drives the session's
// selection back: session.selection() returns the matching depth range.
void TableSelectionSyncTest::table_row_selection_drives_session() {
  WellLogSession session;
  QVERIFY(session.execute(SetDocumentCommand{make_document()}).has_value());

  const auto tables = TableProjectionBuilder::from_document(*session.document(document_id));
  const auto *axis_a_table = find_table(tables, axis_a_id);
  QVERIFY(axis_a_table != nullptr);

  TableModel model;
  model.set_projection(*axis_a_table);
  model.set_session_selection_source(&session, document_id, axis_a_id);

  // Select rows [1, 3) from the table → depth range [1001, 1002].
  QVERIFY(model.set_row_selection(1, 3));
  const auto sel = session.selection(document_id);
  QVERIFY(sel.has_value());
  QCOMPARE(sel->sampling_axis_id, axis_a_id);
  QCOMPARE(sel->first_row, std::uint64_t{1});
  QCOMPARE(sel->last_row, std::uint64_t{3});
  QVERIFY(std::abs(sel->reference_depth_range.top - 1001.0) < 1.0e-9);
  QVERIFY(std::abs(sel->reference_depth_range.bottom - 1002.0) < 1.0e-9);
}

// A model built on axis A ignores a selection made on axis B (each axis maps
// its own rows — table-and-export.md §4.1). After an axis-B selection, the
// axis-A model's reflection is empty.
void TableSelectionSyncTest::model_on_one_axis_ignores_other_axis_selection() {
  WellLogSession session;
  QVERIFY(session.execute(SetDocumentCommand{make_document()}).has_value());

  const auto tables = TableProjectionBuilder::from_document(*session.document(document_id));
  const auto *axis_a_table = find_table(tables, axis_a_id);
  const auto *axis_b_table = find_table(tables, axis_b_id);
  QVERIFY(axis_a_table != nullptr && axis_b_table != nullptr);

  TableModel model_a;
  model_a.set_projection(*axis_a_table);
  model_a.set_session_selection_source(&session, document_id, axis_a_id);

  // Make a selection on axis B (not A).
  QVERIFY(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_b_id,
                  .reference_depth_range = {.top = 2000.0, .bottom = 2050.0},
              })
              .has_value());
  model_a.refresh_session_selection();
  // The axis-A model reflects NOTHING for an axis-B selection.
  const auto reflected = model_a.current_row_selection();
  QCOMPARE(reflected.first_row, std::uint64_t{0});
  QCOMPARE(reflected.last_row, std::uint64_t{0});
}

// refresh_session_selection emits dataChanged over the previous and new spans
// so a connected view/QItemSelectionModel refreshes.
void TableSelectionSyncTest::refresh_emits_data_changed_over_spans() {
  WellLogSession session;
  QVERIFY(session.execute(SetDocumentCommand{make_document()}).has_value());

  const auto tables = TableProjectionBuilder::from_document(*session.document(document_id));
  const auto *axis_a_table = find_table(tables, axis_a_id);
  QVERIFY(axis_a_table != nullptr);

  TableModel model;
  model.set_projection(*axis_a_table);
  model.set_session_selection_source(&session, document_id, axis_a_id);

  QSignalSpy spy(&model, &TableModel::dataChanged);
  QVERIFY(spy.isValid());
  QVERIFY(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1002.0},
              })
              .has_value());
  model.refresh_session_selection();
  QVERIFY(spy.count() >= 1);
}

// The clipboard internal MIME now carries the axis id + per-column curve ids +
// units (ADR 0024 identity), not just document id/revision.
void TableSelectionSyncTest::clipboard_internal_mime_carries_axis_and_curve_identity() {
  WellLogSession session;
  QVERIFY(session.execute(SetDocumentCommand{make_document()}).has_value());

  const auto tables = TableProjectionBuilder::from_document(*session.document(document_id));
  const auto *axis_a_table = find_table(tables, axis_a_id);
  QVERIFY(axis_a_table != nullptr);

  TableModel model;
  model.set_projection(*axis_a_table);

  const auto payload =
      build_selection_clipboard(model, RowSelection{.first_row = 0, .last_row = 2});
  QVERIFY(!payload.too_large_for_clipboard);
  QVERIFY(payload.mime != nullptr);
  const QByteArray internal =
      payload.mime->data(QStringLiteral("application/x-welllog-table-selection"));
  QVERIFY(!internal.isEmpty());
  const QString text = QString::fromUtf8(internal);
  // Line 0: doc|rev|axis. The axis id must be present.
  QVERIFY(text.contains(QString::fromStdString(axis_a_id.to_string())));
  // The curve ids (GR, RT) and units (API, ohm.m) must be present.
  QVERIFY(text.contains(QString::fromStdString(curve_gr_id.to_string())));
  QVERIFY(text.contains(QString::fromStdString(curve_rt_id.to_string())));
  QVERIFY(text.contains(QStringLiteral("API")));
  QVERIFY(text.contains(QStringLiteral("ohm.m")));
}

// set_projection() drops the selection source and any reflected span so stale
// row indices from a previous projection are never emitted. (A new projection
// may be a different document/axis with different row indices.)
void TableSelectionSyncTest::set_projection_clears_stale_selection_source() {
  WellLogSession session;
  QVERIFY(session.execute(SetDocumentCommand{make_document()}).has_value());

  const auto tables = TableProjectionBuilder::from_document(*session.document(document_id));
  const auto *axis_a_table = find_table(tables, axis_a_id);
  QVERIFY(axis_a_table != nullptr);

  TableModel model;
  model.set_projection(*axis_a_table);
  model.set_session_selection_source(&session, document_id, axis_a_id);
  QVERIFY(session
              .execute(SetSelectionCommand{
                  .document_id = document_id,
                  .sampling_axis_id = axis_a_id,
                  .reference_depth_range = {.top = 1000.0, .bottom = 1002.0},
              })
              .has_value());
  model.refresh_session_selection();
  QVERIFY(model.current_row_selection().last_row > 0);

  // Replace the projection — the reflected span and source must clear.
  TableProjection empty;
  model.set_projection(empty);
  QCOMPARE(model.current_row_selection().first_row, std::uint64_t{0});
  QCOMPARE(model.current_row_selection().last_row, std::uint64_t{0});
  // After clearing, refresh is a no-op (no source): no reflection returns.
  model.refresh_session_selection();
  QCOMPARE(model.current_row_selection().last_row, std::uint64_t{0});
  // set_row_selection returns false with no source attached.
  QVERIFY(!model.set_row_selection(0, 1));
}

} // namespace

int main(int argc, char **argv) {
  // No GL surface needed — a TableModel + headless session only. Runs under
  // QT_QPA_PLATFORM=minimal (set via ctest ENVIRONMENT).
  QApplication application(argc, argv);
  TableSelectionSyncTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "table_selection_sync_test.moc"
