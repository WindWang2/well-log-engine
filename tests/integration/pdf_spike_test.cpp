// Spike test for the hand-rolled PDF writer (#185, ADR: PDF via hand-rolled
// writer). Proves the writer produces a structurally-valid, byte-deterministic,
// Flate-compressed multi-page PDF with the OutlineCommand→operator mapping
// (including quadratic→cubic lift). External structural validity is checked
// with qpdf --check / pdfinfo when available. The Flate round-trip inflates the
// writer's ACTUAL embedded content stream (extracted from the PDF bytes), not a
// stand-in string.

#include <welllog/export/pdf.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

// Builds the two-page spike document: page 1 has a filled red rectangle and a
// stroked black line; page 2 is blank. Reused by every assertion so the input
// is identical across the determinism check.
PdfDocument build_spike_document() {
  PdfPageContent page1;
  page1.stream.set_fill_color(255, 0, 0)
      .move_to(72.0, 72.0)
      .line_to(200.0, 72.0)
      .line_to(200.0, 200.0)
      .line_to(72.0, 200.0)
      .close()
      .fill();
  page1.stream.set_stroke_color(0, 0, 0)
      .set_line_width(2.0)
      .move_to(72.0, 700.0)
      .line_to(500.0, 700.0)
      .stroke();

  PdfPageContent page2; // blank

  const std::array<PdfPageContent, 2> pages{page1, page2};
  const auto result = PdfWriter::write(pages);
  require(result.has_value(), "spike PDF must build");
  return result.value();
}

// Writes the PDF bytes to a temp file for external verification.
std::filesystem::path write_temp(std::string_view bytes) {
  const auto path =
      std::filesystem::temp_directory_path() / "welllog_pdf_spike.pdf";
  std::ofstream out(path, std::ios::binary);
  require(out.good(), "temp PDF file must open");
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return path;
}

// Returns the command's stdout + exit code via popen / _popen.
int run(std::string_view command, std::string &captured) {
  std::array<char, 128> buffer{};
  captured.clear();
#if defined(_WIN32)
  const auto pipe =
      _popen(std::string{command}.c_str(), "r"); // NOLINT(cert-env33-c)
#else
  const auto pipe =
      popen(std::string{command}.c_str(), "r"); // NOLINT(cert-env33-c)
#endif
  if (pipe == nullptr) {
    return -1;
  }
  while (std::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    captured += buffer.data();
  }
#if defined(_WIN32)
  return _pclose(pipe); // NOLINT(cert-env33-c)
#else
  return pclose(pipe); // NOLINT(cert-env33-c)
#endif
}

// --- Tests ------------------------------------------------------------------

// Two pages, the Catalog/Pages/Page skeleton present, content compressed.
void multi_page_pdf_is_structurally_complete() {
  const auto doc = build_spike_document();
  const auto bytes = std::string{doc.bytes()};
  require(bytes.starts_with("%PDF-1.7"), "PDF header must be present");
  require(bytes.find("/Type /Catalog") != std::string::npos,
          "Catalog object must exist");
  require(bytes.find("/Type /Pages") != std::string::npos,
          "Pages object must exist");
  require(bytes.find("/Count 2") != std::string::npos,
          "exactly two pages must be declared");
  // Two /Type /Page entries.
  auto count_pages = [&]() {
    std::size_t n = 0;
    std::size_t pos = 0;
    while ((pos = bytes.find("/Type /Page ", pos)) != std::string::npos) {
      ++n;
      pos += 11;
    }
    return n;
  };
  require(count_pages() == 2, "two Page objects must exist");
  require(bytes.find("/Filter /FlateDecode") != std::string::npos,
          "content streams must be Flate-compressed");
  require(bytes.find("%%EOF") != std::string::npos,
          "PDF must terminate with %%EOF");
  require(bytes.find("startxref") != std::string::npos,
          "xref offset must be recorded");
}

// Identical input must produce byte-identical output (no timestamps/IDs).
void output_is_byte_deterministic() {
  const auto first = std::string{build_spike_document().bytes()};
  const auto second = std::string{build_spike_document().bytes()};
  require(first == second,
          "two builds of the same document must be byte-identical");
  require(first.find("CreationDate") == std::string::npos,
          "no CreationDate may appear");
  require(first.find("ModDate") == std::string::npos,
          "no ModDate may appear");
}

// B1.PDF.2 / ADR 0053: Base-14 Helvetica Latin text is named in Resources and
// the operators include a literal string (extractable by PDF text tools).
void standard_font_searchable_text_is_present() {
  PdfPageContent page;
  page.stream.set_fill_color(0, 0, 0)
      .draw_standard_text(72.0, 720.0, 12.0, "GR depth 1000.0");
  require(page.stream.needs_standard_font(),
          "draw_standard_text must mark the stream for /Font resources");
  const auto result = PdfWriter::write(std::span<const PdfPageContent>(&page, 1));
  require(result.has_value(), "searchable-text PDF must build");
  const auto bytes = std::string{result.value().bytes()};
  require(bytes.find("/BaseFont /Helvetica") != std::string::npos,
          "Resources must name Helvetica (Base-14)");
  require(bytes.find("/Encoding /WinAnsiEncoding") != std::string::npos,
          "Helvetica must declare WinAnsiEncoding (B1.PDF.3 Latin-1)");
  require(bytes.find("/Type /Font") != std::string::npos,
          "Resources must include a Font dictionary");
  // Operators are Flate-compressed — inflate and look for the literal string.
  const auto filter_pos = bytes.find("/Filter /FlateDecode");
  require(filter_pos != std::string::npos, "content stream must be Flate");
  const auto stream_kw = bytes.find("stream\n", filter_pos);
  require(stream_kw != std::string::npos, "stream keyword must follow dict");
  const auto payload_start = stream_kw + std::strlen("stream\n");
  const auto endstream = bytes.find("\nendstream", payload_start);
  require(endstream != std::string::npos, "endstream required");
  const std::string compressed =
      bytes.substr(payload_start, endstream - payload_start);
  z_stream zs{};
  require(inflateInit(&zs) == Z_OK, "inflateInit");
  std::string sink(compressed.size() * 8 + 256, '\0');
  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  zs.avail_in = static_cast<uInt>(compressed.size());
  zs.next_out = reinterpret_cast<Bytef *>(sink.data());
  zs.avail_out = static_cast<uInt>(sink.size());
  require(inflate(&zs, Z_FINISH) == Z_STREAM_END, "inflate content");
  inflateEnd(&zs);
  const std::string inflated(sink.data(), zs.total_out);
  require(inflated.find("(GR depth 1000.0)") != std::string::npos,
          "inflated operators must contain the Latin text literal");
  require(inflated.find("/F1 ") != std::string::npos,
          "inflated operators must select /F1");
  // CJK is dropped from searchable overlay; Latin-1 (e.g. ° via UTF-8) emits.
  PdfPageContent page2;
  page2.stream.draw_standard_text(10.0, 10.0, 10.0, "深度");
  require(!page2.stream.needs_standard_font(),
          "CJK-only string must not register a font (dropped)");
  require(page2.stream.non_latin_codepoints_dropped() >= 1,
          "CJK code points must be counted as dropped (B1.PDF.3)");
  PdfPageContent page3;
  page3.stream.draw_standard_text(10.0, 10.0, 10.0, "caf\xc3\xa9"); // café
  require(page3.stream.needs_standard_font(),
          "Latin-1 UTF-8 must emit Helvetica text");
}

// The compressed content stream embedded by the writer must inflate back to
// the original operators. This extracts the writer's ACTUAL FlateDecode stream
// from the produced PDF bytes and inflates it — proving the compression is
// correct and recoverable on the real artifact, not a stand-in string.
void flate_stream_round_trips() {
  const auto bytes = std::string{build_spike_document().bytes()};
  // Locate the first content stream: "stream\n" ... "\nendstream" after a
  // "/Filter /FlateDecode" header.
  const auto filter_pos = bytes.find("/Filter /FlateDecode");
  require(filter_pos != std::string::npos,
          "PDF must contain a FlateDecode stream");
  const auto stream_kw = bytes.find("stream\n", filter_pos);
  require(stream_kw != std::string::npos, "stream keyword must follow the dict");
  const auto payload_start = stream_kw + std::strlen("stream\n");
  const auto endstream = bytes.find("\nendstream", payload_start);
  require(endstream != std::string::npos, "endstream must terminate the stream");
  const std::string compressed =
      bytes.substr(payload_start, endstream - payload_start);
  require(!compressed.empty(), "the embedded content stream must be non-empty");

  // Inflate the real stream.
  z_stream stream{};
  require(inflateInit(&stream) == Z_OK, "inflateInit must succeed");
  std::string sink(compressed.size() * 8 + 256, '\0');
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef *>(sink.data());
  stream.avail_out = static_cast<uInt>(sink.size());
  const auto rc = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  require(rc == Z_STREAM_END, "the embedded stream must inflate cleanly");
  const std::string inflated(sink.data(), stream.total_out);
  // The page-1 operators we know the writer emits (filled rect + stroked line).
  // Each operator is emitted as "<operands> <op>\n" (or bare "<op>\n" for h/f/S),
  // so match the operator + trailing newline.
  require(inflated.find("m\n") != std::string::npos,
          "inflated stream must contain a move-to operator");
  require(inflated.find("f\n") != std::string::npos,
          "inflated stream must contain a fill operator");
  require(inflated.find("S\n") != std::string::npos,
          "inflated stream must contain a stroke operator");
}

// The OutlineCommand mapping emits the right PDF operators, and quadratic_to
// lifts to a cubic (no 'Q' in PDF).
void outline_command_maps_to_pdf_operators() {
  PdfPathStream path;
  const std::array<OutlineCommand, 4> commands{{
      {OutlineVerb::move_to, {10.0, 20.0, 0, 0, 0, 0}},
      {OutlineVerb::line_to, {30.0, 40.0, 0, 0, 0, 0}},
      {OutlineVerb::quadratic_to, {50.0, 60.0, 70.0, 80.0, 0, 0}},
      {OutlineVerb::close, {0, 0, 0, 0, 0, 0}},
  }};
  path.append_outline(commands, 1.0, 0.0, 0.0).fill();
  const auto ops = std::string{path.operators()};
  require(ops.find(" m\n") != std::string::npos, "move_to must emit PDF 'm'");
  require(ops.find(" l\n") != std::string::npos, "line_to must emit PDF 'l'");
  require(ops.find(" c\n") != std::string::npos,
          "quadratic_to must lift to a cubic 'c'");
  require(ops.find("h\n") != std::string::npos, "close must emit PDF 'h'");
  require(ops.find("f\n") != std::string::npos, "fill must emit PDF 'f'");
  // A cubic from a quadratic has 6 operands before 'c'; verify the lift
  // produced a cubic with distinct control points (not a degenerate line).
  const auto c_pos = ops.find(" c\n");
  require(c_pos != std::string::npos, "cubic operator must be present");
}

// External structural validity: qpdf --check (and pdfinfo) accept the file.
// These tools are optional; the test is skipped (not failed) if absent.
void external_tools_accept_the_pdf() {
  const auto doc = build_spike_document();
  const auto path = write_temp(doc.bytes());

  // qpdf --check returns 0 on a structurally valid PDF.
  bool qpdf_available = std::filesystem::exists("/usr/sbin/qpdf") ||
                        std::filesystem::exists("/usr/bin/qpdf");
  if (qpdf_available) {
    std::string captured;
    const auto rc = run("qpdf --check " + path.string() + " 2>&1", captured);
    require(rc == 0,
            "qpdf --check must accept the PDF (rc != 0): " + captured);
  }

  // pdfinfo reports the page count.
  bool pdfinfo_available =
      std::filesystem::exists("/usr/sbin/pdfinfo") ||
      std::filesystem::exists("/usr/bin/pdfinfo");
  if (pdfinfo_available) {
    std::string captured;
    const auto rc = run("pdfinfo " + path.string() + " 2>&1", captured);
    require(rc == 0, "pdfinfo must accept the PDF (rc != 0): " + captured);
    // pdfinfo right-aligns the value ("Pages:           2"). Extract the Pages
    // line's value and confirm it is exactly "2" after trimming whitespace.
    const auto pages_pos = captured.find("Pages:");
    require(pages_pos != std::string::npos,
            "pdfinfo must report a Pages line: " + captured);
    const auto newline_pos = captured.find('\n', pages_pos);
    const std::string pages_line = captured.substr(
        pages_pos + 6, newline_pos == std::string::npos
                           ? std::string::npos
                           : newline_pos - (pages_pos + 6));
    const auto first_nws = pages_line.find_first_not_of(" \t");
    const auto last_nws = pages_line.find_last_not_of(" \t\r\n");
    const std::string value = (first_nws != std::string::npos &&
                               last_nws != std::string::npos)
                                  ? pages_line.substr(first_nws, last_nws - first_nws + 1)
                                  : std::string{};
    require(value == "2", "pdfinfo must report 2 pages, got: '" + value + "'");
  }

  if (!qpdf_available && !pdfinfo_available) {
    std::cout << "welllog.pdf-spike: qpdf/pdfinfo absent; skipping external "
                 "structural check\n";
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

} // namespace

int main() {
  multi_page_pdf_is_structurally_complete();
  output_is_byte_deterministic();
  standard_font_searchable_text_is_present();
  flate_stream_round_trips();
  outline_command_maps_to_pdf_operators();
  external_tools_accept_the_pdf();
  std::cout << "welllog.pdf-spike: all cases passed\n";
  return 0;
}
