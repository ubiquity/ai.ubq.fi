#include "tests/helpers/sentinel_kv_stub.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace uos::tests::helpers::sentinel_kv_stub {
  namespace {
    constexpr const char* UNSUPPORTED = "Sentinel in-memory KV does not support this operation";

    rt::KvValue clone(const rt::KvValue& value) { return rt::kvStructuredClone(value); }

    // `Promise.resolve(result)`: settles one microtask after the call did its work.
    template <class T> rt::Task<T> resolved(T result) {
      co_await rt::microtask();
      co_return result;
    }
    rt::Task<void> resolvedVoid() { co_await rt::microtask(); }

    json::value keyPartValue(const rt::KvKeyPart& part) {
      if (part.isBytes()) {
        json::array bytes;
        for (const std::uint8_t byte : part.bytes()) bytes.push_back(rt::makeNumber(static_cast<std::int64_t>(byte)));
        json::array tagged;
        tagged.push_back("bytes");
        tagged.push_back(std::move(bytes));
        return tagged;
      }
      if (part.isBigInt()) {
        json::array tagged;
        tagged.push_back("bigint");
        tagged.push_back(json::string(part.bigint().toString()));
        return tagged;
      }
      if (part.isString()) return json::string(part.string());
      if (part.isNumber()) return rt::Number::isFinite(part.number()) ? rt::makeNumber(part.number()) : json::value(nullptr);
      return json::value(part.boolean());
    }

    std::string keyText(const rt::KvKey& key) {
      json::array parts;
      for (const rt::KvKeyPart& part : key) parts.push_back(keyPartValue(part));
      return rt::jsonStringify(json::value(std::move(parts)));
    }

    // `String(value)` of a value a decoded cursor holds (Array.prototype.join for arrays).
    std::string stringOf(const json::value& value) {
      if (value.is_string()) return std::string(value.get_string());
      if (value.is_bool()) return value.get_bool() ? "true" : "false";
      if (value.is_null()) return "null";
      if (rt::isNumber(value)) return rt::numberToString(rt::toNumber(value));
      if (value.is_array()) {
        std::string out;
        const json::array& items = value.get_array();
        for (std::size_t index = 0; index < items.size(); ++index) {
          if (index > 0) out += ",";
          if (!items[index].is_null()) rt::append(out, stringOf(items[index]));
        }
        return out;
      }
      return "[object Object]";
    }

    /** Deno KV orders key parts by type first: bytes, string, number, bigint, boolean. */
    // A part as the comparisons read it: a stored key part, or any value of a decoded cursor key.
    struct PartView {
      int rank;
      double number = 0;
      std::optional<rt::BigInt> bigint{};
      // `keyPartText(part)`: byte-array parts compare on their comma-joined text.
      std::string text{};
    };

    PartView viewOf(const rt::KvKeyPart& part) {
      if (part.isBytes()) {
        std::string text;
        const rt::KvBytes& bytes = part.bytes();
        for (std::size_t index = 0; index < bytes.size(); ++index) {
          if (index > 0) text += ",";
          text += rt::numberToString(bytes[index]);
        }
        return {.rank = 0, .text = std::move(text)};
      }
      if (part.isString()) return {.rank = 1, .text = part.string()};
      if (part.isNumber()) return {.rank = 2, .number = part.number(), .text = rt::numberToString(part.number())};
      if (part.isBigInt()) return {.rank = 3, .bigint = part.bigint(), .text = part.bigint().toString()};
      return {.rank = 4, .text = part.boolean() ? "true" : "false"};
    }

    PartView viewOf(const json::value& decoded) {
      if (decoded.is_string()) return {.rank = 1, .text = std::string(decoded.get_string())};
      if (rt::isNumber(decoded)) return {.rank = 2, .number = rt::toNumber(decoded), .text = stringOf(decoded)};
      return {.rank = 4, .text = stringOf(decoded)};
    }

    double compareOrdered(const std::string& left, const std::string& right) {
      if (left == right) return 0;
      return rt::lessThan(left, right) ? -1 : 1;
    }

    double compareParts(const PartView& left, const PartView& right) {
      const double rank = left.rank - right.rank;
      if (rank != 0) return rank;
      if (left.rank == 2) return left.number - right.number;
      if (left.bigint && right.bigint) {
        if (*left.bigint == *right.bigint) return 0;
        return *left.bigint < *right.bigint ? -1 : 1;
      }
      return compareOrdered(left.text, right.text);
    }

    // A key to compare against: stored parts, or the parts of a decoded cursor (any JSON value).
    struct KeyView {
      std::vector<PartView> parts;
      // `key.length`: NaN for a decoded cursor that is neither an array nor a string.
      double length;
    };

    KeyView viewOf(const rt::KvKey& key) {
      KeyView view{.parts = {}, .length = static_cast<double>(key.size())};
      for (const rt::KvKeyPart& part : key) view.parts.push_back(viewOf(part));
      return view;
    }

    KeyView viewOfDecoded(const json::value& decoded) {
      KeyView view{.parts = {}, .length = std::numeric_limits<double>::quiet_NaN()};
      if (const json::array* items = decoded.if_array()) {
        for (const json::value& item : *items) view.parts.push_back(viewOf(item));
        view.length = static_cast<double>(items->size());
      } else if (decoded.is_string()) {
        // `after[index]` of a string is its code unit at `index`.
        const std::string text(decoded.get_string());
        const std::int64_t length = rt::length(text);
        for (std::int64_t index = 0; index < length; ++index) view.parts.push_back(PartView{.rank = 1, .text = rt::charAt(text, index)});
        view.length = static_cast<double>(length);
      }
      return view;
    }

    double compareKeys(const KeyView& left, const KeyView& right) {
      const double shared = rt::Math::min(left.length, right.length);
      for (std::size_t index = 0; static_cast<double>(index) < shared; ++index) {
        const double comparison = compareParts(left.parts[index], right.parts[index]);
        if (comparison != 0) return comparison;
      }
      return left.length - right.length;
    }

    bool matchesPrefix(const KeyView& key, const KeyView& prefix) {
      if (prefix.length > key.length) return false;
      for (std::size_t index = 0; index < prefix.parts.size(); ++index) {
        if (compareParts(prefix.parts[index], key.parts[index]) != 0) return false;
      }
      return true;
    }

    // Base64 padding only ever trails, so removing every "=" is the same operation
    // as stripping the 1-2 character pad, without a backtracking pattern.
    std::string encodeCursor(const rt::KvKey& after) {
      return rt::replaceAll(rt::replaceAll(rt::replaceAll(rt::btoa(keyText(after)), "+", "-"), "/", "_"), "=", "");
    }
    json::value decodeCursor(const std::string& cursor) { return rt::jsonParse(rt::atob(rt::replaceAll(rt::replaceAll(cursor, "-", "+"), "_", "/"))); }

    /** One iteration over a frozen snapshot of the matching keys, with a resumable cursor. */
    class SentinelKvListIterator final : public rt::KvListIterator {
    public:
      explicit SentinelKvListIterator(std::vector<StoredEntry> entries) : entries_(std::move(entries)) {}
      std::string cursor() const override {
        if (index_ == 0) throw rt::JsError("Cannot get cursor before first iteration");
        return cursor_;
      }
      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        const std::size_t at = index_;
        index_ += 1;
        if (at >= entries_.size()) {
          cursor_ = "";
          return resolved<std::optional<rt::KvEntry<>>>(std::nullopt);
        }
        const StoredEntry& entry = entries_[at];
        cursor_ = encodeCursor(entry.key);
        return resolved<std::optional<rt::KvEntry<>>>(rt::KvEntry<>{.key = entry.key, .value = entry.value, .versionstamp = entry.versionstamp});
      }
      // The TS iterator has no `return` method: `for await ... break` calls nothing.
      rt::Task<void> return_() override { co_return; }

    private:
      std::vector<StoredEntry> entries_;
      std::size_t index_ = 0;
      std::string cursor_;
    };

    class SentinelAtomicOperation final : public rt::AtomicOperation {
    public:
      explicit SentinelAtomicOperation(SentinelKvStorage& storage) : storage_(storage) {}
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        checks_.insert(checks_.end(), items.begin(), items.end());
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation> items) override {
        mutations_.insert(mutations_.end(), items.begin(), items.end());
        return *this;
      }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::JsError(UNSUPPORTED); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::JsError(UNSUPPORTED); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::JsError(UNSUPPORTED); }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
        rt::KvMutation mutation;
        mutation.key = std::move(key);
        mutation.type = rt::KvMutationType::set;
        mutation.value = std::move(value);
        mutations_.push_back(std::move(mutation));
        return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override {
        rt::KvMutation mutation;
        mutation.key = std::move(key);
        mutation.type = rt::KvMutationType::delete_;
        mutations_.push_back(std::move(mutation));
        return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override {
        for (const rt::AtomicCheck& check : checks_) {
          const StoredEntry* stored = storage_.entries.get(keyText(check.key));
          const std::optional<std::string> current = stored ? std::optional<std::string>(stored->versionstamp) : std::nullopt;
          if (current != check.versionstamp) return resolved(rt::KvCommitResult{.ok = false, .versionstamp = ""});
        }
        const std::string versionstamp = storage_.nextVersionstamp();
        for (const rt::KvMutation& mutation : mutations_) {
          if (mutation.type == rt::KvMutationType::delete_) storage_.entries.delete_(keyText(mutation.key));
          else if (mutation.type == rt::KvMutationType::set)
            storage_.entries.set(keyText(mutation.key), StoredEntry{.key = mutation.key, .value = clone(mutation.value), .versionstamp = versionstamp});
          else throw rt::JsError(UNSUPPORTED);
        }
        return resolved(rt::KvCommitResult{.ok = true, .versionstamp = versionstamp});
      }

    private:
      SentinelKvStorage& storage_;
      std::vector<rt::AtomicCheck> checks_;
      std::vector<rt::KvMutation> mutations_;
    };

    rt::KvEntryMaybe<> readEntry(const SentinelKvStorage& storage, const rt::KvKey& key) {
      const StoredEntry* entry = storage.entries.get(keyText(key));
      if (!entry) return rt::KvEntryMaybe<>{.key = key, .value = rt::KvValue(nullptr), .versionstamp = std::nullopt};
      return rt::KvEntryMaybe<>{.key = key, .value = clone(entry->value), .versionstamp = entry->versionstamp};
    }

    // `ordered.slice(0, limit)`
    std::size_t sliceEnd(double limit, std::size_t length) {
      const double end = rt::Math::trunc(rt::Number::isNaN(limit) ? 0 : limit);
      const double size = static_cast<double>(length);
      const double bounded = end < 0 ? rt::Math::max(size + end, 0) : rt::Math::min(end, size);
      return static_cast<std::size_t>(bounded);
    }
  }

  SentinelKvStub::SentinelKvStub() { storage.nextVersionstamp = [this] { return nextVersionstamp(); }; }

  std::string SentinelKvStub::nextVersionstamp() {
    sequence_ += 1;
    return rt::padStart(rt::numberToString(sequence_), 20, "0");
  }

  rt::Task<rt::KvEntryMaybe<>> SentinelKvStub::get(rt::KvKey key, rt::KvGetOptions) { return resolved(readEntry(storage, key)); }

  rt::Task<std::vector<rt::KvEntryMaybe<>>> SentinelKvStub::getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions) {
    std::vector<rt::KvEntryMaybe<>> entries;
    entries.reserve(keys.size());
    for (const rt::KvKey& key : keys) entries.push_back(readEntry(storage, key));
    return resolved(std::move(entries));
  }

  rt::Task<rt::KvCommitResult> SentinelKvStub::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) {
    const std::string versionstamp = nextVersionstamp();
    const std::string text = keyText(key);
    storage.entries.set(text, StoredEntry{.key = std::move(key), .value = clone(value), .versionstamp = versionstamp});
    return resolved(rt::KvCommitResult{.ok = true, .versionstamp = versionstamp});
  }

  rt::Task<void> SentinelKvStub::delete_(rt::KvKey key) {
    storage.entries.delete_(keyText(key));
    return resolvedVoid();
  }

  std::unique_ptr<rt::KvListIterator> SentinelKvStub::list(rt::KvListSelector selector, rt::KvListOptions options) {
    const KeyView prefix = viewOf(selector.prefix.value_or(rt::KvKey()));
    const std::optional<KeyView> start = selector.start ? std::optional<KeyView>(viewOf(*selector.start)) : std::nullopt;
    const std::optional<KeyView> end = selector.end ? std::optional<KeyView>(viewOf(*selector.end)) : std::nullopt;
    const std::optional<KeyView> after =
      options.cursor && !options.cursor->empty() ? std::optional<KeyView>(viewOfDecoded(decodeCursor(*options.cursor))) : std::nullopt;
    const double limit = options.limit.value_or(100);
    std::vector<StoredEntry> selected;
    for (const auto& [text, entry] : storage.entries) {
      const KeyView key = viewOf(entry.key);
      if (!matchesPrefix(key, prefix)) continue;
      if (start && !(compareKeys(key, *start) >= 0)) continue;
      if (end && !(compareKeys(key, *end) < 0)) continue;
      if (after && !(compareKeys(key, *after) > 0)) continue;
      selected.push_back(entry);
    }
    rt::sort(selected, [](const StoredEntry& left, const StoredEntry& right) { return compareKeys(viewOf(left.key), viewOf(right.key)); });
    if (options.reverse) std::reverse(selected.begin(), selected.end());
    std::vector<StoredEntry> page;
    const std::size_t count = sliceEnd(limit, selected.size());
    for (std::size_t index = 0; index < count; ++index) {
      page.push_back(StoredEntry{.key = selected[index].key, .value = clone(selected[index].value), .versionstamp = selected[index].versionstamp});
    }
    return std::make_unique<SentinelKvListIterator>(std::move(page));
  }

  std::unique_ptr<rt::AtomicOperation> SentinelKvStub::atomic() { return std::make_unique<SentinelAtomicOperation>(storage); }

  void SentinelKvStub::close() { storage.entries.clear(); }

  bool hasRuntimeKv() { return true; }

  rt::Task<std::shared_ptr<rt::Kv>> openSentinelTestKv() {
    std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    co_return kv;
  }
}
