#pragma once

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"

#include <simdjson.h>

#include <algorithm>

namespace uos::rt::detail {
  namespace simd = simdjson::ondemand;

  class JsonInput {
  public:
    json::value parse(std::string_view text) {
      buffer_.assign(text);
      buffer_.append(simdjson::SIMDJSON_PADDING, '\0');
      if (parser_.capacity() < text.size()) {
        if (const auto error = parser_.allocate(text.size(), limits::kJsonDepth + 2)) throw simdjson::simdjson_error(error);
      }
      simd::document document = parser_.iterate(buffer_.data(), text.size(), buffer_.size()).value();
      json::value result = read(document, 0);
      if (!document.at_end()) throw SyntaxError("Invalid JSON");
      return result;
    }

  private:
    template <class Node>
    json::value read(Node& node, std::size_t depth) {
      const auto type = node.type().value();
      if (depth >= limits::kJsonDepth && (type == simd::json_type::array || type == simd::json_type::object))
        throw SyntaxError("Invalid JSON");
      switch (type) {
        case simd::json_type::array: {
          json::array output;
          auto array = node.get_array().value();
          for (auto element : array) {
            auto child = std::move(element).value();
            output.push_back(read(child, depth + 1));
          }
          return output;
        }
        case simd::json_type::object: {
          json::object output;
          auto object = node.get_object().value();
          for (auto entry : object) {
            std::string_view key = entry.escaped_key().value();
            if (key.find('\\') != std::string_view::npos) {
              keyBuffer_.resize(key.size() + simdjson::SIMDJSON_PADDING);
              auto* destination = reinterpret_cast<std::uint8_t*>(keyBuffer_.data());
              key = parser_.unescape_wobbly(entry.key().value(), destination).value();
            }
            auto& child = output[key];
            auto fieldValue = entry.value().value();
            child = read(fieldValue, depth + 1);
          }
          return output;
        }
        case simd::json_type::number: {
          const double number = node.get_double().value();
          if (!std::isfinite(number)) throw SyntaxError("Invalid JSON");
          return makeNumber(number);
        }
        case simd::json_type::string:
          return json::string(node.get_wobbly_string().value());
        case simd::json_type::boolean:
          return node.get_bool().value();
        case simd::json_type::null:
          if (node.is_null().value()) return nullptr;
          break;
        case simd::json_type::unknown:
          break;
      }
      throw SyntaxError("Invalid JSON");
    }

    simd::parser parser_;
    std::string buffer_;
    std::string keyBuffer_;
  };

  inline json::value parseSimdJson(std::string_view text, std::size_t maxInput) {
    constexpr std::size_t maxReusableInput = 2 * 1024 * 1024;
    if (text.empty() || text.size() > std::min(maxInput, limits::kImageEditJsonBytes) || text.starts_with("\xef\xbb\xbf")) throw SyntaxError("Invalid JSON");
    try {
      if (text.size() <= maxReusableInput) {
        thread_local JsonInput input;
        return input.parse(text);
      }
      return JsonInput().parse(text);
    } catch (const simdjson::simdjson_error&) {
      throw SyntaxError("Invalid JSON");
    }
  }
}
