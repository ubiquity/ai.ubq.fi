#pragma once
// Helpers shared by the src/rt/kv*.cpp translation units. Not a public header.
#include "uos/rt/kv.hpp"
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>

namespace uos::rt::kv_detail {
  inline constexpr std::size_t MaxContainerDepth = 2000;

  inline void checkContainerDepth(std::size_t depth) {
    if (depth >= MaxContainerDepth) throw RangeError("Maximum call stack size exceeded.");
  }

  struct GraphNode {
    KvValue value;
    std::unique_ptr<KvValue> backing;
    std::size_t height = 0;
  };

  // Codecs build a graph through borrowed links. The arena owns every node,
  // and only handles that leave it own the arena, so cycles reclaim normally.
  class ValueGraph : public std::enable_shared_from_this<ValueGraph> {
  public:
    KvValue add(KvValue value);
    KvValue link(GraphNode& node) const;
    static KvValue borrow(const KvValue& value);
    static GraphNode& node(const KvValue& link);
    static KvValue finish(KvValue value);

  private:
    std::deque<GraphNode> nodes_;
  };
}

namespace uos::rt::kv_internal {
  // The UTF-16 code units of a WTF-8 string (docs/rt/text.md): a split surrogate
  // pair reads as the pair, a lone surrogate as itself, each maximal ill-formed
  // subpart as one U+FFFD.
  std::u16string utf16Units(std::string_view wtf8);
  // `new TextEncoder().encode(string)`: UTF-8 with every lone surrogate as U+FFFD.
  std::string textEncode(std::string_view wtf8);
  // `new TextDecoder().decode(bytes)`: WHATWG UTF-8 with U+FFFD replacement, one
  // leading BOM dropped. The result is well-formed UTF-8.
  std::string textDecode(std::span<const std::uint8_t> bytes);
  // Lowercase hex of the bytes (std's encodeHex).
  std::string encodeHex(std::span<const std::uint8_t> bytes);
  // std@0.208.0 decodeHex: TypeError "Invalid byte '<c>'", RangeError "Odd length
  // hex string".
  KvBytes decodeHex(std::string_view text);
  // The magnitude of a BigInt as big-endian bytes without leading zeros (empty for 0n).
  KvBytes bigintMagnitude(const BigInt& value);
  // The BigInt with this big-endian magnitude, negated when `negative`.
  BigInt bigintFromMagnitude(std::span<const std::uint8_t> magnitude, bool negative);
}
