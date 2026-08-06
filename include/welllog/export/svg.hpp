#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <welllog/core/result.hpp>
#include <welllog/export/export.hpp>
#include <welllog/scene/scene.hpp>

namespace welllog {

class WELLLOG_EXPORT_VECTOR_API SvgDocument {
public:
  SvgDocument();
  ~SvgDocument();
  SvgDocument(const SvgDocument &);
  SvgDocument &operator=(const SvgDocument &);
  SvgDocument(SvgDocument &&) noexcept;
  SvgDocument &operator=(SvgDocument &&) noexcept;

  [[nodiscard]] std::string_view text() const noexcept;

private:
  struct Impl;
  explicit SvgDocument(std::string text);
  std::shared_ptr<const Impl> impl_;
  friend class SvgExporter;
  friend class PaginatedSvgExporter;
};

class WELLLOG_EXPORT_VECTOR_API SvgExporter {
public:
  [[nodiscard]] static Result<SvgDocument>
  write(const PreparedScene &scene) noexcept;
};

} // namespace welllog
