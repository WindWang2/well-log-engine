#include "render_gl/capability_probe.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using welllog::detail::evaluate_capabilities;
using welllog::detail::OpenGlContextCapabilities;

[[noreturn]] void fail(const std::string &message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect_unavailable(OpenGlContextCapabilities capabilities,
                        const std::string &expected_reason) {
  const auto report = evaluate_capabilities(std::move(capabilities));
  if (!report.initialization_complete || report.graphics_available ||
      report.unavailable_reason != expected_reason) {
    fail("capability rejection did not publish the expected report");
  }
}

} // namespace

int main() {
  const auto supported = evaluate_capabilities(OpenGlContextCapabilities{
      .core_profile = true,
      .open_gl_major = 4,
      .open_gl_minor = 6,
      .stencil_bits = 8,
      .maximum_texture_size = 32768,
      .maximum_combined_texture_units = 192,
      .maximum_vertex_attributes = 16,
      .maximum_uniform_block_size = 65536,
      .buffer_storage_supported = true,
      .timer_query_supported = true,
      .vendor = "fixture-vendor",
      .renderer = "fixture-renderer",
      .open_gl_version = "4.6 fixture",
      .glsl_version = "4.60 fixture",
  });
  if (!supported.initialization_complete || !supported.graphics_available ||
      !supported.unavailable_reason.empty() ||
      supported.maximum_vertex_attributes != 16 ||
      !supported.buffer_storage_supported ||
      supported.persistent_mapping_enabled ||
      supported.active_upload_path != "OpenGL 3.3 budgeted double-buffer VBO") {
    fail("supported OpenGL capabilities were rejected");
  }

  expect_unavailable(
      OpenGlContextCapabilities{
          .core_profile = true,
          .open_gl_major = 3,
          .open_gl_minor = 2,
          .stencil_bits = 8,
          .maximum_texture_size = 0,
          .maximum_combined_texture_units = 0,
          .maximum_vertex_attributes = 0,
          .maximum_uniform_block_size = 0,
          .buffer_storage_supported = false,
          .timer_query_supported = false,
          .vendor = {},
          .renderer = {},
          .open_gl_version = {},
          .glsl_version = "3.30",
      },
      "OpenGL 3.3 Core is required");
  expect_unavailable(
      OpenGlContextCapabilities{
          .core_profile = false,
          .open_gl_major = 3,
          .open_gl_minor = 3,
          .stencil_bits = 8,
          .maximum_texture_size = 0,
          .maximum_combined_texture_units = 0,
          .maximum_vertex_attributes = 0,
          .maximum_uniform_block_size = 0,
          .buffer_storage_supported = false,
          .timer_query_supported = false,
          .vendor = {},
          .renderer = {},
          .open_gl_version = {},
          .glsl_version = "3.30",
      },
      "an OpenGL Core profile is required");
  expect_unavailable(
      OpenGlContextCapabilities{
          .core_profile = true,
          .open_gl_major = 3,
          .open_gl_minor = 3,
          .stencil_bits = 0,
          .maximum_texture_size = 0,
          .maximum_combined_texture_units = 0,
          .maximum_vertex_attributes = 0,
          .maximum_uniform_block_size = 0,
          .buffer_storage_supported = false,
          .timer_query_supported = false,
          .vendor = {},
          .renderer = {},
          .open_gl_version = {},
          .glsl_version = "3.30",
      },
      "an 8-bit stencil buffer is required");
  expect_unavailable(
      OpenGlContextCapabilities{
          .core_profile = true,
          .open_gl_major = 3,
          .open_gl_minor = 3,
          .stencil_bits = 8,
          .maximum_texture_size = 0,
          .maximum_combined_texture_units = 0,
          .maximum_vertex_attributes = 0,
          .maximum_uniform_block_size = 0,
          .buffer_storage_supported = false,
          .timer_query_supported = false,
          .vendor = {},
          .renderer = {},
          .open_gl_version = {},
          .glsl_version = "3.20",
      },
      "GLSL 3.30 or newer is required");

  std::cout << "PASS: OpenGL capability reports\n";
  return EXIT_SUCCESS;
}
