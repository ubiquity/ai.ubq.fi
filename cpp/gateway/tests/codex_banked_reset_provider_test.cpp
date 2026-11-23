// Port of tests/codex-banked-reset-provider.test.ts.
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::codex::banked_reset_provider;

  // `await assert.rejects(promise, ErrorClass | predicate)`.
  template <class E, class T> rt::Task<void> rejectsWith(rt::Task<T> task, const std::function<bool(const E&)>* check, const char* file, int line) {
    bool matched = false;
    std::string other = "no rejection";
    try {
      co_await std::move(task);
    } catch (const E& error) {
      matched = check == nullptr || (*check)(error);
      if (!matched) other = error.toString();
    } catch (...) {
      other = rt::reasonToString(std::current_exception());
    }
    if (!matched) rt::test::report(file, line, "expected a matching rejection, got " + other, false);
  }
#define ASSERT_REJECTS_TYPE(task, Type) co_await rejectsWith<Type>((task), nullptr, __FILE__, __LINE__)
#define ASSERT_REJECTS_IF(task, Type, check) co_await rejectsWith<Type>((task), &(check), __FILE__, __LINE__)

  // `await assert.rejects(promise, /pattern/)`
  template <class T> rt::Task<void> rejectsMatching(rt::Task<T> task, const char* pattern, const char* file, int line) {
    bool rejected = false;
    std::string text;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
      text = rt::reasonToString(std::current_exception());
    }
    if (!rejected || !rt::Regex(pattern).test(text)) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/, got " + text, false);
  }
#define ASSERT_REJECTS(task, pattern) co_await rejectsMatching((task), pattern, __FILE__, __LINE__)

  // `runSequence(async (step) => ...)`: a failing step is reported under its name; later steps still run.
  template <class F> rt::Task<void> step(std::string name, F body) {
    rt::Reason failure;
    try {
      rt::Task<void> running = rt::async(std::move(body));
      co_await std::move(running);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) rt::test::report(__FILE__, __LINE__, "step " + name + " failed: " + rt::reasonToString(failure), false);
  }

  ResetAccountContext context(const std::string& accountId = "account-one") {
    return ResetAccountContext{
      .accountId = accountId,
      .accountIdHash = "account-one-hash",
      .credentialVersion = "credential-v1",
      .quotaGeneration = "quota-generation-v1",
    };
  }

  RedeemResetInput redeem(const std::string& accountId = "account-one") {
    RedeemResetInput input;
    static_cast<ResetAccountContext&>(input) = context(accountId);
    input.idempotencyKey = "redeem-request-id-123";
    input.creditId = "credit-selected";
    return input;
  }

  rt::AbortSignal signal() { return rt::AbortController().signal(); }

  /** Resolve a captured fetch target exactly as its overloads expose it: string, URL, or Request. */
  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  std::shared_ptr<CodexUsageResetProvider> provider(CodexUsageResetFetch fetch) {
    return createUpstreamCodexUsageResetProvider(UpstreamCodexUsageResetProviderOptions{
      .codexBaseUrl = "https://chatgpt.com/backend-api/codex",
      .accountId = "account-one",
      .accessToken = "test-access-token",
      .userAgent = "codex-banked-reset-provider-test/1.0",
      .fetch = std::move(fetch),
      .now = [] { return 1'700'000'000'000.0; },
    });
  }

  rt::Response textResponse(std::string body, double status) {
    rt::ResponseInit init;
    init.status = status;
    return rt::Response(rt::BodyInit(std::move(body)), std::move(init));
  }

  const RedeemResetResult unknownWithoutReceipt = RedeemResetResultUnknown{.providerReceiptId = std::nullopt};
}

TEST("upstream reset adapter derives the Codex reset-credit routes") {
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/backend-api/codex") ==
         (CodexUsageResetCreditEndpoints{
           .inventoryUrl = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
           .consumeUrl = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume",
         }));
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://gateway.example/") ==
         (CodexUsageResetCreditEndpoints{
           .inventoryUrl = "https://gateway.example/api/codex/rate-limit-reset-credits",
           .consumeUrl = "https://gateway.example/api/codex/rate-limit-reset-credits/consume",
         }));
  EXPECT_THROWS(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/unknown-layout"), CodexUsageResetProviderConfigurationError);
  EXPECT_THROWS(resolveCodexUsageResetCreditEndpoints("http://chatgpt.com/backend-api/codex"), CodexUsageResetProviderConfigurationError);
}

ASYNC_TEST("upstream reset adapter parses inventory and sends the selected credit contract") {
  struct Call {
    std::string url;
    rt::RequestInit init;
  };
  auto calls = std::make_shared<std::vector<Call>>();
  auto responses = std::make_shared<std::deque<rt::Response>>();
  responses->push_back(textResponse(rt::jsonStringify(rt::jsonParse(R"({
        "available_count": 2,
        "credits": [
          { "id": "other", "reset_type": "other", "status": "available", "expires_at": null },
          { "id": "credit-available", "reset_type": "codex_rate_limits", "status": "available", "expires_at": "2026-12-31T00:00:00Z" }
        ]
      })")),
                                    200));
  responses->push_back(textResponse(R"({"code":"reset","windows_reset":2})", 201));
  const auto resetProvider = provider([calls, responses](rt::RequestInfo input, rt::RequestInit init) {
    calls->push_back(Call{.url = requestUrl(input), .init = init});
    if (responses->empty()) throw rt::JsError("unexpected provider request");
    rt::Response response = responses->front();
    responses->pop_front();
    return response;
  });

  EXPECT_EQ(providerSupportsLiveRedemption(resetProvider.get()), true);
  rt::Task<ResetInventory> reading = resetProvider->readInventory(context(), signal());
  const ResetInventory inventory = co_await std::move(reading);
  EXPECT(inventory == (ResetInventory{
                        .availableCount = 2,
                        .observedAtMs = 1'700'000'000'000,
                        .credits =
                          {
                            ResetInventoryCredit{.id = "other", .status = "available", .resetType = "other", .expiresAtMs = std::nullopt},
                            ResetInventoryCredit{.id = "credit-available", .status = "available", .resetType = "codex_rate_limits", .expiresAtMs = 1'798'675'200'000},
                          },
                      }));
  RedeemResetInput selected = redeem();
  selected.creditId = "credit-available";
  rt::Task<RedeemResetResult> redeeming = resetProvider->redeem(selected, signal());
  const RedeemResetResult redeemed = co_await std::move(redeeming);
  EXPECT(redeemed == RedeemResetResult(RedeemResetResultCompleted{.providerReceiptId = "upstream-reset"}));
  ASSERT_EQ(calls->size(), 2u);
  std::vector<std::string> requests;
  for (const Call& call : *calls) requests.push_back(call.init.method.value_or("undefined") + " " + call.url);
  EXPECT_EQ(requests, std::vector<std::string>({"GET https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
                                                "POST https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume"}));
  EXPECT(!(*calls)[0].init.body.has_value());
  ASSERT((*calls)[1].init.body.has_value());
  EXPECT_EQ(std::get<std::string>(*(*calls)[1].init.body), R"({"redeem_request_id":"redeem-request-id-123","credit_id":"credit-available"})");
  const rt::Headers headers((*calls)[1].init.headers.value_or(rt::Headers()));
  EXPECT_EQ(headers.get("Authorization"), std::optional<std::string>("Bearer test-access-token"));
  EXPECT_EQ(headers.get("ChatGPT-Account-ID"), std::optional<std::string>("account-one"));
  EXPECT_EQ(headers.get("User-Agent"), std::optional<std::string>("codex-banked-reset-provider-test/1.0"));
  EXPECT_EQ(headers.get("Content-Type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headers.get("originator"), std::nullopt);
}

ASYNC_TEST("upstream reset adapter maps documented 2xx codes and preserves the exact request id") {
  struct Case {
    double status;
    std::string code;
    RedeemResetResult expected;
  };
  const std::vector<Case> cases{
    {200, "reset", RedeemResetResultCompleted{.providerReceiptId = "upstream-reset"}},
    {201, "already_redeemed", RedeemResetResultAlreadyRedeemed{.providerReceiptId = "upstream-already-redeemed"}},
    {299, "nothing_to_reset", RedeemResetResultRejected{.reason = "nothing_to_reset"}},
    {200, "no_credit", RedeemResetResultRejected{.reason = "no_credit"}},
  };
  for (const Case& entry : cases) {
    auto body = [entry]() -> rt::Task<void> {
      auto captured = std::make_shared<std::optional<std::string>>();
      const auto resetProvider = provider([captured, entry](rt::RequestInfo, rt::RequestInit init) {
        *captured = init.body && std::holds_alternative<std::string>(*init.body) ? std::optional<std::string>(std::get<std::string>(*init.body)) : std::nullopt;
        json::object payload;
        payload["code"] = entry.code;
        return textResponse(rt::jsonStringify(payload), entry.status);
      });
      RedeemResetInput input = redeem();
      input.idempotencyKey = "  stable-id  ";
      rt::Task<RedeemResetResult> redeeming = resetProvider->redeem(input, signal());
      const RedeemResetResult redeemed = co_await std::move(redeeming);
      EXPECT(redeemed == entry.expected);
      EXPECT_EQ(*captured, std::optional<std::string>(R"({"redeem_request_id":"  stable-id  ","credit_id":"credit-selected"})"));
    };
    rt::Task<void> running = step(rt::numberToString(entry.status) + " " + entry.code, body);
    co_await std::move(running);
  }
}

ASYNC_TEST("upstream reset adapter treats non-2xx and malformed 2xx consume replies as ambiguous") {
  rt::ResponseInit noContent;
  noContent.status = 204;
  const std::vector<rt::Response> responses{
    rt::Response(nullptr, noContent),
    textResponse("not json", 200),
    textResponse(R"({"code":"unrecognized"})", 200),
    textResponse("not relevant", 429),
  };
  for (const rt::Response& response : responses) {
    auto body = [response]() -> rt::Task<void> {
      const auto resetProvider = provider([response](rt::RequestInfo, rt::RequestInit) { return response; });
      rt::Task<RedeemResetResult> redeeming = resetProvider->redeem(redeem(), signal());
      const RedeemResetResult redeemed = co_await std::move(redeeming);
      EXPECT(redeemed == unknownWithoutReceipt);
    };
    rt::Task<void> running = step(rt::numberToString(response.status()), body);
    co_await std::move(running);
  }
}

ASYNC_TEST("upstream reset adapter rejects malformed inventory and makes no transport call for invalid inputs") {
  const auto badInventory = provider([](rt::RequestInfo, rt::RequestInit) { return textResponse("{}", 200); });
  rt::Task<ResetInventory> bad = badInventory->readInventory(context(), signal());
  ASSERT_REJECTS_TYPE(std::move(bad), CodexUsageResetProviderConfigurationError);
  const auto unavailableInventory = provider([](rt::RequestInfo, rt::RequestInit) {
    rt::ResponseInit init;
    init.status = 503;
    return rt::Response(nullptr, init);
  });
  rt::Task<ResetInventory> unavailableRead = unavailableInventory->readInventory(context(), signal());
  const std::function<bool(const CodexUsageResetProviderHttpError&)> is503 = [](const CodexUsageResetProviderHttpError& error) { return error.status == 503; };
  ASSERT_REJECTS_IF(std::move(unavailableRead), CodexUsageResetProviderHttpError, is503);

  auto calls = std::make_shared<double>(0);
  const auto resetProvider = provider([calls](rt::RequestInfo, rt::RequestInit) {
    *calls += 1;
    return textResponse(R"({"code":"reset"})", 200);
  });
  rt::Task<RedeemResetResult> otherAccount = resetProvider->redeem(redeem("different-account"), signal());
  ASSERT_REJECTS_TYPE(std::move(otherAccount), CodexUsageResetProviderConfigurationError);
  RedeemResetInput blank = redeem();
  blank.idempotencyKey = "   ";
  rt::Task<RedeemResetResult> blankKey = resetProvider->redeem(blank, signal());
  ASSERT_REJECTS_TYPE(std::move(blankKey), CodexUsageResetProviderConfigurationError);
  const rt::AbortController controller;
  controller.abort(rt::makeReason(rt::JsError("cancelled")));
  rt::Task<RedeemResetResult> aborted = resetProvider->redeem(redeem(), controller.signal());
  ASSERT_REJECTS(std::move(aborted), "");
  EXPECT_EQ(*calls, 0);
}

ASYNC_TEST("upstream reset adapter rejects summary-only, capped, malformed, or duplicate detailed inventories") {
  const json::object completeCredit{{"id", "credit-one"}, {"reset_type", "codex_rate_limits"}, {"status", "available"}, {"expires_at", nullptr}};
  json::object malformedExpiry = completeCredit;
  malformedExpiry["expires_at"] = "not-a-date";
  const std::vector<std::pair<std::string, json::object>> cases{
    {"summary only", json::object{{"available_count", 1}, {"credits", nullptr}}},
    {"capped details", json::object{{"available_count", 2}, {"credits", json::array{completeCredit}}}},
    {"malformed expiry", json::object{{"available_count", 1}, {"credits", json::array{malformedExpiry}}}},
    {"duplicate opaque ID", json::object{{"available_count", 2}, {"credits", json::array{completeCredit, completeCredit}}}},
  };
  for (const auto& [name, payload] : cases) {
    auto body = [payload]() -> rt::Task<void> {
      const auto resetProvider = provider([payload](rt::RequestInfo, rt::RequestInit) { return textResponse(rt::jsonStringify(payload), 200); });
      rt::Task<ResetInventory> reading = resetProvider->readInventory(context(), signal());
      ASSERT_REJECTS_TYPE(std::move(reading), CodexUsageResetProviderConfigurationError);
    };
    rt::Task<void> running = step(name, body);
    co_await std::move(running);
  }
}

ASYNC_TEST("display reset counts use only inventory GET and preserve zero and unavailable") {
  const std::vector<double> counts{0.0, 3.0};
  for (const double count : counts) {
    UpstreamCodexUsageResetProviderOptions options{
      .codexBaseUrl = "https://chatgpt.com/backend-api/codex",
      .accountId = "account-one",
      .accessToken = "test-token",
      .userAgent = "test-agent",
      .fetch = [count](rt::RequestInfo url, rt::RequestInit init) {
        std::string requestedUrl;
        if (const auto* text = std::get_if<std::string>(&url)) {
          requestedUrl = *text;
        } else if (const auto* parsed = std::get_if<rt::URL>(&url)) {
          requestedUrl = parsed->href();
        } else {
          requestedUrl = std::get<rt::Request>(url).url();
        }
        EXPECT_EQ(requestedUrl, "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits");
        EXPECT_EQ(init.method, std::optional<std::string>("GET"));
        EXPECT_EQ(rt::Headers(init.headers.value_or(rt::Headers())).get("ChatGPT-Account-ID"), std::optional<std::string>("account-one"));
        return rt::Response::json(json::object{{"available_count", rt::makeNumber(count)}});
      },
      .now = {},
    };
    rt::Task<double> reading = readCodexResetAvailableCount(options, signal());
    const double value = co_await std::move(reading);
    EXPECT_EQ(value, count);
  }
  const std::vector<json::value> payloads{json::value(json::object{}), json::value(json::object{{"available_count", -1}}), json::value(json::object{{"available_count", "3"}})};
  for (const json::value& payload : payloads) {
    UpstreamCodexUsageResetProviderOptions options{
      .codexBaseUrl = "https://chatgpt.com/backend-api/codex",
      .accountId = "account-one",
      .accessToken = "test-token",
      .userAgent = "test-agent",
      .fetch = [payload](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(payload); },
      .now = {},
    };
    rt::Task<double> reading = readCodexResetAvailableCount(options, signal());
    ASSERT_REJECTS(std::move(reading), "count was invalid");
  }
}
