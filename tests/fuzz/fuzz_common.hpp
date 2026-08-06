#pragma once

// Deterministic fuzz harness shared by corpus + mutation tests (#172).
// Runs without libFuzzer so CTest/ASan builds always exercise the same
// seeds. Optional WELLLOG_FUZZ_ITERS env raises iteration count for local
// stress (default 256 mutations per seed).

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace welllog::fuzz {

// xorshift64* — fast, deterministic, no global state.
class Rng {
public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed ? seed : 0xDEADBEEFCAFEULL) {}

  std::uint64_t next() noexcept {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  std::uint32_t next_u32() noexcept {
    return static_cast<std::uint32_t>(next() >> 32);
  }

  std::size_t next_index(std::size_t n) noexcept {
    if (n == 0) {
      return 0;
    }
    return static_cast<std::size_t>(next() % n);
  }

private:
  std::uint64_t state_;
};

inline std::size_t mutation_iterations() noexcept {
  if (const char *env = std::getenv("WELLLOG_FUZZ_ITERS")) {
    char *end = nullptr;
    const auto v = std::strtoul(env, &end, 10);
    if (end != env && v > 0 && v < 1'000'000) {
      return static_cast<std::size_t>(v);
    }
  }
  return 256;
}

// Load all files under `dir` (non-recursive) as byte vectors. Missing dir → empty.
inline std::vector<std::vector<std::byte>>
load_corpus_dir(const std::filesystem::path &dir) {
  std::vector<std::vector<std::byte>> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return out;
  }
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) {
      continue;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    // Cap individual seed size so the harness stays bounded.
    if (size == 0 || size > 4ULL * 1024ULL * 1024ULL) {
      continue;
    }
    std::vector<std::byte> buf(size);
    in.read(reinterpret_cast<char *>(buf.data()),
            static_cast<std::streamsize>(size));
    if (in || in.eof()) {
      out.push_back(std::move(buf));
    }
  }
  return out;
}

// In-place mutation: bit flip / byte insert / delete / overwrite / splice.
inline void mutate(std::vector<std::byte> &buf, Rng &rng) noexcept {
  if (buf.empty()) {
    buf.push_back(static_cast<std::byte>(rng.next_u32() & 0xff));
    return;
  }
  const auto op = rng.next_u32() % 5U;
  switch (op) {
  case 0: { // bit flip
    const auto i = rng.next_index(buf.size());
    const auto bit = rng.next_u32() & 7U;
    auto v = static_cast<std::uint8_t>(buf[i]);
    v = static_cast<std::uint8_t>(v ^ (1U << bit));
    buf[i] = static_cast<std::byte>(v);
    break;
  }
  case 1: { // overwrite byte
    buf[rng.next_index(buf.size())] =
        static_cast<std::byte>(rng.next_u32() & 0xff);
    break;
  }
  case 2: { // insert byte (cap growth)
    if (buf.size() < 1ULL << 20) {
      const auto i = rng.next_index(buf.size() + 1);
      buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(i),
                 static_cast<std::byte>(rng.next_u32() & 0xff));
    }
    break;
  }
  case 3: { // delete byte
    if (buf.size() > 1) {
      buf.erase(buf.begin() +
                static_cast<std::ptrdiff_t>(rng.next_index(buf.size())));
    }
    break;
  }
  default: { // interesting constants
    static constexpr std::uint8_t k[] = {0x00, 0xff, 0x7f, 0x80, 0x01, 0xfe};
    buf[rng.next_index(buf.size())] =
        static_cast<std::byte>(k[rng.next_u32() % (sizeof(k))]);
    break;
  }
  }
}

// Apply `rounds` mutations starting from seed; invoke callback each time.
// Callback must not throw across C++ exceptions from the engine (Result only).
template <typename Fn>
void run_mutations(std::span<const std::byte> seed, std::size_t rounds,
                   std::uint64_t rng_seed, Fn &&fn) {
  std::vector<std::byte> buf(seed.begin(), seed.end());
  Rng rng(rng_seed);
  fn(std::span<const std::byte>(buf));
  for (std::size_t i = 0; i < rounds; ++i) {
    mutate(buf, rng);
    fn(std::span<const std::byte>(buf));
  }
}

// Built-in minimal seeds when corpus directory is empty/missing.
inline std::vector<std::vector<std::byte>> builtin_binary_seeds() {
  std::vector<std::vector<std::byte>> seeds;
  seeds.push_back({});
  seeds.push_back({std::byte{0x00}});
  seeds.push_back(std::vector<std::byte>(16, std::byte{0xff}));
  seeds.push_back(std::vector<std::byte>(128, std::byte{0x00}));
  // Truncated 716-like header (128 bytes zeros).
  seeds.push_back(std::vector<std::byte>(128, std::byte{0x00}));
  // Random-looking short blob.
  std::vector<std::byte> rnd;
  Rng r(0xF172);
  for (int i = 0; i < 64; ++i) {
    rnd.push_back(static_cast<std::byte>(r.next_u32() & 0xff));
  }
  seeds.push_back(std::move(rnd));
  // Length overflow-ish: huge claimed counts in little-endian ints at start.
  std::vector<std::byte> big(256, std::byte{0});
  const std::uint32_t huge = 0xffffffffU;
  std::memcpy(big.data() + 80, &huge, 4); // curve_count slot for 716
  std::memcpy(big.data() + 100, &huge, 4);
  seeds.push_back(std::move(big));
  return seeds;
}

inline std::vector<std::string> builtin_text_seeds() {
  return {
      "",
      "{}",
      "[]",
      "{\"schemaVersion\":999}",
      std::string(4096, 'A'),
      "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><r/>",
      std::string(100, '{') + "1" + std::string(100, '}'),
      "javascript:alert(1)",
      "http://evil.example/shader.glsl",
      "data:text/html,<script>x</script>",
  };
}

} // namespace welllog::fuzz
