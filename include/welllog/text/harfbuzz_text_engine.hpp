#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <welllog/core/result.hpp>
#include <welllog/scene/text_engine.hpp>
#include <welllog/text/export.hpp>

namespace welllog {

// HarfBuzz/FreeType/ICU implementation of the platform-neutral text
// pipeline (ADR 0029). Fonts resolve in project, built-in fallback, then
// system order; every resolved face records a content fingerprint. The
// built-in fallback is an embedded 5x7 dot-matrix font covering printable
// ASCII so the pipeline stays deterministic with no font files at all.
//
// All methods are safe to call from any single preparation thread at a
// time; concurrent shaping from multiple threads is not supported.
class WELLLOG_TEXT_API HarfBuzzTextEngine final : public TextEngine {
public:
  HarfBuzzTextEngine();
  ~HarfBuzzTextEngine() override;
  HarfBuzzTextEngine(HarfBuzzTextEngine &&) noexcept;
  HarfBuzzTextEngine &operator=(HarfBuzzTextEngine &&) noexcept;
  HarfBuzzTextEngine(const HarfBuzzTextEngine &) = delete;
  HarfBuzzTextEngine &operator=(const HarfBuzzTextEngine &) = delete;

  // Registers a project font file (highest resolution priority) and
  // returns its engine-local font index. Files larger than the untrusted
  // asset limit are rejected.
  [[nodiscard]] Result<std::uint32_t>
  add_project_font(std::string_view path) noexcept;

  // Adds a directory searched (recursively) after the built-in fallback
  // when no registered font covers a code point.
  void add_system_font_directory(std::string_view path) noexcept;

  [[nodiscard]] Result<ShapedRun>
  shape(const TextShapeRequest &request) noexcept override;
  [[nodiscard]] Result<GlyphOutline>
  glyph_outline(std::uint32_t font_index, std::uint32_t glyph_id) noexcept
      override;
  [[nodiscard]] std::string
  font_fingerprint(std::uint32_t font_index) const override;
  [[nodiscard]] std::string
  font_family_name(std::uint32_t font_index) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace welllog
