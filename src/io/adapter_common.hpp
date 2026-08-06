#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>

namespace welllog::io_detail {

[[nodiscard]] inline Error invalid_document_error() {
  return Error{.code = ErrorCode::invalid_document,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::document_structure_invalid,
               .arguments = {}};
}

[[nodiscard]] inline Error resource_exhausted_error() {
  return Error{.code = ErrorCode::resource_exhausted,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::resource_exhausted,
               .arguments = {}};
}

[[nodiscard]] inline Error internal_error() {
  return Error{.code = ErrorCode::internal_error,
               .severity = Severity::error,
               .entity_id = std::nullopt,
               .message = MessageKey::internal_error,
               .arguments = {}};
}

[[nodiscard]] inline EntityId stable_id(std::string_view identity,
                                         std::string_view kind,
                                         std::uint64_t ordinal = 0) {
  constexpr std::uint64_t offset_a = 1469598103934665603ULL;
  constexpr std::uint64_t offset_b = 1099511628211ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto first = offset_a;
  auto second = offset_b;
  const auto append = [&](std::string_view value) {
    for (const auto byte : value) {
      first ^= static_cast<std::uint8_t>(byte);
      first *= prime;
      second ^= static_cast<std::uint8_t>(byte);
      second *= prime;
    }
  };
  append(identity);
  append("/");
  append(kind);
  append("/");
  for (std::uint32_t index = 0; index < 8; ++index) {
    const auto byte = static_cast<std::uint8_t>((ordinal >> (index * 8U)) & 0xffU);
    first ^= byte;
    first *= prime;
    second ^= byte;
    second *= prime;
  }

  std::array<std::uint8_t, 16> bytes{};
  for (std::uint32_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((first >> (index * 8U)) & 0xffU);
    bytes[index + 8] =
        static_cast<std::uint8_t>((second >> (index * 8U)) & 0xffU);
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  constexpr std::array<char, 16> digits{
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::array<char, 36> text{};
  std::size_t output{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text[output++] = '-';
    }
    text[output++] = digits[bytes[index] >> 4U];
    text[output++] = digits[bytes[index] & 0x0fU];
  }
  return *EntityId::parse(std::string_view{text.data(), text.size()});
}

[[nodiscard]] inline std::optional<AxisDirection>
axis_direction(const std::vector<double> &coordinates) noexcept {
  std::optional<AxisDirection> direction;
  for (std::size_t index = 1; index < coordinates.size(); ++index) {
    if (coordinates[index] == coordinates[index - 1]) {
      continue;
    }
    const auto next = coordinates[index] > coordinates[index - 1]
                          ? AxisDirection::increasing
                          : AxisDirection::decreasing;
    if (direction.has_value() && *direction != next) {
      return std::nullopt;
    }
    direction = next;
  }
  return direction.value_or(AxisDirection::increasing);
}

[[nodiscard]] inline BufferView
owned_values(const std::shared_ptr<const std::vector<double>> &values,
             BufferSourceReference source) {
  return BufferView::from_raw(
      values->data(), static_cast<std::uint64_t>(values->size()), sizeof(double),
      ScalarType::float64,
      static_cast<std::uint64_t>(values->size() * sizeof(double)), SharedOwner{values},
      std::move(source), BufferAccessMode::converted_copy);
}

[[nodiscard]] inline NullBitmapView
owned_nulls(const std::vector<std::uint8_t> &null_flags,
            BufferSourceReference source) {
  if (std::none_of(null_flags.begin(), null_flags.end(),
                   [](std::uint8_t flag) { return flag != 0U; })) {
    return {};
  }
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
      (null_flags.size() + 7U) / 8U, std::uint8_t{0});
  for (std::size_t index{}; index < null_flags.size(); ++index) {
    if (null_flags[index] != 0U) {
      (*bytes)[index / 8U] |= static_cast<std::uint8_t>(1U << (index % 8U));
    }
  }
  const auto owner = std::shared_ptr<const std::vector<std::uint8_t>>{std::move(bytes)};
  return NullBitmapView::from_raw(
      owner->data(), static_cast<std::uint64_t>(null_flags.size()),
      static_cast<std::uint64_t>(owner->size()), SharedOwner{owner}, std::move(source));
}

} // namespace welllog::io_detail
