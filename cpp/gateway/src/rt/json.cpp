#include "uos/rt/json_document.hpp"

#include "uos/rt/error.hpp"

#include "json_simdjson.hpp"

#include <boost/json/serializer.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <memory>

namespace uos::rt {
  namespace {
    constexpr double kMaxSafeInteger = 9007199254740991.0;
    constexpr std::int64_t kMaxSafeInt64 = 9007199254740991;
    constexpr char kHexDigits[] = "0123456789abcdef";

    [[noreturn]] void throwStackOverflow() { throw RangeError("Maximum call stack size exceeded."); }

    std::string_view view(json::string_view text) noexcept { return {text.data(), text.size()}; }
    std::string_view view(const json::string& text) noexcept { return {text.data(), text.size()}; }

    double numberOf(const json::value& value) noexcept {
      switch (value.kind()) {
        case json::kind::int64:
          return static_cast<double>(value.get_int64());
        case json::kind::uint64:
          return static_cast<double>(value.get_uint64());
        case json::kind::double_:
          return value.get_double();
        default:
          return std::numeric_limits<double>::quiet_NaN();
      }
    }

    bool isCanonicalNumber(const json::value& value) noexcept {
      switch (value.kind()) {
        case json::kind::int64:
          return value.get_int64() >= -kMaxSafeInt64 && value.get_int64() <= kMaxSafeInt64;
        case json::kind::uint64:
          return false;
        case json::kind::double_: {
          const double number = value.get_double();
          return !(number >= -kMaxSafeInteger && number <= kMaxSafeInteger && std::trunc(number) == number && !(number == 0 && std::signbit(number)));
        }
        default:
          return true;
      }
    }

    void canonicalizeTop(json::value& value) {
      if (!isCanonicalNumber(value)) value = makeNumber(numberOf(value));
    }

    std::uint32_t indexValue(std::string_view key) noexcept {
      std::uint32_t value = 0;
      for (const char c : key) value = value * 10u + static_cast<std::uint32_t>(c - '0');
      return value;
    }

    // The members of an object in JS enumeration order.
    template <class Pair, class Object>
    std::vector<Pair*> orderedMembers(Object& object) {
      std::vector<Pair*> members;
      members.reserve(object.size());
      bool anyIndex = false;
      for (auto& member : object) {
        members.push_back(&member);
        if (!anyIndex && isArrayIndex(view(member.key()))) anyIndex = true;
      }
      if (anyIndex) {
        const auto split = std::stable_partition(members.begin(), members.end(), [](Pair* member) { return isArrayIndex(view(member->key())); });
        std::sort(members.begin(), split, [](Pair* a, Pair* b) { return indexValue(view(a->key())) < indexValue(view(b->key())); });
      }
      return members;
    }

    void defineMember(json::object& object, std::string_view key, json::value value) {
      canonicalizeTop(value);
      if (const auto found = object.find(key); found != object.end()) {
        found->value() = std::move(value);
        return;
      }
      if (!isArrayIndex(key)) {
        object.emplace(key, std::move(value));
        return;
      }
      const std::uint32_t index = indexValue(key);
      std::size_t position = 0;
      for (const auto& member : object) {
        const std::string_view existing = view(member.key());
        if (!isArrayIndex(existing) || indexValue(existing) > index) break;
        ++position;
      }
      if (position == object.size()) {
        object.emplace(key, std::move(value));
        return;
      }
      json::object rebuilt(object.storage());
      rebuilt.reserve(object.size() + 1);
      std::size_t cursor = 0;
      for (auto& member : object) {
        if (cursor++ == position) rebuilt.emplace(key, std::move(value));
        rebuilt.emplace(member.key(), std::move(member.value()));
      }
      object = std::move(rebuilt);
    }

    void appendNumber(std::string& out, const json::value& value) {
      if (value.is_int64() && value.get_int64() >= -kMaxSafeInt64 && value.get_int64() <= kMaxSafeInt64) {
        char buffer[24];
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, value.get_int64());
        out.append(buffer, result.ptr);
        return;
      }
      const double number = numberOf(value);
      if (!std::isfinite(number)) out += "null";
      else out += numberToString(number);
    }

    void appendUnicodeEscape(std::string& out, std::uint32_t unit) {
      out += "\\u";
      out += kHexDigits[(unit >> 12) & 0xF];
      out += kHexDigits[(unit >> 8) & 0xF];
      out += kHexDigits[(unit >> 4) & 0xF];
      out += kHexDigits[unit & 0xF];
    }

    void appendQuoted(std::string& out, json::serializer& writer, std::string_view text) {
      if (text.size() > limits::kTextInputBytes) throw RangeError("JSON string exceeds limit");
      if (simdjson::validate_utf8(text.data(), text.size())) {
        writer.reset(json::string_view(text.data(), text.size()));
        char buffer[4096];
        while (!writer.done()) out += view(writer.read(buffer));
        return;
      }

      // Runtime strings can hold lone UTF-16 code units. Escape their WTF-8
      // form so durable and wire JSON carries the original code unit.
      const std::string normalized = fromUtf16(toUtf16(text));
      const std::string quoted = json::serialize(json::string_view(normalized.data(), normalized.size()));
      std::size_t start = 0;
      for (std::size_t offset = 0; offset + 2 < quoted.size(); ++offset) {
        const auto first = static_cast<unsigned char>(quoted[offset]);
        const auto second = static_cast<unsigned char>(quoted[offset + 1]);
        if (first != 0xed || second < 0xa0 || second > 0xbf) continue;
        const auto third = static_cast<unsigned char>(quoted[offset + 2]);
        out.append(quoted, start, offset - start);
        appendUnicodeEscape(out, ((first & 0x0fu) << 12) | ((second & 0x3fu) << 6) | (third & 0x3fu));
        offset += 2;
        start = offset + 1;
      }
      out.append(quoted, start);
    }

    // Boost.JSON writes strings. The traversal handles indentation, the
    // replacer and numeric spellings used by the gateway's byte contracts.
    std::string writeJson(const json::value& root, std::string_view gap, const JsonReplacer& replacer = {}) {
      struct Frame {
        const json::value* value;
        std::size_t next = 0;
        bool wroteChild = false;
        std::unique_ptr<json::value> owned;
      };

      std::string out;
      json::serializer writer;
      std::vector<Frame> stack;
      const auto newline = [&](std::size_t depth) {
        if (gap.empty()) return;
        out += '\n';
        for (std::size_t level = 0; level < depth; ++level) out += gap;
      };
      const auto emit = [&](const json::value& value, std::unique_ptr<json::value> owned = {}) {
        if (value.is_array() || value.is_object()) {
          if (stack.size() >= kJsonMaxDepth) throwStackOverflow();
          out += value.is_array() ? '[' : '{';
          stack.push_back({&value, 0, false, std::move(owned)});
        } else if (value.is_string()) {
          appendQuoted(out, writer, view(value.get_string()));
        } else if (isNumber(value)) {
          appendNumber(out, value);
        } else {
          out += value.is_null() ? "null" : value.get_bool() ? "true" : "false";
        }
      };

      emit(root);
      while (!stack.empty()) {
        Frame& frame = stack.back();
        const json::object* object = frame.value->if_object();
        const json::array* array = frame.value->if_array();
        const std::size_t size = object ? object->size() : array->size();
        if (frame.next == size) {
          if (frame.wroteChild) newline(stack.size() - 1);
          out += object ? '}' : ']';
          stack.pop_back();
          continue;
        }

        const std::size_t index = frame.next++;
        const auto* member = object ? object->begin() + index : nullptr;
        const json::value& child = member ? member->value() : (*array)[index];
        std::unique_ptr<json::value> replacement;
        if (replacer) {
          auto value = replacer(member ? view(member->key()) : std::string_view(std::to_string(index)), child);
          if (!value && member) continue;
          replacement = std::make_unique<json::value>(value ? std::move(*value) : json::value(nullptr));
        }

        if (frame.wroteChild) out += ',';
        frame.wroteChild = true;
        newline(stack.size());
        if (member) {
          appendQuoted(out, writer, view(member->key()));
          out += gap.empty() ? ":" : ": ";
        }
        const json::value& selected = replacement ? *replacement : child;
        emit(selected, std::move(replacement));
      }
      return out;
    }

    std::string gapFromString(std::string_view gap) {
      if (isAscii(gap)) return std::string(gap.substr(0, 10));
      const std::u16string units = toUtf16(gap);
      return fromUtf16(std::u16string_view(units).substr(0, 10));
    }

    // `depth` counts the arrays and objects already entered; entering one more must stay
    // within kJsonMaxDepth.
    void checkDepth(std::size_t depth) {
      if (depth >= kJsonMaxDepth) throwStackOverflow();
    }

    void joinInto(std::string& out, const json::array& array, std::size_t depth);

    void stringInto(std::string& out, const json::value& value, std::size_t depth) {
      switch (value.kind()) {
        case json::kind::null:
          out += "null";
          return;
        case json::kind::bool_:
          out += value.get_bool() ? "true" : "false";
          return;
        case json::kind::int64:
        case json::kind::uint64:
        case json::kind::double_:
          out += numberToString(numberOf(value));
          return;
        case json::kind::string:
          append(out, view(value.get_string()));
          return;
        case json::kind::array:
          joinInto(out, value.get_array(), depth);
          return;
        case json::kind::object:
          out += "[object Object]";
          return;
      }
    }

    // Array.prototype.join(","): null and undefined elements print as "".
    void joinInto(std::string& out, const json::array& array, std::size_t depth) {
      checkDepth(depth);
      for (std::size_t index = 0; index < array.size(); ++index) {
        if (index) out += ',';
        if (!array[index].is_null()) stringInto(out, array[index], depth + 1);
      }
    }

    bool deepEqualAt(const json::value& a, const json::value& b, std::size_t depth) {
      if (isNumber(a) || isNumber(b)) return isNumber(a) && isNumber(b) && sameValue(numberOf(a), numberOf(b));
      if (a.kind() != b.kind()) return false;
      switch (a.kind()) {
        case json::kind::null:
          return true;
        case json::kind::bool_:
          return a.get_bool() == b.get_bool();
        case json::kind::string:
          return a.get_string() == b.get_string();
        case json::kind::array: {
          checkDepth(depth);
          const auto& left = a.get_array();
          const auto& right = b.get_array();
          if (left.size() != right.size()) return false;
          for (std::size_t index = 0; index < left.size(); ++index)
            if (!deepEqualAt(left[index], right[index], depth + 1)) return false;
          return true;
        }
        case json::kind::object: {
          checkDepth(depth);
          const auto& left = a.get_object();
          const auto& right = b.get_object();
          if (left.size() != right.size()) return false;
          for (const auto& member : left) {
            const auto found = right.find(member.key());
            if (found == right.end() || !deepEqualAt(member.value(), found->value(), depth + 1)) return false;
          }
          return true;
        }
        default:
          return false;
      }
    }

    bool matchesAt(const json::value& received, const json::value& expected, std::size_t depth) {
      if (expected.is_array() || expected.is_object()) checkDepth(depth);
      if (expected.is_array()) {
        if (!received.is_array() || received.get_array().size() != expected.get_array().size()) return false;
        for (std::size_t index = 0; index < expected.get_array().size(); ++index)
          if (!matchesAt(received.get_array()[index], expected.get_array()[index], depth + 1)) return false;
        return true;
      }
      if (expected.is_object()) {
        if (received.is_object()) {
          for (const auto& member : expected.get_object()) {
            const auto found = received.get_object().find(member.key());
            if (found == received.get_object().end() || !matchesAt(found->value(), member.value(), depth + 1)) return false;
          }
          return true;
        }
        if (received.is_array()) {
          const auto& elements = received.get_array();
          for (const auto& member : expected.get_object()) {
            const std::string_view key = view(member.key());
            if (key == "length") {
              if (!isNumber(member.value()) || numberOf(member.value()) != static_cast<double>(elements.size())) return false;
            } else if (!isArrayIndex(key) || indexValue(key) >= elements.size() || !matchesAt(elements[indexValue(key)], member.value(), depth + 1)) {
              return false;
            }
          }
          return true;
        }
        return false;
      }
      return deepEqualAt(received, expected, depth);
    }

    void canonicalizeAt(json::value& value, std::size_t depth) {
      if (value.is_array()) {
        checkDepth(depth);
        for (auto& element : value.get_array()) canonicalizeAt(element, depth + 1);
      } else if (value.is_object()) {
        checkDepth(depth);
        for (auto& member : value.get_object()) canonicalizeAt(member.value(), depth + 1);
      } else {
        canonicalizeTop(value);
      }
    }

    void flatInto(json::array& out, const json::array& values, double depth, std::size_t level) {
      checkDepth(level);
      for (const auto& value : values) {
        if (depth > 0 && value.is_array()) flatInto(out, value.get_array(), depth - 1, level + 1);
        else out.push_back(value);
      }
    }

    template <class Keyed>
    void sortByKey(std::vector<Keyed>& keyed) {
      detail::stableSort(keyed, [](const Keyed& a, const Keyed& b) { return compare(b.first, a.first) < 0; });
    }
  }

  json::value makeNumber(double value) {
    if (value >= -kMaxSafeInteger && value <= kMaxSafeInteger && std::trunc(value) == value && !(value == 0 && std::signbit(value)))
      return json::value(static_cast<std::int64_t>(value));
    return json::value(value);
  }

  json::value makeNumber(std::int64_t value) {
    if (value >= -kMaxSafeInt64 && value <= kMaxSafeInt64) return json::value(value);
    return json::value(static_cast<double>(value));
  }

  json::value canonicalNumbers(json::value value) {
    canonicalizeAt(value, 0);
    return value;
  }

  bool isNumber(const json::value& value) noexcept { return value.is_int64() || value.is_uint64() || value.is_double(); }

  std::string_view typeOf(const json::value& value) noexcept {
    switch (value.kind()) {
      case json::kind::bool_:
        return "boolean";
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        return "number";
      case json::kind::string:
        return "string";
      default:
        return "object";
    }
  }

  std::string_view typeOf(const json::value* value) noexcept { return value ? typeOf(*value) : "undefined"; }

  bool isRecord(const json::value& value) noexcept { return value.is_object(); }
  bool isRecord(const json::value* value) noexcept { return value && value->is_object(); }
  bool isNullish(const json::value* value) noexcept { return !value || value->is_null(); }

  bool truthy(const json::value& value) noexcept {
    switch (value.kind()) {
      case json::kind::null:
        return false;
      case json::kind::bool_:
        return value.get_bool();
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        return truthy(numberOf(value));
      case json::kind::string:
        return !value.get_string().empty();
      default:
        return true;
    }
  }

  bool truthy(const json::value* value) noexcept { return value && truthy(*value); }
  bool truthy(double value) noexcept { return !(value == 0 || std::isnan(value)); }
  bool truthy(std::string_view value) noexcept { return !value.empty(); }

  double toNumber(const json::value& value) {
    switch (value.kind()) {
      case json::kind::null:
        return 0;
      case json::kind::bool_:
        return value.get_bool() ? 1 : 0;
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        return numberOf(value);
      case json::kind::string:
        return stringToNumber(view(value.get_string()));
      default:
        return stringToNumber(toString(value));
    }
  }

  double toNumber(const json::value* value) { return value ? toNumber(*value) : std::numeric_limits<double>::quiet_NaN(); }

  std::string toString(const json::value& value) {
    std::string out;
    stringInto(out, value, 0);
    return out;
  }

  std::string toString(const json::value* value) { return value ? toString(*value) : "undefined"; }

  namespace Number {
    bool isFinite(const json::value& value) noexcept { return isNumber(value) && std::isfinite(numberOf(value)); }
    bool isNaN(const json::value& value) noexcept { return isNumber(value) && std::isnan(numberOf(value)); }
    bool isInteger(const json::value& value) noexcept { return isNumber(value) && isInteger(numberOf(value)); }
    bool isSafeInteger(const json::value& value) noexcept { return isNumber(value) && isSafeInteger(numberOf(value)); }
  }

  bool isNaN(const json::value& value) { return std::isnan(toNumber(value)); }
  bool isFinite(const json::value& value) { return std::isfinite(toNumber(value)); }

  bool sameValue(double a, double b) noexcept {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return a == b && std::signbit(a) == std::signbit(b);
  }

  bool sameValueZero(double a, double b) noexcept { return a == b || (std::isnan(a) && std::isnan(b)); }

  bool strictEquals(const json::value& a, const json::value& b) noexcept {
    if (isNumber(a) || isNumber(b)) return isNumber(a) && isNumber(b) && numberOf(a) == numberOf(b);
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
      case json::kind::null:
        return true;
      case json::kind::bool_:
        return a.get_bool() == b.get_bool();
      case json::kind::string:
        return a.get_string() == b.get_string();
      default:
        return &a == &b;
    }
  }

  bool deepEqual(const json::value& a, const json::value& b) { return deepEqualAt(a, b, 0); }
  bool matchesObject(const json::value& received, const json::value& expected) {
    if (!(received.is_object() || received.is_array()) || !(expected.is_object() || expected.is_array())) return false;
    return matchesAt(received, expected, 0);
  }
  json::value structuredClone(const json::value& value) { return value; }

  bool isArrayIndex(std::string_view key) noexcept {
    if (key.empty() || key.size() > 10) return false;
    if (key[0] == '0') return key.size() == 1;
    std::uint64_t value = 0;
    for (const char c : key) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value <= 4294967294u;
  }

  const json::value* get(const json::object& object, std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->value();
  }

  json::value* get(json::object& object, std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->value();
  }

  const json::value* get(const json::value& value, std::string_view key) noexcept { return value.is_object() ? get(value.get_object(), key) : nullptr; }
  const json::value* get(const json::value* value, std::string_view key) noexcept { return value ? get(*value, key) : nullptr; }
  const json::value* get(const json::array& array, std::size_t index) noexcept { return index < array.size() ? &array[index] : nullptr; }
  bool hasOwn(const json::object& object, std::string_view key) noexcept { return object.find(key) != object.end(); }

  std::optional<std::string_view> getString(const json::value& value) noexcept {
    if (!value.is_string()) return std::nullopt;
    return view(value.get_string());
  }
  std::optional<double> getNumber(const json::value& value) noexcept {
    if (!isNumber(value)) return std::nullopt;
    return numberOf(value);
  }
  std::optional<bool> getBool(const json::value& value) noexcept {
    if (!value.is_bool()) return std::nullopt;
    return value.get_bool();
  }
  const json::object* getObject(const json::value& value) noexcept { return value.if_object(); }
  const json::array* getArray(const json::value& value) noexcept { return value.if_array(); }

  std::optional<std::string_view> getString(const json::value* value) noexcept { return value ? getString(*value) : std::nullopt; }
  std::optional<double> getNumber(const json::value* value) noexcept { return value ? getNumber(*value) : std::nullopt; }
  std::optional<bool> getBool(const json::value* value) noexcept { return value ? getBool(*value) : std::nullopt; }
  const json::object* getObject(const json::value* value) noexcept { return value ? value->if_object() : nullptr; }
  const json::array* getArray(const json::value* value) noexcept { return value ? value->if_array() : nullptr; }

  std::optional<std::string_view> getString(const json::object& object, std::string_view key) noexcept { return getString(get(object, key)); }
  std::optional<double> getNumber(const json::object& object, std::string_view key) noexcept { return getNumber(get(object, key)); }
  std::optional<bool> getBool(const json::object& object, std::string_view key) noexcept { return getBool(get(object, key)); }
  const json::object* getObject(const json::object& object, std::string_view key) noexcept { return getObject(get(object, key)); }
  const json::array* getArray(const json::object& object, std::string_view key) noexcept { return getArray(get(object, key)); }
  json::object* getObject(json::object& object, std::string_view key) noexcept {
    json::value* member = get(object, key);
    return member ? member->if_object() : nullptr;
  }
  json::array* getArray(json::object& object, std::string_view key) noexcept {
    json::value* member = get(object, key);
    return member ? member->if_array() : nullptr;
  }

  std::optional<std::string_view> getString(const json::value& value, std::string_view key) noexcept { return getString(get(value, key)); }
  std::optional<double> getNumber(const json::value& value, std::string_view key) noexcept { return getNumber(get(value, key)); }
  std::optional<bool> getBool(const json::value& value, std::string_view key) noexcept { return getBool(get(value, key)); }
  const json::object* getObject(const json::value& value, std::string_view key) noexcept { return getObject(get(value, key)); }
  const json::array* getArray(const json::value& value, std::string_view key) noexcept { return getArray(get(value, key)); }

  std::vector<std::string> objectKeys(const json::object& object) {
    std::vector<std::string> keys;
    keys.reserve(object.size());
    for (const auto* member : orderedMembers<const json::key_value_pair>(object)) keys.emplace_back(view(member->key()));
    return keys;
  }

  std::vector<const json::value*> objectValues(const json::object& object) {
    std::vector<const json::value*> values;
    values.reserve(object.size());
    for (const auto* member : orderedMembers<const json::key_value_pair>(object)) values.push_back(&member->value());
    return values;
  }

  std::vector<const json::key_value_pair*> objectEntries(const json::object& object) { return orderedMembers<const json::key_value_pair>(object); }
  std::vector<json::key_value_pair*> objectEntries(json::object& object) { return orderedMembers<json::key_value_pair>(object); }

  void objectSet(json::object& object, std::string_view key, json::value value) {
    if (key == "__proto__" && object.find(key) == object.end()) return;
    defineMember(object, key, std::move(value));
  }

  void objectDefine(json::object& object, std::string_view key, json::value value) { defineMember(object, key, std::move(value)); }

  bool objectDelete(json::object& object, std::string_view key) noexcept { return object.stable_erase(key) != 0; }

  void objectAssign(json::object& target, const json::object& source) {
    if (&target == &source) return;
    // The source may live inside the target, which objectSet can rebuild: copy it first.
    std::vector<std::pair<std::string, json::value>> members;
    members.reserve(source.size());
    for (const auto* member : orderedMembers<const json::key_value_pair>(source)) members.emplace_back(view(member->key()), member->value());
    for (auto& [key, value] : members) objectSet(target, key, std::move(value));
  }

  json::object objectSpread(std::initializer_list<const json::object*> sources) {
    json::object out;
    for (const json::object* source : sources)
      for (const auto* member : orderedMembers<const json::key_value_pair>(*source)) defineMember(out, view(member->key()), member->value());
    return out;
  }

  json::object objectFromEntries(const std::vector<std::pair<std::string, json::value>>& entries) {
    json::object out;
    for (const auto& [key, value] : entries) defineMember(out, key, value);
    return out;
  }

  std::string jsonStringify(const json::value& value) { return writeJson(value, {}); }

  std::string jsonStringify(const json::value& value, int space) {
    return writeJson(value, std::string(static_cast<std::size_t>(std::clamp(space, 0, 10)), ' '));
  }

  std::string jsonStringify(const json::value& value, std::string_view gap) { return writeJson(value, gapFromString(gap)); }

  std::optional<std::string> jsonStringify(const json::value& value, const JsonReplacer& replacer, std::string_view gap) {
    if (!replacer) return writeJson(value, gapFromString(gap));
    auto root = replacer("", value);
    if (!root) return std::nullopt;
    return writeJson(*root, gapFromString(gap), replacer);
  }

  std::string jsonQuote(std::string_view text) {
    std::string out;
    json::serializer writer;
    appendQuoted(out, writer, text);
    return out;
  }

  json::value jsonParse(std::string_view text) { return jsonParseWithLimit(text, limits::kJsonDocumentBytes); }

  json::value jsonParseWithLimit(std::string_view text, std::size_t maxDocumentBytes) { return detail::parseSimdJson(text, maxDocumentBytes); }

  json::value jsonParseBody(std::string_view text, bool emptyBody) {
    if (emptyBody) throw SyntaxError("Unexpected end of JSON input");
    try {
      return jsonParse(text);
    } catch (const SyntaxError&) {
      throw SyntaxError("Failed to parse JSON");
    }
  }

  std::vector<std::string>& sort(std::vector<std::string>& values) {
    detail::stableSort(values, [](const std::string& a, const std::string& b) { return compare(b, a) < 0; });
    return values;
  }

  std::vector<double>& sort(std::vector<double>& values) {
    std::vector<std::pair<std::string, double>> keyed;
    keyed.reserve(values.size());
    for (const double value : values) keyed.emplace_back(numberToString(value), value);
    sortByKey(keyed);
    for (std::size_t index = 0; index < values.size(); ++index) values[index] = keyed[index].second;
    return values;
  }

  json::array& sort(json::array& values) {
    std::vector<std::pair<std::string, json::value>> keyed;
    keyed.reserve(values.size());
    for (auto& value : values) {
      std::string key = toString(value);
      keyed.emplace_back(std::move(key), std::move(value));
    }
    sortByKey(keyed);
    for (std::size_t index = 0; index < values.size(); ++index) values[index] = std::move(keyed[index].second);
    return values;
  }

  std::vector<std::string> toSorted(std::vector<std::string> values) {
    sort(values);
    return values;
  }

  std::vector<double> toSorted(std::vector<double> values) {
    sort(values);
    return values;
  }

  json::array toSorted(json::array values) {
    sort(values);
    return values;
  }

  bool includes(const json::array& values, const json::value& search) noexcept {
    for (const auto& value : values) {
      if (isNumber(value) && isNumber(search) ? sameValueZero(numberOf(value), numberOf(search)) : strictEquals(value, search)) return true;
    }
    return false;
  }

  std::int64_t indexOf(const json::array& values, const json::value& search) noexcept {
    for (std::size_t index = 0; index < values.size(); ++index)
      if (strictEquals(values[index], search)) return static_cast<std::int64_t>(index);
    return -1;
  }

  const json::value* at(const json::array& values, NumberArg index) noexcept {
    double relative = std::isnan(index.value) ? 0.0 : std::trunc(index.value);
    const double length = static_cast<double>(values.size());
    if (relative < 0) relative += length;
    if (relative < 0 || relative >= length) return nullptr;
    return &values[static_cast<std::size_t>(relative)];
  }

  json::array flat(const json::array& values, double depth) {
    json::array out;
    flatInto(out, values, std::isnan(depth) ? 0.0 : std::trunc(depth), 0);
    return out;
  }
}
