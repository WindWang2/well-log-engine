// Virtualized Qt table model for a Table Projection (ADR 0022, #154 Phase A).
// See header. The model is a thin adapter: rowCount/columnCount/data all read
// on demand from the core TableProjection, which reads the raw Curve buffer —
// no QVariant/row matrix is ever materialized, so a million-row table is
// virtualized.

#include <welllog/qtwidgets/table_model.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <QVariant>

#include <welllog/core/document.hpp>
#include <welllog/core/entity_id.hpp>
#include <welllog/table/table_projection.hpp>

namespace welllog {

namespace {

// Qt's int row/column index is 32-bit; a projection's row_count is 64-bit. A
// well-log table can exceed INT_MAX rows in principle. Cap the reported row
// count at INT_MAX (QAbstractItemModel's limit) — the visible block a view
// requests is always far below this, so virtualization is unaffected; only the
// reported total is capped. (A future ticket would handle >2^31-row paging.)
constexpr std::uint64_t max_qt_rows =
    static_cast<std::uint64_t>(std::numeric_limits<int>::max());

} // namespace

struct TableModel::Impl {
  TableProjection projection;
  // ADR 0024 selection sync (Phase B). When non-null, the model reflects the
  // session's Selection Set for this (document, axis) as a [first,last) span.
  WellLogSession *session{nullptr};
  EntityId document_id{};
  EntityId sampling_axis_id{};
  RowSelection reflected{{}, {}};
  bool has_reflected{false};
};

TableModel::TableModel(QObject *parent)
    : QAbstractTableModel(parent), impl_(std::make_unique<Impl>()) {}

TableModel::~TableModel() = default;

void TableModel::set_projection(TableProjection projection) noexcept {
  beginResetModel();
  impl_->projection = std::move(projection);
  // A new projection may be a different document/axis with different row
  // indices — drop the reflected selection so stale indices are never emitted.
  // The host re-attaches a selection source and calls refresh_session_selection
  // after setting the projection.
  impl_->reflected = {{}, {}};
  impl_->has_reflected = false;
  impl_->session = nullptr;
  impl_->document_id = EntityId{};
  impl_->sampling_axis_id = EntityId{};
  endResetModel();
}

EntityId TableModel::document_id() const noexcept {
  return impl_->projection.document_id();
}
DocumentRevision TableModel::document_revision() const noexcept {
  return impl_->projection.document_revision();
}
EntityId TableModel::sampling_axis_id() const noexcept {
  return impl_->projection.sampling_axis_id();
}
TableColumn TableModel::column(std::uint64_t index) const noexcept {
  return impl_->projection.column(index);
}
std::uint64_t TableModel::projection_column_count() const noexcept {
  return impl_->projection.column_count();
}

int TableModel::rowCount(const QModelIndex &parent) const noexcept {
  if (parent.isValid()) {
    return 0; // flat table — no children
  }
  const auto rows = impl_->projection.row_count();
  return rows > max_qt_rows ? static_cast<int>(max_qt_rows)
                            : static_cast<int>(rows);
}

int TableModel::columnCount(const QModelIndex &parent) const noexcept {
  if (parent.isValid()) {
    return 0;
  }
  // Column counts are tiny by construction (depth + N curves per axis); cap at
  // INT_MAX for parity with rowCount() and -Wconversion cleanliness.
  const auto cols = impl_->projection.column_count();
  return cols > max_qt_rows ? static_cast<int>(max_qt_rows)
                            : static_cast<int>(cols);
}

QVariant TableModel::data(const QModelIndex &index, int role) const noexcept {
  if (!index.isValid() || index.row() < 0 || index.column() < 0) {
    return {};
  }
  const auto cell =
      raw_cell(static_cast<std::uint64_t>(index.row()),
               static_cast<std::uint64_t>(index.column()));
  switch (role) {
    case Qt::DisplayRole:
      // Null cells render as an empty cell (no sentinel text); non-null as the
      // raw double (Qt formats per the view's delegate). Keeping it a double
      // preserves numeric type (no pre-formatting to string).
      return cell.null ? QVariant{} : QVariant{cell.value};
    case RawValueRole:
      return cell.null ? QVariant{} : QVariant{cell.value};
    case NullRole:
      return cell.null;
    default:
      return {};
  }
}

QVariant TableModel::headerData(int section, Qt::Orientation orientation,
                                int role) const noexcept {
  if (role != Qt::DisplayRole) {
    return {};
  }
  if (orientation == Qt::Horizontal) {
    // Column header: the column name (mnemonic / "DEPTH"). 0..columnCount-1.
    if (section < 0 ||
        static_cast<std::uint64_t>(section) >= impl_->projection.column_count()) {
      return {};
    }
    const auto col = impl_->projection.column(static_cast<std::uint64_t>(section));
    // Include the unit when present, e.g. "GR (API)".
    if (!col.unit.empty()) {
      return QString::fromStdString(col.name + " (" + col.unit + ")");
    }
    return QString::fromStdString(col.name);
  }
  // Vertical header: the row index (the source sample index into the axis).
  return section;
}

TableRowCell TableModel::raw_cell(std::uint64_t row,
                                  std::uint64_t column) const noexcept {
  const auto cell = impl_->projection.cell(row, column);
  return TableRowCell{cell.value.value_or(0.0), cell.null()};
}

void TableModel::set_session_selection_source(WellLogSession *session,
                                              EntityId document_id,
                                              EntityId sampling_axis_id) noexcept {
  impl_->session = session;
  impl_->document_id = document_id;
  impl_->sampling_axis_id = sampling_axis_id;
  // Drop any stale reflection; the host calls refresh_session_selection() once
  // the source is wired (and on every subsequent selection event).
  impl_->has_reflected = false;
  impl_->reflected = {{}, {}};
}

void TableModel::refresh_session_selection() noexcept {
  if (impl_->session == nullptr) {
    return;
  }
  const auto sel = impl_->session->selection(impl_->document_id);
  const auto prev = impl_->reflected;
  const auto had = impl_->has_reflected;
  // Reflect only selections on THIS model's axis (ADR 0024 / table-and-export.md
  // §4.1: each axis maps its own rows). A selection on another axis, an
  // invalidated selection, or no selection → clear the reflection.
  RowSelection next{{}, {}};
  bool reflect = false;
  if (sel.has_value() && sel->valid &&
      sel->sampling_axis_id == impl_->sampling_axis_id) {
    next = RowSelection{.first_row = sel->first_row, .last_row = sel->last_row};
    reflect = true;
  }
  impl_->reflected = next;
  impl_->has_reflected = reflect;
  const auto cols = columnCount();
  // Emit dataChanged over the union of the previous and new spans so a
  // connected view/QItemSelectionModel refreshes both the newly-selected and
  // newly-deselected rows. QModelIndex() parent (flat table).
  const auto emit_span = [&](std::uint64_t first, std::uint64_t last) {
    if (first >= last || cols <= 0) {
      return;
    }
    const auto row_count = static_cast<std::uint64_t>(rowCount());
    const auto clamped_last = std::min(last, row_count);
    if (first >= clamped_last) {
      return;
    }
    emit dataChanged(index(static_cast<int>(first), 0),
                     index(static_cast<int>(clamped_last - 1), cols - 1),
                     {Qt::DisplayRole, NullRole});
  };
  if (had) {
    emit_span(prev.first_row, prev.last_row);
  }
  if (reflect) {
    emit_span(next.first_row, next.last_row);
  }
}

RowSelection TableModel::current_row_selection() const noexcept {
  return impl_->reflected;
}

bool TableModel::set_row_selection(std::uint64_t first_row,
                                   std::uint64_t last_row) noexcept {
  if (impl_->session == nullptr) {
    return false;
  }
  return impl_->session
      ->execute(SetRowSelectionCommand{
          .document_id = impl_->document_id,
          .sampling_axis_id = impl_->sampling_axis_id,
          .first_row = first_row,
          .last_row = last_row,
      })
      .has_value();
}

namespace {

// Formats a double with the shortest round-trip representation (mirrors the
// export backends' deterministic number formatting). Returns "" for null.
QString format_cell(const TableModel &model, std::uint64_t row,
                    std::uint64_t column) {
  const auto c = model.raw_cell(row, column);
  if (c.null) {
    return {};
  }
  // QString::number default uses 6 sig figs; use 'g' with max precision for a
  // faithful round-trip of the raw value.
  return QString::number(c.value, 'g', 17);
}

// Strips tab/newline/carriage-return from a TSV field so a mnemonic/unit/header
// containing those characters cannot corrupt the column structure. TSV has no
// quoting convention here, so sanitizing is the robust choice (RFC 4180 quoting
// is reserved for the CSV exporter in a later ticket).
QString sanitize_tsv_field(QString s) {
  s.remove('\t');
  s.remove('\n');
  s.remove('\r');
  return s;
}

// The app-internal MIME type carrying document id/revision/units so an in-app
// paste preserves identity (table-and-export.md §4.2 "可选内部 MIME").
constexpr const char *kWellLogTableMime = "application/x-welllog-table-selection";

} // namespace

SelectionClipboard build_selection_clipboard(const TableModel &model,
                                             RowSelection selection,
                                             std::uint64_t cell_limit) {
  SelectionClipboard result;
  const auto row_count = static_cast<std::uint64_t>(model.rowCount());
  const auto col_count = static_cast<std::uint64_t>(model.columnCount());
  // Clamp to the model's bounds.
  if (selection.first_row >= row_count || col_count == 0) {
    result.mime = std::make_unique<QMimeData>();
    return result;
  }
  const auto last = std::min(selection.last_row, row_count);
  const auto rows = last - selection.first_row;
  // Large-selection guard: do NOT build a giant string on the GUI thread
  // (criterion "超大选择不在 GUI 线程构造巨型字符串"). Signal the host to prompt
  // export instead.
  if (rows * col_count > cell_limit) {
    result.too_large_for_clipboard = true;
    return result;
  }

  // TSV (text/plain): header row + one tab-separated row per selected row.
  // Null cells are empty (no sentinel). Header fields are sanitized so a
  // mnemonic/unit containing tab/newline cannot corrupt the column structure.
  QString tsv;
  for (std::uint64_t c = 0; c < col_count; ++c) {
    if (c != 0) {
      tsv += '\t';
    }
    tsv += sanitize_tsv_field(
        model.headerData(static_cast<int>(c), Qt::Horizontal, Qt::DisplayRole)
            .toString());
  }
  tsv += '\n';
  // HTML (text/html): a <table> with <th> headers and <td> cells.
  QString html = QStringLiteral("<table><tr>");
  for (std::uint64_t c = 0; c < col_count; ++c) {
    html += "<th>";
    html += model.headerData(static_cast<int>(c), Qt::Horizontal, Qt::DisplayRole)
                .toString()
                .toHtmlEscaped();
    html += "</th>";
  }
  html += "</tr>";
  for (std::uint64_t r = selection.first_row; r < last; ++r) {
    // TSV row.
    for (std::uint64_t c = 0; c < col_count; ++c) {
      if (c != 0) {
        tsv += '\t';
      }
      tsv += format_cell(model, r, c);
    }
    tsv += '\n';
    // HTML row.
    html += "<tr>";
    for (std::uint64_t c = 0; c < col_count; ++c) {
      html += "<td>";
      html += format_cell(model, r, c).toHtmlEscaped();
      html += "</td>";
    }
    html += "</tr>";
  }
  html += "</table>";

  // Internal MIME: the full semantic identity for an in-app paste (ADR 0024 /
  // table-and-export.md §4.2). Carries document id, revision, the Sampling
  // Axis id, and each column's curve id + unit so an in-app paste can resolve
  // back to the same raw samples without re-deriving from headers. The line
  // format is: "doc|rev|axis\ncurveId|unit\ncurveId|unit..." (column 0 is the
  // axis/depth column: a nil curve id with the axis unit). Nil curve ids are
  // emitted as the empty string. Sanitized of newlines within fields.
  QString internal =
      QString::fromStdString(model.document_id().to_string()) + '|' +
      QString::number(model.document_revision().value) + '|' +
      QString::fromStdString(model.sampling_axis_id().to_string()) + '\n';
  for (std::uint64_t c = 0; c < col_count; ++c) {
    if (c != 0) {
      internal += '\n';
    }
    const auto col = model.column(c);
    internal += sanitize_tsv_field(
        QString::fromStdString(col.curve_id.to_string()) + '|' +
        sanitize_tsv_field(QString::fromStdString(col.unit)));
  }

  result.copied_rows = rows;
  result.mime = std::make_unique<QMimeData>();
  result.mime->setData(QStringLiteral("text/plain"), tsv.toUtf8());
  result.mime->setData(QStringLiteral("text/html"), html.toUtf8());
  result.mime->setData(QString::fromLatin1(kWellLogTableMime), internal.toUtf8());
  return result;
}

} // namespace welllog
