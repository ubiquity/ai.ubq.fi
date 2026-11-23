#include "uos/kv_conversion/profile_json.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <variant>

namespace uos::kv_conversion::profile_json {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    // `Uint8Array.from(Buffer.from(value, "base64url"))`
    rt::KvBytes bytes(std::string_view value) { return rt::fromBase64(value); }

    KvValueJson binary(std::string_view type, std::span<const std::uint8_t> value) {
      KvValueJson json;
      json["type"] = type;
      json["value"] = rt::toBase64Url(value);
      json["byteLength"] = rt::makeNumber(static_cast<double>(value.size()));
      return json;
    }

    // `typedArrays`: the constructor names toValue rebuilds over a whole decoded buffer.
    struct TypedArray {
      std::string_view name;
      rt::KvViewKind kind;
      std::size_t elementSize;
    };
    constexpr std::array<TypedArray, 11> TYPED_ARRAYS{{
      {"Int8Array", rt::KvViewKind::Int8Array, 1},
      {"Uint8Array", rt::KvViewKind::Uint8Array, 1},
      {"Uint8ClampedArray", rt::KvViewKind::Uint8ClampedArray, 1},
      {"Int16Array", rt::KvViewKind::Int16Array, 2},
      {"Uint16Array", rt::KvViewKind::Uint16Array, 2},
      {"Int32Array", rt::KvViewKind::Int32Array, 4},
      {"Uint32Array", rt::KvViewKind::Uint32Array, 4},
      {"Float32Array", rt::KvViewKind::Float32Array, 4},
      {"Float64Array", rt::KvViewKind::Float64Array, 8},
      {"BigInt64Array", rt::KvViewKind::BigInt64Array, 8},
      {"BigUint64Array", rt::KvViewKind::BigUint64Array, 8},
    }};

    const TypedArray* typedArray(std::string_view name) {
      for (const TypedArray& candidate : TYPED_ARRAYS) {
        if (candidate.name == name) return &candidate;
      }
      return nullptr;
    }

    // `errors`
    constexpr std::array<std::string_view, 7> ERRORS{"Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"};

    bool isErrorName(std::string_view name) {
      for (const std::string_view candidate : ERRORS) {
        if (candidate == name) return true;
      }
      return false;
    }

    KvValueJson profileObjectToJson(const rt::KvValue& value) {
      KvValueJson json;
      const rt::KvValue::Variant& variant = value.variant();
      if (value.isU64()) {
        json["type"] = "KvU64";
        json["value"] = rt::BigInt::fromUint64(value.asU64().value).toString();
        return json;
      }
      if (const auto* date = std::get_if<rt::KvDate>(&variant)) {
        const std::optional<std::string> iso = rt::Date(date->time).toJSON();
        json["type"] = "Date";
        json["value"] = iso ? json::value(json::string(*iso)) : json::value(nullptr);
        return json;
      }
      if (const auto* regexp = std::get_if<rt::KvRegExp>(&variant)) {
        json["type"] = "RegExp";
        json["value"] = "/" + regexp->source + "/" + regexp->flags;
        return json;
      }
      if (const auto* buffer = std::get_if<rt::KvArrayBuffer>(&variant)) return binary("ArrayBuffer", buffer->bytes);
      // Preserve the legacy profile payload contract, including its whole backing-buffer representation.
      if (value.isBytes()) {
        KvValueJson view = binary("Uint8Array", value.asBytes());
        view["byteLength"] = rt::makeNumber(static_cast<double>(value.asBytes().size()));
        return view;
      }
      if (const auto* typed = std::get_if<rt::KvArrayBufferView>(&variant)) {
        KvValueJson view = binary(rt::kvViewKindName(typed->kind), typed->buffer);
        view["byteLength"] = rt::makeNumber(static_cast<double>(typed->byteLength));
        return view;
      }
      if (value.isArray()) {
        json::array items;
        // `value.map(encode)` keeps a hole, which JSON writes as null.
        for (const rt::KvValue& item : value.asArray()) items.push_back(item.is<rt::KvHole>() ? json::value(nullptr) : json::value(valueToJson(item)));
        json["type"] = "Array";
        json["value"] = std::move(items);
        return json;
      }
      if (const auto* map = std::get_if<rt::KvMap>(&variant)) {
        json::array entries;
        for (const auto& [key, item] : *map) entries.push_back(json::array{valueToJson(key), valueToJson(item)});
        json["type"] = "Map";
        json["value"] = std::move(entries);
        return json;
      }
      if (const auto* set = std::get_if<rt::KvSet>(&variant)) {
        json::array items;
        for (const rt::KvValue& item : *set) items.push_back(valueToJson(item));
        json["type"] = "Set";
        json["value"] = std::move(items);
        return json;
      }
      if (const auto* error = std::get_if<rt::KvError>(&variant)) {
        // `value.message` is the inherited "" when the error has no own message.
        KvValueJson detail;
        detail["message"] = error->message.value_or("");
        if (error->stack && !error->stack->empty()) detail["stack"] = *error->stack;
        if (error->cause && error->cause->truthy()) detail["cause"] = valueToJson(*error->cause);
        json["type"] = isErrorName(error->name) ? error->name : std::string("Error");
        json["value"] = std::move(detail);
        return json;
      }
      KvValueJson members;
      for (const auto& [key, item] : value.asObject()) rt::objectDefine(members, key, json::value(valueToJson(item)));
      json["type"] = "object";
      json["value"] = std::move(members);
      return json;
    }

    // `value.map(...)` as the TS calls it on an unchecked `value`.
    const json::array& mapTarget(const json::value* value, std::string_view expression) {
      if (value == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'value.map')");
      if (value->is_null()) throw rt::TypeError("null is not an object (evaluating 'value.map')");
      if (!value->is_array()) throw rt::TypeError("value.map is not a function. (In '" + std::string(expression) + "', 'value.map' is undefined)");
      return value->get_array();
    }

    // The `[key, item]` destructuring of a Map entry: the first two elements of an array or the
    // first two characters of a string (missing ones undefined).
    std::pair<std::optional<json::value>, std::optional<json::value>> mapEntry(const json::value& entry) {
      if (const json::array* items = entry.if_array()) {
        std::optional<json::value> key;
        std::optional<json::value> item;
        if (!items->empty()) key = (*items)[0];
        if (items->size() > 1) item = (*items)[1];
        return {std::move(key), std::move(item)};
      }
      if (entry.is_string()) {
        const std::vector<std::string> characters = rt::codePointStrings(std::string_view(entry.get_string()));
        std::optional<json::value> key;
        std::optional<json::value> item;
        if (!characters.empty()) key = json::string(characters[0]);
        if (characters.size() > 1) item = json::string(characters[1]);
        return {std::move(key), std::move(item)};
      }
      if (entry.is_null()) throw rt::TypeError("null is not an object (evaluating '[key, item]')");
      if (entry.is_bool()) throw rt::TypeError(std::string(entry.get_bool() ? "true" : "false") + " is not iterable");
      if (entry.is_object()) throw rt::TypeError("{} is not iterable");
      throw rt::TypeError("number is not iterable");
    }

    // `new Date(value)`: a number is clipped, a string parsed, an array or object read through
    // its string form.
    double dateTime(const json::value* value) {
      if (value == nullptr) return NaN;
      if (value->is_null()) return 0;
      if (value->is_bool()) return value->get_bool() ? 1 : 0;
      if (const std::optional<double> number = rt::getNumber(*value)) return rt::Date(*number).getTime();
      const std::string text = rt::toString(*value);
      return rt::Date(std::string_view(text)).getTime();
    }

    // One `json.map((part) => ...)` step of toKey.
    rt::KvKeyPart keyPart(const json::value& part) {
      if (part.is_null()) throw rt::TypeError("null is not an object (evaluating 'part.type')");
      const std::optional<std::string_view> type = rt::getString(rt::get(part, "type"));
      if (!type || !(*type == "bigint" || *type == "boolean" || *type == "number" || *type == "string" || *type == "Uint8Array")) {
        throw rt::TypeError("Unsupported profile key type");
      }
      const rt::KvValue value = toValue(&part);
      if (value.isString()) return rt::KvKeyPart(value.asString());
      if (value.isNumber()) return rt::KvKeyPart(value.asNumber());
      if (value.isBool()) return rt::KvKeyPart(value.asBool());
      if (value.isBigInt()) return rt::KvKeyPart(value.asBigInt());
      if (value.isBytes()) return rt::KvKeyPart(value.asBytes());
      // Only a "boolean" or "string" tag returns its raw value, which can be any JSON value.
      const json::value* raw = rt::get(part, "value");
      throw rt::JsError("Unsupported keyPart: " + std::string(rt::typeOf(raw)) + " " + rt::toString(raw));
    }
  }

  KvValueJson keyPartToJson(const rt::KvKeyPart& value) {
    if (value.isBytes()) return binary("Uint8Array", value.bytes());
    KvValueJson json;
    if (value.isBigInt()) {
      json["type"] = "bigint";
      json["value"] = value.bigint().toString();
    } else if (value.isNumber()) {
      const double number = value.number();
      json["type"] = "number";
      json["value"] = rt::Number::isFinite(number) ? rt::makeNumber(number) : json::value(json::string(rt::numberToString(number)));
    } else if (value.isString()) {
      json["type"] = "string";
      json["value"] = value.string();
    } else {
      json["type"] = "boolean";
      json["value"] = value.boolean();
    }
    return json;
  }

  std::vector<KvValueJson> keyToJson(const rt::KvKey& key) {
    std::vector<KvValueJson> parts;
    parts.reserve(key.size());
    for (const rt::KvKeyPart& part : key) parts.push_back(keyPartToJson(part));
    return parts;
  }

  KvValueJson valueToJson(const rt::KvValue& value) {
    if (value.isUndefined()) {
      KvValueJson json;
      json["type"] = "undefined";
      return json;
    }
    if (value.isNull()) {
      KvValueJson json;
      json["type"] = "null";
      json["value"] = nullptr;
      return json;
    }
    if (value.isBool()) return keyPartToJson(rt::KvKeyPart(value.asBool()));
    if (value.isNumber()) return keyPartToJson(rt::KvKeyPart(value.asNumber()));
    if (value.isString()) return keyPartToJson(rt::KvKeyPart(value.asString()));
    if (value.isBigInt()) return keyPartToJson(rt::KvKeyPart(value.asBigInt()));
    return profileObjectToJson(value);
  }

  rt::KvValue toValue(const json::value& input) { return toValue(&input); }

  rt::KvValue toValue(const json::value* input) {
    const json::object* object = input != nullptr ? input->if_object() : nullptr;
    const json::value* typeMember = object != nullptr ? rt::get(*object, "type") : nullptr;
    if (typeMember == nullptr || !typeMember->is_string()) throw rt::TypeError("Malformed profile value");
    const std::string type(typeMember->get_string());
    const json::value* value = rt::get(*object, "value");
    if (const TypedArray* typed = typedArray(type)) {
      rt::KvBytes buffer = bytes(rt::toString(value));
      if (typed->kind == rt::KvViewKind::Uint8Array) return rt::KvValue(std::move(buffer));
      if (buffer.size() % typed->elementSize != 0) throw rt::RangeError("ArrayBuffer length minus the byteOffset is not a multiple of the element size");
      rt::KvArrayBufferView view;
      view.kind = typed->kind;
      view.byteLength = buffer.size();
      view.buffer = std::move(buffer);
      return rt::KvValue(std::move(view));
    }
    if (isErrorName(type)) {
      // `detail.message` on an undefined or null `value` throws; on another primitive it is undefined.
      if (value == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'detail.message')");
      if (value->is_null()) throw rt::TypeError("null is not an object (evaluating 'detail.message')");
      const json::value* message = rt::get(*value, "message");
      const json::value* cause = rt::get(*value, "cause");
      rt::KvError error;
      error.name = type;
      if (rt::truthy(cause)) error.cause = std::make_shared<const rt::KvValue>(toValue(cause));
      if (message != nullptr) error.message = rt::toString(*message);
      const json::value* stack = rt::get(*value, "stack");
      // rt::KvError holds a string stack: another truthy value is kept as its string form.
      if (rt::truthy(stack)) error.stack = rt::toString(*stack);
      return rt::KvValue(std::move(error));
    }
    if (type == "undefined") return rt::KvValue();
    if (type == "null") return rt::KvValue(nullptr);
    if (type == "bigint") return rt::KvValue(rt::BigInt::fromString(rt::toString(value)));
    if (type == "number") return rt::KvValue(value != nullptr && rt::isNumber(*value) ? *rt::getNumber(*value) : rt::toNumber(value));
    if (type == "boolean" || type == "string") return value != nullptr ? rt::KvValue(*value) : rt::KvValue();
    if (type == "KvU64") return rt::KvValue(rt::KvU64(rt::BigInt::fromString(rt::toString(value))));
    if (type == "ArrayBuffer") return rt::KvValue(rt::KvArrayBuffer{bytes(rt::toString(value))});
    if (type == "DataView") {
      rt::KvArrayBufferView view;
      view.kind = rt::KvViewKind::DataView;
      view.buffer = bytes(rt::toString(value));
      view.byteLength = view.buffer.size();
      return rt::KvValue(std::move(view));
    }
    if (type == "Date") return rt::KvValue(rt::KvDate{dateTime(value)});
    if (type == "RegExp") {
      const std::string pattern = rt::toString(value);
      const std::int64_t split = rt::lastIndexOf(pattern, "/");
      const rt::Regex regexp(rt::slice(pattern, 1, split), rt::slice(pattern, split + 1));
      return rt::KvValue(rt::KvRegExp{regexp.source(), regexp.flags()});
    }
    if (type == "Array") {
      rt::KvArray items;
      for (const json::value& item : mapTarget(value, "value.map(toValue)")) items.push_back(toValue(&item));
      return rt::KvValue(std::move(items));
    }
    if (type == "Set") {
      rt::KvArray items;
      for (const json::value& item : mapTarget(value, "value.map(toValue)")) items.push_back(toValue(&item));
      rt::KvSet set;
      for (rt::KvValue& item : items) set.add(std::move(item));
      return rt::KvValue(std::move(set));
    }
    if (type == "Map") {
      std::vector<std::pair<rt::KvValue, rt::KvValue>> entries;
      for (const json::value& entry : mapTarget(value, "value.map(([key, item]) => [toValue(key), toValue(item)])")) {
        const auto [key, item] = mapEntry(entry);
        rt::KvValue convertedKey = toValue(key ? &*key : nullptr);
        rt::KvValue convertedItem = toValue(item ? &*item : nullptr);
        entries.emplace_back(std::move(convertedKey), std::move(convertedItem));
      }
      rt::KvMap map;
      for (auto& [key, item] : entries) map.set(std::move(key), std::move(item));
      return rt::KvValue(std::move(map));
    }
    if (type == "object") {
      // Object.entries(value): a string enumerates its characters, an array its indexes, another
      // primitive nothing; null and undefined throw.
      if (value == nullptr || value->is_null()) throw rt::TypeError("Object.entries requires that input parameter not be null or undefined");
      rt::KvObject members;
      if (const json::object* fields = value->if_object()) {
        for (const json::key_value_pair* entry : rt::objectEntries(*fields)) members.set(std::string(entry->key()), toValue(&entry->value()));
      } else if (const json::array* items = value->if_array()) {
        for (std::size_t index = 0; index < items->size(); ++index) members.set(rt::numberToString(static_cast<double>(index)), toValue(&(*items)[index]));
      } else if (value->is_string() && !value->get_string().empty()) {
        // The first character is a string, which is never a profile value.
        const json::value first(json::string(rt::charAt(std::string_view(value->get_string()), 0)));
        members.set("0", toValue(&first));
      }
      return rt::KvValue(std::move(members));
    }
    throw rt::TypeError("Unsupported profile value type: " + type);
  }

  rt::KvKey toKey(const std::vector<KvValueJson>& json) {
    rt::KvKey key;
    for (const KvValueJson& part : json) {
      const json::value value(part);
      key.push_back(keyPart(value));
    }
    return key;
  }

  rt::KvKey toKey(const json::array& json) {
    rt::KvKey key;
    for (const json::value& part : json) key.push_back(keyPart(part));
    return key;
  }

  KvEntryJson entryToJson(const rt::KvEntry<>& entry) {
    return KvEntryJson{.key = keyToJson(entry.key), .value = valueToJson(entry.value), .versionstamp = entry.versionstamp};
  }

  rt::ReadableStream<rt::ByteChunk> exportEntries(rt::Kv& kv, rt::KvListSelector selector) {
    std::shared_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [iterator](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      auto reading = iterator->next();
      const std::optional<rt::KvEntry<>> entry = co_await std::move(reading);
      if (!entry) {
        controller.close();
      } else {
        const json::value line = json::value_from(entryToJson(*entry));
        controller.enqueue(rt::encodeUtf8(rt::jsonStringify(line) + "\n"));
      }
    };
    source.cancel = [iterator](rt::Reason) -> rt::Task<void> {
      auto returning = iterator->return_();
      co_await std::move(returning);
    };
    return rt::ReadableStream<rt::ByteChunk>(std::move(source));
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvEntryJson& value) {
    out = Writer().required("key", value.key).required("value", value.value).required("versionstamp", value.versionstamp).done();
  }

  KvEntryJson tag_invoke(const json::value_to_tag<KvEntryJson>&, const json::value& value) {
    const Reader in(value, "KvEntryJson");
    return {
      .key = in.required<std::vector<json::object>>("key"),
      .value = in.required<json::object>("value"),
      .versionstamp = in.required<std::string>("versionstamp"),
    };
  }
}
