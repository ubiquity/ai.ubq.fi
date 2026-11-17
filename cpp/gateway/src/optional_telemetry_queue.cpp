// Port of src/optional-telemetry-queue.ts: the non-generic parts (the queue itself is
// a template in the header).
#include "uos/optional_telemetry_queue.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/text_codec.hpp"
#include <vector>

namespace uos::optional_telemetry_queue {
  namespace {
    // The replacer of measureJsonPayloadBytes throws for a non-finite number
    // anywhere in the payload; this finds one without serializing.
    bool hasNonFiniteNumber(const json::value& root) {
      std::vector<const json::value*> pending = {&root};
      while (!pending.empty()) {
        const json::value* value = pending.back();
        pending.pop_back();
        if (const std::optional<double> number = rt::getNumber(*value)) {
          if (!rt::Number::isFinite(*number)) return true;
        } else if (value->is_array()) {
          for (const json::value& item : value->as_array()) pending.push_back(&item);
        } else if (value->is_object()) {
          for (const auto& member : value->as_object()) pending.push_back(&member.value());
        }
      }
      return false;
    }
  }

  double measureJsonPayloadBytes(const json::value& value) {
    try {
      // JSON.stringify(value, replacer) where the replacer throws TypeError("non-finite
      // number") for any non-finite number (and "bigint" for a BigInt, which a
      // json::value cannot hold).
      if (hasNonFiniteNumber(value)) return rt::Number::NaN;
      const std::string serialized = rt::jsonStringify(value);
      return static_cast<double>(rt::utf8ByteLength(serialized));
    } catch (...) {
      return rt::Number::NaN;
    }
  }

  namespace detail {
    double boundedInteger(const std::optional<double>& value, double fallback) {
      return value && rt::Number::isSafeInteger(*value) && *value > 0 ? *value : fallback;
    }

    std::string errorClass(const rt::Reason& error) {
      if (rt::isError(error)) return rt::errorName(error).value_or("Error");
      const std::optional<json::value> thrown = rt::thrownValue(error);
      return thrown ? std::string(rt::typeOf(*thrown)) : std::string("undefined");
    }
  }
}
