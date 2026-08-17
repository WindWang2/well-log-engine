#pragma once

// CGM Version 3 **Binary** subset exporter (B1.CGM.1–3 / ADR 0054).
//
// Self-written — no third-party CGM SDK. Consumes PreparedScene.
//
// B1.CGM.3:
//   - multi-PICTURE pagination (fixed page height in mm)
//   - pattern fills: solid + diagonal hatch approximation + diagnostics
//   - VDC geometry helpers for 0.5 mm entry golden (format dimension)

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <welllog/core/result.hpp>
#include <welllog/export/cgm_export.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog {

class WELLLOG_EXPORT_CGM_API CgmDocument {
public:
  CgmDocument();
  ~CgmDocument();
  CgmDocument(const CgmDocument &);
  CgmDocument &operator=(const CgmDocument &);
  CgmDocument(CgmDocument &&) noexcept;
  CgmDocument &operator=(CgmDocument &&) noexcept;

  [[nodiscard]] std::string_view bytes() const noexcept;

private:
  struct Impl;
  explicit CgmDocument(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend class CgmBinaryWriter;
  friend class CgmSceneExporter;
};

// Out-of-band degradation notes (ADR 0054).
struct WELLLOG_EXPORT_CGM_API CgmExportDiagnostics {
  std::uint32_t patterns_flattened_to_solid{0};
  std::uint32_t patterns_hatch_approximated{0};
  std::uint32_t alpha_flattened_to_opaque{0};
  std::uint32_t non_latin_text_dropped{0};
  std::uint32_t intervals_emitted{0};
  std::uint32_t fill_regions_emitted{0};
  std::uint32_t pictures_emitted{0};
  std::uint32_t vdc_coordinates_clamped{0};
  std::vector<std::string> notes;

  [[nodiscard]] bool empty() const noexcept {
    return patterns_flattened_to_solid == 0 &&
           patterns_hatch_approximated == 0 && alpha_flattened_to_opaque == 0 &&
           non_latin_text_dropped == 0 && vdc_coordinates_clamped == 0 &&
           notes.empty();
  }

  [[nodiscard]] std::string summary() const;
};

// Pagination options for CgmSceneExporter (B1.CGM.3).
// ``page_height_mm <= 0`` → one continuous PICTURE spanning the full scene
// height (B1.CGM.1/2 behaviour). Positive → fixed pages with that printable
// depth height (mm of scene), optional overlap fraction in [0, 1).
struct WELLLOG_EXPORT_CGM_API CgmExportOptions {
  double page_height_mm{0.0};
  double page_overlap{0.0};
  // Diagonal hatch spacing for pattern-approximated fills (mm of scene).
  double hatch_step_mm{2.0};
};

// Integer VDC: 1 unit = 0.01 mm (centi-millimetre).
constexpr double k_cgm_vdc_per_mm = 100.0;

// Convert scene millimetres to VDC (y-up). ``window_top_mm`` / height define
// the depth window of the current PICTURE (0 / full height for continuous).
[[nodiscard]] WELLLOG_EXPORT_CGM_API std::pair<std::int16_t, std::int16_t>
cgm_scene_to_vdc(double scene_x_mm, double scene_y_mm, double window_top_mm,
                 double window_height_mm) noexcept;

class WELLLOG_EXPORT_CGM_API CgmBinaryWriter {
public:
  CgmBinaryWriter();
  ~CgmBinaryWriter();
  CgmBinaryWriter(const CgmBinaryWriter &) = delete;
  CgmBinaryWriter &operator=(const CgmBinaryWriter &) = delete;

  void begin_metafile(std::string_view name) noexcept;
  void metafile_version(std::int16_t version = 3) noexcept;
  void metafile_description(std::string_view text) noexcept;
  void vdc_type_integer() noexcept;
  void integer_precision(std::int16_t bits = 16) noexcept;
  void colour_precision(std::int16_t bits = 8) noexcept;
  void colour_value_extent() noexcept;
  void metafile_element_list_drawing_plus() noexcept;
  void begin_picture(std::string_view name) noexcept;
  void colour_selection_mode_direct() noexcept;
  void vdc_extent(std::int16_t x0, std::int16_t y0, std::int16_t x1,
                  std::int16_t y1) noexcept;
  void begin_picture_body() noexcept;
  void background_colour(std::uint8_t r, std::uint8_t g,
                         std::uint8_t b) noexcept;
  void line_width(std::int16_t width_vdc) noexcept;
  void line_colour(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;
  void fill_colour(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;
  void interior_style_solid() noexcept;
  void edge_visibility_off() noexcept;
  void character_height(std::int16_t height_vdc) noexcept;
  void text_colour(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

  void polyline(std::span<const std::pair<std::int16_t, std::int16_t>>
                    points) noexcept;
  void polygon(std::span<const std::pair<std::int16_t, std::int16_t>>
                   points) noexcept;
  void rectangle_polyline(std::int16_t x, std::int16_t y, std::int16_t w,
                          std::int16_t h) noexcept;
  void rectangle_fill(std::int16_t x, std::int16_t y, std::int16_t w,
                      std::int16_t h) noexcept;
  bool text(std::int16_t x, std::int16_t y, std::string_view s) noexcept;

  void end_picture() noexcept;
  void end_metafile() noexcept;

  [[nodiscard]] Result<CgmDocument> finish() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class WELLLOG_EXPORT_CGM_API CgmSceneExporter {
public:
  // Continuous single-PICTURE export (page_height_mm = 0).
  [[nodiscard]] static Result<CgmDocument>
  write(const PreparedScene &scene,
        CgmExportDiagnostics *diagnostics = nullptr) noexcept;

  // Paginated multi-PICTURE export (B1.CGM.3).
  [[nodiscard]] static Result<CgmDocument>
  write(const PreparedScene &scene, const CgmExportOptions &options,
        CgmExportDiagnostics *diagnostics = nullptr) noexcept;
};

[[nodiscard]] WELLLOG_EXPORT_CGM_API std::size_t
cgm_count_polylines(std::string_view cgm_bytes) noexcept;

// Sum of (x, y) pairs across all POLYLINE commands — chunked polylines each
// contribute their points (test parity for the 8191-point chunking).
WELLLOG_EXPORT_CGM_API std::size_t
cgm_polyline_total_points(std::string_view cgm_bytes) noexcept;

[[nodiscard]] WELLLOG_EXPORT_CGM_API std::size_t
cgm_count_polygons(std::string_view cgm_bytes) noexcept;

// Count BEGIN PICTURE commands (class 0, id 3).
[[nodiscard]] WELLLOG_EXPORT_CGM_API std::size_t
cgm_count_pictures(std::string_view cgm_bytes) noexcept;

[[nodiscard]] WELLLOG_EXPORT_CGM_API bool
cgm_has_metafile_delimiters(std::string_view cgm_bytes) noexcept;

} // namespace welllog
