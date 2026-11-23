// Links: tests/cluster_w04c1/PA1_headers_test.cpp (and the rt sources unit.sh adds).
// Phase A of chunk w04c1 (headers first), header agent PA1: the nineteen headers below compile
// on their own and together, before any of their .cpp files exists. The one test checks only
// what the headers define inline (the error classes, constants built from literals) against
// Bun 1.4.0 output of the TS class bodies, and the static_asserts pin that the LithosAI and
// Cerebras transports bind to ChatOnlyResponsesProfile's function pointers as DeepSeek's do.
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/request_policy.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/deepseek/responses.hpp"
#include "uos/metered_quota.hpp"
#include "uos/provider/surplus_claude_chat_request.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <type_traits>

using namespace uos;

namespace {
  using Profile = deepseek::responses::ChatOnlyResponsesProfile;
  using FinishDisposition = decltype(Profile::finishDisposition);
  using ToolChoiceConflict = decltype(Profile::thinkingToolChoiceConflict);

  // Unevaluated, so no definition is needed: every profile slot accepts its transport's function.
  static_assert(std::is_same_v<decltype(&deepseek::index::deepSeekUpstreamModelFor), decltype(Profile::upstreamModelFor)>);
  static_assert(std::is_same_v<decltype(static_cast<FinishDisposition>(&deepseek::index::deepSeekFinishDisposition)), FinishDisposition>);
  static_assert(std::is_same_v<decltype(static_cast<ToolChoiceConflict>(&deepseek::index::deepSeekThinkingToolChoiceConflict)), ToolChoiceConflict>);
  static_assert(std::is_same_v<decltype(&deepseek::index::deepSeekToolChoiceThinkingConflictMessage), decltype(Profile::toolChoiceThinkingConflictMessage)>);
  static_assert(std::is_same_v<decltype(&deepseek::index::deepSeekCachedPromptTokens), decltype(Profile::cachedPromptTokens)>);
  static_assert(std::is_same_v<decltype(&deepseek::index::deepSeekReasoningTokens), decltype(Profile::reasoningTokens)>);
  static_assert(std::is_same_v<decltype(&provider::lithos::lithosUpstreamModelFor), decltype(Profile::upstreamModelFor)>);
  static_assert(std::is_same_v<decltype(&provider::lithos::lithosCachedPromptTokens), decltype(Profile::cachedPromptTokens)>);
  static_assert(std::is_same_v<decltype(&provider::lithos::lithosReasoningTokens), decltype(Profile::reasoningTokens)>);
  static_assert(std::is_same_v<decltype(&provider::cerebras::cerebrasUpstreamModelFor), decltype(Profile::upstreamModelFor)>);
  static_assert(std::is_same_v<decltype(&provider::cerebras::cerebrasCachedPromptTokens), decltype(Profile::cachedPromptTokens)>);
  static_assert(std::is_same_v<decltype(&provider::cerebras::cerebrasReasoningTokens), decltype(Profile::reasoningTokens)>);

  std::optional<std::string> baseCode(const rt::JsError& error) { return error.code; }
}

TEST("w04c1 PA1 headers: inline error classes and literal constants") {
  using api_key_policy::ApiKeyQuotaDispatchError;
  using api_key_policy::ApiKeyQuotaDispatchErrorOptions;
  using Headers = rt::OrderedMap<std::string, std::string>;

  // new ApiKeyQuotaDispatchError()
  const ApiKeyQuotaDispatchError plain;
  EXPECT_EQ(plain.name, std::string("ApiKeyQuotaDispatchError"));
  EXPECT_EQ(plain.message, std::string("API key quota reservation is no longer available"));
  EXPECT_EQ(plain.status, 503.0);
  EXPECT_EQ(plain.code, std::string("api_key_quota_reservation_unavailable"));
  EXPECT_EQ(plain.errorType, std::string("server_error"));
  EXPECT(!plain.retryAfter.has_value());
  EXPECT_EQ(plain.headers.size(), std::size_t{0});
  EXPECT(baseCode(plain) == std::optional<std::string>("api_key_quota_reservation_unavailable"));

  // { status: 429, headers: { "Retry-After": "7", RateLimit: "x" } }: retryAfter "7", headers kept.
  ApiKeyQuotaDispatchErrorOptions fromHeader;
  fromHeader.status = 429;
  fromHeader.headers = Headers{{"Retry-After", "7"}, {"RateLimit", "x"}};
  const ApiKeyQuotaDispatchError limited("m", fromHeader);
  EXPECT_EQ(limited.status, 429.0);
  EXPECT(limited.retryAfter == std::optional<std::string>("7"));
  EXPECT_EQ(limited.headers.size(), std::size_t{2});
  EXPECT_EQ(*limited.headers.get("Retry-After"), std::string("7"));

  // { retryAfter: "9", headers: { RateLimit: "x" } }: Retry-After appended after RateLimit.
  ApiKeyQuotaDispatchErrorOptions appended;
  appended.retryAfter = std::string("9");
  appended.headers = Headers{{"RateLimit", "x"}};
  const ApiKeyQuotaDispatchError added("m", appended);
  EXPECT(added.retryAfter == std::optional<std::string>("9"));
  EXPECT_EQ(rt::arrayFrom(added.headers.keys()), (std::vector<std::string>{"RateLimit", "Retry-After"}));
  EXPECT_EQ(*added.headers.get("Retry-After"), std::string("9"));

  // { retryAfter: null, headers: { "Retry-After": "" } }: null ?? "" is "".
  ApiKeyQuotaDispatchErrorOptions nulled;
  nulled.retryAfter = nullptr;
  nulled.headers = Headers{{"Retry-After", ""}};
  const ApiKeyQuotaDispatchError empty("m", nulled);
  EXPECT(empty.retryAfter == std::optional<std::string>(""));
  EXPECT_EQ(*empty.headers.get("Retry-After"), std::string(""));

  // { retryAfter: "4", headers: { "Retry-After": "" } }: an empty header is replaced in place.
  ApiKeyQuotaDispatchErrorOptions replaced;
  replaced.retryAfter = std::string("4");
  replaced.headers = Headers{{"Retry-After", ""}};
  const ApiKeyQuotaDispatchError refilled("m", replaced);
  EXPECT_EQ(*refilled.headers.get("Retry-After"), std::string("4"));
  EXPECT_EQ(refilled.headers.size(), std::size_t{1});

  const provider::metered::MeteredError metered("x", "metered_logs_invalid", 502, 418);
  EXPECT_EQ(metered.name, std::string("MeteredError"));
  EXPECT(metered.upstream_status == std::optional<double>(418));
  EXPECT(baseCode(metered) == std::optional<std::string>("metered_logs_invalid"));
  const provider::surplus::SurplusError surplus("x", "surplus_request_invalid", 400);
  EXPECT_EQ(surplus.name, std::string("SurplusError"));
  EXPECT(!surplus.upstream_status.has_value());
  const deepseek::index::DeepSeekError deepSeek("x", "gateway_timeout", 504);
  EXPECT_EQ(deepSeek.name, std::string("DeepSeekError"));
  EXPECT_EQ(deepSeek.status, 504.0);
  const provider::cerebras::CerebrasError cerebras("x", "cerebras_api_key_missing", 503);
  EXPECT_EQ(cerebras.name, std::string("CerebrasError"));
  const provider::lithos::LithosError lithos("x", "lithos_request_invalid", 400);
  EXPECT_EQ(lithos.name, std::string("LithosError"));
  EXPECT(baseCode(lithos) == std::optional<std::string>("lithos_request_invalid"));

  EXPECT(api_key_policy::API_KEY_USAGE_V3_WINDOW_PREFIX == (rt::KvKey{"uos_ai", "api_key_usage", "v3", "window"}));
  EXPECT(api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX == (rt::KvKey{"uos_ai", "api_key_usage", "v3", "request"}));
  EXPECT_EQ(*request_policy::WARNING_KEY_MAP.get("max_tokens"), std::string("max_output_tokens_ignored"));
  EXPECT_EQ(provider::lithos::LITHOS_UPSTREAM_MODEL_BY_ID.size(), std::size_t{8});
  EXPECT_EQ(metered_quota::METERED_QUOTA_BALANCE_HISTORY_DAILY_BUCKET_MS, 86'400'000.0);
  EXPECT_EQ(provider::lithos_rate_limits::LITHOS_BUFFERED_REFUSAL_WAIT_POLICY.totalCapMs, 120'000.0);
}
