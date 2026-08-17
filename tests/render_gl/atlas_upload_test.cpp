#include "render_gl/renderer.hpp"

#include <welllog/session/session.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace welllog;
using namespace welllog::detail;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

unsigned next_gl_id() {
  static unsigned id = 1;
  return id++;
}

void stub_gen_ids(int n, unsigned *ids) {
  for (int i = 0; i < n; ++i) {
    ids[i] = next_gl_id();
  }
}

void stub_nop_u(unsigned) {}
void stub_nop_i(int) {}
unsigned stub_get_error() { return 0; }
unsigned stub_create_shader(unsigned) { return next_gl_id(); }
unsigned stub_create_program() { return next_gl_id(); }
void stub_shader_source(unsigned, int, const char *const *, const int *) {}
void stub_get_iv(unsigned, unsigned, int *params) {
  if (params != nullptr) {
    *params = 1;
  }
}
int stub_get_uniform(unsigned, const char *) {
  (void)0;
  return 1;
}
void stub_tex_image(unsigned, int, int, int, int, int, unsigned, unsigned,
                    const void *) {}
void stub_buffer_data(unsigned, std::ptrdiff_t, const void *, unsigned) {}
void stub_buffer_sub_data(unsigned, std::ptrdiff_t, std::ptrdiff_t,
                          const void *) {}
void stub_bind_u(unsigned) {}
void stub_bind_eu(unsigned, unsigned) {}
void stub_viewport(int, int, int, int) {}
void stub_clear_color(float, float, float, float) {}
void stub_uniform1f(int, float) {}
void stub_uniform2f(int, float, float) {}
void stub_uniform4f(int, float, float, float, float) {}
void stub_uniform1i(int, int) {}
void stub_blend(unsigned, unsigned, unsigned, unsigned) {}
void stub_color_mask(unsigned char, unsigned char, unsigned char,
                     unsigned char) {}
void stub_scissor(int, int, int, int) {}
void stub_draw(unsigned, int, int) {}
void stub_tex_param(unsigned, unsigned, int) {}
void stub_pixel_store(unsigned, int) {}
void stub_vertex_attrib(unsigned, int, unsigned, unsigned char, int,
                        const void *) {}
void stub_enable_u(unsigned) {}
void stub_delete_ids(int, const unsigned *) {}
void stub_attach(unsigned, unsigned) {}

detail::GlProcAddress stub_resolver(void *, const char *name) noexcept {
  using Proc = detail::GlProcAddress;
  static const std::unordered_map<std::string, Proc> k_procs = {
      {"glGenVertexArrays", reinterpret_cast<Proc>(&stub_gen_ids)},
      {"glBindVertexArray", reinterpret_cast<Proc>(&stub_bind_u)},
      {"glDeleteVertexArrays", reinterpret_cast<Proc>(&stub_delete_ids)},
      {"glGenBuffers", reinterpret_cast<Proc>(&stub_gen_ids)},
      {"glBindBuffer", reinterpret_cast<Proc>(&stub_bind_eu)},
      {"glBufferData", reinterpret_cast<Proc>(&stub_buffer_data)},
      {"glBufferSubData", reinterpret_cast<Proc>(&stub_buffer_sub_data)},
      {"glDeleteBuffers", reinterpret_cast<Proc>(&stub_delete_ids)},
      {"glGetError", reinterpret_cast<Proc>(&stub_get_error)},
      {"glCreateShader", reinterpret_cast<Proc>(&stub_create_shader)},
      {"glShaderSource", reinterpret_cast<Proc>(&stub_shader_source)},
      {"glCompileShader", reinterpret_cast<Proc>(&stub_nop_u)},
      {"glGetShaderiv", reinterpret_cast<Proc>(&stub_get_iv)},
      {"glDeleteShader", reinterpret_cast<Proc>(&stub_nop_u)},
      {"glCreateProgram", reinterpret_cast<Proc>(&stub_create_program)},
      {"glAttachShader", reinterpret_cast<Proc>(&stub_attach)},
      {"glLinkProgram", reinterpret_cast<Proc>(&stub_nop_u)},
      {"glGetProgramiv", reinterpret_cast<Proc>(&stub_get_iv)},
      {"glDeleteProgram", reinterpret_cast<Proc>(&stub_nop_u)},
      {"glUseProgram", reinterpret_cast<Proc>(&stub_nop_u)},
      {"glEnableVertexAttribArray", reinterpret_cast<Proc>(&stub_enable_u)},
      {"glVertexAttribPointer", reinterpret_cast<Proc>(&stub_vertex_attrib)},
      {"glGetUniformLocation", reinterpret_cast<Proc>(&stub_get_uniform)},
      {"glUniform1f", reinterpret_cast<Proc>(&stub_uniform1f)},
      {"glUniform2f", reinterpret_cast<Proc>(&stub_uniform2f)},
      {"glUniform4f", reinterpret_cast<Proc>(&stub_uniform4f)},
      {"glBindFramebuffer", reinterpret_cast<Proc>(&stub_bind_eu)},
      {"glViewport", reinterpret_cast<Proc>(&stub_viewport)},
      {"glClearColor", reinterpret_cast<Proc>(&stub_clear_color)},
      {"glClearStencil", reinterpret_cast<Proc>(&stub_nop_i)},
      {"glClear", reinterpret_cast<Proc>(&stub_enable_u)},
      {"glEnable", reinterpret_cast<Proc>(&stub_enable_u)},
      {"glDisable", reinterpret_cast<Proc>(&stub_enable_u)},
      {"glBlendFuncSeparate", reinterpret_cast<Proc>(&stub_blend)},
      {"glColorMask", reinterpret_cast<Proc>(&stub_color_mask)},
      {"glStencilMask", reinterpret_cast<Proc>(&stub_nop_u)},
      {"glScissor", reinterpret_cast<Proc>(&stub_scissor)},
      {"glDrawArrays", reinterpret_cast<Proc>(&stub_draw)},
      {"glGenTextures", reinterpret_cast<Proc>(&stub_gen_ids)},
      {"glDeleteTextures", reinterpret_cast<Proc>(&stub_delete_ids)},
      {"glBindTexture", reinterpret_cast<Proc>(&stub_bind_eu)},
      {"glTexImage2D", reinterpret_cast<Proc>(&stub_tex_image)},
      {"glTexParameteri", reinterpret_cast<Proc>(&stub_tex_param)},
      {"glActiveTexture", reinterpret_cast<Proc>(&stub_enable_u)},
      {"glPixelStorei", reinterpret_cast<Proc>(&stub_pixel_store)},
      {"glUniform1i", reinterpret_cast<Proc>(&stub_uniform1i)},
  };
  const auto it = k_procs.find(name == nullptr ? "" : name);
  return it == k_procs.end() ? nullptr : it->second;
}

PreparedScene make_scene() {
  const auto document_id = id("60600000-0000-4000-8000-000000000001");
  const auto axis_id = id("60600000-0000-4000-8000-000000000002");
  const auto curve_id = id("60600000-0000-4000-8000-000000000003");
  const auto track_id = id("60600000-0000-4000-8000-000000000004");
  const auto scale_id = id("60600000-0000-4000-8000-000000000005");
  const auto layer_id = id("60600000-0000-4000-8000-000000000006");
  auto depths = std::make_shared<const std::vector<double>>(
      std::vector<double>{1000.0, 1001.0, 1002.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::vector<double>{10.0, 40.0, 20.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma Ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  WellLogSession session;
  require(session.execute(SetDocumentCommand{builder.build()}).has_value(),
          "atlas fixture document must load");
  ScenePresentationBuilder presentation(
      document_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1002.0},
      Millimetres{40.0}, "font-fixture-v1");
  presentation.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 1});
  presentation.add_scale(TrackScaleSpec{.id = scale_id,
                                        .track_id = track_id,
                                        .mode = ScaleMode::linear,
                                        .minimum = 0.0,
                                        .maximum = 100.0,
                                        .direction = ScaleDirection::left_to_right,
                                        .unit = "API"});
  presentation.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{200, 20, 20, 255},
      .line_width = Millimetres{0.4},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{presentation.build()}).has_value(),
          "atlas fixture presentation must load");
  const auto scene = session.prepared_scene(document_id);
  require(scene != nullptr, "atlas fixture must prepare a scene");
  return *scene;
}

void finish_upload(GlRenderer &renderer) {
  for (int i = 0; i < 64; ++i) {
    const auto progress = renderer.upload_next();
    if (progress.completed) {
      return;
    }
    require(progress.pending, "upload_next must stay pending until complete");
  }
  fail("upload_next did not complete");
}

void identical_fingerprint_does_not_recopy_or_reupload() {
  GlRenderer renderer;
  require(renderer.initialize(&stub_resolver, nullptr),
          "stub GL must initialize the renderer");
  const auto scene = make_scene();
  const GpuUploadBudgets budgets{
      .maximum_cache_bytes = 256ULL * 1024ULL * 1024ULL,
      .maximum_bytes_per_frame = 64ULL * 1024ULL * 1024ULL,
  };

  reset_gl_atlas_debug_stats();
  require(renderer.queue_upload(scene, budgets), "first queue_upload must succeed");
  finish_upload(renderer);
  const auto first_tex = gl_atlas_debug_stats().tex_image_2d_calls;
  require(first_tex >= 2, "first upload must glTexImage2D both atlases");

  reset_gl_atlas_debug_stats();
  require(renderer.queue_upload(scene, budgets),
          "second queue_upload of the same fingerprint must succeed");
  require(gl_atlas_debug_stats().atlas_bytes_copied == 0,
          "cache hit must not value-copy atlas pixels (issue #606)");
  finish_upload(renderer);
  require(gl_atlas_debug_stats().tex_image_2d_calls == 0,
          "unchanged fingerprint must not re-upload atlas textures (issue #606)");
}

} // namespace

int main() {
  identical_fingerprint_does_not_recopy_or_reupload();
  std::cout << "PASS: atlas cache hit skips copy and tex upload\n";
  return EXIT_SUCCESS;
}
