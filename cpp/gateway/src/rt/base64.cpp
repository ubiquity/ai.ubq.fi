#include "uos/rt/base64.hpp"

#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

// Keep the upstream inline conversions outside the port's warning policy.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "third_party/simdutf/simdutf.h"
#pragma GCC diagnostic pop

#include <boost/algorithm/hex.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace uos::rt {
  namespace {
    using Byte = std::uint8_t;

    std::string encode(std::span<const Byte> bytes, simdutf::base64_options options) {
      if (bytes.empty()) return {};
      if (bytes.size() > limits::kTextInputBytes) throw std::length_error("Base64 output is too large");
      std::string out(simdutf::base64_length_from_binary(bytes.size(), options), '\0');
      simdutf::binary_to_base64(reinterpret_cast<const char*>(bytes.data()), bytes.size(), out.data(), options);
      return out;
    }

    std::optional<std::vector<Byte>> decode(std::string_view text, simdutf::base64_options options) {
      if (text.empty()) return std::vector<Byte>{};
      std::vector<Byte> out(std::min(limits::kBase64DecodedBytes, simdutf::maximal_binary_length_from_base64(text.data(), text.size())));
      std::size_t written = out.size();
      const auto result = simdutf::base64_to_binary_safe(text.data(), text.size(), reinterpret_cast<char*>(out.data()), written, options);
      if (result.error == simdutf::OUTPUT_BUFFER_TOO_SMALL) throw RangeError("Base64 decoded input is too large");
      if (result.error != simdutf::SUCCESS) return std::nullopt;
      out.resize(written);
      return out;
    }

    // The low byte of every UTF-16 code unit of a JS string (Buffer's one-byte view).
    std::vector<Byte> lowBytes(std::string_view text) {
      if (isAscii(text)) return {reinterpret_cast<const Byte*>(text.data()), reinterpret_cast<const Byte*>(text.data()) + text.size()};
      const std::u16string units = toUtf16(text);
      std::vector<Byte> out;
      out.reserve(units.size());
      for (const char16_t unit : units) out.push_back(static_cast<Byte>(unit & 0xFF));
      return out;
    }

    std::string lowered(std::string_view name) {
      std::string out(name);
      for (char& value : out) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + 32);
      }
      return out;
    }
  }

  std::string btoa(std::string_view data) {
    if (data.size() > limits::kTextInputBytes) throw InvalidCharacterError();
    if (data.empty()) return {};
    std::vector<Byte> bytes(data.size());
    const auto written = simdutf::convert_utf8_to_latin1(data.data(), data.size(), reinterpret_cast<char*>(bytes.data()));
    if (written == 0) throw InvalidCharacterError();
    return toBase64(std::span(bytes).first(written));
  }

  std::string atob(std::string_view data) { return binaryString(atobBytes(data)); }

  std::vector<std::uint8_t> atobBytes(std::string_view data) {
    if (data.size() > limits::kBase64InputBytes) throw InvalidCharacterError();
    try {
      if (auto bytes = decode(data, simdutf::base64_default)) return std::move(*bytes);
    } catch (const RangeError&) {
      throw InvalidCharacterError();
    }
    throw InvalidCharacterError();
  }

  std::string binaryString(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > limits::kTextInputBytes) throw std::length_error("Binary string is too large");
    const auto* input = reinterpret_cast<const char*>(bytes.data());
    std::string out(simdutf::utf8_length_from_latin1(input, bytes.size()), '\0');
    out.resize(simdutf::convert_latin1_to_utf8(input, bytes.size(), out.data()));
    return out;
  }

  std::string toBase64(std::span<const std::uint8_t> bytes) { return encode(bytes, simdutf::base64_default); }

  std::string toBase64Url(std::span<const std::uint8_t> bytes) { return encode(bytes, simdutf::base64_url); }

  std::string toHex(std::span<const std::uint8_t> bytes) {
    std::string out;
    if (bytes.size() > out.max_size() / 2) throw std::length_error("Hex output is too large");
    out.resize(bytes.size() * 2);
    boost::algorithm::hex_lower(bytes.begin(), bytes.end(), out.begin());
    return out;
  }

  std::vector<std::uint8_t> fromBase64(std::string_view text) {
    if (text.size() > limits::kBase64InputBytes) throw RangeError("Base64 encoded input is too large");
    if (!isAscii(text)) return {};
    return decode(text, simdutf::base64_default_or_url_accept_garbage).value_or(std::vector<Byte>{});
  }

  std::vector<std::uint8_t> fromHex(std::string_view text) {
    text = text.substr(0, text.find_first_not_of("0123456789abcdefABCDEF"));
    text.remove_suffix(text.size() % 2);
    std::vector<Byte> out(text.size() / 2);
    boost::algorithm::unhex(text.begin(), text.end(), out.begin());
    return out;
  }

  BufferEncoding bufferEncoding(std::string_view name) {
    const std::string key = lowered(name);
    if (key == "utf8" || key == "utf-8") return BufferEncoding::utf8;
    if (key == "ucs2" || key == "ucs-2" || key == "utf16le" || key == "utf-16le") return BufferEncoding::utf16le;
    if (key == "latin1" || key == "binary") return BufferEncoding::latin1;
    if (key == "ascii") return BufferEncoding::ascii;
    if (key == "base64") return BufferEncoding::base64;
    if (key == "base64url") return BufferEncoding::base64url;
    if (key == "hex") return BufferEncoding::hex;
    throw TypeError("Unknown encoding: " + std::string(name));
  }

  std::vector<std::uint8_t> bufferFrom(std::string_view text, BufferEncoding encoding) {
    switch (encoding) {
      case BufferEncoding::utf8:
        return encodeUtf8(text);
      case BufferEncoding::utf16le: {
        const std::u16string units = toUtf16(text);
        std::vector<Byte> out;
        out.reserve(units.size() * 2);
        for (const char16_t unit : units) {
          out.push_back(static_cast<Byte>(unit & 0xFF));
          out.push_back(static_cast<Byte>(unit >> 8));
        }
        return out;
      }
      case BufferEncoding::latin1:
      case BufferEncoding::ascii:
        return lowBytes(text);
      case BufferEncoding::base64:
      case BufferEncoding::base64url:
        return fromBase64(text);
      case BufferEncoding::hex:
        return fromHex(text);
    }
    return {};
  }

  std::vector<std::uint8_t> bufferFrom(std::string_view text, std::string_view encoding) { return bufferFrom(text, bufferEncoding(encoding)); }

  std::string bufferToString(std::span<const std::uint8_t> bytes, BufferEncoding encoding) {
    switch (encoding) {
      case BufferEncoding::utf8:
        return decodeUtf8(bytes, {.ignoreBOM = true});
      case BufferEncoding::utf16le: {
        std::u16string units;
        units.reserve(bytes.size() / 2);
        for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) units.push_back(static_cast<char16_t>(bytes[index] | (bytes[index + 1] << 8)));
        return fromUtf16(units);
      }
      case BufferEncoding::latin1:
        return binaryString(bytes);
      case BufferEncoding::ascii: {
        std::string out;
        out.reserve(bytes.size());
        for (const Byte value : bytes) out.push_back(static_cast<char>(value & 0x7F));
        return out;
      }
      case BufferEncoding::base64:
        return toBase64(bytes);
      case BufferEncoding::base64url:
        return toBase64Url(bytes);
      case BufferEncoding::hex:
        return toHex(bytes);
    }
    return {};
  }

  std::string bufferToString(std::span<const std::uint8_t> bytes, std::string_view encoding) { return bufferToString(bytes, bufferEncoding(encoding)); }
}
