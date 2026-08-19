#pragma once

// Virtualized Qt table model for a Table Projection (ADR 0022, #154 Phase A).
// A QAbstractTableModel adapter over a core welllog::TableProjection — the
// model reads cells ON DEMAND straight from the projection (which reads the
// raw Curve buffer), never materializing a full QVariant/row matrix. A
// million-row table is virtualized: rowCount() is O(1), and only the visible
// block the view requests is fetched. Mirrors the Qt-widgets-first adapter
// convention (ADR 0009): Qt types live only here, the core stays Qt-agnostic.
//
// Phase A scope: the host drives updates via set_projection() (the document
// id/revision is carried on the projection for invalidation). Phase B adds
// bidirectional graphics↔table selection sync via the ADR 0024 Selection Set.

#include <cstdint>
#include <memory>

#include <QAbstractTableModel>
#include <QMimeData>
#include <QVariant>

#include <welllog/qtwidgets/export.hpp>
#include <welllog/session/session.hpp>
#include <welllog/table/table_projection.hpp>

namespace welllog {

// One row of a projected curve table, materialized on demand for clipboard /
// block reads. Carries the raw double value and an explicit null flag (a null
// sample is empty, not a sentinel — table-and-export.md §4.2).
struct TableRowCell {
  double value{};
  bool null{true};
};

// A half-open [first_row, last_row) selection range on a table (also the
// reflected span from the session's Selection Set, ADR 0024).
struct RowSelection {
  std::uint64_t first_row{};
  std::uint64_t last_row{}; // exclusive
};

class WELLLOG_QTWIDGETS_API TableModel : public QAbstractTableModel {
  Q_OBJECT

public:
  // Custom item-data roles.
  enum Role {
    // The raw double value (invalid QVariant if the cell is null). Use this to
    // read the source value without display formatting.
    RawValueRole = Qt::UserRole,
    // True if the cell is null (null bitmap / out-of-range / non-finite).
    NullRole = Qt::UserRole + 1,
  };

  explicit TableModel(QObject *parent = nullptr);
  ~TableModel() override;
  TableModel(const TableModel &) = delete;
  TableModel &operator=(const TableModel &) = delete;

  // Replaces the projection this model adapts. Resets the model (emits
  // modelReset) so views refetch. An empty/null projection yields a 0×0 model.
  // Thread-safe in the #147 sense: call from the GUI thread; cross-thread
  // callers should marshal via QMetaObject::invokeMethod(QueuedConnection).
  void set_projection(TableProjection projection) noexcept;

  // The document id/revision this model's projection was built from (nil/0
  // when empty) — a host uses these to invalidate on document replacement.
  [[nodiscard]] EntityId document_id() const noexcept;
  [[nodiscard]] DocumentRevision document_revision() const noexcept;
  // The Sampling Axis this model's projection is aligned to (nil when empty).
  [[nodiscard]] EntityId sampling_axis_id() const noexcept;
  // Column metadata for the projection (column 0 is the axis/depth column).
  [[nodiscard]] TableColumn column(std::uint64_t index) const noexcept;
  // The projection's column count (64-bit; rowCount/columnCount report the
  // Qt int-capped value).
  [[nodiscard]] std::uint64_t projection_column_count() const noexcept;

  // --- QAbstractTableModel overrides (on-demand, no matrix copy) -----------
  [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const noexcept
      override;
  [[nodiscard]] int columnCount(const QModelIndex &parent = {}) const noexcept
      override;
  [[nodiscard]] QVariant data(const QModelIndex &index,
                              int role = Qt::DisplayRole) const noexcept
      override;
  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                    int role = Qt::DisplayRole) const noexcept
      override;

  // Reads one cell's raw value + null flag on demand (the zero-copy raw-buffer
  // path through the projection). Used by clipboard copy and exposed for tests.
  [[nodiscard]] TableRowCell raw_cell(std::uint64_t row,
                                      std::uint64_t column) const noexcept;

  // --- ADR 0024 selection sync (Phase B) -----------------------------------
  //
  // The model is a bidirectional adapter over the session's shared Selection
  // Set. A model built on one Sampling Axis reflects ONLY selections on that
  // axis (a selection on another axis is ignored — multi-axis tables map their
  // own rows, table-and-export.md §4.1).
  //
  // Attaches the model to a session's Selection Set for one axis. The model
  // subscribes to the session's selection events and refreshes its reflected
  // span automatically (events are marshalled onto the model's thread via a
  // queued invocation, so an async publication on a worker thread is safe).
  // refresh_session_selection() forces an immediate refresh. Passing a null
  // session detaches. The session must outlive the attachment (or be detached
  // first). The document/axis ids must match this model's projection.
  void set_session_selection_source(WellLogSession *session,
                                    EntityId document_id,
                                    EntityId sampling_axis_id) noexcept;
  // Reflects the session's current selection (if any, on this model's axis) as
  // a [first_row, last_row) span. Emits dataChanged over the previous and new
  // spans so a connected QItemSelectionModel/view updates. No-op when no source
  // is set. Safe to call on every selection event.
  void refresh_session_selection() noexcept;
  // The row span this model currently reflects from the session (or the
  // explicitly-set span when the host drives selection from the table).
  // `{0,0}` when there is no selection. Half-open [first_row, last_row).
  [[nodiscard]] RowSelection current_row_selection() const noexcept;
  // Drives selection FROM the table: issues a SetRowSelectionCommand on the
  // attached session for this model's axis. Returns false when no source is
  // attached or the command is rejected. The session then publishes
  // selection_changed, which refresh_session_selection() reflects back.
  [[nodiscard]] bool set_row_selection(std::uint64_t first_row,
                                       std::uint64_t last_row) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Result of building a clipboard payload from a table selection (ADR 0022,
// #154 Phase A / table-and-export.md §4.2). When the selection exceeds the
// safe per-clipboard limit, `too_large_for_clipboard` is set and no payload is
// built — the host should prompt the user to export instead (the criterion:
// "超大选择不在 GUI 线程构造巨型字符串").
struct SelectionClipboard {
  // True when the selection is too large to build on the GUI thread; the host
  // should offer a file export instead. When true, the QMimeData pointers below
  // are null.
  bool too_large_for_clipboard{false};
  // The number of rows actually copied (0 when too large).
  std::uint64_t copied_rows{};
  // Owning the built payload. Null when too large. The caller takes ownership
  // (parent it to the QApplication clipboard or delete it).
  std::unique_ptr<QMimeData> mime;
};

// Default ceiling above which a selection is "too large" to build on the GUI
// thread (table-and-export.md §4.2). ~250k cells keeps TSV/HTML well under a
// few MB; above this the host should prompt to export a file.
constexpr std::uint64_t default_clipboard_cell_limit = 250'000;

// Builds a clipboard payload (TSV `text/plain` + HTML `text/html` + an
// app-internal MIME carrying document id/revision/units) for the given
// selection on `model`. Reads the RAW buffer via the model (not LOD). When the
// selection's cell count exceeds `cell_limit`, returns
// `too_large_for_clipboard` without building anything. `first_row`/`last_row`
// are clamped to the model's row count.
[[nodiscard]] WELLLOG_QTWIDGETS_API SelectionClipboard
build_selection_clipboard(const TableModel &model, RowSelection selection,
                          std::uint64_t cell_limit = default_clipboard_cell_limit);

} // namespace welllog
