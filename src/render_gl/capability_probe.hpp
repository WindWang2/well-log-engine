#pragma once

#include <string>

#include <welllog/render_gl/capability.hpp>

namespace welllog::detail {

struct OpenGlContextCapabilities {
  bool core_profile{};
  int open_gl_major{};
  int open_gl_minor{};
  int stencil_bits{};
  int maximum_texture_size{};
  int maximum_combined_texture_units{};
  int maximum_vertex_attributes{};
  int maximum_uniform_block_size{};
  bool buffer_storage_supported{};
  bool timer_query_supported{};
  std::string vendor;
  std::string renderer;
  std::string open_gl_version;
  std::string glsl_version;
};

[[nodiscard]] WELLLOG_RENDER_GL_API CapabilityReport
evaluate_capabilities(OpenGlContextCapabilities capabilities) noexcept;

} // namespace welllog::detail
