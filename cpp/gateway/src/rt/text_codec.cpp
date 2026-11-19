#include "uos/rt/text_codec.hpp"

#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"
#include "third_party/simdutf/simdutf.h"

namespace uos::rt {
  namespace {
    using Byte = std::uint8_t;

    constexpr std::string_view FATAL_MESSAGE = "The encoded data was not valid for encoding utf-8";
    constexpr std::string_view UTF8_LABELS[] = {"unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8"};
    constexpr char REPLACEMENT[] = "\xEF\xBF\xBD";

    // WHATWG UTF-8 decoder bounds for the byte after `lead`: continuation count, and the
    // accepted range of the second byte (narrowed after E0, ED, F0 and F4).
    struct Lead {
      std::size_t needed;
      Byte lower;
      Byte upper;
    };
    constexpr Lead leadOf(Byte lead) {
      if (lead >= 0xC2 && lead <= 0xDF) return {1, 0x80, 0xBF};
      if (lead >= 0xE0 && lead <= 0xEF) return {2, static_cast<Byte>(lead == 0xE0 ? 0xA0 : 0x80), static_cast<Byte>(lead == 0xED ? 0x9F : 0xBF)};
      if (lead >= 0xF0 && lead <= 0xF4) return {3, static_cast<Byte>(lead == 0xF0 ? 0x90 : 0x80), static_cast<Byte>(lead == 0xF4 ? 0x8F : 0xBF)};
      return {0, 0, 0};
    }
    constexpr bool acceptsAt(const Lead& lead, std::size_t position, Byte value) {
      return position == 1 ? value >= lead.lower && value <= lead.upper : value >= 0x80 && value <= 0xBF;
    }

    inline const Byte* bytesOf(std::string_view text) { return reinterpret_cast<const Byte*>(text.data()); }
    inline std::span<const Byte> spanOf(std::string_view text) { return {bytesOf(text), text.size()}; }

    bool isAsciiWhitespace(char value) { return value == '\t' || value == '\n' || value == '\f' || value == '\r' || value == ' '; }

    bool isUtf8Label(std::string_view label) {
      while (!label.empty() && isAsciiWhitespace(label.front())) label.remove_prefix(1);
      while (!label.empty() && isAsciiWhitespace(label.back())) label.remove_suffix(1);
      std::string lowered(label);
      for (char& value : lowered) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + 32);
      }
      for (const std::string_view known : UTF8_LABELS) {
        if (lowered == known) return true;
      }
      return false;
    }

    // Decodes [p, end) with no sequence held from an earlier chunk. Returns the length of
    // the incomplete trailing sequence that `stream` keeps (0 when none). Calls `fail`,
    // which throws, on the first error when fatal.
    template <class Fail>
    std::size_t decodeRun(std::string& out, const Byte* p, const Byte* end, bool fatal, bool stream, Fail&& fail) {
      while (p < end) {
        const auto prefix = simdutf::validate_utf8_with_errors(
          reinterpret_cast<const char*>(p), static_cast<std::size_t>(end - p));
        out.append(reinterpret_cast<const char*>(p), prefix.count);
        p += prefix.count;
        if (p == end) break;

        // Inspect only the rejected sequence. A valid incomplete prefix is held
        // when streaming; other maximal subparts are replaced or rejected.
        const Lead lead = leadOf(*p);
        std::size_t consumed = 1;
        while (consumed <= lead.needed && p + consumed < end && acceptsAt(lead, consumed, p[consumed])) ++consumed;
        if (stream && consumed <= lead.needed && p + consumed == end) return consumed;
        if (fatal) fail();
        out += REPLACEMENT;
        p += consumed;
      }
      return 0;
    }
  }

  std::vector<std::uint8_t> encodeUtf8(std::string_view text) {
    if (isUtf8(text)) return {bytesOf(text), bytesOf(text) + text.size()};
    const std::string wellFormed = toWellFormed(text);
    return {bytesOf(wellFormed), bytesOf(wellFormed) + wellFormed.size()};
  }

  std::size_t utf8ByteLength(std::string_view text) { return isUtf8(text) ? text.size() : toWellFormed(text).size(); }

  bool isUtf8(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() <= limits::kTextInputBytes && (bytes.empty() || simdutf::validate_utf8(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  }
  bool isUtf8(std::string_view bytes) noexcept { return isUtf8(spanOf(bytes)); }

  std::string decodeUtf8(std::span<const std::uint8_t> bytes, TextDecoderOptions options) {
    if (bytes.size() > limits::kTextInputBytes) throw TypeError("Text input exceeds limit");
    const Byte* p = bytes.data();
    const Byte* end = p + bytes.size();
    std::string out;
    out.reserve(bytes.size());
    decodeRun(out, p, end, options.fatal, false, [] { throw TypeError(std::string(FATAL_MESSAGE)); });
    if (!options.ignoreBOM && out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return out;
  }
  std::string decodeUtf8(std::string_view bytes, TextDecoderOptions options) { return decodeUtf8(spanOf(bytes), options); }

  TextDecoder::TextDecoder(std::string_view label, TextDecoderOptions options) : fatal_(options.fatal), ignoreBOM_(options.ignoreBOM) {
    if (!isUtf8Label(label)) throw RangeError("Unsupported encoding label \"" + std::string(label) + "\"");
  }

  void TextDecoder::fail() {
    heldSize_ = 0;
    throw TypeError(std::string(FATAL_MESSAGE));
  }

  // Serializes decoded output: the first code point of a stream is dropped when it is
  // U+FEFF, unless ignoreBOM.
  void TextDecoder::commit(std::string& out, std::string&& part) {
    if (part.empty()) return;
    if (!ignoreBOM_ && !bomSeen_) {
      bomSeen_ = true;
      if (part.compare(0, 3, "\xEF\xBB\xBF") == 0) part.erase(0, 3);
    }
    out += part;
  }

  // Bun decodes a chunk in two parts: first it completes the sequence held from the
  // previous chunk and serializes that code point, then it decodes the rest. A fatal
  // error in the rest therefore loses the rest's output but not the first part's
  // BOM-seen effect, and the bytes after the error are dropped.
  std::string TextDecoder::decode(std::span<const std::uint8_t> input, TextDecodeOptions options) {
    if (input.size() > limits::kTextInputBytes) throw TypeError("Text input exceeds limit");
    if (!doNotFlush_) {
      heldSize_ = 0;
      bomSeen_ = false;
    }
    doNotFlush_ = options.stream;
    std::string out;
    const Byte* p = input.data();
    const Byte* end = p + input.size();
    if (heldSize_ > 0) {
      const Lead lead = leadOf(held_[0]);
      bool broken = false;
      while (heldSize_ < lead.needed + 1 && p < end) {
        if (!acceptsAt(lead, heldSize_, *p)) {
          if (fatal_) fail();
          heldSize_ = 0;
          commit(out, std::string(REPLACEMENT));
          broken = true;
          break;
        }
        held_[heldSize_++] = *p++;
      }
      if (!broken) {
        if (heldSize_ < lead.needed + 1) {
          if (!options.stream) {
            if (fatal_) fail();
            heldSize_ = 0;
            commit(out, std::string(REPLACEMENT));
          }
          return out;
        }
        std::string sequence(reinterpret_cast<const char*>(held_.data()), heldSize_);
        heldSize_ = 0;
        commit(out, std::move(sequence));
      }
    }
    std::string part;
    part.reserve(static_cast<std::size_t>(end - p));
    const std::size_t kept = decodeRun(part, p, end, fatal_, options.stream, [this] { fail(); });
    if (kept > 0) {
      for (std::size_t index = 0; index < kept; ++index) held_[index] = *(end - kept + index);
      heldSize_ = kept;
    }
    commit(out, std::move(part));
    return out;
  }

  std::string TextDecoder::decode(std::string_view input, TextDecodeOptions options) { return decode(spanOf(input), options); }
}
