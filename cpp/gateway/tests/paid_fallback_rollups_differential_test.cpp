// Differential cases for src/paid-fallback/rollups.ts, recorded under Bun 1.4.0
// (paid_fallback_rollups_differential_vectors.inc), beside its five direct cases in
// tests/quota_projection_test.cpp: the `{ start, end }` selector listPaidFallbackUsageRollups builds
// for odd windows (NaN and infinities written as "#NaN" / "#Infinity"), which stored rows the guard
// keeps (exotic values only a structured clone holds included; the C++ record drops a kept row's
// undeclared members, as PORT-NOTES records), a merge over a stored row holding a non-JSON member,
// and terminal recordings (trimmed identity, bucket flooring, token guards; clock-dependent fields
// masked). Bun's stubs answered every key; here the rows sit at the keys the module reads.
// Links the sources of tests/quota_projection_test.cpp but ledger_stats.cpp and api_keys.cpp.
#include "uos/paid_fallback/rollups.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::paid_fallback::rollups;

namespace {
#include "paid_fallback_rollups_differential_vectors.inc"

  constexpr double HOUR_MS = 60.0 * 60 * 1'000;
  constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
  constexpr double INF = std::numeric_limits<double>::infinity();

  double numberOf(const json::value& text) { return rt::stringToNumber(std::string_view(text.as_string())); }

  // A key as Bun's JSON.stringify replacer printed it: non-finite numbers as "#NaN" / "#Infinity".
  std::string keyText(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.push_back(json::string(part.string()));
      else if (part.isNumber()) parts.push_back(rt::Number::isFinite(part.number()) ? rt::makeNumber(part.number()) : json::value(json::string("#" + rt::numberToString(part.number()))));
      else parts.push_back(json::string(part.toString()));
    }
    return rt::jsonStringify(json::value(parts));
  }

  class EmptyIterator final : public rt::KvListIterator {
  public:
    rt::Task<std::optional<rt::KvEntry<>>> next() override { co_return std::nullopt; }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { co_return; }
  };

  // Records the selector and lists nothing.
  class SelectorKv final : public rt::Kv {
  public:
    std::optional<rt::KvListSelector> selector;
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions) override { throw std::logic_error("unused"); }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override { throw std::logic_error("unused"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw std::logic_error("unused"); }
    rt::Task<void> delete_(rt::KvKey) override { throw std::logic_error("unused"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector chosen, rt::KvListOptions) override {
      selector = std::move(chosen);
      return std::make_unique<EmptyIterator>();
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override { throw std::logic_error("unused"); }
    void close() override {}
  };

  std::shared_ptr<rt::MemoryKv> memoryKv() { return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); }

  rt::Task<void> setRow(std::shared_ptr<rt::Kv> kv, rt::KvKey key, rt::KvValue value) {
    auto writing = kv->set(std::move(key), std::move(value));
    co_await std::move(writing);
  }

  rt::Task<std::vector<PaidFallbackUsageRollup>> listRollups(std::shared_ptr<rt::Kv> kv, double sinceMs, double nowMs) {
    const ListPaidFallbackUsageRollupsOptions options{.sinceMs = sinceMs, .nowMs = nowMs};
    auto listing = listPaidFallbackUsageRollups(std::move(kv), options);
    std::vector<PaidFallbackUsageRollup> listed = co_await std::move(listing);
    co_return listed;
  }

  rt::Task<bool> recordObservation(TerminalUsageRollupInput input, std::shared_ptr<rt::Kv> kv) {
    auto recording = recordTerminalUsageRollup(std::move(input), std::move(kv));
    const bool recorded = co_await std::move(recording);
    co_return recorded;
  }

  // The rollup record members, in declaration order (what the C++ record keeps of a stored row).
  json::value knownMembers(const json::value& row) {
    json::object kept;
    for (const char* key : {"v", "bucket_start_at_ms", "model", "provider", "request_count", "quota_sum", "input_tokens", "cached_input_tokens", "output_tokens",
                            "spend_microcredits", "first_request_at_ms", "last_request_at_ms", "updated_at_ms"})
      if (const json::value* member = rt::get(row, key)) kept[key] = *member;
    return kept;
  }

  rt::KvValue valid() {
    return rt::KvValue(rt::jsonParse(R"({"v":1,"bucket_start_at_ms":3600000,"model":"m","provider":"p","request_count":1,"quota_sum":0,"input_tokens":1,)"
                                     R"("cached_input_tokens":0,"output_tokens":1,"spend_microcredits":0,"first_request_at_ms":1,"last_request_at_ms":1,"updated_at_ms":1})"));
  }

  rt::KvValue with(std::string_view key, rt::KvValue member) {
    rt::KvValue row = valid();
    row.asObject().set(std::string(key), std::move(member));
    return row;
  }
}

ASYNC_TEST("listing selectors floor the window start to its hour as Bun does") {
  const json::value vectors = rt::jsonParse(ROLLUPS_VECTORS);
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("ranges").as_array()) {
    auto kv = std::make_shared<SelectorKv>();
    auto listing = listRollups(kv, numberOf(vector.at("sinceMs")), numberOf(vector.at("nowMs")));
    co_await std::move(listing);
    ASSERT(kv->selector.has_value() && kv->selector->start && kv->selector->end && !kv->selector->prefix);
    EXPECT_EQ(keyText(*kv->selector->start), rt::jsonStringify(vector.at("selector").at("start")));
    EXPECT_EQ(keyText(*kv->selector->end), rt::jsonStringify(vector.at("selector").at("end")));
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{9});
  auto none = listRollups(nullptr, 0, 0);
  const std::vector<PaidFallbackUsageRollup> withoutKv = co_await std::move(none);
  EXPECT(withoutKv.empty());
}

ASYNC_TEST("the rollup guard keeps the stored rows Bun keeps") {
  const json::value vectors = rt::jsonParse(ROLLUPS_VECTORS);
  const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
  rt::KvMap map;
  map.set(rt::KvValue("v"), rt::KvValue(1));
  const std::vector<std::pair<std::string, rt::KvValue>> rows{
    {"valid", valid()},
    {"extra Date", with("extra", rt::KvValue(rt::KvDate{5}))},
    {"extra undefined", with("extra", rt::KvValue())},
    {"bigint tokens", with("input_tokens", rt::KvValue(rt::BigInt(1)))},
    {"u64 v", with("v", rt::KvValue(rt::KvU64(1)))},
    {"map", rt::KvValue(map)},
    {"array", rt::KvValue(rt::KvArray{valid()})},
    {"string", rt::KvValue("row")},
    {"null quota", with("quota_sum", rt::KvValue(nullptr))},
    {"negative count", with("request_count", rt::KvValue(-1))},
    {"fractional quota", with("quota_sum", rt::KvValue(0.5))},
    {"infinite spend", with("spend_microcredits", rt::KvValue(INF))},
    {"empty model", with("model", rt::KvValue(""))},
    {"undefined cached", with("cached_input_tokens", rt::KvValue())},
  };
  for (const auto& [name, value] : rows) {
    auto writing = setRow(kv, rt::KvKey{PAID_FALLBACK_USAGE_ROLLUP_PREFIX, 3'600'000, name}, value);
    co_await std::move(writing);
  }
  // The rows sit in the 3600000 bucket, inside [0, 3600000 + an hour).
  auto listing = listRollups(kv, 0, 3'600'000);
  const std::vector<PaidFallbackUsageRollup> listed = co_await std::move(listing);
  std::vector<std::string> actual;
  for (const PaidFallbackUsageRollup& rollup : listed) actual.push_back(rt::jsonStringify(json::value_from(rollup)));
  std::vector<std::string> expected;
  for (const json::value& row : vectors.at("listed").as_array()) expected.push_back(rt::jsonStringify(knownMembers(row)));
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(static_cast<double>(listed.size()), *rt::getNumber(vectors, "listedCount"));
}

ASYNC_TEST("a terminal observation merges into a stored row that holds a non-JSON member") {
  const json::value vectors = rt::jsonParse(ROLLUPS_VECTORS);
  const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
  rt::KvValue stored = valid();
  stored.asObject().set("bucket_start_at_ms", rt::KvValue(0));
  stored.asObject().set("model", rt::KvValue("gpt"));
  stored.asObject().set("provider", rt::KvValue("codex"));
  stored.asObject().set("extra", rt::KvValue(rt::KvDate{5}));
  const rt::KvKey key = paidFallbackUsageRollupKey(0, "gpt", "codex", paidFallbackUsageRollupShard("r1"));
  auto writing = setRow(kv, key, stored);
  co_await std::move(writing);
  const TerminalUsageRollupInput input{
    .model = "gpt",
    .provider = "codex",
    .request_id = "r1",
    .request_created_at_ms = 1000.9,
    .input_tokens = 5,
    .cached_input_tokens = 2.5,
    .output_tokens = 7,
  };
  const double now = rt::dateNow();
  auto recording = recordObservation(input, kv);
  const bool recorded = co_await std::move(recording);
  EXPECT_EQ(recorded, vectors.at("mergeOverExotic").at("recorded").as_bool());
  json::value expected = vectors.at("mergeOverExotic").at("setValue");
  expected.as_object()["updated_at_ms"] = rt::makeNumber(now);
  EXPECT_EQ(rt::jsonStringify(kv->database()->row(key)->value.toJson()), rt::jsonStringify(expected));
}

ASYNC_TEST("terminal recordings trim identity, floor buckets and skip unobserved tokens as Bun does") {
  const json::value vectors = rt::jsonParse(ROLLUPS_VECTORS);
  const std::vector<TerminalUsageRollupInput> inputs{
    {.model = " gpt ", .provider = " codex ", .request_id = " r2 ", .request_created_at_ms = 7'200'000.5, .input_tokens = 1, .cached_input_tokens = std::nullopt, .output_tokens = 1},
    {.model = "gpt", .provider = "codex", .request_id = "r3", .request_created_at_ms = -5, .input_tokens = 1, .cached_input_tokens = -1, .output_tokens = 1},
    {.model = "gpt", .provider = "codex", .request_id = "r4", .request_created_at_ms = NaN, .input_tokens = 1, .cached_input_tokens = 1.5, .output_tokens = 1},
    {.model = "gpt", .provider = "codex", .request_id = "r5", .request_created_at_ms = std::nullopt, .input_tokens = 1, .cached_input_tokens = 9007199254740992.0, .output_tokens = 1},
    {.model = "gpt", .provider = "codex", .request_id = "r6", .request_created_at_ms = 1, .input_tokens = -1, .cached_input_tokens = 0, .output_tokens = 1},
    {.model = "gpt", .provider = "codex", .request_id = "r7", .request_created_at_ms = 1, .input_tokens = 1.5, .cached_input_tokens = 0, .output_tokens = 1},
    {.model = "gpt", .provider = "  ", .request_id = "r8", .request_created_at_ms = 1, .input_tokens = 1, .cached_input_tokens = 0, .output_tokens = 1},
  };
  const json::array& recordings = vectors.at("recordings").as_array();
  ASSERT_EQ(recordings.size(), inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const json::object& vector = recordings[index].as_object();
    const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
    const double now = rt::dateNow();
    auto recording = recordObservation(inputs[index], kv);
    const bool recorded = co_await std::move(recording);
    EXPECT_EQ(recorded, vector.at("recorded").as_bool());
    const std::vector<rt::MemoryKvDatabase::Row> rows = kv->database()->rows();
    if (!vector.at("recorded").as_bool()) {
      EXPECT(rows.empty());
      continue;
    }
    ASSERT_EQ(rows.size(), std::size_t{1});
    const bool clockBucket = !inputs[index].request_created_at_ms || rt::Number::isNaN(*inputs[index].request_created_at_ms);
    json::value expectedKey = vector.at("key");
    if (clockBucket) expectedKey.as_array()[4] = rt::makeNumber(rt::Math::floor(now / HOUR_MS) * HOUR_MS);
    EXPECT_EQ(keyText(rows[0].key), rt::jsonStringify(expectedKey));
    json::value stored = rows[0].value.toJson();
    rt::objectSet(stored.as_object(), "updated_at_ms", json::string("<now>"));
    if (clockBucket) {
      EXPECT_EQ(*rt::getNumber(stored, "bucket_start_at_ms"), rt::Math::floor(now / HOUR_MS) * HOUR_MS);
      EXPECT_EQ(*rt::getNumber(stored, "first_request_at_ms"), now);
      for (const char* key : {"bucket_start_at_ms", "first_request_at_ms", "last_request_at_ms"}) rt::objectSet(stored.as_object(), key, json::string("<now>"));
    }
    EXPECT_EQ(rt::jsonStringify(stored), rt::jsonStringify(vector.at("value")));
  }
}
