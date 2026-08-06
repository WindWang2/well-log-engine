// Qt test for the virtualized TableModel adapter (ADR 0022, #154 Phase A). No
// GL context needed (a table model has no OpenGL), so it runs under
// QT_QPA_PLATFORM=minimal. Asserts: on-demand cell reads from the raw buffer
// (no QVariant matrix copy), null cells render empty, revision-driven model
// reset via QSignalSpy, header data carries name + unit, and the model is
// virtualized (rowCount is the projection's count with no row materialization).

#include <welllog/qtwidgets/table_model.hpp>
#include <welllog/table/table_projection.hpp>

#include <QApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace welllog;

EntityId id(std::string_view text) {
  auto parsed = EntityId::parse(text);
  return parsed.value_or(EntityId{});
}

const auto document_id = id("22222222-0000-4000-8000-000000000001");
const auto axis_id = id("22222222-0000-4000-8000-000000000002");
const auto curve_gr_id = id("22222222-0000-4000-8000-000000000003");

// A small document: one axis with 4 samples, one curve GR, plus a null bitmap
// marking sample 1 (so the model exercises the null-cell path).
TableProjection make_projection(DocumentRevision revision) {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0, 1003.0});
  auto gr = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0, 40.0});
  auto nulls_owner = std::make_shared<std::vector<std::uint8_t>>(1, 0);
  (*nulls_owner)[0] = 0b00000010; // sample 1 null
  NullBitmapView gr_nulls = NullBitmapView::from_raw(
      nulls_owner->data(), 4, nulls_owner->size(), SharedOwner{nulls_owner});

  WellLogDocumentBuilder builder(document_id, revision);
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "Gamma Ray",
      .unit = "API", .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(gr), .nulls = gr_nulls});
  const auto tables = TableProjectionBuilder::from_document(builder.build());
  return tables.empty() ? TableProjection{} : tables.front();
}

class TableModelTest final : public QObject {
  Q_OBJECT

private slots:
  void model_reports_projection_shape_and_reads_cells_on_demand();
  void null_cells_render_empty_and_flag_null();
  void header_data_carries_name_and_unit();
  void set_projection_resets_model_and_emits_signals();
  void model_is_virtualized_for_large_row_counts();
  void clipboard_copy_produces_tsv_html_and_internal_mime();
  void clipboard_large_selection_is_guarded();
  void document_revision_change_drives_invalidation();
};

void TableModelTest::
    model_reports_projection_shape_and_reads_cells_on_demand() {
  TableModel model;
  model.set_projection(make_projection(DocumentRevision{1}));
  // Shape: DEPTH + GR = 2 columns, 4 rows.
  QCOMPARE(model.rowCount(), 4);
  QCOMPARE(model.columnCount(), 2);

  // On-demand raw read: row 0 DEPTH = 1000.0, GR = 10.0 (exact source values,
  // read from the raw buffer via RawValueRole — not LOD).
  const auto depth0 = model.data(model.index(0, 0), TableModel::RawValueRole);
  QCOMPARE(depth0.toDouble(), 1000.0);
  const auto gr0 = model.data(model.index(0, 1), TableModel::RawValueRole);
  QCOMPARE(gr0.toDouble(), 10.0);
  // Last row.
  const auto gr3 = model.data(model.index(3, 1), TableModel::RawValueRole);
  QCOMPARE(gr3.toDouble(), 40.0);
}

void TableModelTest::null_cells_render_empty_and_flag_null() {
  TableModel model;
  model.set_projection(make_projection(DocumentRevision{1}));
  // GR row 1 is null (null bitmap). DisplayRole → empty (no sentinel text);
  // NullRole → true; RawValueRole → invalid.
  const auto display = model.data(model.index(1, 1), Qt::DisplayRole);
  QVERIFY(!display.isValid());
  const auto is_null = model.data(model.index(1, 1), TableModel::NullRole);
  QCOMPARE(is_null.toBool(), true);
  const auto raw = model.data(model.index(1, 1), TableModel::RawValueRole);
  QVERIFY(!raw.isValid());
  // Row 0 is NOT null.
  QCOMPARE(model.data(model.index(0, 1), TableModel::NullRole).toBool(), false);
  // A non-null DisplayRole is a double (numeric type preserved, not a string).
  const auto gr0 = model.data(model.index(0, 1), Qt::DisplayRole);
  QCOMPARE(static_cast<QMetaType::Type>(gr0.metaType().id()),
           QMetaType::Double);
  QCOMPARE(gr0.toDouble(), 10.0);
}

void TableModelTest::header_data_carries_name_and_unit() {
  TableModel model;
  model.set_projection(make_projection(DocumentRevision{1}));
  // Column 0 header = "DEPTH (m)"; column 1 = "GR (API)".
  QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(),
           QStringLiteral("DEPTH (m)"));
  QCOMPARE(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(),
           QStringLiteral("GR (API)"));
  // Vertical header = row index (the source sample index).
  QCOMPARE(model.headerData(2, Qt::Vertical, Qt::DisplayRole).toInt(), 2);
}

void TableModelTest::set_projection_resets_model_and_emits_signals() {
  TableModel model;
  model.set_projection(make_projection(DocumentRevision{1}));
  QCOMPARE(model.document_revision(), DocumentRevision{1});

  // set_projection emits modelReset (QSignalSpy catches modelReset on
  // QAbstractItemModel). Swapping to a new revision updates the carried
  // revision and signals the reset.
  QSignalSpy reset_spy(&model, &QAbstractTableModel::modelReset);
  QVERIFY(reset_spy.isValid());
  model.set_projection(make_projection(DocumentRevision{42}));
  QCOMPARE(reset_spy.count(), 1);
  QCOMPARE(model.document_revision(), DocumentRevision{42});
  QCOMPARE(model.document_id(), document_id);

  // An empty projection resets to a 0×0 model.
  model.set_projection(TableProjection{});
  QCOMPARE(model.rowCount(), 0);
  QCOMPARE(model.columnCount(), 0);
}

void TableModelTest::model_is_virtualized_for_large_row_counts() {
  // A 1M-row projection: rowCount() reports the count immediately (capped at
  // INT_MAX for Qt's 32-bit int), and only requested cells are read — no full
  // row matrix is materialized. Spot-check first and last without iterating.
  constexpr std::uint64_t n = 1'000'000;
  auto depths_fill = std::make_shared<std::vector<double>>(n);
  auto gr_fill = std::make_shared<std::vector<double>>(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    (*depths_fill)[i] = 1000.0 + static_cast<double>(i);
    (*gr_fill)[i] = static_cast<double>(i);
  }
  auto depths = std::shared_ptr<const std::vector<double>>(std::move(depths_fill));
  auto gr = std::shared_ptr<const std::vector<double>>(std::move(gr_fill));
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_id, .values = BufferView::from_vector(gr),
      .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());

  TableModel model;
  model.set_projection(tables.front());
  QCOMPARE(model.rowCount(), static_cast<int>(n));
  // Spot-read first/last (the view only ever requests a visible block; here we
  // confirm on-demand reads work at the extremes without materializing rows).
  QCOMPARE(model.data(model.index(0, 1), TableModel::RawValueRole).toDouble(),
           0.0);
  QCOMPARE(
      model.data(model.index(static_cast<int>(n - 1), 1), TableModel::RawValueRole)
          .toDouble(),
           static_cast<double>(n - 1));
}

void TableModelTest::clipboard_copy_produces_tsv_html_and_internal_mime() {
  // A small selection on the 4-row/2-column fixture: rows [0, 3) (3 rows).
  // TSV carries headers + tab-separated rows; null cells are empty; HTML is a
  // <table>; internal MIME carries document id/revision. Reads raw buffers.
  TableModel model;
  model.set_projection(make_projection(DocumentRevision{5}));
  auto payload = build_selection_clipboard(model, RowSelection{0, 3});
  QVERIFY(!payload.too_large_for_clipboard);
  QCOMPARE(payload.copied_rows, static_cast<std::uint64_t>(3));
  QVERIFY(payload.mime != nullptr);

  const auto tsv = QString::fromUtf8(payload.mime->data(QStringLiteral("text/plain")));
  // Header line first.
  QVERIFY(tsv.startsWith(QStringLiteral("DEPTH (m)\tGR (API)\n")));
  // Row 0: 1000 \t 10. Row 1 (GR null): 1001 \t (empty). Row 2: 1002 \t 30.
  QVERIFY(tsv.contains(QStringLiteral("\n1000\t10\n")));
  QVERIFY(tsv.contains(QStringLiteral("\n1001\t\n"))); // null GR → empty cell
  QVERIFY(tsv.contains(QStringLiteral("\n1002\t30\n")));

  const auto html = QString::fromUtf8(payload.mime->data(QStringLiteral("text/html")));
  QVERIFY(html.contains(QStringLiteral("<table>")));
  QVERIFY(html.contains(QStringLiteral("<th>DEPTH (m)</th>")));
  QVERIFY(html.contains(QStringLiteral("<td>10</td>")));
  QVERIFY(html.contains(QStringLiteral("<tr><td>1001</td><td></td></tr>")));

  // Internal MIME (ADR 0024 identity): "doc|rev|axis" on line 0, then one
  // "curveId|unit" line per column (column 0 is the axis/depth column → nil
  // curve id with the axis unit "m").
  const auto internal =
      QString::fromUtf8(payload.mime->data(
          QStringLiteral("application/x-welllog-table-selection")));
  QVERIFY(internal.startsWith(
      QStringLiteral("22222222-0000-4000-8000-000000000001")));
  // The revision (5) and the axis id are on the first line.
  QVERIFY(internal.contains(QStringLiteral("|5|")));
  QVERIFY(internal.contains(
      QStringLiteral("22222222-0000-4000-8000-000000000002")));
  // The curve id (GR) and its unit (API) are present.
  QVERIFY(internal.contains(
      QStringLiteral("22222222-0000-4000-8000-000000000003")));
  QVERIFY(internal.contains(QStringLiteral("|API")));
  QVERIFY(internal.contains(QStringLiteral("|m")));
}

void TableModelTest::clipboard_large_selection_is_guarded() {
  // A 1M-row, 2-column table: selecting all rows = 2M cells > the 250k limit.
  // The build returns too_large_for_clipboard WITHOUT building a payload (no
  // giant string on the GUI thread).
  constexpr std::uint64_t n = 1'000'000;
  auto depths_fill = std::make_shared<std::vector<double>>(n);
  auto gr_fill = std::make_shared<std::vector<double>>(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    (*depths_fill)[i] = static_cast<double>(i);
    (*gr_fill)[i] = static_cast<double>(i);
  }
  auto depths = std::shared_ptr<const std::vector<double>>(std::move(depths_fill));
  auto gr = std::shared_ptr<const std::vector<double>>(std::move(gr_fill));
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id, .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth, .unit = "m",
      .direction = AxisDirection::increasing});
  builder.add_curve(Curve{
      .id = curve_gr_id, .mnemonic = "GR", .display_name = "GR", .unit = "API",
      .sampling_axis_id = axis_id, .values = BufferView::from_vector(gr),
      .nulls = {}});
  const auto tables = TableProjectionBuilder::from_document(builder.build());

  TableModel model;
  model.set_projection(tables.front());
  // Select all rows — well over the cell limit.
  auto payload =
      build_selection_clipboard(model, RowSelection{0, n},
                                default_clipboard_cell_limit);
  QVERIFY(payload.too_large_for_clipboard);
  QVERIFY(payload.mime == nullptr);
  QCOMPARE(payload.copied_rows, static_cast<std::uint64_t>(0));

  // A small selection on the same large table still copies fine (only the
  // requested block is read — virtualized).
  auto small = build_selection_clipboard(model, RowSelection{10, 13});
  QVERIFY(!small.too_large_for_clipboard);
  QCOMPARE(small.copied_rows, static_cast<std::uint64_t>(3));
}

void TableModelTest::document_revision_change_drives_invalidation() {
  // End-to-end invalidation: a host holds a model at revision 1, detects the
  // document advanced to revision 2, and swaps the projection. The modelReset
  // signal fires and the carried revision updates — the view's stale content is
  // invalidated. (The ADR 0024 Selection-Set remap on revision change is Phase B.)
  TableModel model;
  model.set_projection(make_projection(DocumentRevision{1}));
  QCOMPARE(model.document_revision(), DocumentRevision{1});
  const auto v1_cell =
      model.data(model.index(0, 1), TableModel::RawValueRole).toDouble();
  QCOMPARE(v1_cell, 10.0);

  QSignalSpy reset_spy(&model, &QAbstractTableModel::modelReset);
  QVERIFY(reset_spy.isValid());
  // Document replaced at revision 2 → host rebuilds the projection and swaps.
  model.set_projection(make_projection(DocumentRevision{2}));
  QCOMPARE(reset_spy.count(), 1);
  QCOMPARE(model.document_revision(), DocumentRevision{2});
  // Document id is stable across the revision change.
  QCOMPARE(model.document_id(), document_id);
  // Content is the same fixture (revision is the only diff), confirming the
  // swap is clean — no stale/leftover rows from revision 1.
  QCOMPARE(model.rowCount(), 4);
  QCOMPARE(model.data(model.index(0, 1), TableModel::RawValueRole).toDouble(),
           10.0);
}

} // namespace

int main(int argc, char **argv) {
  // A table model needs no GL surface, only a QApplication; run under
  // QT_QPA_PLATFORM=minimal (set via ctest ENVIRONMENT).
  QApplication application(argc, argv);
  TableModelTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "table_model_test.moc"
