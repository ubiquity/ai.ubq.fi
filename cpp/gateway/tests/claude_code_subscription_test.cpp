// Links: tests/claude_code_subscription_test.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/responses_stream.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/utils.cpp
// Port of tests/claude-code-subscription.test.ts. QuotaKv is the test's minimal Kv stand-in: atomic checks are
// ignored, writes land in insertion order, and every present key reads back with versionstamp ...01.
#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
namespace cc = provider::claude_code;
namespace credentials = provider::claude_code_credentials;
namespace quota = provider::claude_code_quota;
using provider::claude_code::ClaudeCodeEnvLookup;

namespace {
  std::string keyOf(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.emplace_back(json::string(part.string()));
    return rt::jsonStringify(json::value(std::move(parts)));
  }

  /** Minimal Kv stand-in for the keys this tier reads and writes. */
  class QuotaKv final : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    bool failWrites = false;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      const rt::KvValue* stored = values.get(keyOf(key));
      rt::KvEntryMaybe<> entry;
      entry.key = key;
      entry.value = stored != nullptr ? *stored : rt::KvValue(nullptr);
      if (stored != nullptr) entry.versionstamp = "00000000000000000001";
      co_return entry;
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override {
      throw rt::TypeError("kv.getMany is not a function");
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) override {
      if (failWrites) throw rt::JsError("KV write failed");
      values.set(keyOf(key), std::move(value));
      rt::KvCommitResult result;
      result.ok = true;
      result.versionstamp = "00000000000000000001";
      co_return result;
    }
    rt::Task<void> delete_(rt::KvKey key) override {
      values.delete_(keyOf(key));
      co_return;
    }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw rt::TypeError("kv.list is not a function"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      return std::make_unique<rt::GenericAtomicOperation>(
        [this](std::vector<rt::AtomicCheck>, std::vector<rt::KvMutation> mutations) { return commit(this, std::move(mutations)); });
    }
    void close() override {}

  private:
    static rt::Task<rt::KvCommitResult> commit(QuotaKv* self, std::vector<rt::KvMutation> mutations) {
      if (self->failWrites) throw rt::JsError("KV write failed");
      for (rt::KvMutation& mutation : mutations) self->values.set(keyOf(mutation.key), std::move(mutation.value));
      rt::KvCommitResult result;
      result.ok = true;
      result.versionstamp = "00000000000000000002";
      co_return result;
    }
  };

  ClaudeCodeEnvLookup envLookup(std::vector<std::pair<std::string, std::string>> values) {
    return [values](const std::string& name) -> std::optional<std::string> {
      for (const auto& [key, value] : values)
        if (key == name) return value;
      return std::nullopt;
    };
  }

  rt::OrderedSet<std::string> allowed() {
    rt::OrderedSet<std::string> ids;
    ids.add("owner-key");
    return ids;
  }

  const credentials::ClaudeCodeResolvedCredential oauthCredential{
    .kind = "oauth", .secret = "token-1", .hint = "token-1", .label = std::nullopt, .source = "CLAUDE_CODE_OAUTH_TOKEN", .updated_at_ms = std::nullopt};

  quota::ClaudeCodeQuotaObservation emptyObservation(double nowMs) {
    return {.windows = {}, .standard = {}, .representative_claim = std::nullopt, .observed_at_ms = nowMs};
  }

  std::vector<std::string> setItems(const rt::OrderedSet<std::string>& set) {
    std::vector<std::string> out;
    for (const std::string& item : set) out.push_back(item);
    return out;
  }

  cc::ClaudeCodeEligibilityInput eligibilityBase() {
    return {
      .model = json::value("claude-opus-5-5"),
      .keyId = "owner-key",
      .providerEnabled = true,
      .allowedKeyIds = allowed(),
      .credential = oauthCredential,
      .quota = std::nullopt,
    };
  }

  bool isNotEligible(const cc::ClaudeCodeEligibility& eligibility, std::string_view reason) {
    const auto* result = std::get_if<cc::ClaudeCodeEligibilityNotEligible>(&eligibility);
    return result != nullptr && result->reason == reason;
  }
  bool isEligible(const cc::ClaudeCodeEligibility& eligibility, const credentials::ClaudeCodeResolvedCredential& credential) {
    const auto* result = std::get_if<cc::ClaudeCodeEligibilityEligible>(&eligibility);
    return result != nullptr && result->credential == credential && !result->workspace.has_value();
  }

  bool classified(const cc::ClaudeCodeResponseClassification& actual, std::string_view kind, std::optional<quota::ClaudeCodeQuotaExhaustion> exhaustion,
                  std::optional<double> retryAfterMs, std::optional<std::string> detail) {
    return actual.kind == kind && actual.exhaustion == exhaustion && actual.retry_after_ms == retryAfterMs && actual.detail == detail;
  }

  std::optional<std::string> errorBody(std::string_view type, std::string_view message) {
    json::object error;
    error["type"] = json::string(type);
    error["message"] = json::string(message);
    json::object body;
    body["type"] = "error";
    body["error"] = std::move(error);
    return rt::jsonStringify(json::value(std::move(body)));
  }

  json::object requestBody(std::string_view text) { return rt::jsonParse(text).as_object(); }

  const json::array& systemBlocks(const json::object& body) {
    static const json::array empty;
    const json::array* system = rt::getArray(body, "system");
    return system != nullptr ? *system : empty;
  }
}

TEST("credential and allowlist reads are explicit and empty by default") {
  const std::vector<std::pair<std::string, std::string>> emptyValues;
  const auto emptyLookup = envLookup(emptyValues);
  EXPECT(!credentials::readClaudeCodeEnvCredential(emptyLookup).has_value());
  const std::vector<std::pair<std::string, std::string>> blankValues{{"CLAUDE_CODE_OAUTH_TOKEN", "  "}};
  const auto blankLookup = envLookup(blankValues);
  EXPECT(!credentials::readClaudeCodeEnvCredential(blankLookup).has_value());
  const std::vector<std::pair<std::string, std::string>> tokenValues{{"CLAUDE_CODE_OAUTH_TOKEN", " token-1 "}};
  const auto tokenLookup = envLookup(tokenValues);
  const auto token = credentials::readClaudeCodeEnvCredential(tokenLookup);
  ASSERT(token.has_value());
  EXPECT_EQ(token->secret, std::string("token-1"));
  EXPECT_EQ(token->kind, std::string("oauth"));
  EXPECT_EQ(token->source, std::string("CLAUDE_CODE_OAUTH_TOKEN"));
  const auto absentIds = cc::parseClaudeCodeAllowedKeyIds(std::nullopt);
  const auto absentItems = setItems(absentIds);
  EXPECT(absentItems.empty());
  const auto blankIds = cc::parseClaudeCodeAllowedKeyIds(std::string("  "));
  const auto blankItems = setItems(blankIds);
  EXPECT(blankItems.empty());
  const auto parsedIds = cc::parseClaudeCodeAllowedKeyIds(std::string("a, b\n c"));
  const auto parsedItems = setItems(parsedIds);
  const std::vector<std::string> expectedParsedItems{"a", "b", "c"};
  EXPECT(parsedItems == expectedParsedItems);
  const std::vector<std::pair<std::string, std::string>> allowedValues{{"CLAUDE_CODE_ALLOWED_KEY_IDS", "owner-key"}};
  const auto allowedLookup = envLookup(allowedValues);
  const auto allowedIds = cc::readClaudeCodeAllowedKeyIds(allowedLookup);
  const auto allowedItems = setItems(allowedIds);
  const std::vector<std::string> expectedAllowedItems{"owner-key"};
  EXPECT(allowedItems == expectedAllowedItems);
}

TEST("CLAUDE_API_KEY is the primary environment credential and wins over the legacy OAuth token") {
  const std::vector<std::pair<std::string, std::string>> apiValues{{"CLAUDE_API_KEY", " sk-ant-usr-1A6exampleBwAA "}};
  const auto apiLookup = envLookup(apiValues);
  const auto apiKey = credentials::readClaudeCodeEnvCredential(apiLookup);
  ASSERT(apiKey.has_value());
  EXPECT_EQ(apiKey->kind, std::string("api_key"));
  EXPECT_EQ(apiKey->secret, std::string("sk-ant-usr-1A6exampleBwAA"));
  EXPECT_EQ(apiKey->source, std::string("CLAUDE_API_KEY"));
  const std::vector<std::pair<std::string, std::string>> bothValues{{"CLAUDE_API_KEY", "sk-ant-usr-key"}, {"CLAUDE_CODE_OAUTH_TOKEN", "sk-ant-oat01-token"}};
  const auto bothLookup = envLookup(bothValues);
  const auto both = credentials::readClaudeCodeEnvCredential(bothLookup);
  EXPECT(both.has_value() && both->source == "CLAUDE_API_KEY");
  const std::vector<std::pair<std::string, std::string>> oauthValues{{"CLAUDE_CODE_OAUTH_TOKEN", "sk-ant-oat01-token"}};
  const auto oauthLookup = envLookup(oauthValues);
  const auto oauthOnly = credentials::readClaudeCodeEnvCredential(oauthLookup);
  EXPECT(oauthOnly.has_value() && oauthOnly->kind == "oauth");
}

TEST("credential kinds and partial hints never expose the secret") {
  EXPECT_EQ(cc::claudeCodeCredentialKind("sk-ant-api03-abc"), std::string("api_key"));
  EXPECT_EQ(cc::claudeCodeCredentialKind("sk-ant-usr-1A6abc"), std::string("api_key"));
  EXPECT_EQ(cc::claudeCodeCredentialKind("sk-ant-oat01-abc"), std::string("oauth"));
  EXPECT_EQ(cc::claudeCodeCredentialKind("sk-ant-ort01-abc"), std::string("oauth"));
  EXPECT_EQ(cc::claudeCodeCredentialKind("something-else"), std::string("api_key"));
  const std::string hint = credentials::partialClaudeCodeCredentialHint("sk-ant-usr-1A6ABCDEFGHIJKLMNOPBwAA");
  EXPECT_EQ(hint, std::string("sk-ant-usr-1A6...BwAA"));
  // the middle of the secret never appears in a hint
  EXPECT(hint.find("ABCDEFGH") == std::string::npos);
  EXPECT_EQ(credentials::partialClaudeCodeCredentialHint("short"), std::string("configured"));
}

ASYNC_TEST("a stored credential beats the bootstrapped environment and removal never resurrects it") {
  credentials::resetClaudeCodeCredentialCacheForTest();
  const auto kv = std::make_shared<QuotaKv>();
  const ClaudeCodeEnvLookup env = envLookup({{"CLAUDE_API_KEY", "sk-ant-usr-env-key"}});
  auto resolvingEnv = credentials::resolveClaudeCodeCredential(kv, env);
  const auto fromEnv = co_await std::move(resolvingEnv);
  EXPECT(fromEnv.has_value() && fromEnv->source == "CLAUDE_API_KEY");

  const credentials::StoreClaudeCodeStoredCredentialInput input{.secret = "sk-ant-usr-kv-key", .label = "primary"};
  auto storing = credentials::storeClaudeCodeStoredCredential(kv, input);
  const auto stored = co_await std::move(storing);
  ASSERT(stored.has_value());
  EXPECT_EQ(stored->kind, std::string("api_key"));
  // a short secret is never partially revealed
  EXPECT_EQ(stored->hint, std::string("configured"));
  auto resolving = credentials::resolveClaudeCodeCredential(kv, env);
  const auto resolved = co_await std::move(resolving);
  ASSERT(resolved.has_value());
  EXPECT_EQ(resolved->source, std::string("kv"));
  EXPECT_EQ(resolved->secret, std::string("sk-ant-usr-kv-key"));
  EXPECT_EQ(resolved->label, std::optional<std::string>("primary"));
  EXPECT(resolved->updated_at_ms.has_value());
  EXPECT(kv->values.has(keyOf(provider::claude_code_accounts::CLAUDE_CODE_ACCOUNTS_KEY)));

  auto loading = credentials::loadClaudeCodeStoredCredential(kv);
  const auto loaded = co_await std::move(loading);
  EXPECT(loaded.has_value() && loaded->secret == "sk-ant-usr-kv-key");
  // new roster credentials do not overwrite retained singleton keys
  EXPECT(!kv->values.has(keyOf(credentials::CLAUDE_CODE_AUTH_KV_KEY)));

  auto clearing = credentials::clearClaudeCodeStoredCredential(kv);
  co_await std::move(clearing);
  auto resolvingAfter = credentials::resolveClaudeCodeCredential(kv, env);
  const auto afterClear = co_await std::move(resolvingAfter);
  EXPECT(!afterClear.has_value());
  EXPECT_EQ(credentials::CLAUDE_CODE_CREDENTIAL_CACHE_TTL_MS, 60'000.0);
  credentials::resetClaudeCodeCredentialCacheForTest();
}

ASYNC_TEST("workspace resolution prefers a KV pin, then the environment pin, then discovery state") {
  const auto kv = std::make_shared<QuotaKv>();
  auto none = credentials::resolveClaudeCodeWorkspace(kv, envLookup({}));
  const auto nothing = co_await std::move(none);
  EXPECT(!nothing.has_value());
  auto envResolving = credentials::resolveClaudeCodeWorkspace(kv, envLookup({{"CLAUDE_CODE_WORKSPACE_ID", "wrkspc_01FBEp5VyCHs7ghEnJqk6giF"}}));
  const auto fromEnv = co_await std::move(envResolving);
  EXPECT(fromEnv == std::optional(credentials::ClaudeCodeResolvedWorkspace{.id = "wrkspc_01FBEp5VyCHs7ghEnJqk6giF", .name = std::nullopt, .source = "env"}));

  const credentials::ClaudeCodeWorkspaceState discovered = credentials::mergeClaudeCodeWorkspaceState(
    std::nullopt, credentials::MergeClaudeCodeWorkspaceStateInput{.id = "wrkspc_01ErorW3iFLRvQy55cpNWtEt", .name = "Default", .source = "discovered"});
  auto storingDiscovered = credentials::storeClaudeCodeWorkspaceState(kv, discovered);
  co_await std::move(storingDiscovered);
  auto discoveryResolving = credentials::resolveClaudeCodeWorkspace(kv, envLookup({}));
  const auto fromDiscovery = co_await std::move(discoveryResolving);
  EXPECT(fromDiscovery == std::optional(credentials::ClaudeCodeResolvedWorkspace{.id = "wrkspc_01ErorW3iFLRvQy55cpNWtEt", .name = "Default", .source = "discovered"}));
  auto loadingState = credentials::loadClaudeCodeWorkspaceState(kv);
  const auto loadedState = co_await std::move(loadingState);
  EXPECT(loadedState.has_value() && loadedState->name == std::optional<std::string>("Default"));

  const credentials::ClaudeCodeWorkspaceState pinned = credentials::mergeClaudeCodeWorkspaceState(
    discovered, credentials::MergeClaudeCodeWorkspaceStateInput{.id = "wrkspc_01FBEp5VyCHs7ghEnJqk6giF", .source = "kv", .verified = true});
  auto storingPinned = credentials::storeClaudeCodeWorkspaceState(kv, pinned);
  co_await std::move(storingPinned);
  auto pinnedResolving = credentials::resolveClaudeCodeWorkspace(kv, envLookup({}));
  const auto resolved = co_await std::move(pinnedResolving);
  ASSERT(resolved.has_value());
  EXPECT_EQ(resolved->id, std::string("wrkspc_01FBEp5VyCHs7ghEnJqk6giF"));
  EXPECT_EQ(resolved->source, std::string("kv"));
  auto loadingPinned = credentials::loadClaudeCodeWorkspaceState(kv);
  const auto loadedPinned = co_await std::move(loadingPinned);
  EXPECT(loadedPinned.has_value() && loadedPinned->verified_at_ms.has_value());

  EXPECT_EQ(credentials::parseClaudeCodeWorkspaceId(std::string_view("wrkspc_abc")), std::optional<std::string>("wrkspc_abc"));
  EXPECT_EQ(credentials::parseClaudeCodeWorkspaceId(std::string_view("abc")), std::nullopt);
  EXPECT_EQ(credentials::parseClaudeCodeWorkspaceId(std::string_view("  ")), std::nullopt);
  EXPECT(kv->values.has(keyOf(credentials::CLAUDE_CODE_WORKSPACE_KV_KEY)));
}

TEST("eligibility is the conjunction of model, provider, principal, credential, and quota gates") {
  const cc::ClaudeCodeEligibilityInput base = eligibilityBase();
  EXPECT(isEligible(cc::resolveClaudeCodeEligibility(base), oauthCredential));
  cc::ClaudeCodeEligibilityInput otherModel = base;
  otherModel.model = json::value("gpt-5.6-sol");
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(otherModel), "model_unsupported"));
  cc::ClaudeCodeEligibilityInput disabled = base;
  disabled.providerEnabled = false;
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(disabled), "provider_disabled"));
  cc::ClaudeCodeEligibilityInput anonymous = base;
  anonymous.keyId = std::nullopt;
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(anonymous), "principal_not_allowed"));
  cc::ClaudeCodeEligibilityInput otherKey = base;
  otherKey.keyId = "other-key";
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(otherKey), "principal_not_allowed"));
  cc::ClaudeCodeEligibilityInput noCredential = base;
  noCredential.credential = std::nullopt;
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(noCredential), "credential_missing"));
}

TEST("an active durable block or cooldown removes eligibility until it expires") {
  const double nowMs = 1'800'000'000'000;
  quota::ClaudeCodeQuotaState blocked{
    .windows = {},
    .standard = {},
    .usage = std::nullopt,
    .representative_claim = std::nullopt,
    .blocked_until_ms = nowMs + 60'000,
    .block_window = "5h",
    .cooldown_until_ms = std::nullopt,
    .last_error = std::nullopt,
    .updated_at_ms = nowMs,
  };
  cc::ClaudeCodeEligibilityInput base = eligibilityBase();
  base.nowMs = nowMs;
  cc::ClaudeCodeEligibilityInput blockedInput = base;
  blockedInput.quota = blocked;
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(blockedInput), "quota_blocked"));
  quota::ClaudeCodeQuotaState expired = blocked;
  expired.blocked_until_ms = nowMs - 1;
  cc::ClaudeCodeEligibilityInput expiredInput = base;
  expiredInput.quota = expired;
  EXPECT(isEligible(cc::resolveClaudeCodeEligibility(expiredInput), oauthCredential));
  quota::ClaudeCodeQuotaState cooling = blocked;
  cooling.blocked_until_ms = std::nullopt;
  cooling.cooldown_until_ms = nowMs + 1'000;
  cc::ClaudeCodeEligibilityInput coolingInput = base;
  coolingInput.quota = cooling;
  EXPECT(isNotEligible(cc::resolveClaudeCodeEligibility(coolingInput), "cooling_down"));
}

TEST("observations read the documented unified headers and reject malformed values") {
  const rt::Headers headers{
    {"anthropic-ratelimit-unified-5h-status", "allowed"},
    {"anthropic-ratelimit-unified-5h-utilization", "0.25"},
    {"anthropic-ratelimit-unified-5h-reset", "1783180800"},
    {"anthropic-ratelimit-unified-representative-claim", "five_hour"},
  };
  const auto observation = quota::observeClaudeCodeQuota(headers, 1'800'000'000'000);
  ASSERT(observation.has_value());
  const quota::ClaudeCodeQuotaWindow* fiveHour = observation->windows.get("5h");
  ASSERT(fiveHour != nullptr);
  EXPECT(*fiveHour == (quota::ClaudeCodeQuotaWindow{.status = "allowed", .utilization = 0.25, .reset_at_ms = 1'783'180'800'000, .observed_at_ms = 1'800'000'000'000}));
  EXPECT_EQ(observation->representative_claim, std::optional<std::string>("five_hour"));
  EXPECT(!quota::observeClaudeCodeQuota(rt::Headers(), 1'800'000'000'000).has_value());
  const rt::Headers malformed{{"anthropic-ratelimit-unified-5h-utilization", "not-a-number"}, {"anthropic-ratelimit-unified-5h-reset", "-5"}};
  EXPECT(!quota::observeClaudeCodeQuota(malformed, 1'800'000'000'000).has_value());
  const auto statusOnly = quota::observeClaudeCodeQuota(rt::Headers{{"anthropic-ratelimit-unified-7d-status", "limited"}}, 1'800'000'000'000);
  ASSERT(statusOnly.has_value());
  const quota::ClaudeCodeQuotaWindow* sevenDay = statusOnly->windows.get("7d");
  ASSERT(sevenDay != nullptr);
  EXPECT(*sevenDay == (quota::ClaudeCodeQuotaWindow{.status = "limited", .utilization = std::nullopt, .reset_at_ms = std::nullopt, .observed_at_ms = 1'800'000'000'000}));
}

TEST("standard per-family rate limits are captured for API-key credentials") {
  const rt::Headers headers{
    {"anthropic-ratelimit-requests-limit", "50"},
    {"anthropic-ratelimit-requests-remaining", "49"},
    {"anthropic-ratelimit-requests-reset", "1783180800"},
    {"anthropic-ratelimit-input-tokens-limit", "40000"},
    {"anthropic-ratelimit-input-tokens-remaining", "39000"},
    {"anthropic-ratelimit-output-tokens-limit", "8000"},
  };
  const auto observation = quota::observeClaudeCodeQuota(headers, 1'800'000'000'000);
  ASSERT(observation.has_value());
  const quota::ClaudeCodeStandardWindow* requests = observation->standard.get("requests");
  ASSERT(requests != nullptr);
  EXPECT(*requests == (quota::ClaudeCodeStandardWindow{.limit = 50, .remaining = 49, .reset_at_ms = 1'783'180'800'000, .observed_at_ms = 1'800'000'000'000}));
  const quota::ClaudeCodeStandardWindow* input = observation->standard.get("input-tokens");
  EXPECT(input != nullptr && input->limit == std::optional<double>(40'000));
  const quota::ClaudeCodeStandardWindow* output = observation->standard.get("output-tokens");
  EXPECT(output != nullptr && !output->remaining.has_value());
  EXPECT_EQ(observation->windows.size(), std::size_t{0});
}

TEST("response classification keeps durable exhaustion, transient limits, and auth failures apart") {
  EXPECT(classified(cc::classifyClaudeCodeResponse(200, rt::Headers()), "ok", std::nullopt, std::nullopt, std::nullopt));
  const rt::Headers durable{{"anthropic-ratelimit-unified-5h-status", "limited"}, {"anthropic-ratelimit-unified-5h-reset", "1783180800"}};
  EXPECT(classified(cc::classifyClaudeCodeResponse(429, durable), "durable_exhaustion",
                    quota::ClaudeCodeQuotaExhaustion{.window = "5h", .reset_at_ms = 1'783'180'800'000}, std::nullopt, std::nullopt));
  const rt::Headers utilizationFull{{"anthropic-ratelimit-unified-7d-utilization", "1"}, {"anthropic-ratelimit-unified-7d-reset", "1783180800"}};
  EXPECT(cc::classifyClaudeCodeResponse(429, utilizationFull).exhaustion ==
         std::optional(quota::ClaudeCodeQuotaExhaustion{.window = "7d", .reset_at_ms = 1'783'180'800'000}));
  const rt::Headers transient{{"retry-after", "30"}};
  EXPECT(classified(cc::classifyClaudeCodeResponse(429, transient), "transient_rate_limited", std::nullopt, 30'000, std::nullopt));
  const rt::Headers transientAllowed{{"anthropic-ratelimit-unified-5h-status", "allowed"}, {"anthropic-ratelimit-unified-5h-utilization", "0.9"}};
  EXPECT_EQ(cc::classifyClaudeCodeResponse(429, transientAllowed).kind, std::string("transient_rate_limited"));
  EXPECT_EQ(cc::classifyClaudeCodeResponse(401, rt::Headers()).kind, std::string("auth_invalid"));
  EXPECT_EQ(cc::classifyClaudeCodeResponse(403, rt::Headers()).kind, std::string("forbidden"));
  EXPECT_EQ(cc::classifyClaudeCodeResponse(529, rt::Headers()).kind, std::string("upstream_error"));
  EXPECT_EQ(cc::classifyClaudeCodeResponse(400, rt::Headers()).kind, std::string("request_rejected"));
}

TEST("error bodies separate workspace, billing, and credential scoping rejections from generic failures") {
  const cc::ClaudeCodeResponseClassification rejected = cc::classifyClaudeCodeResponse(400, rt::Headers());
  const auto workspaceRequired = cc::classifyClaudeCodeErrorBody(
    rejected, errorBody("invalid_request_error",
                        "This API key is not scoped to a workspace, so this request must include the anthropic-workspace-id header with the ID of the workspace to use."));
  EXPECT_EQ(workspaceRequired.kind, std::string("workspace_required"));
  EXPECT(workspaceRequired.detail && workspaceRequired.detail->find("not scoped to a workspace") != std::string::npos);

  const auto notFound = cc::classifyClaudeCodeErrorBody(cc::classifyClaudeCodeResponse(404, rt::Headers()),
                                                        errorBody("not_found_error", "Workspace `72cde404-5c32-4031-b07f-26df74e0980c` not found."));
  EXPECT_EQ(notFound.kind, std::string("workspace_not_found"));

  const auto billing = cc::classifyClaudeCodeErrorBody(
    rejected, errorBody("invalid_request_error",
                        "Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits."));
  EXPECT_EQ(billing.kind, std::string("billing_blocked"));

  const auto generic = cc::classifyClaudeCodeErrorBody(rejected, errorBody("invalid_request_error", "max_tokens: must be at least 1"));
  EXPECT_EQ(generic.kind, std::string("request_rejected"));
  EXPECT(generic.detail && generic.detail->rfind("max_tokens", 0) == 0);
  EXPECT_EQ(cc::classifyClaudeCodeErrorBody(cc::classifyClaudeCodeResponse(200, rt::Headers()), std::nullopt).kind, std::string("ok"));
}

TEST("quota state merges newest window observations and never resurrects stale windows") {
  quota::ClaudeCodeQuotaObservation early = emptyObservation(1'800'000'000'000);
  early.windows.set("5h", quota::ClaudeCodeQuotaWindow{.status = "allowed", .utilization = 0.1, .reset_at_ms = 1'783'000'000'000, .observed_at_ms = 1'800'000'000'000});
  quota::ClaudeCodeQuotaObservation later = emptyObservation(1'800'000'100'000);
  later.windows.set("5h", quota::ClaudeCodeQuotaWindow{.status = "allowed", .utilization = 0.4, .reset_at_ms = 1'783'000'000'000, .observed_at_ms = 1'800'000'100'000});
  later.representative_claim = "five_hour";
  const quota::ClaudeCodeQuotaState first = quota::mergeClaudeCodeQuotaState(std::nullopt, early, std::nullopt, 1'800'000'000'000);
  const quota::ClaudeCodeQuotaState merged = quota::mergeClaudeCodeQuotaState(first, later, std::nullopt, 1'800'000'100'000);
  EXPECT(merged.windows.get("5h") != nullptr && merged.windows.get("5h")->utilization == std::optional<double>(0.4));
  EXPECT_EQ(merged.representative_claim, std::optional<std::string>("five_hour"));
  const quota::ClaudeCodeQuotaState stale = quota::mergeClaudeCodeQuotaState(merged, early, std::nullopt, 1'800'000'200'000);
  // an older observation never replaces a newer one
  EXPECT(stale.windows.get("5h") != nullptr && stale.windows.get("5h")->utilization == std::optional<double>(0.4));
}

TEST("durable blocks extend past the reset margin, survive merges, and expire") {
  const double nowMs = 1'800'000'000'000;
  const quota::ClaudeCodeQuotaObservation observation = emptyObservation(nowMs);
  const quota::ClaudeCodeQuotaState blocked =
    quota::mergeClaudeCodeQuotaState(std::nullopt, observation, quota::ClaudeCodeQuotaExhaustion{.window = "5h", .reset_at_ms = nowMs + 100'000}, nowMs);
  EXPECT_EQ(blocked.blocked_until_ms, std::optional<double>(nowMs + 100'000 + quota::CLAUDE_CODE_RESET_MARGIN_MS));
  EXPECT_EQ(blocked.block_window, std::optional<std::string>("5h"));
  EXPECT(quota::claudeCodeQuotaBlocked(blocked, nowMs + 1'000));
  const quota::ClaudeCodeQuotaState stillBlocked = quota::mergeClaudeCodeQuotaState(blocked, observation, std::nullopt, nowMs + 50'000);
  EXPECT_EQ(stillBlocked.blocked_until_ms, blocked.blocked_until_ms);
  const quota::ClaudeCodeQuotaState expired =
    quota::mergeClaudeCodeQuotaState(blocked, observation, std::nullopt, nowMs + 100'000 + quota::CLAUDE_CODE_RESET_MARGIN_MS + 1);
  EXPECT_EQ(expired.blocked_until_ms, std::nullopt);
  const quota::ClaudeCodeQuotaState noReset =
    quota::mergeClaudeCodeQuotaState(std::nullopt, observation, quota::ClaudeCodeQuotaExhaustion{.window = "7d", .reset_at_ms = std::nullopt}, nowMs);
  EXPECT_EQ(noReset.blocked_until_ms, std::optional<double>(nowMs + quota::CLAUDE_CODE_MIN_BLOCK_MS));
}

ASYNC_TEST("recorded outcomes persist windows, blocks, and cooldowns through KV") {
  const auto kv = std::make_shared<QuotaKv>();
  const double nowMs = 1'800'000'000'000;
  const rt::Headers headers{
    {"anthropic-ratelimit-unified-5h-status", "limited"},
    {"anthropic-ratelimit-unified-5h-reset", rt::numberToString(rt::Math::trunc((nowMs + 60'000) / 1'000))},
    {"anthropic-ratelimit-unified-5h-utilization", "1"},
  };
  const cc::ClaudeCodeResponseClassification classification = cc::classifyClaudeCodeResponse(429, headers, nowMs);
  quota::QuotaOutcome limited;
  limited.observation = quota::observeClaudeCodeQuota(headers, nowMs);
  limited.exhaustion = classification.exhaustion;
  limited.transient = std::nullopt;
  limited.nowMs = nowMs;
  auto recording = quota::recordClaudeCodeQuotaOutcome(kv, limited);
  const auto recorded = co_await std::move(recording);
  EXPECT(recorded.has_value() && recorded->blocked_until_ms == std::optional<double>(nowMs + 60'000 + quota::CLAUDE_CODE_RESET_MARGIN_MS));
  auto reloading = quota::loadClaudeCodeQuotaState(kv);
  const auto reloaded = co_await std::move(reloading);
  EXPECT(reloaded.has_value() && reloaded->windows.get("5h") != nullptr && reloaded->windows.get("5h")->utilization == std::optional<double>(1));
  EXPECT(quota::claudeCodeQuotaBlocked(reloaded, nowMs));

  quota::QuotaOutcome cooldown;
  cooldown.observation = std::nullopt;
  cooldown.exhaustion = std::nullopt;
  cooldown.transient = quota::QuotaOutcome::Transient{.retry_after_ms = std::nullopt};
  cooldown.nowMs = nowMs;
  auto recordingTransient = quota::recordClaudeCodeQuotaOutcome(kv, cooldown);
  const auto transient = co_await std::move(recordingTransient);
  ASSERT(transient.has_value());
  EXPECT_EQ(transient->cooldown_until_ms, std::optional<double>(nowMs + quota::CLAUDE_CODE_TRANSIENT_COOLDOWN_MS));
  // a transient limit never touches the durable block
  EXPECT_EQ(transient->blocked_until_ms, std::optional<double>(nowMs + 60'000 + quota::CLAUDE_CODE_RESET_MARGIN_MS));
  EXPECT(quota::claudeCodeQuotaCoolingDown(transient, nowMs + 1'000));
  auto reloadingState = quota::loadClaudeCodeQuotaState(kv);
  const auto reloadedState = co_await std::move(reloadingState);
  ASSERT(reloadedState.has_value());
  EXPECT_EQ(reloadedState->cooldown_until_ms, std::optional<double>(nowMs + quota::CLAUDE_CODE_TRANSIENT_COOLDOWN_MS));
  std::vector<std::string> keys;
  for (const auto& [key, value] : kv->values) keys.push_back(key);
  EXPECT(keys == (std::vector<std::string>{keyOf(quota::CLAUDE_CODE_QUOTA_KV_KEY)}));

  // `parseClaudeCodeQuotaState({ ...transient, blocked_until_ms: null })`
  json::value spread = json::value_from(*transient);
  spread.as_object()["blocked_until_ms"] = nullptr;
  const auto state = quota::parseClaudeCodeQuotaState(rt::KvValue(spread));
  ASSERT(state.has_value());
  EXPECT_EQ(state->blocked_until_ms, std::nullopt);
  EXPECT(!quota::parseClaudeCodeQuotaState(rt::KvValue(rt::jsonParse(R"({"windows":{},"updated_at_ms":0})"))).has_value());
}

ASYNC_TEST("a configuration rejection records its reason and a bounded cooldown, and a served request clears it") {
  const auto kv = std::make_shared<QuotaKv>();
  const double nowMs = 1'800'000'000'000;
  quota::QuotaOutcome billing;
  billing.observation = std::nullopt;
  billing.config_error = quota::QuotaOutcome::ConfigError{
    .kind = "billing_blocked", .status = 400, .message = "Your credit balance is too low to access the Anthropic API.", .retry_after_ms = std::nullopt};
  billing.nowMs = nowMs;
  auto recording = quota::recordClaudeCodeQuotaOutcome(kv, billing);
  const auto recorded = co_await std::move(recording);
  ASSERT(recorded.has_value() && recorded->last_error.has_value() && recorded->last_error->message.has_value());
  EXPECT_EQ(recorded->cooldown_until_ms, std::optional<double>(nowMs + quota::CLAUDE_CODE_CONFIG_COOLDOWN_MS));
  EXPECT_EQ(recorded->last_error->kind, std::string("billing_blocked"));
  EXPECT_EQ(recorded->last_error->status, std::optional<double>(400));
  EXPECT(recorded->last_error->message->find("credit balance") != std::string::npos);
  EXPECT(quota::claudeCodeQuotaCoolingDown(recorded, nowMs + 1'000));
  // a billing rejection is not a quota block
  EXPECT_EQ(recorded->blocked_until_ms, std::nullopt);

  quota::QuotaOutcome servedOutcome;
  servedOutcome.observation = std::nullopt;
  servedOutcome.served = true;
  servedOutcome.nowMs = nowMs + 20 * 60'000;
  auto serving = quota::recordClaudeCodeQuotaOutcome(kv, servedOutcome);
  const auto served = co_await std::move(serving);
  // serving the request clears the recorded configuration error
  EXPECT(served.has_value() && !served->last_error.has_value());

  auto clearing = quota::clearClaudeCodeQuotaGuards(kv);
  const auto cleared = co_await std::move(clearing);
  ASSERT(cleared.has_value());
  EXPECT_EQ(cleared->cooldown_until_ms, std::nullopt);
  EXPECT_EQ(cleared->blocked_until_ms, std::nullopt);
  EXPECT(!cleared->last_error.has_value());
  auto clearingNull = quota::clearClaudeCodeQuotaGuards(nullptr);
  const auto clearedNull = co_await std::move(clearingNull);
  EXPECT(!clearedNull.has_value());
}

ASYNC_TEST("configuration rejections never silently degrade into request-local cooldowns") {
  const auto kv = std::make_shared<QuotaKv>();
  const double nowMs = 1'800'000'000'000;
  quota::QuotaOutcome workspace;
  workspace.observation = std::nullopt;
  workspace.exhaustion = std::nullopt;
  workspace.transient = std::nullopt;
  workspace.config_error =
    quota::QuotaOutcome::ConfigError{.kind = "workspace_not_found", .status = 404, .message = "Workspace `72cde404` not found.", .retry_after_ms = 120'000};
  workspace.nowMs = nowMs;
  auto recording = quota::recordClaudeCodeQuotaOutcome(kv, workspace);
  const auto rejected = co_await std::move(recording);
  ASSERT(rejected.has_value() && rejected->last_error.has_value());
  // an explicit retry-after bounds the configuration cooldown
  EXPECT_EQ(rejected->cooldown_until_ms, std::optional<double>(nowMs + 120'000));
  EXPECT_EQ(rejected->last_error->kind, std::string("workspace_not_found"));
}

TEST("the native request builder defaults max_tokens, strips router hints, and refuses non-Claude models") {
  const auto built = cc::buildClaudeCodeRequest(
    requestBody(R"({"model":"claude-opus-5-5","input":"hello","stream":true,"instructions":"be brief","prompt_cache_key":"affinity-1"})"));
  ASSERT(built.has_value());
  EXPECT_EQ(rt::getString(built->body, "model"), std::optional<std::string>("claude-opus-5-5"));
  EXPECT_EQ(rt::getNumber(built->body, "max_tokens"), std::optional<double>(cc::CLAUDE_CODE_DEFAULT_MAX_OUTPUT_TOKENS));
  EXPECT(rt::get(built->body, "stream") != nullptr && rt::get(built->body, "stream")->is_bool() && rt::get(built->body, "stream")->get_bool());
  EXPECT(rt::get(built->body, "prompt_cache_key") == nullptr);
  EXPECT(rt::getArray(built->body, "messages") != nullptr && rt::getArray(built->body, "messages")->size() == 1);
  const auto explicitBuilt = cc::buildClaudeCodeRequest(requestBody(R"({"model":"claude-sonnet-5-5","input":"hi","max_output_tokens":4096})"));
  EXPECT(explicitBuilt.has_value() && rt::getNumber(explicitBuilt->body, "max_tokens") == std::optional<double>(4'096));
  EXPECT(!cc::buildClaudeCodeRequest(requestBody(R"({"model":"gpt-5.6-sol","input":"hi"})")).has_value());
}

TEST("the subscription lane leads its system blocks with the native attribution") {
  cc::BuildClaudeCodeRequestOptions subscriptionOptions;
  subscriptionOptions.systemAttribution = true;
  const auto subscription =
    cc::buildClaudeCodeRequest(requestBody(R"({"model":"claude-opus-5-5","input":"hello","instructions":"be brief"})"), subscriptionOptions);
  ASSERT(subscription.has_value());
  const json::array& system = systemBlocks(subscription->body);
  ASSERT(system.size() >= 2);
  EXPECT_EQ(rt::getString(system[0], "type"), std::optional<std::string>("text"));
  // the attribution must be the first system block
  EXPECT_EQ(rt::getString(system[0], "text"), std::optional<std::string>(std::string(cc::CLAUDE_CODE_SYSTEM_ATTRIBUTION)));
  // caller system content follows the attribution
  EXPECT_EQ(rt::getString(system[1], "text"), std::optional<std::string>("be brief"));

  cc::BuildClaudeCodeRequestOptions apiKeyOptions;
  apiKeyOptions.systemAttribution = false;
  const auto apiKey = cc::buildClaudeCodeRequest(requestBody(R"({"model":"claude-opus-5-5","input":"hello","instructions":"be brief"})"), apiKeyOptions);
  ASSERT(apiKey.has_value());
  bool claimsAttribution = false;
  for (const json::value& block : systemBlocks(apiKey->body))
    if (rt::getString(block, "text") == std::optional<std::string>(std::string(cc::CLAUDE_CODE_SYSTEM_ATTRIBUTION))) claimsAttribution = true;
  // an API key request never claims the native client attribution
  EXPECT(!claimsAttribution);
}

TEST("OAuth credentials carry Bearer plus the OAuth beta and never an API key header") {
  const rt::Headers streaming = cc::claudeCodeRequestHeaders(cc::ClaudeCodeAuthHeaders{.kind = "oauth", .secret = "token-1"}, cc::ClaudeCodeRequestHeadersOptions{.stream = true});
  EXPECT_EQ(streaming.get("authorization"), std::optional<std::string>("Bearer token-1"));
  EXPECT_EQ(streaming.get("anthropic-version"), std::optional<std::string>("2023-06-01"));
  EXPECT_EQ(streaming.get("anthropic-beta"), std::optional<std::string>(std::string(cc::CLAUDE_CODE_OAUTH_BETA)));
  EXPECT_EQ(streaming.get("accept"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(streaming.get("x-api-key"), std::nullopt);
  EXPECT_EQ(streaming.get("anthropic-workspace-id"), std::nullopt);
  EXPECT_EQ(cc::claudeCodeRequestHeaders(cc::ClaudeCodeAuthHeaders{.kind = "oauth", .secret = "token-1"}, cc::ClaudeCodeRequestHeadersOptions{.stream = false}).get("accept"),
            std::optional<std::string>("application/json"));
}

TEST("API-key credentials carry x-api-key, the workspace header when selected, and no OAuth beta") {
  const rt::Headers headers = cc::claudeCodeRequestHeaders(cc::ClaudeCodeAuthHeaders{.kind = "api_key", .secret = "sk-ant-usr-1A6exampleBwAA"},
                                                           cc::ClaudeCodeRequestHeadersOptions{.stream = false, .workspaceId = "wrkspc_01ErorW3iFLRvQy55cpNWtEt"});
  EXPECT_EQ(headers.get("x-api-key"), std::optional<std::string>("sk-ant-usr-1A6exampleBwAA"));
  EXPECT_EQ(headers.get("authorization"), std::nullopt);
  EXPECT_EQ(headers.get("anthropic-beta"), std::nullopt);
  EXPECT_EQ(headers.get("anthropic-version"), std::optional<std::string>("2023-06-01"));
  EXPECT_EQ(headers.get("anthropic-workspace-id"), std::optional<std::string>("wrkspc_01ErorW3iFLRvQy55cpNWtEt"));
  const rt::Headers noWorkspace =
    cc::claudeCodeRequestHeaders(cc::ClaudeCodeAuthHeaders{.kind = "api_key", .secret = "key"}, cc::ClaudeCodeRequestHeadersOptions{.stream = false, .workspaceId = std::nullopt});
  EXPECT_EQ(noWorkspace.get("anthropic-workspace-id"), std::nullopt);
}
