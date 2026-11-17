#pragma once
// TextEncoder / TextDecoder (WHATWG Encoding, UTF-8) with Bun 1.4's results. Strings are
// canonical WTF-8 (docs/rt/text.md); bytes are returned as std::vector<std::uint8_t> and
// taken as std::span<const std::uint8_t>, as everywhere in uos::rt.
#include "uos/rt/error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uos::rt {
  // `new TextEncoder().encode(text)` and node `Buffer.from(text)`: the UTF-8 bytes of
  // the string; every lone surrogate becomes EF BF BD (U+FFFD). Equal to the bytes of
  // rt::toWellFormed(text).
  std::vector<std::uint8_t> encodeUtf8(std::string_view text);
  // `new TextEncoder().encode(text).byteLength` (and `.length`) without the array.
  std::size_t utf8ByteLength(std::string_view text);
  // True when the bytes are well-formed UTF-8: no surrogate code points, no overlong,
  // truncated or out-of-range sequences. Such bytes decode to themselves.
  bool isUtf8(std::string_view bytes) noexcept;
  bool isUtf8(std::span<const std::uint8_t> bytes) noexcept;

  // `new TextDecoder(label, options)` options and `decode(input, options)` options.
  struct TextDecoderOptions {
    bool fatal = false;
    bool ignoreBOM = false;
  };
  struct TextDecodeOptions {
    bool stream = false;
  };

  // One-shot `new TextDecoder(undefined, options).decode(bytes)`: each maximal ill-formed
  // subpart becomes one U+FFFD (fatal: TypeError instead), and a leading U+FEFF is
  // dropped unless ignoreBOM. Node `buf.toString()` / `toString("utf8")` is
  // decodeUtf8(bytes, {.ignoreBOM = true}): Buffer keeps the BOM.
  std::string decodeUtf8(std::span<const std::uint8_t> bytes, TextDecoderOptions options = {});
  std::string decodeUtf8(std::string_view bytes, TextDecoderOptions options = {});

  class TextEncoder {
  public:
    std::string_view encoding() const noexcept { return "utf-8"; }
    // `encoder.encode(input)`; `encode()` with no argument is empty.
    std::vector<std::uint8_t> encode(std::string_view input = {}) const { return encodeUtf8(input); }
  };

  class TextDecoder {
  public:
    // `new TextDecoder(label?, options?)`. The label is matched after trimming ASCII
    // whitespace, ASCII case-insensitively, against the UTF-8 labels (utf-8, utf8,
    // unicode-1-1-utf-8, unicode11utf8, unicode20utf8, x-unicode20utf8). Any other label
    // throws RangeError `Unsupported encoding label "<label>"` (Bun accepts the other
    // WHATWG encodings; the ported code uses none, see docs/rt/text.md).
    explicit TextDecoder(std::string_view label = "utf-8", TextDecoderOptions options = {});
    explicit TextDecoder(TextDecoderOptions options) : TextDecoder("utf-8", options) {}

    std::string_view encoding() const noexcept { return "utf-8"; }
    bool fatal() const noexcept { return fatal_; }
    bool ignoreBOM() const noexcept { return ignoreBOM_; }

    // `decoder.decode(input?, { stream })`. With stream an incomplete trailing sequence
    // is held for the next call; without it the decoder flushes (a held sequence becomes
    // U+FFFD or the fatal error) and the next call starts fresh. Fatal errors throw
    // TypeError "The encoded data was not valid for encoding utf-8" and, as in Bun, drop
    // the held bytes and the rest of the chunk. The BOM is dropped once per stream.
    std::string decode(std::span<const std::uint8_t> input = {}, TextDecodeOptions options = {});
    std::string decode(std::string_view input, TextDecodeOptions options = {});

  private:
    void commit(std::string& out, std::string&& part);
    [[noreturn]] void fail();

    bool fatal_ = false;
    bool ignoreBOM_ = false;
    bool doNotFlush_ = false;
    bool bomSeen_ = false;
    std::array<std::uint8_t, 4> held_{};
    std::size_t heldSize_ = 0;
  };
}
