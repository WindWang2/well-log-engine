// Minimal hand-rolled PDF writer (ADR: PDF via hand-rolled writer, #185 spike).
//
// Produces a byte-deterministic, Flate-compressed multi-page PDF with no
// third-party PDF library (only zlib for stream compression). The structure is
// the standard one: a header, an object for each indirect object (Catalog,
// Pages, one Page + one content stream per page), an xref table, and a trailer.
// Determinism is by construction: no CreationDate/ModDate and no /ID are
// emitted, so identical inputs always produce identical output.
//
// The path-operator vocabulary mirrors the SVG emitter; quadratic curves lift
// to cubics because PDF has no quadratic Bezier operator.

#include <welllog/export/pdf.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace welllog {
namespace {

// Appends a double as a compact PDF number (general format, no trailing zeros).
// Mirrors the SVG emitter's append_number for consistency.
void append_number(std::string &out, double value) {
  if (value == 0.0) {
    out.push_back('0');
    return;
  }
  std::array<char, 48> buffer{};
  // No explicit precision: to_chars then emits the SHORTEST round-trip
  // representation (max_digits10 digits is never shortest — it padded every
  // coordinate with numeric noise and 2-5x'd export file sizes).
  const auto res =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (res.ec != std::errc{}) {
    out.push_back('0');
    return;
  }
  out.append(buffer.data(), res.ptr);
}

// Appends an integer.
void append_integer(std::string &out, std::int64_t value) {
  std::array<char, 24> buffer{};
  const auto res =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (res.ec == std::errc{}) {
    out.append(buffer.data(), res.ptr);
  }
}

// Lifts a quadratic Bezier (p0, c, p1) to a cubic using the standard identity:
//   c1 = p0 + 2/3 (c - p0)
//   c2 = c + 1/3 (p1 - c)
// p0 is the current point carried by the caller; we only need c and p1 here
// because the cubic's first control point is derived from p0.
struct CubicFromQuadratic {
  double c1x, c1y, c2x, c2y, x, y;
};
[[nodiscard]] CubicFromQuadratic lift_quadratic(double p0x, double p0y,
                                                double cx, double cy,
                                                double x, double y) noexcept {
  return {.c1x = p0x + (2.0 / 3.0) * (cx - p0x),
          .c1y = p0y + (2.0 / 3.0) * (cy - p0y),
          .c2x = cx + (1.0 / 3.0) * (x - cx),
          .c2y = cy + (1.0 / 3.0) * (y - cy),
          .x = x,
          .y = y};
}

// Compresses a byte range with zlib (FlateDecode). Returns false on zlib error.
[[nodiscard]] bool flate_compress_view(std::string_view input,
                                       std::string &output) noexcept {
  z_stream stream{};
  // deflateInit2 with windowBits=15 (zlib format — PDF FlateDecode expects the
  // zlib 2-byte header, not raw deflate).
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  output.resize(deflateBound(&stream, input.size()));
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef *>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  const auto rc = deflate(&stream, Z_FINISH);
  deflateEnd(&stream);
  if (rc != Z_STREAM_END) {
    return false;
  }
  output.resize(stream.total_out);
  return true;
}

// Compresses a std::string (legacy call sites).
[[nodiscard]] bool flate_compress(const std::string &input,
                                  std::string &output) noexcept {
  return flate_compress_view(std::string_view{input}, output);
}

[[nodiscard]] Error pdf_error(ErrorCode code, MessageKey message) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = message,
      .arguments = {},
  };
}

} // namespace

struct PdfPathStream::Impl {
  std::string operators;
  // Last move-to point, carried so a following quadratic_to can lift to cubic.
  double current_x{};
  double current_y{};
  // Distinct fill-alpha values this stream emitted `gs /GSn` for, kept in
  // first-encountered order (deduplicated) so the writer's /ExtGState objects
  // are named in the exact order the stream assigns /GSn names.
  std::vector<double> fill_alphas;
  // B1.PDF.2: draw_standard_text was used → writer must name /F1 Helvetica.
  bool uses_standard_font{false};
  // B1.PDF.3: non-Latin-1 code points dropped from searchable overlay.
  std::uint32_t non_latin_codepoints_dropped{0};
};

PdfPathStream::PdfPathStream() : impl_(std::make_unique<Impl>()) {}
PdfPathStream::~PdfPathStream() = default;
PdfPathStream::PdfPathStream(const PdfPathStream &other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}
PdfPathStream &PdfPathStream::operator=(const PdfPathStream &other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>(*other.impl_);
  }
  return *this;
}
PdfPathStream::PdfPathStream(PdfPathStream &&) noexcept = default;
PdfPathStream &PdfPathStream::operator=(PdfPathStream &&) noexcept = default;

PdfPathStream &PdfPathStream::move_to(double x, double y) noexcept {
  append_number(impl_->operators, x);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, y);
  impl_->operators += " m\n";
  impl_->current_x = x;
  impl_->current_y = y;
  return *this;
}

PdfPathStream &PdfPathStream::line_to(double x, double y) noexcept {
  append_number(impl_->operators, x);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, y);
  impl_->operators += " l\n";
  impl_->current_x = x;
  impl_->current_y = y;
  return *this;
}

PdfPathStream &PdfPathStream::append_outline(std::span<const OutlineCommand> commands,
                                             double scale, double dx,
                                             double dy) noexcept {
  // PDF user space is y-up; OutlineCommand is also y-up (em fractions, glyph
  // local), so no y-flip is needed — only scale + translate. We track the
  // current point so a quadratic_to can be lifted to a cubic.
  for (const auto &command : commands) {
    switch (command.verb) {
    case OutlineVerb::move_to: {
      const auto x = dx + scale * command.coordinates[0];
      const auto y = dy + scale * command.coordinates[1];
      move_to(x, y);
      break;
    }
    case OutlineVerb::line_to: {
      const auto x = dx + scale * command.coordinates[0];
      const auto y = dy + scale * command.coordinates[1];
      line_to(x, y);
      break;
    }
    case OutlineVerb::quadratic_to: {
      const auto cx = dx + scale * command.coordinates[0];
      const auto cy = dy + scale * command.coordinates[1];
      const auto x = dx + scale * command.coordinates[2];
      const auto y = dy + scale * command.coordinates[3];
      const auto lifted = lift_quadratic(impl_->current_x, impl_->current_y,
                                         cx, cy, x, y);
      append_number(impl_->operators, lifted.c1x);
      impl_->operators.push_back(' ');
      append_number(impl_->operators, lifted.c1y);
      impl_->operators.push_back(' ');
      append_number(impl_->operators, lifted.c2x);
      impl_->operators.push_back(' ');
      append_number(impl_->operators, lifted.c2y);
      impl_->operators.push_back(' ');
      append_number(impl_->operators, lifted.x);
      impl_->operators.push_back(' ');
      append_number(impl_->operators, lifted.y);
      impl_->operators += " c\n";
      impl_->current_x = x;
      impl_->current_y = y;
      break;
    }
    case OutlineVerb::cubic_to: {
      for (std::size_t i = 0; i < 6; ++i) {
        append_number(impl_->operators,
                      dx + scale * command.coordinates[i]);
        impl_->operators.push_back(' ');
      }
      impl_->operators += "c\n";
      impl_->current_x = dx + scale * command.coordinates[4];
      impl_->current_y = dy + scale * command.coordinates[5];
      break;
    }
    case OutlineVerb::close:
      close();
      break;
    }
  }
  return *this;
}

PdfPathStream &PdfPathStream::close() noexcept {
  impl_->operators += "h\n";
  return *this;
}

PdfPathStream &PdfPathStream::fill() noexcept {
  impl_->operators += "f\n";
  return *this;
}

PdfPathStream &PdfPathStream::stroke() noexcept {
  impl_->operators += "S\n";
  return *this;
}

PdfPathStream &PdfPathStream::set_fill_color(std::uint8_t r, std::uint8_t g,
                                             std::uint8_t b) noexcept {
  append_number(impl_->operators, r / 255.0);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, g / 255.0);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, b / 255.0);
  impl_->operators += " rg\n";
  return *this;
}

PdfPathStream &PdfPathStream::set_stroke_color(std::uint8_t r, std::uint8_t g,
                                               std::uint8_t b) noexcept {
  append_number(impl_->operators, r / 255.0);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, g / 255.0);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, b / 255.0);
  impl_->operators += " RG\n";
  return *this;
}

PdfPathStream &PdfPathStream::set_line_width(double width) noexcept {
  append_number(impl_->operators, width);
  impl_->operators += " w\n";
  return *this;
}

PdfPathStream &PdfPathStream::set_dash(std::span<const double> dash_array,
                                       double phase) noexcept {
  impl_->operators += "[";
  for (std::size_t i = 0; i < dash_array.size(); ++i) {
    if (i > 0) {
      impl_->operators.push_back(' ');
    }
    append_number(impl_->operators, dash_array[i]);
  }
  impl_->operators += "] ";
  append_number(impl_->operators, phase);
  impl_->operators += " d\n";
  return *this;
}

PdfPathStream &PdfPathStream::save_state() noexcept {
  impl_->operators += "q\n";
  return *this;
}

PdfPathStream &PdfPathStream::restore_state() noexcept {
  impl_->operators += "Q\n";
  return *this;
}

PdfPathStream &PdfPathStream::mark_begin(std::uint32_t layer_index) noexcept {
  impl_->operators += "/Lay";
  append_integer(impl_->operators, static_cast<std::int64_t>(layer_index));
  impl_->operators += " OC BMC\n";
  return *this;
}

PdfPathStream &PdfPathStream::mark_end() noexcept {
  impl_->operators += "EMC\n";
  return *this;
}

PdfPathStream &PdfPathStream::concat_matrix(double a, double b, double c,
                                            double d, double e,
                                            double f) noexcept {
  append_number(impl_->operators, a);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, b);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, c);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, d);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, e);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, f);
  impl_->operators += " cm\n";
  return *this;
}

PdfPathStream &PdfPathStream::rect(double x, double y, double width,
                                   double height) noexcept {
  append_number(impl_->operators, x);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, y);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, width);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, height);
  impl_->operators += " re\n";
  return *this;
}

PdfPathStream &PdfPathStream::clip_nonzero() noexcept {
  impl_->operators += "W\n";
  return *this;
}

PdfPathStream &PdfPathStream::end_path_no_paint() noexcept {
  impl_->operators += "n\n";
  return *this;
}

// Clamps an alpha into PDF's [0,1] range, then records it (dedup, in
// first-encountered order) and emits `gs /GSn` where n is the value's index in
// that insertion-ordered list. Insertion order (not sorted) is essential: the
// name is assigned at the moment `set_fill_alpha` is called, so it must match
// the writer's Resources/ExtGState dictionary, which names the objects in the
// SAME order they appear here. Sorted order would re-index alphas retroactively
// and desync the per-stream `gs` operators from the page's /GSi→object map.
PdfPathStream &PdfPathStream::set_fill_alpha(double alpha) noexcept {
  if (!std::isfinite(alpha) || alpha < 0.0) {
    alpha = 0.0;
  } else if (alpha > 1.0) {
    alpha = 1.0;
  }
  auto &alphas = impl_->fill_alphas;
  std::size_t index = alphas.size();
  for (std::size_t i = 0; i < alphas.size(); ++i) {
    if (alphas[i] == alpha) {
      index = i;
      break;
    }
  }
  if (index == alphas.size()) {
    alphas.push_back(alpha);
  }
  impl_->operators += "/GS";
  append_integer(impl_->operators, static_cast<std::int64_t>(index));
  impl_->operators += " gs\n";
  return *this;
}

PdfPathStream &PdfPathStream::set_pattern_fill(
    std::string_view pattern_name) noexcept {
  // Switch the non-stroking colour space to /Pattern, select the named tiling
  // pattern, then paint the current path with it (`f`).
  impl_->operators += "/Pattern cs\n/";
  impl_->operators.append(pattern_name.data(), pattern_name.size());
  impl_->operators += " scn\nf\n";
  return *this;
}

PdfPathStream &PdfPathStream::invoke_xobject(std::string_view name) noexcept {
  impl_->operators.push_back('/');
  impl_->operators.append(name.data(), name.size());
  impl_->operators += " Do\n";
  return *this;
}

PdfPathStream &PdfPathStream::draw_standard_text(double x, double y,
                                                double font_size,
                                                std::string_view text) noexcept {
  // UTF-8 → WinAnsi Latin-1 for Base-14 Helvetica (B1.PDF.3). CJK dropped.
  std::string filtered;
  filtered.reserve(text.size());
  const auto *p = reinterpret_cast<const unsigned char *>(text.data());
  const auto *end = p + text.size();
  while (p < end) {
    std::uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
      cp = (static_cast<std::uint32_t>(p[0] & 0x1F) << 6) |
           static_cast<std::uint32_t>(p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
      cp = (static_cast<std::uint32_t>(p[0] & 0x0F) << 12) |
           (static_cast<std::uint32_t>(p[1] & 0x3F) << 6) |
           static_cast<std::uint32_t>(p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
      cp = (static_cast<std::uint32_t>(p[0] & 0x07) << 18) |
           (static_cast<std::uint32_t>(p[1] & 0x3F) << 12) |
           (static_cast<std::uint32_t>(p[2] & 0x3F) << 6) |
           static_cast<std::uint32_t>(p[3] & 0x3F);
      p += 4;
    } else {
      ++p;
      impl_->non_latin_codepoints_dropped += 1;
      continue;
    }
    if (cp >= 32 && cp < 127) {
      if (cp == '(' || cp == ')' || cp == '\\') {
        filtered.push_back('\\');
      }
      filtered.push_back(static_cast<char>(cp));
    } else if (cp >= 0xA0 && cp <= 0xFF) {
      // WinAnsi high byte as octal escape.
      const auto b = static_cast<unsigned>(cp);
      filtered.push_back('\\');
      filtered.push_back(static_cast<char>('0' + ((b >> 6) & 7)));
      filtered.push_back(static_cast<char>('0' + ((b >> 3) & 7)));
      filtered.push_back(static_cast<char>('0' + (b & 7)));
    } else if (cp == 0x09 || cp == 0x0A || cp == 0x0D) {
      // skip whitespace control
    } else {
      impl_->non_latin_codepoints_dropped += 1;
    }
  }
  if (filtered.empty() || !std::isfinite(font_size) || font_size <= 0.0) {
    return *this;
  }
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return *this;
  }
  impl_->uses_standard_font = true;
  impl_->operators += "BT\n/F1 ";
  append_number(impl_->operators, font_size);
  impl_->operators += " Tf\n";
  append_number(impl_->operators, x);
  impl_->operators.push_back(' ');
  append_number(impl_->operators, y);
  impl_->operators += " Td\n(";
  impl_->operators += filtered;
  impl_->operators += ") Tj\nET\n";
  return *this;
}

std::string_view PdfPathStream::operators() const noexcept {
  return impl_->operators;
}

std::span<const double> PdfPathStream::fill_alphas() const noexcept {
  return impl_->fill_alphas;
}

bool PdfPathStream::needs_standard_font() const noexcept {
  return impl_->uses_standard_font;
}

std::uint32_t PdfPathStream::non_latin_codepoints_dropped() const noexcept {
  return impl_->non_latin_codepoints_dropped;
}

struct PdfDocument::Impl {
  std::string bytes;
};

PdfDocument::PdfDocument() = default;
PdfDocument::~PdfDocument() = default;
PdfDocument::PdfDocument(const PdfDocument &) = default;
PdfDocument &PdfDocument::operator=(const PdfDocument &) = default;
PdfDocument::PdfDocument(PdfDocument &&) noexcept = default;
PdfDocument &PdfDocument::operator=(PdfDocument &&) noexcept = default;
PdfDocument::PdfDocument(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

std::string_view PdfDocument::bytes() const noexcept {
  return impl_ == nullptr ? std::string_view{} : std::string_view{impl_->bytes};
}

// Builds the PDF byte stream. Object layout (1-based object numbers), allocated
// up front so the count is known before any object body references another:
//   1: Catalog
//   2: Pages
//   per page p (0-based):  content stream = 3 + 2*p ;  Page = 3 + 2*p + 1
//   per-page /ExtGState objects (one per distinct fill-alpha THAT PAGE uses)
//   follow all page objects, numbered 3 + 2*N + (per-page running offset).
//   caller-supplied objects (images/patterns), then the global OCG objects
//   (layered PDF, one per entry in `layers`) follow.
// The xref table follows, then trailer. All offsets are deterministic given the
// (deterministic) content streams. Each page's /ExtGState dictionary maps its
// own local /GSn names (n = index in that page's first-encountered-order,
// deduplicated alpha list) to its own ExtGState objects, so the per-stream
// `gs /GSn` operators resolve correctly regardless of what alphas other pages
// use. The first-encountered order (not sorted) is what the stream assigns names
// in, so the writer must name objects in that exact same order.
Result<PdfDocument> PdfWriter::write(std::span<const PdfPageContent> pages,
                                     std::span<const PdfPageSpec> page_specs,
                                     std::span<const std::string> layers) noexcept {
  try {
    if (pages.empty()) {
      return pdf_error(ErrorCode::invalid_buffer,
                       MessageKey::buffer_data_required);
    }
    std::vector<PdfPageSpec> specs(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
      specs[i] = i < page_specs.size() ? page_specs[i] : PdfPageSpec{};
    }

    // Each page's distinct fill-alphas (already sorted-unique per stream). The
    // /GSn names a stream emits are indices into ITS OWN list, so ExtGState
    // objects and the Resources/ExtGState dictionary are page-local — no cross-
    // page name collision, no global re-indexing needed.
    std::vector<std::span<const double>> page_alphas(pages.size());
    for (std::size_t p = 0; p < pages.size(); ++p) {
      page_alphas[p] = pages[p].stream.fill_alphas();
    }
    // Per-page ExtGState object-number offsets (after all the page objects).
    // Page p's alpha g maps to object (ext_gstate_base + running_sum_to_p + g).
    const std::size_t ext_gstate_base = 3 + 2 * pages.size();
    std::vector<std::size_t> page_alpha_base(pages.size() + 1);
    page_alpha_base[0] = ext_gstate_base;
    for (std::size_t p = 0; p < pages.size(); ++p) {
      page_alpha_base[p + 1] = page_alpha_base[p] + page_alphas[p].size();
    }
    const std::size_t total_alpha_objects =
        page_alpha_base[pages.size()] - ext_gstate_base;

    // Per-page caller-supplied indirect objects (#188: image XObjects + tiling
    // patterns). They are numbered AFTER the ExtGState objects, page by page in
    // `objects` order. page_object_base[p] is the first object number page p's
    // objects occupy; objects are emitted in the order given (the exporter is
    // responsible for deterministic ordering).
    const std::size_t object_base = ext_gstate_base + total_alpha_objects;
    std::vector<std::size_t> page_object_base(pages.size() + 1);
    page_object_base[0] = object_base;
    for (std::size_t p = 0; p < pages.size(); ++p) {
      page_object_base[p + 1] =
          page_object_base[p] + pages[p].objects.size();
    }
    const std::size_t total_caller_objects =
        page_object_base[pages.size()] - object_base;

    // Global OCG objects (layered PDF, FRS §5): one per `layers` entry, after
    // all caller objects. Page streams reference them as /Lay<i> where i is the
    // index into `layers`; the per-page /Properties dict maps each used index.
    const std::size_t ocg_base = object_base + total_caller_objects;
    const std::size_t total_ocg_objects = layers.size();

    // Compress every page's content stream once (deterministic) so its length
    // is known when the content-stream object is written.
    std::vector<std::string> compressed(pages.size());
    for (std::size_t p = 0; p < pages.size(); ++p) {
      if (!flate_compress(std::string(pages[p].stream.operators()),
                          compressed[p])) {
        return pdf_error(ErrorCode::internal_error,
                         MessageKey::internal_error);
      }
    }

    std::string out;
    out.reserve(4096);
    out += "%PDF-1.7\n";
    // Binary comment marking the file as binary (4 high-bit bytes) — standard
    // practice; helps tools detect binary content.
    out += "%\xE2\xE3\xCF\xD3\n";

    // Record the byte offset of each indirect object for the xref table.
    std::vector<std::size_t> offsets;
    const auto reserve_object = [&](std::size_t object_number) {
      if (offsets.size() <= object_number) {
        offsets.resize(object_number + 1);
      }
    };
    auto emit_object_header = [&](std::size_t object_number) -> std::size_t {
      reserve_object(object_number);
      offsets[object_number] = out.size();
      append_integer(out, static_cast<std::int64_t>(object_number));
      out += " 0 obj\n";
      return object_number;
    };

    // Object 1: Catalog. Layered PDF adds an OCProperties dict (all layers ON
    // by default) referencing the OCG objects emitted after the caller objects.
    emit_object_header(1);
    out += "<< /Type /Catalog /Pages 2 0 R";
    if (total_ocg_objects > 0) {
      out += "\n   /OCProperties << /OCGs [";
      for (std::size_t i = 0; i < total_ocg_objects; ++i) {
        if (i > 0) {
          out.push_back(' ');
        }
        append_integer(out, static_cast<std::int64_t>(ocg_base + i));
        out += " 0 R";
      }
      out += "] /D << /ON [";
      for (std::size_t i = 0; i < total_ocg_objects; ++i) {
        if (i > 0) {
          out.push_back(' ');
        }
        append_integer(out, static_cast<std::int64_t>(ocg_base + i));
        out += " 0 R";
      }
      out += "] >> >>";
    }
    out += " >>\nendobj\n";

    // Object 2: Pages (kids filled after page objects exist)
    emit_object_header(2);
    out += "<< /Type /Pages /Count ";
    append_integer(out, static_cast<std::int64_t>(pages.size()));
    out += "\n   /Kids [";
    for (std::size_t p = 0; p < pages.size(); ++p) {
      if (p > 0) {
        out.push_back(' ');
      }
      // Page object number = 3 + 2*p + 1
      append_integer(out, static_cast<std::int64_t>(3 + 2 * p + 1));
      out += " 0 R";
    }
    out += "]\n>>\nendobj\n";

    // Per-page: content stream object + page object.
    for (std::size_t p = 0; p < pages.size(); ++p) {
      // Content stream object = 3 + 2*p
      emit_object_header(3 + 2 * p);
      out += "<< /Length ";
      append_integer(out, static_cast<std::int64_t>(compressed[p].size()));
      out += " /Filter /FlateDecode >>\nstream\n";
      out.append(compressed[p].data(), compressed[p].size());
      out += "\nendstream\nendobj\n";

      // Page object = 3 + 2*p + 1
      emit_object_header(3 + 2 * p + 1);
      out += "<< /Type /Page /Parent 2 0 R ";
      out += "/MediaBox [0 0 ";
      append_number(out, specs[p].width_points);
      out.push_back(' ');
      append_number(out, specs[p].height_points);
      out += "] ";
      out += "/Contents ";
      append_integer(out, static_cast<std::int64_t>(3 + 2 * p));
      out += " 0 R ";
      // Resources. With no alphas AND no caller objects AND no standard font
      // (opaque-only + no images/patterns, e.g. the #185 spike) this stays
      // `/Resources << >>`, byte-identical to the pre-#188 / pre-B1.PDF.2
      // writer. Otherwise build a dict naming /ExtGState, /XObject, /Pattern,
      // and optionally /Font (Base-14 Helvetica for searchable Latin text).
      const bool has_alphas = !page_alphas[p].empty();
      const bool has_objects = !pages[p].objects.empty();
      const bool has_font = pages[p].stream.needs_standard_font();
      // Layered PDF: /Properties names the OCGs this page's marked content
      // references (/Lay<i> from mark_begin). Same index used twice on one
      // page is emitted once (a track appears once per page anyway).
      const bool has_properties = !pages[p].layer_indices.empty();
      if (!has_alphas && !has_objects && !has_font && !has_properties) {
        out += "/Resources << >> >>\nendobj\n";
      } else {
        out += "/Resources <<";
        if (has_alphas) {
          out += " /ExtGState << ";
          for (std::size_t g = 0; g < page_alphas[p].size(); ++g) {
            out += "/GS";
            append_integer(out, static_cast<std::int64_t>(g));
            out.push_back(' ');
            append_integer(out,
                           static_cast<std::int64_t>(page_alpha_base[p] + g));
            out += " 0 R ";
          }
          out += ">>";
        }
        if (has_objects) {
          // Partition the page's objects by kind so each resource dict only
          // names the objects of its kind. Object numbers are page-local and
          // assigned in `objects` order.
          bool any_image = false;
          bool any_pattern = false;
          for (const auto &obj : pages[p].objects) {
            if (obj.kind == PdfObjectKind::image) {
              any_image = true;
            } else {
              any_pattern = true;
            }
            if (any_image && any_pattern) {
              break;
            }
          }
          if (any_image) {
            out += " /XObject << ";
            std::size_t oi = 0;
            for (const auto &obj : pages[p].objects) {
              if (obj.kind == PdfObjectKind::image) {
                out += '/';
                out += obj.local_name;
                out.push_back(' ');
                append_integer(out, static_cast<std::int64_t>(
                                        page_object_base[p] + oi));
                out += " 0 R ";
              }
              ++oi;
            }
            out += ">>";
          }
          if (any_pattern) {
            out += " /Pattern << ";
            std::size_t oi = 0;
            for (const auto &obj : pages[p].objects) {
              if (obj.kind == PdfObjectKind::pattern) {
                out += '/';
                out += obj.local_name;
                out.push_back(' ');
                append_integer(out, static_cast<std::int64_t>(
                                        page_object_base[p] + oi));
                out += " 0 R ";
              }
              ++oi;
            }
            out += ">>";
          }
        }
        if (has_font) {
          // Inline Base-14 Type1 + WinAnsiEncoding for Latin-1 (B1.PDF.3).
          out += " /Font << /F1 << /Type /Font /Subtype /Type1 "
                 "/BaseFont /Helvetica /Encoding /WinAnsiEncoding >> >>";
        }
        if (has_properties) {
          out += " /Properties << ";
          std::uint32_t prev = std::numeric_limits<std::uint32_t>::max();
          for (const auto layer_index : pages[p].layer_indices) {
            if (layer_index == prev) {
              continue;  // dedupe consecutive repeats (track emits once/page)
            }
            prev = layer_index;
            out += "/Lay";
            append_integer(out, static_cast<std::int64_t>(layer_index));
            out.push_back(' ');
            append_integer(out, static_cast<std::int64_t>(ocg_base + layer_index));
            out += " 0 R ";
          }
          out += ">>";
        }
        out += " >> >>\nendobj\n";
      }
    }

    // Per-page /ExtGState objects (one per distinct fill-alpha that page uses).
    for (std::size_t p = 0; p < pages.size(); ++p) {
      for (std::size_t g = 0; g < page_alphas[p].size(); ++g) {
        emit_object_header(page_alpha_base[p] + g);
        out += "<< /Type /ExtGState /ca ";
        append_number(out, page_alphas[p][g]);
        out += " >>\nendobj\n";
      }
    }

    // Per-page caller-supplied objects (image XObjects + tiling patterns). Each
    // body is already a complete dict (+ optional stream); the writer only adds
    // the "N 0 obj\n" header and "endobj\n" trailer.
    for (std::size_t p = 0; p < pages.size(); ++p) {
      for (std::size_t oi = 0; oi < pages[p].objects.size(); ++oi) {
        emit_object_header(page_object_base[p] + oi);
        out += pages[p].objects[oi].body;
        out += "\nendobj\n";
      }
    }

    // Global OCG objects (layered PDF). Names are ASCII (the scene exporter
    // authors "track-<index>"), so a plain PDF literal string is safe.
    for (std::size_t i = 0; i < total_ocg_objects; ++i) {
      emit_object_header(ocg_base + i);
      out += "<< /Type /OCG /Name (";
      out += layers[i];
      out += ") >>\nendobj\n";
    }

    // xref table.
    const auto xref_offset = out.size();
    const auto object_count = static_cast<std::size_t>(
        2 + 2 * pages.size() + total_alpha_objects + total_caller_objects +
        total_ocg_objects + 1);
    out += "xref\n0 ";
    append_integer(out, static_cast<std::int64_t>(object_count));
    out += "\n0000000000 65535 f \n";
    for (std::size_t obj = 1; obj < object_count; ++obj) {
      std::array<char, 10> offset_buf{};
      auto off = offsets[obj];
      for (int i = 9; i >= 0; --i) {
        offset_buf[static_cast<std::size_t>(i)] =
            static_cast<char>('0' + (off % 10));
        off /= 10;
      }
      out.append(offset_buf.data(), 10);
      out += " 00000 n \n";
    }

    // trailer — no CreationDate/ModDate by construction (determinism).
    out += "trailer\n<< /Size ";
    append_integer(out, static_cast<std::int64_t>(object_count));
    out += " /Root 1 0 R >>\nstartxref\n";
    append_integer(out, static_cast<std::int64_t>(xref_offset));
    out += "\n%%EOF";

    auto doc_impl = std::make_shared<PdfDocument::Impl>();
    doc_impl->bytes = std::move(out);
    return PdfDocument{std::move(doc_impl)};
  } catch (const std::bad_alloc &) {
    return pdf_error(ErrorCode::resource_exhausted,
                     MessageKey::resource_exhausted);
  } catch (...) {
    return pdf_error(ErrorCode::internal_error, MessageKey::internal_error);
  }
}

// Public alias of the writer's Flate codec, for image/pattern stream bodies
// (#188). Lives at welllog scope (not anonymous) so it satisfies the exported
// declaration in pdf.hpp.
bool flate_compress_buffer(std::string_view input,
                           std::string &output) noexcept {
  return flate_compress_view(input, output);
}

} // namespace welllog
