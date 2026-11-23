// Port of tests/cache-usage-normalization.test.ts.
// Links: tests/cache_usage_normalization_test.cpp src/openai_telemetry.cpp src/auth/relay.cpp src/codex/responses_error.cpp src/config.cpp
//        src/http.cpp src/inference_deadline.cpp src/provider/cerebras_rate_limits.cpp src/provider/removed_provider_telemetry.cpp
//        src/release.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/utils.cpp
//
// Not representable: the last malformed Surplus charge of the TS loop, an own `buyer_cost_micro` member holding
// `undefined` (a JS object literal can have one; a json::value cannot, and no JSON body produces one). The other five
// malformed amounts are checked.
#include "uos/openai_telemetry.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::openai_telemetry::extractChatUsageTokens;
using uos::openai_telemetry::extractUsageTokens;
using uos::openai_telemetry::toChatUsage;
using uos::openai_telemetry::UsageTokens;

namespace {
  json::value val(std::string_view text) { return rt::jsonParse(text); }

  // `details?: unknown`: nullptr is undefined.
  json::object chatUsage(const json::value* details = nullptr) {
    json::object usage;
    rt::objectDefine(usage, "prompt_tokens", 100);
    rt::objectDefine(usage, "completion_tokens", 10);
    rt::objectDefine(usage, "total_tokens", 110);
    if (details) rt::objectDefine(usage, "prompt_tokens_details", *details);
    return usage;
  }

  json::object responsesUsage(const json::value* details = nullptr) {
    json::object usage;
    rt::objectDefine(usage, "input_tokens", 100);
    rt::objectDefine(usage, "output_tokens", 10);
    rt::objectDefine(usage, "total_tokens", 110);
    if (details) rt::objectDefine(usage, "input_tokens_details", *details);
    return usage;
  }

  // `{ ...base, key: value }`
  json::object with(json::object base, std::string_view key, json::value value) {
    rt::objectDefine(base, key, std::move(value));
    return base;
  }

  std::optional<UsageTokens> chat(const json::object& value) { return extractChatUsageTokens(json::value(value)); }
}

TEST("Chat and Responses cache usage retain identical reported, partial and invalid counters") {
  const std::vector<std::optional<json::value>> cases{
    std::nullopt,
    val(R"({"cached_tokens":0})"),
    val(R"({"cached_tokens":80,"cache_write_tokens":20})"),
    val(R"({"cached_tokens":101})"),
    val(R"({"cached_tokens":-0.1})"),
    val(R"({"cached_tokens":0.9})"),
    val(R"({"cached_tokens":"0"})"),
    val(R"({"cached_tokens":80,"cache_write_tokens":0.9})"),
    val(R"({"cached_tokens":80,"cache_write_tokens":-0.1})"),
    json::value(nullptr),
    json::value(json::array()),
  };
  for (const std::optional<json::value>& details : cases) {
    const json::value* detailsValue = details ? &*details : nullptr;
    EXPECT(chat(chatUsage(detailsValue)) == extractUsageTokens(json::value(responsesUsage(detailsValue))));
  }
  for (const double cachedTokens : {-0.1, 0.9}) {
    json::object detailsObject;
    rt::objectDefine(detailsObject, "cached_tokens", rt::makeNumber(cachedTokens));
    const json::value details(detailsObject);
    const std::optional<UsageTokens> usage = chat(chatUsage(&details));
    ASSERT(usage.has_value());
    EXPECT_EQ(usage->cachedInputTokens, std::optional<double>());  // a fractional counter must not become a measured cache miss
    EXPECT_EQ(usage->status, std::string("invalid"));
  }
}

TEST("Chat usage preserves measured counters when another token field is missing or malformed") {
  EXPECT(chat(val(R"({"prompt_tokens":100,"prompt_tokens_details":{"cached_tokens":80}})").as_object()) ==
         std::optional<UsageTokens>(UsageTokens{
           .inputTokens = 100,
           .cachedInputTokens = 80,
           .cacheWriteInputTokens = std::nullopt,
           .outputTokens = std::nullopt,
           .totalTokens = std::nullopt,
           .status = "partial",
         }));
  const json::value cached = val(R"({"cached_tokens":80})");
  for (const double promptTokens : {-0.1, 100.9, rt::Number::MAX_SAFE_INTEGER + 1}) {
    EXPECT(chat(with(chatUsage(&cached), "prompt_tokens", rt::makeNumber(promptTokens))) == std::optional<UsageTokens>(UsageTokens{
                                                                                                .inputTokens = std::nullopt,
                                                                                                .cachedInputTokens = 80,
                                                                                                .cacheWriteInputTokens = std::nullopt,
                                                                                                .outputTokens = 10,
                                                                                                .totalTokens = 110,
                                                                                                .status = "invalid",
                                                                                              }));
  }
}

TEST("usage translation keeps unmeasured cache counters absent and preserves measured zero") {
  EXPECT(!extractChatUsageTokens(json::value(nullptr)).has_value());
  EXPECT(!extractChatUsageTokens(static_cast<const json::value*>(nullptr)).has_value());
  const std::optional<json::object> plain = toChatUsage(chat(chatUsage()));
  ASSERT(plain.has_value());
  EXPECT(rt::deepEqual(json::value(*plain), json::value(chatUsage())));
  const json::value zero = val(R"({"cached_tokens":0})");
  const std::optional<json::object> measuredZero = toChatUsage(chat(chatUsage(&zero)));
  ASSERT(measuredZero.has_value());
  EXPECT(rt::deepEqual(json::value(*measuredZero), json::value(chatUsage(&zero))));
  EXPECT(!toChatUsage(chat(val(R"({"prompt_tokens":100,"prompt_tokens_details":{"cached_tokens":80}})").as_object())).has_value());
}

TEST("usage retains an original exact Surplus charge and distinguishes absent or malformed amounts") {
  const json::value zero = val(R"({"cached_tokens":0})");
  for (const double buyerCostMicro : {0.0, 69.0, 1056.0}) {
    const std::optional<UsageTokens> chatResult = chat(with(chatUsage(&zero), "buyer_cost_micro", rt::makeNumber(buyerCostMicro)));
    EXPECT(chatResult && chatResult->buyerCostMicro == buyerCostMicro);
    json::object responses = val(R"({"input_tokens":1043,"output_tokens":4,"total_tokens":1047,"input_tokens_details":{"cached_tokens":1028}})").as_object();
    rt::objectDefine(responses, "buyer_cost_micro", rt::makeNumber(buyerCostMicro));
    const std::optional<UsageTokens> responsesResult = extractUsageTokens(json::value(responses));
    EXPECT(responsesResult && responsesResult->buyerCostMicro == buyerCostMicro);
  }
  const std::optional<UsageTokens> absent = chat(chatUsage());
  ASSERT(absent.has_value());
  EXPECT(absent->buyerCostMicro == rt::undefined);
  const std::vector<json::value> malformed{
    rt::makeNumber(-1.0), rt::makeNumber(0.1), json::value("69"), rt::makeNumber(rt::Number::MAX_SAFE_INTEGER + 1), json::value(nullptr),
  };
  for (const json::value& buyerCostMicro : malformed) {
    const std::optional<UsageTokens> usage = chat(with(chatUsage(&zero), "buyer_cost_micro", buyerCostMicro));
    ASSERT(usage.has_value());
    EXPECT(usage->buyerCostMicro != rt::undefined);
    EXPECT(usage->buyerCostMicro == nullptr);
  }
}
