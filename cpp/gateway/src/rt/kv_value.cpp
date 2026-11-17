// The stored value model: KvU64, objects/maps/sets, JSON interop, structured
// clone as a KV round trip performs it, and assertPortableKvValue.
#include "kv_internal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace uos::rt {
  namespace {
    bool sameNumber(double left, double right) {
      if (std::isnan(left) || std::isnan(right)) return std::isnan(left) && std::isnan(right);
      return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
    }

    // A canonical array index: "0" or a decimal without leading zeros, at most 2^32 - 2.
    std::optional<std::uint32_t> arrayIndex(std::string_view key) {
      if (key.empty() || key.size() > 10) return std::nullopt;
      if (key.size() > 1 && key[0] == '0') return std::nullopt;
      std::uint64_t value = 0;
      for (const char c : key) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
      }
      if (value > 4294967294ull) return std::nullopt;
      return static_cast<std::uint32_t>(value);
    }

    // Primitive values compare as Map keys by SameValueZero; containers are identities.
    bool isPrimitive(const KvValue& value) {
      return value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt();
    }

    bool sameValueZero(const KvValue& left, const KvValue& right) {
      if (left.variant().index() != right.variant().index()) return false;
      if (left.isNumber()) {
        const double a = left.asNumber(), b = right.asNumber();
        return (std::isnan(a) && std::isnan(b)) || a == b;
      }
      return left == right;
    }

    KvValue normalizedKey(KvValue value) {
      if (value.isNumber() && value.asNumber() == 0) return KvValue(0.0);
      return value;
    }

    // Order-insensitive multiset match, as deep equality treats Map entries and Set members.
    template <class Range, class Same> bool sameMembers(const Range& left, const Range& right, Same same) {
      if (left.size() != right.size()) return false;
      std::vector<bool> used(right.size(), false);
      for (const auto& item : left) {
        bool matched = false;
        std::size_t index = 0;
        for (const auto& candidate : right) {
          if (!used[index] && same(item, candidate)) {
            used[index] = true;
            matched = true;
            break;
          }
          ++index;
        }
        if (!matched) return false;
      }
      return true;
    }

    const KvValue& undefinedValue() {
      static const KvValue value;
      return value;
    }

    bool isStandardErrorName(std::string_view name) {
      return name == "Error" || name == "EvalError" || name == "RangeError" || name == "ReferenceError" || name == "SyntaxError" || name == "TypeError" ||
             name == "URIError";
    }

    std::string canonicalRegExpFlags(std::string_view flags) {
      std::string out;
      for (const char flag : std::string_view("dgimsuvy"))
        if (flags.find(flag) != std::string_view::npos) out += flag;
      return out;
    }

    json::value numberToJson(double number) {
      if (std::isfinite(number) && number == std::trunc(number) && std::fabs(number) < 1e21 && !(number == 0 && std::signbit(number))) {
        if (number >= -9223372036854775808.0 && number < 9223372036854775808.0) return json::value(static_cast<std::int64_t>(number));
        if (number >= 0 && number < 18446744073709551616.0) return json::value(static_cast<std::uint64_t>(number));
      }
      return json::value(number);
    }

    json::value toJsonValue(const KvValue& value);

    json::value toJsonMember(const KvValue& value) {
      if (value.isUndefined() || value.is<KvHole>()) return nullptr;
      return toJsonValue(value);
    }

    json::value toJsonValue(const KvValue& value) {
      return std::visit(
        [&](const auto& item) -> json::value {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, KvUndefined> || std::is_same_v<T, std::nullptr_t> || std::is_same_v<T, KvHole>) return nullptr;
          else if constexpr (std::is_same_v<T, bool>) return item;
          else if constexpr (std::is_same_v<T, double>) return numberToJson(item);
          else if constexpr (std::is_same_v<T, std::string>) return json::string(item);
          else if constexpr (std::is_same_v<T, KvArray>) {
            json::array out;
            out.reserve(item.size());
            for (const KvValue& element : item) out.push_back(toJsonMember(element));
            return out;
          } else if constexpr (std::is_same_v<T, KvObject>) {
            json::object out;
            for (const auto& [key, member] : item)
              if (!member.isUndefined() && !member.template is<KvHole>()) out[key] = toJsonValue(member);
            return out;
          } else {
            throw TypeError("KV value is " + std::string(value.kind()) + ", not JSON");
          }
        },
        value.variant());
    }

    std::string quoted(std::string_view text) { return json::serialize(json::value(json::string(text))); }

    std::string showNumber(double number) { return number == 0 && std::signbit(number) ? "-0" : numberToString(number); }

    std::string showBytes(std::span<const std::uint8_t> bytes) {
      std::string out = "[";
      for (std::size_t index = 0; index < bytes.size(); ++index) out += (index ? ", " : "") + std::to_string(bytes[index]);
      return out + "]";
    }

  }

  KvValue kvStructuredClone(const KvValue& value) {
    return std::visit(
      [&](const auto& item) -> KvValue {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvU64>) return KvObject{{"value", item.toBigInt()}};
        else if constexpr (std::is_same_v<T, KvArrayBufferView>) {
          if (item.kind == KvViewKind::Uint8Array && item.byteOffset == 0 && item.byteLength == item.buffer.size()) return KvBytes(item.buffer);
          return item;
        } else if constexpr (std::is_same_v<T, KvError>) {
          KvError copy = item;
          if (!isStandardErrorName(copy.name)) copy.name = "Error";
          copy.cause = nullptr;
          return copy;
        } else if constexpr (std::is_same_v<T, KvRegExp>) return KvRegExp{item.source, canonicalRegExpFlags(item.flags)};
        else if constexpr (std::is_same_v<T, double>) return std::isnan(item) ? std::numeric_limits<double>::quiet_NaN() : item;
        else if constexpr (std::is_same_v<T, KvArray>) {
          KvArray out;
          out.reserve(item.size());
          for (const KvValue& element : item) out.push_back(kvStructuredClone(element));
          return out;
        } else if constexpr (std::is_same_v<T, KvObject>) {
          KvObject out;
          for (const auto& [key, member] : item) out.set(key, kvStructuredClone(member));
          return out;
        } else if constexpr (std::is_same_v<T, KvMap>) {
          KvMap out;
          for (const auto& [key, member] : item) out.set(kvStructuredClone(key), kvStructuredClone(member));
          return out;
        } else if constexpr (std::is_same_v<T, KvSet>) {
          KvSet out;
          for (const KvValue& member : item) out.add(kvStructuredClone(member));
          return out;
        } else {
          return item;
        }
      },
      value.variant());
  }

  // ---- KvU64 ------------------------------------------------------------------------------

  KvU64::KvU64(const BigInt& n) {
    if (n.isNegative()) throw JsError("value must be a positive bigint");
    if (n > BigInt::fromUint64(std::numeric_limits<std::uint64_t>::max())) throw JsError("value must fit in a 64-bit unsigned integer");
    value = n.toUint64();
  }

  BigInt KvU64::toBigInt() const { return BigInt::fromUint64(value); }

  // ---- KvObject / KvMap / KvSet -------------------------------------------------------------

  KvObject::KvObject(std::initializer_list<Entry> entries) {
    for (const Entry& entry : entries) set(entry.first, entry.second);
  }

  void KvObject::set(std::string key, KvValue value) {
    if (KvValue* existing = find(key)) {
      *existing = std::move(value);
      return;
    }
    if (const auto index = arrayIndex(key)) {
      const auto position = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        const auto other = arrayIndex(entry.first);
        return !other || *other > *index;
      });
      entries_.emplace(position, std::move(key), std::move(value));
      return;
    }
    entries_.emplace_back(std::move(key), std::move(value));
  }

  bool KvObject::contains(std::string_view key) const noexcept { return find(key) != nullptr; }

  const KvValue* KvObject::find(std::string_view key) const noexcept {
    for (const Entry& entry : entries_)
      if (entry.first == key) return &entry.second;
    return nullptr;
  }

  KvValue* KvObject::find(std::string_view key) noexcept {
    for (Entry& entry : entries_)
      if (entry.first == key) return &entry.second;
    return nullptr;
  }

  const KvValue& KvObject::at(std::string_view key) const noexcept {
    const KvValue* found = find(key);
    return found ? *found : undefinedValue();
  }

  bool KvObject::erase(std::string_view key) {
    const auto position = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.first == key; });
    if (position == entries_.end()) return false;
    entries_.erase(position);
    return true;
  }

  std::size_t KvObject::size() const noexcept { return entries_.size(); }
  bool KvObject::empty() const noexcept { return entries_.empty(); }
  KvObject::const_iterator KvObject::begin() const noexcept { return entries_.begin(); }
  KvObject::const_iterator KvObject::end() const noexcept { return entries_.end(); }
  KvObject::iterator KvObject::begin() noexcept { return entries_.begin(); }
  KvObject::iterator KvObject::end() noexcept { return entries_.end(); }

  bool operator==(const KvObject& left, const KvObject& right) {
    if (left.size() != right.size()) return false;
    for (const auto& [key, value] : left) {
      const KvValue* other = right.find(key);
      if (!other || !(*other == value)) return false;
    }
    return true;
  }

  KvMap::KvMap(std::initializer_list<Entry> entries) {
    for (const Entry& entry : entries) set(entry.first, entry.second);
  }

  void KvMap::set(KvValue key, KvValue value) {
    key = normalizedKey(std::move(key));
    if (isPrimitive(key))
      for (Entry& entry : entries_)
        if (sameValueZero(entry.first, key)) {
          entry.second = std::move(value);
          return;
        }
    entries_.emplace_back(std::move(key), std::move(value));
  }

  std::size_t KvMap::size() const noexcept { return entries_.size(); }
  bool KvMap::empty() const noexcept { return entries_.empty(); }
  KvMap::const_iterator KvMap::begin() const noexcept { return entries_.begin(); }
  KvMap::const_iterator KvMap::end() const noexcept { return entries_.end(); }

  bool operator==(const KvMap& left, const KvMap& right) {
    return sameMembers(left, right, [](const KvMap::Entry& a, const KvMap::Entry& b) { return a.first == b.first && a.second == b.second; });
  }

  KvSet::KvSet(std::initializer_list<KvValue> values) {
    for (const KvValue& value : values) add(value);
  }

  void KvSet::add(KvValue value) {
    value = normalizedKey(std::move(value));
    if (isPrimitive(value))
      for (const KvValue& member : values_)
        if (sameValueZero(member, value)) return;
    values_.push_back(std::move(value));
  }

  std::size_t KvSet::size() const noexcept { return values_.size(); }
  bool KvSet::empty() const noexcept { return values_.empty(); }
  KvSet::const_iterator KvSet::begin() const noexcept { return values_.begin(); }
  KvSet::const_iterator KvSet::end() const noexcept { return values_.end(); }

  bool operator==(const KvSet& left, const KvSet& right) {
    return sameMembers(left, right, [](const KvValue& a, const KvValue& b) { return a == b; });
  }

  bool operator==(const KvDate& left, const KvDate& right) noexcept { return sameNumber(left.time, right.time); }

  bool operator==(const KvError& left, const KvError& right) {
    if (left.name != right.name || left.message != right.message || left.line != right.line || left.column != right.column || left.sourceURL != right.sourceURL ||
        left.stack != right.stack)
      return false;
    if (!left.cause || !right.cause) return !left.cause && !right.cause;
    return *left.cause == *right.cause;
  }

  std::string_view kvViewKindName(KvViewKind kind) noexcept {
    static constexpr std::string_view names[] = {"DataView",   "Int8Array",    "Uint8Array",   "Uint8ClampedArray", "Int16Array",    "Uint16Array",
                                                 "Int32Array", "Uint32Array", "Float32Array", "Float64Array",      "BigInt64Array", "BigUint64Array"};
    return names[static_cast<std::size_t>(kind)];
  }

  // ---- KvValue ------------------------------------------------------------------------------

  KvValue::KvValue(const json::value& value) {
    switch (value.kind()) {
      case json::kind::null: value_ = nullptr; break;
      case json::kind::bool_: value_ = value.get_bool(); break;
      case json::kind::int64: value_ = static_cast<double>(value.get_int64()); break;
      case json::kind::uint64: value_ = static_cast<double>(value.get_uint64()); break;
      case json::kind::double_: value_ = value.get_double(); break;
      case json::kind::string: value_ = std::string(value.get_string()); break;
      case json::kind::array: value_ = KvValue(value.get_array()).value_; break;
      case json::kind::object: value_ = KvValue(value.get_object()).value_; break;
    }
  }

  KvValue::KvValue(const json::object& object) {
    KvObject out;
    for (const auto& member : object) out.set(std::string(member.key()), KvValue(member.value()));
    value_ = std::move(out);
  }

  KvValue::KvValue(const json::array& array) {
    KvArray out;
    out.reserve(array.size());
    for (const json::value& element : array) out.emplace_back(element);
    value_ = std::move(out);
  }

  KvValue KvValue::array(std::initializer_list<KvValue> items) { return KvArray(items); }

  KvValue KvValue::object(std::initializer_list<KvObject::Entry> entries) { return KvObject(entries); }

  void KvValue::throwMismatch(std::string_view expected) const {
    throw TypeError("KV value is " + std::string(kind()) + ", not " + std::string(expected));
  }

  const KvValue& KvValue::operator[](std::string_view key) const noexcept {
    if (const auto* object = std::get_if<KvObject>(&value_)) return object->at(key);
    return undefinedValue();
  }

  const KvValue& KvValue::operator[](std::size_t index) const noexcept {
    if (const auto* array = std::get_if<KvArray>(&value_))
      if (index < array->size() && !(*array)[index].is<KvHole>()) return (*array)[index];
    return undefinedValue();
  }

  bool KvValue::truthy() const noexcept {
    return std::visit(
      [](const auto& item) -> bool {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvUndefined> || std::is_same_v<T, std::nullptr_t> || std::is_same_v<T, KvHole>) return false;
        else if constexpr (std::is_same_v<T, bool>) return item;
        else if constexpr (std::is_same_v<T, double>) return !(item == 0 || std::isnan(item));
        else if constexpr (std::is_same_v<T, std::string>) return !item.empty();
        else if constexpr (std::is_same_v<T, BigInt>) return !item.isZero();
        else return true;
      },
      value_);
  }

  std::string_view KvValue::kind() const noexcept {
    return std::visit(
      [](const auto& item) -> std::string_view {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvArrayBufferView>) return kvViewKindName(item.kind);
        else return kindName<T>();
      },
      value_);
  }

  json::value KvValue::toJson() const { return toJsonValue(*this); }

  bool operator==(const KvValue& left, const KvValue& right) {
    if (left.value_.index() != right.value_.index()) return false;
    return std::visit(
      [&](const auto& item) -> bool {
        using T = std::decay_t<decltype(item)>;
        const T& other = std::get<T>(right.value_);
        if constexpr (std::is_same_v<T, double>) return sameNumber(item, other);
        else return item == other;
      },
      left.value_);
  }

  std::string show(const KvValue& value) {
    return std::visit(
      [&](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvUndefined>) return "undefined";
        else if constexpr (std::is_same_v<T, std::nullptr_t>) return "null";
        else if constexpr (std::is_same_v<T, KvHole>) return "<hole>";
        else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<T, double>) return showNumber(item);
        else if constexpr (std::is_same_v<T, std::string>) return quoted(item);
        else if constexpr (std::is_same_v<T, BigInt>) return item.toString() + "n";
        else if constexpr (std::is_same_v<T, KvU64>) return show(item);
        else if constexpr (std::is_same_v<T, KvBytes>) return "Uint8Array(" + std::to_string(item.size()) + ") " + showBytes(item);
        else if constexpr (std::is_same_v<T, KvArrayBuffer>) return "ArrayBuffer(" + std::to_string(item.bytes.size()) + ") " + showBytes(item.bytes);
        else if constexpr (std::is_same_v<T, KvArrayBufferView>)
          return std::string(kvViewKindName(item.kind)) + "(offset " + std::to_string(item.byteOffset) + ", length " + std::to_string(item.byteLength) + ") " +
                 showBytes(item.buffer);
        else if constexpr (std::is_same_v<T, KvDate>) return "Date(" + showNumber(item.time) + ")";
        else if constexpr (std::is_same_v<T, KvRegExp>) return "/" + item.source + "/" + item.flags;
        else if constexpr (std::is_same_v<T, KvError>)
          return item.name + "(" + (item.message ? quoted(*item.message) : std::string("undefined")) + (item.cause ? ", { cause: " + show(*item.cause) + " }" : "") + ")";
        else if constexpr (std::is_same_v<T, KvArray>) {
          std::string out = "[";
          for (std::size_t index = 0; index < item.size(); ++index) out += (index ? ", " : "") + show(item[index]);
          return out + "]";
        } else if constexpr (std::is_same_v<T, KvObject>) {
          if (item.empty()) return "{}";
          std::string out = "{ ";
          bool first = true;
          for (const auto& [key, member] : item) {
            out += (first ? "" : ", ") + quoted(key) + ": " + show(member);
            first = false;
          }
          return out + " }";
        } else if constexpr (std::is_same_v<T, KvMap>) {
          std::string out = "Map(" + std::to_string(item.size()) + ") {";
          bool first = true;
          for (const auto& [key, member] : item) {
            out += (first ? " " : ", ") + show(key) + " => " + show(member);
            first = false;
          }
          return out + (item.empty() ? "}" : " }");
        } else {
          std::string out = "Set(" + std::to_string(item.size()) + ") {";
          bool first = true;
          for (const KvValue& member : item) {
            out += (first ? " " : ", ") + show(member);
            first = false;
          }
          return out + (item.empty() ? "}" : " }");
        }
      },
      value.variant());
  }

  std::string show(const KvKeyPart& part) {
    switch (part.value.index()) {
      case 0: return "Uint8Array " + showBytes(part.bytes());
      case 1: return quoted(part.string());
      case 2: return showNumber(part.number());
      case 3: return part.bigint().toString() + "n";
      default: return part.boolean() ? "true" : "false";
    }
  }

  std::string show(const KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) out += (index ? ", " : "") + show(key[index]);
    return out + "]";
  }

  std::string show(KvU64 value) { return "KvU64(" + std::to_string(value.value) + "n)"; }

  std::string show(const KvCommitResult& result) {
    return result.ok ? "{ ok: true, versionstamp: " + quoted(result.versionstamp) + " }" : (result.versionstamp.empty() ? "{ ok: false }" : "{ ok: false, versionstamp: " + quoted(result.versionstamp) + " }");
  }

  std::string_view toString(KvMutationType type) noexcept {
    switch (type) {
      case KvMutationType::set: return "set";
      case KvMutationType::delete_: return "delete";
      case KvMutationType::sum: return "sum";
      case KvMutationType::min: return "min";
      default: return "max";
    }
  }

  void assertPortableKvValue(const KvValue& value) {
    std::vector<const KvValue*> pending{&value};
    while (!pending.empty()) {
      const KvValue* current = pending.back();
      pending.pop_back();
      if (const auto* error = std::get_if<KvError>(&current->variant())) {
        if (error->cause) throw TypeError("Bun KV cannot preserve Error.cause; persist an explicit error record instead");
      } else if (const auto* map = std::get_if<KvMap>(&current->variant())) {
        for (const auto& [key, member] : *map) {
          pending.push_back(&key);
          pending.push_back(&member);
        }
      } else if (const auto* set = std::get_if<KvSet>(&current->variant())) {
        for (const KvValue& member : *set) pending.push_back(&member);
      } else if (const auto* array = std::get_if<KvArray>(&current->variant())) {
        for (const KvValue& element : *array) pending.push_back(&element);
      } else if (const auto* object = std::get_if<KvObject>(&current->variant())) {
        for (const auto& [key, member] : *object) pending.push_back(&member);
      }
    }
  }

  KvValue kvStoredValue(const KvValue& value) {
    if (value.isU64() || value.isBytes()) return value;
    if (const auto* view = std::get_if<KvArrayBufferView>(&value.variant()); view && view->kind == KvViewKind::Uint8Array) {
      if (view->byteOffset > view->buffer.size() || view->byteLength > view->buffer.size() - view->byteOffset)
        throw RangeError("KV value has an ArrayBuffer view outside its buffer");
      const auto begin = view->buffer.begin() + static_cast<std::ptrdiff_t>(view->byteOffset);
      return KvBytes(begin, begin + static_cast<std::ptrdiff_t>(view->byteLength));
    }
    return kvStructuredClone(value);
  }
}
