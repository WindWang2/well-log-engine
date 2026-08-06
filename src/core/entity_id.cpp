#include <welllog/core/entity_id.hpp>

#include <charconv>
namespace welllog {
namespace {

[[nodiscard]] std::optional<std::uint8_t>
parse_byte(std::string_view text) noexcept {
  unsigned int value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (error != std::errc{} || end != text.data() + text.size() ||
      value > 0xff) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(value);
}

} // namespace

std::optional<EntityId> EntityId::parse(std::string_view text) noexcept {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return std::nullopt;
  }

  std::array<std::uint8_t, 16> bytes{};
  std::size_t source_index = 0;
  for (auto &byte : bytes) {
    while (source_index < text.size() && text[source_index] == '-') {
      ++source_index;
    }
    if (source_index + 2 > text.size()) {
      return std::nullopt;
    }
    const auto parsed = parse_byte(text.substr(source_index, 2));
    if (!parsed) {
      return std::nullopt;
    }
    byte = *parsed;
    source_index += 2;
  }
  return EntityId{bytes};
}

std::string EntityId::to_string() const {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes_.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(digits[bytes_[index] >> 4]);
    result.push_back(digits[bytes_[index] & 0x0f]);
  }
  return result;
}

bool EntityId::is_nil() const noexcept {
  for (const auto byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::size_t EntityIdHash::operator()(const EntityId &id) const noexcept {
  constexpr std::size_t offset = sizeof(std::size_t) == 8
                                     ? std::size_t{1469598103934665603ULL}
                                     : std::size_t{2166136261U};
  constexpr std::size_t prime = sizeof(std::size_t) == 8
                                    ? std::size_t{1099511628211ULL}
                                    : std::size_t{16777619U};
  auto hash = offset;
  for (const auto byte : id.bytes_) {
    hash ^= static_cast<std::size_t>(byte);
    hash *= prime;
  }
  return hash;
}

} // namespace welllog
