#pragma once
// btoa / atob (WHATWG forgiving-base64) and Buffer string encodings. Strings are
// canonical WTF-8 (docs/rt/text.md); a "binary string" is a JS string whose code
// units are all <= U+00FF, one per byte.
#include "uos/rt/error.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uos::rt {
  // What atob and btoa throw: in JS a DOMException named "InvalidCharacterError" (legacy
  // code 5) with this message. It is a plain JsError subclass because rt::DOMException
  // is defined by the async runtime (see docs/rt/text.md).
  struct InvalidCharacterError : JsError {
    InvalidCharacterError() : JsError("The string contains invalid characters.", "InvalidCharacterError") {}
    int code() const noexcept { return 5; }
  };

  // `btoa(data)`: standard base64 with padding of a binary string. InvalidCharacterError
  // when a code unit is above U+00FF (including every surrogate).
  std::string btoa(std::string_view data);
  // `atob(data)`: forgiving-base64 decode to a binary string. ASCII whitespace (TAB, LF,
  // FF, CR, SPACE) is ignored; one or two '=' are allowed only as padding of a length
  // that is a multiple of 4; a length of 4n+1 or any other character throws
  // InvalidCharacterError. Leftover low bits are discarded.
  std::string atob(std::string_view data);
  // `Uint8Array.from(atob(data), (c) => c.charCodeAt(0))`: atob's bytes, same errors.
  std::vector<std::uint8_t> atobBytes(std::string_view data);

  // The binary string of a byte array: `String.fromCharCode(...bytes)`, `buf.toString("latin1")`.
  std::string binaryString(std::span<const std::uint8_t> bytes);
  // `buf.toString("base64")`, and `btoa(String.fromCharCode(...bytes))`: padded.
  std::string toBase64(std::span<const std::uint8_t> bytes);
  // `buf.toString("base64url")`: URL alphabet, no padding.
  std::string toBase64Url(std::span<const std::uint8_t> bytes);
  // `buf.toString("hex")`, and the `byte.toString(16).padStart(2, "0")` join: lowercase.
  std::string toHex(std::span<const std::uint8_t> bytes);
  // Buffer base64 and base64url accept both alphabets in ASCII input, skip other
  // ASCII bytes, and stop at the first '='. Non-ASCII input yields an empty result.
  std::vector<std::uint8_t> fromBase64(std::string_view text);
  // Buffer hex decodes the complete byte pairs in the leading ASCII hex prefix.
  std::vector<std::uint8_t> fromHex(std::string_view text);

  // Node's Buffer encodings. Names are matched ASCII case-insensitively: utf8/utf-8,
  // ucs2/ucs-2/utf16le/utf-16le, latin1/binary, ascii, base64, base64url, hex.
  enum class BufferEncoding { utf8, utf16le, latin1, ascii, base64, base64url, hex };
  // TypeError "Unknown encoding: <name>" for any other name.
  BufferEncoding bufferEncoding(std::string_view name);

  // `Buffer.from(text, encoding)`. utf8: as TextEncoder (lone surrogates -> EF BF BD);
  // latin1 and ascii: the low byte of each code unit; utf16le: each code unit as two
  // little-endian bytes; base64, base64url, hex: fromBase64 / fromHex.
  std::vector<std::uint8_t> bufferFrom(std::string_view text, BufferEncoding encoding = BufferEncoding::utf8);
  std::vector<std::uint8_t> bufferFrom(std::string_view text, std::string_view encoding);
  // `buf.toString(encoding)`. utf8: replacement decoding that keeps a BOM; latin1: one
  // code point per byte; ascii: the same with the high bit cleared; utf16le: byte pairs
  // as little-endian code units (an odd last byte is ignored; lone surrogates kept).
  std::string bufferToString(std::span<const std::uint8_t> bytes, BufferEncoding encoding = BufferEncoding::utf8);
  std::string bufferToString(std::span<const std::uint8_t> bytes, std::string_view encoding);
}
