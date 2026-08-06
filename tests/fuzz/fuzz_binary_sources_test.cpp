// Corpus + mutation fuzz for DLIS / LIS / Format716 binary adapters (#172).
// Guarantees: no crash; Result/Error only; tight resource limits; diagnostics
// never dump sample payloads into Error.arguments.

#include "fuzz_common.hpp"

#include <welllog/io/dlis.hpp>
#include <welllog/io/format716.hpp>
#include <welllog/io/lis.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

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

// Tight ceilings so mutated huge-count fields cannot allocate unbounded RAM.
DlisLimits tight_dlis() {
  return DlisLimits{
      .max_input_bytes = 1ULL << 20,
      .max_logical_record_bytes = 64ULL * 1024ULL,
      .max_logical_records = 256,
      .max_samples = 10'000,
  };
}

LisLimits tight_lis() {
  return LisLimits{
      .max_input_bytes = 1ULL << 20,
      .max_logical_record_bytes = 64ULL * 1024ULL,
      .max_logical_records = 256,
      .max_samples_per_data_set = 10'000,
      .max_curves_per_data_set = 64,
  };
}

Format716Limits tight_716() {
  return Format716Limits{
      .max_input_bytes = 1ULL << 20,
      .max_curves = 64,
      .max_samples = 10'000,
  };
}

void fuzz_bytes(std::span<const std::byte> bytes) {
  // Each adapter must return Result without throwing or aborting.
  const auto dlis = DlisSourceAdapter::inspect(bytes, tight_dlis());
  if (dlis.has_value()) {
    // Import with empty selection must reject cleanly.
    (void)DlisSourceAdapter::import(
        bytes,
        BufferSourceReference{.uri = "fuzz://seed", .checksum = {},
                              .byte_offset = 0},
        DlisSelection{}, tight_dlis());
  } else {
    require(dlis.error().arguments.size == 0,
            "dlis error must not carry raw payload arguments");
  }

  const auto lis = LisSourceAdapter::inspect(bytes, tight_lis());
  if (lis.has_value()) {
    (void)LisSourceAdapter::import(
        bytes,
        BufferSourceReference{.uri = "fuzz://seed", .checksum = {},
                              .byte_offset = 0},
        LisSelection{}, default_lis_normalization_profile(), tight_lis());
  } else {
    require(lis.error().arguments.size == 0,
            "lis error must not carry raw payload arguments");
  }

  (void)Format716SourceAdapter::detect_endian(bytes, tight_716());
  const auto f716 = Format716SourceAdapter::inspect(bytes, {}, tight_716());
  if (f716.has_value()) {
    (void)Format716SourceAdapter::import(
        bytes,
        BufferSourceReference{.uri = "fuzz://seed", .checksum = {},
                              .byte_offset = 0},
        {}, tight_716());
  } else {
    require(f716.error().arguments.size == 0,
            "716 error must not carry raw payload arguments");
  }
}

std::filesystem::path corpus_dir() {
  // Prefer source tree corpus; env overrides the corpus root.
  const char *env = std::getenv("WELLLOG_FUZZ_CORPUS");
  if (env != nullptr && env[0] != '\0') {
    return std::filesystem::path{env} / "binary";
  }
  // CMake defines WELLLOG_FUZZ_CORPUS_DIR when available.
#ifdef WELLLOG_FUZZ_CORPUS_DIR
  return std::filesystem::path{WELLLOG_FUZZ_CORPUS_DIR} / "binary";
#else
  return std::filesystem::path{"tests/fuzz/corpus/binary"};
#endif
}

void run_corpus_and_mutations() {
  auto seeds = load_corpus_dir(corpus_dir());
  if (seeds.empty()) {
    seeds = builtin_binary_seeds();
  }
  require(!seeds.empty(), "at least one seed");
  const auto rounds = mutation_iterations();
  std::uint64_t cases = 0;
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    run_mutations(seeds[i], rounds, 0x17200ULL + i, [&](std::span<const std::byte> b) {
      fuzz_bytes(b);
      ++cases;
    });
  }
  require(cases > 0, "exercised cases");
  std::cerr << "INFO: binary fuzz cases=" << cases << " seeds=" << seeds.size()
            << " iters=" << rounds << '\n';
}

} // namespace

int main() {
  run_corpus_and_mutations();
  return EXIT_SUCCESS;
}
