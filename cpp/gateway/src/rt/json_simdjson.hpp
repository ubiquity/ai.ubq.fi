#pragma once

#include "uos/rt/json.hpp"

#include <simdjson.h>

#include <cfenv>
#include <cstring>
#include <memory>

namespace uos::rt::detail {
  namespace simdDom = simdjson::dom;

  inline constexpr std::size_t kSimdJsonMaxDepth = 32;
  inline constexpr std::size_t kSimdJsonMaxInput = 2 * 1024 * 1024;

  class SimdJsonInput {
  public:
    bool parse(std::string_view text, simdDom::element& root) {
      // Reject potential negative zero before parsing because the tape can lose its sign.
      if (text.empty() || text.size() > kSimdJsonMaxInput || text.starts_with("\xef\xbb\xbf") ||
          text.find("-0") != std::string_view::npos || std::fegetround() != FE_TONEAREST)
        return false;
      if (parser_.capacity() < text.size() && parser_.allocate(text.size(), kSimdJsonMaxDepth + 1)) return false;
      const std::size_t size = text.size() + simdjson::SIMDJSON_PADDING;
      if (capacity_ < size) {
        buffer_ = std::make_unique_for_overwrite<char[]>(size);
        capacity_ = size;
      }
      std::memcpy(buffer_.get(), text.data(), text.size());
      std::memset(buffer_.get() + text.size(), 0, simdjson::SIMDJSON_PADDING);
      const simdjson::padded_string_view payload(buffer_.get(), text.size(), size);
      return !parser_.parse(payload).get(root);
    }

  private:
    simdDom::parser parser_{kSimdJsonMaxInput};
    std::unique_ptr<char[]> buffer_;
    std::size_t capacity_ = 0;
  };

  inline void putSimdNumber(json::value& output, double number) {
    constexpr double maxSafeInteger = 9007199254740991.0;
    if (number >= -maxSafeInteger && number <= maxSafeInteger && std::trunc(number) == number &&
        !(number == 0 && std::signbit(number)))
      output.emplace_int64() = static_cast<std::int64_t>(number);
    else
      output.emplace_double() = number;
  }

  inline bool convertSimdJson(simdDom::element node, json::value& output, std::size_t depth) {
    switch (node.type()) {
      case simdDom::element_type::ARRAY: {
        if (depth >= kSimdJsonMaxDepth) return false;
        simdDom::array input;
        if (node.get_array().get(input)) return false;
        auto& array = output.emplace_array();
        array.reserve(input.size());
        for (const auto element : input) {
          if (!convertSimdJson(element, array.emplace_back(nullptr), depth + 1)) return false;
        }
        return true;
      }
      case simdDom::element_type::OBJECT: {
        if (depth >= kSimdJsonMaxDepth) return false;
        simdDom::object input;
        if (node.get_object().get(input)) return false;
        auto& object = output.emplace_object();
        object.reserve(input.size());
        for (const auto member : input) {
          if (member.key == "__proto__" || isArrayIndex(member.key)) return false;
          // The tape retains occurrences of each decoded key in input order.
          const auto [position, inserted] = object.emplace(member.key, nullptr);
          if (!inserted || !convertSimdJson(member.value, position->value(), depth + 1)) return false;
        }
        return true;
      }
      case simdDom::element_type::INT64: {
        constexpr std::int64_t maxSafeInteger = 9007199254740991;
        std::int64_t value;
        if (node.get_int64().get(value) || value < -maxSafeInteger || value > maxSafeInteger) return false;
        output.emplace_int64() = value;
        return true;
      }
      case simdDom::element_type::DOUBLE: {
        double value;
        // The exact parser handles decimal underflow and other range edges
        // using the literal spelling, which the DOM does not retain.
        if (node.get_double().get(value) || !std::isnormal(value)) return false;
        putSimdNumber(output, value);
        return true;
      }
      case simdDom::element_type::STRING: {
        std::string_view text;
        if (node.get_string().get(text)) return false;
        output.emplace_string().assign(text);
        return true;
      }
      case simdDom::element_type::BOOL: {
        bool value;
        if (node.get_bool().get(value)) return false;
        output.emplace_bool() = value;
        return true;
      }
      case simdDom::element_type::NULL_VALUE:
        return true;
      case simdDom::element_type::UINT64:
      case simdDom::element_type::BIGINT:
        return false;
    }
    return false;
  }

  inline std::optional<json::value> tryParseSimdJson(std::string_view text) {
    thread_local SimdJsonInput input;
    simdDom::element root;
    if (!input.parse(text, root)) return std::nullopt;
    json::value value;
    if (!convertSimdJson(root, value, 0)) return std::nullopt;
    return value;
  }
}
