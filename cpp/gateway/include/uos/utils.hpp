#pragma once
// Port of src/utils.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uos::utils {
  // typeof value === "object" && value !== null: unlike rt::isRecord, arrays are records here.
  bool isRecord(const json::value& value) noexcept;
  bool isRecord(const json::value* value) noexcept;

  std::optional<std::string> getString(const json::value& value);
  std::optional<std::string> getString(const json::value* value);

  // Throws rt::InvalidCharacterError like atob.
  std::string decodeBase64ToString(std::string_view raw);

  std::string encodeHex(std::span<const std::uint8_t> bytes);

  std::string base64UrlEncode(std::span<const std::uint8_t> bytes);

  // Throws rt::InvalidCharacterError like atob.
  std::vector<std::uint8_t> base64UrlDecode(std::string_view value);

  rt::Task<std::string> sha256Base64Url(std::string value);

  rt::Task<std::string> sha256Hex(std::string value);

  std::string bytesToBase64(std::span<const std::uint8_t> bytes);

  // null (std::nullopt) when atob throws.
  std::optional<std::vector<std::uint8_t>> base64ToBytes(std::string_view value);
}
