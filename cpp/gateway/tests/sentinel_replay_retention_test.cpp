// Links: tests/sentinel_replay_retention_test.cpp src/sentinel/replay_store.cpp src/sentinel/replay_retention.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp
//        src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp
//        src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp
//        src/request.cpp src/utils.cpp src/config.cpp src/release.cpp src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-retention.test.ts. Storage is `openKv(":memory:")`: rt::MemoryKv, installed with
// setKvForTest where the TS installs it (and always cleared again). The TS Proxies are rt::ForwardingKv /
// rt::ForwardingAtomicOperation subclasses recording the same calls. `for (const scenario of absentManifestCases)` is
// one test per scenario sharing a body.
#include "uos/kv.hpp"
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention.hpp"
#include "uos/sentinel/replay_retention_bootstrap.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/utils.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "helpers/live_allocations.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace kvmodule = uos::kv;
  using namespace uos::sentinel::replay_model;
  using namespace uos::sentinel::replay_retention;
  using namespace uos::sentinel::replay_retention_schema;
  using uos::sentinel::replay_limits::SENTINEL_REPLAY_BUDGET_BYTES;
  using uos::sentinel::replay_limits::SENTINEL_REPLAY_MAX_METADATA_BYTES;
  using uos::sentinel::replay_limits::sentinelReplayReservationBytes;
  using uos::sentinel::replay_observation::isSentinelReplayCaptureStatusRow;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::SentinelReplayPersistResult;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultDuplicate;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultIncomplete;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;
  using uos::sentinel::replay_read::decryptExportedSentinelReplay;
  using uos::sentinel::replay_read::EncryptedSentinelRequestReplays;
  using uos::sentinel::replay_read::listEncryptedSentinelReplaysByRequestId;
  using uos::sentinel::replay_read::readSentinelReplayCaptureStatus;
  using uos::sentinel::replay_store::persistEncryptedSentinelReplay;

  double kvClock() { return rt::dateNow(); }

  using Store = std::shared_ptr<rt::MemoryKv>;
  Store openStore() { return std::make_shared<rt::MemoryKv>(kvClock); }

  // `try { ... } finally { kv.close(); setKvForTest(null); }`
  struct InstalledStore {
    Store kv;
    explicit InstalledStore(Store store) : kv(std::move(store)) { kvmodule::setKvForTest(kv); }
    ~InstalledStore() {
      kv->close();
      kvmodule::setKvForTest(nullptr);
    }
  };

  SentinelFailureObservation failureObservation() {
    return {
      .status = 502,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "chatgpt_codex",
    };
  }

  AcceptedSentinelReplayInput syntheticInput(std::shared_ptr<rt::Bytes> body, const std::string& requestId) {
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = std::move(body),
      .content_type = "application/json",
      .compatibility_headers = {},
      .request_id = requestId,
      .git_sha = std::string(40, 'a'),
      .deno_revision = "retention-fixture",
    };
  }

  /** WebCrypto refuses a single getRandomValues buffer above 65,536 bytes. */
  rt::Bytes fillRandom(std::size_t bytes) {
    rt::Bytes out(bytes);
    for (std::size_t offset = 0; offset < bytes; offset += 65'536) {
      rt::crypto::getRandomValues(std::span<std::uint8_t>(out).subspan(offset, std::min<std::size_t>(65'536, bytes - offset)));
    }
    return out;
  }

  /**
   * A deterministic per-request body whose stored charge is large and essentially
   * incompressible (random base64), so a small test budget can exercise eviction
   * without writing anywhere near a real GiB.
   */
  std::shared_ptr<rt::Bytes> chargedBody(const std::string& requestId) {
    static rt::OrderedMap<std::string, std::shared_ptr<rt::Bytes>> bodyCache;
    if (const std::shared_ptr<rt::Bytes>* cached = bodyCache.get(requestId)) return *cached;
    json::object body;
    body["model"] = "gpt-5.6-sol";
    body["request"] = json::string(requestId);
    body["blob"] = rt::toBase64(fillRandom(256 * 1'024));
    auto bytes = std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(body)))));
    bodyCache.set(requestId, bytes);
    return bytes;
  }

  /**
   * Budgets are derived from the real accounting contract, never guessed:
   * HEADROOM admits several captures with zero evictions, and a tight budget adds
   * the ledger's stored bytes to a lower bound of the next reservation so the
   * next admission must evict the oldest capture.
   */
  const double HEADROOM_BUDGET_BYTES = 4 * sentinelReplayReservationBytes(256 * 1'024 + 1'024, SENTINEL_REPLAY_MAX_METADATA_BYTES);
  constexpr double MIN_TEST_BUDGET_BYTES = 64 * 1'024;
  double reservationBound(const rt::Bytes& body) {
    return sentinelReplayReservationBytes(static_cast<double>(body.size()) + 4, SENTINEL_REPLAY_MAX_METADATA_BYTES);
  }
  double storedPlusReserved(const SentinelReplayRetentionStatus& status) { return status.stored_bytes.value_or(0) + status.reserved_bytes.value_or(0); }

  rt::Bytes newKey() {
    rt::Bytes key(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(key));
    return key;
  }

  std::string_view statusOf(const SentinelReplayPersistResult& result) {
    return std::visit([](const auto& alternative) -> std::string_view { return std::decay_t<decltype(alternative)>::status; }, result);
  }
  std::string reasonOf(const SentinelReplayPersistResult& result) {
    const auto* incomplete = std::get_if<SentinelReplayPersistResultIncomplete>(&result);
    return incomplete ? incomplete->reason : std::string();
  }
  const SentinelReplayManifest& manifestOf(const SentinelReplayPersistResult& result) {
    const auto* stored = std::get_if<SentinelReplayPersistResultStored>(&result);
    if (!stored) throw rt::JsError("the capture was not stored");
    return stored->manifest;
  }

  rt::Task<SentinelReplayPersistResult> persistBody(std::shared_ptr<rt::Kv> kv, rt::Bytes keyBytes, std::string requestId, std::optional<double> budgetBytes,
                                                    double nowMs, std::shared_ptr<rt::Bytes> body) {
    const PersistDependencies dependencies{
      .kv = std::move(kv),
      .keyBytes = std::move(keyBytes),
      .now = [nowMs] { return nowMs; },
      .randomUuid = [requestId] { return "capture-" + requestId; },
      .budgetBytes = budgetBytes,
    };
    const AcceptedSentinelReplayInput input = syntheticInput(std::move(body), requestId);
    const SentinelFailureObservation observation = failureObservation();
    auto persisting = persistEncryptedSentinelReplay(input, observation, dependencies);
    SentinelReplayPersistResult result = co_await std::move(persisting);
    co_return result;
  }

  rt::Task<SentinelReplayPersistResult> persist(std::shared_ptr<rt::Kv> kv, rt::Bytes keyBytes, std::string requestId, double budgetBytes, double nowMs) {
    std::shared_ptr<rt::Bytes> body = chargedBody(requestId);
    auto persisting = persistBody(std::move(kv), std::move(keyBytes), std::move(requestId), budgetBytes, nowMs, std::move(body));
    SentinelReplayPersistResult result = co_await std::move(persisting);
    co_return result;
  }

  rt::Task<std::optional<rt::KvKey>> firstManifestKey(rt::Kv& kv) {
    rt::KvListSelector selector;
    selector.prefix = SENTINEL_REPLAY_MANIFEST_PREFIX;
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
    if (!entry) co_return std::nullopt;
    co_await iterator->return_();
    co_return entry->key;
  }

  rt::Task<SentinelReplayRetentionStatus> maintain(rt::Kv& kv, double nowMs, double budgetBytes) {
    const SentinelReplayMaintenanceOptions options{.now_ms = nowMs, .budget_bytes = budgetBytes};
    auto maintaining = runSentinelReplayRetentionMaintenance(kv, options);
    SentinelReplayRetentionStatus status = co_await std::move(maintaining);
    co_return status;
  }

  rt::Task<SentinelReplayCaptureStatusRow> captureStatus(rt::Kv& kv, std::string requestId, double nowMs) {
    auto reading = readSentinelReplayCaptureStatus(kv, std::move(requestId), nowMs);
    SentinelReplayCaptureStatusRow row = co_await std::move(reading);
    co_return row;
  }
}

ASYNC_TEST("the injectable budget seam governs the production default path for admission and eviction") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const double now = 1'700'000'000'000;
  const auto store = [&installed, &keyBytes](std::string requestId, double at) {
    std::shared_ptr<rt::Bytes> body = chargedBody(requestId);
    // No budget override anywhere: the seam is the production default path.
    return persistBody(installed.kv, keyBytes, std::move(requestId), std::nullopt, at, std::move(body));
  };
  setSentinelReplayBudgetForTest(HEADROOM_BUDGET_BYTES);
  auto first = store("seam-oldest", now);
  const SentinelReplayPersistResult oldest = co_await std::move(first);
  EXPECT_EQ(statusOf(oldest), std::string_view("stored"));
  auto reading = readSentinelReplayRetentionStatus(kv);
  const SentinelReplayRetentionStatus seeded = co_await std::move(reading);
  EXPECT_EQ(seeded.budget_bytes, std::optional<double>(HEADROOM_BUDGET_BYTES));  // the seam value is the live budget, not a hard-coded GiB
  EXPECT_EQ(seeded.records, std::optional<double>(1));
  EXPECT_EQ(seeded.evicted_records, std::optional<double>(0));  // the first capture fits with no eviction

  // Tighten the live budget below stored + the next reservation's lower
  // bound, so the next admission must evict the oldest capture instead of
  // refusing or overshooting.
  const double tightBudget =
    seeded.stored_bytes.value_or(0) + sentinelReplayReservationBytes(static_cast<double>(chargedBody("seam-newest")->size()) + 4, 0) - 1;
  EXPECT(tightBudget > MIN_TEST_BUDGET_BYTES);
  setSentinelReplayBudgetForTest(tightBudget);

  auto second = store("seam-newest", now + 1);
  const SentinelReplayPersistResult newest = co_await std::move(second);
  EXPECT_EQ(statusOf(newest), std::string_view("stored"));  // bounded cleanup admits the newest capture
  auto rereading = readSentinelReplayRetentionStatus(kv);
  const SentinelReplayRetentionStatus after = co_await std::move(rereading);
  EXPECT_EQ(after.budget_bytes, std::optional<double>(tightBudget));
  EXPECT(after.evicted_records.value_or(0) >= 1);  // the oldest capture was evicted
  EXPECT(storedPlusReserved(after) <= tightBudget);  // the live budget still holds
  EXPECT_EQ(after.reserved_bytes, std::optional<double>(0));  // no reservation may leak after publication
  const SentinelReplayCaptureStatusRow oldestStatus = co_await captureStatus(kv, std::string("seam-oldest"), now + 1);
  EXPECT_EQ(oldestStatus.status, std::string("evicted"));
  const SentinelReplayCaptureStatusRow newestStatus = co_await captureStatus(kv, std::string("seam-newest"), now + 1);
  EXPECT_EQ(newestStatus.status, std::string("incomplete"));
  setSentinelReplayBudgetForTest(std::nullopt);
}

ASYNC_TEST("oldest capture is evicted first and its owner lookup reports evicted, not ready") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  auto storing = persist(installed.kv, keyBytes, "retention-oldest", HEADROOM_BUDGET_BYTES, 1'700'000'000'000);
  const SentinelReplayPersistResult first = co_await std::move(storing);
  EXPECT_EQ(statusOf(first), std::string_view("stored"));
  const SentinelReplayRetentionStatus seeded = co_await maintain(kv, 1'700'000'000'500, HEADROOM_BUDGET_BYTES);
  EXPECT_EQ(seeded.state, std::string("ok"));
  EXPECT_EQ(seeded.evicted_records, std::optional<double>(0));  // prepopulation must evict nothing
  EXPECT_EQ(seeded.records, std::optional<double>(1));
  EXPECT_EQ(seeded.reserved_bytes, std::optional<double>(0));
  // The next admission must cross this budget: stored so far plus a lower
  // bound of the next reservation, minus one byte.
  const std::shared_ptr<rt::Bytes> secondBody = chargedBody("retention-newest");
  const double tightBudget = seeded.stored_bytes.value_or(0) + sentinelReplayReservationBytes(static_cast<double>(secondBody->size()) + 4, 0) - 1;
  EXPECT(tightBudget > MIN_TEST_BUDGET_BYTES);

  auto storingSecond = persist(installed.kv, keyBytes, "retention-newest", tightBudget, 1'700'000'001'000);
  const SentinelReplayPersistResult second = co_await std::move(storingSecond);
  EXPECT_EQ(statusOf(second), std::string_view("stored"));  // the newest capture must be admitted after bounded cleanup

  const SentinelReplayCaptureStatusRow oldest = co_await captureStatus(kv, std::string("retention-oldest"), 1'700'000'001'000);
  EXPECT_EQ(oldest.status, std::string("evicted"));
  EXPECT_EQ(oldest.reason, std::optional<std::string>(SENTINEL_REPLAY_EVICTION_REASON));
  auto exportingOldest = listEncryptedSentinelReplaysByRequestId(kv, "retention-oldest", 1'700'000'001'000);
  const EncryptedSentinelRequestReplays oldestExport = co_await std::move(exportingOldest);
  EXPECT(oldestExport.captures.empty());
  EXPECT_EQ(oldestExport.status.status, std::string("evicted"));
  EXPECT_EQ(oldestExport.status.reason, std::optional<std::string>(SENTINEL_REPLAY_EVICTION_REASON));

  auto exportingNewest = listEncryptedSentinelReplaysByRequestId(kv, "retention-newest", 1'700'000'001'000);
  const EncryptedSentinelRequestReplays newest = co_await std::move(exportingNewest);
  ASSERT_EQ(newest.captures.size(), std::size_t{1});
  // This fixture sends no recorded upstream trace on a 502, so the capture is
  // truthfully partial. `limits.test.ts` covers the full-coverage `ready` path.
  EXPECT_EQ(newest.status.status, std::string("incomplete"));
  EXPECT(!newest.status.reason.has_value());
  auto decrypting = decryptExportedSentinelReplay(newest.captures[0], keyBytes);
  const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
  EXPECT(*plaintext.body == *secondBody);

  const SentinelReplayRetentionStatus retention = co_await maintain(kv, 1'700'000'002'000, tightBudget);
  EXPECT_EQ(retention.state, std::string("ok"));
  EXPECT(storedPlusReserved(retention) <= tightBudget);
  EXPECT_EQ(retention.reserved_bytes, std::optional<double>(0));  // no reservation may leak after publication
  EXPECT(retention.evicted_records.value_or(0) >= 1);
}

ASYNC_TEST("a duplicate status row uses the winning manifest's actual expiry") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const std::shared_ptr<rt::Bytes> body = chargedBody("duplicate-winner");
  // The conservative reservation bound of one capture admits the winner
  // without eviction; no guessed constant is involved.
  const double budgetBytes = reservationBound(*body);
  auto storingFirst = persistBody(installed.kv, keyBytes, "duplicate-winner", budgetBytes, 1'700'000'000'000, body);
  const SentinelReplayPersistResult first = co_await std::move(storingFirst);
  EXPECT_EQ(statusOf(first), std::string_view("stored"));
  EXPECT_EQ(manifestOf(first).expires_at_ms, 1'700'000'000'000 + SENTINEL_REPLAY_TTL_MS);
  // `randomUuid: () => "capture-duplicate-alias"`: the request id names the capture.
  auto storingSecond = persistBody(installed.kv, keyBytes, "duplicate-alias", budgetBytes, 1'700'000'500'000, body);
  const SentinelReplayPersistResult second = co_await std::move(storingSecond);
  EXPECT_EQ(statusOf(second), std::string_view("duplicate"));
  const SentinelReplayCaptureStatusRow alias = co_await captureStatus(kv, std::string("duplicate-alias"), 1'700'000'500'000);
  EXPECT_EQ(alias.status, std::string("incomplete"));  // the alias mirrors the winner's coverage, not a made-up ready
  EXPECT_EQ(alias.expires_at_ms, std::optional<double>(manifestOf(first).expires_at_ms));  // the alias must inherit the winner's expiry, never now + TTL
}

ASYNC_TEST("the same failure re-captures after its capture was evicted") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  auto storingA = persist(installed.kv, keyBytes, "recapture-a", HEADROOM_BUDGET_BYTES, 1'700'000'000'000);
  const SentinelReplayPersistResult a = co_await std::move(storingA);
  EXPECT_EQ(statusOf(a), std::string_view("stored"));
  const SentinelReplayRetentionStatus seeded = co_await maintain(kv, 1'700'000'000'500, HEADROOM_BUDGET_BYTES);
  EXPECT_EQ(seeded.evicted_records, std::optional<double>(0));  // prepopulation must evict nothing
  const std::shared_ptr<rt::Bytes> bodyB = chargedBody("recapture-b");
  const double tightBudget = seeded.stored_bytes.value_or(0) + sentinelReplayReservationBytes(static_cast<double>(bodyB->size()) + 4, 0) - 1;
  auto storingB = persist(installed.kv, keyBytes, "recapture-b", tightBudget, 1'700'000'001'000);
  const SentinelReplayPersistResult b = co_await std::move(storingB);
  EXPECT_EQ(statusOf(b), std::string_view("stored"));
  const SentinelReplayCaptureStatusRow evicted = co_await captureStatus(kv, std::string("recapture-a"), 1'700'000'001'000);
  EXPECT_EQ(evicted.status, std::string("evicted"));
  auto recapturing = persist(installed.kv, keyBytes, "recapture-a", tightBudget, 1'700'000'002'000);
  const SentinelReplayPersistResult recaptured = co_await std::move(recapturing);
  EXPECT_EQ(statusOf(recaptured), std::string_view("stored"));  // a stale dedupe row must not alias evicted evidence
  const SentinelReplayCaptureStatusRow again = co_await captureStatus(kv, std::string("recapture-a"), 1'700'000'002'000);
  EXPECT_EQ(again.status, std::string("incomplete"));
}

ASYNC_TEST("concurrent arrivals never overshoot the budget and leave no reservation") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  // Two concurrent arrivals under a budget that can admit both sequentially
  // but not both at once: at least one must succeed without overshooting.
  const std::shared_ptr<rt::Bytes> bodyA = chargedBody("concurrent-a");
  const std::shared_ptr<rt::Bytes> bodyB = chargedBody("concurrent-b");
  const double budgetBytes = sentinelReplayReservationBytes(static_cast<double>(bodyA->size()) + 4, 0) +
                             sentinelReplayReservationBytes(static_cast<double>(bodyB->size()) + 4, 0);
  std::vector<rt::Promise<SentinelReplayPersistResult>> arrivals;
  arrivals.push_back(rt::Promise<SentinelReplayPersistResult>(persist(installed.kv, keyBytes, "concurrent-a", budgetBytes, 1'700'000'000'000)));
  arrivals.push_back(rt::Promise<SentinelReplayPersistResult>(persist(installed.kv, keyBytes, "concurrent-b", budgetBytes, 1'700'000'000'100)));
  auto all = rt::all(std::move(arrivals));
  const std::vector<SentinelReplayPersistResult> results = co_await all.wait();
  bool anyStored = false;
  for (const SentinelReplayPersistResult& result : results) {
    const std::string_view status = statusOf(result);
    EXPECT(status == "stored" || status == "duplicate" || status == "incomplete");
    anyStored = anyStored || status == "stored" || status == "duplicate";
  }
  EXPECT(anyStored);
  const SentinelReplayRetentionStatus retention = co_await maintain(kv, 1'700'000'001'000, budgetBytes);
  EXPECT_EQ(retention.state, std::string("ok"));
  EXPECT(storedPlusReserved(retention) <= budgetBytes);
  EXPECT_EQ(retention.reserved_bytes, std::optional<double>(0));
  // An unrelated namespace must survive every eviction path untouched.
  rt::KvObject preserved;
  preserved.set("preserved", rt::KvValue(true));
  const rt::KvValue preservedValue(std::move(preserved));
  co_await kv.set(rt::KvKey{"ubq_ai", "api_keys", "id", "retention-unrelated"}, preservedValue);
  auto storingC = persist(installed.kv, keyBytes, "concurrent-c", budgetBytes, 1'700'000'002'000);
  co_await std::move(storingC);
  const rt::KvEntryMaybe<> unrelated = co_await kv.get(rt::KvKey{"ubq_ai", "api_keys", "id", "retention-unrelated"});
  EXPECT_EQ(unrelated.value, preservedValue);
}

ASYNC_TEST("pre-existing legacy manifests are bootstrapped into the budget, never treated as zero") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double capturedAtMs = 1'700'000'000'000;
  const SentinelReplayManifest legacy{
    .version = 1,
    .capture_id = "legacy-capture",
    .fingerprint = std::string(64, 'b'),
    .case_group_digest = std::string(64, 'c'),
    .captured_at_ms = capturedAtMs,
    .expires_at_ms = capturedAtMs + SENTINEL_REPLAY_TTL_MS,
    .algorithm = "AES-256-GCM",
    .compression = "gzip",
    .iv = uos::utils::base64UrlEncode(rt::Bytes(12, 0)),
    .chunk_count = 1,
    .ciphertext_bytes = 100,
  };
  const rt::KvValue legacyValue(json::value_from(legacy));
  co_await kv.set(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, capturedAtMs, legacy.fingerprint, legacy.capture_id}, legacyValue);
  const rt::KvValue chunk(rt::KvBytes(static_cast<std::size_t>(legacy.ciphertext_bytes), 0));
  co_await kv.set(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, legacy.capture_id, 0}, chunk);
  const SentinelReplayRetentionStatus retained = co_await maintain(kv, capturedAtMs + 1'000, SENTINEL_REPLAY_BUDGET_BYTES);
  EXPECT_EQ(retained.state, std::string("ok"));
  EXPECT_EQ(retained.accounting_complete, true);
  EXPECT(retained.stored_bytes.value_or(0) > 0);  // legacy retained bytes must be accounted, not assumed zero
  EXPECT_EQ(retained.records, std::optional<double>(1));
  const std::optional<rt::KvKey> manifestKey = co_await firstManifestKey(kv);
  EXPECT(manifestKey.has_value());
  const rt::KvKey accountingKey = sentinelReplayAccountingKey(capturedAtMs, legacy.capture_id);
  const rt::KvEntryMaybe<> accounting = co_await kv.get(accountingKey);
  EXPECT_EQ(accounting.value["state"], rt::KvValue("stored"));  // a legacy charge must have a durable owner that reclamation can select
  EXPECT_EQ(accounting.value["bytes"], rt::KvValue(sentinelReplayManifestCharge(legacy)));
  EXPECT_EQ(accounting.value["request_id"], rt::KvValue(""));  // an absent legacy owner must not become an invented request id
  EXPECT_EQ(accounting.value["expires_at_ms"], rt::KvValue(legacy.expires_at_ms));

  const SentinelReplayReclaimOptions reclaimOptions{.now_ms = legacy.expires_at_ms + 1, .budget_bytes = SENTINEL_REPLAY_BUDGET_BYTES};
  auto reclaiming = reclaimExpiredSentinelReplays(kv, reclaimOptions);
  const SentinelReplayReclamation expired = co_await std::move(reclaiming);
  EXPECT_EQ(expired.records, 1.0);
  const rt::KvEntryMaybe<> accountingGone = co_await kv.get(accountingKey);
  EXPECT(accountingGone.value.isNull());
  const rt::KvEntryMaybe<> chunkGone = co_await kv.get(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, legacy.capture_id, 0});
  EXPECT(chunkGone.value.isNull());
  const std::optional<rt::KvKey> manifestGone = co_await firstManifestKey(kv);
  EXPECT(!manifestGone.has_value());
  const rt::KvEntryMaybe<> emptyStatus = co_await kv.get(sentinelReplayRequestStatusKey(""));
  EXPECT(emptyStatus.value.isNull());  // ownerless cleanup must not write an empty request status
  const SentinelReplayRetentionStatus released = co_await maintain(kv, legacy.expires_at_ms + 2, SENTINEL_REPLAY_BUDGET_BYTES);
  EXPECT_EQ(released.stored_bytes, std::optional<double>(0));
  EXPECT_EQ(released.records, std::optional<double>(0));
  EXPECT_EQ(released.expired_records, std::optional<double>(1));
  EXPECT_EQ(released.evicted_records, std::optional<double>(0));
  EXPECT_EQ(released.status_records, std::optional<double>(1));  // only the fingerprint tombstone is retained without a request owner
}

ASYNC_TEST("a storage_full refusal keeps the newest failure discoverable and never stores unaccounted bytes") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  // The retention clamp's minimum injectable budget is exactly 64 KiB, which
  // is below one capture's reserved capacity, so admission must refuse.
  const double budgetBytes = MIN_TEST_BUDGET_BYTES;
  json::object body;
  body["model"] = "gpt-5.6-sol";
  body["input"] = json::string(std::string(200 * 1'024, 'y'));
  const PersistDependencies dependencies{
    .kv = installed.kv,
    .keyBytes = keyBytes,
    .now = [] { return 1'700'000'000'000.0; },
    .randomUuid = [] { return std::string("capture-too-large"); },
    .budgetBytes = budgetBytes,
  };
  const AcceptedSentinelReplayInput input =
    syntheticInput(std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(body))))), "too-large-for-budget");
  const SentinelFailureObservation observation = failureObservation();
  auto persisting = persistEncryptedSentinelReplay(input, observation, dependencies);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  EXPECT_EQ(statusOf(result), std::string_view("incomplete"));
  EXPECT_EQ(reasonOf(result), std::string(SENTINEL_REPLAY_STORAGE_FULL_REASON));
  auto reading = readSentinelReplayCaptureStatus(kv, "too-large-for-budget");
  const SentinelReplayCaptureStatusRow status = co_await std::move(reading);
  EXPECT_EQ(status.status, std::string("unknown"));  // the raw persist path does not invent a status row; the environment wrapper does
  std::vector<rt::KvKey> chunks;
  rt::KvListSelector selector;
  selector.prefix = rt::KvKey{"uos_ai", "sentinel_replay", "v1", "chunk"};
  const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
  while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) chunks.push_back(entry->key);
  EXPECT_EQ(chunks.size(), std::size_t{0});  // a refused capture must not leave staged chunks
}

// ---------------------------------------------------------------------------
// Accounting-row regressions: fencing, exactly-once eviction, bounded cleanup,
// TTL reclamation, dedupe provenance, the metadata bound and fail-closed
// bootstrap. Every case uses an in-memory KV, synthetic bytes and an injectable
// small budget or reserve; no GiB corpus, paid inference or live host.
// ---------------------------------------------------------------------------

namespace {
  constexpr double TEST_BUDGET_BYTES = 4 * 1'024 * 1'024;

  std::string fingerprintFor(const std::string& tag) {
    std::string out;
    for (const std::string& character : rt::codePointStrings(tag)) {
      const auto unit = static_cast<std::uint16_t>(rt::charCodeAt(character, 0));
      out += "0123456789abcdef"[unit & 0x0f];
    }
    return rt::slice(out + std::string(64, '0'), 0, 64);
  }

  rt::Task<SentinelReplayAdmission> admitForTest(rt::Kv& kv, std::string captureId, double nowMs, double budgetBytes = TEST_BUDGET_BYTES,
                                                 double plaintextBytes = 1'024) {
    const SentinelReplayCapacityInput input{
      .capture_id = captureId,
      .request_id = captureId,
      .plaintext_bytes = plaintextBytes,
      .metadata_bytes = 0,
      .fingerprint = fingerprintFor(captureId),
      .now_ms = nowMs,
      .budget_bytes = budgetBytes,
    };
    auto reserving = reserveSentinelReplayCapacity(kv, input);
    SentinelReplayAdmission admission = co_await std::move(reserving);
    co_return admission;
  }

  const SentinelReplayAdmissionOk& admitted(const SentinelReplayAdmission& admission) {
    const auto* ok = std::get_if<SentinelReplayAdmissionOk>(&admission);
    if (!ok) throw rt::JsError("the admission was refused");
    return *ok;
  }

  rt::Task<void> writeChunkRows(rt::Kv& kv, std::string captureId, double count) {
    rt::KvSetOptions options;
    options.expireIn = SENTINEL_REPLAY_TTL_MS;
    for (double index = 0; index < count; index += 1) {
      const rt::KvKey key{SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index};
      const rt::KvValue value(rt::KvBytes{static_cast<std::uint8_t>(rt::mod(index, 256))});
      co_await kv.set(key, value, options);
    }
  }

  rt::Task<double> countChunkRows(rt::Kv& kv, std::string captureId) {
    const rt::KvKey prefix{SENTINEL_REPLAY_CHUNK_PREFIX, captureId};
    double count = 0;
    rt::KvListSelector selector;
    selector.prefix = prefix;
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      // Count only rows that really sit under this capture's chunk prefix, so the
      // survival checks can never be satisfied by an unrelated stray key.
      if (entry->key.size() == prefix.size() + 1) count += 1;
    }
    co_return count;
  }

  rt::Task<SentinelReplayBudgetLedger> ledgerOf(rt::Kv& kv, double budgetBytes = TEST_BUDGET_BYTES) {
    auto reading = readSentinelReplayLedgerSnapshot(kv, budgetBytes);
    const std::optional<SentinelReplayLedgerSnapshot> snapshot = co_await std::move(reading);
    EXPECT(snapshot.has_value());  // the ledger must be readable
    if (!snapshot) throw rt::JsError("unreachable");
    co_return snapshot->ledger;
  }

  rt::Task<std::optional<SentinelReplayAccountingRow>> accountingRowOf(rt::Kv& kv, rt::KvKey key) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(std::move(key));
    if (entry.value.isNullish()) co_return std::nullopt;
    co_return json::value_to<SentinelReplayAccountingRow>(entry.value.toJson());
  }

  rt::Task<SentinelReplayAbandonment> abandon(rt::Kv& kv, SentinelReplayAccountingRow accounting, rt::KvKey key, SentinelReplayAbandonOptions options) {
    auto abandoning = abandonSentinelReplayAccounting(kv, accounting, key, options);
    SentinelReplayAbandonment result = co_await std::move(abandoning);
    co_return result;
  }
}

ASYNC_TEST("two eviction callers racing for the same victim evict it exactly once and hold no charge") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const double now = 1'700'100'000'000;
  auto storing = persist(installed.kv, keyBytes, "race-victim", HEADROOM_BUDGET_BYTES, now);
  const SentinelReplayPersistResult stored = co_await std::move(storing);
  EXPECT_EQ(statusOf(stored), std::string_view("stored"));
  const double charge = manifestOf(stored).stored_bytes.value_or(0);
  EXPECT(charge > 0);
  const double staged = co_await countChunkRows(kv, std::string("capture-race-victim"));
  EXPECT(staged > 0);

  const SentinelReplayEvictionOptions firstOptions{.target_bytes = 0, .now_ms = now + 1'000, .budget_bytes = HEADROOM_BUDGET_BYTES};
  const SentinelReplayEvictionOptions secondOptions{.target_bytes = 0, .now_ms = now + 1'001, .budget_bytes = HEADROOM_BUDGET_BYTES};
  rt::Promise<SentinelReplayEviction> firstEviction(evictSentinelReplays(kv, firstOptions));
  rt::Promise<SentinelReplayEviction> secondEviction(evictSentinelReplays(kv, secondOptions));
  auto both = rt::all(firstEviction, secondEviction);
  const std::tuple<SentinelReplayEviction, SentinelReplayEviction> results = co_await both.wait();
  EXPECT_EQ(std::get<0>(results).records + std::get<1>(results).records, 1.0);  // exactly one caller may win the eviction claim
  // Actual remaining payload rows, not the ledger alone.
  const double remaining = co_await countChunkRows(kv, std::string("capture-race-victim"));
  EXPECT_EQ(remaining, 0.0);
  const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv, HEADROOM_BUDGET_BYTES);
  EXPECT_EQ(ledger.stored_bytes, 0.0);  // the charge is released exactly once
  EXPECT_EQ(ledger.reserved_bytes, 0.0);
  EXPECT_EQ(ledger.records, 0.0);
  EXPECT_EQ(ledger.evicted_records, 1.0);
  EXPECT_EQ(ledger.evicted_bytes, charge);
}

ASYNC_TEST("a writer paused between batches cannot advance its fence or publish after a revoke") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double now = 1'700'200'000'000;
  const SentinelReplayAdmission admission = co_await admitForTest(kv, std::string("paused-writer"), now);
  ASSERT(std::get_if<SentinelReplayAdmissionOk>(&admission) != nullptr);
  const SentinelReplayAdmissionOk& ok = admitted(admission);
  const rt::KvKey key = ok.accounting_key;
  const double fence = ok.accounting.fence;

  const SentinelReplayStagingFenceInput fenceInput{.accounting_key = key, .fence = fence, .now_ms = now, .budget_bytes = TEST_BUDGET_BYTES};
  auto advancing = advanceSentinelReplayStagingFence(kv, fenceInput);
  const SentinelReplayStagingFence firstAdvance = co_await std::move(advancing);
  const auto* advanced = std::get_if<SentinelReplayStagingFenceOk>(&firstAdvance);
  ASSERT(advanced != nullptr);
  EXPECT_EQ(advanced->stage, 1.0);
  const rt::KvEntryMaybe<> current = co_await kv.get(key);
  EXPECT_EQ(std::optional<std::string>(advanced->versionstamp), current.versionstamp);
  co_await writeChunkRows(kv, std::string("paused-writer"), 2);

  // The reaper fences the paused writer off. One bounded pass leaves a chunk
  // behind, so the revoked row keeps its charge.
  const SentinelReplayAbandonOptions oneDelete{.now_ms = now, .max_chunk_deletes = 1, .budget_bytes = TEST_BUDGET_BYTES};
  const SentinelReplayAbandonment revoked = co_await abandon(kv, ok.accounting, key, oneDelete);
  EXPECT_EQ(revoked.revoked, true);
  EXPECT_EQ(revoked.released, false);  // a partial deletion must hold the charge
  const SentinelReplayBudgetLedger held = co_await ledgerOf(kv);
  EXPECT_EQ(held.reserved_bytes, ok.charge);

  // The writer resumes: its fence is stale, so it may not stage or publish.
  auto resuming = advanceSentinelReplayStagingFence(kv, fenceInput);
  const SentinelReplayStagingFence resumed = co_await std::move(resuming);
  EXPECT(resumed == SentinelReplayStagingFence(SentinelReplayStagingFenceNotOk{.reason = "revoked"}));
  const SentinelReplayPublicationOptions publicationOptions{
    .now_ms = now,
    .lease_now_ms = now,
    .status_key = sentinelReplayRequestStatusKey("paused-writer"),
    .budget_bytes = TEST_BUDGET_BYTES,
  };
  auto preparing = prepareSentinelReplayPublication(kv, ok.accounting, key, 1'000, publicationOptions);
  const std::optional<SentinelReplayPublication> publication = co_await std::move(preparing);
  EXPECT(!publication.has_value());  // a revoked writer can never publish

  // The resumed cleanup pass converges and releases exactly once.
  const std::optional<SentinelReplayAccountingRow> row = co_await accountingRowOf(kv, key);
  ASSERT(row.has_value());
  const SentinelReplayAbandonOptions fullDelete{.now_ms = now, .max_chunk_deletes = 512, .budget_bytes = TEST_BUDGET_BYTES};
  const SentinelReplayAbandonment resumedCleanup = co_await abandon(kv, *row, key, fullDelete);
  EXPECT_EQ(resumedCleanup.released, true);
  const double remaining = co_await countChunkRows(kv, std::string("paused-writer"));
  EXPECT_EQ(remaining, 0.0);
  const SentinelReplayBudgetLedger released = co_await ledgerOf(kv);
  EXPECT_EQ(released.reserved_bytes, 0.0);
  const std::optional<SentinelReplayAccountingRow> gone = co_await accountingRowOf(kv, key);
  EXPECT(!gone.has_value());
}

ASYNC_TEST("more than 512 chunks are cleaned across bounded passes with the charge held until empty") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double now = 1'700'250'000'000;
  const SentinelReplayAdmission admission = co_await admitForTest(kv, std::string("bounded-cleanup"), now);
  ASSERT(std::get_if<SentinelReplayAdmissionOk>(&admission) != nullptr);
  const SentinelReplayAdmissionOk& ok = admitted(admission);
  const rt::KvKey key = ok.accounting_key;
  co_await writeChunkRows(kv, std::string("bounded-cleanup"), 600);

  const SentinelReplayAbandonOptions options{.now_ms = now, .max_chunk_deletes = 512, .budget_bytes = TEST_BUDGET_BYTES};
  const SentinelReplayAbandonment first = co_await abandon(kv, ok.accounting, key, options);
  EXPECT_EQ(first.revoked, true);
  EXPECT_EQ(first.deleted_chunks, 512.0);
  EXPECT_EQ(first.released, false);  // the charge is held until the prefix is empty
  const double left = co_await countChunkRows(kv, std::string("bounded-cleanup"));
  EXPECT_EQ(left, 88.0);
  const SentinelReplayBudgetLedger held = co_await ledgerOf(kv);
  EXPECT_EQ(held.reserved_bytes, ok.charge);

  const std::optional<SentinelReplayAccountingRow> current = co_await accountingRowOf(kv, key);
  ASSERT(current.has_value());
  const SentinelReplayAbandonment second = co_await abandon(kv, *current, key, options);
  EXPECT_EQ(second.released, true);
  const double remaining = co_await countChunkRows(kv, std::string("bounded-cleanup"));
  EXPECT_EQ(remaining, 0.0);
  const SentinelReplayBudgetLedger released = co_await ledgerOf(kv);
  EXPECT_EQ(released.reserved_bytes, 0.0);
  const std::optional<SentinelReplayAccountingRow> gone = co_await accountingRowOf(kv, key);
  EXPECT(!gone.has_value());  // the accounting row stops existing only when its charge is released
}

namespace {
  // `setSentinelReplayRetentionFaultsForTest(null)` when the scope ends (`finally`).
  struct FaultScope {
    ~FaultScope() { setSentinelReplayRetentionFaultsForTest(nullptr); }
  };

  rt::Task<bool> rejects(rt::Task<SentinelReplayAbandonment> task) {
    bool rejected = false;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
    }
    co_return rejected;
  }
}

ASYNC_TEST("an injected delete and commit failure each converge through a restart-style continuation") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const FaultScope faults;
  const double now = 1'700'300'000'000;
  const SentinelReplayAbandonOptions options{.now_ms = now, .budget_bytes = TEST_BUDGET_BYTES};
  // Phase 1: a payload delete fails mid-cleanup.
  const SentinelReplayAdmission deleteAdmission = co_await admitForTest(kv, std::string("faulty-delete"), now);
  ASSERT(std::get_if<SentinelReplayAdmissionOk>(&deleteAdmission) != nullptr);
  const SentinelReplayAdmissionOk& deleteVictim = admitted(deleteAdmission);
  co_await writeChunkRows(kv, std::string("faulty-delete"), 3);
  auto deleteFailures = std::make_shared<int>(1);
  setSentinelReplayRetentionFaultsForTest([deleteFailures](const std::string& kind) {
    if (kind == "delete" && *deleteFailures > 0) {
      *deleteFailures -= 1;
      return true;
    }
    return false;
  });
  auto failingDelete = abandonSentinelReplayAccounting(kv, deleteVictim.accounting, deleteVictim.accounting_key, options);
  const bool deleteRejected = co_await rejects(std::move(failingDelete));
  EXPECT(deleteRejected);
  setSentinelReplayRetentionFaultsForTest(nullptr);
  const std::optional<SentinelReplayAccountingRow> afterDeleteFailure = co_await accountingRowOf(kv, deleteVictim.accounting_key);
  ASSERT(afterDeleteFailure.has_value());
  EXPECT_EQ(afterDeleteFailure->state, std::string("revoked"));
  const SentinelReplayBudgetLedger heldAfterDelete = co_await ledgerOf(kv);
  EXPECT_EQ(heldAfterDelete.reserved_bytes, deleteVictim.charge);  // the charge is held while chunks survive
  const double survivors = co_await countChunkRows(kv, std::string("faulty-delete"));
  EXPECT_EQ(survivors, 3.0);
  const SentinelReplayAbandonment deleteRecovery = co_await abandon(kv, *afterDeleteFailure, deleteVictim.accounting_key, options);
  EXPECT_EQ(deleteRecovery.released, true);  // a restart-style continuation converges
  const double drained = co_await countChunkRows(kv, std::string("faulty-delete"));
  EXPECT_EQ(drained, 0.0);

  // Phase 2: the release commit itself fails after the payload is gone.
  const SentinelReplayAdmission commitAdmission = co_await admitForTest(kv, std::string("faulty-commit"), now);
  ASSERT(std::get_if<SentinelReplayAdmissionOk>(&commitAdmission) != nullptr);
  const SentinelReplayAdmissionOk& commitVictim = admitted(commitAdmission);
  co_await writeChunkRows(kv, std::string("faulty-commit"), 2);
  auto commitFailures = std::make_shared<int>(1);
  setSentinelReplayRetentionFaultsForTest([commitFailures](const std::string& kind) {
    if (kind == "commit" && *commitFailures > 0) {
      *commitFailures -= 1;
      return true;
    }
    return false;
  });
  auto failingCommit = abandonSentinelReplayAccounting(kv, commitVictim.accounting, commitVictim.accounting_key, options);
  const bool commitRejected = co_await rejects(std::move(failingCommit));
  EXPECT(commitRejected);
  setSentinelReplayRetentionFaultsForTest(nullptr);
  const std::optional<SentinelReplayAccountingRow> afterCommitFailure = co_await accountingRowOf(kv, commitVictim.accounting_key);
  ASSERT(afterCommitFailure.has_value());
  EXPECT_EQ(afterCommitFailure->state, std::string("revoked"));
  const SentinelReplayBudgetLedger heldAfterCommit = co_await ledgerOf(kv);
  EXPECT_EQ(heldAfterCommit.reserved_bytes, commitVictim.charge);  // no ledger delta is applied without its row change
  const SentinelReplayAbandonment commitRecovery = co_await abandon(kv, *afterCommitFailure, commitVictim.accounting_key, options);
  EXPECT_EQ(commitRecovery.released, true);
  const SentinelReplayBudgetLedger released = co_await ledgerOf(kv);
  EXPECT_EQ(released.reserved_bytes, 0.0);
}

ASYNC_TEST("a resumable reaper reaches an expired reservation stranded in the accounting prefix") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double now = 1'700'350'000'000;
  const double budgetBytes = 8 * 1'024 * 1'024;
  const std::size_t middle = 32;
  std::vector<rt::KvKey> keys;
  double totalCharge = 0;
  for (std::size_t index = 0; index < middle * 2 + 1; ++index) {
    const SentinelReplayAdmission admission =
      co_await admitForTest(kv, "reap-stranded-" + rt::numberToString(static_cast<double>(index)), now + static_cast<double>(index), budgetBytes);
    ASSERT(std::get_if<SentinelReplayAdmissionOk>(&admission) != nullptr);
    keys.push_back(admitted(admission).accounting_key);
    totalCharge += admitted(admission).charge;
  }
  co_await kv.delete_(rt::KvKey{"uos_ai", "sentinel_replay", "v1", "reap_cursor"});
  const std::optional<SentinelReplayAccountingRow> stranded = co_await accountingRowOf(kv, keys[middle]);
  ASSERT(stranded.has_value());
  const double maintenanceNow = stranded->expires_at_ms + 1;
  for (std::size_t index = 0; index < keys.size(); ++index) {
    if (index == middle) continue;
    const std::optional<SentinelReplayAccountingRow> row = co_await accountingRowOf(kv, keys[index]);
    ASSERT(row.has_value());
    SentinelReplayAccountingRow restated = *row;
    restated.state = "stored";
    restated.expires_at_ms = maintenanceNow + 1;
    const rt::KvValue restatedValue(json::value_from(restated));
    co_await kv.set(keys[index], restatedValue);
  }
  SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv, budgetBytes);
  ledger.stored_bytes = totalCharge - stranded->bytes;
  ledger.reserved_bytes = stranded->bytes;
  ledger.records = static_cast<double>(keys.size()) - 1;
  const rt::KvValue ledgerValue(json::value_from(ledger));
  co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue);

  co_await maintain(kv, maintenanceNow, budgetBytes);
  const std::optional<SentinelReplayAccountingRow> notYet = co_await accountingRowOf(kv, keys[middle]);
  EXPECT(notYet.has_value());  // the first bounded page stops before the stranded row
  co_await maintain(kv, maintenanceNow, budgetBytes);
  const std::optional<SentinelReplayAccountingRow> reaped = co_await accountingRowOf(kv, keys[middle]);
  EXPECT(!reaped.has_value());  // the cursor reaches the middle on the next pass
  const SentinelReplayBudgetLedger after = co_await ledgerOf(kv, budgetBytes);
  EXPECT_EQ(after.stored_bytes, totalCharge - stranded->bytes);
  EXPECT_EQ(after.reserved_bytes, 0.0);
}

ASYNC_TEST("an expired payload is reclaimed exactly once as expired, never as evicted") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const double now = 1'700'400'000'000;
  auto storing = persist(installed.kv, keyBytes, "expiry-reclaim", HEADROOM_BUDGET_BYTES, now);
  const SentinelReplayPersistResult stored = co_await std::move(storing);
  EXPECT_EQ(statusOf(stored), std::string_view("stored"));
  const double charge = manifestOf(stored).stored_bytes.value_or(0);
  const double expiredAt = now + SENTINEL_REPLAY_TTL_MS + 1;

  const SentinelReplayReclaimOptions options{.now_ms = expiredAt, .budget_bytes = HEADROOM_BUDGET_BYTES};
  auto reclaiming = reclaimExpiredSentinelReplays(kv, options);
  const SentinelReplayReclamation reclaimed = co_await std::move(reclaiming);
  EXPECT_EQ(reclaimed.records, 1.0);
  EXPECT_EQ(reclaimed.bytes, charge);
  auto reclaimingAgain = reclaimExpiredSentinelReplays(kv, options);
  const SentinelReplayReclamation again = co_await std::move(reclaimingAgain);
  EXPECT_EQ(again.records, 0.0);  // reclamation is exactly once

  const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv, HEADROOM_BUDGET_BYTES);
  EXPECT_EQ(ledger.stored_bytes, 0.0);
  EXPECT_EQ(ledger.records, 0.0);
  EXPECT_EQ(ledger.expired_records, 1.0);  // expiry is not an eviction
  EXPECT_EQ(ledger.expired_bytes, charge);
  EXPECT_EQ(ledger.evicted_records, 0.0);
  EXPECT_EQ(ledger.evicted_bytes, 0.0);

  const SentinelReplayCaptureStatusRow status = co_await captureStatus(kv, std::string("expiry-reclaim"), expiredAt);
  EXPECT_EQ(status.status, std::string("expired"));
  EXPECT_EQ(status.reason, std::optional<std::string>(SENTINEL_REPLAY_EXPIRED_REASON));
  const double remaining = co_await countChunkRows(kv, std::string("capture-expiry-reclaim"));
  EXPECT_EQ(remaining, 0.0);
}

namespace {
  rt::KvValue keyValue(const rt::KvKey& key) {
    rt::KvArray parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(std::visit([](const auto& value) { return rt::KvValue(value); }, part.value));
    return rt::KvValue(std::move(parts));
  }

  // `key.join("\u0000")`
  std::string joinedKey(const rt::KvKey& key) {
    std::vector<std::string> parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(part.toString());
    return rt::join(parts, std::string(1, '\0'));
  }
}

ASYNC_TEST("a stale eviction cannot delete a newer dedupe row for the same fingerprint") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const double now = 1'700'500'000'000;
  auto storing = persist(installed.kv, keyBytes, "dedupe-victim", HEADROOM_BUDGET_BYTES, now);
  const SentinelReplayPersistResult stored = co_await std::move(storing);
  EXPECT_EQ(statusOf(stored), std::string_view("stored"));
  const SentinelReplayManifest& manifest = manifestOf(stored);
  const rt::KvKey dedupeKey{SENTINEL_REPLAY_DEDUPE_PREFIX, manifest.fingerprint};
  const rt::KvKey newerManifestKey{SENTINEL_REPLAY_MANIFEST_PREFIX, now + 60'000, manifest.fingerprint, "newer-capture"};
  // A newer capture with the same fingerprint now owns the dedupe row.
  rt::KvObject newer;
  newer.set("manifest_key", keyValue(newerManifestKey));
  newer.set("captured_at_ms", rt::KvValue(now + 60'000));
  newer.set("expires_at_ms", rt::KvValue(now + 60'000 + SENTINEL_REPLAY_TTL_MS));
  const rt::KvValue newerValue(std::move(newer));
  rt::KvSetOptions options;
  options.expireIn = SENTINEL_REPLAY_TTL_MS;
  co_await kv.set(dedupeKey, newerValue, options);

  const SentinelReplayEvictionOptions eviction{.target_bytes = 0, .now_ms = now + 120'000, .budget_bytes = HEADROOM_BUDGET_BYTES};
  auto evicting = evictSentinelReplays(kv, eviction);
  const SentinelReplayEviction evicted = co_await std::move(evicting);
  EXPECT_EQ(evicted.records, 1.0);
  const rt::KvEntryMaybe<> surviving = co_await kv.get(dedupeKey);
  EXPECT(!surviving.value.isNull());  // the newer owner's dedupe row must survive
  EXPECT_EQ(surviving.value["manifest_key"], keyValue(newerManifestKey));
}

namespace {
  // `type DedupeCommitObservation`
  struct DedupeCommitObservation {
    rt::KvKey key;
    std::vector<rt::AtomicCheck> checks;
    bool deletesDedupe = false;
    std::function<void(const std::vector<rt::AtomicCheck>& checks, const rt::KvCommitResult& result)> afterCommit;
  };

  class ObservedDedupeAtomic final : public rt::ForwardingAtomicOperation {
  public:
    ObservedDedupeAtomic(std::unique_ptr<rt::AtomicOperation> operation, DedupeCommitObservation observation)
        : rt::ForwardingAtomicOperation(std::move(operation)), observation_(std::move(observation)) {}
    using rt::ForwardingAtomicOperation::check;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      for (const rt::AtomicCheck& item : items) observation_.checks.push_back(item);
      inner->check(std::move(items));
      return *this;
    }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      inner->set(std::move(key), std::move(value), options);
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      if (joinedKey(key) == joinedKey(observation_.key)) observation_.deletesDedupe = true;
      inner->delete_(std::move(key));
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      rt::KvCommitResult result = co_await inner->commit();
      if (observation_.deletesDedupe) observation_.afterCommit(observation_.checks, result);
      co_return result;
    }

  private:
    DedupeCommitObservation observation_;
  };

  struct AbsentManifestProbe {
    std::string scenario;
    rt::KvKey manifestKey;
    rt::KvKey dedupeKey;
    rt::KvValue replacement;
    bool interleaved = false;
    std::vector<rt::AtomicCheck> nativeChecks{};
    std::vector<bool> nativeOutcomes{};
  };

  rt::Task<rt::KvEntryMaybe<>> probedGet(rt::Kv& kv, std::shared_ptr<AbsentManifestProbe> probe, rt::KvKey key, rt::KvGetOptions options) {
    rt::KvEntryMaybe<> entry = co_await kv.get(key, options);
    const bool isManifest = joinedKey(key) == joinedKey(probe->manifestKey);
    const bool isDedupe = joinedKey(key) == joinedKey(probe->dedupeKey);
    if ((probe->scenario == "manifest-read" && isManifest) || (probe->scenario == "dedupe-read" && isDedupe)) {
      throw rt::JsError("synthetic cleanup read failure");
    }
    if (probe->scenario == "raced" && isDedupe && !probe->interleaved) {
      probe->interleaved = true;
      co_await kv.set(probe->dedupeKey, probe->replacement);
      const rt::KvEntryMaybe<> raced = co_await kv.get(probe->dedupeKey);
      EXPECT(raced.versionstamp != entry.versionstamp);
    }
    co_return entry;
  }

  class AbsentManifestKv final : public rt::ForwardingKv {
  public:
    AbsentManifestKv(rt::Kv& kv, std::shared_ptr<AbsentManifestProbe> probe) : rt::ForwardingKv(kv), probe_(std::move(probe)) {}
    using rt::ForwardingKv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override { return probedGet(inner, probe_, std::move(key), options); }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      const std::shared_ptr<AbsentManifestProbe> probe = probe_;
      DedupeCommitObservation observation{
        .key = probe_->dedupeKey,
        .checks = {},
        .deletesDedupe = false,
        .afterCommit =
          [probe](const std::vector<rt::AtomicCheck>& checks, const rt::KvCommitResult& result) {
            probe->nativeChecks.insert(probe->nativeChecks.end(), checks.begin(), checks.end());
            probe->nativeOutcomes.push_back(result.ok);
          },
      };
      return std::make_unique<ObservedDedupeAtomic>(inner.atomic(), std::move(observation));
    }

  private:
    std::shared_ptr<AbsentManifestProbe> probe_;
  };

  bool sameEntry(const rt::KvEntryMaybe<>& left, const rt::KvEntryMaybe<>& right) {
    return joinedKey(left.key) == joinedKey(right.key) && left.value == right.value && left.versionstamp == right.versionstamp;
  }

  rt::Task<void> claimedPayloadCleanupPreservesProvenance(std::string scenario) {
    const Store store = openStore();
    rt::MemoryKv& kv = *store;
    const double now = 1'700'550'000'000;
    const std::string requestId = "absent-manifest-" + scenario;
    auto storing = persist(store, newKey(), requestId, HEADROOM_BUDGET_BYTES, now);
    const SentinelReplayPersistResult stored = co_await std::move(storing);
    EXPECT_EQ(statusOf(stored), std::string_view("stored"));
    const std::optional<rt::KvKey> foundManifestKey = co_await firstManifestKey(kv);
    ASSERT(foundManifestKey.has_value());  // stored capture has no manifest
    const rt::KvKey manifestKey = *foundManifestKey;
    const SentinelReplayManifest manifest = manifestOf(stored);
    const rt::KvKey dedupeKey{SENTINEL_REPLAY_DEDUPE_PREFIX, manifest.fingerprint};
    const rt::KvKey accountingKey = sentinelReplayAccountingKey(now, manifest.capture_id);
    const rt::KvEntryMaybe<> originalDedupe = co_await kv.get(dedupeKey);
    rt::KvObject expectedDedupe;
    expectedDedupe.set("manifest_key", keyValue(manifestKey));
    expectedDedupe.set("captured_at_ms", rt::KvValue(now));
    expectedDedupe.set("expires_at_ms", rt::KvValue(manifest.expires_at_ms));
    EXPECT_EQ(originalDedupe.value, rt::KvValue(std::move(expectedDedupe)));
    const double staged = co_await countChunkRows(kv, manifest.capture_id);
    EXPECT(staged > 1);
    const SentinelReplayEvictionOptions partialOptions{
      .target_bytes = 0,
      .max_chunk_deletes = 1,
      .now_ms = now + 1,
      .budget_bytes = HEADROOM_BUDGET_BYTES,
    };
    auto evicting = evictSentinelReplays(kv, partialOptions);
    const SentinelReplayEviction partial = co_await std::move(evicting);
    EXPECT_EQ(partial.records, 0.0);
    const std::optional<SentinelReplayAccountingRow> claimed = co_await accountingRowOf(kv, accountingKey);
    EXPECT(claimed.has_value() && claimed->state == "evicting");
    const SentinelReplayBudgetLedger charged = co_await ledgerOf(kv, HEADROOM_BUDGET_BYTES);
    EXPECT_EQ(std::optional<double>(charged.stored_bytes), manifest.stored_bytes);
    // Model native TTL/manual loss of only the manifest after the durable claim.
    co_await kv.delete_(manifestKey);
    const rt::KvEntryMaybe<> lost = co_await kv.get(manifestKey);
    EXPECT(!lost.versionstamp.has_value());
    rt::KvObject replacementObject;
    replacementObject.set("manifest_key", keyValue(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, now + 60'000, manifest.fingerprint, "newer-capture"}));
    replacementObject.set("captured_at_ms", rt::KvValue(now + 60'000));
    replacementObject.set("expires_at_ms", rt::KvValue(manifest.expires_at_ms + 60'000));
    const rt::KvValue replacement(std::move(replacementObject));
    if (scenario == "newer") co_await kv.set(dedupeKey, replacement);
    if (scenario == "malformed") {
      rt::KvObject malformed;
      malformed.set("not", rt::KvValue("a manifest"));
      const rt::KvValue malformedValue(std::move(malformed));
      co_await kv.set(manifestKey, malformedValue);
    }
    if (scenario == "stored-null") {
      const rt::KvValue nullValue(nullptr);
      co_await kv.set(manifestKey, nullValue);
    }
    if (scenario == "mismatched") {
      SentinelReplayManifest mismatched = manifest;
      mismatched.capture_id = "other-capture";
      const rt::KvValue mismatchedValue(json::value_from(mismatched));
      co_await kv.set(manifestKey, mismatchedValue);
    }
    const rt::KvEntryMaybe<> presentManifest = co_await kv.get(manifestKey);
    const auto probe = std::make_shared<AbsentManifestProbe>(AbsentManifestProbe{
      .scenario = scenario,
      .manifestKey = manifestKey,
      .dedupeKey = dedupeKey,
      .replacement = replacement,
    });
    AbsentManifestKv wrapped(kv, probe);
    const SentinelReplayMaintenanceOptions maintenance{.now_ms = now + 2, .budget_bytes = HEADROOM_BUDGET_BYTES};
    if (scenario == "manifest-read" || scenario == "dedupe-read") {
      auto failing = runSentinelReplayRetentionMaintenance(wrapped, maintenance);
      EXPECT_REJECTS_MESSAGE(std::move(failing), rt::JsError, "synthetic cleanup read failure");
      const SentinelReplayBudgetLedger retained = co_await ledgerOf(kv, HEADROOM_BUDGET_BYTES);
      EXPECT(retained == charged);  // failed reads must retain the entire charge
      const std::optional<SentinelReplayAccountingRow> still = co_await accountingRowOf(kv, accountingKey);
      EXPECT(still.has_value() && still->state == "evicting");
      const rt::KvEntryMaybe<> dedupeAfterFailure = co_await kv.get(dedupeKey);
      EXPECT(sameEntry(dedupeAfterFailure, originalDedupe));
      auto recovering = runSentinelReplayRetentionMaintenance(kv, maintenance);
      co_await std::move(recovering);
    } else {
      auto maintaining = runSentinelReplayRetentionMaintenance(wrapped, maintenance);
      co_await std::move(maintaining);
    }
    const bool newerOwner = scenario == "newer" || scenario == "raced";
    const bool present = scenario == "malformed" || scenario == "stored-null" || scenario == "mismatched";
    const rt::KvEntryMaybe<> afterDedupe = co_await kv.get(dedupeKey);
    const rt::KvValue existingDedupe = present ? originalDedupe.value : rt::KvValue(nullptr);
    EXPECT_EQ(afterDedupe.value, newerOwner ? replacement : existingDedupe);
    const rt::KvEntryMaybe<> manifestAfter = co_await kv.get(manifestKey);
    EXPECT(sameEntry(manifestAfter, presentManifest));  // present malformed or mismatched manifests must survive unchanged
    if (scenario == "matching" || scenario == "raced") {
      EXPECT_EQ(probe->nativeChecks.size(), std::size_t{1});
      if (!probe->nativeChecks.empty()) {
        EXPECT_EQ(joinedKey(probe->nativeChecks[0].key), joinedKey(dedupeKey));
        EXPECT_EQ(probe->nativeChecks[0].versionstamp, originalDedupe.versionstamp);
      }
      EXPECT_EQ(probe->nativeOutcomes, std::vector<bool>{scenario == "matching"});  // native CAS rejects a dedupe overwrite after its read
      EXPECT_EQ(probe->interleaved, scenario == "raced");
    } else {
      EXPECT(probe->nativeOutcomes.empty());
    }
    const double remaining = co_await countChunkRows(kv, manifest.capture_id);
    EXPECT_EQ(remaining, 0.0);
    const std::optional<SentinelReplayAccountingRow> released = co_await accountingRowOf(kv, accountingKey);
    EXPECT(!released.has_value());
    const SentinelReplayBudgetLedger releasedLedger = co_await ledgerOf(kv, HEADROOM_BUDGET_BYTES);
    EXPECT_EQ(releasedLedger.stored_bytes, 0.0);
    EXPECT_EQ(releasedLedger.records, 0.0);
    EXPECT_EQ(std::optional<double>(releasedLedger.evicted_bytes), manifest.stored_bytes);
    EXPECT_EQ(releasedLedger.evicted_records, 1.0);
    if (!present) {
      auto continuing = runSentinelReplayRetentionMaintenance(kv, maintenance);
      co_await std::move(continuing);
      const SentinelReplayBudgetLedger continued = co_await ledgerOf(kv, HEADROOM_BUDGET_BYTES);
      EXPECT(continued == releasedLedger);  // continuation must release and count the victim only once
      const rt::KvEntryMaybe<> dedupeAfterContinuation = co_await kv.get(dedupeKey);
      EXPECT(sameEntry(dedupeAfterContinuation, afterDedupe));
    }
    kv.close();
  }
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: matching") {
  auto running = claimedPayloadCleanupPreservesProvenance("matching");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: newer") {
  auto running = claimedPayloadCleanupPreservesProvenance("newer");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: raced") {
  auto running = claimedPayloadCleanupPreservesProvenance("raced");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: malformed") {
  auto running = claimedPayloadCleanupPreservesProvenance("malformed");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: stored-null") {
  auto running = claimedPayloadCleanupPreservesProvenance("stored-null");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: mismatched") {
  auto running = claimedPayloadCleanupPreservesProvenance("mismatched");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: manifest-read") {
  auto running = claimedPayloadCleanupPreservesProvenance("manifest-read");
  co_await std::move(running);
}

ASYNC_TEST("claimed payload cleanup preserves provenance after manifest loss: dedupe-read") {
  auto running = claimedPayloadCleanupPreservesProvenance("dedupe-read");
  co_await std::move(running);
}

namespace {
  // `{ version: 1, request_id, status: "disabled", reason: "retention_pressure", captured_at_ms, ... }`
  rt::KvValue pressureRow(const std::string& requestId, double capturedAtMs) {
    rt::KvObject row;
    row.set("version", rt::KvValue(1));
    row.set("request_id", rt::KvValue(requestId));
    row.set("status", rt::KvValue("disabled"));
    row.set("reason", rt::KvValue("retention_pressure"));
    row.set("captured_at_ms", rt::KvValue(capturedAtMs));
    row.set("manifest_key", rt::KvValue(nullptr));
    row.set("fingerprint", rt::KvValue(nullptr));
    row.set("expires_at_ms", rt::KvValue(nullptr));
    return rt::KvValue(std::move(row));
  }
}

ASYNC_TEST("status and tombstone pressure is pruned at the metadata reserve and reports not-retained") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double budgetBytes = 1 * 1'024 * 1'024;
  const double reserve = rt::Math::floor(budgetBytes / 16);
  const double now = 1'700'600'000'000;
  for (double index = 0; index < 12; index += 1) {
    const std::string requestId = "pressure-" + rt::numberToString(index);
    const SentinelReplayStatusMetadataInput input{
      .key = rt::KvKey{SENTINEL_REPLAY_REQUEST_PREFIX, requestId},
      .row = pressureRow(requestId, now + index),
      .now_ms = now + index,
      .ttl_ms = SENTINEL_REPLAY_TTL_MS,
      .budget_bytes = budgetBytes,
    };
    auto admitting = admitSentinelReplayStatusMetadata(kv, input);
    co_await std::move(admitting);
  }
  // An eviction tombstone shares the same bounded metadata namespace.
  rt::KvObject tombstone;
  tombstone.set("version", rt::KvValue(1));
  tombstone.set("fingerprint", rt::KvValue(fingerprintFor("tombstone")));
  tombstone.set("request_id", rt::KvValue("pressure-tombstone"));
  tombstone.set("evicted_at_ms", rt::KvValue(now - 1'000));
  tombstone.set("reason", rt::KvValue(SENTINEL_REPLAY_EVICTION_REASON));
  const rt::KvValue tombstoneValue(std::move(tombstone));
  co_await kv.set(rt::KvKey{SENTINEL_REPLAY_EVICTION_PREFIX, fingerprintFor("tombstone")}, tombstoneValue);

  const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv, budgetBytes);
  EXPECT(ledger.metadata_bytes <= reserve);  // capture-owned status metadata must stay inside the reserve
  EXPECT(ledger.status_records < 12);
  EXPECT(ledger.status_pruned_records >= 1);
  auto readingOldest = readSentinelReplayCaptureStatus(kv, "pressure-0");
  const SentinelReplayCaptureStatusRow oldest = co_await std::move(readingOldest);
  EXPECT_EQ(oldest.status, std::string(SENTINEL_REPLAY_STATUS_NOT_RETAINED));
  auto readingNewest = readSentinelReplayCaptureStatus(kv, "pressure-11");
  const SentinelReplayCaptureStatusRow newest = co_await std::move(readingNewest);
  EXPECT_EQ(newest.status, std::string("disabled"));  // the newest row survives
}

ASYNC_TEST("status pruning selects the oldest timestamp beyond the key-order scan") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double budgetBytes = 64 * 1'024;
  const double now = 1'700'600'000'000;
  const std::string oldestRequestId = "zzzz-oldest";
  const std::string newestRequestId = "request-000";
  const rt::KvValue oldestRow = pressureRow(oldestRequestId, now);
  co_await kv.set(sentinelReplayRequestStatusKey(oldestRequestId), oldestRow);
  for (double index = 0; index < 128; index += 1) {
    const std::string requestId = "request-" + rt::padStart(rt::numberToString(index), 3, "0");
    const rt::KvValue row = pressureRow(requestId, now + index + 1);
    co_await kv.set(sentinelReplayRequestStatusKey(requestId), row);
  }
  SentinelReplayRetentionStatus maintenance = co_await maintain(kv, now, budgetBytes);
  for (int pass = 0; pass < 200 && !maintenance.accounting_complete; pass += 1) {
    maintenance = co_await maintain(kv, now, budgetBytes);
  }
  EXPECT_EQ(maintenance.accounting_complete, true);

  auto pruning = pruneCaptureOwnedStatusMetadata(kv, now, 0, 0, 1, budgetBytes);
  const double pruned = co_await std::move(pruning);
  EXPECT_EQ(pruned, 1.0);
  const rt::KvEntryMaybe<> oldest = co_await kv.get(sentinelReplayRequestStatusKey(oldestRequestId));
  EXPECT(oldest.value.isNull());
  const rt::KvEntryMaybe<> newest = co_await kv.get(sentinelReplayRequestStatusKey(newestRequestId));
  EXPECT(!newest.value.isNull());
}

ASYNC_TEST("an invalid in-scope row fails bootstrap closed instead of completing at zero") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::KvKey corruptKey{SENTINEL_REPLAY_MANIFEST_PREFIX, 1, std::string(64, 'f'), "corrupt-capture"};
  const double now = 1'700'700'000'000;
  rt::KvObject corrupt;
  corrupt.set("not", rt::KvValue("a manifest"));
  const rt::KvValue corruptValue(std::move(corrupt));
  co_await kv.set(corruptKey, corruptValue);
  const SentinelReplayRetentionStatus first = co_await maintain(kv, now, TEST_BUDGET_BYTES);
  EXPECT_EQ(first.accounting_complete, false);  // an unaccountable row must never look complete
  EXPECT_EQ(first.accounting_error, std::optional<std::string>("invalid_in_scope_row"));
  const SentinelReplayAdmission refused = co_await admitForTest(kv, std::string("blocked-by-corrupt"), now + 1);
  const auto* notOk = std::get_if<SentinelReplayAdmissionNotOk>(&refused);
  ASSERT(notOk != nullptr);
  EXPECT_EQ(notOk->reason, std::string(SENTINEL_REPLAY_ACCOUNTING_REASON));

  // A later pass resolves it once the corrupt row is gone.
  co_await kv.delete_(corruptKey);
  const SentinelReplayRetentionStatus second = co_await maintain(kv, now + 2, TEST_BUDGET_BYTES);
  EXPECT_EQ(second.accounting_complete, true);
  EXPECT(!second.accounting_error.has_value());
  const SentinelReplayAdmission resolved = co_await admitForTest(kv, std::string("blocked-by-corrupt"), now + 3);
  EXPECT(std::get_if<SentinelReplayAdmissionOk>(&resolved) != nullptr);
}

ASYNC_TEST("a corrupt ledger fails closed instead of being replaced with an empty ledger") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const double now = 1'700'800'000'000;
  const SentinelReplayBudgetLedger corrupt{
    .version = 1,
    .budget_bytes = -1,
    .stored_bytes = 0,
    .reserved_bytes = 0,
    .records = 0,
    .status_records = 0,
    .metadata_bytes = 0,
    .evicted_bytes = 0,
    .evicted_records = 0,
    .expired_bytes = 0,
    .expired_records = 0,
    .status_pruned_records = 0,
    .last_eviction_at_ms = std::nullopt,
    .last_warning_at_ms = std::nullopt,
    .last_full_at_ms = std::nullopt,
    .last_skip_reason = std::nullopt,
    .bootstrap_complete = true,
    .bootstrap_cursor = std::nullopt,
    .accounting_error = std::nullopt,
    .over_budget = false,
  };
  const rt::KvValue corruptValue(json::value_from(corrupt));
  co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, corruptValue);
  auto reading = readSentinelReplayLedgerSnapshot(kv, TEST_BUDGET_BYTES);
  const std::optional<SentinelReplayLedgerSnapshot> snapshot = co_await std::move(reading);
  EXPECT(!snapshot.has_value());  // a corrupt ledger is reported, never zeroed

  const SentinelReplayRetentionStatus status = co_await maintain(kv, now, TEST_BUDGET_BYTES);
  EXPECT_EQ(status.accounting_complete, false);
  EXPECT_EQ(status.accounting_error, std::optional<std::string>("ledger_corrupt"));
  EXPECT(!status.stored_bytes.has_value());  // an unreadable ledger is never reported as zero

  auto refusing = persist(installed.kv, keyBytes, "corrupt-ledger", TEST_BUDGET_BYTES, now + 1);
  const SentinelReplayPersistResult refused = co_await std::move(refusing);
  EXPECT_EQ(statusOf(refused), std::string_view("incomplete"));
  EXPECT_EQ(reasonOf(refused), std::string(SENTINEL_REPLAY_ACCOUNTING_REASON));

  const rt::KvEntryMaybe<> preserved = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT_EQ(preserved.value["budget_bytes"], rt::KvValue(-1));  // the corrupt ledger must not be silently replaced
}

namespace {
  struct LiveStatusMetadata {
    double records;
    double bytes;
  };

  /**
   * The metadata bound is an admission gate: publication and status admission are
   * both refused once `metadata_bytes` or `status_records` reaches its bound.
   * Counting WRITES instead of live rows inflated those counters on every
   * eviction, expiry and repeated status write, so the gate drifted away from the
   * row set it describes and would eventually refuse every capture while the store
   * was nearly empty.
   */
  rt::Task<LiveStatusMetadata> liveStatusMetadata(rt::Kv& kv) {
    double records = 0;
    double bytes = 0;
    {
      rt::KvListSelector selector;
      selector.prefix = SENTINEL_REPLAY_REQUEST_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
      while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (!isSentinelReplayCaptureStatusRow(entry->value)) continue;
        records += 1;
        bytes += sentinelReplayStatusMetadataBytes(entry->value);
      }
    }
    {
      rt::KvListSelector selector;
      selector.prefix = SENTINEL_REPLAY_EVICTION_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
      while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (entry->value.isNull()) continue;
        records += 1;
        bytes += sentinelReplayStatusMetadataBytes(entry->value);
      }
    }
    co_return LiveStatusMetadata{.records = records, .bytes = bytes};
  }

  /** The ledger counters must describe the live status/tombstone rows exactly. */
  rt::Task<void> assertMetadataMatchesRows(rt::Kv& kv, std::string label, double budgetBytes = TEST_BUDGET_BYTES) {
    const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv, budgetBytes);
    const LiveStatusMetadata live = co_await liveStatusMetadata(kv);
    if (ledger.status_records != live.records || ledger.metadata_bytes != live.bytes) {
      EXPECT(false);
      std::fprintf(stderr, "      %s: ledger {status_records: %s, metadata_bytes: %s} live {records: %s, bytes: %s}\n", label.c_str(),
                   rt::numberToString(ledger.status_records).c_str(), rt::numberToString(ledger.metadata_bytes).c_str(),
                   rt::numberToString(live.records).c_str(), rt::numberToString(live.bytes).c_str());
    }
  }
}

ASYNC_TEST("status and tombstone counters stay row-exact across eviction, expiry and repeated writes") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const rt::Bytes keyBytes = newKey();
  const double now = 1'700'900'000'000;
  const double budgetBytes = HEADROOM_BUDGET_BYTES;
  // Publishing one capture accounts exactly its own status row.
  auto storing = persist(installed.kv, keyBytes, "row-exact-victim", budgetBytes, now);
  const SentinelReplayPersistResult stored = co_await std::move(storing);
  EXPECT_EQ(statusOf(stored), std::string_view("stored"));
  co_await assertMetadataMatchesRows(kv, std::string("after publish"), budgetBytes);

  // Eviction deletes that status row and writes a replacement plus a
  // tombstone: the counters move by the net difference, not by a whole row.
  const SentinelReplayEvictionOptions eviction{.target_bytes = 0, .now_ms = now + 1'000, .budget_bytes = budgetBytes};
  auto evicting = evictSentinelReplays(kv, eviction);
  const SentinelReplayEviction evicted = co_await std::move(evicting);
  EXPECT_EQ(evicted.records, 1.0);
  co_await assertMetadataMatchesRows(kv, std::string("after eviction"), budgetBytes);

  // A duplicate capture rewrites the SAME request status row.
  auto storingFirst = persist(installed.kv, keyBytes, "row-exact-duplicate", budgetBytes, now + 2'000);
  const SentinelReplayPersistResult first = co_await std::move(storingFirst);
  EXPECT_EQ(statusOf(first), std::string_view("stored"));
  auto storingSecond = persist(installed.kv, keyBytes, "row-exact-duplicate", budgetBytes, now + 3'000);
  const SentinelReplayPersistResult second = co_await std::move(storingSecond);
  EXPECT_EQ(statusOf(second), std::string_view("duplicate"));
  co_await assertMetadataMatchesRows(kv, std::string("after a duplicate capture"), budgetBytes);

  // TTL reclamation deletes and replaces a status row too.
  auto storingReclaimable = persist(installed.kv, keyBytes, "row-exact-expired", budgetBytes, now + 4'000);
  const SentinelReplayPersistResult reclaimable = co_await std::move(storingReclaimable);
  EXPECT_EQ(statusOf(reclaimable), std::string_view("stored"));
  const SentinelReplayReclaimOptions reclaim{.now_ms = now + 4'000 + SENTINEL_REPLAY_TTL_MS + 1, .max_records = 1, .budget_bytes = budgetBytes};
  auto reclaiming = reclaimExpiredSentinelReplays(kv, reclaim);
  const SentinelReplayReclamation reclaimed = co_await std::move(reclaiming);
  EXPECT_EQ(reclaimed.records, 1.0);
  co_await assertMetadataMatchesRows(kv, std::string("after expiry"), budgetBytes);

  // Phantom growth would have pruned live rows to satisfy a bound that no
  // longer described them, and the next capture would eventually be refused.
  const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv, budgetBytes);
  EXPECT_EQ(ledger.status_pruned_records, 0.0);  // no live status row may be pruned for a phantom bound
  auto storingAfter = persist(installed.kv, keyBytes, "row-exact-after", budgetBytes, now + 6'000);
  const SentinelReplayPersistResult after = co_await std::move(storingAfter);
  EXPECT_EQ(statusOf(after), std::string_view("stored"));
  co_await assertMetadataMatchesRows(kv, std::string("after a later capture"), budgetBytes);
}

/**
 * A fresh ledger is not authoritative until the resumable sweep has counted the
 * rows that already exist. A row admitted by its own writer while the sweep is
 * still pending is therefore counted twice: once by that admission commit and
 * once by the sweep. Both counters share the same two admission gates, so the
 * phantom growth would refuse status writes and finally whole captures.
 */
ASYNC_TEST("a status row admitted into an unbootstrapped ledger is counted exactly once") {
  const InstalledStore installed(openStore());
  rt::MemoryKv& kv = *installed.kv;
  const double now = 1'701'000'000'000;
  const double budgetBytes = 1 * 1'024 * 1'024;
  const std::string requestId = "bootstrap-race";
  rt::KvValue row = pressureRow(requestId, now);
  row.asObject().set("reason", rt::KvValue("bootstrap_race"));
  const SentinelReplayStatusMetadataInput input{
    .key = rt::KvKey{SENTINEL_REPLAY_REQUEST_PREFIX, requestId},
    .row = row,
    .now_ms = now,
    .ttl_ms = SENTINEL_REPLAY_TTL_MS,
    .budget_bytes = budgetBytes,
  };
  auto admitting = admitSentinelReplayStatusMetadata(kv, input);
  const bool admitted = co_await std::move(admitting);
  EXPECT_EQ(admitted, true);  // a fresh ledger must bootstrap so its first status row can still be admitted
  co_await assertMetadataMatchesRows(kv, std::string("after admitting into an unbootstrapped ledger"), budgetBytes);

  // The sweep must not count the same row a second time.
  const SentinelReplayRetentionStatus maintained = co_await maintain(kv, now + 1, budgetBytes);
  EXPECT_EQ(maintained.accounting_complete, true);
  co_await assertMetadataMatchesRows(kv, std::string("after the accounting sweep"), budgetBytes);
}

namespace {
  SentinelReplayManifest legacyManifestFor(double index, double capturedAtMs, std::optional<std::string> requestId = std::nullopt) {
    char hex[8];
    const auto end = std::to_chars(hex, hex + sizeof hex, static_cast<std::uint32_t>(index), 16).ptr;
    return {
      .version = 1,
      .capture_id = "legacy-bootstrap-" + rt::numberToString(index),
      .fingerprint = rt::padStart(std::string(hex, end), 64, "0"),
      .case_group_digest = std::string(64, 'c'),
      .captured_at_ms = capturedAtMs,
      .expires_at_ms = capturedAtMs + SENTINEL_REPLAY_TTL_MS,
      .algorithm = "AES-256-GCM",
      .compression = "gzip",
      .iv = uos::utils::base64UrlEncode(rt::Bytes(12, 0)),
      .chunk_count = 1,
      .ciphertext_bytes = 100,
      .request_id = std::move(requestId),
    };
  }

  rt::KvKey legacyManifestKey(const SentinelReplayManifest& manifest) {
    return rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, manifest.captured_at_ms, manifest.fingerprint, manifest.capture_id};
  }

  struct BootstrapCommitObservation {
    std::function<rt::Task<void>()> beforeCommit;
    std::function<void(const rt::KvCommitResult& result)> afterCommit;
  };

  /** Observe and interleave a real native bootstrap commit, retaining every chained operation. */
  class ObservedBootstrapAtomic final : public rt::ForwardingAtomicOperation {
  public:
    ObservedBootstrapAtomic(std::unique_ptr<rt::AtomicOperation> operation, std::shared_ptr<BootstrapCommitObservation> observation)
        : rt::ForwardingAtomicOperation(std::move(operation)), observation_(std::move(observation)) {}
    using rt::ForwardingAtomicOperation::check;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      inner->check(std::move(items));
      return *this;
    }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      if (key.size() > 3 && key[3].isString() && key[3].string() == "accounting") materializes_ = true;
      inner->set(std::move(key), std::move(value), options);
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      inner->delete_(std::move(key));
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      if (materializes_) {
        auto before = observation_->beforeCommit();
        co_await std::move(before);
      }
      rt::KvCommitResult result = co_await inner->commit();
      if (materializes_) observation_->afterCommit(result);
      co_return result;
    }

  private:
    std::shared_ptr<BootstrapCommitObservation> observation_;
    bool materializes_ = false;
  };

  class ObserveBootstrapCommits final : public rt::ForwardingKv {
  public:
    ObserveBootstrapCommits(rt::Kv& kv, std::shared_ptr<BootstrapCommitObservation> observation) : rt::ForwardingKv(kv), observation_(std::move(observation)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ObservedBootstrapAtomic>(inner.atomic(), observation_); }

  private:
    std::shared_ptr<BootstrapCommitObservation> observation_;
  };
}

ASYNC_TEST("a bootstrapped legacy capture with an owner is evicted and releases its charge once") {
  const Store store = openStore();
  rt::MemoryKv& kv = *store;
  const double now = 1'701'100'000'000;
  const SentinelReplayManifest legacy = legacyManifestFor(1, now, "legacy-owned");
  const rt::KvKey unrelatedKey{"uos_ai", "legacy-bootstrap-unrelated"};
  const rt::KvValue legacyValue(json::value_from(legacy));
  co_await kv.set(legacyManifestKey(legacy), legacyValue);
  const rt::KvValue chunk(rt::KvBytes(static_cast<std::size_t>(legacy.ciphertext_bytes), 0));
  co_await kv.set(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, legacy.capture_id, 0}, chunk);
  rt::KvObject preserved;
  preserved.set("preserved", rt::KvValue(true));
  const rt::KvValue preservedValue(std::move(preserved));
  co_await kv.set(unrelatedKey, preservedValue);
  const SentinelReplayRetentionStatus initial = co_await maintain(kv, now + 1, TEST_BUDGET_BYTES);
  EXPECT_EQ(initial.accounting_complete, true);
  const SentinelReplayEvictionOptions eviction{.target_bytes = 0, .now_ms = now + 2, .budget_bytes = TEST_BUDGET_BYTES};
  auto evicting = evictSentinelReplays(kv, eviction);
  const SentinelReplayEviction evicted = co_await std::move(evicting);
  EXPECT_EQ(evicted.records, 1.0);
  const SentinelReplayRetentionStatus after = co_await maintain(kv, now + 3, TEST_BUDGET_BYTES);
  EXPECT_EQ(after.stored_bytes, std::optional<double>(0));
  EXPECT_EQ(after.records, std::optional<double>(0));
  EXPECT_EQ(after.evicted_records, std::optional<double>(1));
  const rt::KvEntryMaybe<> accounting = co_await kv.get(sentinelReplayAccountingKey(now, legacy.capture_id));
  EXPECT(accounting.value.isNull());
  const SentinelReplayCaptureStatusRow owned = co_await captureStatus(kv, std::string("legacy-owned"), now + 3);
  EXPECT_EQ(owned.status, std::string("evicted"));
  const rt::KvEntryMaybe<> unrelated = co_await kv.get(unrelatedKey);
  EXPECT_EQ(unrelated.value, preservedValue);
  kv.close();
}

ASYNC_TEST("completed replay owner index fills release their wait promises") {
  const auto bootstrap = []() -> rt::Task<void> {
    const Store store = openStore();
    const double now = 1'701'100'000'000;
    const auto manifest = legacyManifestFor(1, now);
    const auto manifestKey = legacyManifestKey(manifest);
    const rt::KvKey chunkKey{SENTINEL_REPLAY_CHUNK_PREFIX, manifest.capture_id, 0};
    const rt::KvValue manifestValue(json::value_from(manifest));
    const rt::KvValue chunk(rt::KvBytes(static_cast<std::size_t>(manifest.ciphertext_bytes), 0));
    co_await store->set(manifestKey, manifestValue);
    co_await store->set(chunkKey, chunk);
    namespace sweep = uos::sentinel::replay_retention_bootstrap;
    auto filling = sweep::bootstrapBeforeMutation(*store, TEST_BUDGET_BYTES, now + 1, 60'000);
    const auto ledger = co_await std::move(filling);
    ASSERT(ledger);
    EXPECT(ledger->bootstrap_complete);
    store->close();
  };
  auto checking = uos::tests::helpers::expectStableLiveAllocations(bootstrap, 128);
  co_await std::move(checking);
}

ASYNC_TEST("legacy accounting materialization resumes across bootstrap batches without charging rows twice") {
  const Store store = openStore();
  rt::MemoryKv& kv = *store;
  const double now = 1'701'200'000'000;
  std::vector<SentinelReplayManifest> manifests;
  for (double index = 0; index < 39; index += 1) manifests.push_back(legacyManifestFor(index + 1, now + index));
  const double budgetBytes = SENTINEL_REPLAY_BUDGET_BYTES;
  for (const SentinelReplayManifest& manifest : manifests) {
    const rt::KvValue value(json::value_from(manifest));
    co_await kv.set(legacyManifestKey(manifest), value);
  }
  SentinelReplayRetentionStatus status = co_await maintain(kv, now + 1'000, budgetBytes);
  EXPECT_EQ(status.accounting_complete, false);  // one bounded pass must not claim to have swept all 39 manifests
  for (int pass = 0; pass < 100 && !status.accounting_complete; pass += 1) {
    status = co_await maintain(kv, now + 1'000, budgetBytes);
  }
  EXPECT_EQ(status.accounting_complete, true);
  EXPECT_EQ(status.records, std::optional<double>(static_cast<double>(manifests.size())));
  double charge = 0;
  for (const SentinelReplayManifest& manifest : manifests) charge += sentinelReplayManifestCharge(manifest);
  EXPECT_EQ(status.stored_bytes, std::optional<double>(charge));
  for (const SentinelReplayManifest& manifest : manifests) {
    const rt::KvEntryMaybe<> accounting = co_await kv.get(sentinelReplayAccountingKey(manifest.captured_at_ms, manifest.capture_id));
    EXPECT_EQ(accounting.value["state"], rt::KvValue("stored"));
  }
  const SentinelReplayRetentionStatus repeated = co_await maintain(kv, now + 1'001, budgetBytes);
  EXPECT_EQ(repeated.stored_bytes, status.stored_bytes);
  EXPECT_EQ(repeated.records, status.records);
  kv.close();
}

ASYNC_TEST("a changed legacy manifest rejects the whole bootstrap row and ledger transaction") {
  const Store store = openStore();
  rt::MemoryKv& kv = *store;
  const double now = 1'701'300'000'000;
  const SentinelReplayManifest manifest = legacyManifestFor(1, now);
  SentinelReplayManifest changed = manifest;
  changed.ciphertext_bytes = 101;
  auto interleaved = std::make_shared<bool>(false);
  const auto outcomes = std::make_shared<std::vector<bool>>();
  rt::Kv* raw = &kv;
  const auto observation = std::make_shared<BootstrapCommitObservation>();
  observation->beforeCommit = [raw, interleaved, manifest, changed]() -> rt::Task<void> {
    if (*interleaved) co_return;
    *interleaved = true;
    const rt::KvValue changedValue(json::value_from(changed));
    co_await raw->set(legacyManifestKey(manifest), changedValue);
  };
  observation->afterCommit = [outcomes](const rt::KvCommitResult& result) { outcomes->push_back(result.ok); };
  ObserveBootstrapCommits wrapped(kv, observation);
  const rt::KvValue manifestValue(json::value_from(manifest));
  co_await kv.set(legacyManifestKey(manifest), manifestValue);
  const SentinelReplayRetentionStatus raced = co_await maintain(wrapped, now + 1, TEST_BUDGET_BYTES);
  EXPECT_EQ(*interleaved, true);  // the fixture must observe a real accounting-row materialization transaction
  EXPECT_EQ(*outcomes, std::vector<bool>{false});  // the native manifest version check must reject the stale charge
  EXPECT_EQ(raced.accounting_complete, false);
  EXPECT_EQ(raced.records, std::optional<double>(0));
  EXPECT_EQ(raced.stored_bytes, std::optional<double>(0));
  const rt::KvEntryMaybe<> accounting = co_await kv.get(sentinelReplayAccountingKey(now, manifest.capture_id));
  EXPECT(accounting.value.isNull());
  const SentinelReplayRetentionStatus retried = co_await maintain(kv, now + 2, TEST_BUDGET_BYTES);
  EXPECT_EQ(retried.accounting_complete, true);
  EXPECT_EQ(retried.records, std::optional<double>(1));
  EXPECT_EQ(retried.stored_bytes, std::optional<double>(sentinelReplayManifestCharge(changed)));
  kv.close();
}
