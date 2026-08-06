// Corpus + mutation fuzz for Manifest JSON, XML, ZIP, ImageSource, CustomLayer,
// Pattern limits, and untrusted asset URIs (#172).

#include "fuzz_common.hpp"

#include <welllog/core/document.hpp>
#include <welllog/io/asset_security.hpp>
#include <welllog/io/container_security.hpp>
#include <welllog/io/manifest.hpp>
#include <welllog/scene/image_pyramid.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace welllog;
using namespace welllog::fuzz;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  return *EntityId::parse(text);
}

void fuzz_text(std::string_view text) {
  // Manifest
  ManifestResolvers resolvers{};
  auto man = ManifestCodec::read(text, resolvers);
  if (!man.has_value()) {
    require(man.error().arguments.size == 0, "manifest error no payload args");
  }

  // XML threat scan
  auto xml = scan_untrusted_xml(text);
  if (xml.has_value()) {
    require(xml->arguments.size == 0, "xml error no payload args");
  }

  // URI policy (treat text as URI)
  (void)is_safe_untrusted_asset_uri(text);

  // ZIP inspect when bytes look large enough / always try
  std::vector<std::byte> as_bytes(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    as_bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  auto zip = inspect_untrusted_zip(as_bytes);
  if (!zip.has_value()) {
    require(zip.error().arguments.size == 0, "zip error no payload args");
  }
}

void fuzz_image_dims(std::uint64_t w, std::uint64_t h, std::uint32_t dpi) {
  ImageSource src{
      .id = id("c1720000-0000-4000-8000-000000000001"),
      .width_px = w,
      .height_px = h,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 1000.0,
      .reference_depth_bottom = 1100.0,
      .dpi = dpi,
      .source = BufferSourceReference{.uri = "tiles/local.rgba",
                                     .checksum = {},
                                     .byte_offset = 0},
  };
  auto err = validate_image_source_limits(src);
  if (err.has_value()) {
    require(err->arguments.size == 0, "image limit error no payload");
  } else {
    // Valid dims may still fail pyramid build on budget — must not throw.
    auto pyramid = ImagePyramid::build(src, ImagePyramidOptions{});
    (void)pyramid;
  }
}

void fuzz_custom_layer(std::size_t n_points) {
  CustomLayerSource source;
  source.id = id("c1720000-0000-4000-8000-000000000002");
  source.content_revision = DocumentRevision{1};
  CustomPolyline poly;
  poly.points.reserve(n_points);
  for (std::size_t i = 0; i < n_points; ++i) {
    poly.points.push_back(PhysicalPoint{
        .left = Millimetres{static_cast<double>(i)},
        .top = Millimetres{static_cast<double>(i % 7)},
    });
  }
  source.primitives.push_back(poly);
  auto err = validate_custom_layer_source(source);
  if (err.has_value()) {
    require(err->arguments.size == 0, "custom error no payload");
  }
}

void uri_policy_rejects_executable_and_network() {
  require(!is_safe_untrusted_asset_uri("javascript:alert(1)"), "js");
  require(!is_safe_untrusted_asset_uri("https://evil/x"), "https");
  require(!is_safe_untrusted_asset_uri("data:text/html,<script>"), "data");
  require(!is_safe_untrusted_asset_uri("file:///etc/passwd"), "file");
  require(!is_safe_untrusted_asset_uri("local#version 330\nvoid main(){}"),
          "shader token");
  require(is_safe_untrusted_asset_uri("buffers/curve-gr.bin"), "relative ok");
  require(is_safe_untrusted_asset_uri("asset:welllog/tiles/1"), "opaque ok");
}

void pattern_limits() {
  require(validate_pattern_definition_limits(0, 0, 10.0, 10.0).has_value(),
          "empty pattern rejected");
  require(validate_pattern_definition_limits(1, 2, 10.0, 10.0).has_value() ==
              false,
          "tiny pattern ok");
  require(validate_pattern_definition_limits(1, 2, -1.0, 10.0).has_value(),
          "negative tile rejected");
  require(validate_pattern_definition_limits(1, 100000, 10.0, 10.0).has_value(),
          "polyline points rejected");
}

void image_limits() {
  ImageSource zero{
      .id = id("c1720000-0000-4000-8000-000000000003"),
      .width_px = 0,
      .height_px = 1,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 0,
      .reference_depth_bottom = 1,
      .dpi = 72,
      .source = BufferSourceReference{.uri = "x", .checksum = {},
                                     .byte_offset = 0},
  };
  require(validate_image_source_limits(zero).has_value(), "zero dim");
  ImageSource bad_uri{
      .id = id("c1720000-0000-4000-8000-000000000003"),
      .width_px = 100,
      .height_px = 100,
      .pixel_format = PixelFormat::rgba8,
      .reference_depth_top = 0,
      .reference_depth_bottom = 1,
      .dpi = 72,
      .source = BufferSourceReference{.uri = "javascript:x", .checksum = {},
                                     .byte_offset = 0},
  };
  require(validate_image_source_limits(bad_uri).has_value(), "bad uri");
}

std::filesystem::path corpus_text_dir() {
  const char *env = std::getenv("WELLLOG_FUZZ_CORPUS");
  if (env != nullptr && env[0] != '\0') {
    return std::filesystem::path{env} / "assets";
  }
#ifdef WELLLOG_FUZZ_CORPUS_DIR
  return std::filesystem::path{WELLLOG_FUZZ_CORPUS_DIR} / "assets";
#else
  return std::filesystem::path{"tests/fuzz/corpus/assets"};
#endif
}

void run_text_corpus_and_mutations() {
  auto files = load_corpus_dir(corpus_text_dir());
  std::vector<std::string> seeds = builtin_text_seeds();
  for (const auto &f : files) {
    seeds.emplace_back(reinterpret_cast<const char *>(f.data()), f.size());
  }
  const auto rounds = mutation_iterations();
  std::uint64_t cases = 0;
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    std::vector<std::byte> as_bytes(seeds[i].size());
    for (std::size_t b = 0; b < seeds[i].size(); ++b) {
      as_bytes[b] =
          static_cast<std::byte>(static_cast<unsigned char>(seeds[i][b]));
    }
    run_mutations(as_bytes, rounds, 0xA55E7000ULL + i,
                  [&](std::span<const std::byte> span) {
                    std::string text(reinterpret_cast<const char *>(span.data()),
                                     span.size());
                    fuzz_text(text);
                    ++cases;
                  });
  }
  // Dimension edge fuzz (not mutation of text).
  for (std::uint64_t w : {0ULL, 1ULL, 65536ULL, 65537ULL, (1ULL << 20)}) {
    for (std::uint64_t h : {0ULL, 1ULL, 100ULL, 65537ULL}) {
      fuzz_image_dims(w, h, 72);
      ++cases;
    }
  }
  for (std::size_t n : {0ULL, 1ULL, 2ULL, 100ULL, 8193ULL}) {
    fuzz_custom_layer(n);
    ++cases;
  }
  require(cases > 0, "cases");
  std::cerr << "INFO: asset fuzz cases=" << cases << " seeds=" << seeds.size()
            << " iters=" << rounds << '\n';
}

} // namespace

int main() {
  uri_policy_rejects_executable_and_network();
  pattern_limits();
  image_limits();
  run_text_corpus_and_mutations();
  return EXIT_SUCCESS;
}
