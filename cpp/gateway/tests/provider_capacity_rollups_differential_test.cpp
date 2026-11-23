// Differential cases for src/provider/capacity-rollups.ts (its own test file drives the module mostly
// through unported capacity.ts): bucket starts, keys, isProviderCapacityRollupPoint over stored values,
// mergeProviderCapacityRollup chains, and listProviderCapacityRollups over a scripted Kv that records
// its list call. Expected values were recorded from Bun 1.4.0
// (tests/provider_capacity_rollups_differential_vectors.inc).
// Links: tests/provider_capacity_rollups_differential_test.cpp src/provider/capacity_rollups.cpp
#include "uos/provider/capacity_rollups.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::provider::capacity_rollups;

#include "provider_capacity_rollups_differential_vectors.inc"

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(kProviderCapacityRollupsVectors).as_object();
    return parsed;
  }

  // Special numbers are tagged {"$num": text}.
  json::value untag(const json::value& value) {
    if (const json::array* items = value.if_array()) {
      json::array out;
      for (const json::value& item : *items) out.push_back(untag(item));
      return out;
    }
    if (const json::object* object = value.if_object()) {
      if (object->size() == 1) {
        if (const json::value* number = rt::get(*object, "$num"); number && number->is_string()) return rt::makeNumber(rt::stringToNumber(std::string(number->get_string())));
      }
      json::object out;
      for (const auto& member : *object) out[member.key()] = untag(member.value());
      return out;
    }
    return value;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) out.push_back(json::value(part.string()));
      else out.push_back(rt::makeNumber(part.number()));
    }
    return out;
  }

  std::string label(const json::value& row) { return std::string(row.as_array().at(0).as_string()); }

  class ScriptedIterator final : public rt::KvListIterator {
  public:
    explicit ScriptedIterator(std::vector<rt::KvEntry<>> entries) : entries_(std::move(entries)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (index_ >= entries_.size()) co_return std::nullopt;
      const std::size_t current = index_;
      index_ += 1;
      co_return entries_[current];
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { co_return; }

  private:
    std::vector<rt::KvEntry<>> entries_;
    std::size_t index_ = 0;
  };

  // `{ list(selector, options) { calls.push({ selector, options }); return entries(); } }`
  class ScriptedKv final : public rt::ForwardingKv {
  public:
    explicit ScriptedKv(std::vector<rt::KvValue> values) : rt::ForwardingKv(store_), values_(std::move(values)) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options) override {
      calls.emplace_back(selector, options);
      std::vector<rt::KvEntry<>> entries;
      for (std::size_t index = 0; index < values_.size(); ++index) entries.push_back(rt::KvEntry<>{rt::KvKey{"scripted", index}, values_[index], "00000000000000010000"});
      return std::make_unique<ScriptedIterator>(std::move(entries));
    }
    std::vector<std::pair<rt::KvListSelector, rt::KvListOptions>> calls;

  private:
    rt::MemoryKv store_{[] { return 0.0; }};
    std::vector<rt::KvValue> values_;
  };

  json::value callJson(const rt::KvListSelector& selector, const rt::KvListOptions& options) {
    json::object recordedSelector;
    if (selector.prefix) recordedSelector["prefix"] = keyJson(*selector.prefix);
    if (selector.start) recordedSelector["start"] = keyJson(*selector.start);
    if (selector.end) recordedSelector["end"] = keyJson(*selector.end);
    json::object recordedOptions;
    if (options.limit) recordedOptions["limit"] = rt::makeNumber(*options.limit);
    if (options.cursor) recordedOptions["cursor"] = *options.cursor;
    if (options.reverse) recordedOptions["reverse"] = true;
    if (options.batchSize) recordedOptions["batchSize"] = rt::makeNumber(*options.batchSize);
    json::object call;
    call["selector"] = std::move(recordedSelector);
    call["options"] = std::move(recordedOptions);
    return call;
  }
}

TEST("differential: the recorded vectors are complete") {
  EXPECT_EQ(vectors().at("bucket").as_array().size(), std::size_t{14});
  EXPECT_EQ(vectors().at("keys").as_array().size(), std::size_t{4});
  EXPECT_EQ(vectors().at("guard").as_array().size(), std::size_t{62});
  EXPECT_EQ(vectors().at("merge").as_array().size(), std::size_t{9});
  EXPECT_EQ(vectors().at("list").as_array().size(), std::size_t{9});
}

TEST("differential: providerCapacityRollupBucketStartAtMs") {
  for (const json::value& row : vectors().at("bucket").as_array()) {
    const double input = rt::getNumber(untag(row.as_array().at(0))).value();
    const double expected = rt::getNumber(untag(row.as_array().at(1))).value();
    const double actual = providerCapacityRollupBucketStartAtMs(input);
    if (!rt::sameValue(actual, expected)) EXPECT_EQ(rt::numberToString(actual), rt::numberToString(expected));
  }
}

TEST("differential: providerCapacityRollupKey") {
  for (const json::value& row : vectors().at("keys").as_array()) {
    const double input = rt::getNumber(untag(row.as_array().at(0))).value();
    const json::value actual = keyJson(providerCapacityRollupKey(input));
    const json::value expected = untag(row.as_array().at(1));
    if (!rt::deepEqual(actual, expected)) EXPECT_EQ(rt::jsonStringify(actual), rt::jsonStringify(expected));
  }
  EXPECT(PROVIDER_CAPACITY_ROLLUP_PREFIX == (rt::KvKey{"uos_ai", "provider_capacity", "v1", "rollup"}));
  EXPECT_EQ(PROVIDER_CAPACITY_ROLLUP_BUCKET_MS, 3'600'000.0);
  EXPECT_EQ(PROVIDER_CAPACITY_ROLLUP_READ_LIMIT, 20'000.0);
}

TEST("differential: isProviderCapacityRollupPoint over stored values") {
  for (const json::value& row : vectors().at("guard").as_array()) {
    const rt::KvValue stored(untag(row.as_array().at(1)));
    const bool expected = row.as_array().at(2).as_bool();
    const bool actual = isProviderCapacityRollupPoint(stored);
    if (actual != expected) EXPECT_EQ(label(row) + (actual ? " accepted" : " rejected"), label(row) + (expected ? " accepted" : " rejected"));
    // The narrowing form agrees with the guard and reads back the declared members.
    const std::optional<ProviderCapacityRollupPoint> narrowed = providerCapacityRollupPointOf(stored);
    EXPECT_EQ(narrowed.has_value(), expected);
    if (narrowed && label(row) != "extra members") {
      const json::value written = json::value_from(*narrowed);
      if (!rt::deepEqual(written, untag(row.as_array().at(1)))) EXPECT_EQ(label(row) + " " + rt::jsonStringify(written), label(row) + " " + rt::jsonStringify(untag(row.as_array().at(1))));
    }
  }
}

TEST("differential: mergeProviderCapacityRollup") {
  for (const json::value& row : vectors().at("merge").as_array()) {
    const json::value existingJson = untag(row.as_array().at(1));
    const std::optional<ProviderCapacityRollupPoint> existing =
        existingJson.is_null() ? std::nullopt : std::optional<ProviderCapacityRollupPoint>(json::value_to<ProviderCapacityRollupPoint>(existingJson));
    const ProviderCapacityRollupInput input = json::value_to<ProviderCapacityRollupInput>(untag(row.as_array().at(2)));
    const json::value actual = json::value_from(mergeProviderCapacityRollup(existing, input));
    const json::value expected = untag(row.as_array().at(3));
    if (!rt::deepEqual(actual, expected)) EXPECT_EQ(label(row) + " " + rt::jsonStringify(actual), label(row) + " " + rt::jsonStringify(expected));
    // Records round-trip through their JSON pair (NaN members compare with Object.is).
    EXPECT(rt::deepEqual(json::value_from(json::value_to<ProviderCapacityRollupInput>(json::value_from(input))), untag(row.as_array().at(2))));
  }
}

ASYNC_TEST("differential: listProviderCapacityRollups bounds, filters and sorts the range scan") {
  for (const json::value& row : vectors().at("list").as_array()) {
    const json::value options = untag(row.as_array().at(1));
    const json::value scripted = untag(row.as_array().at(2));
    std::vector<rt::KvValue> values;
    for (const json::value& value : scripted.as_array()) values.emplace_back(value);
    const auto kv = std::make_shared<ScriptedKv>(std::move(values));
    ListProviderCapacityRollupsOptions listOptions{.sinceMs = rt::getNumber(options, "sinceMs").value(), .nowMs = rt::getNumber(options, "nowMs").value()};
    listOptions.limit = rt::getNumber(options, "limit");
    auto listing = listProviderCapacityRollups(kv, listOptions);
    const std::vector<ProviderCapacityRollupPoint> points = co_await std::move(listing);
    json::array calls;
    for (const auto& [selector, recordedOptions] : kv->calls) calls.push_back(callJson(selector, recordedOptions));
    const json::value expectedCalls = untag(row.as_array().at(3));
    if (!rt::deepEqual(json::value(calls), expectedCalls)) EXPECT_EQ(label(row) + " " + rt::jsonStringify(json::value(calls)), label(row) + " " + rt::jsonStringify(expectedCalls));
    json::array written;
    for (const ProviderCapacityRollupPoint& point : points) written.push_back(json::value_from(point));
    const json::value expectedPoints = untag(row.as_array().at(4));
    if (!rt::deepEqual(json::value(written), expectedPoints)) EXPECT_EQ(label(row) + " " + rt::jsonStringify(json::value(written)), label(row) + " " + rt::jsonStringify(expectedPoints));
  }
}

TEST("record pairs reject malformed slots tuples") {
  json::value point = json::value_from(ProviderCapacityRollupPoint{
    .v = 1,
    .bucket_start_at_ms = 0,
    .sample_count = 0,
    .first_sampled_at_ms = 0,
    .last_sampled_at_ms = 0,
    .slots = {ProviderCapacityRollupSlot{.slot = 1,
                                         .available_sample_count = 0,
                                         .last_state = "unavailable",
                                         .primary = {.samples = 0, .limit_window_seconds = std::nullopt, .min_used_percent = std::nullopt, .max_used_percent = std::nullopt, .last_used_percent = std::nullopt, .last_reset_at_ms = std::nullopt},
                                         .secondary = {.samples = 0, .limit_window_seconds = std::nullopt, .min_used_percent = std::nullopt, .max_used_percent = std::nullopt, .last_used_percent = std::nullopt, .last_reset_at_ms = std::nullopt}},
              ProviderCapacityRollupSlot{.slot = 2,
                                         .available_sample_count = 0,
                                         .last_state = "unavailable",
                                         .primary = {.samples = 0, .limit_window_seconds = std::nullopt, .min_used_percent = std::nullopt, .max_used_percent = std::nullopt, .last_used_percent = std::nullopt, .last_reset_at_ms = std::nullopt},
                                         .secondary = {.samples = 0, .limit_window_seconds = std::nullopt, .min_used_percent = std::nullopt, .max_used_percent = std::nullopt, .last_used_percent = std::nullopt, .last_reset_at_ms = std::nullopt}}},
  });
  EXPECT(json::value_to<ProviderCapacityRollupPoint>(point).slots[1].slot == 2);
  point.as_object()["slots"].as_array().pop_back();
  EXPECT_THROWS_MESSAGE(json::value_to<ProviderCapacityRollupPoint>(point), rt::TypeError, "ProviderCapacityRollupPoint.slots is array, not [object, object]");
}
