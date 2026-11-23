#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

namespace uos::utils {
  namespace {
    std::string removeWhitespace(std::string_view text) {
      static const rt::Regex WHITESPACE("\\s+", "g");
      return rt::replace(text, WHITESPACE, "");
    }
  }

  bool isRecord(const json::value& value) noexcept { return value.is_object() || value.is_array(); }

  bool isRecord(const json::value* value) noexcept { return value != nullptr && isRecord(*value); }

  std::optional<std::string> getString(const json::value& value) {
    if (!value.is_string()) return std::nullopt;
    return std::string(value.get_string());
  }

  std::optional<std::string> getString(const json::value* value) {
    if (value == nullptr) return std::nullopt;
    return getString(*value);
  }

  std::string decodeBase64ToString(std::string_view raw) {
    const std::string cleaned = removeWhitespace(rt::trim(raw));
    const std::vector<std::uint8_t> bytes = rt::atobBytes(cleaned);
    return rt::decodeUtf8(bytes);
  }

  std::string encodeHex(std::span<const std::uint8_t> bytes) { return rt::toHex(bytes); }

  std::string base64UrlEncode(std::span<const std::uint8_t> bytes) {
    // btoa emits canonical base64, so the only '=' run is the 1-2 character
    // padding suffix; bounding the quantifier keeps this match linear.
    static const rt::Regex PLUS("\\+", "g");
    static const rt::Regex SLASH("\\/", "g");
    static const rt::Regex PADDING("={1,2}$", "");
    const std::string encoded = rt::btoa(rt::binaryString(bytes));
    return rt::replace(rt::replace(rt::replace(encoded, PLUS, "-"), SLASH, "_"), PADDING, "");
  }

  std::vector<std::uint8_t> base64UrlDecode(std::string_view value) {
    static const rt::Regex DASH("-", "g");
    static const rt::Regex UNDERSCORE("_", "g");
    const std::string cleaned = removeWhitespace(rt::trim(value));
    const std::string normalized = rt::replace(rt::replace(cleaned, DASH, "+"), UNDERSCORE, "/");
    const double length = static_cast<double>(rt::length(normalized));
    const std::string padded = rt::padEnd(normalized, rt::Math::ceil(length / 4) * 4, "=");
    return rt::atobBytes(padded);
  }

  rt::Task<std::string> sha256Base64Url(std::string value) {
    const std::vector<std::uint8_t> bytes = rt::encodeUtf8(value);
    const std::vector<std::uint8_t> digest = rt::crypto::subtle::digest("SHA-256", bytes);
    co_await rt::microtask();
    co_return base64UrlEncode(digest);
  }

  rt::Task<std::string> sha256Hex(std::string value) {
    const std::vector<std::uint8_t> bytes = rt::encodeUtf8(value);
    const std::vector<std::uint8_t> digest = rt::crypto::subtle::digest("SHA-256", bytes);
    co_await rt::microtask();
    co_return encodeHex(digest);
  }

  std::string bytesToBase64(std::span<const std::uint8_t> bytes) { return rt::toBase64(bytes); }

  std::optional<std::vector<std::uint8_t>> base64ToBytes(std::string_view value) {
    try {
      return rt::atobBytes(value);
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
  }
}
