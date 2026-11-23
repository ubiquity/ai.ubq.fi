// Port of all 17 cases in tests/local-development-key.test.ts.
// These source native-KV cases use isolated rt::MemoryKv CAS; no native-storage acceptance is claimed.
// Earlier grouped provisioning differentials remain in paid_fallback_index_local_development_key_differential_test.
// Links: tests/local_development_key_test.cpp
// src/authority/native_api_key_audit.cpp src/admin/api_key_audit.cpp src/kv.cpp tests/helpers/admin_auth_harness.cpp
// src/utils.cpp src/admin/api_keys.cpp src/types.cpp src/api_keys.cpp
// src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/api_key_policy.cpp src/storage/kv.cpp
// src/paid_fallback/ledger_state.cpp src/request.cpp src/paid_fallback/index.cpp src/models/codex_models_fetch.cpp
// src/codex/dispatch.cpp src/authority/native_codex_contracts.cpp src/authority/native_paid_policy.cpp src/runtime_config.cpp
// src/defaults.cpp src/http.cpp src/authority/native_paid.cpp src/config.cpp
// src/paid_fallback/ledger_admission.cpp src/provider/surplus.cpp src/storage/sqlite_readonly.cpp src/models/codex_models.cpp
// src/provider/health.cpp src/authority/native_paid_contracts.cpp src/provider/capacity_events.cpp src/paid_fallback/ledger_backfill.cpp
// src/auth/relay.cpp src/paid_fallback/ledger_settlement.cpp src/provider/metered.cpp src/provider/capacity.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/claude_capacity_history.cpp src/provider/claude_code_accounts.cpp src/provider/capacity_rollups.cpp
// src/codex/auth.cpp src/codex/routing_mutations.cpp src/provider/claude_code_quota.cpp src/provider/capacity_history.cpp
// src/authority/native_codex_capacity.cpp src/provider/anthropic_responses.cpp src/authority/native_codex_mutations.cpp src/provider/claude_code_credentials.cpp
// src/paid_fallback/ledger_stats.cpp src/codex/capacity_routing.cpp src/responses_stream.cpp src/codex/experiment.cpp
// src/models/codex_models_availability.cpp src/provider/surplus_claude_request.cpp src/codex/auth_refresh.cpp src/codex/routing_state.cpp
// src/provider/capacity_contract.cpp src/storage/native_kv.cpp src/provider/anthropic_chat_responses.cpp src/codex/external_auth.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/capacity_parse.cpp src/metered_quota.cpp src/provider/anthropic_replay.cpp
// src/provider/anthropic_response_wire.cpp src/release.cpp src/codex/rate_limit_429.cpp
// src/authority/native_codex_429.cpp src/provider/anthropic_chat_stream.cpp src/bounded_response_body.cpp src/authority/native_codex_evaluation.cpp
// src/codex/banked_reset.cpp src/provider/cerebras_rate_limits.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_core.cpp
// src/codex/account_routing.cpp src/provider/surplus_claude_chat_request.cpp src/provider/capacity_analytics.cpp src/codex/routing_evaluation.cpp
// src/codex/native_auth.cpp src/codex/routing_serial.cpp src/authority/native_codex.cpp src/provider/selection.cpp
// src/codex/banked_reset_provider.cpp src/codex/reset_settings.cpp src/codex/overage_settings.cpp src/codex/native_auth_transport.cpp
// src/runtime/websocket.cpp
// src/admin/api_key_mutations.cpp src/analytics.cpp src/auth/local_development_key.cpp
// src/paid_fallback/rollups.cpp
// src/auth/index.cpp src/auth/passport.cpp src/auth/passport_sessions.cpp src/kernel/attestation.cpp
// src/auth/passkeys.cpp src/kernel/policy_queue.cpp src/auth/local_admin.cpp src/kernel/quota_reservations.cpp
// src/kernel/quota_v2.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp
#include "uos/admin/api_key_mutations.hpp"
#include "uos/analytics.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/auth/local_admin.hpp"
#include "uos/auth/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/test_async.hpp"
#include "helpers/live_allocations.hpp"

#include <memory>
#include <string>
#include <vector>
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace local = uos::auth::local_development_key;
  namespace mutation = uos::admin::api_key_mutations; namespace keys = uos::api_keys; namespace quota = uos::api_key_policy;
  namespace paid = uos::paid_fallback::index; namespace ledger = uos::paid_fallback::ledger_state; namespace admission = uos::paid_fallback::ledger_admission;
  const std::string ID(local::LOCAL_DEVELOPMENT_KEY_ID);
  const rt::KvKey& idKey() { static const auto key = keys::apiKeyIdKey(ID); return key; }
  const rt::KvKey& guardKey() { static const auto key = ledger::paidFallbackDeletionGuardV3Key(ID); return key; }
  paid::InitializePaidFallbackPolicyResult pricing() { return {{"gpt-6-astra"}, 1000, json::object(), rt::dateNow()}; }
  local::EnsureLocalDevelopmentApiKeyOptions options() {
    local::EnsureLocalDevelopmentApiKeyOptions value; value.initializePolicy = [](std::optional<rt::AbortSignal>) { return rt::fulfilledTask(pricing()); }; return value;
  }
  rt::Task<std::string> ensure(std::shared_ptr<rt::Kv> kv) { const auto input = options(); auto task = local::ensureLocalDevelopmentApiKey(kv, input); const auto status = co_await std::move(task); co_return status; }
  rt::Request request(const std::string& id = ID) { return rt::Request("http://127.0.0.1/admin/api-keys", rt::RequestInit{.method = "POST", .body = rt::BodyInit(rt::jsonStringify(json::object{{"id", id}}))}); }
  struct Pause {
    rt::Resolvers<void> entered = rt::withResolvers<void>();
    rt::Resolvers<void> resumed = rt::withResolvers<void>();
    bool used = false;
  };
  class PausedIterator final : public rt::KvListIterator {
    std::unique_ptr<rt::KvListIterator> inner_; std::shared_ptr<Pause> pause_;
  public:
    PausedIterator(std::unique_ptr<rt::KvListIterator> inner, std::shared_ptr<Pause> pause) : inner_(std::move(inner)), pause_(std::move(pause)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (!pause_->used) { pause_->used = true; pause_->entered.resolve(); co_await pause_->resumed.promise.wait(); }
      auto task = inner_->next(); const auto result = co_await std::move(task); co_return result;
    }
    std::string cursor() const override { return inner_->cursor(); }
    rt::Task<void> return_() override { return inner_->return_(); }
  };
  class HoldingKv final : public rt::ForwardingKv {
    std::shared_ptr<rt::MemoryKv> inner_;
  public:
    std::optional<rt::KvKey> pausedPrefix; std::shared_ptr<Pause> pause;
    explicit HoldingKv(std::shared_ptr<rt::MemoryKv> store) : rt::ForwardingKv(*store), inner_(std::move(store)) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions listOptions = {}) override {
      auto result = inner_->list(selector, listOptions);
      if (pausedPrefix && selector.prefix && rt::kvKeyStrictEquals(*selector.prefix, *pausedPrefix)) {
        pausedPrefix.reset(); return std::make_unique<PausedIterator>(std::move(result), pause);
      }
      return result;
    }
    std::shared_ptr<Pause> hold(rt::KvKey prefix) { pausedPrefix = std::move(prefix); pause = std::make_shared<Pause>(); return pause; }
  };
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::initializer_list<std::pair<std::string, std::string>>()};
    std::shared_ptr<rt::MemoryKv> memory = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    std::shared_ptr<HoldingKv> kv = std::make_shared<HoldingKv>(memory);
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); rt::env::set("SURPLUS_API_KEY", "synthetic-surplus-key"); uos::kv::setKvForTest(kv); quota::resetApiKeyPolicyCacheForTest(); paid::setPaidFallbackTerminalSweepForTest({}); }
    ~Fixture() { uos::kv::setKvForTest(nullptr); paid::setPaidFallbackTerminalSweepForTest(nullptr); }
  };
  rt::KvValue completedGuard() { return rt::KvValue(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow())}, {"local_deletion", json::object{{"owner", "synthetic-completed"}, {"completed_at_ms", rt::makeNumber(rt::dateNow())}}}}); }
  rt::KvValue ownedGuard(const std::string& owner) { return rt::KvValue(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow())}, {"local_deletion", json::object{{"owner", owner}, {"completed_at_ms", nullptr}}}}); }
}
ASYNC_TEST("native KV re-provisions a completely deleted local key and admits paid work") {
  Fixture fixture; const auto kv = fixture.kv; const auto unrelatedKey = ledger::paidFallbackDeletionGuardV3Key("unrelated-key");
  const rt::KvValue unrelated(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow())}}); AWAIT(storedUnrelated, kv->set(unrelatedKey, unrelated)); EXPECT(storedUnrelated.ok); AWAIT(unrelatedBefore, kv->get(unrelatedKey));
  AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(original, local::resolveLocalDevelopmentApiKeyPolicy(kv)); ASSERT(original);
  AWAIT(revoked, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revoked.status(), 200); AWAIT(held, ensure(kv)); EXPECT_EQ(held, "revoked");
  AWAIT(deleted, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(deleted.status(), 200); AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull()); AWAIT(hashAbsent, kv->get(keys::apiKeyHashKey(original->token_hash))); EXPECT(hashAbsent.value.isNull()); AWAIT(windowAbsent, kv->get(quota::apiKeyUsageV3WindowKey(*original))); EXPECT(windowAbsent.value.isNull());
  AWAIT(completed, kv->get(guardKey())); EXPECT(completed.value["local_deletion"]["owner"].isString()); EXPECT(completed.value["local_deletion"]["completed_at_ms"].isNumber()); AWAIT(outstanding, ledger::getPaidFallbackOutstandingV3(ID, kv)); ASSERT(outstanding); EXPECT(!outstanding->has_outstanding);
  AWAIT(recreated, ensure(kv)); EXPECT_EQ(recreated, "created"); AWAIT(guardAbsent, kv->get(guardKey())); EXPECT(guardAbsent.value.isNull()); AWAIT(unrelatedAfter, kv->get(unrelatedKey)); EXPECT_EQ(unrelatedAfter.value, unrelatedBefore.value); EXPECT_EQ(unrelatedAfter.versionstamp, unrelatedBefore.versionstamp);
  AWAIT(replacement, local::resolveLocalDevelopmentApiKeyPolicy(kv)); ASSERT(replacement); EXPECT(replacement->token_hash != original->token_hash); AWAIT(policyCheck, kv->get(keys::apiKeyHashKey(replacement->token_hash)));
  ledger::AdmissionInput input; input.keyId = replacement->key_id; input.requestId = "native-reprovision"; input.createdAtMs = rt::dateNow(); input.policyVersion = replacement->policy_version;
  input.limitMicrocredits = replacement->paid_fallback_limit_microcredits; input.maximumExposureMicrocredits = std::nullopt; input.initialSettledMicrocredits = 0; input.quotaPerCredit = 1000; input.windowResetAtMs = replacement->usage_reset_at_ms;
  input.model = "gpt-6-astra"; input.route = "responses"; input.path = "/v1/responses"; input.stream = false; input.reasoning = std::nullopt; input.policyCheck = rt::AtomicCheck(policyCheck);
  AWAIT(admitted, admission::admitPaidFallbackV3(input)); ASSERT(std::holds_alternative<admission::AdmitPaidFallbackV3ResultReserved>(admitted)); const auto reservation = std::get<admission::AdmitPaidFallbackV3ResultReserved>(admitted).reservation; auto releasing = admission::releaseUndispatchedPaidFallbackV3(reservation); co_await std::move(releasing);
  if (const auto scheduled = uos::paid_fallback::ledger_backfill::scheduledPaidFallbackBackfillForTest()) co_await scheduled->wait();
  AWAIT(finalOutstanding, ledger::getPaidFallbackOutstandingV3(ID, kv)); ASSERT(finalOutstanding); EXPECT(!finalOutstanding->has_outstanding);
}
ASYNC_TEST("native KV retains local deletion guards while billing or cleanup remains") {
  const auto now = rt::dateNow();
  const std::vector<std::pair<rt::KvKey, rt::KvValue>> fixtures{
    {ledger::paidFallbackRequestV3Key(ID, "pending"), rt::KvValue(json::object{{"billing_state", "pending"}})},
    {ledger::paidFallbackRequestV3Key(ID, "unresolved"), rt::KvValue(json::object{{"billing_state", "unresolved"}})},
    {ledger::paidFallbackPendingV3Key(ID, "pending-marker"), rt::KvValue(json::object{{"created_at_ms", rt::makeNumber(now)}})},
    {ledger::paidFallbackRequestV3Key(ID, "settled"), rt::KvValue(json::object{{"billing_state", "settled"}})},
    {ledger::paidFallbackWindowV3Key(ID, now), rt::KvValue(json::object{{"pending_count", 0}})},
    {ledger::paidFallbackReconciliationLeaseV3Key(ID), rt::KvValue(json::object{{"token", "synthetic"}, {"expires_at_ms", rt::makeNumber(now + 60000)}})},
    {rt::KvKey{quota::API_KEY_USAGE_V3_WINDOW_PREFIX, ID, "old-policy", 1}, rt::KvValue(json::object{{"reserved_requests", 0}})},
    {rt::KvKey{quota::API_KEY_USAGE_V3_REQUEST_PREFIX, ID, "old-policy", 1, "old-request"}, rt::KvValue(json::object{{"state", "committed"}})},
    {rt::KvKey{uos::analytics::apiKeyRequestLogPrefix(ID), 1, "old-request"}, rt::KvValue(json::object{{"request_id", "old-request"}})}};
  for (const auto& [key, value] : fixtures) {
    Fixture fixture; const auto kv = fixture.kv; const auto guard = completedGuard(); AWAIT(setGuard, kv->set(guardKey(), guard)); EXPECT(setGuard.ok); AWAIT(before, kv->get(guardKey())); AWAIT(setValue, kv->set(key, value)); EXPECT(setValue.ok); AWAIT(retained, kv->get(key));
    AWAIT(status, ensure(kv)); EXPECT_EQ(status, "conflict"); AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull()); AWAIT(after, kv->get(guardKey())); EXPECT_EQ(after.value, before.value); EXPECT_EQ(after.versionstamp, before.versionstamp); AWAIT(afterRow, kv->get(key)); EXPECT_EQ(afterRow.value, retained.value); EXPECT_EQ(afterRow.versionstamp, retained.versionstamp);
  }
}
ASYNC_TEST("native KV deletion guard CAS rejects a concurrent deletion during local provisioning") {
  const std::vector<bool> retainedCases{false, true};
  for (const auto retained : retainedCases) {
    Fixture fixture; const auto kv = fixture.kv;
    if (retained) { const auto guard = completedGuard(); AWAIT(stored, kv->set(guardKey(), guard)); EXPECT(stored.ok); }
    auto observed = std::make_shared<std::optional<rt::KvEntryMaybe<>>>();
    local::EnsureLocalDevelopmentApiKeyOptions input;
    input.initializePolicy = [kv, retained, observed](std::optional<rt::AbortSignal>) {
      return rt::async([kv, retained, observed]() -> rt::Task<paid::InitializePaidFallbackPolicyResult> {
        if (retained) { const rt::KvValue replacement(json::object{{"created_at_ms", 2}}); AWAIT(stored, kv->set(guardKey(), replacement)); EXPECT(stored.ok); }
        else { AWAIT(deleted, admission::deletePaidFallbackStateV3(ID, kv)); EXPECT(std::holds_alternative<admission::PaidFallbackDeletionV3Deleted>(deleted)); }
        AWAIT(current, kv->get(guardKey())); *observed = current; co_return pricing();
      });
    };
    AWAIT(result, local::ensureLocalDevelopmentApiKey(kv, input)); EXPECT_EQ(result, "conflict"); ASSERT(*observed); AWAIT(after, kv->get(guardKey())); EXPECT_EQ(after.value, (*observed)->value); EXPECT_EQ(after.versionstamp, (*observed)->versionstamp); AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull()); const rt::KvListSelector selector{.prefix = quota::API_KEY_USAGE_V3_WINDOW_PREFIX}; auto iterator = kv->list(selector); AWAIT(window, iterator->next()); EXPECT(!window);
  }
}
ASYNC_TEST("native KV keeps provisioning blocked while old deletion scans an empty usage prefix") {
  const std::vector<bool> staleCases{false, true};
  for (const auto staleOwner : staleCases) {
    Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(policy, local::resolveLocalDevelopmentApiKeyPolicy(kv)); ASSERT(policy); auto removal = kv->delete_(quota::apiKeyUsageV3WindowKey(*policy)); co_await std::move(removal); AWAIT(revoked, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revoked.status(), 200);
    const auto pause = kv->hold(rt::KvKey{quota::API_KEY_USAGE_V3_WINDOW_PREFIX, ID}); rt::Promise<rt::Response> deletion(mutation::handleAdminApiKeysDelete(request())); co_await pause->entered.promise.wait();
    AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull()); AWAIT(blocked, ensure(kv)); EXPECT_EQ(blocked, "conflict"); std::optional<rt::KvEntryMaybe<>> replacementGuard;
    if (staleOwner) { const auto guard = ownedGuard("replacement-owner"); AWAIT(stored, kv->set(guardKey(), guard)); EXPECT(stored.ok); AWAIT(current, kv->get(guardKey())); replacementGuard = current; }
    pause->resumed.resolve(); const auto output = co_await deletion.wait(); EXPECT_EQ(output.status(), staleOwner ? 409 : 200);
    if (staleOwner) { AWAIT(current, kv->get(guardKey())); EXPECT_EQ(current.value, replacementGuard->value); EXPECT_EQ(current.versionstamp, replacementGuard->versionstamp); AWAIT(stillBlocked, ensure(kv)); EXPECT_EQ(stillBlocked, "conflict"); }
    else { AWAIT(recreated, ensure(kv)); EXPECT_EQ(recreated, "created"); AWAIT(replacement, local::resolveLocalDevelopmentApiKeyPolicy(kv)); ASSERT(replacement); AWAIT(window, kv->get(quota::apiKeyUsageV3WindowKey(*replacement))); EXPECT(window.value.truthy()); }
  }
}
ASYNC_TEST("native KV local deletion refuses overlapping and crashed owners without changing other guards") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(record, kv->get(idKey())); ASSERT(record.value.truthy()); AWAIT(revoked, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revoked.status(), 200);
  const auto pause = kv->hold(ledger::paidFallbackRequestV3Prefix(ID)); rt::Promise<rt::Response> deletion(mutation::handleAdminApiKeysDelete(request())); co_await pause->entered.promise.wait(); AWAIT(claimed, kv->get(guardKey())); AWAIT(overlap, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(overlap.status(), 409); AWAIT(unrevoke, mutation::handleAdminApiKeysUnrevoke(request())); EXPECT_EQ(unrevoke.status(), 409); AWAIT(same, kv->get(guardKey())); EXPECT_EQ(same.value, claimed.value); EXPECT_EQ(same.versionstamp, claimed.versionstamp); AWAIT(held, ensure(kv)); EXPECT_EQ(held, "revoked"); pause->resumed.resolve(); const auto first = co_await deletion.wait(); EXPECT_EQ(first.status(), 200);
  const std::string otherId = "unrelated-key"; const auto otherGuardKey = ledger::paidFallbackDeletionGuardV3Key(otherId); auto other = record.value.as<rt::KvObject>(); other.set("id", otherId); other.set("hash", "synthetic-other-hash"); other.set("revoked_at_ms", rt::dateNow()); const rt::KvValue otherValue(other); const rt::KvValue otherGuard(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow())}}); AWAIT(setOther, kv->set(keys::apiKeyIdKey(otherId), otherValue)); EXPECT(setOther.ok); AWAIT(setOtherGuard, kv->set(otherGuardKey, otherGuard)); EXPECT(setOtherGuard.ok); AWAIT(otherBefore, kv->get(otherGuardKey)); AWAIT(otherDeleted, mutation::handleAdminApiKeysDelete(request(otherId))); EXPECT_EQ(otherDeleted.status(), 200); AWAIT(otherAfter, kv->get(otherGuardKey)); EXPECT_EQ(otherAfter.value, otherBefore.value); EXPECT_EQ(otherAfter.versionstamp, otherBefore.versionstamp);
  AWAIT(recreated, ensure(kv)); EXPECT_EQ(recreated, "created"); AWAIT(revokedAgain, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revokedAgain.status(), 200); const auto crashedGuard = ownedGuard("crashed-owner"); AWAIT(stored, kv->set(guardKey(), crashedGuard)); EXPECT(stored.ok); AWAIT(crashed, kv->get(guardKey())); AWAIT(refused, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(refused.status(), 409); AWAIT(stillCrashed, kv->get(guardKey())); EXPECT_EQ(stillCrashed.value, crashed.value); EXPECT_EQ(stillCrashed.versionstamp, crashed.versionstamp); auto deletingId = kv->delete_(idKey()); co_await std::move(deletingId); AWAIT(provisioningBlocked, ensure(kv)); EXPECT_EQ(provisioningBlocked, "conflict");
  const rt::KvValue legacyGuard(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow())}}); AWAIT(storedLegacy, kv->set(guardKey(), legacyGuard)); EXPECT(storedLegacy.ok); AWAIT(legacyBefore, kv->get(guardKey())); AWAIT(legacyBlocked, ensure(kv)); EXPECT_EQ(legacyBlocked, "conflict"); AWAIT(legacyAfter, kv->get(guardKey())); EXPECT_EQ(legacyAfter.value, legacyBefore.value); EXPECT_EQ(legacyAfter.versionstamp, legacyBefore.versionstamp);
}
ASYNC_TEST("native KV known outstanding billing refusal releases only its local owner for explicit retry") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(revoked, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revoked.status(), 200); const auto pendingKey = ledger::paidFallbackRequestV3Key(ID, "pending-billing"); const rt::KvValue pending(json::object{{"billing_state", "pending"}}); AWAIT(stored, kv->set(pendingKey, pending)); EXPECT(stored.ok);
  AWAIT(blocked, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(blocked.status(), 409); AWAIT(released, kv->get(guardKey())); EXPECT_EQ(released.value["local_deletion"], rt::KvValue(json::object{{"owner", nullptr}, {"completed_at_ms", nullptr}})); AWAIT(held, ensure(kv)); EXPECT_EQ(held, "revoked"); auto removal = kv->delete_(pendingKey); co_await std::move(removal); AWAIT(deleted, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(deleted.status(), 200); AWAIT(recreated, ensure(kv)); EXPECT_EQ(recreated, "created");
}
ASYNC_TEST("native KV releases a local owner after a concurrent revoked-record update") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(revoked, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revoked.status(), 200);
  const auto pause = kv->hold(ledger::paidFallbackRequestV3Prefix(ID)); rt::Promise<rt::Response> deletion(mutation::handleAdminApiKeysDelete(request())); co_await pause->entered.promise.wait(); AWAIT(claimed, kv->get(guardKey())); EXPECT(claimed.value["local_deletion"]["owner"].isString()); AWAIT(overlap, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(overlap.status(), 409); AWAIT(unrevoke, mutation::handleAdminApiKeysUnrevoke(request())); EXPECT_EQ(unrevoke.status(), 409); AWAIT(stillClaimed, kv->get(guardKey())); EXPECT_EQ(stillClaimed.value, claimed.value); EXPECT_EQ(stillClaimed.versionstamp, claimed.versionstamp);
  AWAIT(revokedAgain, mutation::handleAdminApiKeysRevoke(request())); EXPECT_EQ(revokedAgain.status(), 200); pause->resumed.resolve(); const auto failed = co_await deletion.wait(); EXPECT_EQ(failed.status(), 409); AWAIT(released, kv->get(guardKey())); EXPECT_EQ(released.value["local_deletion"], rt::KvValue(json::object{{"owner", nullptr}, {"completed_at_ms", nullptr}})); AWAIT(deleted, mutation::handleAdminApiKeysDelete(request())); EXPECT_EQ(deleted.status(), 200); AWAIT(recreated, ensure(kv)); EXPECT_EQ(recreated, "created");
}

ASYNC_TEST("loopback development provisions one unlimited, non-expiring local API key") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(status, ensure(kv)); EXPECT_EQ(status, "created"); AWAIT(row, kv->get(idKey())); EXPECT(row.value.truthy()); EXPECT(paid::hasStrictPaidFallbackKeyPolicy(rt::storedJsonView(row.value))); EXPECT_EQ(row.value["paid_fallback_enabled"], rt::KvValue(true)); EXPECT_EQ(row.value["paid_fallback_limit_microcredits"], rt::KvValue(keys::PAID_FALLBACK_NO_LIMIT)); EXPECT_EQ(row.value["usage_limit_requests"], rt::KvValue(keys::API_KEY_NO_USAGE_LIMIT)); EXPECT_EQ(row.value["expires_at_ms"], rt::KvValue(keys::API_KEY_NO_EXPIRATION_MS)); EXPECT(row.value["revoked_at_ms"].isNull());
  AWAIT(hash, kv->get(keys::apiKeyHashKey(row.value["hash"].asString()))); EXPECT(hash.value.truthy()); AWAIT(policy, local::resolveLocalDevelopmentApiKeyPolicy(kv)); ASSERT(policy); EXPECT_EQ(policy->key_id, ID); EXPECT(policy->paid_fallback_enabled); EXPECT_EQ(policy->paid_fallback_limit_microcredits, keys::PAID_FALLBACK_NO_LIMIT); EXPECT_EQ(policy->usage_limit_requests, keys::API_KEY_NO_USAGE_LIMIT); AWAIT(window, kv->get(quota::apiKeyUsageV3WindowKey(*policy))); EXPECT(window.value.truthy());
}
ASYNC_TEST("local development key provisioning is idempotent and never rewrites a record") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(before, kv->get(idKey())); AWAIT(status, ensure(kv)); EXPECT_EQ(status, "present"); AWAIT(after, kv->get(idKey())); EXPECT_EQ(after.versionstamp, before.versionstamp); EXPECT_EQ(after.value, before.value);
}
ASYNC_TEST("local development key provisioning reports an unavailable pricing snapshot without writing") {
  Fixture fixture; const auto kv = fixture.kv; local::EnsureLocalDevelopmentApiKeyOptions input;
  input.initializePolicy = [](std::optional<rt::AbortSignal>) -> rt::Task<paid::InitializePaidFallbackPolicyResult> { throw rt::JsError("pricing unavailable"); co_return pricing(); };
  AWAIT(status, local::ensureLocalDevelopmentApiKey(kv, input)); EXPECT_EQ(status, "unavailable"); AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull());
}
ASYNC_TEST("local development key provisioning bounds a pricing initializer that never settles") {
  Fixture fixture; const auto kv = fixture.kv; const auto pending = rt::withResolvers<paid::InitializePaidFallbackPolicyResult>(); auto entered = std::make_shared<int>(0); auto signals = std::make_shared<std::vector<rt::AbortSignal>>();
  local::setLocalDevelopmentPricingDeadlineMsForTest(1000); const auto keepAlive = rt::setTimeout([] {}, 10000); auto cleanup = rt::scopeExit([keepAlive] { rt::clearTimeout(keepAlive); local::setLocalDevelopmentPricingDeadlineMsForTest(std::nullopt); });
  local::EnsureLocalDevelopmentApiKeyOptions input; input.initializePolicy = [pending, entered, signals](std::optional<rt::AbortSignal> signal) { ++*entered; if (signal) signals->push_back(*signal); return pending.promise.wait(); };
  const auto started = rt::performanceNow(); AWAIT(status, local::ensureLocalDevelopmentApiKey(kv, input)); const auto elapsed = rt::performanceNow() - started; EXPECT_EQ(*entered, 1); EXPECT_EQ(status, "unavailable"); EXPECT(elapsed >= 750); EXPECT(elapsed < 10000); ASSERT_EQ(signals->size(), std::size_t(1)); EXPECT(signals->front().aborted()); EXPECT_EQ(rt::errorName(signals->front().reason()), "TimeoutError"); AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull());
  pending.resolve(pricing()); co_await rt::sleep(10); AWAIT(stillAbsent, kv->get(idKey())); EXPECT(stillAbsent.value.isNull());
}
ASYNC_TEST("local development key provisioning keeps a normal pricing initialization inside the deadline") {
  Fixture fixture; const auto kv = fixture.kv; local::setLocalDevelopmentPricingDeadlineMsForTest(1000); auto cleanup = rt::scopeExit([] { local::setLocalDevelopmentPricingDeadlineMsForTest(std::nullopt); }); AWAIT(status, ensure(kv)); EXPECT_EQ(status, "created"); AWAIT(record, kv->get(idKey())); EXPECT_EQ(record.value["paid_fallback_model_ids"], rt::KvValue(json::array{"gpt-6-astra"})); EXPECT_EQ(record.value["paid_fallback_pricing_checked_at_ms"], rt::KvValue(rt::dateNow()));
}
ASYNC_TEST("completed local provisioning releases the private abort promise") {
  Fixture fixture;
  const auto provision = []() -> rt::Task<void> {
    const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    const auto status = co_await ensure(store);
    EXPECT_EQ(status, "created");
  };
  auto checking = uos::tests::helpers::expectStableLiveAllocations(provision, 64);
  co_await std::move(checking);
}
ASYNC_TEST("local development key provisioning discards a cancelled pricing snapshot") {
  Fixture fixture; const auto kv = fixture.kv; const auto pending = rt::withResolvers<paid::InitializePaidFallbackPolicyResult>(); auto entered = std::make_shared<int>(0); rt::AbortController controller;
  local::EnsureLocalDevelopmentApiKeyOptions input; input.signal = controller.signal(); input.initializePolicy = [pending, entered](std::optional<rt::AbortSignal>) { ++*entered; return pending.promise.wait(); };
  rt::Promise<std::string> attempt(local::ensureLocalDevelopmentApiKey(kv, input)); controller.abort(rt::makeReason(rt::DOMException("cancelled", "AbortError"))); const auto status = co_await attempt.wait(); EXPECT_EQ(status, "unavailable"); EXPECT_EQ(*entered, 1); pending.resolve(pricing()); co_await rt::sleep(10); AWAIT(absent, kv->get(idKey())); EXPECT(absent.value.isNull());
}
namespace {
  namespace auth = uos::auth::index; namespace localAdmin = uos::auth::local_admin;
  uos::runtime::http::Addr loopback() { return uos::runtime::http::NetAddr{"tcp", "127.0.0.1", 8000}; }
  struct AuthScope {
    bool previousDeploy = uos::config::config.isDeploy; bool previousProduction = uos::config::config.isProductionTimeline;
    AuthScope() { uos::config::config.isDeploy = false; uos::config::config.isProductionTimeline = false; localAdmin::configureAdminAuthForListener(localAdmin::ServeRuntimeOptions{true}, loopback()); localAdmin::configureAdminAuthPeerForRequest(loopback()); }
    ~AuthScope() { localAdmin::configureAdminAuthForListener(localAdmin::ServeRuntimeOptions{false}, loopback()); localAdmin::configureAdminAuthPeerForRequest(std::nullopt); uos::config::config.isDeploy = previousDeploy; uos::config::config.isProductionTimeline = previousProduction; }
  };
  rt::Request authRequest(const std::string& url, const rt::Headers& headers = {}) { return rt::Request(url, rt::RequestInit{.method = "POST", .headers = headers}); }
}
ASYNC_TEST("the hostname-only legacy fallback never attaches the paid local development key") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AWAIT(policy, local::resolveLocalDevelopmentApiKeyPolicy(kv)); ASSERT(policy); AuthScope scope;
  const auto foreign = authRequest("http://127.0.0.1/v1/chat/completions", rt::Headers{{"origin", "https://attacker.example"}}); AWAIT(crossOrigin, auth::authenticateClient(foreign)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultOk>(crossOrigin)); EXPECT(std::holds_alternative<auth::ClientAuthMethodDisabled>(std::get<auth::AuthenticateClientResultOk>(crossOrigin).method));
  AWAIT(whoami, auth::handleV1Auth(foreign)); EXPECT_EQ(whoami.status(), 200); AWAIT(body, whoami.json()); const auto* authBody = rt::get(body, "auth"); ASSERT(authBody); EXPECT_EQ(rt::getString(*authBody, "mode"), std::optional<std::string_view>("disabled")); EXPECT_EQ(rt::getString(*rt::get(*authBody, "method"), "kind"), std::optional<std::string_view>("disabled")); EXPECT_EQ(rt::getBool(*authBody, "is_admin"), std::optional(false)); EXPECT_EQ(rt::getBool(*authBody, "is_super_admin"), std::optional(false)); EXPECT(!rt::hasOwn(rt::get(*authBody, "method")->as_object(), "key"));
  const auto sameOriginRequest = authRequest("http://127.0.0.1/v1/chat/completions", rt::Headers{{"origin", "http://127.0.0.1"}, {"sec-fetch-site", "same-origin"}}); AWAIT(sameOrigin, auth::authenticateClient(sameOriginRequest)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultOk>(sameOrigin)); const auto& method = std::get<auth::AuthenticateClientResultOk>(sameOrigin).method; ASSERT(std::holds_alternative<auth::ClientAuthMethodKvApiKey>(method)); EXPECT_EQ(std::get<auth::ClientAuthMethodKvApiKey>(method).key_id, ID); EXPECT(std::get<auth::ClientAuthMethodKvApiKey>(method).policy.paid_fallback_enabled);
}
ASYNC_TEST("the hostname-only legacy fallback stays policy-free without a local development key") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(policy, local::resolveLocalDevelopmentApiKeyPolicy(kv)); EXPECT(!policy); AuthScope scope; const auto foreign = authRequest("http://127.0.0.1/v1/chat/completions", rt::Headers{{"origin", "https://attacker.example"}}); AWAIT(result, auth::authenticateClient(foreign)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultOk>(result)); EXPECT(std::holds_alternative<auth::ClientAuthMethodDisabled>(std::get<auth::AuthenticateClientResultOk>(result).method));
}
ASYNC_TEST("the loopback bypass authenticates as the unlimited local development key") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AuthScope scope; const auto localRequest = authRequest("http://127.42.9.3/v1/chat/completions"); AWAIT(result, auth::authenticateClient(localRequest)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultOk>(result)); const auto& method = std::get<auth::AuthenticateClientResultOk>(result).method; ASSERT(std::holds_alternative<auth::ClientAuthMethodKvApiKey>(method)); const auto& policy = std::get<auth::ClientAuthMethodKvApiKey>(method).policy; EXPECT_EQ(policy.key_id, ID); EXPECT(policy.paid_fallback_enabled); EXPECT_EQ(policy.paid_fallback_limit_microcredits, keys::PAID_FALLBACK_NO_LIMIT); EXPECT_EQ(policy.usage_limit_requests, keys::API_KEY_NO_USAGE_LIMIT);
  AWAIT(row, kv->get(idKey())); auto revoked = row.value.as<rt::KvObject>(); revoked.set("revoked_at_ms", rt::dateNow()); const rt::KvValue revokedValue(revoked); AWAIT(stored, kv->set(idKey(), revokedValue)); EXPECT(stored.ok); AWAIT(absent, local::resolveLocalDevelopmentApiKeyPolicy(kv)); EXPECT(!absent); AWAIT(revokedAuth, auth::authenticateClient(localRequest)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultOk>(revokedAuth)); EXPECT(std::holds_alternative<auth::ClientAuthMethodDisabled>(std::get<auth::AuthenticateClientResultOk>(revokedAuth).method)); const uos::runtime::http::Addr remote = uos::runtime::http::NetAddr{"tcp", "192.0.2.10", 8000}; localAdmin::configureAdminAuthPeerForRequest(remote); AWAIT(remoteAuth, auth::authenticateClient(localRequest)); EXPECT(std::holds_alternative<auth::AuthenticateClientResultNotOk>(remoteAuth));
}
ASYNC_TEST("the Mac LAN listener grants the local principal to an actual loopback peer only") {
  Fixture fixture; const auto kv = fixture.kv; AWAIT(created, ensure(kv)); EXPECT_EQ(created, "created"); AuthScope scope; const auto localRequest = authRequest("http://127.42.9.3/v1/chat/completions"); const auto lanRequest = authRequest("http://127.42.9.3/v1/chat/completions"); const uos::runtime::http::Addr listener = uos::runtime::http::NetAddr{"tcp", "0.0.0.0", 7999}; const uos::runtime::http::Addr peer = uos::runtime::http::NetAddr{"tcp", "127.0.0.1", 7999}; const uos::runtime::http::Addr remote = uos::runtime::http::NetAddr{"tcp", "192.0.2.10", 7999}; localAdmin::configureMacLocalAdminAuthBypassForListener(listener); localAdmin::configureAdminAuthPeerForRequest(peer, localRequest); localAdmin::configureAdminAuthPeerForRequest(remote, lanRequest);
  AWAIT(lanAuth, auth::authenticateClient(lanRequest)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultNotOk>(lanAuth)); EXPECT_EQ(std::get<auth::AuthenticateClientResultNotOk>(lanAuth).response.status(), 401); AWAIT(localAuth, auth::authenticateClient(localRequest)); ASSERT(std::holds_alternative<auth::AuthenticateClientResultOk>(localAuth)); const auto& method = std::get<auth::AuthenticateClientResultOk>(localAuth).method; EXPECT(std::holds_alternative<auth::ClientAuthMethodKvApiKey>(method) || std::holds_alternative<auth::ClientAuthMethodDisabled>(method));
}
