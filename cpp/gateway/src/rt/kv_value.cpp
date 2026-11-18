// The stored value model: KvU64, objects/maps/sets, JSON interop, structured
// clone as a KV round trip performs it, and assertPortableKvValue.
#include "kv_internal.hpp"
#include "uos/rt/json.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>

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
      if (!isPrimitive(left) || !isPrimitive(right)) return left.identity() && left.identity() == right.identity();
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

    struct Ancestors {
      explicit Ancestors(std::unordered_set<const void*>& path, const void* id)
          : active(path), identity(id), repeated(id && !path.insert(id).second) {}
      ~Ancestors() { if (identity && !repeated) active.erase(identity); }
      std::unordered_set<const void*>& active;
      const void* identity;
      bool repeated;
    };

    struct TraversalDepth {
      explicit TraversalDepth(std::size_t& depth) : level(depth) { ++level; }
      ~TraversalDepth() { --level; }
      std::size_t& level;
    };

    json::value toJsonValue(const KvValue& value, std::unordered_set<const void*>& active, std::size_t depth) {
      const Ancestors ancestors(active, value.identity());
      if (ancestors.repeated) throw TypeError("JSON.stringify cannot serialize cyclic structures.");
      return std::visit(
        [&](const auto& item) -> json::value {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, KvUndefined> || std::is_same_v<T, std::nullptr_t> || std::is_same_v<T, KvHole>) return nullptr;
          else if constexpr (std::is_same_v<T, bool>) return item;
          else if constexpr (std::is_same_v<T, double>) return numberToJson(item);
          else if constexpr (std::is_same_v<T, std::string>) return json::string(item);
          else if constexpr (std::is_same_v<T, KvArray>) {
            if (depth >= kJsonMaxDepth) throw RangeError("Maximum call stack size exceeded.");
            json::array out;
            out.reserve(item.size());
            for (const KvValue& element : item) out.push_back(toJsonValue(element, active, depth + 1));
            return out;
          } else if constexpr (std::is_same_v<T, KvObject>) {
            if (depth >= kJsonMaxDepth) throw RangeError("Maximum call stack size exceeded.");
            json::object out;
            for (const auto& [key, member] : item)
              if (!member.isUndefined() && !member.template is<KvHole>()) out[key] = toJsonValue(member, active, depth + 1);
            return out;
          } else {
            throw TypeError("KV value is " + std::string(value.kind()) + ", not JSON");
          }
        },
        value.variant());
    }

    std::string quoteJsonString(std::string_view text) { return json::serialize(json::value(json::string(text))); }

    std::string showNumber(double number) { return number == 0 && std::signbit(number) ? "-0" : numberToString(number); }

    std::string showBytes(std::span<const std::uint8_t> bytes) {
      std::string out = "[";
      for (std::size_t index = 0; index < bytes.size(); ++index) out += (index ? ", " : "") + std::to_string(bytes[index]);
      return out + "]";
    }

  }

  KvValue kvStructuredClone(const KvValue& value) {
    if (value.identity()) return kvJscDeserialize(kvJscSerialize(value));
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
          KvArray out(item.size(), KvHole{});
          for (std::size_t index = item.nextPresent(0); index < item.size(); index = item.nextPresent(index + 1))
            out.set(index, kvStructuredClone(item[index]));
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
        } else if constexpr (std::is_constructible_v<KvValue, T>) return item;
        else std::terminate();
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

  // ---- KvArray ----------------------------------------------------------------------------

  struct KvArray::Sparse {
    std::size_t length;
    std::map<std::size_t, KvValue> entries;
  };

  namespace {
    constexpr std::size_t SmallArrayLength = 32;

    const KvValue& holeValue() {
      static const KvValue hole = KvHole{};
      return hole;
    }
  }

  KvArray::KvArray() = default;
  KvArray::KvArray(std::initializer_list<KvValue> values) : dense_(values) {}
  KvArray::KvArray(std::size_t count, const KvValue& value) {
    if (count > SmallArrayLength && value.is<KvHole>()) sparse_ = std::make_unique<Sparse>(Sparse{count, {}});
    else dense_.assign(count, value);
  }
  KvArray::KvArray(const_iterator first, const_iterator last)
      : KvArray(static_cast<std::size_t>(last - first), holeValue()) {
    if (first == last) return;
    const auto start = static_cast<std::size_t>(first.index_);
    const auto end = static_cast<std::size_t>(last.index_);
    const KvArray& source = *first.array_;
    for (std::size_t index = source.nextPresent(start); index < end; index = source.nextPresent(index + 1)) set(index - start, source[index]);
  }
  KvArray::KvArray(const KvArray& other) : dense_(other.dense_) {
    if (other.sparse_) sparse_ = std::make_unique<Sparse>(*other.sparse_);
  }
  KvArray::KvArray(KvArray&&) noexcept = default;
  KvArray& KvArray::operator=(const KvArray& other) {
    if (this != &other) *this = KvArray(other);
    return *this;
  }
  KvArray& KvArray::operator=(KvArray&&) noexcept = default;
  KvArray::~KvArray() = default;

  std::size_t KvArray::sparseSize() const noexcept { return sparse_->length; }
  void KvArray::reserve(std::size_t count) { if (!sparse_) dense_.reserve(count); }
  void KvArray::push_back(KvValue value) {
    if (sparse_) set(size(), std::move(value));
    else dense_.push_back(std::move(value));
  }

  KvArray::iterator KvArray::insert(const_iterator position, const_iterator first, const_iterator last) {
    const auto at = static_cast<std::size_t>(position.index_);
    KvArray inserted(first, last);
    if (inserted.empty()) return iterator(this, at);
    KvArray result(size() + inserted.size(), holeValue());
    for (std::size_t index = nextPresent(0); index < size(); index = nextPresent(index + 1))
      result.set(index < at ? index : index + inserted.size(), std::move((*this)[index]));
    for (std::size_t index = inserted.nextPresent(0); index < inserted.size(); index = inserted.nextPresent(index + 1))
      result.set(at + index, std::move(inserted[index]));
    *this = std::move(result);
    return iterator(this, at);
  }

  const KvValue& KvArray::sparseElement(std::size_t index) const noexcept {
    if (sparse_) {
      const auto found = sparse_->entries.find(index);
      if (found != sparse_->entries.end()) return found->second;
    }
    return holeValue();
  }

  KvValue& KvArray::operator[](std::size_t index) {
    if (!sparse_ || index < dense_.size()) return dense_[index];
    return sparse_->entries.try_emplace(index, KvHole{}).first->second;
  }

  void KvArray::set(std::size_t index, KvValue value) {
    if (!sparse_ && index > dense_.size() && index - dense_.size() > SmallArrayLength)
      sparse_ = std::make_unique<Sparse>(Sparse{dense_.size(), {}});
    if (index < dense_.size()) {
      dense_[index] = std::move(value);
    } else if (!sparse_) {
      dense_.resize(index, holeValue());
      dense_.push_back(std::move(value));
    } else {
      sparse_->length = std::max(sparse_->length, index + 1);
      if (index == dense_.size() && !sparse_->entries.contains(index) && !value.is<KvHole>()) {
        dense_.push_back(std::move(value));
        if (dense_.size() == sparse_->length) sparse_.reset();
      } else if (value.is<KvHole>()) {
        sparse_->entries.erase(index);
      } else {
        sparse_->entries.insert_or_assign(index, std::move(value));
      }
    }
  }

  std::size_t KvArray::nextSparsePresent(std::size_t from) const noexcept {
    auto next = sparse_->entries.lower_bound(from);
    while (next != sparse_->entries.end() && next->second.is<KvHole>()) ++next;
    return next == sparse_->entries.end() ? sparse_->length : next->first;
  }

  bool operator==(const KvArray& left, const KvArray& right) {
    if (left.size() != right.size()) return false;
    std::size_t index = left.nextPresent(0);
    std::size_t other = right.nextPresent(0);
    while (index == other && index < left.size()) {
      if (!(left[index] == right[index])) return false;
      index = left.nextPresent(index + 1);
      other = right.nextPresent(other + 1);
    }
    return index == other;
  }

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
    if (isPrimitive(key) || key.identity())
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
    if (isPrimitive(value) || value.identity())
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

  kv_detail::ErrorCause::ErrorCause(const ErrorCause& other)
      : value_(other && other->identity() ? std::make_shared<const KvValue>(*other) : other.value_) {}

  kv_detail::ErrorCause& kv_detail::ErrorCause::operator=(const ErrorCause& other) {
    if (this != &other) *this = ErrorCause(other);
    return *this;
  }

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

  KvValue kv_detail::ValueGraph::add(KvValue value) {
    return link(nodes_.emplace_back(GraphNode{std::move(value), nullptr, 0}));
  }

  KvValue kv_detail::ValueGraph::link(GraphNode& node) const {
    return KvValue(KvValue::GraphReference{this, &node, nullptr});
  }

  KvValue kv_detail::ValueGraph::borrow(const KvValue& value) {
    if (const auto* reference = std::get_if<KvValue::GraphReference>(&value.value_))
      return KvValue(KvValue::GraphReference{reference->graph, reference->node, nullptr});
    return value;
  }

  kv_detail::GraphNode& kv_detail::ValueGraph::node(const KvValue& link) {
    return *std::get<KvValue::GraphReference>(link.value_).node;
  }

  KvValue kv_detail::ValueGraph::finish(KvValue value) {
    value.retainGraph();
    return value;
  }

  KvValue::KvValue(const KvValue& other) : value_(other.value_) { retainGraph(); }

  KvValue& KvValue::operator=(const KvValue& other) {
    if (this != &other) *this = KvValue(other);
    return *this;
  }

  void KvValue::retainGraph() {
    if (auto* reference = std::get_if<GraphReference>(&value_); reference && !reference->owner)
      reference->owner = reference->graph->shared_from_this();
  }

  const KvValue::Variant& KvValue::graphValue(const GraphReference& reference) noexcept { return reference.node->value.value_; }

  KvValue::Variant& KvValue::variant() {
    if (const auto* reference = std::get_if<GraphReference>(&value_)) {
      Variant snapshot = graphValue(*reference);
      value_ = std::move(snapshot);
    }
    return value_;
  }

  const KvValue* KvValue::backingBuffer() const noexcept {
    const auto* reference = std::get_if<GraphReference>(&value_);
    return reference ? reference->node->backing.get() : nullptr;
  }

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
    if (const auto* object = std::get_if<KvObject>(&variant())) {
      const KvValue& member = object->at(key);
      if (const void* id = identity(); id && member.identity() == id) return *this;
      return member;
    }
    return undefinedValue();
  }

  const KvValue& KvValue::operator[](std::size_t index) const noexcept {
    if (const auto* array = std::get_if<KvArray>(&variant()); array && index < array->size()) {
      const KvValue& element = (*array)[index];
      if (const void* id = identity(); id && element.identity() == id) return *this;
      if (!element.is<KvHole>()) return element;
    }
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
      variant());
  }

  std::string_view KvValue::kind() const noexcept {
    return std::visit(
      [](const auto& item) -> std::string_view {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvArrayBufferView>) return kvViewKindName(item.kind);
        else return kindName<T>();
      },
      variant());
  }

  json::value KvValue::toJson() const {
    std::unordered_set<const void*> active;
    return toJsonValue(*this, active, 0);
  }

  bool operator==(const KvValue& left, const KvValue& right) {
    const KvValue::Variant& lhs = left.variant();
    const KvValue::Variant& rhs = right.variant();
    if (lhs.index() != rhs.index()) return false;
    using Pair = std::pair<const void*, const void*>;
    struct PairHash {
      std::size_t operator()(const Pair& pair) const noexcept {
        return std::hash<const void*>{}(pair.first) ^ (std::hash<const void*>{}(pair.second) << 1);
      }
    };
    using Pairs = std::unordered_set<Pair, PairHash>;
    static thread_local Pairs active;
    static thread_local std::size_t depth = 0;
    const Pair pair{left.identity(), right.identity()};
    if (pair.first && pair.first == pair.second) return true;
    const bool track = pair.first && pair.second;
    if (track && !active.insert(pair).second) return true;
    struct RemovePair {
      Pairs& active;
      Pair pair;
      bool track;
      ~RemovePair() { if (track) active.erase(pair); }
    } cleanup{active, pair, track};
    if (depth > kv_detail::MaxContainerDepth) throw RangeError("Maximum call stack size exceeded.");
    const TraversalDepth nesting(depth);
    return std::visit(
      [&](const auto& item) -> bool {
        using T = std::decay_t<decltype(item)>;
        const T& other = std::get<T>(rhs);
        if constexpr (std::is_same_v<T, double>) return sameNumber(item, other);
        else if constexpr (std::is_same_v<T, KvValue::GraphReference>) std::terminate();
        else return item == other;
      },
      lhs);
  }

  std::string show(const KvValue& value) {
    static thread_local std::unordered_set<const void*> active;
    static thread_local std::size_t depth = 0;
    const Ancestors ancestors(active, value.identity());
    if (ancestors.repeated) return "<cycle>";
    if (depth > kv_detail::MaxContainerDepth) return "<truncated>";
    const TraversalDepth nesting(depth);
    return std::visit(
      [&](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvUndefined>) return "undefined";
        else if constexpr (std::is_same_v<T, std::nullptr_t>) return "null";
        else if constexpr (std::is_same_v<T, KvHole>) return "<hole>";
        else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<T, double>) return showNumber(item);
        else if constexpr (std::is_same_v<T, std::string>) return quoteJsonString(item);
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
          return item.name + "(" + (item.message ? quoteJsonString(*item.message) : std::string("undefined")) + (item.cause ? ", { cause: " + show(*item.cause) + " }" : "") + ")";
        else if constexpr (std::is_same_v<T, KvArray>) {
          std::string out = "[";
          for (std::size_t index = 0; index < item.size();) {
            const std::size_t present = item.nextPresent(index);
            if (index != 0) out += ", ";
            if (present == index) out += show(item[index++]);
            else {
              out += present == index + 1 ? "<hole>" : "<" + std::to_string(present - index) + " holes>";
              index = present;
            }
          }
          return out + "]";
        } else if constexpr (std::is_same_v<T, KvObject>) {
          if (item.empty()) return "{}";
          std::string out = "{ ";
          bool first = true;
          for (const auto& [key, member] : item) {
            out += (first ? "" : ", ") + quoteJsonString(key) + ": " + show(member);
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
        } else if constexpr (std::is_same_v<T, KvSet>) {
          std::string out = "Set(" + std::to_string(item.size()) + ") {";
          bool first = true;
          for (const KvValue& member : item) {
            out += (first ? " " : ", ") + show(member);
            first = false;
          }
          return out + (item.empty() ? "}" : " }");
        } else std::terminate();
      },
      value.variant());
  }

  std::string show(const KvKeyPart& part) {
    switch (part.value.index()) {
      case 0: return "Uint8Array " + showBytes(part.bytes());
      case 1: return quoteJsonString(part.string());
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
    return result.ok ? "{ ok: true, versionstamp: " + quoteJsonString(result.versionstamp) + " }" : (result.versionstamp.empty() ? "{ ok: false }" : "{ ok: false, versionstamp: " + quoteJsonString(result.versionstamp) + " }");
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
    std::unordered_set<const void*> seen;
    while (!pending.empty()) {
      const KvValue* current = pending.back();
      pending.pop_back();
      if (const void* id = current->identity(); id && !seen.insert(id).second) continue;
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
        for (std::size_t index = array->nextPresent(0); index < array->size(); index = array->nextPresent(index + 1))
          pending.push_back(&(*array)[index]);
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
