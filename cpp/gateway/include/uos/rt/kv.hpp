#pragma once
// Deno KV for ported modules: the `#kv` facade (src/storage/kv.ts over the
// @deno/kv 0.14 SDK, its native SQLite store and the native-kv.ts guard) as the
// abstract rt::Kv, the tuple key codec, the stored value model, and MemoryKv, an
// in-memory store with the observable semantics of that guarded native store.
// docs/rt/kv.md is the porting table; docs/rt/kv-durable-design.md the plan for
// a store that opens the production database.
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/prelude.hpp"
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt {
  // A Uint8Array that owns its whole ArrayBuffer (every Uint8Array a KV read
  // returns at the top level, and the common case nested).
  using KvBytes = std::vector<std::uint8_t>;

  // `KvU64`: an unsigned 64-bit counter, stored as 8 little-endian bytes and the
  // operand of sum/min/max. Construction validates like `new KvU64(n)`.
  struct KvU64 {
    std::uint64_t value = 0;

    constexpr KvU64() noexcept = default;
    // Error "value must be a positive bigint" for a negative argument.
    template <std::integral T>
      requires(!std::same_as<T, bool>)
    constexpr explicit KvU64(T n) : value(checked(n)) {}
    // `new KvU64(n)`: Error "value must be a positive bigint" below zero, Error
    // "value must fit in a 64-bit unsigned integer" above 2^64 - 1.
    explicit KvU64(const BigInt& n);

    // `u.sum(other)` wraps modulo 2^64; `u.min(other)` / `u.max(other)`.
    constexpr KvU64 sum(KvU64 other) const noexcept { return KvU64(fromBits, value + other.value); }
    constexpr KvU64 min(KvU64 other) const noexcept { return other.value < value ? other : *this; }
    constexpr KvU64 max(KvU64 other) const noexcept { return other.value > value ? other : *this; }
    // `u.value` as the bigint the TS reads.
    BigInt toBigInt() const;
    friend constexpr bool operator==(KvU64, KvU64) noexcept = default;

  private:
    struct FromBits {};
    static constexpr FromBits fromBits{};
    constexpr KvU64(FromBits, std::uint64_t bits) noexcept : value(bits) {}
    template <class T> static constexpr std::uint64_t checked(T n) {
      if constexpr (std::is_signed_v<T>)
        if (n < 0) throw JsError("value must be a positive bigint");
      return static_cast<std::uint64_t>(n);
    }
  };

  // ---- Keys ---------------------------------------------------------------------------

  // One key part: Uint8Array, string, number, bigint or boolean. Every arithmetic
  // C++ type except bool is a JS number (as Number(x)); a bigint part is an
  // rt::BigInt. Strings are WTF-8 (docs/rt/text.md).
  class KvKeyPart {
  public:
    using Variant = std::variant<KvBytes, std::string, double, BigInt, bool>;
    Variant value;

    KvKeyPart(KvBytes bytes) : value(std::move(bytes)) {}
    KvKeyPart(std::string text) : value(std::move(text)) {}
    KvKeyPart(std::string_view text) : value(std::string(text)) {}
    KvKeyPart(const char* text) : value(std::string(text)) {}
    KvKeyPart(const json::string& text) : value(std::string(text)) {}
    template <class T>
      requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    KvKeyPart(T number) : value(static_cast<double>(number)) {}
    KvKeyPart(bool boolean) : value(boolean) {}
    KvKeyPart(BigInt bigint) : value(std::move(bigint)) {}
    template <class T> KvKeyPart(T*) = delete;

    bool isBytes() const noexcept { return value.index() == 0; }
    bool isString() const noexcept { return value.index() == 1; }
    bool isNumber() const noexcept { return value.index() == 2; }
    bool isBigInt() const noexcept { return value.index() == 3; }
    bool isBool() const noexcept { return value.index() == 4; }
    // Typed access; TypeError "KV key part is <type>, not <type>" on a mismatch.
    const KvBytes& bytes() const;
    const std::string& string() const;
    double number() const;
    const BigInt& bigint() const;
    bool boolean() const;
    // `String(part)`: "1,2" for bytes, JS number and bigint text, "true"/"false".
    std::string toString() const;

    // Equality is key identity (equal encodings): -0 and 0 differ, every NaN is
    // the same part. Ordering is Deno KV order (the encoded bytes).
    friend bool operator==(const KvKeyPart& left, const KvKeyPart& right);
    friend std::strong_ordering operator<=>(const KvKeyPart& left, const KvKeyPart& right);
  };

  // A key: a sequence of parts. Braces build one without an std::initializer_list
  // (`kv.get({"a", id})`), and a KvKey argument is spread into the new key, so
  // `KvKey{prefix, id}` is `[...prefix, id]`. (GCC 12.2 cannot keep an
  // initializer_list's array alive across co_await; see docs/rt/kv.md.)
  class KvKey : public std::vector<KvKeyPart> {
  public:
    KvKey() = default;
    KvKey(std::vector<KvKeyPart> parts) : std::vector<KvKeyPart>(std::move(parts)) {}
    template <class... Parts>
      requires(sizeof...(Parts) > 0 && !(sizeof...(Parts) == 1 && (std::is_same_v<std::remove_cvref_t<Parts>, KvKey> && ...)) &&
               ((std::is_same_v<std::remove_cvref_t<Parts>, KvKey> || std::is_constructible_v<KvKeyPart, Parts>) && ...))
    KvKey(Parts&&... parts) {
      reserve(sizeof...(Parts));
      (append(std::forward<Parts>(parts)), ...);
    }

  private:
    template <class Part> void append(Part&& part) {
      if constexpr (std::is_same_v<std::remove_cvref_t<Part>, KvKey>) {
        reserve(size() + part.size());
        for (const auto& component : part) emplace_back(component);
      } else emplace_back(std::forward<Part>(part));
    }
  };

  // `[...prefix, a, b]`: KvKey arguments are spread, every other argument is one part.
  template <class... Parts> KvKey kvKey(Parts&&... parts) { return KvKey(std::forward<Parts>(parts)...); }

  // The tuple codec of @deno/kv 0.14 with the repository's number patch:
  // 0x01 bytes and 0x02 UTF-8 string (0x00 escaped as 00 FF, terminated by 00;
  // lone surrogates encode as U+FFFD, as TextEncoder does), 0x0b..0x1d bigint,
  // 0x21 number (IEEE bits, sign-flipped; every NaN canonical), 0x26 false,
  // 0x27 true. Byte order of the encodings is Deno KV order.
  KvBytes packKeyPart(const KvKeyPart& part);
  KvBytes packKey(const KvKey& key);
  // `unpackKey` with the SDK's errors: Error "Unsupported typecode: <n> in key:
  // [<bytes>] after <parts>", Error "Unexpected trailing bytes: <n>", RangeError
  // "Out of bounds access" (truncated number), TypeError "Invalid argument type in
  // ToBigInt operation" (truncated bigint). Strings decode like `new TextDecoder()`:
  // a leading BOM is dropped and ill-formed bytes become U+FFFD.
  KvKey unpackKey(std::span<const std::uint8_t> bytes);
  // <0, 0, >0 in Deno KV order.
  int compareKvKeys(const KvKey& left, const KvKey& right);

  // A versionstamp: 20 lowercase hex digits, the 8-byte big-endian commit version
  // followed by "0000" (`00000000000000010000` for version 1).
  std::string formatVersionstamp(std::uint64_t version);

  // A list cursor: base64 of `{"lastYieldedKeyBytes":"<hex of the packed key>"}`,
  // the SDK's `packCursor` (gateway records persist these strings).
  std::string packCursor(std::span<const std::uint8_t> lastYieldedKeyBytes);
  // The SDK's `unpackCursor`: forgiving base64 (atob), UTF-8, JSON, hex; Error
  // "Invalid cursor" for anything else.
  KvBytes unpackCursor(std::string_view cursor);

  // ---- Values ---------------------------------------------------------------------------

  class KvValue;
  namespace kv_detail {
    class ValueGraph;
    struct GraphNode;
  }

  // Array length includes holes. Large gaps occupy no element slots.
  class KvArray {
  public:
    template<bool Const> class Iterator {
    public:
      using iterator_category = std::random_access_iterator_tag;
      using value_type = KvValue;
      using difference_type = std::ptrdiff_t;
      using pointer = std::conditional_t<Const, const KvValue*, KvValue*>;
      using reference = std::conditional_t<Const, const KvValue&, KvValue&>;

      Iterator() = default;
      template<bool Other>
        requires(Const && !Other)
      Iterator(const Iterator<Other>& other) : array_(other.array_), index_(other.index_) {}
      reference operator*() const { return (*array_)[static_cast<std::size_t>(index_)]; }
      pointer operator->() const { return &**this; }
      reference operator[](difference_type offset) const { return *(*this + offset); }
      Iterator& operator++() { return *this += 1; }
      Iterator& operator--() { return *this -= 1; }
      Iterator operator++(int) { auto previous = *this; ++*this; return previous; }
      Iterator operator--(int) { auto previous = *this; --*this; return previous; }
      Iterator& operator+=(difference_type offset) { index_ += offset; return *this; }
      Iterator& operator-=(difference_type offset) { index_ -= offset; return *this; }
      friend Iterator operator+(Iterator it, difference_type offset) { return it += offset; }
      friend Iterator operator+(difference_type offset, Iterator it) { return it += offset; }
      friend Iterator operator-(Iterator it, difference_type offset) { return it -= offset; }
      friend difference_type operator-(Iterator left, Iterator right) { return left.index_ - right.index_; }
      friend auto operator<=>(const Iterator&, const Iterator&) = default;

    private:
      friend class KvArray;
      template<bool> friend class Iterator;
      using Array = std::conditional_t<Const, const KvArray, KvArray>;
      Iterator(Array* array, std::size_t index) : array_(array), index_(static_cast<difference_type>(index)) {}
      Array* array_ = nullptr;
      difference_type index_ = 0;
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    KvArray();
    KvArray(std::initializer_list<KvValue> values);
    KvArray(std::size_t count, const KvValue& value);
    KvArray(const_iterator first, const_iterator last);
    KvArray(const KvArray&);
    KvArray(KvArray&&) noexcept;
    KvArray& operator=(const KvArray&);
    KvArray& operator=(KvArray&&) noexcept;
    ~KvArray();

    std::size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }
    void reserve(std::size_t count);
    void push_back(KvValue value);
    template<class... Args> void emplace_back(Args&&... args);
    iterator insert(const_iterator position, const_iterator first, const_iterator last);
    const KvValue& operator[](std::size_t index) const noexcept;
    KvValue& operator[](std::size_t index);
    const KvValue& front() const noexcept { return (*this)[0]; }
    KvValue& front() { return (*this)[0]; }
    iterator begin() noexcept { return iterator(this, 0); }
    iterator end() noexcept { return iterator(this, size()); }
    const_iterator begin() const noexcept { return const_iterator(this, 0); }
    const_iterator end() const noexcept { return const_iterator(this, size()); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // Define an index, extending the length as necessary. nextPresent returns
    // size() at the end. Explicit undefined is present; a hole is absent.
    void set(std::size_t index, KvValue value);
    std::size_t nextPresent(std::size_t from) const noexcept;
    friend bool operator==(const KvArray& left, const KvArray& right);

  private:
    struct Sparse;
    std::size_t sparseSize() const noexcept;
    const KvValue& sparseElement(std::size_t index) const noexcept;
    std::size_t nextSparsePresent(std::size_t from) const noexcept;
    std::vector<KvValue> dense_;
    std::unique_ptr<Sparse> sparse_;
  };

  // `undefined`
  struct KvUndefined {
    friend constexpr bool operator==(KvUndefined, KvUndefined) noexcept { return true; }
  };

  // An array hole (`[, 1]`). Valid only as a KvArray element; appears in arrays
  // converted from legacy databases, never in values the gateway writes.
  struct KvHole {
    friend constexpr bool operator==(KvHole, KvHole) noexcept { return true; }
  };

  // A plain object. Members iterate in JS enumeration order: array-index keys
  // ("0".."4294967294") ascending first, then the others in insertion order.
  class KvObject {
  public:
    using Entry = std::pair<std::string, KvValue>;
    using const_iterator = std::vector<Entry>::const_iterator;
    using iterator = std::vector<Entry>::iterator;

    KvObject() = default;
    KvObject(std::initializer_list<Entry> entries);
    // `object[key] = value`: an existing key keeps its position.
    void set(std::string key, KvValue value);
    // `Object.hasOwn(object, key)`
    bool contains(std::string_view key) const noexcept;
    // The member, or nullptr when absent.
    const KvValue* find(std::string_view key) const noexcept;
    KvValue* find(std::string_view key) noexcept;
    // `object[key]`: the member, or undefined when absent.
    const KvValue& at(std::string_view key) const noexcept;
    // `delete object[key]`; true when a member was removed.
    bool erase(std::string_view key);
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;
    iterator begin() noexcept;
    iterator end() noexcept;
    friend bool operator==(const KvObject& left, const KvObject& right);

  private:
    std::vector<Entry> entries_;
  };

  // A Map in insertion order. Primitive keys are unique under SameValueZero (a -0
  // key is stored as +0, like Map.prototype.set); decoded objects compare by identity.
  class KvMap {
  public:
    using Entry = std::pair<KvValue, KvValue>;
    using const_iterator = std::vector<Entry>::const_iterator;

    KvMap() = default;
    KvMap(std::initializer_list<Entry> entries);
    void set(KvValue key, KvValue value);
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;
    friend bool operator==(const KvMap& left, const KvMap& right);

  private:
    std::vector<Entry> entries_;
  };

  // A Set in insertion order, unique like KvMap keys.
  class KvSet {
  public:
    using const_iterator = std::vector<KvValue>::const_iterator;

    KvSet() = default;
    KvSet(std::initializer_list<KvValue> values);
    void add(KvValue value);
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;
    friend bool operator==(const KvSet& left, const KvSet& right);

  private:
    std::vector<KvValue> values_;
  };

  // `new Date(time)`: milliseconds since the epoch; NaN is an invalid date.
  struct KvDate {
    double time = 0;
    // Object.is on the time value.
    friend bool operator==(const KvDate& left, const KvDate& right) noexcept;
  };

  // A RegExp's source and flags (`/a+/gi` is {"a+", "gi"}).
  struct KvRegExp {
    std::string source{};
    std::string flags{};
    friend bool operator==(const KvRegExp&, const KvRegExp&) = default;
  };

  // An ArrayBuffer.
  struct KvArrayBuffer {
    KvBytes bytes{};
    friend bool operator==(const KvArrayBuffer&, const KvArrayBuffer&) = default;
  };

  // The view classes structured clone keeps, in their serialization subtag order.
  enum class KvViewKind : std::uint8_t {
    DataView,
    Int8Array,
    Uint8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array,
    BigInt64Array,
    BigUint64Array,
  };
  std::string_view kvViewKindName(KvViewKind kind) noexcept;

  // A typed array or DataView: `byteLength` bytes at `byteOffset` of its whole
  // backing ArrayBuffer (structured clone copies the entire buffer). A Uint8Array
  // spanning its whole buffer is KvBytes.
  struct KvArrayBufferView {
    KvViewKind kind = KvViewKind::Uint8Array;
    KvBytes buffer{};
    std::uint64_t byteOffset = 0;
    std::uint64_t byteLength = 0;
    friend bool operator==(const KvArrayBufferView&, const KvArrayBufferView&) = default;
  };

  namespace kv_detail {
    class ErrorCause {
    public:
      ErrorCause() = default;
      ErrorCause(std::nullptr_t) {}
      ErrorCause(std::shared_ptr<const KvValue> value) : value_(std::move(value)) {}
      ErrorCause(const ErrorCause&);
      ErrorCause(ErrorCause&&) noexcept = default;
      ErrorCause& operator=(const ErrorCause&);
      ErrorCause& operator=(ErrorCause&&) noexcept = default;
      ErrorCause& operator=(std::nullptr_t) { value_.reset(); return *this; }
      ErrorCause& operator=(std::shared_ptr<const KvValue> value) { value_ = std::move(value); return *this; }
      explicit operator bool() const noexcept { return static_cast<bool>(value_); }
      const KvValue* get() const noexcept { return value_.get(); }
      const KvValue& operator*() const noexcept { return *value_; }
      const KvValue* operator->() const noexcept { return get(); }

    private:
      std::shared_ptr<const KvValue> value_;
    };
  }

  // An Error instance as structured clone keeps it: the class name (Error,
  // EvalError, RangeError, ReferenceError, SyntaxError, TypeError or URIError; any
  // other name clones as "Error"), the own message (std::nullopt when absent), the
  // line/column/sourceURL where it was created and its stack. A set `cause` (an own
  // `cause` property, even undefined) cannot be stored: committing a value holding
  // one throws TypeError "Bun KV cannot preserve Error.cause; persist an explicit
  // error record instead".
  struct KvError {
    std::string name = "Error";
    std::optional<std::string> message{};
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::optional<std::string> sourceURL{};
    std::optional<std::string> stack{};
    kv_detail::ErrorCause cause{};
    friend bool operator==(const KvError& left, const KvError& right);
  };

  // A stored value: numbers are doubles and strings are WTF-8. Decoded object
  // graphs own their nodes together, and copying a node handle keeps its graph alive.
  class KvValue {
    struct GraphReference {
      const kv_detail::ValueGraph* graph;
      kv_detail::GraphNode* node;
      std::shared_ptr<const kv_detail::ValueGraph> owner;
    };

  public:
    using Variant = std::variant<KvUndefined, std::nullptr_t, bool, double, std::string, BigInt, KvU64, KvBytes, KvArrayBuffer, KvArrayBufferView,
                                 KvDate, KvRegExp, KvError, KvArray, KvObject, KvMap, KvSet, KvHole, GraphReference>;

    // `undefined`
    KvValue() noexcept : value_(KvUndefined{}) {}
    KvValue(KvUndefined) noexcept : value_(KvUndefined{}) {}
    KvValue(std::nullptr_t) noexcept : value_(nullptr) {}
    KvValue(bool boolean) noexcept : value_(boolean) {}
    template <class T>
      requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    KvValue(T number) noexcept : value_(static_cast<double>(number)) {}
    KvValue(const char* text) : value_(std::string(text)) {}
    KvValue(std::string text) : value_(std::move(text)) {}
    KvValue(std::string_view text) : value_(std::string(text)) {}
    KvValue(BigInt bigint) : value_(std::move(bigint)) {}
    KvValue(KvU64 counter) noexcept : value_(counter) {}
    KvValue(KvBytes bytes) : value_(std::move(bytes)) {}
    KvValue(KvArrayBuffer buffer) : value_(std::move(buffer)) {}
    KvValue(KvArrayBufferView view) : value_(std::move(view)) {}
    KvValue(KvDate date) noexcept : value_(date) {}
    KvValue(KvRegExp regexp) : value_(std::move(regexp)) {}
    KvValue(KvError error) : value_(std::move(error)) {}
    KvValue(KvArray array) : value_(std::move(array)) {}
    KvValue(KvObject object) : value_(std::move(object)) {}
    KvValue(KvMap map) : value_(std::move(map)) {}
    KvValue(KvSet set) : value_(std::move(set)) {}
    KvValue(KvHole) noexcept : value_(KvHole{}) {}
    // JSON-shaped values; every boost::json number becomes a JS number.
    KvValue(const json::value& value);
    KvValue(const json::object& object);
    KvValue(const json::array& array);
    KvValue(const json::string& text) : value_(std::string(text)) {}
    template <class T> KvValue(T*) = delete;
    KvValue(const KvValue&);
    KvValue(KvValue&&) noexcept = default;
    KvValue& operator=(const KvValue&);
    KvValue& operator=(KvValue&&) noexcept = default;

    static KvValue undefined() noexcept { return KvValue(); }
    static KvValue array(std::initializer_list<KvValue> items);
    static KvValue object(std::initializer_list<KvObject::Entry> entries);

    bool isUndefined() const noexcept { return is<KvUndefined>(); }
    bool isNull() const noexcept { return is<std::nullptr_t>(); }
    // `value == null`: null or undefined.
    bool isNullish() const noexcept { return isUndefined() || isNull(); }
    bool isBool() const noexcept { return is<bool>(); }
    bool isNumber() const noexcept { return is<double>(); }
    bool isString() const noexcept { return is<std::string>(); }
    bool isBigInt() const noexcept { return is<BigInt>(); }
    // `value instanceof KvU64` (only a top-level stored KvU64 reads back as one).
    bool isU64() const noexcept { return is<KvU64>(); }
    // `value instanceof Uint8Array` for a Uint8Array owning its whole buffer.
    bool isBytes() const noexcept { return is<KvBytes>(); }
    bool isArray() const noexcept { return is<KvArray>(); }
    // A plain object (not an array, Map, Set, Date, ...).
    bool isObject() const noexcept { return is<KvObject>(); }
    template <class T> bool is() const noexcept { return std::holds_alternative<T>(variant()); }

    // Typed access; TypeError "KV value is <type>, not <type>" on a mismatch.
    bool asBool() const { return as<bool>(); }
    double asNumber() const { return as<double>(); }
    const std::string& asString() const { return as<std::string>(); }
    const BigInt& asBigInt() const { return as<BigInt>(); }
    KvU64 asU64() const { return as<KvU64>(); }
    const KvBytes& asBytes() const { return as<KvBytes>(); }
    const KvArray& asArray() const { return as<KvArray>(); }
    KvArray& asArray() { return as<KvArray>(); }
    const KvObject& asObject() const { return as<KvObject>(); }
    KvObject& asObject() { return as<KvObject>(); }
    template <class T> const T& as() const {
      if (const T* found = std::get_if<T>(&variant())) return *found;
      throwMismatch(kindName<T>());
    }
    template <class T> T& as() {
      if (is<T>()) return std::get<T>(variant());
      throwMismatch(kindName<T>());
    }

    // `value[key]` / `value[index]` on an object / array: the member or element,
    // else undefined (also for a hole and for any other type).
    const KvValue& operator[](std::string_view key) const noexcept;
    const KvValue& operator[](std::size_t index) const noexcept;

    // JS truthiness: false for undefined, null, false, +-0, NaN, "" and 0n.
    bool truthy() const noexcept;
    // "undefined", "null", "boolean", "number", "string", "bigint", "KvU64",
    // "Uint8Array", "ArrayBuffer", the view class, "Date", "RegExp", "Error",
    // "Array", "Object", "Map", "Set" or "hole".
    std::string_view kind() const noexcept;

    // The JSON-shaped equivalent, for code that treats a stored value as parsed
    // JSON: undefined members and holes are dropped from objects, an undefined
    // array element or hole becomes null, a top-level undefined is null; an
    // integral number below 1e21 in magnitude becomes int64 (uint64 past
    // INT64_MAX), as json::parse reads JSON.stringify's text, every other number
    // (fractions, -0, NaN, +-Infinity, 1e21 and up) stays a double. Any other type
    // throws TypeError "KV value is <type>, not JSON".
    json::value toJson() const;

    const Variant& variant() const noexcept {
      if (const auto* reference = std::get_if<GraphReference>(&value_)) return graphValue(*reference);
      return value_;
    }
    // Editable C++ values are snapshots of the selected node. Const access
    // traverses the decoded graph with its original identities.
    Variant& variant();
    const void* identity() const noexcept {
      const auto* reference = std::get_if<GraphReference>(&value_);
      return reference ? reference->node : nullptr;
    }
    const KvValue* backingBuffer() const noexcept;

    // Structural equality; numbers (also inside Date) compare with Object.is.
    friend bool operator==(const KvValue& left, const KvValue& right);

  private:
    friend class kv_detail::ValueGraph;
    explicit KvValue(GraphReference reference) : value_(std::move(reference)) {}
    static const Variant& graphValue(const GraphReference& reference) noexcept;
    void retainGraph();
    template <class T> static constexpr std::string_view kindName() noexcept {
      if constexpr (std::is_same_v<T, KvUndefined>) return "undefined";
      else if constexpr (std::is_same_v<T, std::nullptr_t>) return "null";
      else if constexpr (std::is_same_v<T, bool>) return "boolean";
      else if constexpr (std::is_same_v<T, double>) return "number";
      else if constexpr (std::is_same_v<T, std::string>) return "string";
      else if constexpr (std::is_same_v<T, BigInt>) return "bigint";
      else if constexpr (std::is_same_v<T, KvU64>) return "KvU64";
      else if constexpr (std::is_same_v<T, KvBytes>) return "Uint8Array";
      else if constexpr (std::is_same_v<T, KvArrayBuffer>) return "ArrayBuffer";
      else if constexpr (std::is_same_v<T, KvArrayBufferView>) return "ArrayBufferView";
      else if constexpr (std::is_same_v<T, KvDate>) return "Date";
      else if constexpr (std::is_same_v<T, KvRegExp>) return "RegExp";
      else if constexpr (std::is_same_v<T, KvError>) return "Error";
      else if constexpr (std::is_same_v<T, KvArray>) return "Array";
      else if constexpr (std::is_same_v<T, KvObject>) return "Object";
      else if constexpr (std::is_same_v<T, KvMap>) return "Map";
      else if constexpr (std::is_same_v<T, KvSet>) return "Set";
      else return "hole";
    }
    [[noreturn]] void throwMismatch(std::string_view expected) const;
    Variant value_;
  };

  inline std::size_t KvArray::size() const noexcept { return sparse_ ? sparseSize() : dense_.size(); }

  inline const KvValue& KvArray::operator[](std::size_t index) const noexcept {
    return index < dense_.size() ? dense_[index] : sparseElement(index);
  }

  inline std::size_t KvArray::nextPresent(std::size_t from) const noexcept {
    for (; from < dense_.size(); ++from)
      if (!dense_[from].is<KvHole>()) return from;
    return sparse_ ? nextSparsePresent(from) : dense_.size();
  }

  template<class... Args> void KvArray::emplace_back(Args&&... args) {
    push_back(KvValue(std::forward<Args>(args)...));
  }

  // Test-harness display hooks (found by argument-dependent lookup). The text is a
  // compact debugging rendering, not Bun's console format.
  std::string show(const KvValue& value);
  std::string show(const KvKeyPart& part);
  std::string show(const KvKey& key);
  std::string show(KvU64 value);

  // `assertPortableKvValue` (src/storage/value-compatibility.ts): TypeError "Bun KV
  // cannot preserve Error.cause; persist an explicit error record instead" when an
  // Error anywhere in the value has a cause. (Accessor properties, its other
  // refusal, have no C++ representation.)
  void assertPortableKvValue(const KvValue& value);

  // `structuredClone(value)`: a KvU64 becomes the plain object `{ value: n }`, a
  // Uint8Array view over its whole buffer becomes KvBytes, an Error keeps only a
  // standard class name (and no cause), RegExp flags take canonical order. Test
  // doubles that clone what they store (counting-kv.ts, sentinel-kv-stub.ts) use it.
  KvValue kvStructuredClone(const KvValue& value);

  // The value as a KV read returns it after storing `value`: a top-level KvU64 or
  // Uint8Array is stored raw and returned as KvU64 / KvBytes; everything else is a
  // structured clone (nested KvU64 becomes the plain object `{ value: n }`, a
  // Uint8Array view over its whole buffer becomes KvBytes, an Error keeps only a
  // standard class name, RegExp flags take canonical order).
  KvValue kvStoredValue(const KvValue& value);

  // The bytes Bun 1.4's `node:v8` serialize writes for `value` (JavaScriptCore
  // SerializedScriptValue version 14, with its string constant pool), the stored
  // form of every value except a top-level KvU64 or Uint8Array. Where
  // JavaScriptCore's output depends on a value's history rather than its content,
  // the canonical form is written: an integral number in int32 range (not -0) uses
  // the int32 tags, and a string whose code units are all <= U+00FF the 8-bit form.
  // A KvU64 is written as the object `{ value: n }`.
  KvBytes kvJscSerialize(const KvValue& value);

  // Bun 1.4's node:v8 `deserialize(bytes)`, the store's decodeV8: the value a
  // JavaScriptCore SerializedScriptValue (version <= 14) holds, also inside Bun's
  // `ff 42 55 4e 01` envelope around graphs with Buffers (a Buffer reads as the
  // Uint8Array it is). Every form Bun writes reads back, the history-dependent ones
  // included (DoubleTag for integral numbers, 16-bit strings of Latin-1 text),
  // with object references and sparse arrays. Errors as Bun reports
  // them: an empty input is null; malformed bytes throw TypeError "Unable to
  // deserialize data."; a malformed envelope throws Error "failed to parse
  // serialized buffer envelope"; nesting deeper than 2000 containers (Bun: 40001;
  // KvValue copies recursively) throws RangeError "Maximum call stack size
  // exceeded.". A value KvValue
  // cannot hold (a boxed primitive, properties on an array, Map or Set, a
  // Float16Array, a resizable or length-tracking buffer, a
  // Blob or DOMException) throws TypeError "Serialized value <what> has no KvValue
  // representation".
  KvValue kvJscDeserialize(std::span<const std::uint8_t> bytes);

  // Conversion behind the typed `get<T>` / `getMany<T>` / entry `as<T>()`:
  //   KvValue           unchanged
  //   json::value       toJson()
  //   std::optional<U>  std::nullopt for null/undefined, else kvValueAs<U>
  //   json::object / json::array  toJson() of that shape, else TypeError
  //   a KvValue alternative (KvU64, KvBytes, BigInt, std::string, double, bool,
  //                     KvObject, KvArray, KvDate, ...)  that alternative, else TypeError
  //   anything else     json::value_to<T>(toJson())
  namespace kv_detail {
    template <class T> struct IsOptional : std::false_type {};
    template <class T> struct IsOptional<std::optional<T>> : std::true_type {};
  }

  template <class T> T kvValueAs(const KvValue& value) {
    if constexpr (std::is_same_v<T, KvValue>) return value;
    else if constexpr (kv_detail::IsOptional<T>::value) {
      if (value.isNullish()) return std::nullopt;
      return kvValueAs<typename T::value_type>(value);
    } else if constexpr (std::is_same_v<T, json::value>) return value.toJson();
    else if constexpr (std::is_same_v<T, json::object>) {
      if (!value.isObject()) throw TypeError("KV value is " + std::string(value.kind()) + ", not Object");
      json::value converted = value.toJson();
      return std::move(converted.as_object());
    } else if constexpr (std::is_same_v<T, json::array>) {
      if (!value.isArray()) throw TypeError("KV value is " + std::string(value.kind()) + ", not Array");
      json::value converted = value.toJson();
      return std::move(converted.as_array());
    } else if constexpr (std::is_constructible_v<KvValue::Variant, std::in_place_type_t<T>, T>) return value.as<T>();
    else return json::value_to<T>(value.toJson());
  }

  // ---- Entries, options, results --------------------------------------------------------

  // `KvEntryMaybe<T>`: for an absent key `value` is null (KvValue, json::value) or
  // std::nullopt (std::optional<U>) and `versionstamp` is std::nullopt.
  template <class T = KvValue> struct KvEntryMaybe {
    KvKey key{};
    T value{};
    std::optional<std::string> versionstamp{};
    template <class U> KvEntryMaybe<U> as() const { return {key, kvValueAs<U>(value), versionstamp}; }
    friend bool operator==(const KvEntryMaybe&, const KvEntryMaybe&) = default;
  };

  // `KvEntry<T>`: a present entry (list results).
  template <class T = KvValue> struct KvEntry {
    KvKey key{};
    T value{};
    std::string versionstamp{};
    template <class U> KvEntry<U> as() const { return {key, kvValueAs<U>(value), versionstamp}; }
    operator KvEntryMaybe<T>() const { return {key, value, versionstamp}; }
    friend bool operator==(const KvEntry&, const KvEntry&) = default;
  };

  // `"strong" | "eventual"`; the SQLite store ignores it.
  enum class KvConsistencyLevel { strong, eventual };

  struct KvGetOptions {
    std::optional<KvConsistencyLevel> consistency{};
  };

  struct KvSetOptions {
    // Milliseconds; a positive safe integer, else TypeError "Bad 'expireIn',
    // expected optional positive integer, found <n>".
    std::optional<double> expireIn{};
  };

  // `KvCommitResult | KvCommitError`: `{ ok: false }` has an empty versionstamp.
  struct KvCommitResult {
    bool ok = false;
    std::string versionstamp{};
    friend bool operator==(const KvCommitResult&, const KvCommitResult&) = default;
  };
  std::string show(const KvCommitResult& result);

  // `{ prefix }`, `{ prefix, start }`, `{ prefix, end }` or `{ start, end }`.
  struct KvListSelector {
    std::optional<KvKey> prefix{};
    std::optional<KvKey> start{};
    std::optional<KvKey> end{};
  };

  // @deno/kv's field order (designated initializers must follow it). `limit` and
  // `batchSize` are JS numbers, validated like the SDK.
  struct KvListOptions {
    std::optional<double> limit{};
    std::optional<std::string> cursor{};
    bool reverse = false;
    std::optional<KvConsistencyLevel> consistency{};
    std::optional<double> batchSize{};
  };

  // `AtomicCheck`; also built from any KvEntryMaybe / KvEntry, as the TS passes
  // entries. std::nullopt (and "") is null: the key must be absent.
  struct AtomicCheck {
    KvKey key;
    std::optional<std::string> versionstamp;
    AtomicCheck(KvKey key_, std::optional<std::string> versionstamp_) : key(std::move(key_)), versionstamp(std::move(versionstamp_)) {}
    template <class T> AtomicCheck(const KvEntryMaybe<T>& entry) : key(entry.key), versionstamp(entry.versionstamp) {}
    template <class T> AtomicCheck(const KvEntry<T>& entry) : key(entry.key), versionstamp(entry.versionstamp) {}
    friend bool operator==(const AtomicCheck&, const AtomicCheck&) = default;
  };

  // `KvMutation["type"]`; `delete_` is "delete".
  enum class KvMutationType { set, delete_, sum, min, max };
  std::string_view toString(KvMutationType type) noexcept;

  // `KvMutation`: set carries any value (and optional expireIn), sum/min/max a
  // KvU64, delete nothing.
  struct KvMutation {
    KvKey key{};
    KvMutationType type = KvMutationType::set;
    KvValue value{};
    std::optional<double> expireIn{};
  };

  // ---- Store interfaces -------------------------------------------------------------------

  // `kv.atomic()`. Every builder records and returns *this for chaining. A
  // subclass that overrides `check`, `sum`, `min` or `max` adds `using
  // AtomicOperation::check;` (and so on) to keep the template forms visible.
  class AtomicOperation {
  public:
    AtomicOperation() = default;
    AtomicOperation(const AtomicOperation&) = delete;
    AtomicOperation& operator=(const AtomicOperation&) = delete;
    virtual ~AtomicOperation() = default;

    // `check(...checks)`
    virtual AtomicOperation& check(std::vector<AtomicCheck> checks) = 0;
    // Any mix of AtomicChecks, entries and vectors of either.
    template <class... Checks>
      requires(sizeof...(Checks) > 0)
    AtomicOperation& check(const Checks&... items) {
      std::vector<AtomicCheck> flat;
      (appendCheck(flat, items), ...);
      return check(std::move(flat));
    }
    // `mutate(...mutations)`
    virtual AtomicOperation& mutate(std::vector<KvMutation> mutations) = 0;
    // `sum(key, n)` / `min` / `max` with a bigint operand (`new KvU64(n)`'s checks).
    virtual AtomicOperation& sum(KvKey key, const BigInt& n) = 0;
    virtual AtomicOperation& min(KvKey key, const BigInt& n) = 0;
    virtual AtomicOperation& max(KvKey key, const BigInt& n) = 0;
    template <std::integral T>
      requires(!std::same_as<T, bool>)
    AtomicOperation& sum(KvKey key, T n) {
      return sum(std::move(key), integralBigInt(n));
    }
    template <std::integral T>
      requires(!std::same_as<T, bool>)
    AtomicOperation& min(KvKey key, T n) {
      return min(std::move(key), integralBigInt(n));
    }
    template <std::integral T>
      requires(!std::same_as<T, bool>)
    AtomicOperation& max(KvKey key, T n) {
      return max(std::move(key), integralBigInt(n));
    }
    virtual AtomicOperation& set(KvKey key, KvValue value, KvSetOptions options = {}) = 0;
    virtual AtomicOperation& delete_(KvKey key) = 0;
    [[nodiscard]] virtual Task<KvCommitResult> commit() = 0;

  private:
    template <std::integral T> static BigInt integralBigInt(T n) {
      if constexpr (std::is_signed_v<T>) return BigInt(static_cast<std::int64_t>(n));
      else return BigInt::fromUint64(static_cast<std::uint64_t>(n));
    }
    static void appendCheck(std::vector<AtomicCheck>& out, const AtomicCheck& item) { out.push_back(item); }
    template <class T> static void appendCheck(std::vector<AtomicCheck>& out, const KvEntryMaybe<T>& entry) { out.emplace_back(entry); }
    template <class T> static void appendCheck(std::vector<AtomicCheck>& out, const KvEntry<T>& entry) { out.emplace_back(entry); }
    template <class T> static void appendCheck(std::vector<AtomicCheck>& out, const std::vector<T>& items) {
      for (const auto& item : items) appendCheck(out, item);
    }
  };

  // The SDK's GenericAtomicOperation: records checks and mutations with its
  // synchronous validation (TypeError "Key cannot be empty"; the expireIn and
  // KvU64 errors) and hands them to `commitFn` at commit(). Test doubles that
  // keep their own rows build their atomic() on it.
  class GenericAtomicOperation : public AtomicOperation {
  public:
    using CommitFunction = std::function<Task<KvCommitResult>(std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations)>;

    std::vector<AtomicCheck> checks;
    std::vector<KvMutation> mutations;

    explicit GenericAtomicOperation(CommitFunction commitFn_) : commitFn(std::move(commitFn_)) {}
    using AtomicOperation::check;
    using AtomicOperation::max;
    using AtomicOperation::min;
    using AtomicOperation::sum;
    AtomicOperation& check(std::vector<AtomicCheck> items) override;
    AtomicOperation& mutate(std::vector<KvMutation> items) override;
    AtomicOperation& sum(KvKey key, const BigInt& n) override;
    AtomicOperation& min(KvKey key, const BigInt& n) override;
    AtomicOperation& max(KvKey key, const BigInt& n) override;
    AtomicOperation& set(KvKey key, KvValue value, KvSetOptions options = {}) override;
    AtomicOperation& delete_(KvKey key) override;
    Task<KvCommitResult> commit() override;

  private:
    CommitFunction commitFn;
  };

  // `kv.list(...)`: an async iterator of entries with a resumable cursor.
  //   for await (const entry of iterator)  ->  while (auto entry = co_await iterator->next())
  class KvListIterator {
  public:
    KvListIterator() = default;
    KvListIterator(const KvListIterator&) = delete;
    KvListIterator& operator=(const KvListIterator&) = delete;
    virtual ~KvListIterator() = default;
    // `await iterator.next()`: the next entry, or std::nullopt when done.
    [[nodiscard]] virtual Task<std::optional<KvEntry<>>> next() = 0;
    // `iterator.cursor`; Error "Cannot get cursor before first iteration" when
    // there is none yet.
    virtual std::string cursor() const = 0;
    // `iterator.return()` (what `for await ... break` calls).
    [[nodiscard]] virtual Task<void> return_() = 0;
  };

  // `Array.fromAsync(iterator)`.
  Task<std::vector<KvEntry<>>> kvListToArray(KvListIterator& iterator);

  // `Kv`: the store every ported module receives. Arguments are taken by value so
  // an implementation may be a coroutine. A subclass that overrides get/getMany adds
  // `using rt::Kv::get; using rt::Kv::getMany;` to keep the typed templates visible.
  // `enqueue`, `listenQueue` and `watch` are not part of it: no first-party module
  // calls them (docs/rt/kv.md).
  class Kv {
  public:
    Kv() = default;
    Kv(const Kv&) = delete;
    Kv& operator=(const Kv&) = delete;
    virtual ~Kv() = default;

    [[nodiscard]] virtual Task<KvEntryMaybe<>> get(KvKey key, KvGetOptions options = {}) = 0;
    // `kv.get<T>(key)`: the entry with its value converted by kvValueAs<T>.
    template <class T> [[nodiscard]] Task<KvEntryMaybe<T>> get(KvKey key, KvGetOptions options = {});
    [[nodiscard]] virtual Task<std::vector<KvEntryMaybe<>>> getMany(std::vector<KvKey> keys, KvGetOptions options = {}) = 0;
    template <class T> [[nodiscard]] Task<std::vector<KvEntryMaybe<T>>> getMany(std::vector<KvKey> keys, KvGetOptions options = {});
    [[nodiscard]] virtual Task<KvCommitResult> set(KvKey key, KvValue value, KvSetOptions options = {}) = 0;
    [[nodiscard]] virtual Task<void> delete_(KvKey key) = 0;
    // Validates the selector and options now (TypeError, as the TS throws synchronously).
    [[nodiscard]] virtual std::unique_ptr<KvListIterator> list(KvListSelector selector, KvListOptions options = {}) = 0;
    [[nodiscard]] virtual std::unique_ptr<AtomicOperation> atomic() = 0;
    virtual void close() = 0;

    // The JS object identity of this store, for `WeakMap<Deno.Kv, T>` / `WeakSet`
    // keys (rt::WeakMap<rt::Kv, T>) and `===`: a token created with this object and
    // destroyed with it, so entries keyed by it die with the Kv and a Kv constructed
    // later never matches it, even at the same address. Every object is its own
    // identity: a ForwardingKv (the TS `new Proxy(kv, ...)`) is not the Kv it wraps.
    const void* identity() const noexcept { return kvIdentity_.get(); }
    std::weak_ptr<const void> weakIdentity() const noexcept { return kvIdentity_; }

  private:
    std::shared_ptr<const void> kvIdentity_ = std::make_shared<char>();
  };

  template <class T> Task<KvEntryMaybe<T>> Kv::get(KvKey key, KvGetOptions options) {
    KvEntryMaybe<> entry = co_await get(std::move(key), options);
    co_return entry.template as<T>();
  }

  template <class T> Task<std::vector<KvEntryMaybe<T>>> Kv::getMany(std::vector<KvKey> keys, KvGetOptions options) {
    std::vector<KvEntryMaybe<>> entries = co_await getMany(std::move(keys), options);
    std::vector<KvEntryMaybe<T>> converted;
    converted.reserve(entries.size());
    for (const auto& entry : entries) converted.push_back(entry.template as<T>());
    co_return converted;
  }

  // ---- The queue -----------------------------------------------------------------------------

  // `kv.enqueue(value, options)`: a capability beside rt::Kv. paid-fallback enqueues
  // reconciliation jobs; no first-party module listens. A store or test double that has a
  // queue also derives from KvEnqueue, and callers detect it with dynamic_cast where the TS
  // probes `typeof kv.enqueue === "function"`. rt::SqliteKv and the guard over it have it;
  // MemoryKv does not.

  // The @deno/kv `enqueue` options (`{ delay?, keysIfUndelivered?, backoffSchedule? }`). The
  // SDK sends a fixed backoff schedule ([100, 200, 400, 800]) whatever `backoffSchedule` says.
  struct KvEnqueueOptions {
    std::optional<double> delay{};
    std::optional<std::vector<KvKey>> keysIfUndelivered{};
    std::optional<std::vector<double>> backoffSchedule{};
  };

  class KvEnqueue {
  public:
    KvEnqueue() = default;
    KvEnqueue(const KvEnqueue&) = delete;
    KvEnqueue& operator=(const KvEnqueue&) = delete;
    virtual ~KvEnqueue() = default;
    // `kv.enqueue(message, options)`; the caller ignores the result.
    [[nodiscard]] virtual Task<void> enqueue(KvValue message, KvEnqueueOptions options) = 0;
  };

  // ---- Wrappers (the tests' `new Proxy(kv, { get(...) })`) -------------------------------

  // Delegates every call to `inner`. Subclass and override what a test intercepts.
  class ForwardingKv : public Kv {
  public:
    explicit ForwardingKv(Kv& inner_) : inner(inner_) {}
    using Kv::get;
    using Kv::getMany;
    Task<KvEntryMaybe<>> get(KvKey key, KvGetOptions options = {}) override;
    Task<std::vector<KvEntryMaybe<>>> getMany(std::vector<KvKey> keys, KvGetOptions options = {}) override;
    Task<KvCommitResult> set(KvKey key, KvValue value, KvSetOptions options = {}) override;
    Task<void> delete_(KvKey key) override;
    std::unique_ptr<KvListIterator> list(KvListSelector selector, KvListOptions options = {}) override;
    std::unique_ptr<AtomicOperation> atomic() override;
    void close() override;

  protected:
    Kv& inner;
  };

  // Delegates every builder call and commit() to `inner`. Subclass and override
  // what a test observes or alters (call the base to forward).
  class ForwardingAtomicOperation : public AtomicOperation {
  public:
    explicit ForwardingAtomicOperation(std::unique_ptr<AtomicOperation> inner_) : inner(std::move(inner_)) {}
    using AtomicOperation::check;
    using AtomicOperation::max;
    using AtomicOperation::min;
    using AtomicOperation::sum;
    AtomicOperation& check(std::vector<AtomicCheck> items) override;
    AtomicOperation& mutate(std::vector<KvMutation> items) override;
    AtomicOperation& sum(KvKey key, const BigInt& n) override;
    AtomicOperation& min(KvKey key, const BigInt& n) override;
    AtomicOperation& max(KvKey key, const BigInt& n) override;
    AtomicOperation& set(KvKey key, KvValue value, KvSetOptions options = {}) override;
    AtomicOperation& delete_(KvKey key) override;
    Task<KvCommitResult> commit() override;

  protected:
    std::unique_ptr<AtomicOperation> inner;
  };

  // ---- MemoryKv -----------------------------------------------------------------------------

  struct MemoryKvOptions {
    // Commit version step. The native store adds a random 1..9 per commit; empty
    // (the default) adds 1, so versionstamps are deterministic in tests.
    std::function<std::uint64_t()> versionIncrement{};
    // Expired rows stay readable until a sweep deletes them. The native store
    // sweeps when a handle opens and every 60 s after that (a 100 ms TTL row was
    // measured gone after 60.9 s), never on read. 0 sweeps before every operation.
    double sweepIntervalMs = 60'000;
  };

  // The rows and commit version of one store, shared by every MemoryKv handle
  // opened on it: the stand-in for one SQLite file (`openKv(path)` twice gives two
  // handles on the same rows).
  class MemoryKvDatabase {
  public:
    // `now` is Date.now() (epoch milliseconds); expiry and sweeps follow it.
    explicit MemoryKvDatabase(std::function<double()> now, MemoryKvOptions options = {});
    ~MemoryKvDatabase();
    MemoryKvDatabase(const MemoryKvDatabase&) = delete;
    MemoryKvDatabase& operator=(const MemoryKvDatabase&) = delete;

    // A stored row as the SQLite table holds it (expired rows not yet swept included).
    struct Row {
      KvKey key;
      KvValue value;
      // `v_encoding`: 1 serialized value, 2 KvU64, 3 bytes.
      int encoding = 1;
      std::uint64_t version = 0;
      // `expiration_ms`: absolute epoch milliseconds, -1 without expiry.
      double expirationMs = -1;
    };
    std::optional<Row> row(const KvKey& key) const;
    // Every row in key order.
    std::vector<Row> rows() const;
    // `data_version.version`: the latest commit version.
    std::uint64_t version() const noexcept;

    struct State;
    State& state() noexcept { return *state_; }

  private:
    std::unique_ptr<State> state_;
  };

  // One open handle (`await openKv(path)`) with the guarded native store's
  // semantics: Deno KV order and tuple keys, the limits and their errors (10
  // checks, 1000 mutations, 2048-byte write keys, 2049-byte read keys, 10 getMany
  // keys, 65536-byte values, 819200-byte commits), all-or-nothing commits (a
  // sum/min/max on a non-KvU64 row throws TypeError "Atomic arithmetic requires an
  // absent key or KvU64 value" and rolls back), versionstamps, the SDK's batched
  // list with its cursors, sweep-based expiry and close. An operation does its work
  // when it starts and completes one macrotask later (a posted handler), as the
  // native addon's promise does; SDK-side validation errors throw at once.
  class MemoryKv final : public Kv {
  public:
    // A private database (`openKv(":memory:")`).
    explicit MemoryKv(std::function<double()> now, MemoryKvOptions options = {});
    // Another handle on `database` (`openKv(path)` again); it sweeps expired rows now.
    explicit MemoryKv(std::shared_ptr<MemoryKvDatabase> database);
    ~MemoryKv() override;

    using Kv::get;
    using Kv::getMany;
    Task<KvEntryMaybe<>> get(KvKey key, KvGetOptions options = {}) override;
    Task<std::vector<KvEntryMaybe<>>> getMany(std::vector<KvKey> keys, KvGetOptions options = {}) override;
    Task<KvCommitResult> set(KvKey key, KvValue value, KvSetOptions options = {}) override;
    Task<void> delete_(KvKey key) override;
    std::unique_ptr<KvListIterator> list(KvListSelector selector, KvListOptions options = {}) override;
    std::unique_ptr<AtomicOperation> atomic() override;
    // Error "Cannot call '.close' after '.close' is called" the second time.
    void close() override;

    const std::shared_ptr<MemoryKvDatabase>& database() const noexcept { return database_; }

    struct Handle;

  private:
    std::shared_ptr<MemoryKvDatabase> database_;
    std::shared_ptr<Handle> handle_;
  };
}
