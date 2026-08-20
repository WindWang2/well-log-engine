#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <welllog/core/export.hpp>

namespace welllog {

struct EntityIdHash;

class WELLLOG_CORE_API EntityId {
public:
  constexpr EntityId() noexcept = default;

  [[nodiscard]] static std::optional<EntityId>
  parse(std::string_view text) noexcept;
  // Generates a fresh random (version 4, variant 1) id. Hosts that need
  // reproducible ids across runs supply their own; session commands use this
  // when a host leaves a to-be-created entity id nil.
  [[nodiscard]] static EntityId generate() noexcept;
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool is_nil() const noexcept;

  friend constexpr bool operator==(const EntityId &,
                                   const EntityId &) = default;
  friend constexpr auto operator<=>(const EntityId &,
                                    const EntityId &) = default;

private:
  explicit constexpr EntityId(std::array<std::uint8_t, 16> bytes) noexcept
      : bytes_(bytes) {}

  std::array<std::uint8_t, 16> bytes_{};
  friend struct EntityIdHash;
};

struct WELLLOG_CORE_API EntityIdHash {
  [[nodiscard]] std::size_t operator()(const EntityId &id) const noexcept;
};

} // namespace welllog
