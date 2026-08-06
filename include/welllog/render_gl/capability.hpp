#pragma once

#include <string>

#include <welllog/render_gl/export.hpp>

namespace welllog {

struct WELLLOG_RENDER_GL_API CapabilityReport {
  bool initialization_complete{};
  bool graphics_available{};
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
  bool persistent_mapping_enabled{};
  std::string vendor;
  std::string renderer;
  std::string open_gl_version;
  std::string glsl_version;
  std::string active_upload_path;
  std::string unavailable_reason;
};

} // namespace welllog
