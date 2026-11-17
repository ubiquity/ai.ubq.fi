#include "uos/rt/kv_js.hpp"

#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace uos::rt {
  namespace {
    using Options = StoredJsonViewOptions;

    [[noreturn]] void tooDeep() { throw RangeError("Maximum call stack size exceeded."); }
    [[noreturn]] void bigintInJson() { throw TypeError("JSON.stringify cannot serialize BigInt."); }

    // ---- typed array elements ----------------------------------------------------------------

    std::uint64_t elementSize(KvViewKind kind) noexcept {
      switch (kind) {
        case KvViewKind::DataView:
        case KvViewKind::Int8Array:
        case KvViewKind::Uint8Array:
        case KvViewKind::Uint8ClampedArray:
          return 1;
        case KvViewKind::Int16Array:
        case KvViewKind::Uint16Array:
          return 2;
        case KvViewKind::Int32Array:
        case KvViewKind::Uint32Array:
        case KvViewKind::Float32Array:
          return 4;
        case KvViewKind::Float64Array:
        case KvViewKind::BigInt64Array:
        case KvViewKind::BigUint64Array:
          return 8;
      }
      return 1;
    }

    std::uint64_t elementCount(const KvArrayBufferView& view) noexcept { return view.byteLength / elementSize(view.kind); }

    template <class T> T readElement(const KvArrayBufferView& view, std::uint64_t index) noexcept {
      T element{};
      std::memcpy(&element, view.buffer.data() + view.byteOffset + index * sizeof(T), sizeof(T));
      return element;
    }

    bool isBigIntView(KvViewKind kind) noexcept { return kind == KvViewKind::BigInt64Array || kind == KvViewKind::BigUint64Array; }

    // A non-bigint element as the JS number it reads as.
    double numberElement(const KvArrayBufferView& view, std::uint64_t index) noexcept {
      switch (view.kind) {
        case KvViewKind::Int8Array:
          return readElement<std::int8_t>(view, index);
        case KvViewKind::Int16Array:
          return readElement<std::int16_t>(view, index);
        case KvViewKind::Uint16Array:
          return readElement<std::uint16_t>(view, index);
        case KvViewKind::Int32Array:
          return readElement<std::int32_t>(view, index);
        case KvViewKind::Uint32Array:
          return readElement<std::uint32_t>(view, index);
        case KvViewKind::Float32Array:
          return static_cast<double>(readElement<float>(view, index));
        case KvViewKind::Float64Array:
          return readElement<double>(view, index);
        case KvViewKind::DataView:
        case KvViewKind::Uint8Array:
        case KvViewKind::Uint8ClampedArray:
        case KvViewKind::BigInt64Array:
        case KvViewKind::BigUint64Array:
          break;
      }
      return readElement<std::uint8_t>(view, index);
    }

    BigInt bigintElement(const KvArrayBufferView& view, std::uint64_t index) {
      if (view.kind == KvViewKind::BigInt64Array) return BigInt(readElement<std::int64_t>(view, index));
      return BigInt::fromUint64(readElement<std::uint64_t>(view, index));
    }

    std::string indexKey(std::uint64_t index) { return std::to_string(index); }

    // ---- the JSON view ---------------------------------------------------------------------------

    json::value undefinedAs(Options::UndefinedView mode) {
      if (mode == Options::UndefinedView::emptyObject) return json::object();
      return nullptr;
    }

    json::value bigintAs(const BigInt& value, Options::BigIntView mode) {
      switch (mode) {
        case Options::BigIntView::emptyObject:
          return json::object();
        case Options::BigIntView::null:
          return nullptr;
        case Options::BigIntView::decimalString:
          return json::string(value.toString());
        case Options::BigIntView::number:
          return makeNumber(value.toNumber());
      }
      return json::object();
    }

    json::value viewOf(const KvValue& value, const Options& options, std::size_t depth) {
      if (depth > kJsonMaxDepth) tooDeep();
      return std::visit(
        [&](const auto& item) -> json::value {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, KvUndefined>) return undefinedAs(options.topLevelUndefined);
          else if constexpr (std::is_same_v<T, KvHole>) return undefinedAs(options.hole);
          else if constexpr (std::is_same_v<T, std::nullptr_t>) return nullptr;
          else if constexpr (std::is_same_v<T, bool>) return item;
          else if constexpr (std::is_same_v<T, double>) return makeNumber(item);
          else if constexpr (std::is_same_v<T, std::string>) return json::string(item);
          else if constexpr (std::is_same_v<T, BigInt>) return bigintAs(item, options.bigint);
          else if constexpr (std::is_same_v<T, KvU64>) {
            json::object out;
            if (options.kvU64Members) out["value"] = bigintAs(item.toBigInt(), options.bigint);
            return out;
          } else if constexpr (std::is_same_v<T, KvBytes>) {
            json::object out;
            if (options.typedArrayMembers)
              for (std::size_t index = 0; index < item.size(); ++index) out[indexKey(index)] = makeNumber(static_cast<std::int64_t>(item[index]));
            return out;
          } else if constexpr (std::is_same_v<T, KvArrayBufferView>) {
            json::object out;
            if (!options.typedArrayMembers || item.kind == KvViewKind::DataView) return out;
            const std::uint64_t count = elementCount(item);
            for (std::uint64_t index = 0; index < count; ++index) {
              if (isBigIntView(item.kind)) out[indexKey(index)] = bigintAs(bigintElement(item, index), options.bigint);
              else out[indexKey(index)] = makeNumber(numberElement(item, index));
            }
            return out;
          } else if constexpr (std::is_same_v<T, KvArray>) {
            json::array out;
            out.reserve(item.size());
            for (const KvValue& element : item) {
              if (element.isUndefined()) out.push_back(undefinedAs(options.undefinedElement));
              else out.push_back(viewOf(element, options, depth + 1));
            }
            return out;
          } else if constexpr (std::is_same_v<T, KvObject>) {
            json::object out;
            for (const auto& [key, member] : item) {
              if (!member.isUndefined()) objectDefine(out, key, viewOf(member, options, depth + 1));
              else if (options.undefinedMember != Options::UndefinedView::omit) objectDefine(out, key, undefinedAs(options.undefinedMember));
            }
            return out;
          } else {
            // ArrayBuffer, DataView, Date, RegExp, Error, Map, Set: no own enumerable property.
            return json::object();
          }
        },
        value.variant());
    }

    // ---- JSON.stringify ----------------------------------------------------------------------------

    // The json::value JSON.stringify serialises for `value`; std::nullopt is undefined.
    std::optional<json::value> stringifiedOf(const KvValue& value, std::size_t depth) {
      if (depth > kJsonMaxDepth) tooDeep();
      return std::visit(
        [&](const auto& item) -> std::optional<json::value> {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, KvUndefined> || std::is_same_v<T, KvHole>) return std::nullopt;
          else if constexpr (std::is_same_v<T, std::nullptr_t>) return json::value(nullptr);
          else if constexpr (std::is_same_v<T, bool>) return json::value(item);
          else if constexpr (std::is_same_v<T, double>) return makeNumber(item);
          else if constexpr (std::is_same_v<T, std::string>) return json::value(json::string(item));
          else if constexpr (std::is_same_v<T, BigInt> || std::is_same_v<T, KvU64>) bigintInJson();
          else if constexpr (std::is_same_v<T, KvBytes>) {
            json::object out;
            for (std::size_t index = 0; index < item.size(); ++index) out[indexKey(index)] = makeNumber(static_cast<std::int64_t>(item[index]));
            return json::value(std::move(out));
          } else if constexpr (std::is_same_v<T, KvArrayBufferView>) {
            json::object out;
            if (item.kind == KvViewKind::DataView) return json::value(std::move(out));
            const std::uint64_t count = elementCount(item);
            if (isBigIntView(item.kind) && count > 0) bigintInJson();
            for (std::uint64_t index = 0; index < count; ++index) out[indexKey(index)] = makeNumber(numberElement(item, index));
            return json::value(std::move(out));
          } else if constexpr (std::is_same_v<T, KvDate>) {
            const std::optional<std::string> iso = Date(item.time).toJSON();
            if (!iso) return json::value(nullptr);
            return json::value(json::string(*iso));
          } else if constexpr (std::is_same_v<T, KvArray>) {
            json::array out;
            out.reserve(item.size());
            for (const KvValue& element : item) {
              std::optional<json::value> converted = stringifiedOf(element, depth + 1);
              out.push_back(converted ? std::move(*converted) : json::value(nullptr));
            }
            return json::value(std::move(out));
          } else if constexpr (std::is_same_v<T, KvObject>) {
            json::object out;
            for (const auto& [key, member] : item) {
              std::optional<json::value> converted = stringifiedOf(member, depth + 1);
              if (converted) objectDefine(out, key, std::move(*converted));
            }
            return json::value(std::move(out));
          } else {
            // ArrayBuffer, RegExp, Error, Map, Set: no own enumerable property, no toJSON.
            return json::value(json::object());
          }
        },
        value.variant());
    }

    // ---- String(value) -------------------------------------------------------------------------------

    std::string stringOf(const KvValue& value, std::size_t depth);

    std::string joined(const KvArray& items, std::size_t depth) {
      std::string out;
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) out += ',';
        const KvValue& element = items[index];
        if (!element.isNullish() && !element.is<KvHole>()) out += stringOf(element, depth + 1);
      }
      return out;
    }

    std::string viewText(const KvArrayBufferView& view) {
      if (view.kind == KvViewKind::DataView) return "[object DataView]";
      std::string out;
      const std::uint64_t count = elementCount(view);
      for (std::uint64_t index = 0; index < count; ++index) {
        if (index > 0) out += ',';
        out += isBigIntView(view.kind) ? bigintElement(view, index).toString() : numberToString(numberElement(view, index));
      }
      return out;
    }

    std::string stringOf(const KvValue& value, std::size_t depth) {
      if (depth > kJsonMaxDepth) tooDeep();
      return std::visit(
        [&](const auto& item) -> std::string {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, KvUndefined> || std::is_same_v<T, KvHole>) return "undefined";
          else if constexpr (std::is_same_v<T, std::nullptr_t>) return "null";
          else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
          else if constexpr (std::is_same_v<T, double>) return numberToString(item);
          else if constexpr (std::is_same_v<T, std::string>) return item;
          else if constexpr (std::is_same_v<T, BigInt>) return item.toString();
          else if constexpr (std::is_same_v<T, KvU64>) return "[object Object]";
          else if constexpr (std::is_same_v<T, KvBytes>) {
            std::string out;
            for (std::size_t index = 0; index < item.size(); ++index) {
              if (index > 0) out += ',';
              out += std::to_string(item[index]);
            }
            return out;
          } else if constexpr (std::is_same_v<T, KvArrayBuffer>) return "[object ArrayBuffer]";
          else if constexpr (std::is_same_v<T, KvArrayBufferView>) return viewText(item);
          else if constexpr (std::is_same_v<T, KvDate>) return Date(item.time).toString();
          else if constexpr (std::is_same_v<T, KvRegExp>) return "/" + item.source + "/" + item.flags;
          else if constexpr (std::is_same_v<T, KvError>) {
            const std::string message = item.message.value_or("");
            if (item.name.empty()) return message;
            if (message.empty()) return item.name;
            return item.name + ": " + message;
          } else if constexpr (std::is_same_v<T, KvArray>) return joined(item, depth);
          else if constexpr (std::is_same_v<T, KvObject>) {
            // An own `toString` (a stored member, never callable) hides Object.prototype.toString;
            // OrdinaryToPrimitive then finds no primitive.
            if (item.contains("toString")) throw TypeError("No default value");
            return "[object Object]";
          } else if constexpr (std::is_same_v<T, KvMap>) return "[object Map]";
          else return "[object Set]";
        },
        value.variant());
    }

    [[noreturn]] void unsupportedKeyPart(const KvValue& value) { throw JsError("Unsupported keyPart: " + std::string(kvTypeof(value)) + " " + kvToString(value)); }
  }

  // ---- typeof and record guards ---------------------------------------------------------------

  std::string_view kvTypeof(const KvValue& value) noexcept {
    if (value.isUndefined() || value.is<KvHole>()) return "undefined";
    if (value.isBool()) return "boolean";
    if (value.isNumber()) return "number";
    if (value.isString()) return "string";
    if (value.isBigInt()) return "bigint";
    return "object";
  }

  bool kvIsObject(const KvValue& value) noexcept { return kvTypeof(value) == "object" && !value.isNull(); }

  bool kvIsRecord(const KvValue& value) noexcept { return kvIsObject(value) && !value.isArray(); }

  // ---- JSON view, JSON.stringify, String ------------------------------------------------------

  json::value storedJsonView(const KvValue& value) { return viewOf(value, Options{}, 0); }

  json::value storedJsonView(const KvValue& value, const StoredJsonViewOptions& options) { return viewOf(value, options, 0); }

  std::optional<std::string> kvJsonStringify(const KvValue& value) {
    const std::optional<json::value> converted = stringifiedOf(value, 0);
    if (!converted) return std::nullopt;
    return jsonStringify(*converted);
  }

  std::optional<std::string> kvJsonStringify(const KvValue& value, int space) {
    const std::optional<json::value> converted = stringifiedOf(value, 0);
    if (!converted) return std::nullopt;
    return jsonStringify(*converted, space);
  }

  std::optional<std::string> kvJsonStringify(const KvValue& value, std::string_view gap) {
    const std::optional<json::value> converted = stringifiedOf(value, 0);
    if (!converted) return std::nullopt;
    return jsonStringify(*converted, gap);
  }

  std::string kvToString(const KvValue& value) { return stringOf(value, 0); }

  // ---- instanceof Uint8Array --------------------------------------------------------------------

  bool kvIsUint8Array(const KvValue& value) noexcept {
    if (value.isBytes()) return true;
    const auto* view = std::get_if<KvArrayBufferView>(&value.variant());
    return view != nullptr && view->kind == KvViewKind::Uint8Array;
  }

  std::optional<std::span<const std::uint8_t>> kvUint8ArrayBytes(const KvValue& value) noexcept {
    if (value.isBytes()) return std::span<const std::uint8_t>(value.asBytes());
    const auto* view = std::get_if<KvArrayBufferView>(&value.variant());
    if (view == nullptr || view->kind != KvViewKind::Uint8Array) return std::nullopt;
    return std::span<const std::uint8_t>(view->buffer).subspan(static_cast<std::size_t>(view->byteOffset), static_cast<std::size_t>(view->byteLength));
  }

  // ---- keys and key parts -----------------------------------------------------------------------

  KvValue kvValueFromKeyPart(const KvKeyPart& part) {
    return std::visit([](const auto& item) { return KvValue(item); }, part.value);
  }

  KvValue kvValueFromKey(const KvKey& key) {
    KvArray parts;
    parts.reserve(key.size());
    for (const KvKeyPart& part : key) parts.push_back(kvValueFromKeyPart(part));
    return KvValue(std::move(parts));
  }

  std::optional<KvKeyPart> kvKeyPartFromValue(const KvValue& value) {
    if (value.isString()) return KvKeyPart(value.asString());
    if (value.isNumber()) return KvKeyPart(value.asNumber());
    if (value.isBigInt()) return KvKeyPart(value.asBigInt());
    if (value.isBool()) return KvKeyPart(value.asBool());
    if (const std::optional<std::span<const std::uint8_t>> bytes = kvUint8ArrayBytes(value)) return KvKeyPart(KvBytes(bytes->begin(), bytes->end()));
    return std::nullopt;
  }

  KvKeyPart toKvKeyPart(const KvValue& value) {
    std::optional<KvKeyPart> part = kvKeyPartFromValue(value);
    if (!part) unsupportedKeyPart(value);
    return std::move(*part);
  }

  KvKey kvKeyFromValue(const KvValue& value) {
    KvKey key;
    for (const KvValue& element : value.asArray()) {
      if (element.is<KvHole>()) continue;
      key.push_back(toKvKeyPart(element));
    }
    return key;
  }

  bool kvKeyPartStrictEquals(const KvKeyPart& left, const KvKeyPart& right) noexcept {
    if (left.isString() && right.isString()) return std::get<std::string>(left.value) == std::get<std::string>(right.value);
    if (left.isNumber() && right.isNumber()) return std::get<double>(left.value) == std::get<double>(right.value);
    if (left.isBigInt() && right.isBigInt()) return std::get<BigInt>(left.value) == std::get<BigInt>(right.value);
    if (left.isBool() && right.isBool()) return std::get<bool>(left.value) == std::get<bool>(right.value);
    if (left.isBytes() && right.isBytes()) return &left == &right;
    return false;
  }

  bool kvKeyStrictEquals(const KvKey& left, const KvKey& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
      if (!kvKeyPartStrictEquals(left[index], right[index])) return false;
    return true;
  }
}
