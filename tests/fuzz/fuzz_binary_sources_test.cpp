// Corpus + mutation fuzz for DLIS / LIS / Format716 binary adapters (#172).
// Guarantees: no crash; Result/Error only; tight resource limits; diagnostics
// never dump sample payloads into Error.arguments.

#include "fuzz_common.hpp"

#include <welllog/io/dlis.hpp>
#include <welllog/io/format716.hpp>
#include <welllog/io/lis.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::filesystem::path corpus_dir();

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

struct FuzzStats {
  std::uint64_t inspect_ok{};
};

void fuzz_bytes(std::span<const std::byte> bytes, FuzzStats &stats) {
  // Each adapter must return Result without throwing or aborting.
  const auto dlis = DlisSourceAdapter::inspect(bytes, tight_dlis());
  if (dlis.has_value()) {
    ++stats.inspect_ok;
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
    ++stats.inspect_ok;
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
    ++stats.inspect_ok;
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

std::vector<std::byte> read_seed_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  require(static_cast<bool>(in), "structural seed must be readable");
  in.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  require(size > 0 && size <= 4096, "structural seed must stay tiny");
  std::vector<std::byte> buf(size);
  in.read(reinterpret_cast<char *>(buf.data()),
          static_cast<std::streamsize>(size));
  require(static_cast<bool>(in) || in.eof(), "structural seed read");
  return buf;
}

void unmutated_structural_seeds_inspect_ok() {
  const auto dir = corpus_dir();
  const auto dlis = read_seed_file(dir / "dlis_sul_v1.bin");
  require(DlisSourceAdapter::inspect(dlis, tight_dlis()).has_value(),
          "unmutated DLIS SUL seed must inspect ok");
  const auto lis = read_seed_file(dir / "lis_lr_header.bin");
  require(LisSourceAdapter::inspect(lis, tight_lis()).has_value(),
          "unmutated LIS logical-record seed must inspect ok");
  const auto f716 = read_seed_file(dir / "valid_716_one_sample.bin");
  require(Format716SourceAdapter::inspect(f716, {}, tight_716()).has_value(),
          "unmutated 716 seed must inspect ok");
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
  FuzzStats stats;
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    run_mutations(seeds[i], rounds, 0x17200ULL + i, [&](std::span<const std::byte> b) {
      fuzz_bytes(b, stats);
      ++cases;
    });
  }
  require(cases > 0, "exercised cases");
  require(stats.inspect_ok > 0,
          "at least one inspect() success so import is reachable");
  std::cerr << "INFO: binary fuzz cases=" << cases << " seeds=" << seeds.size()
            << " iters=" << rounds << " inspect_ok=" << stats.inspect_ok
            << '\n';
}

} // namespace

int main() {
  unmutated_structural_seeds_inspect_ok();
  run_corpus_and_mutations();
  return EXIT_SUCCESS;
}
