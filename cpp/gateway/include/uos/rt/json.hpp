#pragma once
// JavaScript value semantics over boost::json (oracle: Bun 1.4 / JavaScriptCore).
// docs/rt/values.md has the porting table, the canonical number rule and the
// undefined/null conventions. Strings are canonical WTF-8 (docs/rt/text.md).
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/string.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt {
  // ---- Numbers inside json::value ------------------------------------------------------
  // Canonical rule: a JS number is stored as kind::int64 when it is an integer with
  // |x| <= 2^53 - 1 and is not -0; every other number (fractions, -0, NaN, +-Infinity,
  // |x| >= 2^53) is kind::double_. jsonParse, makeNumber and the rt writers store numbers
  // this way, so two canonical values compare equal with boost's == exactly when the JS
  // numbers are the same (NaN aside). Readers accept int64, uint64 and double alike; an
  // int64/uint64 outside the safe range reads as the nearest double, as a JS literal would.
  json::value makeNumber(double value);
  json::value makeNumber(std::int64_t value);
  // The value with every number rewritten canonically (deep).
  json::value canonicalNumbers(json::value value);
  bool isNumber(const json::value& value) noexcept;  // typeof value === "number"

  // ---- typeof, guards, truthiness ------------------------------------------------------
  // A json::value never holds `undefined`: an absent member is `nullptr` (getters below),
  // std::nullopt, or Nullable<T>::undefined (uos/rt/nullable.hpp).
  std::string_view typeOf(const json::value& value) noexcept;  // "object" for null, arrays and objects
  std::string_view typeOf(const json::value* value) noexcept;  // nullptr: "undefined"
  // typeof v === "object" && v !== null && !Array.isArray(v)
  bool isRecord(const json::value& value) noexcept;
  bool isRecord(const json::value* value) noexcept;
  // v == null (loose equality with null: undefined or null).
  bool isNullish(const json::value* value) noexcept;
  // Boolean(v) / `if (v)`: false for undefined, null, false, 0, -0, NaN and "".
  bool truthy(const json::value& value) noexcept;
  bool truthy(const json::value* value) noexcept;
  bool truthy(double value) noexcept;
  bool truthy(std::string_view value) noexcept;
  template <class T>
    requires std::is_integral_v<T>
  bool truthy(T value) noexcept {
    return value != 0;
  }
  template <class T>
  bool truthy(const std::optional<T>& value) {
    return value.has_value() && truthy(*value);
  }

  // ---- Coercions ---------------------------------------------------------------------------
  // Number(v) / unary +: null 0, booleans 0/1, strings by StringToNumber, arrays and
  // objects through String(v) ("" -> 0, "[object Object]" -> NaN).
  double toNumber(const json::value& value);
  double toNumber(const json::value* value);  // nullptr: NaN
  // String(v) / `${v}`: arrays join their elements with "," (null elements print ""),
  // objects print "[object Object]".
  std::string toString(const json::value& value);
  std::string toString(const json::value* value);  // nullptr: "undefined"

  // Number.isFinite / isNaN / isInteger / isSafeInteger on an unknown value: false for
  // anything that is not a number. The global isNaN / isFinite coerce with Number(v).
  namespace Number {
    bool isFinite(const json::value& value) noexcept;
    bool isNaN(const json::value& value) noexcept;
    bool isInteger(const json::value& value) noexcept;
    bool isSafeInteger(const json::value& value) noexcept;
  }
  bool isNaN(const json::value& value);
  bool isFinite(const json::value& value);

  // ---- Equality ----------------------------------------------------------------------------
  bool sameValue(double a, double b) noexcept;      // Object.is
  bool sameValueZero(double a, double b) noexcept;  // Array.includes, Map and Set keys
  // a === b. Primitives compare by value (NaN !== NaN, 0 === -0). Arrays and objects
  // compare by identity: true only when both references name the same json::value.
  bool strictEquals(const json::value& a, const json::value& b) noexcept;
  // Structural equality of JSON data as assert.deepEqual (node:assert/strict),
  // expect().toEqual and Bun.deepEquals agree on it: numbers by Object.is (NaN equals NaN,
  // 0 differs from -0), member order ignored, arrays by position.
  bool deepEqual(const json::value& a, const json::value& b);
  // expect(received).toMatchObject(expected): every member of an expected object must
  // match recursively (extra received members allowed); expected arrays need the same
  // length; primitives by Object.is.
  bool matchesObject(const json::value& received, const json::value& expected);
  // structuredClone(v) of JSON data (an independent deep copy).
  json::value structuredClone(const json::value& value);

  // ---- Members and property order ----------------------------------------------------------
  // JS enumerates own keys as: canonical array indexes ("0".."4294967294") ascending, then
  // every other key in insertion order. json::object keeps insertion order; the helpers
  // below enumerate in JS order whatever the storage order, and the writers keep storage
  // in JS order. Never call json::object::erase: it moves the last member into the hole.
  bool isArrayIndex(std::string_view key) noexcept;
  // obj[key] / obj.key for an own member; nullptr is undefined. Inherited Object.prototype
  // names ("toString", "constructor", "__proto__") are not modelled: they read as nullptr.
  const json::value* get(const json::object& object, std::string_view key) noexcept;
  json::value* get(json::object& object, std::string_view key) noexcept;
  // value[key] on an unknown value: the member when value is an object, else nullptr.
  const json::value* get(const json::value& value, std::string_view key) noexcept;
  const json::value* get(const json::value* value, std::string_view key) noexcept;
  // array[index]: nullptr past the end.
  const json::value* get(const json::array& array, std::size_t index) noexcept;
  namespace detail {
    // An integer index: an integral type other than bool and the character types.
    template <class I>
    concept JsonIndex = std::is_integral_v<I> && !std::is_same_v<I, bool> && !std::is_same_v<I, char> && !std::is_same_v<I, wchar_t> && !std::is_same_v<I, char8_t> &&
                        !std::is_same_v<I, char16_t> && !std::is_same_v<I, char32_t>;
    // obj[index]: the member named String(index).
    template <class Object, class I> auto* memberAt(Object& object, I index) noexcept {
      char digits[24];
      const auto written = std::to_chars(digits, digits + sizeof digits, index);
      const auto found = object.find(std::string_view(digits, static_cast<std::size_t>(written.ptr - digits)));
      return found == object.end() ? nullptr : &found->value();
    }
  }
  // value[index] with an integer index (so a literal `0` reads index 0 instead of converting to a
  // null string_view): the element of an array (nullptr below 0 or past the end), the member
  // String(index) of an object (`obj[0]` is `obj["0"]`), else nullptr (strings are not indexed).
  template <detail::JsonIndex I>
  const json::value* get(const json::array& array, I index) noexcept {
    if constexpr (std::is_signed_v<I>)
      if (index < 0) return nullptr;
    return static_cast<std::uint64_t>(index) < array.size() ? &array[static_cast<std::size_t>(index)] : nullptr;
  }
  template <detail::JsonIndex I>
  const json::value* get(const json::object& object, I index) noexcept {
    return detail::memberAt(object, index);
  }
  template <detail::JsonIndex I>
  json::value* get(json::object& object, I index) noexcept {
    return detail::memberAt(object, index);
  }
  template <detail::JsonIndex I>
  const json::value* get(const json::value& value, I index) noexcept {
    if (const json::array* array = value.if_array()) return get(*array, index);
    if (const json::object* object = value.if_object()) return get(*object, index);
    return nullptr;
  }
  template <detail::JsonIndex I>
  const json::value* get(const json::value* value, I index) noexcept {
    return value ? get(*value, index) : nullptr;
  }
  // Object.hasOwn(obj, key) / obj.hasOwnProperty(key) / `key in obj` for own members.
  bool hasOwn(const json::object& object, std::string_view key) noexcept;

  // Typed reads: the member (or the value itself) when it has that JS type, else nullopt /
  // nullptr. getNumber accepts every number kind and returns the double.
  std::optional<std::string_view> getString(const json::value& value) noexcept;
  std::optional<double> getNumber(const json::value& value) noexcept;
  std::optional<bool> getBool(const json::value& value) noexcept;
  const json::object* getObject(const json::value& value) noexcept;
  const json::array* getArray(const json::value& value) noexcept;
  std::optional<std::string_view> getString(const json::value* value) noexcept;
  std::optional<double> getNumber(const json::value* value) noexcept;
  std::optional<bool> getBool(const json::value* value) noexcept;
  const json::object* getObject(const json::value* value) noexcept;
  const json::array* getArray(const json::value* value) noexcept;
  std::optional<std::string_view> getString(const json::object& object, std::string_view key) noexcept;
  std::optional<double> getNumber(const json::object& object, std::string_view key) noexcept;
  std::optional<bool> getBool(const json::object& object, std::string_view key) noexcept;
  const json::object* getObject(const json::object& object, std::string_view key) noexcept;
  const json::array* getArray(const json::object& object, std::string_view key) noexcept;
  json::object* getObject(json::object& object, std::string_view key) noexcept;
  json::array* getArray(json::object& object, std::string_view key) noexcept;
  std::optional<std::string_view> getString(const json::value& value, std::string_view key) noexcept;
  std::optional<double> getNumber(const json::value& value, std::string_view key) noexcept;
  std::optional<bool> getBool(const json::value& value, std::string_view key) noexcept;
  const json::object* getObject(const json::value& value, std::string_view key) noexcept;
  const json::array* getArray(const json::value& value, std::string_view key) noexcept;

  // Object.keys / Object.values / Object.entries, in JS order. The pointers stay valid
  // until the object is modified.
  std::vector<std::string> objectKeys(const json::object& object);
  std::vector<const json::value*> objectValues(const json::object& object);
  std::vector<const json::key_value_pair*> objectEntries(const json::object& object);
  std::vector<json::key_value_pair*> objectEntries(json::object& object);

  // obj[key] = value ([[Set]]): an existing member keeps its position; a new array-index
  // key goes into ascending index order ahead of the string keys; a new string key is
  // appended. "__proto__" without an own "__proto__" member creates nothing (JS calls the
  // prototype setter instead). Numbers are stored canonically.
  void objectSet(json::object& object, std::string_view key, json::value value);
  // CreateDataProperty: object literals, spread, Object.fromEntries and JSON.parse.
  // As objectSet, except that "__proto__" is an ordinary key.
  void objectDefine(json::object& object, std::string_view key, json::value value);
  // delete obj[key]; true when a member was removed. Keeps the order of the rest.
  bool objectDelete(json::object& object, std::string_view key) noexcept;
  // Object.assign(target, source): objectSet for each source member in JS order.
  void objectAssign(json::object& target, const json::object& source);
  // {...a, ...b, ...}: a new object; members defined in argument order, later wins in place.
  json::object objectSpread(std::initializer_list<const json::object*> sources);
  template <class... Objects>
    requires(std::is_convertible_v<const Objects&, const json::object&> && ...)
  json::object objectSpread(const Objects&... sources) {
    return objectSpread({static_cast<const json::object*>(&sources)...});
  }
  // Object.fromEntries(entries).
  json::object objectFromEntries(const std::vector<std::pair<std::string, json::value>>& entries);

  // ---- JSON ---------------------------------------------------------------------------------
  // Use the parser's container bound for recursive operations on JSON values.
  inline constexpr std::size_t kJsonMaxDepth = limits::kJsonDepth;

  // JSON.stringify(value): byte-identical to JavaScriptCore (shortest round-trip numbers,
  // non-finite numbers and -0 as null and 0, lowercase \u00XX escapes for control
  // characters, lone surrogates as \udXXX, U+2028/U+2029 unescaped, members in JS order).
  std::string jsonStringify(const json::value& value);
  // JSON.stringify(value, null, space): space clamped to 0..10.
  std::string jsonStringify(const json::value& value, int space);
  // JSON.stringify(value, null, gap): the first 10 UTF-16 units of the gap string.
  std::string jsonStringify(const json::value& value, std::string_view gap);
  // JSON.stringify(value, replacer, gap). The replacer sees each (key, value) before it is
  // serialised, the root with key ""; array elements get their index as key. Returning
  // nullopt (undefined) drops an object member, prints null for an array element, and makes
  // the whole result undefined (nullopt) at the root. Exceptions propagate.
  using JsonReplacer = std::function<std::optional<json::value>(std::string_view key, const json::value& value)>;
  std::optional<std::string> jsonStringify(const json::value& value, const JsonReplacer& replacer, std::string_view gap = {});
  // JSON.stringify(text) for a string.
  std::string jsonQuote(std::string_view text);
  // JSON.parse(text). Throws rt::SyntaxError with JavaScriptCore's message text
  // ("JSON Parse error: Unexpected EOF", ...). Duplicate keys: the last value wins at the
  // first key's position. "__proto__" is an ordinary key. Numbers are canonical.
  json::value jsonParse(std::string_view text);
  // await response.json() / request.json(): JSON.parse of the decoded body text (UTF-8,
  // BOM stripped), but Bun throws its own SyntaxError messages: "Unexpected end of JSON
  // input" when the body had no bytes at all, "Failed to parse JSON" for anything else.
  json::value jsonParseBody(std::string_view text, bool emptyBody);

  // ---- Arrays -------------------------------------------------------------------------------
  namespace detail {
    template <class Compare, class T>
    double sortCompare(Compare& compare, const T& a, const T& b) {
      using Result = std::invoke_result_t<Compare&, const T&, const T&>;
      static_assert(std::is_arithmetic_v<Result> && !std::is_same_v<Result, bool>,
                    "an rt::sort comparator returns a number like a JS comparator (negative, zero, positive), not a bool");
      return static_cast<double>(compare(a, b));
    }

    // Stable merge sort, well defined (no out-of-bounds access, terminates) for any
    // comparator, including inconsistent or random ones. `before(a, b)` is true when b
    // must move ahead of a.
    template <class Container, class Before>
    void stableSort(Container& values, Before before) {
      using Value = typename Container::value_type;
      const std::size_t size = values.size();
      if (size < 2) return;
      constexpr std::size_t kRun = 8;
      for (std::size_t start = 0; start < size; start += kRun) {
        const std::size_t end = start + kRun < size ? start + kRun : size;
        for (std::size_t index = start + 1; index < end; ++index) {
          std::size_t position = index;
          while (position > start && before(values[position - 1], values[index])) --position;
          if (position == index) continue;
          Value moving = std::move(values[index]);
          for (std::size_t cursor = index; cursor > position; --cursor) values[cursor] = std::move(values[cursor - 1]);
          values[position] = std::move(moving);
        }
      }
      std::vector<Value> buffer;
      buffer.reserve(size);
      for (std::size_t width = kRun; width < size; width *= 2) {
        for (std::size_t low = 0; low < size; low += 2 * width) {
          const std::size_t middle = low + width < size ? low + width : size;
          const std::size_t high = low + 2 * width < size ? low + 2 * width : size;
          if (middle >= high) continue;
          buffer.clear();
          std::size_t left = low, right = middle;
          while (left < middle && right < high) {
            if (before(values[left], values[right])) buffer.push_back(std::move(values[right++]));
            else buffer.push_back(std::move(values[left++]));
          }
          while (left < middle) buffer.push_back(std::move(values[left++]));
          while (right < high) buffer.push_back(std::move(values[right++]));
          for (std::size_t index = 0; index < buffer.size(); ++index) values[low + index] = std::move(buffer[index]);
        }
      }
    }
  }

  // array.sort(comparator) / toSorted(comparator). The comparator returns a number:
  // negative puts a first, positive puts b first, 0 or NaN keeps their order (stable).
  // For any consistent comparator the result equals JavaScriptCore's.
  template <class Container, class Compare>
    requires requires(Container& c) { c.size(); c[0]; }
  Container& sort(Container& values, Compare compare) {
    detail::stableSort(values, [&compare](const auto& a, const auto& b) { return detail::sortCompare(compare, b, a) < 0; });
    return values;
  }
  template <class Container, class Compare>
    requires requires(Container& c) { c.size(); c[0]; }
  Container toSorted(Container values, Compare compare) {
    sort(values, compare);
    return values;
  }
  // array.sort() without a comparator: by String(element), in UTF-16 code unit order.
  std::vector<std::string>& sort(std::vector<std::string>& values);
  std::vector<double>& sort(std::vector<double>& values);  // [10, 9, 1].sort() is [1, 10, 9]
  json::array& sort(json::array& values);
  std::vector<std::string> toSorted(std::vector<std::string> values);
  std::vector<double> toSorted(std::vector<double> values);
  json::array toSorted(json::array values);

  // array.includes(x): SameValueZero (finds NaN; 0 and -0 match). Objects by identity.
  template <class T>
  bool includes(const std::vector<T>& values, const std::type_identity_t<T>& search) {
    for (const auto& value : values) {
      if constexpr (std::is_floating_point_v<T>) {
        if (sameValueZero(static_cast<double>(value), static_cast<double>(search))) return true;
      } else if (value == search) {
        return true;
      }
    }
    return false;
  }
  bool includes(const json::array& values, const json::value& search) noexcept;
  // array.indexOf(x): strict equality (NaN is never found); -1 when absent.
  template <class T>
  std::int64_t indexOf(const std::vector<T>& values, const std::type_identity_t<T>& search) {
    for (std::size_t index = 0; index < values.size(); ++index)
      if (values[index] == search) return static_cast<std::int64_t>(index);
    return -1;
  }
  std::int64_t indexOf(const json::array& values, const json::value& search) noexcept;
  // array.at(index): negative counts from the end; nullptr when out of range.
  template <class T>
  const T* at(const std::vector<T>& values, NumberArg index) {
    double relative = std::isnan(index.value) ? 0.0 : std::trunc(index.value);
    const double length = static_cast<double>(values.size());
    if (relative < 0) relative += length;
    if (relative < 0 || relative >= length) return nullptr;
    return &values[static_cast<std::size_t>(relative)];
  }
  template <class T>
  T* at(std::vector<T>& values, NumberArg index) {
    return const_cast<T*>(at(static_cast<const std::vector<T>&>(values), index));
  }
  const json::value* at(const json::array& values, NumberArg index) noexcept;
  // `array[index]` with a computed JS number index (a property read, not array.at): an
  // element only for a canonical array index below the length (-0 reads element 0); a
  // negative, fractional, NaN or infinite index, or one >= length, reads undefined (nullptr).
  template <class T>
  const T* element(const std::vector<T>& values, double index) noexcept {
    if (!(index >= 0) || index >= static_cast<double>(values.size()) || std::trunc(index) != index) return nullptr;
    return &values[static_cast<std::size_t>(index)];
  }
  template <class T>
  T* element(std::vector<T>& values, double index) noexcept {
    return const_cast<T*>(element(static_cast<const std::vector<T>&>(values), index));
  }
  inline const json::value* element(const json::array& values, double index) noexcept {
    if (!(index >= 0) || index >= static_cast<double>(values.size()) || std::trunc(index) != index) return nullptr;
    return &values[static_cast<std::size_t>(index)];
  }
  inline json::value* element(json::array& values, double index) noexcept {
    return const_cast<json::value*>(element(static_cast<const json::array&>(values), index));
  }
  // array.flat(depth = 1): nested arrays spliced in up to `depth` levels.
  json::array flat(const json::array& values, double depth = 1);
  template <class T>
  std::vector<T> flat(const std::vector<std::vector<T>>& values) {
    std::vector<T> out;
    for (const auto& inner : values) out.insert(out.end(), inner.begin(), inner.end());
    return out;
  }
}
