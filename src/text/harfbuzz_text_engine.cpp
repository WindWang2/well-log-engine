#include <welllog/text/harfbuzz_text_engine.hpp>

#include "text/font5x7.hpp"
#include "text/utf8_decode.hpp"

#include <welllog/core/utf8.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <hb.h>
#include <hb-ot.h>

#include <unicode/uchar.h>
#include <unicode/ubrk.h>
#include <unicode/ustring.h>

#include <algorithm>
#include <iterator>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace welllog {
namespace {

constexpr std::uint64_t maximum_font_file_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t maximum_fonts = 64;
constexpr std::size_t maximum_system_faces_per_scan = 512;
constexpr char32_t replacement_code_point = U'?';

[[nodiscard]] Error font_error(MessageKey message) {
  return Error{
      .code = ErrorCode::invalid_font,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

[[nodiscard]] std::uint64_t fnv1a64(const std::vector<std::byte> &bytes) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto byte : bytes) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::string fingerprint_for(const std::vector<std::byte> &bytes,
                                          long face_index) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%016llx:%ld",
                static_cast<unsigned long long>(fnv1a64(bytes)), face_index);
  return std::string{buffer};
}

struct FontFace {
  std::string fingerprint;
  std::string family;
  std::vector<std::byte> bytes;
  FT_Face face{};
  hb_blob_t *hb_blob{};
  hb_face_t *hb_face{};
  hb_font_t *hb_font{};
  double units_per_em{1.0};
  double ascender_em{detail::font5x7_ascender_em};
  double descender_em{detail::font5x7_descender_em};
  bool builtin{};

  FontFace() = default;
  ~FontFace() {
    if (hb_font != nullptr) {
      hb_font_destroy(hb_font);
    }
    if (hb_face != nullptr) {
      hb_face_destroy(hb_face);
    }
    if (hb_blob != nullptr) {
      hb_blob_destroy(hb_blob);
    }
    if (face != nullptr) {
      FT_Done_Face(face);
    }
  }
  FontFace(const FontFace &) = delete;
  FontFace &operator=(const FontFace &) = delete;
};

[[nodiscard]] bool has_font_extension(const std::filesystem::path &path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".ttf" || extension == ".otf" ||
         extension == ".ttc" || extension == ".otc";
}

[[nodiscard]] std::vector<std::string> default_system_font_directories() {
#if defined(_WIN32)
  return {"C:/Windows/Fonts"};
#elif defined(__APPLE__)
  return {"/System/Library/Fonts", "/Library/Fonts"};
#else
  return {"/usr/share/fonts", "/usr/local/share/fonts"};
#endif
}

struct OutlineBuilder {
  GlyphOutline outline;
  double scale{1.0};
  bool contour_open{};

  void close_contour() {
    if (contour_open) {
      outline.commands.push_back(OutlineCommand{
          .verb = OutlineVerb::close,
          .coordinates = {},
      });
      contour_open = false;
    }
  }
};

int outline_move_to(const FT_Vector *to, void *user) {
  auto &builder = *static_cast<OutlineBuilder *>(user);
  builder.close_contour();
  builder.outline.commands.push_back(OutlineCommand{
      .verb = OutlineVerb::move_to,
      .coordinates = {static_cast<double>(to->x) * builder.scale,
                      static_cast<double>(to->y) * builder.scale},
  });
  builder.contour_open = true;
  return 0;
}

int outline_line_to(const FT_Vector *to, void *user) {
  auto &builder = *static_cast<OutlineBuilder *>(user);
  builder.outline.commands.push_back(OutlineCommand{
      .verb = OutlineVerb::line_to,
      .coordinates = {static_cast<double>(to->x) * builder.scale,
                      static_cast<double>(to->y) * builder.scale},
  });
  return 0;
}

int outline_conic_to(const FT_Vector *control, const FT_Vector *to,
                     void *user) {
  auto &builder = *static_cast<OutlineBuilder *>(user);
  builder.outline.commands.push_back(OutlineCommand{
      .verb = OutlineVerb::quadratic_to,
      .coordinates = {static_cast<double>(control->x) * builder.scale,
                      static_cast<double>(control->y) * builder.scale,
                      static_cast<double>(to->x) * builder.scale,
                      static_cast<double>(to->y) * builder.scale},
  });
  return 0;
}

int outline_cubic_to(const FT_Vector *first, const FT_Vector *second,
                     const FT_Vector *to, void *user) {
  auto &builder = *static_cast<OutlineBuilder *>(user);
  builder.outline.commands.push_back(OutlineCommand{
      .verb = OutlineVerb::cubic_to,
      .coordinates = {static_cast<double>(first->x) * builder.scale,
                      static_cast<double>(first->y) * builder.scale,
                      static_cast<double>(second->x) * builder.scale,
                      static_cast<double>(second->y) * builder.scale,
                      static_cast<double>(to->x) * builder.scale,
                      static_cast<double>(to->y) * builder.scale},
  });
  return 0;
}

[[nodiscard]] std::uint32_t builtin_glyph_id(char32_t code_point) {
  if (code_point < detail::font5x7_first_code_point ||
      code_point > detail::font5x7_last_code_point) {
    return 0;
  }
  return static_cast<std::uint32_t>(code_point -
                                    detail::font5x7_first_code_point + 1);
}

void append_rect_outline(GlyphOutline &outline, double left, double bottom,
                         double right, double top, bool clockwise = false) {
  outline.commands.push_back(OutlineCommand{
      .verb = OutlineVerb::move_to,
      .coordinates = {left, bottom},
  });
  if (clockwise) {
    outline.commands.push_back(OutlineCommand{
        .verb = OutlineVerb::line_to,
        .coordinates = {left, top},
    });
    outline.commands.push_back(OutlineCommand{
        .verb = OutlineVerb::line_to,
        .coordinates = {right, top},
    });
    outline.commands.push_back(OutlineCommand{
        .verb = OutlineVerb::line_to,
        .coordinates = {right, bottom},
    });
  } else {
    outline.commands.push_back(OutlineCommand{
        .verb = OutlineVerb::line_to,
        .coordinates = {right, bottom},
    });
    outline.commands.push_back(OutlineCommand{
        .verb = OutlineVerb::line_to,
        .coordinates = {right, top},
    });
    outline.commands.push_back(OutlineCommand{
        .verb = OutlineVerb::line_to,
        .coordinates = {left, top},
    });
  }
  outline.commands.push_back(OutlineCommand{
      .verb = OutlineVerb::close,
      .coordinates = {},
  });
}

} // namespace

struct HarfBuzzTextEngine::Impl {
  FT_Library library{};
  std::vector<std::unique_ptr<FontFace>> fonts;
  std::vector<std::string> system_directories;
  std::optional<std::uint32_t> builtin_index;
  std::uint32_t project_font_count{};
  // System-font scan caches (issue #474): the enumerated candidate list is
  // built once, and code points that a FULL scan could not cover are
  // remembered so shape() stops re-scanning every font directory for them.
  // Loading any new font invalidates the negative cache.
  std::optional<std::vector<std::filesystem::path>> system_font_candidates;
  std::set<char32_t> uncovered_code_points;

  Impl() = default;
  ~Impl() {
    fonts.clear();
    if (library != nullptr) {
      FT_Done_FreeType(library);
    }
  }
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  [[nodiscard]] bool ensure_library() {
    if (library != nullptr) {
      return true;
    }
    return FT_Init_FreeType(&library) == 0;
  }

  [[nodiscard]] std::uint32_t ensure_builtin() {
    if (builtin_index.has_value()) {
      return *builtin_index;
    }
    auto face = std::make_unique<FontFace>();
    face->fingerprint = std::string{detail::font5x7_fingerprint};
    face->family = "WellLog Built-in 5x7";
    face->units_per_em = 8.0;
    face->builtin = true;
    fonts.push_back(std::move(face));
    uncovered_code_points.clear();
    builtin_index = static_cast<std::uint32_t>(fonts.size() - 1);
    return *builtin_index;
  }

  // Project fonts always occupy the dense prefix [0, project_font_count).
  [[nodiscard]] std::uint32_t impl_project_count() const {
    return project_font_count;
  }

  [[nodiscard]] Result<std::uint32_t>
  adopt_file_face(std::vector<std::byte> bytes, long face_index) {
    if (!ensure_library()) {
      return font_error(MessageKey::font_load_failed);
    }
    if (fonts.size() >= maximum_fonts) {
      return Error{
          .code = ErrorCode::resource_exhausted,
          .severity = Severity::error,
          .entity_id = std::nullopt,
          .message = MessageKey::font_load_failed,
          .arguments = {},
      };
    }
    auto font = std::make_unique<FontFace>();
    FT_Face ft_face{};
    if (FT_New_Memory_Face(library,
                           reinterpret_cast<const FT_Byte *>(bytes.data()),
                           static_cast<FT_Long>(bytes.size()), face_index,
                           &ft_face) != 0) {
      return font_error(MessageKey::font_load_failed);
    }
    font->face = ft_face;
    // Shaping is table-driven (OpenType layout, not raster metrics) so
    // advances are deterministic font units independent of pixel sizes.
    font->hb_blob = hb_blob_create(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<unsigned int>(bytes.size()), HB_MEMORY_MODE_READONLY,
        nullptr, nullptr);
    font->hb_face = hb_face_create(font->hb_blob,
                                   static_cast<unsigned int>(face_index));
    font->hb_font = hb_font_create(font->hb_face);
    if (font->hb_blob == nullptr || font->hb_face == nullptr ||
        font->hb_font == nullptr) {
      return font_error(MessageKey::font_load_failed);
    }
    hb_ot_font_set_funcs(font->hb_font);
    font->units_per_em = static_cast<double>(hb_face_get_upem(font->hb_face));
    if (font->units_per_em <= 0.0) {
      return font_error(MessageKey::font_load_failed);
    }
    hb_font_set_scale(font->hb_font, static_cast<int>(font->units_per_em),
                      static_cast<int>(font->units_per_em));
    font->ascender_em = static_cast<double>(ft_face->ascender) /
                        font->units_per_em;
    font->descender_em = static_cast<double>(ft_face->descender) /
                         font->units_per_em;
    font->family =
        ft_face->family_name != nullptr ? ft_face->family_name : "unknown";
    font->fingerprint = fingerprint_for(bytes, face_index);
    font->bytes = std::move(bytes);
    fonts.push_back(std::move(font));
    uncovered_code_points.clear();
    return static_cast<std::uint32_t>(fonts.size() - 1);
  }

  [[nodiscard]] bool covers(std::uint32_t font_index,
                            char32_t code_point) const {
    const auto &font = *fonts[font_index];
    if (font.builtin) {
      return builtin_glyph_id(code_point) != 0;
    }
    hb_codepoint_t glyph = 0;
    return hb_font_get_nominal_glyph(
               font.hb_font, static_cast<hb_codepoint_t>(code_point),
               &glyph) != 0 &&
           glyph != 0;
  }

  // Loads system faces covering currently unresolved code points. Returns
  // the font index covering each code point, or nullopt when the whole
  // chain was exhausted.
  void scan_system_fonts(const std::vector<char32_t> &needed) {
    if (needed.empty() || !ensure_library()) {
      return;
    }
    // Negative cache: a previous FULL scan already proved these code points
    // have no covering system font — re-scanning every directory for them on
    // every shape() call was the repeat-stall (issue #474).
    std::set<char32_t> unresolved;
    for (const auto code_point : needed) {
      if (uncovered_code_points.find(code_point) ==
          uncovered_code_points.end()) {
        unresolved.insert(code_point);
      }
    }
    if (unresolved.empty()) {
      return;
    }
    if (!system_font_candidates.has_value()) {
      std::vector<std::filesystem::path> candidates;
      std::vector<std::string> directories = system_directories;
      const auto defaults = default_system_font_directories();
      directories.insert(directories.end(), defaults.begin(), defaults.end());
      for (const auto &directory : directories) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
          continue;
        }
        std::filesystem::recursive_directory_iterator iterator(
            directory,
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
          const auto &entry = *iterator;
          if (entry.is_regular_file(error) &&
              has_font_extension(entry.path())) {
            candidates.push_back(entry.path());
          }
          iterator.increment(error);
        }
      }
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()),
                       candidates.end());
      system_font_candidates = std::move(candidates);
    }
    const auto &candidates = *system_font_candidates;

    std::size_t scanned = 0;
    for (const auto &path : candidates) {
      if (unresolved.empty() || fonts.size() >= maximum_fonts ||
          scanned >= maximum_system_faces_per_scan) {
        break;
      }
      std::error_code error;
      const auto file_size = std::filesystem::file_size(path, error);
      if (error || file_size == 0 || file_size > maximum_font_file_bytes) {
        continue;
      }
      FT_Face probe{};
      if (FT_New_Face(library, path.c_str(), 0, &probe) != 0) {
        continue;
      }
      ++scanned;
      bool covers_any = false;
      for (const auto code_point : unresolved) {
        if (FT_Get_Char_Index(probe, static_cast<FT_ULong>(code_point)) !=
            0) {
          covers_any = true;
          break;
        }
      }
      FT_Done_Face(probe);
      if (!covers_any) {
        continue;
      }
      std::ifstream stream(path, std::ios::binary);
      if (!stream) {
        continue;
      }
      std::vector<std::byte> bytes(file_size);
      stream.read(reinterpret_cast<char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
      if (!stream) {
        continue;
      }
      auto adopted = adopt_file_face(std::move(bytes), 0);
      if (!adopted.has_value()) {
        continue;
      }
      const auto index = adopted.value();
      for (auto iterator = unresolved.begin(); iterator != unresolved.end();) {
        if (covers(index, *iterator)) {
          iterator = unresolved.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
    // Whatever a full scan could not cover stays uncovered until a new font
    // is loaded (which clears the negative cache).
    uncovered_code_points.insert(unresolved.begin(), unresolved.end());
  }

  [[nodiscard]] Result<ShapedRun> shape(const TextShapeRequest &request) {
    if (!is_valid_utf8(request.text)) {
      return Error{
          .code = ErrorCode::invalid_document,
          .severity = Severity::error,
          .entity_id = std::nullopt,
          .message = MessageKey::text_encoding_invalid,
          .arguments = {},
      };
    }
    ShapedRun run;
    const auto code_points = detail::decode_utf8(request.text);
    if (code_points.empty()) {
      return run;
    }
    const auto builtin = ensure_builtin();
    const auto project_count = impl_project_count();

    // Font assignment: project fonts in registration order, then the
    // built-in fallback, then system fonts scanned on demand.
    std::vector<std::optional<std::uint32_t>> assignment(
        code_points.size());
    std::vector<char32_t> unresolved;
    for (std::size_t index = 0; index < code_points.size(); ++index) {
      const auto code_point = code_points[index].code_point;
      for (std::uint32_t font = 0; font < project_count; ++font) {
        if (covers(font, code_point)) {
          assignment[index] = font;
          break;
        }
      }
      if (!assignment[index].has_value()) {
        if (covers(builtin, code_point)) {
          assignment[index] = builtin;
        } else {
          unresolved.push_back(code_point);
        }
      }
    }
    if (!unresolved.empty()) {
      scan_system_fonts(unresolved);
      for (std::size_t index = 0; index < code_points.size(); ++index) {
        if (assignment[index].has_value()) {
          continue;
        }
        const auto code_point = code_points[index].code_point;
        for (std::uint32_t font = 0; font < fonts.size(); ++font) {
          if (covers(font, code_point)) {
            assignment[index] = font;
            break;
          }
        }
      }
    }

    // Grapheme clusters (ICU BreakIterator, created per shaping call so
    // every worker thread uses its own iterator) keep one font whenever
    // the base character's font covers the whole cluster; shaping then
    // sees the cluster as one HarfBuzz segment.
    {
      std::vector<std::int32_t> utf16_offsets;
      utf16_offsets.reserve(code_points.size() + 1);
      std::int32_t utf16_length = 0;
      for (const auto &code_point : code_points) {
        utf16_offsets.push_back(utf16_length);
        utf16_length += code_point.code_point > 0xFFFF ? 2 : 1;
      }
      utf16_offsets.push_back(utf16_length);
      std::vector<UChar> utf16(static_cast<std::size_t>(utf16_length));
      UErrorCode conversion_status = U_ZERO_ERROR;
      u_strFromUTF8(utf16.data(), utf16_length, nullptr,
                    request.text.data(),
                    static_cast<std::int32_t>(request.text.size()),
                    &conversion_status);
      UErrorCode status = U_ZERO_ERROR;
      UBreakIterator *iterator =
          ubrk_open(UBRK_CHARACTER, "root", utf16.data(), utf16_length,
                    &status);
      if (U_SUCCESS(status) && iterator != nullptr) {
        std::vector<std::uint32_t> cluster_of(code_points.size());
        std::uint32_t cluster = 0;
        auto boundary = ubrk_first(iterator);
        for (std::size_t index = 0; index < code_points.size(); ++index) {
          while (boundary != UBRK_DONE &&
                 boundary <= utf16_offsets[index]) {
            if (boundary > 0) {
              ++cluster;
            }
            boundary = ubrk_next(iterator);
          }
          cluster_of[index] = cluster;
        }
        ubrk_close(iterator);
        std::size_t cluster_begin = 0;
        while (cluster_begin < code_points.size()) {
          std::size_t cluster_end = cluster_begin + 1;
          while (cluster_end < code_points.size() &&
                 cluster_of[cluster_end] == cluster_of[cluster_begin]) {
            ++cluster_end;
          }
          const auto cluster_font = assignment[cluster_begin];
          if (cluster_font.has_value()) {
            for (std::size_t index = cluster_begin + 1; index < cluster_end;
                 ++index) {
              if (covers(*cluster_font, code_points[index].code_point)) {
                assignment[index] = cluster_font;
              }
            }
          }
          cluster_begin = cluster_end;
        }
      }
    }

    std::set<char32_t> missing;
    std::set<std::uint32_t> used_fonts;
    // Itemize consecutive code points sharing a font, then shape each
    // segment with HarfBuzz. Unresolved code points become an explicit
    // replacement glyph from the built-in font and are reported.
    std::size_t segment_begin = 0;
    while (segment_begin < code_points.size()) {
      const auto segment_font = assignment[segment_begin];
      std::size_t segment_end = segment_begin + 1;
      while (segment_end < code_points.size() &&
             assignment[segment_end] == segment_font) {
        ++segment_end;
      }
      const auto byte_begin = code_points[segment_begin].byte_offset;
      const auto last = code_points[segment_end - 1];
      const auto byte_end = last.byte_offset + last.byte_length;
      const auto *segment_text = request.text.data() + byte_begin;
      const auto segment_length = byte_end - byte_begin;

      if (!segment_font.has_value()) {
        // Explicit, diagnosed replacement: never a silent .notdef box.
        for (std::size_t index = segment_begin; index < segment_end;
             ++index) {
          missing.insert(code_points[index].code_point);
          used_fonts.insert(builtin);
          run.glyphs.push_back(ShapedGlyph{
              .glyph_id = builtin_glyph_id(replacement_code_point),
              .font_index = builtin,
              .cluster = code_points[index].byte_offset,
              .code_point = code_points[index].code_point,
              .advance_x = detail::font5x7_advance_em,
              .advance_y = 0.0,
              .offset_x = 0.0,
              .offset_y = 0.0,
              .upright = true,
          });
        }
        segment_begin = segment_end;
        continue;
      }

      const auto &font = *fonts[*segment_font];
      used_fonts.insert(*segment_font);
      if (font.builtin) {
        // The dot-matrix fallback maps one glyph per code point and needs
        // no shaping.
        for (std::size_t index = segment_begin; index < segment_end;
             ++index) {
          run.glyphs.push_back(ShapedGlyph{
              .glyph_id = builtin_glyph_id(code_points[index].code_point),
              .font_index = *segment_font,
              .cluster = code_points[index].byte_offset,
              .code_point = code_points[index].code_point,
              .advance_x = detail::font5x7_advance_em,
              .advance_y = 0.0,
              .offset_x = 0.0,
              .offset_y = 0.0,
              .upright = true,
          });
        }
        segment_begin = segment_end;
        continue;
      }
      hb_buffer_t *buffer = hb_buffer_create();
      if (buffer == nullptr) {
        return font_error(MessageKey::font_load_failed);
      }
      hb_buffer_add_utf8(buffer, segment_text,
                         static_cast<int>(segment_length), 0,
                         static_cast<int>(segment_length));
      hb_buffer_guess_segment_properties(buffer);
      hb_buffer_set_direction(
          buffer, request.direction == TextDirection::right_to_left
                      ? HB_DIRECTION_RTL
                      : HB_DIRECTION_LTR);
      if (!request.language.empty()) {
        hb_buffer_set_language(
            buffer, hb_language_from_string(request.language.data(),
                                            static_cast<int>(
                                                request.language.size())));
      }
      hb_shape(font.hb_font, buffer, nullptr, 0);
      unsigned int glyph_count = 0;
      const auto *infos = hb_buffer_get_glyph_infos(buffer, &glyph_count);
      const auto *positions =
          hb_buffer_get_glyph_positions(buffer, &glyph_count);
      // The segment's code_points are sorted by byte_offset; binary-search
      // each glyph's cluster instead of re-walking the segment per glyph
      // (O(glyphs x segment) -> O(glyphs x log segment), issue #484).
      const auto segment_cp_begin =
          code_points.begin() + static_cast<std::ptrdiff_t>(segment_begin);
      const auto segment_cp_end =
          code_points.begin() + static_cast<std::ptrdiff_t>(segment_end);
      for (unsigned int glyph = 0; glyph < glyph_count; ++glyph) {
        const auto cluster = static_cast<std::uint32_t>(byte_begin) +
                             infos[glyph].cluster;
        char32_t code_point = 0;
        const auto upper = std::upper_bound(
            segment_cp_begin, segment_cp_end, cluster,
            [](std::uint32_t value, const auto &cp) {
              return value < cp.byte_offset;
            });
        if (upper != segment_cp_begin) {
          code_point = std::prev(upper)->code_point;
        }
        run.glyphs.push_back(ShapedGlyph{
            .glyph_id = infos[glyph].codepoint,
            .font_index = *segment_font,
            .cluster = cluster,
            .code_point = code_point,
            .advance_x = static_cast<double>(positions[glyph].x_advance) /
                         font.units_per_em,
            .advance_y = static_cast<double>(positions[glyph].y_advance) /
                         font.units_per_em,
            .offset_x = static_cast<double>(positions[glyph].x_offset) /
                        font.units_per_em,
            .offset_y = static_cast<double>(positions[glyph].y_offset) /
                        font.units_per_em,
            .upright = true,
        });
      }
      hb_buffer_destroy(buffer);
      segment_begin = segment_end;
    }

    if (request.direction == TextDirection::top_to_bottom) {
      for (auto &glyph : run.glyphs) {
        const auto orientation = u_getIntPropertyValue(
            static_cast<UChar32>(glyph.code_point),
            UCHAR_VERTICAL_ORIENTATION);
        glyph.upright = orientation == U_VO_UPRIGHT ||
                        orientation == U_VO_TRANSFORMED_UPRIGHT;
        if (glyph.upright) {
          glyph.advance_y = -1.0;
        } else {
          glyph.advance_y = -glyph.advance_x;
        }
        glyph.advance_x = 0.0;
      }
    }

    const auto primary = run.glyphs.empty()
                             ? builtin
                             : run.glyphs.front().font_index;
    run.ascender = fonts[primary]->ascender_em;
    run.descender = fonts[primary]->descender_em;
    run.missing_code_points.assign(missing.begin(), missing.end());
    run.used_fallback_font = used_fonts.size() > 1 || !missing.empty();
    return run;
  }

  [[nodiscard]] Result<GlyphOutline>
  outline(std::uint32_t font_index, std::uint32_t glyph_id) {
    if (font_index >= fonts.size()) {
      return font_error(MessageKey::font_glyph_unavailable);
    }
    const auto &font = *fonts[font_index];
    GlyphOutline outline;
    if (font.builtin) {
      if (glyph_id == 0 ||
          glyph_id > (detail::font5x7_last_code_point -
                      detail::font5x7_first_code_point + 1)) {
        // Explicit replacement: a hollow box, always diagnosed upstream.
        append_rect_outline(outline, 0.0, 0.0, 5.0 * detail::font5x7_cell_em,
                            7.0 * detail::font5x7_cell_em);
        append_rect_outline(outline, detail::font5x7_cell_em,
                            detail::font5x7_cell_em,
                            4.0 * detail::font5x7_cell_em,
                            6.0 * detail::font5x7_cell_em, true);
      } else {
        const auto &columns = detail::font5x7_glyphs[glyph_id - 1];
        for (std::size_t column = 0; column < columns.size(); ++column) {
          for (std::size_t row = 0; row < 7; ++row) {
            if ((columns[column] & (std::uint8_t{1} << row)) == 0) {
              continue;
            }
            const auto left =
                static_cast<double>(column) * detail::font5x7_cell_em;
            const auto top =
                static_cast<double>(6 - row) * detail::font5x7_cell_em;
            append_rect_outline(outline, left, top, left +
                                                     detail::font5x7_cell_em,
                                top + detail::font5x7_cell_em);
          }
        }
      }
      outline.advance_x = detail::font5x7_advance_em;
      outline.left = 0.0;
      outline.bottom = 0.0;
      outline.right = 5.0 * detail::font5x7_cell_em;
      outline.top = 7.0 * detail::font5x7_cell_em;
      return outline;
    }

    if (FT_Load_Glyph(font.face, glyph_id, FT_LOAD_NO_SCALE) != 0) {
      return font_error(MessageKey::font_glyph_unavailable);
    }
    OutlineBuilder builder;
    builder.scale = 1.0 / font.units_per_em;
    const FT_Outline_Funcs funcs{
        .move_to = &outline_move_to,
        .line_to = &outline_line_to,
        .conic_to = &outline_conic_to,
        .cubic_to = &outline_cubic_to,
        .shift = 0,
        .delta = 0,
    };
    if (FT_Outline_Decompose(&font.face->glyph->outline, &funcs, &builder) !=
        0) {
      return font_error(MessageKey::font_glyph_unavailable);
    }
    builder.close_contour();
    outline = std::move(builder.outline);
    outline.advance_x = static_cast<double>(font.face->glyph->advance.x) /
                        font.units_per_em;
    FT_BBox box{};
    FT_Outline_Get_CBox(&font.face->glyph->outline, &box);
    outline.left = static_cast<double>(box.xMin) / font.units_per_em;
    outline.right = static_cast<double>(box.xMax) / font.units_per_em;
    outline.bottom = static_cast<double>(box.yMin) / font.units_per_em;
    outline.top = static_cast<double>(box.yMax) / font.units_per_em;
    return outline;
  }
};

HarfBuzzTextEngine::HarfBuzzTextEngine() : impl_(std::make_unique<Impl>()) {}
HarfBuzzTextEngine::~HarfBuzzTextEngine() = default;
HarfBuzzTextEngine::HarfBuzzTextEngine(HarfBuzzTextEngine &&) noexcept =
    default;
HarfBuzzTextEngine &
HarfBuzzTextEngine::operator=(HarfBuzzTextEngine &&) noexcept = default;

Result<std::uint32_t>
HarfBuzzTextEngine::add_project_font(std::string_view path) noexcept {
  try {
    std::error_code error;
    const auto file_size =
        std::filesystem::file_size(std::filesystem::path{path}, error);
    if (error || file_size == 0 || file_size > maximum_font_file_bytes) {
      return font_error(MessageKey::font_load_failed);
    }
    std::ifstream stream(std::filesystem::path{path}, std::ios::binary);
    if (!stream) {
      return font_error(MessageKey::font_load_failed);
    }
    std::vector<std::byte> bytes(file_size);
    stream.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      return font_error(MessageKey::font_load_failed);
    }
    auto adopted = impl_->adopt_file_face(std::move(bytes), 0);
    if (adopted.has_value()) {
      ++impl_->project_font_count;
    }
    return adopted;
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return font_error(MessageKey::font_load_failed);
  }
}

void HarfBuzzTextEngine::add_system_font_directory(
    std::string_view path) noexcept {
  try {
    impl_->system_directories.emplace_back(path);
  } catch (...) {
  }
}

Result<ShapedRun>
HarfBuzzTextEngine::shape(const TextShapeRequest &request) noexcept {
  try {
    return impl_->shape(request);
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return font_error(MessageKey::internal_error);
  }
}

Result<GlyphOutline>
HarfBuzzTextEngine::glyph_outline(std::uint32_t font_index,
                                  std::uint32_t glyph_id) noexcept {
  try {
    return impl_->outline(font_index, glyph_id);
  } catch (const std::bad_alloc &) {
    return Error{
        .code = ErrorCode::resource_exhausted,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::resource_exhausted,
        .arguments = {},
    };
  } catch (...) {
    return font_error(MessageKey::internal_error);
  }
}

std::string
HarfBuzzTextEngine::font_fingerprint(std::uint32_t font_index) const {
  if (font_index >= impl_->fonts.size()) {
    return {};
  }
  return impl_->fonts[font_index]->fingerprint;
}

std::string
HarfBuzzTextEngine::font_family_name(std::uint32_t font_index) const {
  if (font_index >= impl_->fonts.size()) {
    return {};
  }
  return impl_->fonts[font_index]->family;
}

} // namespace welllog
