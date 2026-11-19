// Source-recorded regressions for getPaidFallbackProviderUsageV3's sparse stored-row projection.
// Oracle: /tmp/claude-1002/wave/w10c1/ledger-oracle/{source-projection.ts,provenance.json,output.json}.
// Only consumed numeric/nullish fields are in the declared return DTO contract; unused stored fields stay opaque.
// Links: tests/paid_fallback_provider_usage_projection_test.cpp src/paid_fallback/ledger_state.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/types.cpp src/utils.cpp src/kv.cpp
// src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace ledger = uos::paid_fallback::ledger_state;
namespace {
  std::vector<std::pair<std::string, std::string>> isolatedEnvironment() {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto* key : {"HOME", "TMPDIR"}) if (const auto value = rt::env::get(key)) values.emplace_back(key, *value);
    return values;
  }
  rt::Task<json::value> project(std::vector<rt::KvValue> rows) {
    const std::function<double()> now = [] { return 1000.0; };
    const auto store = std::make_shared<rt::MemoryKv>(now);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto key = ledger::paidFallbackRequestV3Key("key", rt::numberToString(static_cast<double>(i)));
      auto set = store->set(key, rows[i]);
      co_await std::move(set);
    }
    auto reading = ledger::getPaidFallbackProviderUsageV3("key", 1000, store);
    const auto result = co_await std::move(reading);
    co_return json::value_from(result);
  }
  json::value expected(const std::string& text) { return rt::jsonParse(text); }
  const json::value& arithmeticOracle(const std::string& name) {
    static const auto output = rt::jsonParse(R"oracle({"bun":"1.4.0","credential_presence":false,"results":[{"name":"string","value":{"metered":{"request_count":1,"input_tokens":"03","output_tokens":4,"total_tokens":"034","spend_microcredits":"05"},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"boolean","value":{"metered":{"request_count":1,"input_tokens":1,"output_tokens":0,"total_tokens":1,"spend_microcredits":1},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"array","value":{"metered":{"request_count":1,"input_tokens":"02,3","output_tokens":"0","total_tokens":"02,3","spend_microcredits":"0"},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"object","value":{"metered":{"request_count":1,"input_tokens":"0[object Object]","output_tokens":4,"total_tokens":"0[object Object]4","spend_microcredits":"0[object Object]"},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"date","value":{"metered":{"request_count":1,"input_tokens":"0Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)","output_tokens":4,"total_tokens":"0Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)4","spend_microcredits":0},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"bytes","value":{"metered":{"request_count":1,"input_tokens":"01,2","output_tokens":4,"total_tokens":"01,24","spend_microcredits":0},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"bigint","error":{"name":"TypeError","message":"Invalid mix of BigInt and other type in addition."}},{"name":"bigint_after_string","error":{"name":"TypeError","message":"Invalid mix of BigInt and other type in addition."}},{"name":"noncallable_toString","error":{"name":"TypeError","message":"No default value"}},{"name":"fractional","value":{"metered":{"request_count":1,"input_tokens":-3.5,"output_tokens":4.25,"total_tokens":0.75,"spend_microcredits":1.5},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"nonfinite","value":{"metered":{"request_count":1,"input_tokens":null,"output_tokens":null,"total_tokens":null,"spend_microcredits":null},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}},{"name":"null_row","error":{"name":"TypeError","message":"null is not an object (evaluating 'request.window_reset_at_ms')"}},{"name":"undefined_row","error":{"name":"TypeError","message":"undefined is not an object (evaluating 'request.window_reset_at_ms')"}}]})oracle");
    for (const auto& entry : output.at("results").as_array()) if (rt::getString(entry, "name") == name) return entry;
    throw rt::JsError("Missing source arithmetic oracle " + name);
  }
  struct ArithmeticCase { std::string name; rt::KvValue input; rt::KvValue output; rt::KvValue spend; };
  std::vector<rt::KvValue> caseRows(const ArithmeticCase& value) {
    return {rt::KvValue::object({{"window_reset_at_ms", 1000}, {"input_tokens", value.input}, {"output_tokens", value.output}, {"billing_state", "settled"}, {"spend_microcredits", value.spend}})};
  }

}

ASYNC_TEST("provider usage projects the original sparse V3 rows without full DTO decoding") {
  rt::env::ScopedEnvironment environment(isolatedEnvironment());
  std::vector<rt::KvValue> rows{
    rt::KvValue::object({{"request_id", "m"}, {"provider", "metered"}, {"window_reset_at_ms", 1000}, {"input_tokens", 3}, {"output_tokens", 4}, {"billing_state", "settled"}, {"spend_microcredits", 500}}),
    rt::KvValue::object({{"request_id", "s"}, {"provider", "surplus"}, {"window_reset_at_ms", 1000}, {"input_tokens", 11}, {"output_tokens", 7}, {"billing_state", "settled"}, {"spend_microcredits", 700}}),
  };
  auto task = project(rows); const auto result = co_await std::move(task);
  const auto oracle = expected(R"({"metered":{"request_count":1,"input_tokens":3,"output_tokens":4,"total_tokens":7,"spend_microcredits":500},"surplus":{"request_count":1,"input_tokens":11,"output_tokens":7,"total_tokens":18,"spend_microcredits":700}})");
  EXPECT(rt::deepEqual(result, oracle));
}

ASYNC_TEST("provider usage preserves nullish defaults and leaves unused typed fields opaque") {
  rt::env::ScopedEnvironment environment(isolatedEnvironment());
  std::vector<rt::KvValue> rows{
    rt::KvValue::object({{"window_reset_at_ms", 1000}, {"input_tokens", nullptr}, {"output_tokens", 4}, {"billing_state", "settled"}, {"spend_microcredits", nullptr}, {"opaque", rt::BigInt(1)}}),
    rt::KvValue::object({{"provider", "other"}, {"window_reset_at_ms", 1000}, {"input_tokens", 3}, {"output_tokens", nullptr}, {"billing_state", "pending"}, {"spend_microcredits", rt::BigInt(2)}}),
  };
  auto task = project(rows); const auto result = co_await std::move(task);
  const auto oracle = expected(R"({"metered":{"request_count":2,"input_tokens":3,"output_tokens":4,"total_tokens":7,"spend_microcredits":0},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}})");
  EXPECT(rt::deepEqual(result, oracle));
}

ASYNC_TEST("provider usage applies strict numeric window equality before reading irrelevant fields") {
  rt::env::ScopedEnvironment environment(isolatedEnvironment());
  std::vector<rt::KvValue> rows{
    rt::KvValue::object({{"window_reset_at_ms", "1000"}, {"input_tokens", rt::BigInt(2)}, {"output_tokens", rt::BigInt(3)}}),
    rt::KvValue::object({{"window_reset_at_ms", 2000}, {"input_tokens", rt::BigInt(2)}, {"output_tokens", rt::BigInt(3)}}),
    rt::KvValue::object({{"input_tokens", rt::BigInt(2)}, {"output_tokens", rt::BigInt(3)}}),
  };
  auto task = project(rows); const auto result = co_await std::move(task);
  const auto oracle = expected(R"({"metered":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}})");
  EXPECT(rt::deepEqual(result, oracle));
}


ASYNC_TEST("provider usage preserves source number string and stored-object addition semantics") {
  rt::env::ScopedEnvironment environment(isolatedEnvironment());
  const std::vector<ArithmeticCase> cases{
    {"string", "3", 4, "5"}, {"boolean", true, false, true},
    {"array", rt::KvValue::array({2, 3}), rt::KvValue::array({}), rt::KvValue::array({})},
    {"object", rt::KvValue::object({}), 4, rt::KvValue::object({})},
    {"date", rt::KvDate{0}, 4, 0}, {"bytes", rt::KvBytes{1, 2}, 4, 0},
    {"fractional", -3.5, 4.25, 1.5},
    {"nonfinite", std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()},
  };
  for (const auto& sample : cases) {
    const auto rows = caseRows(sample); auto task = project(rows); const auto result = co_await std::move(task);
    const auto& oracle = arithmeticOracle(sample.name).at("value");
    EXPECT_EQ(rt::jsonStringify(result), rt::jsonStringify(oracle));
  }
}

ASYNC_TEST("provider usage preserves source BigInt coercion and null-row errors") {
  rt::env::ScopedEnvironment environment(isolatedEnvironment());
  const std::vector<ArithmeticCase> cases{
    {"bigint", rt::BigInt(1), 4, 0}, {"bigint_after_string", "x", rt::BigInt(1), 0},
    {"noncallable_toString", rt::KvValue::object({{"toString", 1}}), 4, 0},
  };
  for (const auto& sample : cases) {
    const auto rows = caseRows(sample); auto task = project(rows);
    const auto message = std::string(arithmeticOracle(sample.name).at("error").at("message").as_string());
    EXPECT_REJECTS_MESSAGE(std::move(task), rt::TypeError, message.c_str());
  }
  std::vector<rt::KvValue> nullRows{rt::KvValue(nullptr)}; auto nullTask = project(nullRows);
  const auto nullMessage = std::string(arithmeticOracle("null_row").at("error").at("message").as_string());
  EXPECT_REJECTS_MESSAGE(std::move(nullTask), rt::TypeError, nullMessage.c_str());
  std::vector<rt::KvValue> undefinedRows{rt::KvValue()}; auto undefinedTask = project(undefinedRows);
  const auto undefinedMessage = std::string(arithmeticOracle("undefined_row").at("error").at("message").as_string());
  EXPECT_REJECTS_MESSAGE(std::move(undefinedTask), rt::TypeError, undefinedMessage.c_str());
}

ASYNC_TEST("provider usage retains left-to-right concatenation after an earlier string counter") {
  rt::env::ScopedEnvironment environment(isolatedEnvironment());
  const std::vector<rt::KvValue> rows{
    rt::KvValue::object({{"window_reset_at_ms", 1000}, {"input_tokens", "x"}, {"output_tokens", 0}, {"billing_state", "settled"}, {"spend_microcredits", "5"}}),
    rt::KvValue::object({{"window_reset_at_ms", 1000}, {"input_tokens", rt::BigInt(1)}, {"output_tokens", 0}, {"billing_state", "pending"}, {"spend_microcredits", rt::BigInt(2)}}),
  };
  auto task = project(rows); const auto result = co_await std::move(task);
  const auto output = rt::jsonParse(R"oracle([{"name":"bigint_after_previous_string","value":{"metered":{"request_count":2,"input_tokens":"0x1","output_tokens":0,"total_tokens":"0x010","spend_microcredits":"050"},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}}}])oracle");
  EXPECT_EQ(rt::jsonStringify(result), rt::jsonStringify(output.at(0).at("value")));
}
