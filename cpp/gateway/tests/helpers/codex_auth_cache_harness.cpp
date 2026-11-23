#include "tests/helpers/codex_auth_cache_harness.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <utility>

namespace uos::tests::helpers::codex_auth_cache_harness {
  namespace {
    void require(bool condition, const std::string& message) {
      if (!condition) throw rt::JsError(message, "AssertionError");
    }
    std::string stamp(double version) { return rt::padStart(rt::numberToString(version), 20, "0"); }
  }
  std::string keyOf(const rt::KvKey& key) { return *rt::kvJsonStringify(rt::kvValueFromKey(key)); }
  bool isAuthKey(const rt::KvKey& key) { return keyOf(key) == keyOf(AUTH_KEY); }
  std::string probeErrorType(double status) {
    if (status == 429) return "usage_limit_reached";
    if (status == 401) return "authentication_error";
    return "forbidden";
  }
  bool isProviderHealthSuccessWrite(const FakeKvWrite& write) {
    return write.key.size() >= 3 && write.key[0] == rt::KvKeyPart("uos_ai") && write.key[1] == rt::KvKeyPart("provider_health") &&
           write.key[2] == rt::KvKeyPart("v1") && write.value["event"] == rt::KvValue("success");
  }
  AuthKv::AuthKv(::uos::types::CodexAuthPoolState auth_) : auth(std::move(auth_)) {}
  AuthKv::SnapshotRead AuthKv::snapshotRead(const rt::KvKey& key) {
    if (isAuthKey(key)) {
      ++reads;
      return {{key, rt::KvValue(json::value_from(auth)), stamp(authVersion)}, true, std::nullopt};
    }
    const double routingRead = ++routingReads;
    const auto* stored = extra.get(keyOf(key));
    return {{key, stored ? stored->value : rt::KvValue(nullptr), stored ? std::optional(stamp(stored->version)) : std::nullopt}, false, routingRead};
  }
  rt::Task<rt::KvEntryMaybe<>> AuthKv::get(rt::KvKey key, rt::KvGetOptions options) {
    require(options.consistency == rt::KvConsistencyLevel::strong, "AuthKv.get requires strong consistency");
    if (!isAuthKey(key)) {
      const double routingRead = ++routingReads;
      auto hook = onRoutingRead;
      rt::Promise<void> pending = hook ? rt::Promise<void>(hook(routingRead)) : rt::Promise<void>::resolve();
      return pending.then([this, key = std::move(key)] {
        const auto* entry = extra.get(keyOf(key));
        return rt::KvEntryMaybe<>{key, entry ? entry->value : rt::KvValue(nullptr), entry ? std::optional(stamp(entry->version)) : std::nullopt};
      }).wait();
    }
    auto snapshot = snapshotRead(key);
    const rt::Promise<void> gate = nextReadGate.value_or(rt::Promise<void>::resolve());
    nextReadGate.reset();
    return gate.then([entry = std::move(snapshot.entry)] { return entry; }).wait();
  }
  rt::Task<std::vector<rt::KvEntryMaybe<>>> AuthKv::getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options) {
    require(options.consistency == rt::KvConsistencyLevel::strong, "AuthKv.getMany requires strong consistency");
    std::vector<SnapshotRead> snapshot;
    bool authRead = false;
    for (const auto& key : keys) {
      snapshot.push_back(snapshotRead(key));
      authRead = authRead || snapshot.back().authRead;
    }
    std::optional<rt::Promise<void>> gate = authRead ? nextReadGate : std::nullopt;
    if (gate) nextReadGate.reset();
    auto hooks = [this, snapshot]() -> rt::Task<void> {
      for (const auto& entry : snapshot) {
        if (entry.routingRead) {
          if (onRoutingRead) {
            rt::Promise<void> pending(onRoutingRead(*entry.routingRead));
            co_await pending.wait();
          } else co_await rt::microtask();
        }
      }
    };
    auto pending = rt::Promise<void>::resolve().then(std::move(hooks));
    rt::Promise<void> waiting;
    if (gate) waiting = pending.then([gate] { return *gate; });
    else waiting = pending.then([] {});
    return waiting.then([snapshot = std::move(snapshot)] {
      std::vector<rt::KvEntryMaybe<>> entries;
      for (const auto& entry : snapshot) entries.push_back(entry.entry);
      return entries;
    }).wait();
  }
  rt::Task<rt::KvCommitResult> AuthKv::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) {
    applyWrite(FakeKvWrite{"set", std::move(key), std::move(value)});
    return rt::Promise<rt::KvCommitResult>::resolve(rt::KvCommitResult{true, "00000000000000000001"}).wait();
  }
  rt::Task<void> AuthKv::delete_(rt::KvKey) { throw rt::TypeError("AuthKv.delete is not a function"); }
  std::unique_ptr<rt::KvListIterator> AuthKv::list(rt::KvListSelector, rt::KvListOptions) { throw rt::TypeError("AuthKv.list is not a function"); }
  std::optional<std::string> AuthKv::checkVersion(const rt::KvKey& key) const {
    if (isAuthKey(key)) return stamp(authVersion);
    const auto* entry = extra.get(keyOf(key));
    return entry ? std::optional(stamp(entry->version)) : std::nullopt;
  }
  void AuthKv::applyWrite(const FakeKvWrite& write) {
    if (isAuthKey(write.key)) {
      if (write.type == "set") auth = json::value_to<::uos::types::CodexAuthPoolState>(write.value.toJson());
      ++authVersion;
      return;
    }
    const auto encoded = keyOf(write.key);
    if (write.type == "delete") extra.delete_(encoded);
    else {
      const auto* current = extra.get(encoded);
      const double version = (current ? current->version : 0) + 1;
      extra.set(encoded, ExtraEntry{write.value, version});
    }
  }
  rt::Task<rt::KvCommitResult> AuthKv::commit(std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> mutations) {
    std::vector<FakeKvWrite> writes;
    for (const auto& mutation : mutations) writes.push_back({std::string(rt::toString(mutation.type)), mutation.key, mutation.value});
    const bool providerHealthSuccess = std::any_of(writes.begin(), writes.end(), isProviderHealthSuccessWrite);
    if (providerHealthSuccess) {
      if (onProviderHealthSuccessCommit) onProviderHealthSuccessCommit();
      if (providerHealthSuccessCommitGate) co_await providerHealthSuccessCommitGate->wait();
      else co_await rt::microtask();
    }
    if (routingCommitFailures > 0 && std::any_of(writes.begin(), writes.end(), [](const auto& write) { return !isAuthKey(write.key); })) {
      --routingCommitFailures;
      co_return rt::KvCommitResult{false, ""};
    }
    for (const auto& check : checks)
      if (checkVersion(check.key) != check.versionstamp) co_return rt::KvCommitResult{false, ""};
    for (const auto& write : writes) applyWrite(write);
    if (providerHealthSuccess && onProviderHealthSuccessCommitted) onProviderHealthSuccessCommitted();
    auto result = rt::Promise<rt::KvCommitResult>::resolve(rt::KvCommitResult{true, "00000000000000000001"});
    auto committed = co_await result.wait();
    co_return committed;
  }
  std::unique_ptr<rt::AtomicOperation> AuthKv::atomic() {
    return std::make_unique<rt::GenericAtomicOperation>([this](std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> mutations) {
      return commit(std::move(checks), std::move(mutations));
    });
  }
  std::string encodeBase64Url(const json::value& value) { return rt::toBase64Url(rt::encodeUtf8(rt::jsonStringify(value))); }
  std::string accessToken(const std::string& label) {
    return encodeBase64Url(json::object{{"alg", "none"}}) + "." + encodeBase64Url(json::object{{"exp", (fixedStartMs + 60 * 60'000) / 1000}}) + "." + label;
  }
  ::uos::types::CodexAuthState auth(const std::string& label) {
    ::uos::types::CodexAuthState result;
    result.access_token = accessToken(label);
    result.refresh_token = "refresh-" + label;
    result.account_id = "account-" + label;
    result.updated_at_ms = fixedStartMs;
    return result;
  }
  ::uos::types::CodexAuthState staleAuth(const std::string& label) {
    auto result = auth(label);
    result.access_token = encodeBase64Url(json::object{{"alg", "none"}}) + "." + encodeBase64Url(json::object{{"exp", (fixedStartMs + 30'000) / 1000}}) + "." + label;
    return result;
  }
  ::uos::types::CodexAuthPoolState pool(std::vector<::uos::types::CodexAuthState> accounts) { return {std::move(accounts), fixedStartMs}; }
  std::shared_ptr<AuthKv> kv = std::make_shared<AuthKv>(pool(auth("old")));
  std::string requestUrl(const rt::RequestInfo& input) {
    return std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.toString();
      else return value.url();
    }, input);
  }
  rt::Reason abortReasonError(const rt::Reason& reason) {
    if (rt::isError(reason)) return reason;
    return rt::makeReason(rt::withCause(rt::JsError("transport aborted: " + rt::reasonToString(reason)), reason));
  }
  codex::banked_reset::CodexBankedResetConfig liveBankedResetConfig() { return {true, "live", 1, 1}; }
  codex::dispatch::FetchCodexResponsesOptions bankedResetRequestOptions(const std::string& requestId) {
    codex::dispatch::FetchCodexResponsesOptions result;
    result.clientVersion = "0.145.0";
    result.requestId = requestId;
    codex::dispatch::CodexBankedResetOptions reset;
    reset.config = liveBankedResetConfig();
    reset.kv = kv;
    reset.now = [] { return fixedStartMs; };
    reset.newOwnerToken = [requestId] { return "owner-" + requestId; };
    result.bankedReset = reset;
    return result;
  }
  std::string stableBankedResetRetryAfter() { return rt::Date(fixedStartMs + 60'000).toUTCString(); }
  rt::Task<void> seedStableBankedResetBlock(std::string accountId, double observedAtMs) {
    rt::Promise<codex::routing_state::RouteSelection> pending(codex::account_routing::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs));
    auto initial = co_await pending.wait();
    const auto* eligible = std::get_if<codex::routing_state::RouteSelectionEligible>(&initial);
    require(eligible != nullptr, "initial routing selection must be eligible");
    const auto account = std::find_if(eligible->accounts.begin(), eligible->accounts.end(), [&](const auto& candidate) { return candidate.auth.account_id == accountId; });
    require(account != eligible->accounts.end(), "blocked account must exist");
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", stableBankedResetRetryAfter()}};
    rt::Response response(rt::BodyInit(std::string("{\"error\":{\"type\":\"usage_limit_reached\"}}")), init);
    auto task = codex::account_routing::markCodexQuotaBlocked(*account, response, observedAtMs);
    co_await std::move(task);
    codex::account_routing::resetCodexAccountRoutingForTest();
  }
  ScriptedResetProvider::ScriptedResetProvider(ScriptedResetOptions options_)
    : codex::banked_reset_provider::CodexUsageResetProvider(json::object{
        {"idempotency", json::object{{"callerSupplied", true}, {"retentionMs", 86'400'000}}},
        {"lookup", json::object{{"byIdempotencyKey", true}, {"byProviderReceiptId", true}}},
        {"verification", json::object{{"independentlyVerifiable", true}}}, {"receiptIdsSafeToPersistAndLog", true},
        {"supportedResetTypes", json::array{"codex_rate_limits"}}}), options(std::move(options_)) {}
  rt::Task<codex::banked_reset_provider::ResetInventory> ScriptedResetProvider::readInventory(codex::banked_reset_provider::ResetAccountContext input,
                                                                                           rt::AbortSignal) {
    calls.push_back("inventory");
    inventoryAccountIds.push_back(input.accountId);
    if (options.onInventory) {
      rt::Promise<void> pending(options.onInventory());
      co_await pending.wait();
    } else co_await rt::microtask();
    codex::banked_reset_provider::ResetInventory inventory{1, fixedStartMs, {{"fixture-credit", "available", "codex_rate_limits", std::nullopt}}};
    co_return inventory;
  }
  rt::Task<codex::banked_reset_provider::RedeemResetResult> ScriptedResetProvider::redeem(codex::banked_reset_provider::RedeemResetInput input,
                                                                                       rt::AbortSignal) {
    calls.push_back("redeem");
    idempotencyKeys.push_back(input.idempotencyKey);
    redeemAccountIds.push_back(input.accountId);
    if (options.onRedeem) options.onRedeem();
    if (options.redeemGate) co_await options.redeemGate->wait();
    if (options.redeemKind == "unknown") co_return codex::banked_reset_provider::RedeemResetResultUnknown{std::nullopt};
    if (options.redeemKind == "already_redeemed") co_return codex::banked_reset_provider::RedeemResetResultAlreadyRedeemed{"receipt-sanitized"};
    co_return codex::banked_reset_provider::RedeemResetResultCompleted{"receipt-sanitized"};
  }
  rt::Task<codex::banked_reset_provider::RedeemResetResult> ScriptedResetProvider::lookup(codex::banked_reset_provider::LookupRedeemResetInput input,
                                                                                       rt::AbortSignal) {
    calls.push_back("lookup");
    idempotencyKeys.push_back(input.idempotencyKey);
    codex::banked_reset_provider::RedeemResetResult result = codex::banked_reset_provider::RedeemResetResultCompleted{"receipt-sanitized"};
    return rt::Promise<codex::banked_reset_provider::RedeemResetResult>::resolve(result).wait();
  }
  rt::Task<json::value> ScriptedResetProvider::verifyApplied(codex::banked_reset_provider::ResetAccountContext, rt::AbortSignal) {
    calls.push_back("verify");
    if (options.onVerify) options.onVerify();
    return rt::Promise<json::value>::resolve(json::value(options.verify.value_or(true))).wait();
  }
  std::shared_ptr<ScriptedResetProvider> scriptedResetProvider(ScriptedResetOptions options) { return std::make_shared<ScriptedResetProvider>(std::move(options)); }
  void installHarness() { ::uos::kv::setKvForTest(kv); }
  void resetHarness() {
    kv->extra.clear();
    kv->reads = 0;
    kv->routingReads = 0;
    kv->routingCommitFailures = 0;
    kv->nextReadGate.reset();
    kv->onRoutingRead = {};
    kv->providerHealthSuccessCommitGate.reset();
    kv->onProviderHealthSuccessCommit = {};
    kv->onProviderHealthSuccessCommitted = {};
    codex::auth::resetCodexAuthCacheForTest();
    codex::account_routing::resetCodexAccountRoutingForTest();
    installHarness();
  }
  void cleanupHarness() { ::uos::kv::setKvForTest(nullptr); }
  HarnessScope::HarnessScope() : environment({}), savedDeploy(config::config.isDeploy), savedSeed(config::config.codexAuthJsonB64),
                                  savedBase(config::config.codexBaseUrl) {
    config::config.codexAuthJsonB64.clear();
    config::config.codexBaseUrl = "https://chatgpt.com/backend-api/codex";
    config::config.isDeploy = true;
    rt::globalDateNow() = [] { return fixedStartMs; };
    provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const provider::capacity_events::ProviderCapacitySampleTriggerOptions&) {});
    resetHarness();
  }
  HarnessScope::~HarnessScope() {
    cleanupHarness();
    codex::auth::resetCodexAuthCacheForTest();
    codex::account_routing::resetCodexAccountRoutingForTest();
    provider::capacity_events::setProviderCapacitySampleTriggerForTest({});
    config::config.isDeploy = savedDeploy;
    config::config.codexAuthJsonB64 = savedSeed;
    config::config.codexBaseUrl = savedBase;
  }
}
