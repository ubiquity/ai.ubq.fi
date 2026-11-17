// Differential cases for src/codex/banked-reset-provider.ts paths the ported
// bun test does not reach: configuration errors, contract re-validation of
// injected (also malformed) contracts, the fail-closed provider, the display
// diagnostics list and the request the adapter sends. Expected values were
// recorded from the TypeScript module under Bun 1.4.0.
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::codex::banked_reset_provider;

  struct FakeProvider : CodexUsageResetProvider {
    explicit FakeProvider(json::value contract_) : CodexUsageResetProvider(std::move(contract_)) {}
    rt::Task<ResetInventory> readInventory(ResetAccountContext, rt::AbortSignal) override { throw std::logic_error("unused"); }
    rt::Task<RedeemResetResult> redeem(RedeemResetInput, rt::AbortSignal) override { throw std::logic_error("unused"); }
    rt::Task<RedeemResetResult> lookup(LookupRedeemResetInput, rt::AbortSignal) override { throw std::logic_error("unused"); }
    rt::Task<json::value> verifyApplied(ResetAccountContext, rt::AbortSignal) override { throw std::logic_error("unused"); }
  };

  rt::AbortSignal signal() { return rt::AbortController().signal(); }

  rt::Response respond(std::string body, double status = 200) {
    rt::ResponseInit init;
    init.status = status;
    return rt::Response(rt::BodyInit(std::move(body)), std::move(init));
  }

  UpstreamCodexUsageResetProviderOptions opts(CodexUsageResetFetch fetch) {
    return UpstreamCodexUsageResetProviderOptions{
      .codexBaseUrl = "https://chatgpt.com/backend-api/",
      .accountId = "  account-one  ",
      .accessToken = " token ",
      .userAgent = " agent ",
      .fetch = std::move(fetch),
      .now = [] { return 5.0; },
    };
  }

  ResetAccountContext account(std::string accountId) { return ResetAccountContext{.accountId = std::move(accountId), .accountIdHash = "", .credentialVersion = "", .quotaGeneration = ""}; }

  template <class T> rt::Task<std::string> outcome(rt::Task<T> task) {
    std::string text = "ok";
    try {
      co_await std::move(task);
    } catch (...) {
      text = rt::reasonToString(std::current_exception());
    }
    co_return text;
  }
}

TEST("differential: base URL configuration errors and accepted layouts") {
  const auto error = [](const std::string& url) -> std::string {
    try {
      resolveCodexUsageResetCreditEndpoints(url);
    } catch (const CodexUsageResetProviderConfigurationError& failure) {
      return failure.message;
    }
    return "no error";
  };
  EXPECT_EQ(error(""), "codexBaseUrl must be non-empty.");
  EXPECT_EQ(error("   "), "codexBaseUrl must be non-empty.");
  EXPECT_EQ(error("relative/path"), "codexBaseUrl must be an absolute URL.");
  EXPECT_EQ(error("https://user:pw@chatgpt.com/backend-api"), "codexBaseUrl must not include credentials, a query string, or a fragment.");
  EXPECT_EQ(error("https://chatgpt.com/backend-api?x=1"), "codexBaseUrl must not include credentials, a query string, or a fragment.");
  EXPECT_EQ(error("https://chatgpt.com/backend-api#f"), "codexBaseUrl must not include credentials, a query string, or a fragment.");
  EXPECT_EQ(error("https://chatgpt.com/backend-api/codex/extra"), "codexBaseUrl must use the /backend-api[/codex] or /api/codex layout.");
  const CodexUsageResetCreditEndpoints wham{
    .inventoryUrl = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
    .consumeUrl = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume",
  };
  const CodexUsageResetCreditEndpoints api{
    .inventoryUrl = "https://chatgpt.com/api/codex/rate-limit-reset-credits",
    .consumeUrl = "https://chatgpt.com/api/codex/rate-limit-reset-credits/consume",
  };
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/backend-api///") == wham);
  EXPECT(resolveCodexUsageResetCreditEndpoints("  https://chatgpt.com/backend-api  ") == wham);
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/api/codex/") == api);
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com") == api);
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/?") == api);
  EXPECT(resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/#") == api);
  EXPECT_EQ(CodexUsageResetProviderHttpError("inventory", 503.5).message, "Codex usage-reset inventory request failed with status 503.5.");
  EXPECT_EQ(CodexUsageResetProviderHttpError("inventory", 503.5).name, "CodexUsageResetProviderHttpError");
}

TEST("differential: injected contracts are re-validated, malformed ones fail closed") {
  struct Expected {
    const char* contract;
    bool live, final, receipts, type, blank, x;
  };
  const std::vector<Expected> cases{
    {R"({"idempotency":{"callerSupplied":true,"retentionMs":1000},"lookup":{"byIdempotencyKey":true,"byProviderReceiptId":false},"verification":{"independentlyVerifiable":true},"receiptIdsSafeToPersistAndLog":true,"supportedResetTypes":[" ","codex_rate_limits"]})",
     true, false, true, true, false, false},
    {R"({"idempotency":{"callerSupplied":1,"retentionMs":0},"lookup":{"byIdempotencyKey":true},"verification":{"independentlyVerifiable":true},"supportedResetTypes":["x"]})", false, false, false, false, false, true},
    {R"({"idempotency":{"callerSupplied":"yes"},"lookup":{},"verification":{},"redeemOutcomeIsFinal":true,"supportedResetTypes":["x"]})", true, true, false, false, false, true},
    {R"({"idempotency":{"callerSupplied":true},"lookup":{},"verification":{},"redeemOutcomeIsFinal":1,"receiptIdsSafeToPersistAndLog":1,"supportedResetTypes":["x"]})", false, false, false, false, false, true},
    {R"({"idempotency":{"callerSupplied":true},"lookup":{},"verification":{},"redeemOutcomeIsFinal":true,"supportedResetTypes":["  ",5]})", false, true, false, false, false, false},
    {R"({"idempotency":null,"supportedResetTypes":"not-an-array"})", false, false, false, false, false, false},
    {"[]", false, false, false, false, false, false},
    {"null", false, false, false, false, false, false},
  };
  for (const Expected& entry : cases) {
    const FakeProvider provider(rt::jsonParse(entry.contract));
    EXPECT_EQ(providerSupportsLiveRedemption(&provider), entry.live);
    EXPECT_EQ(providerTreatsRedeemOutcomeAsFinal(&provider), entry.final);
    EXPECT_EQ(providerReceiptIdsSafeToPersistAndLog(&provider), entry.receipts);
    EXPECT_EQ(providerSupportsResetType(&provider, json::value("codex_rate_limits")), entry.type);
    EXPECT_EQ(providerSupportsResetType(&provider, json::value(" ")), entry.blank);
    EXPECT_EQ(providerSupportsResetType(&provider, json::value("x")), entry.x);
  }
  EXPECT_EQ(providerSupportsLiveRedemption(nullptr), false);
  EXPECT_EQ(providerSupportsResetType(nullptr, json::value("x")), false);
}

TEST("differential: the fail-closed provider throws synchronously and advertises nothing") {
  const CodexUsageResetProvider* unavailable = unavailableCodexUsageResetProvider.get();
  EXPECT_EQ(rt::jsonStringify(unavailable->contract),
            R"({"idempotency":{"callerSupplied":false,"retentionMs":null},"lookup":{"byIdempotencyKey":false,"byProviderReceiptId":false},"verification":{"independentlyVerifiable":false},"redeemOutcomeIsFinal":false,"receiptIdsSafeToPersistAndLog":false,"supportedResetTypes":[]})");
  EXPECT_EQ(providerSupportsLiveRedemption(unavailable), false);
  EXPECT_EQ(providerTreatsRedeemOutcomeAsFinal(unavailable), false);
  EXPECT_EQ(providerReceiptIdsSafeToPersistAndLog(unavailable), false);
  EXPECT_THROWS_MESSAGE(unavailableCodexUsageResetProvider->readInventory(ResetAccountContext{}, signal()), CodexUsageResetProviderUnavailableError,
                        "Codex usage-reset provider is unavailable");
  try {
    (void)unavailableCodexUsageResetProvider->verifyApplied(ResetAccountContext{}, signal());
  } catch (const CodexUsageResetProviderUnavailableError& error) {
    EXPECT_EQ(error.code, "codex_usage_reset_provider_unavailable");
    EXPECT_EQ(error.name, "CodexUsageResetProviderUnavailableError");
  }
}

ASYNC_TEST("differential: display credits keep available ones ordered by expiry, never-expiring last") {
  const std::string inventory = R"({"available_count":3,"credits":[
    {"id":"a","reset_type":"t","status":"available","expires_at":"2026-01-02T00:00:00Z"},
    {"id":"b","reset_type":"t","status":"used","expires_at":"2026-01-01T00:00:00Z"},
    {"id":"c","reset_type":"t","status":"available","expires_at":null},
    {"id":"d","reset_type":"t","status":"available","expires_at":"Thu, 01 Jan 2026 00:00:00 GMT"},
    {"id":"e","reset_type":"t","status":"expired","expires_at":null}]})";
  rt::Task<std::vector<CodexResetCreditDiagnostic>> reading = readCodexResetCredits(opts([inventory](rt::RequestInfo, rt::RequestInit) { return respond(inventory); }), signal());
  const std::vector<CodexResetCreditDiagnostic> credits = co_await std::move(reading);
  EXPECT(credits == (std::vector<CodexResetCreditDiagnostic>{
                      {.status = "available", .expires_at_ms = 1767225600000},
                      {.status = "available", .expires_at_ms = 1767312000000},
                      {.status = "available", .expires_at_ms = std::nullopt},
                    }));
  const std::vector<std::pair<std::string, std::pair<std::string, double>>> failures{
    {"CodexUsageResetProviderConfigurationError: Reset-credit inventory did not include a complete detailed available-credit list.",
     {R"({"available_count":1,"credits":[{"id":"a","reset_type":"t","status":"available","expires_at":"1969-12-31T23:59:59Z"}]})", 200}},
    {"CodexUsageResetProviderConfigurationError: Reset-credit inventory did not include a complete detailed available-credit list.",
     {R"({"available_count":1,"credits":[{"id":"a","reset_type":"t","status":"available","expires_at":"  "}]})", 200}},
    {"CodexUsageResetProviderConfigurationError: Reset-credit inventory did not include a complete detailed available-credit list.",
     {R"({"available_count":1,"credits":[{"id":"a","reset_type":"t","status":"available"}]})", 200}},
    {"SyntaxError: Failed to parse JSON", {"{", 200}},
    {"CodexUsageResetProviderHttpError: Codex usage-reset inventory request failed with status 500.", {"x", 500}},
    {"CodexUsageResetProviderConfigurationError: Reset-credit inventory response was invalid.", {"[]", 200}},
  };
  for (const auto& [expected, response] : failures) {
    const auto responseFields = response;
    const auto& body = std::get<0>(responseFields);
    const auto& status = std::get<1>(responseFields);
    rt::Task<std::vector<CodexResetCreditDiagnostic>> failing =
      readCodexResetCredits(opts([body, status](rt::RequestInfo, rt::RequestInit) { return respond(body, status); }), signal());
    rt::Task<std::string> observing = outcome(std::move(failing));
    const std::string text = co_await std::move(observing);
    EXPECT_EQ(text, expected);
  }
}

ASYNC_TEST("differential: the upstream adapter trims bound options and sends a manual GET") {
  const auto offline = createUpstreamCodexUsageResetProvider(opts([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("no network"); }));
  EXPECT_EQ(rt::jsonStringify(offline->contract),
            R"({"idempotency":{"callerSupplied":true,"retentionMs":null},"lookup":{"byIdempotencyKey":false,"byProviderReceiptId":false},"verification":{"independentlyVerifiable":false},"redeemOutcomeIsFinal":true,"receiptIdsSafeToPersistAndLog":false,"supportedResetTypes":["codex_rate_limits"]})");
  rt::Task<RedeemResetResult> looking = offline->lookup(LookupRedeemResetInput(), signal());
  const RedeemResetResult looked = co_await std::move(looking);
  EXPECT(looked == RedeemResetResult(RedeemResetResultUnknown{.providerReceiptId = std::nullopt}));
  rt::Task<json::value> verifying = offline->verifyApplied(ResetAccountContext{}, signal());
  const json::value verified = co_await std::move(verifying);
  EXPECT_EQ(verified, json::value(false));
  rt::Task<std::string> trimmed = outcome(offline->readInventory(account("account-one"), signal()));
  const std::string trimmedText = co_await std::move(trimmed);
  EXPECT_EQ(trimmedText, "Error: no network");
  rt::Task<std::string> untrimmed = outcome(offline->readInventory(account("  account-one  "), signal()));
  const std::string untrimmedText = co_await std::move(untrimmed);
  EXPECT_EQ(untrimmedText, "CodexUsageResetProviderConfigurationError: Reset account does not match the provider-bound account.");

  auto seen = std::make_shared<std::vector<std::string>>();
  const auto recording = createUpstreamCodexUsageResetProvider(opts([seen](rt::RequestInfo input, rt::RequestInit init) {
    json::array headerEntries;
    for (const auto& [name, value] : init.headers.value_or(rt::Headers()).entries()) headerEntries.push_back(json::array{json::value(name), json::value(value)});
    json::array record{json::value(std::get<std::string>(input)), json::value(init.method.value_or("")), headerEntries, json::value(init.redirect.value_or("")),
                       init.body ? json::value(std::get<std::string>(*init.body)) : json::value(nullptr)};
    seen->push_back(rt::jsonStringify(record));
    return respond(R"({"available_count":0,"credits":[]})");
  }));
  rt::Task<ResetInventory> reading = recording->readInventory(account("account-one"), signal());
  const ResetInventory empty = co_await std::move(reading);
  EXPECT(empty == (ResetInventory{.availableCount = 0, .observedAtMs = 5, .credits = {}}));
  EXPECT_EQ(*seen, std::vector<std::string>({R"(["https://chatgpt.com/backend-api/wham/rate-limit-reset-credits","GET",[["authorization","Bearer token"],["chatgpt-account-id","account-one"],["user-agent","agent"]],"manual",null])"}));

  UpstreamCodexUsageResetProviderOptions fractionalNow = opts([](rt::RequestInfo, rt::RequestInit) { return respond(R"({"available_count":0,"credits":[]})"); });
  fractionalNow.now = [] { return 1.5; };
  rt::Task<std::string> badNow = outcome(createUpstreamCodexUsageResetProvider(fractionalNow)->readInventory(account("account-one"), signal()));
  const std::string badNowText = co_await std::move(badNow);
  EXPECT_EQ(badNowText, "CodexUsageResetProviderConfigurationError: now must return a non-negative safe integer.");
  const auto notJsonProvider = createUpstreamCodexUsageResetProvider(opts([](rt::RequestInfo, rt::RequestInit) { return respond("nope"); }));
  rt::Task<std::string> notJson = outcome(notJsonProvider->readInventory(account("account-one"), signal()));
  const std::string notJsonText = co_await std::move(notJson);
  EXPECT_EQ(notJsonText, "CodexUsageResetProviderConfigurationError: Reset-credit inventory response was not valid JSON.");
  for (const auto& [field, message] : std::vector<std::pair<std::string, std::string>>{{"accountId", "accountId must be non-empty."}, {"accessToken", "accessToken must be non-empty."}, {"userAgent", "userAgent must be non-empty."}}) {
    UpstreamCodexUsageResetProviderOptions blank = opts([](rt::RequestInfo, rt::RequestInit) { return respond(""); });
    (field == "accountId" ? blank.accountId : field == "accessToken" ? blank.accessToken : blank.userAgent) = "   ";
    EXPECT_THROWS_MESSAGE(createUpstreamCodexUsageResetProvider(blank), CodexUsageResetProviderConfigurationError, message);
  }
  RedeemResetInput blankCredit;
  blankCredit.accountId = "account-one";
  blankCredit.idempotencyKey = "k";
  blankCredit.creditId = " ";
  const auto redeeming = createUpstreamCodexUsageResetProvider(opts([](rt::RequestInfo, rt::RequestInit) { return respond(""); }));
  rt::Task<std::string> blankRedeem = outcome(redeeming->redeem(blankCredit, signal()));
  const std::string blankRedeemText = co_await std::move(blankRedeem);
  EXPECT_EQ(blankRedeemText, "CodexUsageResetProviderConfigurationError: creditId must be non-empty.");
}

TEST("codec: CodexResetCreditDiagnostic writes the snake_case display row") {
  EXPECT_EQ(rt::jsonStringify(json::value_from(CodexResetCreditDiagnostic{.status = "available", .expires_at_ms = std::nullopt})), R"({"status":"available","expires_at_ms":null})");
  EXPECT(json::value_to<CodexResetCreditDiagnostic>(rt::jsonParse(R"({"status":"available","expires_at_ms":5})")) ==
         (CodexResetCreditDiagnostic{.status = "available", .expires_at_ms = 5}));
}
