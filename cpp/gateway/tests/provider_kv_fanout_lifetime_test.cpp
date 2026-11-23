// Links the sources of tests/codex_supervisor_test.cpp.
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/authority/native_paid.hpp"
#include "uos/codex/manual_reset.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/embeddings/handlers.hpp"
#include "uos/embeddings/ledger.hpp"
#include "uos/embeddings/voyage.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace codex = uos::codex;
  namespace reset = codex::reset_settings;
  namespace codex_keys = uos::authority::native_codex_contracts;
  namespace paid_keys = uos::authority::native_paid;
  namespace backfill = uos::paid_fallback::ledger_backfill;
  namespace settlement = uos::paid_fallback::ledger_settlement;
  namespace health = uos::provider::health;
  namespace selection = uos::provider::selection;
  namespace embeddings = uos::embeddings;
  namespace ledger = embeddings::ledger;
  namespace voyage = embeddings::voyage;

  struct ReadControl {
    rt::Resolvers<> resume = rt::withResolvers();
    rt::Resolvers<> completed = rt::withResolvers();
    rt::Reason failure = rt::makeReason(rt::JsError("fixture read rejected"));
    bool suspended = false;
  };

  class FanoutKv final : public rt::ForwardingKv {
  public:
    FanoutKv(rt::Kv& store, ReadControl& control, rt::KvKey rejectKey, rt::KvKey suspendKey, std::optional<rt::KvKey> quotaKey = std::nullopt)
        : rt::ForwardingKv(store), control_(control), rejectKey_(std::move(rejectKey)), suspendKey_(std::move(suspendKey)), quotaKey_(std::move(quotaKey)) {}

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
      if (key == rejectKey_) rt::rethrow(control_.failure);
      if (quotaKey_ && key == *quotaKey_) throw rt::JsError("KV quota exceeded");
      if (key == suspendKey_) {
        control_.suspended = true;
        co_await control_.resume.promise.wait();
        // This virtual implementation needs its receiver after the gate opens.
        auto read = rt::ForwardingKv::get(std::move(key), options);
        auto entry = co_await std::move(read);
        control_.completed.resolve();
        co_return entry;
      }
      auto read = rt::ForwardingKv::get(std::move(key), options);
      auto entry = co_await std::move(read);
      co_return entry;
    }

  private:
    ReadControl& control_;
    rt::KvKey rejectKey_;
    rt::KvKey suspendKey_;
    std::optional<rt::KvKey> quotaKey_;
  };

  rt::Task<void> finishSibling(ReadControl& control, std::weak_ptr<FanoutKv> receiver) {
    ASSERT(control.suspended);
    EXPECT(control.completed.promise.pending());
    EXPECT(!receiver.expired());
    control.resume.resolve();
    co_await control.completed.promise.wait();
    EXPECT(receiver.expired());
  }
}

ASYNC_TEST("manual reset fails fast while a sibling read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  auto kv = std::make_shared<FanoutKv>(store, control, codex_keys::CODEX_AUTH_POOL_KV_KEY, selection::PROVIDER_SELECTION_KV_KEY);
  const std::weak_ptr<FanoutKv> receiver = kv;
  codex::manual_reset::ManualResetDependencies dependencies;
  dependencies.kv = std::move(kv);
  dependencies.reloadConfig = [] { return codex::banked_reset::CodexBankedResetConfig{true, "live", 1, 1}; };
  auto request = codex::manual_reset::consumeManualCodexReset("fixture", std::move(dependencies));
  EXPECT_REJECTS_REASON(std::move(request), control.failure);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("reset opt-out migration fails fast while a sibling read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  auto kv = std::make_shared<FanoutKv>(store, control, reset::CODEX_BANKED_RESET_USAGE_LEGACY_KEY, codex_keys::CODEX_AUTH_POOL_KV_KEY);
  const std::weak_ptr<FanoutKv> receiver = kv;
  auto request = reset::migrateLegacyCodexResetOptOut(std::move(kv));
  EXPECT_REJECTS_REASON(std::move(request), control.failure);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("paid backfill fails fast while a sibling read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  auto kv = std::make_shared<FanoutKv>(store, control, paid_keys::paidFallbackBackfillStateV3Key(), paid_keys::paidFallbackBackfillCursorV3Key());
  const std::weak_ptr<FanoutKv> receiver = kv;
  auto request = backfill::runPaidFallbackBackfillV3(std::move(kv));
  EXPECT_REJECTS_REASON(std::move(request), control.failure);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("paid deferral fails fast while a sibling read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  auto kv = std::make_shared<FanoutKv>(store, control, paid_keys::paidFallbackRequestV3Key("key", "request"),
                                     paid_keys::paidFallbackPendingV3Key("key", "request"));
  const std::weak_ptr<FanoutKv> receiver = kv;
  auto request = settlement::deferPaidFallbackReconciliationV3(std::move(kv), "key", "request", rt::dateNow());
  EXPECT_REJECTS_REASON(std::move(request), control.failure);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("paid settlement fails fast while its outstanding gate read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  auto kv = std::make_shared<FanoutKv>(store, control, paid_keys::paidFallbackRequestV3Key("key", "request"),
                                     paid_keys::paidFallbackReconciliationGateV3Key());
  const std::weak_ptr<FanoutKv> receiver = kv;
  const uos::authority::native_paid_contracts::NativePaidProviderLog log{};
  auto request = settlement::settlePaidFallbackRequestV3(std::move(kv), "key", "request", log, rt::dateNow());
  EXPECT_REJECTS_REASON(std::move(request), control.failure);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("provider health fails fast while a sibling read retains the replaced KV") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  const rt::KvKey current{health::PROVIDER_HEALTH_KEY_PREFIX, "metered", "default", "current"};
  const rt::KvKey success{health::PROVIDER_HEALTH_KEY_PREFIX, "metered", "default", "success"};
  auto kv = std::make_shared<FanoutKv>(store, control, current, success);
  const std::weak_ptr<FanoutKv> receiver = kv;
  uos::kv::setKvForTest(std::move(kv));
  const auto resetStore = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); });
  auto request = health::getMeteredProviderHealth();
  EXPECT_REJECTS_REASON(std::move(request), control.failure);
  uos::kv::setKvForTest(nullptr);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("an embeddings cache lookup fails fast while a sibling read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  ReadControl control;
  const rt::GlobalFetchOverride blocked(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    throw rt::JsError("cache regression must stay offline");
  }));
  const rt::KvValue apiKey("fixture");
  auto seeding = store.set(ledger::VOYAGE_API_KEY_KV_KEY, apiKey);
  co_await std::move(seeding);
  const json::object body{{"model", "voyage-4-large"}, {"input", json::array{"first", "second"}}};
  const auto parsed = voyage::parseUosEmbeddingsRequest(body);
  const auto& profile = std::get<ledger::EmbeddingsParseResultOk>(parsed).value.profile.cache_profile_key;
  auto firstHashing = uos::utils::sha256Hex("first");
  const auto firstHash = co_await std::move(firstHashing);
  auto secondHashing = uos::utils::sha256Hex("second");
  const auto secondHash = co_await std::move(secondHashing);
  auto kv = std::make_shared<FanoutKv>(store, control, ledger::embeddingsCacheKey(profile, firstHash), ledger::embeddingsCacheKey(profile, secondHash));
  const std::weak_ptr<FanoutKv> receiver = kv;
  embeddings::handlers::HandleUosEmbeddingsOptions options{.kv = std::move(kv)};
  const rt::Request request("https://gateway.test/uos/embeddings",
                            rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::jsonStringify(body)});
  auto handling = embeddings::handlers::handleUosEmbeddings(request, nullptr, std::move(options));
  EXPECT_REJECTS_REASON(std::move(handling), control.failure);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("embeddings replay returns unavailable while a sibling chunk read retains its KV receiver") {
  const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow);
  const std::string fingerprint(64, 'f');
  const ledger::AcquireEmbeddingsIdempotencyLeaseParams seed{store, "principal", "key", fingerprint, "seed"};
  auto acquiring = ledger::acquireEmbeddingsIdempotencyLease(seed);
  const auto acquired = co_await std::move(acquiring);
  const auto& lease = std::get<ledger::EmbeddingsIdempotencyAcquireResultAcquired>(acquired).lease;
  const rt::Response body(std::string(50'000, 'a'));
  auto seeding = ledger::storeEmbeddingsIdempotencySuccess(lease, body);
  const bool stored = co_await std::move(seeding);
  ASSERT(stored);
  ReadControl control;
  const rt::KvKey first{lease.responseKeyPrefix, lease.ownerRequestId, 0};
  const rt::KvKey second{lease.responseKeyPrefix, lease.ownerRequestId, 1};
  auto kv = std::make_shared<FanoutKv>(*store, control, first, second);
  const std::weak_ptr<FanoutKv> receiver = kv;
  ledger::AcquireEmbeddingsIdempotencyLeaseParams params{std::move(kv), "principal", "key", fingerprint, "retry"};
  auto replay = ledger::acquireEmbeddingsIdempotencyLease(std::move(params));
  const auto result = co_await std::move(replay);
  const auto* error = std::get_if<ledger::EmbeddingsIdempotencyAcquireResultError>(&result);
  ASSERT(error);
  EXPECT_EQ(error->response.status(), 503);
  co_await finishSibling(control, receiver);
}

ASYNC_TEST("embeddings cache eviction abandons the write while a sibling read retains its KV receiver") {
  rt::MemoryKv store(rt::dateNow);
  const std::string profile = "fixture";
  const auto first = ledger::embeddingsCacheGlobalIndexKey(1, profile, "first");
  const auto second = ledger::embeddingsCacheGlobalIndexKey(2, profile, "second");
  const rt::KvValue one(1);
  auto writeFirst = store.set(first, one);
  co_await std::move(writeFirst);
  auto writeSecond = store.set(second, one);
  co_await std::move(writeSecond);
  ReadControl control;
  auto kv = std::make_shared<FanoutKv>(store, control, ledger::embeddingsCacheIndexByHashKey(profile, "first"),
                                     ledger::embeddingsCacheIndexByHashKey(profile, "second"), ledger::embeddingsCacheIndexByHashKey(profile, "new"));
  const std::weak_ptr<FanoutKv> receiver = kv;
  const std::vector<double> embedding{1.0};
  const double now = rt::dateNow();
  auto writing = voyage::writeEmbeddingsCacheEntryBestEffort(std::move(kv), profile, "new", embedding, now, now + 30'000);
  const auto result = co_await std::move(writing);
  EXPECT(!result.isNew);
  co_await finishSibling(control, receiver);
}
