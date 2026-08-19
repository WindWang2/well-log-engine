#include "render_gl/renderer.hpp"

#include "render_gl/raster.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace welllog::detail {

GlAtlasDebugStats &gl_atlas_debug_stats() noexcept {
  static GlAtlasDebugStats stats{};
  return stats;
}

void reset_gl_atlas_debug_stats() noexcept { gl_atlas_debug_stats() = {}; }

GlDashDebugStats &gl_dash_debug_stats() noexcept {
  static GlDashDebugStats stats{};
  return stats;
}

void reset_gl_dash_debug_stats() noexcept { gl_dash_debug_stats() = {}; }

namespace {

#if defined(_WIN32)
#define WELLLOG_GL_CALL __stdcall
#else
#define WELLLOG_GL_CALL
#endif

using GlBoolean = unsigned char;
using GlChar = char;
using GlEnum = unsigned int;
using GlFloat = float;
using GlInt = int;

// Deterministic content serialization for the atlas fingerprint (#855): the
// pattern/glyph phase must not depend on pointer address, hash order or locale.
// Numbers use the shortest round-trip digits (no locale grouping), colours the
// four raw bytes, primitives a tagged compact form — so equal content always
// yields an equal string and different content never collides.
void append_fingerprint_number(std::string &out, double value) {
  std::array<char, 32> buffer{};
  const auto res = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                 value);
  if (res.ec == std::errc{}) {
    out.append(buffer.data(), res.ptr);
  } else {
    out += "nan";
  }
}

void append_fingerprint_color(std::string &out, const RgbaColor &color) {
  out.push_back(static_cast<char>(color.red));
  out.push_back(static_cast<char>(color.green));
  out.push_back(static_cast<char>(color.blue));
  out.push_back(static_cast<char>(color.alpha));
}

void append_fingerprint_primitive(std::string &out,
                                  const PatternPrimitive &primitive) {
  std::visit(
      [&out](const auto &p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, PatternLine>) {
          out += "l";
          append_fingerprint_number(out, p.from.left.value);
          out.push_back(',');
          append_fingerprint_number(out, p.from.top.value);
          out.push_back('>');
          append_fingerprint_number(out, p.to.left.value);
          out.push_back(',');
          append_fingerprint_number(out, p.to.top.value);
        } else if constexpr (std::is_same_v<T, PatternPolyline>) {
          out += "P";
          out.push_back(p.closed ? '1' : '0');
          for (const auto &pt : p.points) {
            out.push_back(';');
            append_fingerprint_number(out, pt.left.value);
            out.push_back(',');
            append_fingerprint_number(out, pt.top.value);
          }
        } else {
          out += "c";
          append_fingerprint_number(out, p.center.left.value);
          out.push_back(',');
          append_fingerprint_number(out, p.center.top.value);
          out.push_back(',');
          append_fingerprint_number(out, p.radius.value);
          out.push_back(',');
          out.push_back(p.filled ? '1' : '0');
        }
      },
      primitive);
}
using GlSize = int;
using GlSizePointer = std::ptrdiff_t;
using GlUInt = unsigned int;

constexpr GlEnum gl_array_buffer = 0x8892;
constexpr GlEnum gl_dynamic_draw = 0x88E8;
constexpr GlEnum gl_float = 0x1406;
constexpr GlEnum gl_false = 0;
constexpr GlEnum gl_vertex_shader = 0x8B31;
constexpr GlEnum gl_fragment_shader = 0x8B30;
constexpr GlEnum gl_compile_status = 0x8B81;
constexpr GlEnum gl_link_status = 0x8B82;
constexpr GlEnum gl_framebuffer = 0x8D40;
constexpr GlEnum gl_color_buffer_bit = 0x00004000;
constexpr GlEnum gl_stencil_buffer_bit = 0x00000400;
constexpr GlEnum gl_depth_test = 0x0B71;
constexpr GlEnum gl_blend = 0x0BE2;
constexpr GlEnum gl_cull_face = 0x0B44;
constexpr GlEnum gl_stencil_test = 0x0B90;
constexpr GlEnum gl_scissor_test = 0x0C11;
constexpr GlEnum gl_triangles = 0x0004;
constexpr GlEnum gl_src_alpha = 0x0302;
constexpr GlEnum gl_one_minus_src_alpha = 0x0303;
constexpr GlEnum gl_one = 1;
constexpr GlEnum gl_no_error = 0;
constexpr GlEnum gl_texture_2d = 0x0DE1;
constexpr GlEnum gl_texture0 = 0x84C0;
constexpr GlEnum gl_r8 = 0x8229;
constexpr GlEnum gl_red = 0x1903;
constexpr GlEnum gl_rgba8 = 0x8058;
constexpr GlEnum gl_rgba = 0x1908;
constexpr GlEnum gl_rgb8_internal = 0x8401;
constexpr GlEnum gl_rgb = 0x1907;
constexpr GlEnum gl_unsigned_byte = 0x1401;
constexpr GlEnum gl_texture_min_filter = 0x2801;
constexpr GlEnum gl_texture_mag_filter = 0x2800;
constexpr GlEnum gl_texture_wrap_s = 0x2802;
constexpr GlEnum gl_texture_wrap_t = 0x2803;
constexpr GlEnum gl_linear = 0x2601;
constexpr GlEnum gl_clamp_to_edge = 0x812F;
constexpr GlEnum gl_unpack_alignment = 0x0CF5;

using GlGenVertexArrays = void(WELLLOG_GL_CALL *)(GlSize, GlUInt *);
using GlBindVertexArray = void(WELLLOG_GL_CALL *)(GlUInt);
using GlDeleteVertexArrays = void(WELLLOG_GL_CALL *)(GlSize, const GlUInt *);
using GlGenBuffers = void(WELLLOG_GL_CALL *)(GlSize, GlUInt *);
using GlBindBuffer = void(WELLLOG_GL_CALL *)(GlEnum, GlUInt);
using GlBufferData = void(WELLLOG_GL_CALL *)(GlEnum, GlSizePointer,
                                             const void *, GlEnum);
using GlBufferSubData = void(WELLLOG_GL_CALL *)(GlEnum, GlSizePointer,
                                                GlSizePointer, const void *);
using GlDeleteBuffers = void(WELLLOG_GL_CALL *)(GlSize, const GlUInt *);
using GlGetError = GlEnum(WELLLOG_GL_CALL *)();
using GlCreateShader = GlUInt(WELLLOG_GL_CALL *)(GlEnum);
using GlShaderSource = void(WELLLOG_GL_CALL *)(GlUInt, GlSize,
                                               const GlChar *const *,
                                               const GlInt *);
using GlCompileShader = void(WELLLOG_GL_CALL *)(GlUInt);
using GlGetShaderiv = void(WELLLOG_GL_CALL *)(GlUInt, GlEnum, GlInt *);
using GlDeleteShader = void(WELLLOG_GL_CALL *)(GlUInt);
using GlCreateProgram = GlUInt(WELLLOG_GL_CALL *)();
using GlAttachShader = void(WELLLOG_GL_CALL *)(GlUInt, GlUInt);
using GlLinkProgram = void(WELLLOG_GL_CALL *)(GlUInt);
using GlGetProgramiv = void(WELLLOG_GL_CALL *)(GlUInt, GlEnum, GlInt *);
using GlDeleteProgram = void(WELLLOG_GL_CALL *)(GlUInt);
using GlUseProgram = void(WELLLOG_GL_CALL *)(GlUInt);
using GlEnableVertexAttribArray = void(WELLLOG_GL_CALL *)(GlUInt);
using GlVertexAttribPointer = void(WELLLOG_GL_CALL *)(GlUInt, GlInt, GlEnum,
                                                      GlBoolean, GlSize,
                                                      const void *);
using GlGetUniformLocation = GlInt(WELLLOG_GL_CALL *)(GlUInt, const GlChar *);
using GlUniform1f = void(WELLLOG_GL_CALL *)(GlInt, GlFloat);
using GlUniform2f = void(WELLLOG_GL_CALL *)(GlInt, GlFloat, GlFloat);
using GlUniform4f = void(WELLLOG_GL_CALL *)(GlInt, GlFloat, GlFloat, GlFloat,
                                            GlFloat);
using GlBindFramebuffer = void(WELLLOG_GL_CALL *)(GlEnum, GlUInt);
using GlViewport = void(WELLLOG_GL_CALL *)(GlInt, GlInt, GlSize, GlSize);
using GlClearColor = void(WELLLOG_GL_CALL *)(GlFloat, GlFloat, GlFloat,
                                             GlFloat);
using GlClearStencil = void(WELLLOG_GL_CALL *)(GlInt);
using GlClear = void(WELLLOG_GL_CALL *)(GlEnum);
using GlEnable = void(WELLLOG_GL_CALL *)(GlEnum);
using GlDisable = void(WELLLOG_GL_CALL *)(GlEnum);
using GlBlendFuncSeparate = void(WELLLOG_GL_CALL *)(GlEnum, GlEnum, GlEnum,
                                                    GlEnum);
using GlColorMask = void(WELLLOG_GL_CALL *)(GlBoolean, GlBoolean, GlBoolean,
                                            GlBoolean);
using GlStencilMask = void(WELLLOG_GL_CALL *)(GlUInt);
using GlScissor = void(WELLLOG_GL_CALL *)(GlInt, GlInt, GlSize, GlSize);
using GlDrawArrays = void(WELLLOG_GL_CALL *)(GlEnum, GlInt, GlSize);
using GlGenTextures = void(WELLLOG_GL_CALL *)(GlSize, GlUInt *);
using GlDeleteTextures = void(WELLLOG_GL_CALL *)(GlSize, const GlUInt *);
using GlBindTexture = void(WELLLOG_GL_CALL *)(GlEnum, GlUInt);
using GlTexImage2D = void(WELLLOG_GL_CALL *)(GlEnum, GlInt, GlInt, GlSize,
                                             GlSize, GlInt, GlEnum, GlEnum,
                                             const void *);
using GlTexParameter = void(WELLLOG_GL_CALL *)(GlEnum, GlEnum, GlInt);
using GlActiveTexture = void(WELLLOG_GL_CALL *)(GlEnum);
using GlPixelStore = void(WELLLOG_GL_CALL *)(GlEnum, GlInt);
using GlUniform1i = void(WELLLOG_GL_CALL *)(GlInt, GlInt);

template <typename Function>
[[nodiscard]] Function load(GlProcResolver resolver, void *resolver_context,
                            const char *name) noexcept {
  return reinterpret_cast<Function>(resolver(resolver_context, name));
}

struct GlFunctions {
  GlGenVertexArrays gen_vertex_arrays{};
  GlBindVertexArray bind_vertex_array{};
  GlDeleteVertexArrays delete_vertex_arrays{};
  GlGenBuffers gen_buffers{};
  GlBindBuffer bind_buffer{};
  GlBufferData buffer_data{};
  GlBufferSubData buffer_sub_data{};
  GlDeleteBuffers delete_buffers{};
  GlGetError get_error{};
  GlCreateShader create_shader{};
  GlShaderSource shader_source{};
  GlCompileShader compile_shader{};
  GlGetShaderiv get_shader_iv{};
  GlDeleteShader delete_shader{};
  GlCreateProgram create_program{};
  GlAttachShader attach_shader{};
  GlLinkProgram link_program{};
  GlGetProgramiv get_program_iv{};
  GlDeleteProgram delete_program{};
  GlUseProgram use_program{};
  GlEnableVertexAttribArray enable_vertex_attrib_array{};
  GlVertexAttribPointer vertex_attrib_pointer{};
  GlGetUniformLocation get_uniform_location{};
  GlUniform1f uniform_1f{};
  GlUniform2f uniform_2f{};
  GlUniform4f uniform_4f{};
  GlBindFramebuffer bind_framebuffer{};
  GlViewport viewport{};
  GlClearColor clear_color{};
  GlClearStencil clear_stencil{};
  GlClear clear{};
  GlEnable enable{};
  GlDisable disable{};
  GlBlendFuncSeparate blend_func_separate{};
  GlColorMask color_mask{};
  GlStencilMask stencil_mask{};
  GlScissor scissor{};
  GlDrawArrays draw_arrays{};
  GlGenTextures gen_textures{};
  GlDeleteTextures delete_textures{};
  GlBindTexture bind_texture{};
  GlTexImage2D tex_image_2d{};
  GlTexParameter tex_parameteri{};
  GlActiveTexture active_texture{};
  GlPixelStore pixel_storei{};
  GlUniform1i uniform_1i{};

  [[nodiscard]] bool complete() const noexcept {
    return gen_vertex_arrays != nullptr && bind_vertex_array != nullptr &&
           delete_vertex_arrays != nullptr && gen_buffers != nullptr &&
           bind_buffer != nullptr && buffer_data != nullptr &&
           buffer_sub_data != nullptr && delete_buffers != nullptr &&
           get_error != nullptr && create_shader != nullptr &&
           shader_source != nullptr && compile_shader != nullptr &&
           get_shader_iv != nullptr && delete_shader != nullptr &&
           create_program != nullptr && attach_shader != nullptr &&
           link_program != nullptr && get_program_iv != nullptr &&
           delete_program != nullptr && use_program != nullptr &&
           enable_vertex_attrib_array != nullptr &&
           vertex_attrib_pointer != nullptr &&
           get_uniform_location != nullptr && uniform_1f != nullptr &&
           uniform_2f != nullptr && uniform_4f != nullptr &&
           bind_framebuffer != nullptr && viewport != nullptr &&
           clear_color != nullptr && clear_stencil != nullptr &&
           clear != nullptr && enable != nullptr && disable != nullptr &&
           blend_func_separate != nullptr && color_mask != nullptr &&
           stencil_mask != nullptr && scissor != nullptr &&
           draw_arrays != nullptr && gen_textures != nullptr &&
           delete_textures != nullptr && bind_texture != nullptr &&
           tex_image_2d != nullptr && tex_parameteri != nullptr &&
           active_texture != nullptr && pixel_storei != nullptr &&
           uniform_1i != nullptr;
  }
};

[[nodiscard]] GlFunctions load_functions(GlProcResolver resolver,
                                         void *resolver_context) noexcept {
  return GlFunctions{
      .gen_vertex_arrays = load<GlGenVertexArrays>(resolver, resolver_context,
                                                   "glGenVertexArrays"),
      .bind_vertex_array = load<GlBindVertexArray>(resolver, resolver_context,
                                                   "glBindVertexArray"),
      .delete_vertex_arrays = load<GlDeleteVertexArrays>(
          resolver, resolver_context, "glDeleteVertexArrays"),
      .gen_buffers =
          load<GlGenBuffers>(resolver, resolver_context, "glGenBuffers"),
      .bind_buffer =
          load<GlBindBuffer>(resolver, resolver_context, "glBindBuffer"),
      .buffer_data =
          load<GlBufferData>(resolver, resolver_context, "glBufferData"),
      .buffer_sub_data =
          load<GlBufferSubData>(resolver, resolver_context, "glBufferSubData"),
      .delete_buffers =
          load<GlDeleteBuffers>(resolver, resolver_context, "glDeleteBuffers"),
      .get_error = load<GlGetError>(resolver, resolver_context, "glGetError"),
      .create_shader =
          load<GlCreateShader>(resolver, resolver_context, "glCreateShader"),
      .shader_source =
          load<GlShaderSource>(resolver, resolver_context, "glShaderSource"),
      .compile_shader =
          load<GlCompileShader>(resolver, resolver_context, "glCompileShader"),
      .get_shader_iv =
          load<GlGetShaderiv>(resolver, resolver_context, "glGetShaderiv"),
      .delete_shader =
          load<GlDeleteShader>(resolver, resolver_context, "glDeleteShader"),
      .create_program =
          load<GlCreateProgram>(resolver, resolver_context, "glCreateProgram"),
      .attach_shader =
          load<GlAttachShader>(resolver, resolver_context, "glAttachShader"),
      .link_program =
          load<GlLinkProgram>(resolver, resolver_context, "glLinkProgram"),
      .get_program_iv =
          load<GlGetProgramiv>(resolver, resolver_context, "glGetProgramiv"),
      .delete_program =
          load<GlDeleteProgram>(resolver, resolver_context, "glDeleteProgram"),
      .use_program =
          load<GlUseProgram>(resolver, resolver_context, "glUseProgram"),
      .enable_vertex_attrib_array = load<GlEnableVertexAttribArray>(
          resolver, resolver_context, "glEnableVertexAttribArray"),
      .vertex_attrib_pointer = load<GlVertexAttribPointer>(
          resolver, resolver_context, "glVertexAttribPointer"),
      .get_uniform_location = load<GlGetUniformLocation>(
          resolver, resolver_context, "glGetUniformLocation"),
      .uniform_1f =
          load<GlUniform1f>(resolver, resolver_context, "glUniform1f"),
      .uniform_2f =
          load<GlUniform2f>(resolver, resolver_context, "glUniform2f"),
      .uniform_4f =
          load<GlUniform4f>(resolver, resolver_context, "glUniform4f"),
      .bind_framebuffer = load<GlBindFramebuffer>(resolver, resolver_context,
                                                  "glBindFramebuffer"),
      .viewport = load<GlViewport>(resolver, resolver_context, "glViewport"),
      .clear_color =
          load<GlClearColor>(resolver, resolver_context, "glClearColor"),
      .clear_stencil =
          load<GlClearStencil>(resolver, resolver_context, "glClearStencil"),
      .clear = load<GlClear>(resolver, resolver_context, "glClear"),
      .enable = load<GlEnable>(resolver, resolver_context, "glEnable"),
      .disable = load<GlDisable>(resolver, resolver_context, "glDisable"),
      .blend_func_separate = load<GlBlendFuncSeparate>(
          resolver, resolver_context, "glBlendFuncSeparate"),
      .color_mask =
          load<GlColorMask>(resolver, resolver_context, "glColorMask"),
      .stencil_mask =
          load<GlStencilMask>(resolver, resolver_context, "glStencilMask"),
      .scissor = load<GlScissor>(resolver, resolver_context, "glScissor"),
      .draw_arrays =
          load<GlDrawArrays>(resolver, resolver_context, "glDrawArrays"),
      .gen_textures =
          load<GlGenTextures>(resolver, resolver_context, "glGenTextures"),
      .delete_textures = load<GlDeleteTextures>(resolver, resolver_context,
                                                "glDeleteTextures"),
      .bind_texture =
          load<GlBindTexture>(resolver, resolver_context, "glBindTexture"),
      .tex_image_2d =
          load<GlTexImage2D>(resolver, resolver_context, "glTexImage2D"),
      .tex_parameteri =
          load<GlTexParameter>(resolver, resolver_context, "glTexParameteri"),
      .active_texture =
          load<GlActiveTexture>(resolver, resolver_context, "glActiveTexture"),
      .pixel_storei =
          load<GlPixelStore>(resolver, resolver_context, "glPixelStorei"),
      .uniform_1i =
          load<GlUniform1i>(resolver, resolver_context, "glUniform1i"),
  };
}

constexpr std::string_view vertex_shader_source = R"(#version 330 core
layout(location = 0) in vec4 endpoints;
layout(location = 1) in vec2 corner;
uniform vec2 viewportPixels;
uniform float viewportCenter;
uniform float viewportHalfSpan;
uniform float halfWidthPixels;
uniform float sceneWidthMm;
uniform float horizontalLeftMm;
uniform float horizontalSpanMm;

vec2 mapPoint(vec2 pointValue) {
    float xMm = pointValue.x * sceneWidthMm;
    float x = (xMm - horizontalLeftMm) / horizontalSpanMm * 2.0 - 1.0;
    float y = -(pointValue.y - viewportCenter) / viewportHalfSpan;
    return vec2(x, y);
}

void main() {
    vec2 first = mapPoint(endpoints.xy);
    vec2 second = mapPoint(endpoints.zw);
    vec2 deltaPixels = (second - first) * viewportPixels * 0.5;
    float segmentLength = max(length(deltaPixels), 0.0001);
    vec2 normalPixels =
        vec2(-deltaPixels.y, deltaPixels.x) / segmentLength;
    vec2 position = mix(first, second, corner.x);
    position += normalPixels * corner.y * halfWidthPixels *
                2.0 / viewportPixels;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

constexpr std::string_view fragment_shader_source = R"(#version 330 core
uniform vec4 curveColor;
out vec4 fragmentColor;

void main() {
    fragmentColor = curveColor;
}
)";

// Scene-millimetre transform shared by interval, symbol and glyph passes.
// mmScale/mmOffset map scene millimetres into NDC: x spans the physical
// scene width; y maps through the depth range and current viewport.
constexpr std::string_view scene_vertex_shader_source = R"(#version 330 core
layout(location = 0) in vec2 sceneMm;
layout(location = 1) in vec2 textureUv;
uniform vec2 mmScale;
uniform vec2 mmOffset;
out vec2 vUv;
out vec2 vMm;

void main() {
    gl_Position = vec4(sceneMm * mmScale + mmOffset, 0.0, 1.0);
    vUv = textureUv;
    vMm = sceneMm;
}
)";

constexpr std::string_view solid_fragment_shader_source = R"(#version 330 core
uniform vec4 fillColor;
out vec4 fragmentColor;

void main() {
    fragmentColor = fillColor;
}
)";

// Pattern tiles repeat around a scene anchor (rotated like the vector
// export's patternTransform) so intervals and scrolling share phase.
constexpr std::string_view pattern_fragment_shader_source = R"(#version 330 core
uniform sampler2D tileAtlas;
uniform vec2 anchorMm;
uniform vec2 tileMm;
uniform vec2 rotationCosSin;
uniform vec4 atlasUv;
in vec2 vMm;
out vec4 fragmentColor;

void main() {
    vec2 relative = vMm - anchorMm;
    vec2 rotated = vec2(
        relative.x * rotationCosSin.x + relative.y * rotationCosSin.y,
        -relative.x * rotationCosSin.y + relative.y * rotationCosSin.x);
    vec2 tile = fract(rotated / tileMm);
    fragmentColor = texture(tileAtlas, atlasUv.xy + tile * atlasUv.zw);
}
)";

// Samples one decoded image tile directly via per-vertex UVs (0..1 over the
// tile quad). The tile texture is uploaded per visible tile by the host
// resolver; the engine never decodes (ADR 0042).
constexpr std::string_view image_fragment_shader_source = R"(#version 330 core
uniform sampler2D imageTexture;
uniform bool swapRedAlpha;
in vec2 vUv;
out vec4 fragmentColor;

void main() {
    vec4 sampled = texture(imageTexture, vUv);
    fragmentColor = swapRedAlpha ? vec4(sampled.aaa, sampled.r) : sampled;
}
)";

constexpr std::string_view glyph_fragment_shader_source = R"(#version 330 core
uniform sampler2D glyphAtlas;
uniform vec4 fillColor;
in vec2 vUv;
out vec4 fragmentColor;

void main() {
    float coverage = texture(glyphAtlas, vUv).r;
    fragmentColor = vec4(fillColor.rgb, fillColor.a * coverage);
}
)";

[[nodiscard]] GlUInt compile_shader(const GlFunctions &gl, GlEnum type,
                                    std::string_view source) noexcept {
  const auto shader = gl.create_shader(type);
  if (shader == 0) {
    return 0;
  }
  const auto *source_pointer = source.data();
  const auto source_length = static_cast<GlInt>(source.size());
  gl.shader_source(shader, 1, &source_pointer, &source_length);
  gl.compile_shader(shader);
  GlInt status{};
  gl.get_shader_iv(shader, gl_compile_status, &status);
  if (status == 0) {
    gl.delete_shader(shader);
    return 0;
  }
  return shader;
}

[[nodiscard]] GlUInt build_program(const GlFunctions &gl,
                                   std::string_view vertex_source,
                                   std::string_view fragment_source) noexcept {
  const auto vertex = compile_shader(gl, gl_vertex_shader, vertex_source);
  const auto fragment =
      compile_shader(gl, gl_fragment_shader, fragment_source);
  if (vertex == 0 || fragment == 0) {
    if (vertex != 0) {
      gl.delete_shader(vertex);
    }
    if (fragment != 0) {
      gl.delete_shader(fragment);
    }
    return 0;
  }
  const auto program = gl.create_program();
  if (program == 0) {
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    return 0;
  }
  gl.attach_shader(program, vertex);
  gl.attach_shader(program, fragment);
  gl.link_program(program);
  gl.delete_shader(vertex);
  gl.delete_shader(fragment);
  GlInt linked{};
  gl.get_program_iv(program, gl_link_status, &linked);
  if (linked == 0) {
    gl.delete_program(program);
    return 0;
  }
  return program;
}

struct CurveVertex {
  GlFloat first_left{};
  GlFloat first_depth{};
  GlFloat second_left{};
  GlFloat second_depth{};
  GlFloat along{};
  GlFloat side{};
};

static_assert(sizeof(CurveVertex) == 6 * sizeof(float));

struct CurveBatch {
  GlInt first_vertex{};
  GlSize vertex_count{};
  RgbaColor color;
  Millimetres line_width;
  PhysicalRect clip;
};

struct CurveEdge {
  std::uint64_t first_point{};
  std::uint64_t second_point{};
};

struct PrimitiveVertex {
  GlFloat scene_left{};
  GlFloat scene_top{};
  GlFloat uv_u{};
  GlFloat uv_v{};
};

static_assert(sizeof(PrimitiveVertex) == 4 * sizeof(float));

enum class PrimitiveKind : std::uint8_t {
  solid,
  pattern,
  glyph,
  image,
};

struct PrimitiveBatch {
  PrimitiveKind kind{PrimitiveKind::solid};
  GlInt first_vertex{};
  GlSize vertex_count{};
  RgbaColor color;
  PhysicalRect clip{};
  double anchor_left{};
  double anchor_top{};
  double tile_width{1.0};
  double tile_height{1.0};
  double rotation_cos{1.0};
  double rotation_sin{};
  GlFloat atlas_u{};
  GlFloat atlas_v{};
  GlFloat atlas_du{};
  GlFloat atlas_dv{};
  // For image batches: the GL texture name of the decoded tile (0 = none).
  GlUInt image_texture{};
  // For image batches: 1 when the source pixel format is single-channel
  // (coverage in red); the shader packs it into alpha for compositing.
  GlInt image_single_channel{};
};

// Cache key for one decoded image tile (ADR 0032 identity + pyramid coords).
struct ImageTextureKey {
  EntityId image_source_id;
  std::uint32_t level{};
  std::uint32_t row{};
  std::uint32_t col{};
  bool operator==(const ImageTextureKey &) const = default;
};

struct ImageTextureKeyHash {
  std::size_t operator()(const ImageTextureKey &key) const noexcept {
    const auto h = EntityIdHash{}(key.image_source_id);
    return h ^ (std::hash<std::uint32_t>{}(key.level) << 1U) ^
           (std::hash<std::uint32_t>{}(key.row) << 2U) ^
           (std::hash<std::uint32_t>{}(key.col) << 3U);
  }
};

// One cached GPU texture for an image tile, with its byte cost + a recency
// stamp for LRU eviction (ADR 0034).
struct ImageTextureEntry {
  GlUInt texture{};
  std::uint64_t byte_size{};
  std::uint64_t last_used{};
};

[[nodiscard]] constexpr std::uint64_t
glyph_atlas_key(std::uint32_t font_index, std::uint32_t glyph_id) {
  return (static_cast<std::uint64_t>(font_index) << 32U) | glyph_id;
}

void append_quad(std::vector<PrimitiveVertex> &vertices, double left,
                 double top, double right, double bottom, float u0, float v0,
                 float u1, float v1) {
  const std::array<PrimitiveVertex, 6> corners{{
      {static_cast<GlFloat>(left), static_cast<GlFloat>(top), u0, v0},
      {static_cast<GlFloat>(left), static_cast<GlFloat>(bottom), u0, v1},
      {static_cast<GlFloat>(right), static_cast<GlFloat>(bottom), u1, v1},
      {static_cast<GlFloat>(left), static_cast<GlFloat>(top), u0, v0},
      {static_cast<GlFloat>(right), static_cast<GlFloat>(bottom), u1, v1},
      {static_cast<GlFloat>(right), static_cast<GlFloat>(top), u1, v0},
  }};
  vertices.insert(vertices.end(), corners.begin(), corners.end());
}

void append_triangle(std::vector<PrimitiveVertex> &vertices, double ax,
                     double ay, double bx, double by, double cx, double cy) {
  vertices.push_back(PrimitiveVertex{static_cast<GlFloat>(ax),
                                     static_cast<GlFloat>(ay), 0.0F, 0.0F});
  vertices.push_back(PrimitiveVertex{static_cast<GlFloat>(bx),
                                     static_cast<GlFloat>(by), 0.0F, 0.0F});
  vertices.push_back(PrimitiveVertex{static_cast<GlFloat>(cx),
                                     static_cast<GlFloat>(cy), 0.0F, 0.0F});
}

void append_symbol_geometry(std::vector<PrimitiveVertex> &vertices,
                            const PreparedSymbol &symbol, double half_size) {
  const auto cx = symbol.center.left.value;
  const auto cy = symbol.center.top.value;
  switch (symbol.kind) {
  case SymbolKind::circle: {
    constexpr auto segments = 24;
    for (auto index = 0; index < segments; ++index) {
      const auto first_angle =
          2.0 * 3.14159265358979323846 * static_cast<double>(index) /
          segments;
      const auto second_angle =
          2.0 * 3.14159265358979323846 * static_cast<double>(index + 1) /
          segments;
      append_triangle(vertices, cx, cy,
                      cx + half_size * std::cos(first_angle),
                      cy + half_size * std::sin(first_angle),
                      cx + half_size * std::cos(second_angle),
                      cy + half_size * std::sin(second_angle));
    }
    return;
  }
  case SymbolKind::square:
    append_quad(vertices, cx - half_size, cy - half_size, cx + half_size,
                cy + half_size, 0.0F, 0.0F, 0.0F, 0.0F);
    return;
  case SymbolKind::triangle_up:
    append_triangle(vertices, cx, cy - half_size, cx + half_size,
                    cy + half_size, cx - half_size, cy + half_size);
    return;
  case SymbolKind::triangle_down:
    // Inverted triangle: apex at the bottom, base at the top (matches
    // scene::symbol_glyph for MarkerSemantic::formation_top).
    append_triangle(vertices, cx, cy + half_size, cx + half_size,
                    cy - half_size, cx - half_size, cy - half_size);
    return;
  case SymbolKind::shoe: {
    // Casing-shoe arch: flat side up, bulge down (scene y-down), matching the
    // scene::symbol_glyph half-circle outline.
    constexpr auto segments = 16;
    for (auto index = 0; index < segments; ++index) {
      const auto first_angle =
          3.14159265358979323846 * static_cast<double>(index) / segments;
      const auto second_angle =
          3.14159265358979323846 * static_cast<double>(index + 1) / segments;
      append_triangle(vertices, cx, cy,
                      cx + half_size * std::cos(first_angle),
                      cy + half_size * std::sin(first_angle),
                      cx + half_size * std::cos(second_angle),
                      cy + half_size * std::sin(second_angle));
    }
    return;
  }
  case SymbolKind::diamond:
    append_triangle(vertices, cx, cy - half_size, cx + half_size, cy, cx,
                    cy + half_size);
    append_triangle(vertices, cx, cy - half_size, cx, cy + half_size,
                    cx - half_size, cy);
    return;
  case SymbolKind::cross: {
    const auto thickness = half_size / 3.0;
    const auto diagonal = half_size;
    // Two thin rotated quads.
    const auto dx = diagonal;
    const auto tx = thickness;
    append_triangle(vertices, cx - dx + tx, cy - dx - tx, cx + dx + tx,
                    cy + dx - tx, cx + dx - tx, cy + dx + tx);
    append_triangle(vertices, cx - dx + tx, cy - dx - tx, cx - dx - tx,
                    cy - dx + tx, cx + dx - tx, cy + dx + tx);
    append_triangle(vertices, cx - dx - tx, cy + dx - tx, cx + dx - tx,
                    cy - dx - tx, cx + dx + tx, cy - dx + tx);
    append_triangle(vertices, cx - dx - tx, cy + dx - tx, cx + dx + tx,
                    cy - dx + tx, cx - dx + tx, cy + dx + tx);
    return;
  }
  }
}

// Rotates an em-space glyph-local point (y-up) into scene millimetres
// around the glyph origin.
[[nodiscard]] std::pair<double, double>
glyph_corner(double em_x, double em_y, double font_size, double rotation_cos,
             double rotation_sin, double origin_left, double origin_top) {
  const auto local_x = em_x * font_size;
  const auto local_y = -em_y * font_size; // em is y-up, scene is y-down
  return {origin_left + local_x * rotation_cos - local_y * rotation_sin,
          origin_top + local_x * rotation_sin + local_y * rotation_cos};
}

void clear_gl_errors(const GlFunctions &gl) noexcept {
  constexpr auto maximum_stale_errors = 16;
  for (auto count = 0; count < maximum_stale_errors; ++count) {
    if (gl.get_error() == gl_no_error) {
      return;
    }
  }
}

void append_segment_vertices(std::vector<CurveVertex> &vertices,
                             const PreparedCurvePoint &first,
                             const PreparedCurvePoint &second,
                             double physical_width, double scene_depth_center) {
  const auto first_left =
      static_cast<GlFloat>(first.position.left.value / physical_width);
  const auto second_left =
      static_cast<GlFloat>(second.position.left.value / physical_width);
  const auto first_depth =
      static_cast<GlFloat>(first.reference_depth - scene_depth_center);
  const auto second_depth =
      static_cast<GlFloat>(second.reference_depth - scene_depth_center);
  constexpr std::array<std::array<GlFloat, 2>, 6> corners{{
      {0.0F, -1.0F},
      {0.0F, 1.0F},
      {1.0F, -1.0F},
      {1.0F, -1.0F},
      {0.0F, 1.0F},
      {1.0F, 1.0F},
  }};
  for (const auto &corner : corners) {
    vertices.push_back(CurveVertex{
        .first_left = first_left,
        .first_depth = first_depth,
        .second_left = second_left,
        .second_depth = second_depth,
        .along = corner[0],
        .side = corner[1],
    });
  }
}

} // namespace

struct GlRenderer::Impl {
  struct BufferSlot {
    GlUInt vertex_array{};
    GlUInt vertex_buffer{};
  };

  GlFunctions gl;
  std::thread::id owner_thread;
  std::array<BufferSlot, 2> buffers;
  std::size_t active_buffer{};
  std::size_t staging_buffer{1};
  GlUInt program{};
  GlInt viewport_pixels_uniform{-1};
  GlInt viewport_center_uniform{-1};
  GlInt viewport_half_span_uniform{-1};
  GlInt half_width_uniform{-1};
  GlInt scene_width_uniform{-1};
  GlInt horizontal_left_uniform{-1};
  GlInt horizontal_span_uniform{-1};
  GlInt color_uniform{-1};
  GlUInt solid_program{};
  GlUInt pattern_program{};
  GlUInt glyph_program{};
  GlInt solid_mm_scale_uniform{-1};
  GlInt solid_mm_offset_uniform{-1};
  GlInt solid_color_uniform{-1};
  GlInt pattern_mm_scale_uniform{-1};
  GlInt pattern_mm_offset_uniform{-1};
  GlInt pattern_atlas_uniform{-1};
  GlInt pattern_anchor_uniform{-1};
  GlInt pattern_tile_uniform{-1};
  GlInt pattern_rotation_uniform{-1};
  GlInt pattern_atlas_uv_uniform{-1};
  GlInt glyph_mm_scale_uniform{-1};
  GlInt glyph_mm_offset_uniform{-1};
  GlInt glyph_atlas_uniform{-1};
  GlInt glyph_color_uniform{-1};
  GlUInt image_program{};
  GlInt image_mm_scale_uniform{-1};
  GlInt image_mm_offset_uniform{-1};
  GlInt image_texture_uniform{-1};
  GlInt image_swap_red_alpha_uniform{-1};
  BufferSlot primitives;
  GlUInt pattern_texture{};
  GlUInt glyph_texture{};
  // Bounded cache of per-tile image textures, keyed by source/level/row/col.
  // Survives context loss by being cleared on release(); tiles are re-uploaded
  // from prepared metadata + host resolver bytes on the next frame.
  std::unordered_map<ImageTextureKey, ImageTextureEntry, ImageTextureKeyHash>
      image_textures;
  std::uint64_t image_texture_bytes{};
  std::uint64_t maximum_image_texture_bytes{256ULL * 1024ULL * 1024ULL};
  std::function<Result<RasterTile>(const ImageTileRequest &)> image_resolver;
  std::uint64_t frame_stamp{};
  double physical_width{};
  double scene_height{};
  double depth_top{};
  double depth_span{1.0};
  double scene_depth_center{};
  std::uint64_t active_bytes{};
  std::vector<CurveBatch> batches;
  std::vector<PrimitiveBatch> primitive_batches;
  PreparedScene pending_scene;
  std::vector<CurveEdge> pending_edges;
  std::vector<CurveBatch> pending_batches;
  std::vector<PrimitiveVertex> pending_primitive_vertices;
  std::vector<PrimitiveBatch> pending_primitive_batches;
  std::shared_ptr<RasterImage> pending_pattern_atlas;
  std::shared_ptr<RasterImage> pending_glyph_atlas;
  std::string pending_atlas_fingerprint;
  bool pending_atlases_need_upload{true};
  // Atlas reuse cache (issue #463): every queue_upload rebuilt both atlases
  // from scratch although the pattern/glyph sets barely change between
  // viewport moves. A fingerprint over the scene's pattern ids and glyph
  // outline keys reuses the previous bitmaps and uv tables on a hit.
  struct GlyphAtlasEntry {
    float u0{};
    float v0{};
    float u1{};
    float v1{};
    double left_em{};
    double top_em{};
    double pixels_per_em{1.0};
    std::uint32_t pixel_width{};
    std::uint32_t pixel_height{};
  };
  std::string atlas_fingerprint;
  std::optional<std::string> uploaded_atlas_fingerprint;
  std::shared_ptr<RasterImage> cached_pattern_atlas;
  std::shared_ptr<RasterImage> cached_glyph_atlas;
  std::unordered_map<EntityId, std::array<float, 4>, EntityIdHash>
      cached_pattern_uvs;
  std::unordered_map<std::uint64_t, GlyphAtlasEntry> cached_glyph_entries;
  double pending_scene_height{};
  double pending_depth_top{};
  double pending_depth_span{1.0};
  std::vector<GpuUploadChunk> pending_chunks;
  std::size_t next_pending_chunk{};
  std::uint64_t pending_total_bytes{};
  double pending_physical_width{};
  double pending_scene_depth_center{};
  std::uint64_t pending_bytes_uploaded{};
  bool pending_buffer_allocated{};
  bool drop_active_before_upload{};
  bool upload_pending{};
};

GlRenderer::GlRenderer() : impl_(std::make_unique<Impl>()) {}
GlRenderer::~GlRenderer() = default;
GlRenderer::GlRenderer(GlRenderer &&) noexcept = default;
GlRenderer &GlRenderer::operator=(GlRenderer &&) noexcept = default;

bool GlRenderer::initialize(GlProcResolver resolver,
                            void *resolver_context) noexcept {
  if (resolver == nullptr) {
    return false;
  }
  try {
    impl_->gl = load_functions(resolver, resolver_context);
    if (!impl_->gl.complete()) {
      return false;
    }
    impl_->program =
        build_program(impl_->gl, vertex_shader_source, fragment_shader_source);
    impl_->solid_program = build_program(impl_->gl, scene_vertex_shader_source,
                                         solid_fragment_shader_source);
    impl_->pattern_program =
        build_program(impl_->gl, scene_vertex_shader_source,
                      pattern_fragment_shader_source);
    impl_->glyph_program = build_program(impl_->gl, scene_vertex_shader_source,
                                         glyph_fragment_shader_source);
    impl_->image_program = build_program(impl_->gl, scene_vertex_shader_source,
                                         image_fragment_shader_source);
    if (impl_->program == 0 || impl_->solid_program == 0 ||
        impl_->pattern_program == 0 || impl_->glyph_program == 0 ||
        impl_->image_program == 0) {
      release();
      return false;
    }
    impl_->image_mm_scale_uniform = impl_->gl.get_uniform_location(
        impl_->image_program, "mmScale");
    impl_->image_mm_offset_uniform = impl_->gl.get_uniform_location(
        impl_->image_program, "mmOffset");
    impl_->image_texture_uniform = impl_->gl.get_uniform_location(
        impl_->image_program, "imageTexture");
    impl_->image_swap_red_alpha_uniform = impl_->gl.get_uniform_location(
        impl_->image_program, "swapRedAlpha");

    impl_->owner_thread = std::this_thread::get_id();
    for (auto &buffer : impl_->buffers) {
      impl_->gl.gen_vertex_arrays(1, &buffer.vertex_array);
      impl_->gl.gen_buffers(1, &buffer.vertex_buffer);
      if (buffer.vertex_array == 0 || buffer.vertex_buffer == 0) {
        release();
        return false;
      }
      impl_->gl.bind_vertex_array(buffer.vertex_array);
      impl_->gl.bind_buffer(gl_array_buffer, buffer.vertex_buffer);
      impl_->gl.enable_vertex_attrib_array(0);
      impl_->gl.vertex_attrib_pointer(
          0, 4, gl_float, static_cast<GlBoolean>(gl_false),
          static_cast<GlSize>(sizeof(CurveVertex)), nullptr);
      impl_->gl.enable_vertex_attrib_array(1);
      impl_->gl.vertex_attrib_pointer(
          1, 2, gl_float, static_cast<GlBoolean>(gl_false),
          static_cast<GlSize>(sizeof(CurveVertex)),
          reinterpret_cast<const void *>(offsetof(CurveVertex, along)));
    }
    impl_->viewport_pixels_uniform =
        impl_->gl.get_uniform_location(impl_->program, "viewportPixels");
    impl_->viewport_center_uniform =
        impl_->gl.get_uniform_location(impl_->program, "viewportCenter");
    impl_->viewport_half_span_uniform =
        impl_->gl.get_uniform_location(impl_->program, "viewportHalfSpan");
    impl_->half_width_uniform =
        impl_->gl.get_uniform_location(impl_->program, "halfWidthPixels");
    impl_->scene_width_uniform =
        impl_->gl.get_uniform_location(impl_->program, "sceneWidthMm");
    impl_->horizontal_left_uniform =
        impl_->gl.get_uniform_location(impl_->program, "horizontalLeftMm");
    impl_->horizontal_span_uniform =
        impl_->gl.get_uniform_location(impl_->program, "horizontalSpanMm");
    impl_->color_uniform =
        impl_->gl.get_uniform_location(impl_->program, "curveColor");

    const auto locate = [&](GlUInt program, const char *name) {
      return impl_->gl.get_uniform_location(program, name);
    };
    impl_->solid_mm_scale_uniform = locate(impl_->solid_program, "mmScale");
    impl_->solid_mm_offset_uniform = locate(impl_->solid_program, "mmOffset");
    impl_->solid_color_uniform = locate(impl_->solid_program, "fillColor");
    impl_->pattern_mm_scale_uniform =
        locate(impl_->pattern_program, "mmScale");
    impl_->pattern_mm_offset_uniform =
        locate(impl_->pattern_program, "mmOffset");
    impl_->pattern_atlas_uniform =
        locate(impl_->pattern_program, "tileAtlas");
    impl_->pattern_anchor_uniform =
        locate(impl_->pattern_program, "anchorMm");
    impl_->pattern_tile_uniform = locate(impl_->pattern_program, "tileMm");
    impl_->pattern_rotation_uniform =
        locate(impl_->pattern_program, "rotationCosSin");
    impl_->pattern_atlas_uv_uniform =
        locate(impl_->pattern_program, "atlasUv");
    impl_->glyph_mm_scale_uniform = locate(impl_->glyph_program, "mmScale");
    impl_->glyph_mm_offset_uniform = locate(impl_->glyph_program, "mmOffset");
    impl_->glyph_atlas_uniform = locate(impl_->glyph_program, "glyphAtlas");
    impl_->glyph_color_uniform = locate(impl_->glyph_program, "fillColor");

    impl_->gl.gen_vertex_arrays(1, &impl_->primitives.vertex_array);
    impl_->gl.gen_buffers(1, &impl_->primitives.vertex_buffer);
    impl_->gl.gen_textures(1, &impl_->pattern_texture);
    impl_->gl.gen_textures(1, &impl_->glyph_texture);
    if (impl_->primitives.vertex_array == 0 ||
        impl_->primitives.vertex_buffer == 0 || impl_->pattern_texture == 0 ||
        impl_->glyph_texture == 0) {
      release();
      return false;
    }
    impl_->gl.bind_vertex_array(impl_->primitives.vertex_array);
    impl_->gl.bind_buffer(gl_array_buffer, impl_->primitives.vertex_buffer);
    impl_->gl.enable_vertex_attrib_array(0);
    impl_->gl.vertex_attrib_pointer(
        0, 2, gl_float, static_cast<GlBoolean>(gl_false),
        static_cast<GlSize>(sizeof(PrimitiveVertex)), nullptr);
    impl_->gl.enable_vertex_attrib_array(1);
    impl_->gl.vertex_attrib_pointer(
        1, 2, gl_float, static_cast<GlBoolean>(gl_false),
        static_cast<GlSize>(sizeof(PrimitiveVertex)),
        reinterpret_cast<const void *>(offsetof(PrimitiveVertex, uv_u)));
    impl_->gl.pixel_storei(gl_unpack_alignment, 1);
    for (const auto texture : {impl_->pattern_texture, impl_->glyph_texture}) {
      impl_->gl.bind_texture(gl_texture_2d, texture);
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_min_filter,
                               static_cast<GlInt>(gl_linear));
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_mag_filter,
                               static_cast<GlInt>(gl_linear));
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_wrap_s,
                               static_cast<GlInt>(gl_clamp_to_edge));
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_wrap_t,
                               static_cast<GlInt>(gl_clamp_to_edge));
    }
    return initialized();
  } catch (...) {
    abandon();
    return false;
  }
}

bool GlRenderer::upload(const PreparedScene &scene) noexcept {
  if (!queue_upload(scene, GpuUploadBudgets{
                               .maximum_cache_bytes =
                                   std::numeric_limits<std::uint64_t>::max(),
                               .maximum_bytes_per_frame =
                                   std::numeric_limits<std::uint64_t>::max(),
                           })) {
    return false;
  }
  while (true) {
    const auto progress = upload_next();
    if (progress.completed) {
      return true;
    }
    if (!progress.pending) {
      return false;
    }
  }
}

bool GlRenderer::queue_upload(const PreparedScene &scene,
                              GpuUploadBudgets budgets) noexcept {
  if (!initialized() || std::this_thread::get_id() != impl_->owner_thread ||
      scene.physical_width().value <= 0.0) {
    return false;
  }
  try {
    auto schedule = GpuUploadSchedule::plan(scene, budgets);
    if (!schedule.has_value()) {
      return false;
    }
    std::vector<CurveEdge> edges;
    std::vector<CurveBatch> batches;
    const auto depth_range = scene.reference_depth_range();
    const auto scene_depth_center =
        depth_range.top + (depth_range.bottom - depth_range.top) * 0.5;
    for (const auto &layer : scene.curve_layers()) {
      const auto first_edge = edges.size();
      for (std::uint64_t segment_offset = 0;
           segment_offset < layer.segment_count; ++segment_offset) {
        const auto &segment = scene.curve_segments()[static_cast<std::size_t>(
            layer.first_segment + segment_offset)];
        for (std::uint64_t point_offset = 1; point_offset < segment.point_count;
             ++point_offset) {
          const auto first_point =
              static_cast<std::size_t>(segment.first_point + point_offset - 1);
          const auto second_point =
              static_cast<std::size_t>(segment.first_point + point_offset);
          edges.push_back(CurveEdge{
              .first_point = static_cast<std::uint64_t>(first_point),
              .second_point = static_cast<std::uint64_t>(second_point),
          });
        }
      }
      const auto track =
          std::find_if(scene.tracks().begin(), scene.tracks().end(),
                       [&](const PreparedTrack &candidate) {
                         return candidate.id == layer.track_id;
                       });
      if (track != scene.tracks().end() && edges.size() > first_edge) {
        const auto first_vertex = first_edge * std::size_t{6};
        const auto count = (edges.size() - first_edge) * std::size_t{6};
        if (first_vertex >
                static_cast<std::size_t>(std::numeric_limits<GlInt>::max()) ||
            count >
                static_cast<std::size_t>(std::numeric_limits<GlSize>::max())) {
          return false;
        }
        batches.push_back(CurveBatch{
            .first_vertex = static_cast<GlInt>(first_vertex),
            .vertex_count = static_cast<GlSize>(count),
            .color = layer.color,
            .line_width = layer.line_width,
            .clip = track->clip,
        });
      }
    }
    if (edges.size() >
        static_cast<std::size_t>(
            std::numeric_limits<GlSizePointer>::max() /
            static_cast<GlSizePointer>(6 * sizeof(CurveVertex)))) {
      return false;
    }
    const auto byte_count = edges.size() * 6 * sizeof(CurveVertex);
    if (byte_count != schedule.value().total_bytes()) {
      return false;
    }

    // Interval, marker, symbol and text passes consume the same prepared
    // scene. Pattern tiles and glyph outlines rasterize into atlases once
    // per upload; tiling repeats in the shader around the scene anchor.
    // 2048^2 at 128 px/em holds ~256 full-resolution CJK glyphs (the old
    // 1024^2 atlas held ~64 and silently dropped every glyph past that —
    // issue #464); glyphs that still do not fit re-rasterize at half
    // resolution before giving up.
    constexpr std::uint32_t atlas_extent = 2048;
    constexpr double pattern_pixels_per_millimetre = 16.0;
    constexpr double glyph_pixels_per_em = 128.0;
    std::vector<PrimitiveVertex> primitive_vertices;
    std::vector<PrimitiveBatch> primitive_batches;
    ShelfAtlasPacker pattern_packer(atlas_extent, atlas_extent);
    ShelfAtlasPacker glyph_packer(atlas_extent, atlas_extent);
    std::unordered_map<EntityId, std::array<float, 4>, EntityIdHash>
        pattern_uvs;
    std::unordered_map<std::uint64_t, Impl::GlyphAtlasEntry> glyph_atlas_entries;
    // Fingerprint the atlas INPUTS: same pattern set and same glyph outline
    // set (in the same order the packer sees) produce identical atlases, so
    // a hit skips both rasterization passes and reuses the uv tables
    // (issue #463).
    std::string fingerprint;
    fingerprint.reserve(16 * (scene.patterns().size() + 1));
    // Content fingerprint, not just ids (#855): two patterns with the same id
    // but different content (colour, width, primitives, tile size, version)
    // must produce different atlases, and a font swap must invalidate cached
    // glyph bitmaps — otherwise an in-place pattern/font change reuses stale
    // texture pixels until some unrelated id/glyph set changes (ADR 0020
    // "PatternDefinition is the single vector source of truth" was silently
    // violated for the GL backend).
    for (const auto &pattern : scene.patterns()) {
      fingerprint += "p:";
      fingerprint += pattern.id.to_string();
      fingerprint += ":";
      append_fingerprint_number(fingerprint, pattern.tile_width.value);
      fingerprint.push_back('x');
      append_fingerprint_number(fingerprint, pattern.tile_height.value);
      fingerprint += ":r";
      append_fingerprint_number(fingerprint, pattern.rotation_degrees);
      fingerprint += ":c";
      append_fingerprint_color(fingerprint, pattern.foreground);
      fingerprint.push_back(';');
      append_fingerprint_color(fingerprint, pattern.background);
      fingerprint += ":w";
      append_fingerprint_number(fingerprint, pattern.stroke_width.value);
      fingerprint += ":a";
      append_fingerprint_number(fingerprint, pattern.scene_anchor.left.value);
      fingerprint.push_back(',');
      append_fingerprint_number(fingerprint, pattern.scene_anchor.top.value);
      fingerprint += ":v";
      append_fingerprint_number(fingerprint, static_cast<double>(pattern.version));
      fingerprint.push_back('{');
      for (const auto &primitive : pattern.primitives) {
        append_fingerprint_primitive(fingerprint, primitive);
      }
      fingerprint.push_back('}');
      fingerprint.push_back('|');
    }
    for (const auto &outline : scene.glyph_outlines()) {
      fingerprint += "g:";
      fingerprint += std::to_string(outline.font_index);
      fingerprint += ":";
      fingerprint += std::to_string(outline.glyph_id);
    }
    fingerprint += "|font:";
    fingerprint += scene.font_asset_fingerprint();
    const bool atlas_cache_hit =
        impl_->cached_pattern_atlas != nullptr &&
        !impl_->cached_pattern_atlas->pixels.empty() &&
        impl_->atlas_fingerprint == fingerprint;
    std::shared_ptr<RasterImage> pattern_atlas_ptr;
    std::shared_ptr<RasterImage> glyph_atlas_ptr;
    if (atlas_cache_hit) {
      // Share the cached bitmaps — do not value-copy 20 MB (issue #606).
      pattern_atlas_ptr = impl_->cached_pattern_atlas;
      glyph_atlas_ptr = impl_->cached_glyph_atlas;
      pattern_uvs = impl_->cached_pattern_uvs;
      glyph_atlas_entries = impl_->cached_glyph_entries;
    } else {
      pattern_atlas_ptr = std::make_shared<RasterImage>(RasterImage{
          .width = atlas_extent,
          .height = atlas_extent,
          .channels = 4,
          .pixels = std::vector<std::uint8_t>(
              static_cast<std::size_t>(atlas_extent) * atlas_extent * 4, 0),
      });
      glyph_atlas_ptr = std::make_shared<RasterImage>(RasterImage{
          .width = atlas_extent,
          .height = atlas_extent,
          .channels = 1,
          .pixels = std::vector<std::uint8_t>(
              static_cast<std::size_t>(atlas_extent) * atlas_extent, 0),
      });
    }
    RasterImage &pattern_atlas = *pattern_atlas_ptr;
    RasterImage &glyph_atlas = *glyph_atlas_ptr;

    const auto clip_for_track = [&](EntityId track_id) {
      const auto track =
          std::find_if(scene.tracks().begin(), scene.tracks().end(),
                       [&](const PreparedTrack &candidate) {
                         return candidate.id == track_id;
                       });
      return track == scene.tracks().end() ? PhysicalRect{} : track->clip;
    };

    for (const auto &pattern : scene.patterns()) {
      if (atlas_cache_hit) {
        break;
      }
      auto tile =
          rasterize_pattern_tile(pattern, pattern_pixels_per_millimetre);
      const auto rect = pattern_packer.allocate(tile.width, tile.height);
      if (!rect.has_value()) {
        // Atlas exhausted for this tile: the interval falls back to its solid
        // fill_color below. Count the drop instead of silently continuing so a
        // degradation vs. the SVG/PDF backends is observable (issue #855).
        gl_atlas_debug_stats().pattern_tiles_dropped += 1;
        continue;
      }
      for (std::uint32_t row = 0; row < tile.height; ++row) {
        std::memcpy(
            pattern_atlas.pixels.data() +
                (static_cast<std::size_t>(rect->top + row) * atlas_extent +
                 rect->left) *
                    4,
            tile.pixels.data() +
                static_cast<std::size_t>(row) * tile.width * 4,
            static_cast<std::size_t>(tile.width) * 4);
      }
      pattern_uvs.emplace(
          pattern.id,
          std::array<float, 4>{
              static_cast<float>(rect->left) / atlas_extent,
              static_cast<float>(rect->top) / atlas_extent,
              static_cast<float>(rect->width) / atlas_extent,
              static_cast<float>(rect->height) / atlas_extent,
          });
    }
    for (const auto &outline : scene.glyph_outlines()) {
      if (atlas_cache_hit) {
        break;
      }
      auto raster = rasterize_glyph_outline(
          scene.outline_commands().subspan(
              static_cast<std::size_t>(outline.first_command),
              static_cast<std::size_t>(outline.command_count)),
          outline.left, outline.bottom, outline.right, outline.top,
          glyph_pixels_per_em);
      auto rect = glyph_packer.allocate(raster.width, raster.height);
      if (!rect.has_value()) {
        // Atlas exhausted at full resolution: retry the glyph at half
        // resolution instead of silently dropping it (issue #464) — CJK
        // annotation sets of a few hundred distinct glyphs fit entirely at
        // 64 px/em even after the full-resolution region fills.
        raster = rasterize_glyph_outline(
            scene.outline_commands().subspan(
                static_cast<std::size_t>(outline.first_command),
                static_cast<std::size_t>(outline.command_count)),
            outline.left, outline.bottom, outline.right, outline.top,
            glyph_pixels_per_em / 2.0);
        rect = glyph_packer.allocate(raster.width, raster.height);
      }
      if (!rect.has_value()) {
        continue;
      }
      for (std::uint32_t row = 0; row < raster.height; ++row) {
        std::memcpy(glyph_atlas.pixels.data() +
                        static_cast<std::size_t>(rect->top + row) *
                            atlas_extent +
                        rect->left,
                    raster.alpha.data() +
                        static_cast<std::size_t>(row) * raster.width,
                    raster.width);
      }
      glyph_atlas_entries.emplace(
          glyph_atlas_key(outline.font_index, outline.glyph_id),
          Impl::GlyphAtlasEntry{
                   .u0 = static_cast<float>(rect->left) / atlas_extent,
                   .v0 = static_cast<float>(rect->top) / atlas_extent,
                   .u1 = static_cast<float>(rect->left + rect->width) /
                         atlas_extent,
                   .v1 = static_cast<float>(rect->top + rect->height) /
                         atlas_extent,
                   .left_em = raster.left_em,
                   .top_em = raster.top_em,
                   .pixels_per_em = raster.pixels_per_em,
                   .pixel_width = raster.width,
                   .pixel_height = raster.height,
               });
    }

    const auto append_solid_quad = [&](const PhysicalRect &rect,
                                       RgbaColor color,
                                       const PhysicalRect &clip) {
      const auto first_vertex = primitive_vertices.size();
      append_quad(primitive_vertices, rect.left.value, rect.top.value,
                  rect.left.value + rect.width.value,
                  rect.top.value + rect.height.value, 0.0F, 0.0F, 0.0F,
                  0.0F);
      primitive_batches.push_back(PrimitiveBatch{
          .kind = PrimitiveKind::solid,
          .first_vertex = static_cast<GlInt>(first_vertex),
          .vertex_count = static_cast<GlSize>(6),
          .color = color,
          .clip = clip,
      });
    };

    for (const auto &layer : scene.interval_layers()) {
      const auto clip = clip_for_track(layer.track_id);
      for (std::uint64_t offset = 0; offset < layer.interval_count;
           ++offset) {
        const auto &interval = scene.intervals()[static_cast<std::size_t>(
            layer.first_interval + offset)];
        const auto pattern_uv =
            interval.pattern_id.is_nil()
                ? pattern_uvs.end()
                : pattern_uvs.find(interval.pattern_id);
        if (pattern_uv == pattern_uvs.end()) {
          append_solid_quad(interval.rect, interval.fill_color, clip);
          continue;
        }
        const auto pattern =
            std::find_if(scene.patterns().begin(), scene.patterns().end(),
                         [&](const PatternDefinition &candidate) {
                           return candidate.id == interval.pattern_id;
                         });
        if (pattern == scene.patterns().end()) {
          append_solid_quad(interval.rect, interval.fill_color, clip);
          continue;
        }
        const auto first_vertex = primitive_vertices.size();
        append_quad(primitive_vertices, interval.rect.left.value,
                    interval.rect.top.value,
                    interval.rect.left.value + interval.rect.width.value,
                    interval.rect.top.value + interval.rect.height.value,
                    0.0F, 0.0F, 0.0F, 0.0F);
        const auto theta =
            pattern->rotation_degrees * 3.14159265358979323846 / 180.0;
        const auto &uv = pattern_uv->second;
        primitive_batches.push_back(PrimitiveBatch{
            .kind = PrimitiveKind::pattern,
            .first_vertex = static_cast<GlInt>(first_vertex),
            .vertex_count = static_cast<GlSize>(6),
            .color = {},
            .clip = clip,
            .anchor_left = pattern->scene_anchor.left.value,
            .anchor_top = pattern->scene_anchor.top.value,
            .tile_width = pattern->tile_width.value,
            .tile_height = pattern->tile_height.value,
            .rotation_cos = std::cos(theta),
            .rotation_sin = std::sin(theta),
            .atlas_u = uv[0],
            .atlas_v = uv[1],
            .atlas_du = uv[2],
            .atlas_dv = uv[3],
        });
      }
    }
    // Crossover fill regions (rendering.md section 6). Each region is already
    // triangulated in the prepared scene, so GL emits those triangles into a
    // solid batch (color fill) or a pattern batch (tile fill). The pattern
    // shader derives tile coords from the vertex scene position + anchor, so
    // the triangle UVs are unused (left at 0,0 by append_triangle).
    const auto fill_vertices = scene.fill_vertices();
    const auto triangles = scene.fill_triangles();
    // Emits one region's prepared triangulation into the primitive stream.
    // Shared by the solid and pattern batches — only the batch metadata differs.
    const auto emit_region_triangles = [&](const PreparedFillRegion &region) {
      for (std::uint64_t t = 0; t < region.triangle_count; ++t) {
        const auto &tri =
            triangles[static_cast<std::size_t>(region.first_triangle + t)];
        const auto &a = fill_vertices[tri.a];
        const auto &b = fill_vertices[tri.b];
        const auto &c = fill_vertices[tri.c];
        append_triangle(primitive_vertices, a.position.left.value,
                        a.position.top.value, b.position.left.value,
                        b.position.top.value, c.position.left.value,
                        c.position.top.value);
      }
    };
    for (const auto &fill_layer : scene.fill_layers()) {
      const auto clip = clip_for_track(fill_layer.track_id);
      for (std::uint64_t offset = 0; offset < fill_layer.region_count;
           ++offset) {
        const auto &region = scene.fill_regions()[static_cast<std::size_t>(
            fill_layer.first_region + offset)];
        if (region.triangle_count == 0) {
          continue;
        }
        if (region.pattern_id.is_nil()) {
          const auto first_vertex = primitive_vertices.size();
          emit_region_triangles(region);
          primitive_batches.push_back(PrimitiveBatch{
              .kind = PrimitiveKind::solid,
              .first_vertex = static_cast<GlInt>(first_vertex),
              .vertex_count =
                  static_cast<GlSize>(region.triangle_count * 3),
              .color = region.fill_color,
              .clip = clip,
          });
          continue;
        }
        const auto pattern_uv = pattern_uvs.find(region.pattern_id);
        const auto pattern =
            pattern_uv == pattern_uvs.end()
                ? nullptr
                : std::find_if(
                      scene.patterns().begin(), scene.patterns().end(),
                      [&](const PatternDefinition &candidate) {
                        return candidate.id == region.pattern_id;
                      })
                      .operator->();
        if (pattern_uv == pattern_uvs.end() || pattern == nullptr) {
          continue;
        }
        const auto first_vertex = primitive_vertices.size();
        emit_region_triangles(region);
        const auto theta =
            pattern->rotation_degrees * 3.14159265358979323846 / 180.0;
        const auto &uv = pattern_uv->second;
        primitive_batches.push_back(PrimitiveBatch{
            .kind = PrimitiveKind::pattern,
            .first_vertex = static_cast<GlInt>(first_vertex),
            .vertex_count =
                static_cast<GlSize>(region.triangle_count * 3),
            .color = {},
            .clip = clip,
            .anchor_left = pattern->scene_anchor.left.value,
            .anchor_top = pattern->scene_anchor.top.value,
            .tile_width = pattern->tile_width.value,
            .tile_height = pattern->tile_height.value,
            .rotation_cos = std::cos(theta),
            .rotation_sin = std::sin(theta),
            .atlas_u = uv[0],
            .atlas_v = uv[1],
            .atlas_du = uv[2],
            .atlas_dv = uv[3],
        });
      }
    }
    // Image tiles (rendering.md section 10). Resolve each visible tile's
    // decoded pixels via the host resolver (ADR 0042 — the engine never
    // decodes), upload to a cached texture (LRU-evicted under the budget,
    // ADR 0034), and emit a textured quad. Off-screen/evicted tiles are
    // re-resolved on the next frame from prepared metadata.
    const auto upload_image_tile = [&](const PreparedImageTile &tile)
        -> std::pair<GlUInt, bool> {
      if (impl_->image_resolver == nullptr) {
        return {0, false};
      }
      const ImageTextureKey key{tile.image_source_id, tile.level, tile.row,
                                tile.col};
      const auto existing = impl_->image_textures.find(key);
      if (existing != impl_->image_textures.end()) {
        existing->second.last_used = impl_->frame_stamp;
        return {existing->second.texture,
                tile.pixel_format == PixelFormat::r8};
      }
      const auto resolved = impl_->image_resolver(ImageTileRequest{
          .image_source_id = tile.image_source_id,
          .level = tile.level,
          .row = tile.row,
          .col = tile.col,
      });
      if (!resolved.has_value()) {
        return {0, false};
      }
      const auto &raster = resolved.value();
      if (raster.data == nullptr || raster.width_px == 0 ||
          raster.height_px == 0) {
        return {0, false};
      }
      const auto byte_size = raster.byte_size();
      // A single tile larger than the whole budget can never be uploaded;
      // reject it before evicting live textures it would displace for nothing.
      if (byte_size > impl_->maximum_image_texture_bytes) {
        return {0, false};
      }
      // Evict least-recently-used tiles until the new one fits the budget.
      while (!impl_->image_textures.empty() &&
             impl_->image_texture_bytes + byte_size >
                 impl_->maximum_image_texture_bytes) {
        auto victim = std::min_element(
            impl_->image_textures.begin(), impl_->image_textures.end(),
            [](const auto &a, const auto &b) {
              return a.second.last_used < b.second.last_used;
            });
        if (victim->second.texture != 0) {
          impl_->gl.delete_textures(1, &victim->second.texture);
        }
        impl_->image_texture_bytes -= victim->second.byte_size;
        impl_->image_textures.erase(victim);
      }
      GlUInt texture{};
      impl_->gl.gen_textures(1, &texture);
      impl_->gl.bind_texture(gl_texture_2d, texture);
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_min_filter, gl_linear);
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_mag_filter, gl_linear);
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_wrap_s, gl_clamp_to_edge);
      impl_->gl.tex_parameteri(gl_texture_2d, gl_texture_wrap_t, gl_clamp_to_edge);
      const auto internal_format =
          tile.pixel_format == PixelFormat::rgba8 ? gl_rgba8
          : tile.pixel_format == PixelFormat::rgb8 ? gl_rgb8_internal
                                                   : gl_r8;
      const auto upload_format =
          tile.pixel_format == PixelFormat::rgba8 ? gl_rgba
          : tile.pixel_format == PixelFormat::rgb8 ? gl_rgb
                                                   : gl_red;
      impl_->gl.pixel_storei(gl_unpack_alignment, 1);
      impl_->gl.tex_image_2d(
          gl_texture_2d, 0, static_cast<GlInt>(internal_format),
          static_cast<GlSize>(raster.width_px),
          static_cast<GlSize>(raster.height_px), 0, upload_format,
          gl_unsigned_byte, raster.data);
      impl_->image_textures.emplace(
          key, ImageTextureEntry{.texture = texture,
                                 .byte_size = byte_size,
                                 .last_used = impl_->frame_stamp});
      impl_->image_texture_bytes += byte_size;
      return {texture, tile.pixel_format == PixelFormat::r8};
    };

    for (const auto &image_layer : scene.image_layers()) {
      const auto clip = clip_for_track(image_layer.track_id);
      for (std::uint64_t offset = 0; offset < image_layer.tile_count;
           ++offset) {
        const auto &tile = scene.image_tiles()[static_cast<std::size_t>(
            image_layer.first_tile + offset)];
        const auto [texture, single_channel] = upload_image_tile(tile);
        if (texture == 0) {
          continue; // unresolved or over-budget: skip, re-attempt next frame
        }
        const auto first_vertex = primitive_vertices.size();
        append_quad(primitive_vertices, tile.rect.left.value,
                    tile.rect.top.value,
                    tile.rect.left.value + tile.rect.width.value,
                    tile.rect.top.value + tile.rect.height.value, 0.0F, 0.0F,
                    1.0F, 1.0F);
        primitive_batches.push_back(PrimitiveBatch{
            .kind = PrimitiveKind::image,
            .first_vertex = static_cast<GlInt>(first_vertex),
            .vertex_count = static_cast<GlSize>(6),
            .color = {},
            .clip = clip,
            .image_texture = texture,
            .image_single_channel = single_channel ? 1 : 0,
        });
      }
    }
    for (const auto &layer : scene.marker_layers()) {
      const auto clip = clip_for_track(layer.track_id);
      for (std::uint64_t offset = 0; offset < layer.marker_count;
           ++offset) {
        const auto &marker = scene.markers()[static_cast<std::size_t>(
            layer.first_marker + offset)];
        append_solid_quad(
            PhysicalRect{
                .left = clip.left,
                .top = Millimetres{marker.display_top.value -
                                   layer.line_width.value * 0.5},
                .width = clip.width,
                .height = layer.line_width,
            },
            layer.line_color, clip);
      }
    }
    for (const auto &layer : scene.symbol_layers()) {
      const auto clip = clip_for_track(layer.track_id);
      const auto first_vertex = primitive_vertices.size();
      for (std::uint64_t offset = 0; offset < layer.symbol_count;
           ++offset) {
        const auto &symbol = scene.symbols()[static_cast<std::size_t>(
            layer.first_symbol + offset)];
        append_symbol_geometry(primitive_vertices, symbol,
                               layer.symbol_size.value * 0.5);
      }
      if (primitive_vertices.size() > first_vertex) {
        primitive_batches.push_back(PrimitiveBatch{
            .kind = PrimitiveKind::solid,
            .first_vertex = static_cast<GlInt>(first_vertex),
            .vertex_count = static_cast<GlSize>(primitive_vertices.size() -
                                                first_vertex),
            .color = layer.color,
            .clip = clip,
        });
      }
    }
    // Custom layers (ADR 0018/0046). Each triangle/quad/symbol primitive is
    // emitted as a solid batch reusing the existing append_* helpers — no new
    // shader, program or batch kind. Polylines are stroked as a quad ribbon
    // (one thin quad per segment, offset along the segment normal by half the
    // stroke width), which the solid program draws as filled triangles.
    const auto custom_vertices = scene.custom_vertices();
    for (const auto &custom_layer : scene.custom_layers()) {
      const auto clip = clip_for_track(custom_layer.track_id);
      for (std::uint64_t offset = 0; offset < custom_layer.primitive_count;
           ++offset) {
        const auto &primitive =
            scene.custom_primitives()[static_cast<std::size_t>(
                custom_layer.first_primitive + offset)];
        const auto first_vertex = primitive_vertices.size();
        if (primitive.kind == CustomPrimitiveKind::polyline) {
          // Stroke as a quad ribbon: one thin quad (two triangles) per
          // segment, offset along the segment normal by half the stroke width.
          // When a dash_pattern is present, only the "on" portions of the
          // pattern are emitted (ADR 0050 CPU dash subdivision).
          const auto width = primitive.stroke_width.value;
          const auto &dash = primitive.dash_pattern.segments;
          // ADR 0050 dash parity (#840): SVG/PDF treat odd-length dash arrays
          // as if duplicated ([4] -> on 4, off 4); the GL subdivision must do
          // the same or an odd array degenerates into an all-on solid line.
          // The scene preflight already rejects non-positive/non-finite
          // segments and offsets, so the cycle below is always finite and > 0
          // for session-built scenes; a hand-built scene that bypasses
          // preflight falls back to solid here instead of NaN-looping into a
          // vanished polyline (fmod(phase, 0) = NaN, zero-chunk loop exit).
          std::vector<Millimetres> normalized_dash;
          std::span<const Millimetres> dash_view = dash;
          if (!dash.empty() && (dash.size() % 2U) != 0U) {
            normalized_dash.reserve(dash.size() * 2U);
            normalized_dash.insert(normalized_dash.end(), dash.begin(),
                                   dash.end());
            normalized_dash.insert(normalized_dash.end(), dash.begin(),
                                   dash.end());
            dash_view = normalized_dash;
          }
          double dash_cycle = 0.0;
          for (const auto &segment : dash_view) {
            dash_cycle += segment.value;
          }
          const bool dash_usable =
              !dash_view.empty() && std::isfinite(dash_cycle) &&
              dash_cycle > 0.0;
          // dash_phase accumulates along the polyline so the pattern is
          // continuous across segments. It is advanced through the full
          // segment length even for "off" gaps.
          double dash_phase = primitive.dash_pattern.offset;
          if (!dash_usable) {
            gl_dash_debug_stats().dash_arrays_fell_back_to_solid += 1;
          }
          const auto emit_ribbon = [&](const PhysicalPoint &p0,
                                       const PhysicalPoint &p1) {
            const auto dx = p1.left.value - p0.left.value;
            const auto dy = p1.top.value - p0.top.value;
            const auto length = std::hypot(dx, dy);
            if (length <= 0.0) {
              return;
            }
            const auto half = width * 0.5;
            const auto nx = -dy / length * half;
            const auto ny = dx / length * half;
            append_triangle(primitive_vertices, p0.left.value + nx,
                            p0.top.value + ny, p0.left.value - nx,
                            p0.top.value - ny, p1.left.value - nx,
                            p1.top.value - ny);
            append_triangle(primitive_vertices, p0.left.value + nx,
                            p0.top.value + ny, p1.left.value - nx,
                            p1.top.value - ny, p1.left.value + nx,
                            p1.top.value + ny);
          };
          // emit_segment handles both solid (no dash) and dashed lines.
          // For dashed, it subdivides the segment into on/off sub-segments
          // based on the dash cycle, emitting ribbons only for "on" parts.
          const auto emit_segment = [&](const PhysicalPoint &p0,
                                        const PhysicalPoint &p1) {
            if (!dash_usable) {
              emit_ribbon(p0, p1);
              return;
            }
            const auto dx = p1.left.value - p0.left.value;
            const auto dy = p1.top.value - p0.top.value;
            const auto length = std::hypot(dx, dy);
            if (length <= 0.0) {
              return;
            }
            const auto ux = dx / length;
            const auto uy = dy / length;
            double pos = 0.0;
            while (pos < length) {
              // Find which dash element we're currently in.
              auto phase_in_cycle =
                  std::fmod(dash_phase, dash_cycle);
              if (phase_in_cycle < 0.0) {
                phase_in_cycle += dash_cycle;
              }
              std::size_t seg_index = 0;
              double acc = 0.0;
              for (std::size_t i = 0; i < dash_view.size(); ++i) {
                if (phase_in_cycle < acc + dash_view[i].value) {
                  seg_index = i;
                  break;
                }
                acc += dash_view[i].value;
              }
              const bool is_on = (seg_index % 2 == 0);
              const auto remaining_in_element =
                  dash_view[seg_index].value -
                  (phase_in_cycle -
                   [&] {
                     double a = 0.0;
                     for (std::size_t i = 0; i < seg_index; ++i) {
                       a += dash_view[i].value;
                     }
                     return a;
                   }());
              const auto chunk = std::min(remaining_in_element, length - pos);
              if (is_on && chunk > 0.0) {
                const PhysicalPoint sub0{
                    .left = Millimetres{p0.left.value + ux * pos},
                    .top = Millimetres{p0.top.value + uy * pos}};
                const PhysicalPoint sub1{
                    .left = Millimetres{p0.left.value + ux * (pos + chunk)},
                    .top = Millimetres{p0.top.value + uy * (pos + chunk)}};
                emit_ribbon(sub0, sub1);
              }
              pos += chunk;
              dash_phase += chunk;
            }
          };
          for (std::uint64_t point_offset = 1;
               point_offset < primitive.vertex_count; ++point_offset) {
            emit_segment(
                custom_vertices[static_cast<std::size_t>(
                    primitive.first_vertex + point_offset - 1)],
                custom_vertices[static_cast<std::size_t>(
                    primitive.first_vertex + point_offset)]);
          }
          if (primitive.closed && primitive.vertex_count >= 3) {
            emit_segment(
                custom_vertices[static_cast<std::size_t>(
                    primitive.first_vertex + primitive.vertex_count - 1)],
                custom_vertices[static_cast<std::size_t>(
                    primitive.first_vertex)]);
          }
        } else if (primitive.kind == CustomPrimitiveKind::triangle ||
                   primitive.kind == CustomPrimitiveKind::quad) {
          // Pattern-filled quads use the same PrimitiveKind::pattern batch path
          // as Intervals (ADR 0050): a single quad with pattern UVs from the
          // shared atlas. Solid quads and triangles fall through to the
          // triangulated solid path.
          if (primitive.kind == CustomPrimitiveKind::quad &&
              !primitive.pattern_id.is_nil()) {
            const auto pattern_uv = pattern_uvs.find(primitive.pattern_id);
            const auto pattern =
                pattern_uv == pattern_uvs.end()
                    ? scene.patterns().end()
                    : std::find_if(
                          scene.patterns().begin(), scene.patterns().end(),
                          [&](const PatternDefinition &candidate) {
                            return candidate.id == primitive.pattern_id;
                          });
            if (pattern != scene.patterns().end()) {
              const auto &b = primitive.bounds;
              const auto first_vertex = primitive_vertices.size();
              append_quad(primitive_vertices, b.left.value, b.top.value,
                          b.left.value + b.width.value,
                          b.top.value + b.height.value, 0.0F, 0.0F, 0.0F,
                          0.0F);
              const auto theta =
                  pattern->rotation_degrees * 3.14159265358979323846 / 180.0;
              const auto &uv = pattern_uv->second;
              primitive_batches.push_back(PrimitiveBatch{
                  .kind = PrimitiveKind::pattern,
                  .first_vertex = static_cast<GlInt>(first_vertex),
                  .vertex_count = static_cast<GlSize>(6),
                  .color = {},
                  .clip = clip,
                  .anchor_left = pattern->scene_anchor.left.value,
                  .anchor_top = pattern->scene_anchor.top.value,
                  .tile_width = pattern->tile_width.value,
                  .tile_height = pattern->tile_height.value,
                  .rotation_cos = std::cos(theta),
                  .rotation_sin = std::sin(theta),
                  .atlas_u = uv[0],
                  .atlas_v = uv[1],
                  .atlas_du = uv[2],
                  .atlas_dv = uv[3],
              });
              continue;
            }
            // Pattern not found → fall through to solid fill.
          }
          // Triangles and quads are stored as clipped, triangulated geometry:
          // vertex_count vertices in groups of 3 (one solid triangle each).
          // Quads may produce fewer/more than their original two triangles
          // after clipping to the layer-local clip path.
          const auto triangle_count = primitive.vertex_count / 3;
          for (std::uint64_t tri = 0; tri < triangle_count; ++tri) {
            const auto base = static_cast<std::size_t>(
                primitive.first_vertex + tri * 3);
            const auto &a = custom_vertices[base];
            const auto &b = custom_vertices[base + 1];
            const auto &c = custom_vertices[base + 2];
            append_triangle(primitive_vertices, a.left.value, a.top.value,
                            b.left.value, b.top.value, c.left.value,
                            c.top.value);
          }
        } else {
          // Symbol: rebuild geometry from the single center vertex + the
          // primitive's kind/size (mirrors the built-in symbol layer, which
          // handles every SymbolKind via append_symbol_geometry).
          const auto &center = custom_vertices[static_cast<std::size_t>(
              primitive.first_vertex)];
          const PreparedSymbol symbol{
              .layer_id = primitive.layer_id,
              .symbol_id = primitive.source_id,
              .center = center,
              .kind = primitive.symbol_kind,
              .reference_depth = 0.0,
          };
          const auto half = primitive.bounds.width.value * 0.5;
          append_symbol_geometry(primitive_vertices, symbol, half);
        }
        if (primitive_vertices.size() > first_vertex) {
          primitive_batches.push_back(PrimitiveBatch{
              .kind = PrimitiveKind::solid,
              .first_vertex = static_cast<GlInt>(first_vertex),
              .vertex_count = static_cast<GlSize>(primitive_vertices.size() -
                                                  first_vertex),
              .color = primitive.color,
              .clip = clip,
          });
        }
      }
    }
    const auto clip_for_run = [&](const PreparedTextRun &run) {
      const auto track_id = scene.track_id_for_layer(run.layer_id);
      return track_id.has_value() ? clip_for_track(*track_id)
                                  : PhysicalRect{};
    };
    for (const auto &run : scene.text_runs()) {
      const auto clip = clip_for_run(run);
      const auto first_vertex = primitive_vertices.size();
      const auto font_size = run.font_size.value;
      for (std::uint64_t offset = 0; offset < run.glyph_count; ++offset) {
        const auto &glyph = scene.glyphs()[static_cast<std::size_t>(
            run.first_glyph + offset)];
        const auto entry = glyph_atlas_entries.find(
            glyph_atlas_key(glyph.font_index, glyph.glyph_id));
        if (entry == glyph_atlas_entries.end()) {
          continue;
        }
        const auto &atlas = entry->second;
        const auto theta =
            glyph.rotation_degrees * 3.14159265358979323846 / 180.0;
        const auto rotation_cos = std::cos(theta);
        const auto rotation_sin = std::sin(theta);
        const auto right_em =
            atlas.left_em + atlas.pixel_width / atlas.pixels_per_em;
        const auto bottom_em =
            atlas.top_em - atlas.pixel_height / atlas.pixels_per_em;
        const auto top_left = glyph_corner(
            atlas.left_em, atlas.top_em, font_size, rotation_cos,
            rotation_sin, glyph.origin.left.value, glyph.origin.top.value);
        const auto top_right = glyph_corner(
            right_em, atlas.top_em, font_size, rotation_cos, rotation_sin,
            glyph.origin.left.value, glyph.origin.top.value);
        const auto bottom_left = glyph_corner(
            atlas.left_em, bottom_em, font_size, rotation_cos, rotation_sin,
            glyph.origin.left.value, glyph.origin.top.value);
        const auto bottom_right = glyph_corner(
            right_em, bottom_em, font_size, rotation_cos, rotation_sin,
            glyph.origin.left.value, glyph.origin.top.value);
        const auto push = [&](const std::pair<double, double> &point,
                              float u, float v) {
          primitive_vertices.push_back(PrimitiveVertex{
              static_cast<GlFloat>(point.first),
              static_cast<GlFloat>(point.second), u, v});
        };
        push(top_left, atlas.u0, atlas.v0);
        push(bottom_left, atlas.u0, atlas.v1);
        push(bottom_right, atlas.u1, atlas.v1);
        push(top_left, atlas.u0, atlas.v0);
        push(bottom_right, atlas.u1, atlas.v1);
        push(top_right, atlas.u1, atlas.v0);
      }
      if (primitive_vertices.size() > first_vertex) {
        primitive_batches.push_back(PrimitiveBatch{
            .kind = PrimitiveKind::glyph,
            .first_vertex = static_cast<GlInt>(first_vertex),
            .vertex_count = static_cast<GlSize>(primitive_vertices.size() -
                                                first_vertex),
            .color = run.color,
            .clip = clip,
        });
      }
    }
    if (primitive_vertices.size() >
        static_cast<std::size_t>(std::numeric_limits<GlInt>::max())) {
      return false;
    }

    // Center primitive vertices vertically before the float conversion so
    // deep scenes keep sub-millimetre precision (rendering.md section 2).
    const auto scene_y_center = scene.physical_height().value * 0.5;
    for (auto &vertex : primitive_vertices) {
      vertex.scene_top -= static_cast<GlFloat>(scene_y_center);
    }

    impl_->staging_buffer = 1U - impl_->active_buffer;
    impl_->pending_scene = scene;
    impl_->pending_edges = std::move(edges);
    impl_->pending_batches = std::move(batches);
    impl_->pending_primitive_vertices = std::move(primitive_vertices);
    impl_->pending_primitive_batches = std::move(primitive_batches);
    if (atlas_cache_hit) {
      // Cache stays authoritative; pending shares the same immutable
      // bitmaps (issue #606).
    } else {
      impl_->atlas_fingerprint = std::move(fingerprint);
      impl_->cached_pattern_atlas = pattern_atlas_ptr;
      impl_->cached_glyph_atlas = glyph_atlas_ptr;
      impl_->cached_pattern_uvs = pattern_uvs;
      impl_->cached_glyph_entries = glyph_atlas_entries;
    }
    impl_->pending_atlas_fingerprint = impl_->atlas_fingerprint;
    impl_->pending_atlases_need_upload =
        !impl_->uploaded_atlas_fingerprint.has_value() ||
        *impl_->uploaded_atlas_fingerprint != impl_->pending_atlas_fingerprint;
    impl_->pending_pattern_atlas = std::move(pattern_atlas_ptr);
    impl_->pending_glyph_atlas = std::move(glyph_atlas_ptr);
    impl_->pending_scene_height = scene.physical_height().value;
    impl_->pending_depth_top = depth_range.top;
    impl_->pending_depth_span = depth_range.bottom - depth_range.top;
    const auto chunks = schedule.value().chunks();
    impl_->pending_chunks.assign(chunks.begin(), chunks.end());
    impl_->next_pending_chunk = 0;
    impl_->pending_total_bytes = static_cast<std::uint64_t>(byte_count);
    impl_->pending_physical_width = scene.physical_width().value;
    impl_->pending_scene_depth_center = scene_depth_center;
    impl_->pending_bytes_uploaded = 0;
    impl_->pending_buffer_allocated = false;
    impl_->drop_active_before_upload =
        impl_->active_bytes >
        budgets.maximum_cache_bytes - static_cast<std::uint64_t>(byte_count);
    impl_->upload_pending = true;
    return true;
  } catch (...) {
    return false;
  }
}

GlUploadProgress GlRenderer::upload_next() noexcept {
  if (!initialized() || std::this_thread::get_id() != impl_->owner_thread ||
      !impl_->upload_pending) {
    return {};
  }
  try {
    const auto total_bytes = impl_->pending_total_bytes;
    if (!impl_->pending_buffer_allocated) {
      clear_gl_errors(impl_->gl);
      if (impl_->drop_active_before_upload) {
        const auto &active = impl_->buffers[impl_->active_buffer];
        impl_->gl.bind_vertex_array(active.vertex_array);
        impl_->gl.bind_buffer(gl_array_buffer, active.vertex_buffer);
        impl_->gl.buffer_data(gl_array_buffer, 0, nullptr, gl_dynamic_draw);
        if (impl_->gl.get_error() != gl_no_error) {
          impl_->upload_pending = false;
          return {};
        }
        impl_->batches.clear();
        impl_->active_bytes = 0;
      }
      const auto &staging = impl_->buffers[impl_->staging_buffer];
      impl_->gl.bind_vertex_array(staging.vertex_array);
      impl_->gl.bind_buffer(gl_array_buffer, staging.vertex_buffer);
      impl_->gl.buffer_data(gl_array_buffer,
                            static_cast<GlSizePointer>(total_bytes), nullptr,
                            gl_dynamic_draw);
      if (impl_->gl.get_error() != gl_no_error) {
        impl_->upload_pending = false;
        return {};
      }
      impl_->pending_buffer_allocated = true;
    }
    if (impl_->next_pending_chunk < impl_->pending_chunks.size()) {
      const auto chunk = impl_->pending_chunks[impl_->next_pending_chunk];
      if (chunk.byte_offset > total_bytes ||
          chunk.byte_count > total_bytes - chunk.byte_offset ||
          chunk.byte_offset > static_cast<std::uint64_t>(
                                  std::numeric_limits<GlSizePointer>::max()) ||
          chunk.byte_count > static_cast<std::uint64_t>(
                                 std::numeric_limits<GlSizePointer>::max())) {
        impl_->upload_pending = false;
        return {};
      }
      const auto &staging = impl_->buffers[impl_->staging_buffer];
      impl_->gl.bind_vertex_array(staging.vertex_array);
      impl_->gl.bind_buffer(gl_array_buffer, staging.vertex_buffer);
      constexpr auto bytes_per_edge = std::uint64_t{6} * sizeof(CurveVertex);
      const auto first_edge =
          static_cast<std::size_t>(chunk.byte_offset / bytes_per_edge);
      const auto edge_count =
          static_cast<std::size_t>(chunk.byte_count / bytes_per_edge);
      std::vector<CurveVertex> vertices;
      vertices.reserve(edge_count * std::size_t{6});
      for (auto edge_offset = std::size_t{}; edge_offset < edge_count;
           ++edge_offset) {
        const auto &edge = impl_->pending_edges[first_edge + edge_offset];
        append_segment_vertices(
            vertices,
            impl_->pending_scene
                .curve_points()[static_cast<std::size_t>(edge.first_point)],
            impl_->pending_scene
                .curve_points()[static_cast<std::size_t>(edge.second_point)],
            impl_->pending_physical_width, impl_->pending_scene_depth_center);
      }
      clear_gl_errors(impl_->gl);
      impl_->gl.buffer_sub_data(
          gl_array_buffer, static_cast<GlSizePointer>(chunk.byte_offset),
          static_cast<GlSizePointer>(chunk.byte_count), vertices.data());
      if (impl_->gl.get_error() != gl_no_error) {
        impl_->upload_pending = false;
        return {};
      }
      impl_->pending_bytes_uploaded += chunk.byte_count;
      ++impl_->next_pending_chunk;
    }
    if (impl_->next_pending_chunk < impl_->pending_chunks.size()) {
      return GlUploadProgress{
          .bytes_uploaded = impl_->pending_bytes_uploaded,
          .total_bytes = total_bytes,
          .pending = true,
          .completed = false,
      };
    }
    const auto old_buffer = impl_->active_buffer;
    const auto &old = impl_->buffers[old_buffer];
    impl_->gl.bind_vertex_array(old.vertex_array);
    impl_->gl.bind_buffer(gl_array_buffer, old.vertex_buffer);
    clear_gl_errors(impl_->gl);
    impl_->gl.buffer_data(gl_array_buffer, 0, nullptr, gl_dynamic_draw);
    if (impl_->gl.get_error() != gl_no_error) {
      impl_->upload_pending = false;
      return {};
    }
    impl_->active_buffer = impl_->staging_buffer;
    impl_->staging_buffer = old_buffer;
    impl_->physical_width = impl_->pending_physical_width;
    impl_->scene_depth_center = impl_->pending_scene_depth_center;
    impl_->batches = std::move(impl_->pending_batches);
    impl_->active_bytes = total_bytes;

    impl_->gl.bind_vertex_array(impl_->primitives.vertex_array);
    impl_->gl.bind_buffer(gl_array_buffer, impl_->primitives.vertex_buffer);
    const auto primitive_bytes = static_cast<GlSizePointer>(
        impl_->pending_primitive_vertices.size() * sizeof(PrimitiveVertex));
    clear_gl_errors(impl_->gl);
    impl_->gl.buffer_data(gl_array_buffer, primitive_bytes,
                          impl_->pending_primitive_vertices.empty()
                              ? nullptr
                              : impl_->pending_primitive_vertices.data(),
                          gl_dynamic_draw);
    if (impl_->gl.get_error() != gl_no_error) {
      impl_->upload_pending = false;
      return {};
    }
    impl_->gl.active_texture(gl_texture0);
    if (impl_->pending_atlases_need_upload) {
      if (impl_->pending_pattern_atlas != nullptr &&
          !impl_->pending_pattern_atlas->pixels.empty()) {
        clear_gl_errors(impl_->gl);
        impl_->gl.bind_texture(gl_texture_2d, impl_->pattern_texture);
        impl_->gl.tex_image_2d(
            gl_texture_2d, 0, static_cast<GlInt>(gl_rgba8),
            static_cast<GlSize>(impl_->pending_pattern_atlas->width),
            static_cast<GlSize>(impl_->pending_pattern_atlas->height), 0,
            gl_rgba, gl_unsigned_byte,
            impl_->pending_pattern_atlas->pixels.data());
        if (impl_->gl.get_error() != gl_no_error) {
          impl_->upload_pending = false;
          return {};
        }
        ++gl_atlas_debug_stats().tex_image_2d_calls;
      }
      if (impl_->pending_glyph_atlas != nullptr &&
          !impl_->pending_glyph_atlas->pixels.empty()) {
        clear_gl_errors(impl_->gl);
        impl_->gl.bind_texture(gl_texture_2d, impl_->glyph_texture);
        impl_->gl.tex_image_2d(
            gl_texture_2d, 0, static_cast<GlInt>(gl_r8),
            static_cast<GlSize>(impl_->pending_glyph_atlas->width),
            static_cast<GlSize>(impl_->pending_glyph_atlas->height), 0, gl_red,
            gl_unsigned_byte, impl_->pending_glyph_atlas->pixels.data());
        if (impl_->gl.get_error() != gl_no_error) {
          impl_->upload_pending = false;
          return {};
        }
        ++gl_atlas_debug_stats().tex_image_2d_calls;
      }
      impl_->uploaded_atlas_fingerprint = impl_->pending_atlas_fingerprint;
      impl_->pending_atlases_need_upload = false;
    }
    impl_->primitive_batches = std::move(impl_->pending_primitive_batches);
    impl_->scene_height = impl_->pending_scene_height;
    impl_->depth_top = impl_->pending_depth_top;
    impl_->depth_span = impl_->pending_depth_span;
    impl_->pending_primitive_vertices.clear();
    impl_->pending_pattern_atlas.reset();
    impl_->pending_glyph_atlas.reset();
    impl_->pending_scene = PreparedScene{};
    impl_->pending_edges.clear();
    impl_->pending_chunks.clear();
    impl_->pending_total_bytes = 0;
    impl_->pending_buffer_allocated = false;
    impl_->upload_pending = false;
    return GlUploadProgress{
        .bytes_uploaded = total_bytes,
        .total_bytes = total_bytes,
        .pending = false,
        .completed = true,
    };
  } catch (...) {
    impl_->upload_pending = false;
    return {};
  }
}

bool GlRenderer::render(const GlRenderFrame &frame) noexcept {
  if (!initialized() || std::this_thread::get_id() != impl_->owner_thread ||
      frame.pixel_width <= 0 || frame.pixel_height <= 0 ||
      !std::isfinite(frame.physical_pixels_per_millimetre) ||
      frame.physical_pixels_per_millimetre <= 0.0 ||
      !std::isfinite(frame.viewport.top) ||
      !std::isfinite(frame.viewport.bottom) ||
      frame.viewport.top >= frame.viewport.bottom ||
      (frame.horizontal.has_value() &&
       (!std::isfinite(frame.horizontal->left_mm) ||
        !std::isfinite(frame.horizontal->span_mm) ||
        frame.horizontal->span_mm <= 0.0))) {
    return false;
  }
  ++impl_->frame_stamp; // advance LRU recency for image-texture eviction
  const auto viewport_center =
      frame.viewport.top + (frame.viewport.bottom - frame.viewport.top) * 0.5;
  const auto viewport_half_span =
      (frame.viewport.bottom - frame.viewport.top) * 0.5;
  if (!std::isfinite(viewport_center) || !std::isfinite(viewport_half_span) ||
      viewport_half_span <= 0.0) {
    return false;
  }

  impl_->gl.bind_framebuffer(gl_framebuffer, frame.framebuffer);
  impl_->gl.viewport(0, 0, frame.pixel_width, frame.pixel_height);
  impl_->gl.disable(gl_depth_test);
  impl_->gl.disable(gl_cull_face);
  impl_->gl.disable(gl_stencil_test);
  impl_->gl.disable(gl_scissor_test);
  impl_->gl.color_mask(static_cast<GlBoolean>(1), static_cast<GlBoolean>(1),
                       static_cast<GlBoolean>(1), static_cast<GlBoolean>(1));
  impl_->gl.stencil_mask(std::numeric_limits<GlUInt>::max());
  impl_->gl.clear_color(1.0F, 1.0F, 1.0F, 1.0F);
  impl_->gl.clear_stencil(0);
  impl_->gl.clear(gl_color_buffer_bit | gl_stencil_buffer_bit);
  impl_->gl.enable(gl_blend);
  impl_->gl.blend_func_separate(gl_src_alpha, gl_one_minus_src_alpha, gl_one,
                                gl_one_minus_src_alpha);
  impl_->gl.use_program(impl_->program);
  impl_->gl.bind_vertex_array(
      impl_->buffers[impl_->active_buffer].vertex_array);
  impl_->gl.uniform_2f(impl_->viewport_pixels_uniform,
                       static_cast<GlFloat>(frame.pixel_width),
                       static_cast<GlFloat>(frame.pixel_height));
  impl_->gl.uniform_1f(
      impl_->viewport_center_uniform,
      static_cast<GlFloat>(viewport_center - impl_->scene_depth_center));
  impl_->gl.uniform_1f(impl_->viewport_half_span_uniform,
                       static_cast<GlFloat>(viewport_half_span));
  impl_->gl.enable(gl_scissor_test);
  if (frame.draw_scene) {
    // Horizontal surface window (unified canvas): scene x ∈ [left, left+span]
    // maps to the framebuffer width. Absent → legacy fit-to-scene-width.
    const auto horizontal = frame.horizontal.value_or(GlHorizontalView{
        .left_mm = 0.0, .span_mm = impl_->physical_width});
    const auto scissor_for = [&](const PhysicalRect &clip) {
      const auto scissor_left = static_cast<int>(std::floor(
          (clip.left.value - horizontal.left_mm) / horizontal.span_mm *
          static_cast<double>(frame.pixel_width)));
      const auto scissor_right = static_cast<int>(std::ceil(
          (clip.left.value + clip.width.value - horizontal.left_mm) /
          horizontal.span_mm * static_cast<double>(frame.pixel_width)));
      const auto clamped_left =
          std::clamp(scissor_left, 0, std::max(0, frame.pixel_width));
      const auto clamped_right =
          std::clamp(scissor_right, 0, std::max(0, frame.pixel_width));
      impl_->gl.scissor(clamped_left, 0,
                        std::max(0, clamped_right - clamped_left),
                        frame.pixel_height);
    };
    const auto mm_scale_x =
        static_cast<GlFloat>(2.0 / horizontal.span_mm);
    const auto mm_offset_x =
        static_cast<GlFloat>(-1.0 - 2.0 * horizontal.left_mm /
                                        horizontal.span_mm);
    const auto mm_scale_y =
        static_cast<GlFloat>(-impl_->depth_span /
                             (impl_->scene_height * viewport_half_span));
    const auto scene_y_center = impl_->scene_height * 0.5;
    const auto mm_offset_y = static_cast<GlFloat>(
        (viewport_center - impl_->depth_top) / viewport_half_span -
        impl_->depth_span / (impl_->scene_height * viewport_half_span) *
            scene_y_center);
    impl_->gl.active_texture(gl_texture0);

    // Pass order follows rendering.md: intervals/patterns, markers and
    // symbols below curves, text above them.
    auto current_program = GlUInt{0};
    const auto set_scene_uniforms = [&](GlUInt program, GlInt scale_uniform,
                                        GlInt offset_uniform) {
      if (program != current_program) {
        impl_->gl.use_program(program);
        current_program = program;
        impl_->gl.uniform_2f(scale_uniform, mm_scale_x, mm_scale_y);
        impl_->gl.uniform_2f(offset_uniform, mm_offset_x, mm_offset_y);
      }
    };
    impl_->gl.bind_vertex_array(impl_->primitives.vertex_array);
    for (const auto &batch : impl_->primitive_batches) {
      if (batch.kind == PrimitiveKind::glyph) {
        continue;
      }
      scissor_for(batch.clip);
      if (batch.kind == PrimitiveKind::image) {
        set_scene_uniforms(impl_->image_program,
                           impl_->image_mm_scale_uniform,
                           impl_->image_mm_offset_uniform);
        impl_->gl.bind_texture(gl_texture_2d, batch.image_texture);
        impl_->gl.uniform_1i(impl_->image_texture_uniform, 0);
        impl_->gl.uniform_1i(impl_->image_swap_red_alpha_uniform,
                             batch.image_single_channel);
      } else if (batch.kind == PrimitiveKind::pattern) {
        set_scene_uniforms(impl_->pattern_program,
                           impl_->pattern_mm_scale_uniform,
                           impl_->pattern_mm_offset_uniform);
        impl_->gl.bind_texture(gl_texture_2d, impl_->pattern_texture);
        impl_->gl.uniform_1i(impl_->pattern_atlas_uniform, 0);
        impl_->gl.uniform_2f(impl_->pattern_anchor_uniform,
                             static_cast<GlFloat>(batch.anchor_left),
                             static_cast<GlFloat>(batch.anchor_top));
        impl_->gl.uniform_2f(impl_->pattern_tile_uniform,
                             static_cast<GlFloat>(batch.tile_width),
                             static_cast<GlFloat>(batch.tile_height));
        impl_->gl.uniform_2f(impl_->pattern_rotation_uniform,
                             static_cast<GlFloat>(batch.rotation_cos),
                             static_cast<GlFloat>(batch.rotation_sin));
        impl_->gl.uniform_4f(impl_->pattern_atlas_uv_uniform, batch.atlas_u,
                             batch.atlas_v, batch.atlas_du, batch.atlas_dv);
      } else {
        set_scene_uniforms(impl_->solid_program,
                           impl_->solid_mm_scale_uniform,
                           impl_->solid_mm_offset_uniform);
        impl_->gl.uniform_4f(impl_->solid_color_uniform,
                             static_cast<GlFloat>(batch.color.red) / 255.0F,
                             static_cast<GlFloat>(batch.color.green) / 255.0F,
                             static_cast<GlFloat>(batch.color.blue) / 255.0F,
                             static_cast<GlFloat>(batch.color.alpha) / 255.0F);
      }
      impl_->gl.draw_arrays(gl_triangles, batch.first_vertex,
                            batch.vertex_count);
    }

    impl_->gl.use_program(impl_->program);
    current_program = impl_->program;
    impl_->gl.bind_vertex_array(
        impl_->buffers[impl_->active_buffer].vertex_array);
    impl_->gl.uniform_2f(impl_->viewport_pixels_uniform,
                         static_cast<GlFloat>(frame.pixel_width),
                         static_cast<GlFloat>(frame.pixel_height));
    impl_->gl.uniform_1f(
        impl_->viewport_center_uniform,
        static_cast<GlFloat>(viewport_center - impl_->scene_depth_center));
    impl_->gl.uniform_1f(impl_->viewport_half_span_uniform,
                         static_cast<GlFloat>(viewport_half_span));
    impl_->gl.uniform_1f(impl_->scene_width_uniform,
                         static_cast<GlFloat>(impl_->physical_width));
    impl_->gl.uniform_1f(impl_->horizontal_left_uniform,
                         static_cast<GlFloat>(horizontal.left_mm));
    impl_->gl.uniform_1f(impl_->horizontal_span_uniform,
                         static_cast<GlFloat>(horizontal.span_mm));
    for (const auto &batch : impl_->batches) {
      scissor_for(batch.clip);
      impl_->gl.uniform_1f(
          impl_->half_width_uniform,
          static_cast<GlFloat>(
              std::max(0.5, batch.line_width.value *
                                frame.physical_pixels_per_millimetre * 0.5)));
      impl_->gl.uniform_4f(impl_->color_uniform,
                           static_cast<GlFloat>(batch.color.red) / 255.0F,
                           static_cast<GlFloat>(batch.color.green) / 255.0F,
                           static_cast<GlFloat>(batch.color.blue) / 255.0F,
                           static_cast<GlFloat>(batch.color.alpha) / 255.0F);
      impl_->gl.draw_arrays(gl_triangles, batch.first_vertex,
                            batch.vertex_count);
    }

    impl_->gl.bind_vertex_array(impl_->primitives.vertex_array);
    for (const auto &batch : impl_->primitive_batches) {
      if (batch.kind != PrimitiveKind::glyph) {
        continue;
      }
      scissor_for(batch.clip);
      set_scene_uniforms(impl_->glyph_program,
                         impl_->glyph_mm_scale_uniform,
                         impl_->glyph_mm_offset_uniform);
      impl_->gl.bind_texture(gl_texture_2d, impl_->glyph_texture);
      impl_->gl.uniform_1i(impl_->glyph_atlas_uniform, 0);
      impl_->gl.uniform_4f(impl_->glyph_color_uniform,
                           static_cast<GlFloat>(batch.color.red) / 255.0F,
                           static_cast<GlFloat>(batch.color.green) / 255.0F,
                           static_cast<GlFloat>(batch.color.blue) / 255.0F,
                           static_cast<GlFloat>(batch.color.alpha) / 255.0F);
      impl_->gl.draw_arrays(gl_triangles, batch.first_vertex,
                            batch.vertex_count);
    }
  }
  if (frame.crosshair.has_value() &&
      std::isfinite(frame.crosshair->horizontal_fraction) &&
      std::isfinite(frame.crosshair->display_depth)) {
    const auto horizontal_fraction =
        std::clamp(frame.crosshair->horizontal_fraction, 0.0, 1.0);
    const auto vertical_fraction =
        (frame.crosshair->display_depth - frame.viewport.top) /
        (frame.viewport.bottom - frame.viewport.top);
    if (vertical_fraction >= 0.0 && vertical_fraction <= 1.0) {
      const auto crosshair_left =
          std::clamp(static_cast<int>(std::lround(
                         horizontal_fraction *
                         static_cast<double>(frame.pixel_width - 1))),
                     0, frame.pixel_width - 1);
      const auto crosshair_bottom =
          std::clamp(static_cast<int>(std::lround(
                         (1.0 - vertical_fraction) *
                         static_cast<double>(frame.pixel_height - 1))),
                     0, frame.pixel_height - 1);
      impl_->gl.clear_color(0.85F, 0.1F, 0.1F, 1.0F);
      impl_->gl.scissor(crosshair_left, 0, 1, frame.pixel_height);
      impl_->gl.clear(gl_color_buffer_bit);
      impl_->gl.scissor(0, crosshair_bottom, frame.pixel_width, 1);
      impl_->gl.clear(gl_color_buffer_bit);
    }
  }
  impl_->gl.disable(gl_scissor_test);
  return true;
}

void GlRenderer::release() noexcept {
  if (std::this_thread::get_id() != impl_->owner_thread) {
    abandon();
    return;
  }
  for (auto &buffer : impl_->buffers) {
    if (buffer.vertex_buffer != 0) {
      impl_->gl.delete_buffers(1, &buffer.vertex_buffer);
    }
    if (buffer.vertex_array != 0) {
      impl_->gl.delete_vertex_arrays(1, &buffer.vertex_array);
    }
  }
  if (impl_->primitives.vertex_buffer != 0) {
    impl_->gl.delete_buffers(1, &impl_->primitives.vertex_buffer);
  }
  if (impl_->primitives.vertex_array != 0) {
    impl_->gl.delete_vertex_arrays(1, &impl_->primitives.vertex_array);
  }
  if (impl_->pattern_texture != 0) {
    impl_->gl.delete_textures(1, &impl_->pattern_texture);
  }
  if (impl_->glyph_texture != 0) {
    impl_->gl.delete_textures(1, &impl_->glyph_texture);
  }
  for (const auto &[key, entry] : impl_->image_textures) {
    if (entry.texture != 0) {
      impl_->gl.delete_textures(1, &entry.texture);
    }
  }
  impl_->image_textures.clear();
  impl_->image_texture_bytes = 0;
  for (const auto program :
       {impl_->program, impl_->solid_program, impl_->pattern_program,
        impl_->glyph_program, impl_->image_program}) {
    if (program != 0) {
      impl_->gl.delete_program(program);
    }
  }
  abandon();
}

void GlRenderer::abandon() noexcept {
  impl_->buffers = {};
  impl_->program = 0;
  impl_->solid_program = 0;
  impl_->pattern_program = 0;
  impl_->glyph_program = 0;
  impl_->image_program = 0;
  impl_->primitives = {};
  impl_->pattern_texture = 0;
  impl_->glyph_texture = 0;
  impl_->image_textures.clear();
  impl_->image_texture_bytes = 0;
  impl_->batches.clear();
  impl_->primitive_batches.clear();
  impl_->pending_scene = PreparedScene{};
  impl_->pending_edges.clear();
  impl_->pending_batches.clear();
  impl_->pending_primitive_vertices.clear();
  impl_->pending_primitive_batches.clear();
  impl_->pending_pattern_atlas.reset();
  impl_->pending_glyph_atlas.reset();
  impl_->cached_pattern_atlas.reset();
  impl_->cached_glyph_atlas.reset();
  impl_->atlas_fingerprint.clear();
  impl_->uploaded_atlas_fingerprint.reset();
  impl_->pending_atlas_fingerprint.clear();
  impl_->pending_atlases_need_upload = true;
  impl_->pending_chunks.clear();
  impl_->pending_total_bytes = 0;
  impl_->pending_buffer_allocated = false;
  impl_->drop_active_before_upload = false;
  impl_->upload_pending = false;
  impl_->active_buffer = 0;
  impl_->staging_buffer = 1;
  impl_->active_bytes = 0;
  impl_->owner_thread = {};
}

bool GlRenderer::initialized() const noexcept {
  return impl_ != nullptr && impl_->program != 0 &&
         impl_->solid_program != 0 && impl_->pattern_program != 0 &&
         impl_->glyph_program != 0 && impl_->image_program != 0 &&
         impl_->primitives.vertex_array != 0 &&
         impl_->primitives.vertex_buffer != 0 &&
         impl_->pattern_texture != 0 && impl_->glyph_texture != 0 &&
         std::all_of(impl_->buffers.begin(), impl_->buffers.end(),
                     [](const Impl::BufferSlot &buffer) {
                       return buffer.vertex_array != 0 &&
                              buffer.vertex_buffer != 0;
                     });
}

void GlRenderer::set_image_tile_resolver(
    std::function<Result<RasterTile>(const ImageTileRequest &)> resolver,
    std::uint64_t maximum_texture_bytes) noexcept {
  // The resolver is invoked on the GL thread during upload, so it must be
  // installed from the same (GUI) thread that owns the context (ADR 0016).
  if (impl_ == nullptr ||
      std::this_thread::get_id() != impl_->owner_thread) {
    return;
  }
  impl_->image_resolver = std::move(resolver);
  impl_->maximum_image_texture_bytes = maximum_texture_bytes;
}

} // namespace welllog::detail
