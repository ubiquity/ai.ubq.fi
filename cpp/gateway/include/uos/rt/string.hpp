#pragma once
// JavaScript string semantics over UTF-8/WTF-8 storage. See docs/rt/text.md for the
// encoding convention: a JS string is a std::string holding canonical WTF-8, and every
// index, length and position below counts UTF-16 code units exactly as JS does.
#include "uos/rt/error.hpp"
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::rt {
  // `throw new URIError(m)` and the URIError thrown by decodeURIComponent and friends.
  struct URIError : JsError {
    explicit URIError(std::string message_, std::shared_ptr<const JsError> cause_ = nullptr) : JsError(std::move(message_), "URIError", std::move(cause_)) {}
  };

  // A JS number argument (index, position, count, length). Accepts any arithmetic
  // value; the callee applies ToIntegerOrInfinity (NaN -> 0, truncation, +-Infinity).
  struct NumberArg {
    double value;
    constexpr NumberArg(double number) : value(number) {}
    template <class T>
      requires std::integral<T>
    constexpr NumberArg(T number) : value(static_cast<double>(number)) {}
  };

  // ---- Encoding layer -------------------------------------------------------------
  bool isAscii(std::string_view text) noexcept;
  // UTF-16 code units of a WTF-8 string (a split surrogate pair reads as the pair;
  // ill-formed bytes read as one U+FFFD per maximal subpart).
  std::u16string toUtf16(std::string_view text);
  // Canonical WTF-8 of a UTF-16 code unit sequence: pairs become one 4-byte sequence,
  // lone surrogates their 3-byte form. Inverse of toUtf16 on canonical input.
  std::string fromUtf16(std::u16string_view units);
  // Appends one code point or lone surrogate as canonical WTF-8. A low surrogate
  // appended right after a lone high surrogate completes the pair (JSON "😀").
  void appendCodePoint(std::string& out, std::uint32_t codePoint);
  // Appends `tail`, joining a trailing lone high surrogate of `out` with a leading lone
  // low surrogate of `tail` (JS `+`). Plain `out += tail` is equal whenever that join
  // cannot happen.
  void append(std::string& out, std::string_view tail);
  std::string concat(std::string_view left, std::string_view right);
  // True when the bytes are generalized UTF-8 (UTF-8 plus encoded surrogates).
  bool isWtf8(std::string_view bytes) noexcept;
  // True when isWtf8 and no high surrogate is directly followed by a low surrogate.
  bool isCanonicalWtf8(std::string_view bytes) noexcept;
  // Rewrites any byte string into canonical WTF-8 using the reading rules above.
  std::string canonicalWtf8(std::string_view bytes);

  // String.prototype.isWellFormed / toWellFormed (lone surrogates -> U+FFFD).
  bool isWellFormed(std::string_view text) noexcept;
  std::string toWellFormed(std::string_view text);

  // ---- Length, access, slicing (UTF-16 indexes) ------------------------------------
  std::int64_t length(std::string_view text) noexcept;  // s.length
  std::string charAt(std::string_view text, NumberArg position);
  double charCodeAt(std::string_view text, NumberArg position);  // NaN when out of range
  std::optional<std::string> at(std::string_view text, NumberArg index);
  std::string slice(std::string_view text, NumberArg start);
  std::string slice(std::string_view text, NumberArg start, NumberArg end);
  std::string substring(std::string_view text, NumberArg start);
  std::string substring(std::string_view text, NumberArg start, NumberArg end);
  std::string substr(std::string_view text, NumberArg start);
  std::string substr(std::string_view text, NumberArg start, NumberArg count);

  // ---- Search ----------------------------------------------------------------------
  std::int64_t indexOf(std::string_view text, std::string_view search, NumberArg position = 0);
  std::int64_t lastIndexOf(std::string_view text, std::string_view search);
  std::int64_t lastIndexOf(std::string_view text, std::string_view search, NumberArg position);
  bool includes(std::string_view text, std::string_view search, NumberArg position = 0);
  bool startsWith(std::string_view text, std::string_view search, NumberArg position = 0);
  bool endsWith(std::string_view text, std::string_view search);
  bool endsWith(std::string_view text, std::string_view search, NumberArg endPosition);

  // ---- Transformations ---------------------------------------------------------------
  std::string padStart(std::string_view text, NumberArg maxLength, std::string_view fill = " ");
  std::string padEnd(std::string_view text, NumberArg maxLength, std::string_view fill = " ");
  std::string repeat(std::string_view text, NumberArg count);  // RangeError as JS
  std::string trim(std::string_view text);
  std::string trimEnd(std::string_view text);
  bool isJsWhitespace(std::uint32_t codePoint) noexcept;  // WhiteSpace or LineTerminator
  std::vector<std::string> split(std::string_view text);  // separator undefined
  std::vector<std::string> split(std::string_view text, std::string_view separator);
  std::vector<std::string> split(std::string_view text, std::string_view separator, NumberArg limit);
  // String-pattern replace / replaceAll, with GetSubstitution ($$, $&, $`, $').
  std::string replace(std::string_view text, std::string_view pattern, std::string_view replacement);
  std::string replaceAll(std::string_view text, std::string_view pattern, std::string_view replacement);
  // Unicode case mapping in the ICU root locale.
  std::string toLowerCase(std::string_view text);
  std::string toUpperCase(std::string_view text);

  // ---- Construction ------------------------------------------------------------------
  std::string fromCharCode(std::uint16_t unit);
  std::string fromCharCode(std::span<const std::uint16_t> units);
  // String.fromCharCode(...bytes): the Latin-1 "binary string" of a byte array.
  std::string fromCharCode(std::span<const std::uint8_t> bytes);
  std::string fromCodePoint(NumberArg codePoint);  // RangeError as JS
  std::string fromCodePoint(std::span<const std::uint32_t> codePoints);

  // ---- Arrays of strings ---------------------------------------------------------------
  // `array.join(separator)` over strings (`array.join()` joins with ","). Pieces are appended
  // like JS `+` (rt::append), so a lone high surrogate before a lone low one becomes a pair.
  inline std::string join(std::span<const std::string> values, std::string_view separator = ",") {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) append(out, separator);
      append(out, values[index]);
    }
    return out;
  }

  // ---- Comparison --------------------------------------------------------------------
  // JS relational order (<, >, default Array.prototype.sort): UTF-16 code unit order.
  int compare(std::string_view left, std::string_view right);  // <0, 0, >0
  bool lessThan(std::string_view left, std::string_view right);
  // ICU root collation with canonical normalization and tertiary strength.
  // Returns -1, 0 or 1.
  int localeCompare(std::string_view left, std::string_view right);

  // ---- URI coding --------------------------------------------------------------------
  std::string encodeURIComponent(std::string_view text);  // URIError on lone surrogates
  std::string encodeURI(std::string_view text);
  std::string decodeURIComponent(std::string_view text);  // URIError as JS
  std::string decodeURI(std::string_view text);

  // ---- Iteration by code point (for...of, [...s], Array.from(s)) -------------------
  // Yields each code point; an unpaired surrogate is yielded as its own value.
  class CodePoints {
  public:
    class iterator {
    public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = std::uint32_t;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = std::uint32_t;
      iterator() = default;
      iterator(const char* position, const char* end) : position_(position), end_(end) { decode(); }
      std::uint32_t operator*() const { return value_; }
      iterator& operator++() {
        position_ += size_;
        decode();
        return *this;
      }
      iterator operator++(int) {
        iterator copy = *this;
        ++*this;
        return copy;
      }
      bool operator==(const iterator& other) const { return position_ == other.position_; }
      // The bytes of the current element, as a JS string ([...s] element).
      std::string_view text() const { return {position_, size_}; }

    private:
      void decode();
      const char* position_ = nullptr;
      const char* end_ = nullptr;
      std::size_t size_ = 0;
      std::uint32_t value_ = 0;
    };
    explicit CodePoints(std::string_view text) : text_(text) {}
    iterator begin() const { return {text_.data(), text_.data() + text_.size()}; }
    iterator end() const { return {text_.data() + text_.size(), text_.data() + text_.size()}; }

  private:
    std::string_view text_;
  };
  inline CodePoints codePoints(std::string_view text) { return CodePoints(text); }
  // [...s] / Array.from(s): each element is one code point as a JS string.
  std::vector<std::string> codePointStrings(std::string_view text);
  // `ch.charCodeAt(0)` for an element of for...of: the lead UTF-16 code unit.
  constexpr std::uint16_t leadCodeUnit(std::uint32_t codePoint) noexcept {
    return codePoint >= 0x10000 ? static_cast<std::uint16_t>(0xD800 + ((codePoint - 0x10000) >> 10)) : static_cast<std::uint16_t>(codePoint);
  }
}
