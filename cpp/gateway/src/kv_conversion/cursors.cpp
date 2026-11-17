#include "uos/kv_conversion/cursors.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>
#include <optional>
#include <utility>

namespace uos::kv_conversion::cursors {
  namespace {
    const std::vector<std::string> prefix{"uos_ai", "sentinel_replay", "v1"};
    const std::vector<std::string> bootstrapParts{"accounting", "chunk", "dedupe", "evicted", "manifest", "request", "reservation"};

    // [...prefix, part]
    std::vector<std::string> prefixed(const std::string& part) {
      std::vector<std::string> parts = prefix;
      parts.push_back(part);
      return parts;
    }
  }

  std::vector<std::uint8_t> packStringKey(const std::vector<std::string>& parts) {
    std::vector<std::uint8_t> packed;
    for (const std::string& part : parts) {
      packed.push_back(2);
      const std::vector<std::uint8_t> encoded = rt::encodeUtf8(part);
      packed.insert(packed.end(), encoded.begin(), encoded.end());
      packed.push_back(0);
    }
    return packed;
  }

  namespace {
    std::string hex(std::span<const std::uint8_t> value) { return rt::toHex(value); }

    std::string cursorForPrefix(const std::string& cursor, const std::vector<std::string>& parts, const ConversionDirection& direction) {
      const std::vector<std::uint8_t> packed = packStringKey(parts);
      if (direction == "deno-to-bun") {
        static const rt::Regex LEGACY_CURSOR("^[A-Za-z0-9_-]+={0,2}$");
        if (!LEGACY_CURSOR.test(cursor)) throw rt::JsError("Invalid legacy cursor");
        const std::vector<std::uint8_t> suffix = rt::fromBase64(cursor);
        std::string unpadded = cursor;
        while (rt::endsWith(unpadded, "=")) unpadded = rt::slice(unpadded, 0, -1);
        if (rt::toBase64Url(suffix) != unpadded) throw rt::JsError("Noncanonical legacy cursor");
        std::vector<std::uint8_t> full = packed;
        full.insert(full.end(), suffix.begin(), suffix.end());
        json::object record;
        record["lastYieldedKeyBytes"] = hex(full);
        return rt::toBase64(rt::encodeUtf8(rt::jsonStringify(record)));
      }
      const json::value decoded = rt::jsonParse(rt::bufferToString(rt::fromBase64(cursor)));
      if (decoded.is_null()) throw rt::TypeError("null is not an object (evaluating 'decoded.lastYieldedKeyBytes')");
      static const rt::Regex HEX_BYTES("^(?:[a-f0-9]{2})+$");
      const std::optional<std::string_view> lastYieldedKeyBytes = rt::getString(decoded, "lastYieldedKeyBytes");
      if (!lastYieldedKeyBytes || !HEX_BYTES.test(*lastYieldedKeyBytes)) throw rt::JsError("Invalid Bun cursor");
      const std::vector<std::uint8_t> full = rt::fromHex(*lastYieldedKeyBytes);
      if (full.size() < packed.size() || !std::equal(packed.begin(), packed.end(), full.begin())) throw rt::JsError("Cursor outside expected replay prefix");
      const std::span<const std::uint8_t> tail = std::span<const std::uint8_t>(full).subspan(packed.size());
      return rt::replaceAll(rt::replaceAll(rt::toBase64(tail), "+", "-"), "/", "_");
    }

    // `bootstrapParts[Number(digits)]`: the part, or nullptr where JS reads undefined.
    const std::string* bootstrapPartAt(const std::string& digits) {
      const double index = rt::stringToNumber(digits);
      if (!(index >= 0 && index < static_cast<double>(bootstrapParts.size())) || rt::Math::trunc(index) != index) return nullptr;
      return &bootstrapParts[static_cast<std::size_t>(index)];
    }
  }

  TranscodePersistedCursorResult transcodePersistedCursor(std::span<const std::uint8_t> key, const rt::KvValue& value, const ConversionDirection& direction) {
    if (hex(key) == hex(packStringKey(prefixed("reap_cursor")))) {
      if (!value.isString() || value.asString().empty()) throw rt::JsError("Malformed replay reaper cursor");
      return {cursorForPrefix(value.asString(), prefixed("accounting"), direction), true};
    }
    if (hex(key) != hex(packStringKey(prefixed("budget")))) return {value, false};
    // Only a plain object can own "bootstrap_cursor": no other stored value type has it.
    if (!value.isObject() || !value.asObject().contains("bootstrap_cursor")) throw rt::JsError("Malformed replay budget ledger");
    const rt::KvObject& ledger = value.asObject();
    const rt::KvValue& bootstrapCursor = ledger.at("bootstrap_cursor");
    if (bootstrapCursor.isNull()) return {value, false};
    if (!bootstrapCursor.isString()) throw rt::JsError("Malformed replay bootstrap cursor");
    static const rt::Regex BOOTSTRAP_CURSOR("^(\\d+)\\|(.*)$");
    const std::optional<rt::RegexMatch> match = BOOTSTRAP_CURSOR.exec(bootstrapCursor.asString());
    const std::string* part = match ? bootstrapPartAt(*(*match)[1]) : nullptr;
    if (!match || part == nullptr || part->empty()) throw rt::JsError("Unsupported replay bootstrap prefix");
    if ((*match)[2]->empty()) return {value, false};
    rt::KvObject next = ledger;
    next.set("bootstrap_cursor", *(*match)[1] + "|" + cursorForPrefix(*(*match)[2], prefixed(*part), direction));
    return {rt::KvValue(std::move(next)), true};
  }
}
