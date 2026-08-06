#include "render_gl/capability_probe.hpp"

#include <cctype>
#include <utility>

namespace welllog::detail {
namespace {

[[nodiscard]] bool glsl_is_at_least_330(std::string_view version) noexcept {
  std::size_t offset{};
  while (offset < version.size() &&
         std::isspace(static_cast<unsigned char>(version[offset])) != 0) {
    ++offset;
  }
  if (offset == version.size() ||
      std::isdigit(static_cast<unsigned char>(version[offset])) == 0) {
    return false;
  }
  const auto major = version[offset] - '0';
  ++offset;
  if (offset == version.size() || version[offset] != '.') {
    return false;
  }
  ++offset;
  int minor{};
  int digits{};
  while (offset < version.size() &&
         std::isdigit(static_cast<unsigned char>(version[offset])) != 0 &&
         digits < 2) {
    minor = minor * 10 + (version[offset] - '0');
    ++offset;
    ++digits;
  }
  return major > 3 || (major == 3 && minor >= 30);
}

} // namespace

CapabilityReport
evaluate_capabilities(OpenGlContextCapabilities capabilities) noexcept {
  try {
    CapabilityReport report{
        .initialization_complete = true,
        .graphics_available = false,
        .core_profile = capabilities.core_profile,
        .open_gl_major = capabilities.open_gl_major,
        .open_gl_minor = capabilities.open_gl_minor,
        .stencil_bits = capabilities.stencil_bits,
        .maximum_texture_size = capabilities.maximum_texture_size,
        .maximum_combined_texture_units =
            capabilities.maximum_combined_texture_units,
        .maximum_vertex_attributes = capabilities.maximum_vertex_attributes,
        .maximum_uniform_block_size = capabilities.maximum_uniform_block_size,
        .buffer_storage_supported = capabilities.buffer_storage_supported,
        .timer_query_supported = capabilities.timer_query_supported,
        .persistent_mapping_enabled = false,
        .vendor = std::move(capabilities.vendor),
        .renderer = std::move(capabilities.renderer),
        .open_gl_version = std::move(capabilities.open_gl_version),
        .glsl_version = std::move(capabilities.glsl_version),
        .active_upload_path = "OpenGL 3.3 budgeted double-buffer VBO",
        .unavailable_reason = {},
    };
    const auto version_available =
        report.open_gl_major > 3 ||
        (report.open_gl_major == 3 && report.open_gl_minor >= 3);
    if (!version_available) {
      report.unavailable_reason = "OpenGL 3.3 Core is required";
    } else if (!report.core_profile) {
      report.unavailable_reason = "an OpenGL Core profile is required";
    } else if (report.stencil_bits < 8) {
      report.unavailable_reason = "an 8-bit stencil buffer is required";
    } else if (!glsl_is_at_least_330(report.glsl_version)) {
      report.unavailable_reason = "GLSL 3.30 or newer is required";
    } else {
      report.graphics_available = true;
    }
    return report;
  } catch (...) {
    CapabilityReport report;
    report.initialization_complete = true;
    return report;
  }
}

} // namespace welllog::detail
