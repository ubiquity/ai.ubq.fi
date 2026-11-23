#pragma once
// Shared helpers of the b4 capacity differential tests (provider_capacity_analytics_differential_test.cpp and
// provider_capacity_history_events_differential_test.cpp): the recorded vectors, JSON <-> record conversions and the
// fake stores standing in for the generator's `{ list }` / `{ atomic }` stubs.
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::tests::capacity_differential {
  namespace contract = provider::capacity_contract;

  inline const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(
#include "tests/provider_capacity_modules_differential_vectors.inc"
                                         )
                                         .as_object();
    return parsed;
  }

  inline void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected) || rt::jsonStringify(actual) != rt::jsonStringify(expected))
      rt::test::report(file, line, label + "\n      actual:   " + rt::jsonStringify(actual) + "\n      expected: " + rt::jsonStringify(expected), false);
  }
#define EXPECT_JSON(actual, expected, label) ::uos::tests::capacity_differential::expectJson((actual), (expected), (label), __FILE__, __LINE__)

  inline bool isUndefinedTag(const json::value& value) {
    const json::object* object = value.if_object();
    return object && object->size() == 1 && object->contains("$undefined");
  }

  // A tagged input as a stored value (undefined members stay present-but-undefined).
  inline rt::KvValue storedOf(const json::value& value) {
    if (isUndefinedTag(value)) return rt::KvValue();
    if (const json::array* array = value.if_array()) {
      rt::KvArray out;
      for (const json::value& item : *array) out.push_back(storedOf(item));
      return rt::KvValue(std::move(out));
    }
    if (const json::object* object = value.if_object()) {
      rt::KvObject out;
      for (const auto& [key, item] : *object) out.set(std::string(key), storedOf(item));
      return rt::KvValue(std::move(out));
    }
    return rt::KvValue(value);
  }

  template <class T> json::value plain(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
  template <class T> json::value plain(const std::vector<T>& values) { return json::value_from(values); }

  template <class T> T recordOf(const json::value& value) { return json::value_to<T>(value); }
  template <class T> std::vector<T> recordsOf(const json::value& value) {
    std::vector<T> out;
    for (const json::value& item : value.as_array()) out.push_back(json::value_to<T>(item));
    return out;
  }
  template <class T> std::optional<T> optionalRecordOf(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return json::value_to<T>(value);
  }

  inline json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) out.emplace_back(json::string(part.string()));
      else out.push_back(rt::makeNumber(part.number()));
    }
    return out;
  }

  inline rt::KvKey keyOf(const json::value& value) {
    rt::KvKey key;
    for (const json::value& part : value.as_array()) {
      if (part.is_string()) key.emplace_back(std::string(part.get_string()));
      else key.emplace_back(rt::toNumber(part));
    }
    return key;
  }

  // The generator's `[...kv.rows.values()]` under `prefix` (or every row when `filter` accepts it), sorted by the key's
  // JSON text.
  inline rt::Task<json::value> rowsOf(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix, std::function<bool(const rt::KvKey&)> filter) {
    const rt::KvListSelector selector{.prefix = std::move(prefix)};
    auto iterator = kv->list(selector);
    std::vector<std::pair<std::string, json::value>> rows;
    while (auto entry = co_await iterator->next()) {
      if (filter && !filter(entry->key)) continue;
      json::object row;
      row["key"] = keyJson(entry->key);
      row["value"] = entry->value.toJson();
      rows.emplace_back(rt::jsonStringify(row["key"]), std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    json::array out;
    for (auto& [text, row] : rows) out.push_back(std::move(row));
    co_return out;
  }

  inline std::shared_ptr<rt::Kv> memoryKv() {
    return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  }

  // `{ list: () => { throw ... } }` / an iterator whose `next()` rejects.
  class ThrowingListIterator final : public rt::KvListIterator {
  public:
    rt::Task<std::optional<rt::KvEntry<>>> next() override { throw rt::JsError("list down"); }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { co_return; }
  };

  // Owns the store a ForwardingKv wraps; a base so it is constructed before the ForwardingKv base.
  struct OwnedInnerKv {
    std::shared_ptr<rt::Kv> owned = memoryKv();
  };

  class FailingListKv final : private OwnedInnerKv, public rt::ForwardingKv {
  public:
    explicit FailingListKv(bool throwOnCall_) : OwnedInnerKv(), rt::ForwardingKv(*owned), throwOnCall(throwOnCall_) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override {
      if (throwOnCall) throw rt::JsError("list down");
      return std::make_unique<ThrowingListIterator>();
    }

  private:
    bool throwOnCall;
  };
}
