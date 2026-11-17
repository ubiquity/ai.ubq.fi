// Links: tests/claude_code_usage_test.cpp src/provider/claude_code_usage.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/utils.cpp
// Port of tests/claude-code-usage.test.ts.
#include "uos/provider/claude_code_usage.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using provider::claude_code::ClaudeCodeAuthHeaders;
using provider::claude_code_usage::CLAUDE_CODE_USAGE_URL;
using provider::claude_code_usage::CLAUDE_CODE_USAGE_USER_AGENT;
using provider::claude_code_usage::ClaudeCodeUsageFailure;
using provider::claude_code_usage::ClaudeCodeUsageResult;
using provider::claude_code_usage::ClaudeCodeUsageSuccess;
using provider::claude_code_usage::fetchClaudeCodeUsage;
using provider::claude_code_usage::FetchClaudeCodeUsageOptions;

namespace {
  const double NOW_MS = rt::Date::UTC(2026, 9, 25, 19, 0, 0);
  const ClaudeCodeAuthHeaders CREDENTIAL{.kind = "oauth", .secret = "sk-ant-oat-fixture-usage"};

  std::string requestUrl(const rt::RequestInfo& input) {
    if (const std::string* text = std::get_if<std::string>(&input)) return *text;
    if (const rt::URL* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }

  FetchClaudeCodeUsageOptions withFetcher(rt::Fetch fetcher) {
    FetchClaudeCodeUsageOptions options;
    options.fetcher = std::move(fetcher);
    options.now = [] { return NOW_MS; };
    return options;
  }
}

ASYNC_TEST("direct Claude usage parses percentages, resets, and the required OAuth headers") {
  const std::string resetFiveHour = "2026-10-25T23:00:00.123456+00:00";
  const std::string resetSevenDay = "2026-11-01T18:00:00.123456+00:00";
  auto seenUrl = std::make_shared<std::optional<std::string>>();
  auto seenInit = std::make_shared<std::optional<rt::RequestInit>>();
  const rt::Fetch fetcher([seenUrl, seenInit](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    *seenUrl = requestUrl(input);
    *seenInit = init;
    return rt::Response::json(rt::jsonParse(R"({"five_hour":{"utilization":42,"resets_at":"2026-10-25T23:00:00.123456+00:00"},)"
                                            R"("seven_day":{"utilization":12.5,"resets_at":"2026-11-01T18:00:00.123456+00:00"},"seven_day_opus":null})"));
  });

  auto call = fetchClaudeCodeUsage(CREDENTIAL, withFetcher(fetcher));
  const ClaudeCodeUsageResult outcome = co_await std::move(call);

  ASSERT(std::holds_alternative<ClaudeCodeUsageSuccess>(outcome));
  const ClaudeCodeUsageSuccess& result = std::get<ClaudeCodeUsageSuccess>(outcome);
  ASSERT(seenUrl->has_value());
  ASSERT(seenInit->has_value());
  EXPECT_EQ(**seenUrl, std::string(CLAUDE_CODE_USAGE_URL));
  EXPECT_EQ((*seenInit)->method, std::optional<std::string>("GET"));
  // usage reads never send an inference body
  EXPECT(!(*seenInit)->body.has_value());
  const rt::Headers headers((*seenInit)->headers.value_or(rt::Headers()));
  EXPECT_EQ(headers.get("authorization"), std::optional<std::string>("Bearer " + CREDENTIAL.secret));
  EXPECT_EQ(headers.get("anthropic-beta"), std::optional<std::string>("oauth-2025-04-20"));
  EXPECT_EQ(headers.get("anthropic-version"), std::optional<std::string>("2023-06-01"));
  EXPECT_EQ(headers.get("user-agent"), std::optional<std::string>(std::string(CLAUDE_CODE_USAGE_USER_AGENT)));
  EXPECT_EQ(headers.get("accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headers.get("content-type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(result.observation.observed_at_ms, NOW_MS);
  const provider::claude_code_quota::ClaudeCodeQuotaWindow* fiveHour = result.observation.windows.get("5h");
  ASSERT(fiveHour != nullptr);
  EXPECT(*fiveHour == (provider::claude_code_quota::ClaudeCodeQuotaWindow{
                        .status = std::nullopt, .utilization = 0.42, .reset_at_ms = rt::Date::parse(resetFiveHour), .observed_at_ms = NOW_MS}));
  const provider::claude_code_quota::ClaudeCodeQuotaWindow* sevenDay = result.observation.windows.get("7d");
  ASSERT(sevenDay != nullptr);
  EXPECT_EQ(sevenDay->utilization, std::optional<double>(0.125));
  EXPECT_EQ(sevenDay->reset_at_ms, std::optional<double>(rt::Date::parse(resetSevenDay)));
  EXPECT_EQ(result.observation.standard.size(), std::size_t{0});
  EXPECT_EQ(result.observation.representative_claim, std::nullopt);
}

ASYNC_TEST("a setup token without the profile scope is an explicit non-fatal usage result") {
  const rt::Fetch fetcher([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    rt::ResponseInit init;
    init.status = 403;
    return rt::Response::json(rt::jsonParse(R"({"type":"error","error":{"type":"permission_error","message":"OAuth token does not meet scope requirement user:profile"}})"),
                              init);
  });
  auto call = fetchClaudeCodeUsage(CREDENTIAL, withFetcher(fetcher));
  const ClaudeCodeUsageResult outcome = co_await std::move(call);

  ASSERT(std::holds_alternative<ClaudeCodeUsageFailure>(outcome));
  const ClaudeCodeUsageFailure& result = std::get<ClaudeCodeUsageFailure>(outcome);
  EXPECT_EQ(result.kind, std::string("scope_missing"));
  EXPECT_EQ(result.status, std::optional<double>(403));
  EXPECT_EQ(result.observed_at_ms, NOW_MS);
  static const rt::Regex PROFILE("user:profile", "");
  EXPECT(PROFILE.test(result.message.value_or("")));
}

ASYNC_TEST("malformed Claude usage payloads fail closed as invalid responses") {
  std::vector<std::pair<std::string, std::function<rt::Response()>>> payloads;
  payloads.emplace_back("invalid JSON", [] {
    rt::ResponseInit init;
    init.status = 200;
    init.headers = rt::Headers{{"content-type", "application/json"}};
    return rt::Response(rt::BodyInit(std::string("not-json")), init);
  });
  payloads.emplace_back("unrecognized shape", [] { return rt::Response::json(rt::jsonParse(R"({"unknown_window":{"utilization":10,"resets_at":null}})")); });
  payloads.emplace_back("invalid reset type", [] {
    return rt::Response::json(rt::jsonParse(R"({"five_hour":{"utilization":10,"resets_at":123},"seven_day":{"utilization":2,"resets_at":null}})"));
  });
  payloads.emplace_back("missing utilization", [] {
    return rt::Response::json(rt::jsonParse(R"({"five_hour":{"resets_at":null},"seven_day":{"utilization":2,"resets_at":null}})"));
  });

  for (const auto& [label, response] : payloads) {
    const std::function<rt::Response()> make = response;
    const rt::Fetch fetcher([make](rt::RequestInfo, rt::RequestInit) -> rt::Response { return make(); });
    auto call = fetchClaudeCodeUsage(CREDENTIAL, withFetcher(fetcher));
    const ClaudeCodeUsageResult outcome = co_await std::move(call);
    if (!std::holds_alternative<ClaudeCodeUsageFailure>(outcome)) {
      rt::test::report(__FILE__, __LINE__, "expected a failure: " + label, false);
      continue;
    }
    const ClaudeCodeUsageFailure& result = std::get<ClaudeCodeUsageFailure>(outcome);
    EXPECT_EQ(result.kind, std::string("invalid_response"));
    EXPECT_EQ(result.status, std::optional<double>(200));
  }
}

ASYNC_TEST("a Console API key does not call the OAuth usage endpoint") {
  auto calls = std::make_shared<int>(0);
  const rt::Fetch fetcher([calls](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    *calls += 1;
    return rt::Response::json(json::object());
  });
  const ClaudeCodeAuthHeaders apiKey{.kind = "api_key", .secret = "sk-ant-usr-fixture"};
  auto call = fetchClaudeCodeUsage(apiKey, withFetcher(fetcher));
  const ClaudeCodeUsageResult outcome = co_await std::move(call);
  EXPECT_EQ(*calls, 0);
  ASSERT(std::holds_alternative<ClaudeCodeUsageFailure>(outcome));
  const ClaudeCodeUsageFailure& result = std::get<ClaudeCodeUsageFailure>(outcome);
  EXPECT_EQ(result.kind, std::string("unsupported_credential"));
  EXPECT_EQ(result.status, std::nullopt);
}
