#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QOpenGLWidget>

#include <welllog/io/manifest.hpp>
#include <welllog/qtwidgets/export.hpp>
#include <welllog/render_gl/capability.hpp>
#include <welllog/session/observability.hpp>
#include <welllog/session/session.hpp>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

namespace welllog {

WELLLOG_QTWIDGETS_API void configure_well_log_surface_format();

class WELLLOG_QTWIDGETS_API WellLogView : public QOpenGLWidget {
  Q_OBJECT

public:
  explicit WellLogView(QWidget *parent = nullptr);
  explicit WellLogView(std::shared_ptr<WellLogSession> session,
                       QWidget *parent = nullptr);
  ~WellLogView() override;

  [[nodiscard]] WellLogSession &session() noexcept;
  [[nodiscard]] const WellLogSession &session() const noexcept;
  void set_document_id(EntityId document_id) noexcept;
  // Overrides the text pipeline installed on the session. The view
  // installs a HarfBuzz/FreeType/ICU engine by default when the text
  // library is built.
  void set_text_engine(std::shared_ptr<TextEngine> text_engine) noexcept;
  // Installs the host-side image-tile decoder used by the GL renderer to
  // upload raster layers (ADR 0045: the engine never decodes images; the
  // host supplies decoded tile bytes on demand). Must be called from the GUI
  // thread; takes effect on the next frame.
  void set_image_tile_resolver(
      std::function<Result<RasterTile>(const ImageTileRequest &)> resolver)
      noexcept;
  // Overrides the image-pyramid build options (tile size, derived-byte budget)
  // the session uses to build ImagePyramidMap from ImageSource entities (#184).
  // Mirrors how curve-LOD budgets flow through PerformanceBudgets; takes effect
  // on the next document LOD build.
  void set_image_pyramid_options(ImagePyramidOptions options) noexcept;
  // Sets the append viewport mode for this view's document (#200): whether an
  // AppendBatchCommand preserves the current viewport (Fixed, the default) or
  // advances its bottom to the new tail depth (Follow-Latest). Mirrors how the
  // host configures other session interaction state through the view.
  void set_append_viewport_mode(AppendViewportMode mode) noexcept;
  [[nodiscard]] AppendViewportMode append_viewport_mode() const noexcept;
  [[nodiscard]] std::optional<EntityId> document_id() const noexcept;
  [[nodiscard]] const CapabilityReport &capability_report() const noexcept;
  [[nodiscard]] std::optional<CurvePick> hover_pick() const noexcept;
  [[nodiscard]] std::optional<CurvePick> click_pick() const noexcept;
  // The current shared Selection Set entry for this view's document (ADR 0024),
  // or nullopt when there is none. Read straight from the session — the view is
  // an adapter, selection state lives in the session.
  [[nodiscard]] std::optional<SelectionState> selection() const noexcept;

  // --- Profiler Overlay + Chrome Trace (#168, ADR 0043) ---------------------
  // Low-overhead rolling aggregates (default off for overlay). Detailed Chrome
  // Trace recording is off by default and must be enabled explicitly.
  void set_profiler_overlay_visible(bool visible) noexcept;
  [[nodiscard]] bool profiler_overlay_visible() const noexcept;
  void set_chrome_trace_enabled(bool enabled) noexcept;
  [[nodiscard]] bool chrome_trace_enabled() const noexcept;
  void clear_chrome_trace() noexcept;
  // Chrome Trace Event JSON (safe for chrome://tracing). Empty when disabled
  // and no events captured.
  [[nodiscard]] std::string export_chrome_trace_json() const;
  // Rolling aggregate of recent paint frames for Python poll / host UI.
  [[nodiscard]] AggregatedFrameStats frame_stats() const noexcept;

public slots:
  void reset_viewport();
  // Selects a Reference Depth Range on `axis_id` for this view's document
  // (issues a SetSelectionCommand on the session). Mirrors the host API; the
  // built-in Ctrl+drag gesture uses the same path.
  void set_selection(EntityId axis_id, SelectionDepthRange range);
  // Clears the selection on this view's document.
  void clear_selection();

signals:
  void capabilityChanged();
  void fatalViewError();
  void viewError(const QString &code, const QString &message);
  void documentChanged(const QString &document_id, quint64 revision);
  void diagnosticPublished(const QString &code, const QString &document_id,
                           quint64 revision);
  void viewportChanged();
  void crosshairChanged();
  void hoverChanged();
  void curveClicked();
  // Emitted (coalesced) when the session's selection for this document changes
  // or is invalidated (ADR 0024).
  void selectionChanged();

protected:
  void initializeGL() override;
  void resizeGL(int width, int height) override;
  void paintGL() override;
  void showEvent(QShowEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  void publish_fatal_error();
  void cleanup_context() noexcept;
  void handle_session_event(ViewEvent event) noexcept;
  void schedule_coalesced_signals() noexcept;
  void update_pointer(double left, double top) noexcept;
  void update_capability_overlay() noexcept;
  // Ctrl+drag selection gesture (ADR 0024). begin_selection_drag captures the
  // Reference Depth at `pixel_top` and resolves the axis the press falls in
  // (the hovered curve's axis, else the document's first axis).
  // update_selection_drag issues a live SetSelectionCommand between the anchor
  // depth and the depth at `pixel_top`.
  void begin_selection_drag(double pixel_top) noexcept;
  void update_selection_drag(double pixel_top) noexcept;
  void update_profiler_overlay() noexcept;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace welllog
