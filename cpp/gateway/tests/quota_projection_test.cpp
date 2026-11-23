// Port of tests/quota-projection.test.ts; pure projection cases replay all exported calls from
// the original Bun assertions. Runnable ledger, balance-history and rollup cases remain direct.
// The TS shares one instance of the test's MemoryKv fake and clears it per case; these give each case
// an rt::MemoryKv, the store the fake stands in for (the cases read and write rows only through get,
// set, list and check/set commits, where the two agree). The fake's one-shot `atomicConflictOnce`
// is a forwarding wrapper (ConflictOnceKv). seedKeyRecord writes the key's two records as the TS
// literal does; the rollup cases never read them.
// PAID_FALLBACK_REQUEST_LOG_RETENTION_MS comes from src/paid-fallback/ledger-state.ts (re-exported
// from src/authority/native-paid.ts), as in the TS.
// Links: tests/quota_projection_test.cpp
// src/analytics.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_contracts.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
// src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
// src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
// src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp
// src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/defaults.cpp src/http.cpp
// src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp
// src/models/codex_models_fetch.cpp src/optional_telemetry_queue.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/rollups.cpp src/provider/anthropic_chat_responses.cpp
// src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp
// src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
// src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp
// src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/metered.cpp
// src/provider/selection.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp
// src/responses_stream.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
// src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp
// src/utils.cpp src/quota_projection.cpp src/kernel/usage.cpp src/kernel/quota_v2.cpp src/authority/native_kernel.cpp
// src/admin/kernel.cpp src/kernel/policy_queue.cpp src/admin/api_keys.cpp src/api_key_policy.cpp src/request.cpp
// src/authority/native_api_key.cpp src/admin/api_key_audit.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/kernel/quota_write.cpp
// src/kernel/quota_reservations.cpp src/kernel/attestation.cpp src/authority/native_kernel_contracts.cpp
// All 30 source cases are now callable, including the three real admin/kernel route cases.
// Handler/index route registration and administrator authentication are separate ingress assertions.
#include "uos/admin/kernel.hpp"
#include "uos/api_keys.hpp"
#include "uos/quota_projection.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/paid_fallback/ledger_stats.hpp"
#include "uos/paid_fallback/rollups.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::paid_fallback::ledger_stats;

namespace {
  using uos::paid_fallback::ledger_state::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS;

  std::shared_ptr<rt::Kv> isolatedKv() {
    return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  }
  const PaidFallbackLedgerProjectionSummary* windowOf(const PaidFallbackLedgerGrowthView& growth, double windowDays) {
    for (const PaidFallbackLedgerProjectionSummary& entry : growth.projections)
      if (entry.window_days == windowDays) return &entry;
    return nullptr;
  }

  namespace rollups = uos::paid_fallback::rollups;
  using rollups::PaidFallbackUsageRollup;
  using rollups::TerminalUsageRollupInput;
  using uos::authority::native_paid_contracts::PaidFallbackUsageRollupInput;

  const std::string keyId = "quota-projection-key";
  const std::string keyHash = "quota-projection-hash";
  constexpr double DAY_MS = 24.0 * 60 * 60 * 1'000;
  constexpr double HOUR_MS = 60.0 * 60 * 1'000;

  rt::Task<void> setRow(std::shared_ptr<rt::Kv> kv, rt::KvKey key, json::value value) {
    const rt::KvValue stored(value);
    auto writing = kv->set(std::move(key), stored);
    co_await std::move(writing);
  }

  // seedKeyRecord() on an empty store: the key's id and hash records.
  rt::Task<void> seedKeyRecord(std::shared_ptr<rt::Kv> kv) {
    const double now = rt::dateNow();
    json::value record = rt::jsonParse(R"({"id":"quota-projection-key","name":"Quota projection","prefix":"u_qp","hash":"quota-projection-hash",)"
                                       R"("created_at_ms":0,"expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":100,"usage_requests":0,)"
                                       R"("usage_reset_at_ms":0,"window_ms":60000,"usage_quota_version":3,"paid_fallback_enabled":true,)"
                                       R"("paid_fallback_limit_microcredits":5000000,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,)"
                                       R"("paid_fallback_reservation_request_id":null,"paid_fallback_model_ids":["gpt-5.6-sol"],"paid_fallback_quota_per_credit":500000,)"
                                       R"("paid_fallback_max_exposure_microcredits":{"gpt-5.6-sol":250000},"paid_fallback_pricing_checked_at_ms":0})");
    json::object& fields = record.as_object();
    fields["created_at_ms"] = rt::makeNumber(now - 60'000);
    fields["usage_reset_at_ms"] = rt::makeNumber(now + 60'000);
    fields["paid_fallback_pricing_checked_at_ms"] = rt::makeNumber(now - 30'000);
    json::object hashRecord;
    for (const char* key : {"id", "expires_at_ms", "revoked_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms",
                            "usage_quota_version", "paid_fallback_enabled", "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits",
                            "paid_fallback_reserved_microcredits", "paid_fallback_reservation_request_id"})
      hashRecord[key] = fields.at(key);
    auto writingId = setRow(kv, uos::api_keys::apiKeyIdKey(keyId), record);
    co_await std::move(writingId);
    auto writingHash = setRow(kv, uos::api_keys::apiKeyHashKey(keyHash), json::value(hashRecord));
    co_await std::move(writingHash);
  }

  rt::Task<bool> recordObservation(TerminalUsageRollupInput input, std::shared_ptr<rt::Kv> kv) {
    auto recording = rollups::recordTerminalUsageRollup(std::move(input), std::move(kv));
    const bool recorded = co_await std::move(recording);
    co_return recorded;
  }

  rt::Task<std::vector<PaidFallbackUsageRollup>> listRollups(std::shared_ptr<rt::Kv> kv, double sinceMs, double nowMs) {
    const rollups::ListPaidFallbackUsageRollupsOptions options{.sinceMs = sinceMs, .nowMs = nowMs};
    auto listing = rollups::listPaidFallbackUsageRollups(std::move(kv), options);
    std::vector<PaidFallbackUsageRollup> listed = co_await std::move(listing);
    co_return listed;
  }

  // The TS fake's `atomicConflictOnce`: the next commit fails once without writing.
  class ConflictOnceAtomic final : public rt::ForwardingAtomicOperation {
  public:
    ConflictOnceAtomic(std::unique_ptr<rt::AtomicOperation> inner_, bool& conflict_) : rt::ForwardingAtomicOperation(std::move(inner_)), conflict(conflict_) {}
    rt::Task<rt::KvCommitResult> commit() override {
      if (conflict) {
        conflict = false;
        rt::KvCommitResult refused;
        co_return refused;
      }
      auto committing = rt::ForwardingAtomicOperation::commit();
      rt::KvCommitResult result = co_await std::move(committing);
      co_return result;
    }

  private:
    bool& conflict;
  };
  class ConflictOnceKv final : public rt::ForwardingKv {
  public:
    explicit ConflictOnceKv(std::shared_ptr<rt::Kv> store_) : rt::ForwardingKv(*store_), store(std::move(store_)) {}
    bool atomicConflictOnce = false;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ConflictOnceAtomic>(inner.atomic(), atomicConflictOnce); }

  private:
    std::shared_ptr<rt::Kv> store;
  };
}

ASYNC_TEST("ledger growth view derives bytes-per-row and the storage alert threshold") {
  const double now = rt::dateNow();
  const double threshold = rt::Math::floor(PAID_FALLBACK_RAW_STORE_BUDGET_BYTES * PAID_FALLBACK_RAW_STORE_ALERT_RATIO);
  const std::shared_ptr<rt::Kv> isolated = isolatedKv();
  const double dayStart = paidFallbackLedgerStatsDayStart(now);
  const PaidFallbackLedgerDailyStats seeded{
    .v = 1,
    .day_start_at_ms = dayStart,
    .settled_rows = 1'000'000,
    .settled_row_bytes = threshold,
    .rollup_writes = 48,
    .rollup_bytes = 50'000,
    .projection_7d = {.views = 1, .read_units = 2, .rollup_rows = 3},
    .projection_30d = {.views = 2, .read_units = 100, .rollup_rows = 500},
    .projection_90d = {.views = 4, .read_units = 1'600, .rollup_rows = 8'000},
    .updated_at_ms = now,
  };
  const rt::KvValue seededValue(json::value_from(seeded));
  co_await isolated->set(paidFallbackLedgerStatsDayKey(dayStart), seededValue);
  const PaidFallbackLedgerGrowthOptions options{.nowMs = now, .retentionMs = PAID_FALLBACK_REQUEST_LOG_RETENTION_MS};
  const PaidFallbackLedgerGrowthView growth = co_await readPaidFallbackLedgerGrowth(isolated, options);
  EXPECT_EQ(growth.scan, std::string("ok"));
  EXPECT(growth.alert);
  EXPECT_EQ(growth.alert_threshold_bytes, threshold);
  EXPECT_EQ(growth.budget_bytes, PAID_FALLBACK_RAW_STORE_BUDGET_BYTES);
  EXPECT_EQ(growth.estimated_retained_rows, 1'000'000.0);
  EXPECT_EQ(growth.estimated_retained_raw_bytes, threshold);
  EXPECT_EQ(growth.avg_row_bytes, std::optional<double>(rt::Math::round(threshold / 1'000'000)));
  EXPECT_EQ(growth.active_days, 1.0);
  EXPECT_EQ(growth.read_units, 2.0);
  const PaidFallbackLedgerProjectionSummary* p90 = windowOf(growth, 90);
  ASSERT(p90 != nullptr);
  EXPECT_EQ(p90->avg_read_units_per_view, std::optional<double>(400));
  EXPECT_EQ(p90->avg_rollup_rows_per_view, std::optional<double>(2'000));
  EXPECT_EQ(growth.leaderboard.size(), std::size_t{1});
  EXPECT(!growth.leaderboard.empty() && growth.leaderboard[0].projection_views == 7);

  const PaidFallbackLedgerGrowthView quiet = co_await readPaidFallbackLedgerGrowth(isolatedKv(), options);
  EXPECT_EQ(quiet.scan, std::string("ok"));
  EXPECT(!quiet.alert);
  EXPECT(!quiet.avg_row_bytes.has_value());
  EXPECT_EQ(quiet.read_units, 1.0);
  bool everyNull = true;
  for (const PaidFallbackLedgerProjectionSummary& entry : quiet.projections)
    everyNull = everyNull && !entry.avg_read_units_per_view.has_value() && !entry.avg_rollup_rows_per_view.has_value();
  EXPECT(everyNull);
}

ASYNC_TEST("projection views record KV read units per 7/30/90-day window") {
  const double now = rt::dateNow();
  const std::shared_ptr<rt::Kv> isolated = isolatedKv();
  const PaidFallbackLedgerProjectionStatsInput first{.windowDays = 30, .readUnits = 120, .rollupRows = 100, .nowMs = now};
  co_await recordPaidFallbackLedgerProjectionStats(isolated, first);
  const PaidFallbackLedgerProjectionStatsInput second{.windowDays = 30, .readUnits = 80, .rollupRows = 60, .nowMs = now};
  co_await recordPaidFallbackLedgerProjectionStats(isolated, second);
  const PaidFallbackLedgerProjectionStatsInput third{.windowDays = 90, .readUnits = 400, .rollupRows = 380, .nowMs = now};
  co_await recordPaidFallbackLedgerProjectionStats(isolated, third);

  const rt::KvEntryMaybe<> stored = co_await isolated->get(paidFallbackLedgerStatsDayKey(paidFallbackLedgerStatsDayStart(now)));
  ASSERT(!stored.value.isNullish());
  EXPECT(isPaidFallbackLedgerDailyStats(stored.value));
  EXPECT(rt::deepEqual(stored.value["projection_30d"].toJson(), rt::jsonParse(R"({"views":2,"read_units":200,"rollup_rows":160})")));

  const PaidFallbackLedgerGrowthOptions options{.nowMs = now, .retentionMs = PAID_FALLBACK_REQUEST_LOG_RETENTION_MS};
  const PaidFallbackLedgerGrowthView growth = co_await readPaidFallbackLedgerGrowth(isolated, options);
  const PaidFallbackLedgerProjectionSummary* byWindow30 = windowOf(growth, 30);
  const PaidFallbackLedgerProjectionSummary* byWindow90 = windowOf(growth, 90);
  const PaidFallbackLedgerProjectionSummary* byWindow7 = windowOf(growth, 7);
  ASSERT(byWindow30 != nullptr && byWindow90 != nullptr && byWindow7 != nullptr);
  EXPECT_EQ(byWindow30->views, 2.0);
  EXPECT_EQ(byWindow30->avg_read_units_per_view, std::optional<double>(100));
  EXPECT_EQ(byWindow30->avg_rollup_rows_per_view, std::optional<double>(80));
  EXPECT_EQ(byWindow90->views, 1.0);
  EXPECT_EQ(byWindow90->avg_read_units_per_view, std::optional<double>(400));
  EXPECT_EQ(byWindow7->views, 0.0);
  EXPECT(!byWindow7->avg_read_units_per_view.has_value());
  EXPECT_EQ(growth.leaderboard.size(), std::size_t{1});
}

ASYNC_TEST("rollup listing respects the requested time range") {
  const rt::env::ScopedOverride metered({{"METERED_API_KEY", std::string("test-metered-key")}});
  const std::shared_ptr<rt::Kv> kv = isolatedKv();
  auto seeding = seedKeyRecord(kv);
  co_await std::move(seeding);
  const double now = rt::dateNow();
  const double hourStart = rt::Math::floor(now / HOUR_MS) * HOUR_MS;
  const rt::KvKey oldBucketKey = rollups::paidFallbackUsageRollupKey(hourStart - 2 * HOUR_MS, "gpt-5.6-sol", "metered", 3);
  const rt::KvKey freshBucketKey = rollups::paidFallbackUsageRollupKey(hourStart, "gpt-5.6-sol", "metered", 3);
  PaidFallbackUsageRollup sample{
    .v = 1,
    .bucket_start_at_ms = 0,
    .model = "gpt-5.6-sol",
    .provider = "metered",
    .request_count = 1,
    .quota_sum = 50,
    .input_tokens = 100,
    .cached_input_tokens = 0,
    .output_tokens = 10,
    .spend_microcredits = 25,
    .first_request_at_ms = 0,
    .last_request_at_ms = 0,
    .updated_at_ms = now,
  };
  sample.bucket_start_at_ms = hourStart - 2 * HOUR_MS;
  auto writingOld = setRow(kv, oldBucketKey, json::value_from(sample));
  co_await std::move(writingOld);
  sample.bucket_start_at_ms = hourStart;
  auto writingFresh = setRow(kv, freshBucketKey, json::value_from(sample));
  co_await std::move(writingFresh);

  auto listing = listRollups(kv, hourStart - HOUR_MS, now + HOUR_MS);
  const std::vector<PaidFallbackUsageRollup> retained = co_await std::move(listing);
  EXPECT_EQ(retained.size(), std::size_t{1});
  EXPECT(!retained.empty() && retained[0].bucket_start_at_ms == hourStart);
}

TEST("merge sums counters and tracks first/last request times") {
  const PaidFallbackUsageRollupInput base{
    .bucket_start_at_ms = 1'000,
    .model = "gpt-5.6-sol",
    .provider = "metered",
    .request_id = "merge-req-1",
    .quota = 30,
    .input_tokens = 10,
    .cached_input_tokens = std::nullopt,
    .output_tokens = 5,
    .spend_microcredits = 15,
    .request_created_at_ms = 5'000,
    .updated_at_ms = 6'000,
  };
  const PaidFallbackUsageRollup first = rollups::mergePaidFallbackUsageRollup(std::nullopt, base);
  EXPECT_EQ(first.request_count, 1.0);
  EXPECT_EQ(first.quota_sum, 30.0);
  EXPECT_EQ(first.first_request_at_ms, 5'000.0);
  EXPECT_EQ(first.last_request_at_ms, 5'000.0);
  PaidFallbackUsageRollupInput next = base;
  next.quota = 20;
  next.input_tokens = 40;
  next.cached_input_tokens = 7;
  next.spend_microcredits = 10;
  next.request_created_at_ms = 4'000;
  next.updated_at_ms = 7'000;
  const PaidFallbackUsageRollup second = rollups::mergePaidFallbackUsageRollup(first, next);
  EXPECT_EQ(second.request_count, 2.0);
  EXPECT_EQ(second.quota_sum, 50.0);
  EXPECT_EQ(second.cached_input_tokens, 7.0);
  EXPECT_EQ(second.first_request_at_ms, 4'000.0);
  EXPECT_EQ(second.last_request_at_ms, 5'000.0);
}

ASYNC_TEST("terminal observations record subscription usage and skip settled providers") {
  const std::shared_ptr<rt::Kv> kv = isolatedKv();
  const double now = rt::dateNow();
  const TerminalUsageRollupInput observation{
    .model = "gpt-5.6-sol",
    .provider = "chatgpt_codex",
    .request_id = "",
    .request_created_at_ms = now,
    .input_tokens = 120,
    .cached_input_tokens = 40,
    .output_tokens = 30,
  };
  const auto with = [&observation](std::string requestId, std::optional<std::string> provider, std::optional<std::string> model) {
    TerminalUsageRollupInput input = observation;
    input.request_id = std::move(requestId);
    input.provider = std::move(provider);
    input.model = std::move(model);
    return input;
  };
  const std::vector<std::pair<TerminalUsageRollupInput, bool>> observations{
    {with("terminal-codex-a", "chatgpt_codex", "gpt-5.6-sol"), true},
    {with("terminal-codex-b", "chatgpt_codex", "gpt-5.6-sol"), true},
    // Settled providers already contribute their authoritative settlement row.
    {with("terminal-metered", "metered", "gpt-5.6-sol"), false},
    {with("terminal-surplus", "surplus", "gpt-5.6-sol"), false},
    // A gateway rejection and an image-fanout aggregate are labels, not routes.
    {with("terminal-gateway", "gateway", "gpt-5.6-sol"), false},
    {with("terminal-mixed", "mixed", "gpt-5.6-sol"), false},
    {with("terminal-model-unknown", "chatgpt_codex", std::nullopt), false},
  };
  for (const auto& [input, expected] : observations) {
    auto recording = recordObservation(input, kv);
    const bool recorded = co_await std::move(recording);
    EXPECT_EQ(recorded, expected);
  }
  // An unavailable KV never throws on the terminal path.
  auto withoutKv = recordObservation(with("terminal-no-kv", "chatgpt_codex", "gpt-5.6-sol"), nullptr);
  const bool recordedWithoutKv = co_await std::move(withoutKv);
  EXPECT_EQ(recordedWithoutKv, false);

  auto listing = listRollups(kv, now - 30 * DAY_MS, now);
  const std::vector<PaidFallbackUsageRollup> rollupRows = co_await std::move(listing);
  std::vector<PaidFallbackUsageRollup> subscription;
  for (const PaidFallbackUsageRollup& entry : rollupRows)
    if (entry.provider == "chatgpt_codex") subscription.push_back(entry);
  EXPECT(subscription.size() >= 1);
  double requests = 0, input = 0, cached = 0, output = 0, quota = 0, spend = 0;
  for (const PaidFallbackUsageRollup& entry : subscription) {
    requests += entry.request_count;
    input += entry.input_tokens;
    cached += entry.cached_input_tokens;
    output += entry.output_tokens;
    quota += entry.quota_sum;
    spend += entry.spend_microcredits;
  }
  EXPECT_EQ(requests, 2.0);
  EXPECT_EQ(input, 240.0);
  EXPECT_EQ(cached, 80.0);
  EXPECT_EQ(output, 60.0);
  EXPECT_EQ(quota, 0.0);
  EXPECT_EQ(spend, 0.0);
  const double bucketStartAtMs = rt::Math::floor(now / HOUR_MS) * HOUR_MS;
  for (const PaidFallbackUsageRollup& entry : subscription) {
    EXPECT_EQ(entry.model, std::string("gpt-5.6-sol"));
    EXPECT_EQ(entry.bucket_start_at_ms, bucketStartAtMs);
  }
}

ASYNC_TEST("terminal observations skip unobserved usage instead of recording zero-token rows") {
  const std::shared_ptr<rt::Kv> kv = isolatedKv();
  const double now = rt::dateNow();
  const TerminalUsageRollupInput unobserved{
    .model = "gpt-5.6-sol",
    .provider = "chatgpt_codex",
    .request_id = "terminal-unobserved",
    .request_created_at_ms = now,
    .input_tokens = std::nullopt,
    .cached_input_tokens = std::nullopt,
    .output_tokens = std::nullopt,
  };
  auto recordingUnobserved = recordObservation(unobserved, kv);
  const bool recordedUnobserved = co_await std::move(recordingUnobserved);
  EXPECT_EQ(recordedUnobserved, false);
  // A partial observation is missing a core dimension, so it is not recorded
  // as a zero either.
  TerminalUsageRollupInput partial = unobserved;
  partial.request_id = "terminal-partial";
  partial.input_tokens = 10;
  auto recordingPartial = recordObservation(partial, kv);
  const bool recordedPartial = co_await std::move(recordingPartial);
  EXPECT_EQ(recordedPartial, false);
  auto listing = listRollups(kv, now - DAY_MS, now);
  const std::vector<PaidFallbackUsageRollup> rollupRows = co_await std::move(listing);
  EXPECT_EQ(rollupRows.size(), std::size_t{0});
}

ASYNC_TEST("terminal observations retry a conflicting shard write instead of dropping it") {
  const auto conflicting = std::make_shared<ConflictOnceKv>(isolatedKv());
  const double now = rt::dateNow();
  conflicting->atomicConflictOnce = true;
  const TerminalUsageRollupInput input{
    .model = "gpt-5.6-luna",
    .provider = "cerebras",
    .request_id = "terminal-cas-retry",
    .request_created_at_ms = now,
    .input_tokens = 11,
    .cached_input_tokens = std::nullopt,
    .output_tokens = 3,
  };
  auto recording = recordObservation(input, conflicting);
  const bool recorded = co_await std::move(recording);
  EXPECT_EQ(recorded, true);
  auto listing = listRollups(conflicting, now - DAY_MS, now);
  const std::vector<PaidFallbackUsageRollup> rollupRows = co_await std::move(listing);
  double recordedCount = 0;
  for (const PaidFallbackUsageRollup& entry : rollupRows)
    if (entry.provider == "cerebras") recordedCount += entry.request_count;
  EXPECT_EQ(recordedCount, 1.0);
}
namespace {
  void replayProjectionCase(std::string_view name) {
    namespace projection = uos::quota_projection;
    const auto vectors = rt::jsonParse(
#include "quota_projection_vectors.inc"
    );
    bool found = false;
    for (const auto& test : vectors.as_object().at("cases").as_array()) {
      if (rt::getString(test, "name") != name) continue;
      found = true;
      for (const auto& call : test.as_object().at("calls").as_array()) {
        const auto& args = call.as_object().at("args").as_array(); const auto function = *rt::getString(call, "name"); json::value actual;
        if (function == "meteredQuotaRunwayView") actual = json::value_from(projection::meteredQuotaRunwayView(args[0].is_null() ? std::nullopt : std::optional<uos::metered_quota::MeteredQuotaSnapshot>(json::value_to<uos::metered_quota::MeteredQuotaSnapshot>(args[0]))));
        else if (function == "groupPaidFallbackUsageRollups") actual = json::value_from(projection::groupPaidFallbackUsageRollups(json::value_to<std::vector<PaidFallbackUsageRollup>>(args[0])));
        else if (function == "summarizePaidFallbackUsage") actual = json::value_from(projection::summarizePaidFallbackUsage(json::value_to<std::vector<projection::PaidFallbackModelSeries>>(args[0]), rt::toNumber(args[1])));
        else if (function == "projectPaidFallbackRunway") {
          const auto usage = json::value_to<projection::PaidFallbackModelUsage>(args[0]); const auto quota = json::value_to<projection::MeteredQuotaRunwayView>(args[1]); const double now = rt::toNumber(args[2]);
          if (args.size() == 3) actual = json::value_from(projection::projectPaidFallbackRunway(usage, quota, now));
          else { const auto history = args[3].is_null() ? std::nullopt : std::optional<std::vector<uos::metered_quota::MeteredQuotaBalanceSample>>(json::value_to<std::vector<uos::metered_quota::MeteredQuotaBalanceSample>>(args[3])); actual = json::value_from(projection::projectPaidFallbackRunway(usage, quota, now, history)); }
        } else throw rt::JsError("unknown quota oracle call");
        const auto& expected = call.as_object().at("output");
        EXPECT(rt::deepEqual(actual, expected));
      }
    }
    EXPECT(found);
  }
}
TEST("summarize and project offer per-window rates and exhaustion estimates") { replayProjectionCase("summarize and project offer per-window rates and exhaustion estimates"); }
TEST("token-usage and unlimited quota modes do not fabricate estimates") { replayProjectionCase("token-usage and unlimited quota modes do not fabricate estimates"); }
TEST("refill projection distinguishes sustainable, early depletion and scheduled exhaustion") { replayProjectionCase("refill projection distinguishes sustainable, early depletion and scheduled exhaustion"); }
TEST("refill projection uses inferred credits rather than net balance growth") { replayProjectionCase("refill projection uses inferred credits rather than net balance growth"); }
TEST("refill projection prefers a newer inferred credit to retained refill metadata") { replayProjectionCase("refill projection prefers a newer inferred credit to retained refill metadata"); }
TEST("refill projection does not apply retained wallet history to finite token inventory") { replayProjectionCase("refill projection does not apply retained wallet history to finite token inventory"); }
TEST("refill projection stays unknown when a later refresh no longer retains the inferred amount") { replayProjectionCase("refill projection stays unknown when a later refresh no longer retains the inferred amount"); }
ASYNC_TEST("subscription observations join the projection without runway estimates") {
  const auto store = isolatedKv(); const double now = rt::dateNow();
  const TerminalUsageRollupInput input{.model = "gpt-5.6-sol", .provider = "chatgpt_codex", .request_id = "terminal-projection", .request_created_at_ms = now, .input_tokens = 50, .cached_input_tokens = std::nullopt, .output_tokens = 20};
  auto recording = rollups::recordTerminalUsageRollup(input, store); co_await std::move(recording);
  auto listing = listRollups(store, now - DAY_MS, now); const auto rows = co_await std::move(listing);
  const auto usage = uos::quota_projection::summarizePaidFallbackUsage(uos::quota_projection::groupPaidFallbackUsageRollups(rows), now); const uos::quota_projection::PaidFallbackModelUsage* subscription = nullptr; for (const auto& model : usage) if (model.provider == "chatgpt_codex") subscription = &model; ASSERT(subscription);
  const uos::quota_projection::PaidFallbackModelUsageWindow* window = nullptr; for (const auto& item : subscription->windows) if (item.window_days == 30) window = &item; ASSERT(window); EXPECT_EQ(window->request_count, 1); EXPECT_EQ(window->input_tokens, 50); EXPECT_EQ(window->quota_sum, 0); EXPECT(uos::quota_projection::projectPaidFallbackRunway(*subscription, uos::quota_projection::meteredQuotaRunwayView(std::nullopt), now).empty());
}
namespace {
  namespace metered = uos::metered_quota;
  namespace paid = uos::paid_fallback::index;
  namespace backfill = uos::paid_fallback::ledger_backfill;
  namespace ledger = uos::paid_fallback::ledger_state;
  metered::MeteredQuotaBalanceSample sample(double bucket, double observed, double balance = 1'000) { return {.bucket_start_at_ms = bucket, .observed_at_ms = observed, .balance_quota = balance, .baseline_quota = 2'000, .quota_per_credit = 1, .remaining_percent = 50}; }
  struct QuotaScope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c7-quota"}, {"METERED_API_KEY", "test-metered-key"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    explicit QuotaScope(const std::shared_ptr<rt::Kv>& store) { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(store); paid::setPaidFallbackTerminalSweepForTest([] {}); }
    ~QuotaScope() { uos::kv::setKvForTest(nullptr); paid::setPaidFallbackTerminalSweepForTest({}); uos::config::config.codexAuthJsonB64 = saved; }
  };
  rt::Task<void> settleBackground() { const auto scheduled = backfill::scheduledPaidFallbackBackfillForTest(); if (scheduled) co_await scheduled->wait(); }
  rt::Task<paid::PaidFallbackReservation> reserve(std::string request, double at) {
    std::string stage = "constructing admission";
    try {
      const paid::ReservePaidFallbackInput input{.keyId = keyId, .requestId = request, .createdAtMs = at, .model = "gpt-5.6-sol", .route = "responses", .path = "/v1/responses", .stream = true, .reasoning = "high", .reason = "primary_429"};
      stage = "starting admission";
      rt::Promise pending(paid::reservePaidFallback(input)); stage = "awaiting admission";
      const auto result = co_await pending.wait(); stage = "reading admission";
      const auto* reserved = std::get_if<paid::PaidFallbackReservationDecisionReserved>(&result);
      if (!reserved) throw rt::JsError("quota fixture reservation rejected");
      co_return reserved->reservation;
    } catch (const std::exception& error) { throw rt::JsError("quota fixture " + stage + ": " + error.what()); }
  }
  rt::Task<void> settleSurplus(paid::PaidFallbackReservation reservation) {
    std::string stage = "dispatch";
    try {
      rt::Promise dispatched(paid::recordMeteredUpstreamResponse(reservation, rt::Response(), std::nullopt, "surplus")); co_await dispatched.wait();
      stage = "terminal";
      rt::Promise terminal(paid::recordMeteredTerminal(reservation, "completed", "surplus")); co_await terminal.wait();
      stage = "usage";
      const paid::SurplusUsage usage{.input_tokens = 100, .cached_input_tokens = 20, .cache_write_input_tokens = 30, .output_tokens = 10};
      const paid::SurplusBillingPricing pricing{.input_price_per_token = 0.000001, .output_price_per_token = 0.000003, .cache_read_price_per_token = 0.0000001, .cache_write_price_per_token = 0.000002};
      rt::Promise settling(paid::recordSurplusUsage(reservation, "surplus:" + reservation.request_id, "gpt-5.6-sol", usage, pricing)); co_await settling.wait();
    } catch (const std::exception& error) { throw rt::JsError("quota fixture settlement " + stage + ": " + error.what()); }
  }
  json::object settledRequestRow(const std::string& request, double at) {
    auto row = rt::jsonParse(R"({"v":3,"key_id":"quota-projection-key","request_id":"","policy_version":"60000:123","route":"responses","path":"/v1/responses","model":"gpt-5.6-sol","stream":true,"reasoning":"high","window_reset_at_ms":0,"reserved_microcredits":300,"quota_per_credit":1,"provider":"metered","provider_request_id":null,"provider_quota":60,"input_tokens":90,"cached_input_tokens":null,"output_tokens":10,"dispatch_state":"dispatched","terminal_state":"completed","spend_microcredits":60,"billing_state":"settled","reconciliation_attempts":1,"last_reconciliation_at_ms":0,"dispatched_at_ms":0,"terminal_at_ms":0,"settled_at_ms":0,"created_at_ms":0,"updated_at_ms":0})").as_object();
    row["request_id"] = request; row["window_reset_at_ms"] = rt::makeNumber(at + 60'000); row["last_reconciliation_at_ms"] = rt::makeNumber(at + 1'000); row["dispatched_at_ms"] = rt::makeNumber(at + 500); row["terminal_at_ms"] = rt::makeNumber(at + 1'000); row["settled_at_ms"] = rt::makeNumber(at + 1'000); row["created_at_ms"] = rt::makeNumber(at); row["updated_at_ms"] = rt::makeNumber(at + 1'000); return row;
  }
}
#define RUN_QUOTA(expression) do { auto runningTask = (expression); co_await std::move(runningTask); } while (false)
#define AWAIT_QUOTA(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
ASYNC_TEST("balance history listing uses a valid bounded KV range selector") {
  constexpr double hour = 1'800'000'000'000;
  constexpr double observedAt = hour + 1'000;
  const auto store = std::make_shared<rt::MemoryKv>([] { return observedAt; });
  const std::string fingerprint = "quota-projection-account";
  const std::vector<metered::MeteredQuotaBalanceSample> samples{
    sample(hour - 2 * HOUR_MS, hour - 2 * HOUR_MS + 1'000),
    sample(hour - HOUR_MS, hour - HOUR_MS + 45 * 60'000),
    sample(hour, observedAt),
  };
  for (const auto& value : samples) {
    const rt::KvValue row(json::value_from(value));
    co_await store->set(rt::KvKey{metered::METERED_QUOTA_BALANCE_HISTORY_PREFIX, fingerprint, value.bucket_start_at_ms}, row);
  }
  metered::ReadMeteredQuotaBalanceHistoryOptions options{
    .sinceMs = hour - HOUR_MS + 30 * 60'000, .nowMs = observedAt - 1, .accountFingerprint = fingerprint,
  };
  AWAIT_QUOTA(earlierRows, metered::readMeteredQuotaBalanceHistory(store, options));
  ASSERT_EQ(earlierRows.size(), 1u);
  EXPECT_EQ(earlierRows[0].bucket_start_at_ms, hour - HOUR_MS);

  options.nowMs = observedAt;
  AWAIT_QUOTA(rows, metered::readMeteredQuotaBalanceHistory(store, options));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].bucket_start_at_ms, hour - HOUR_MS);
  EXPECT_EQ(rows[1].bucket_start_at_ms, hour);
}
TEST("365-day balance history is deterministically resampled to UTC days") {
  const double day = rt::Math::floor(rt::dateNow() / DAY_MS) * DAY_MS; const std::vector<metered::MeteredQuotaBalanceSample> source{sample(day - DAY_MS, day - DAY_MS + HOUR_MS, 1'200), sample(day + HOUR_MS, day + HOUR_MS + 1, 1'100), sample(day + 2 * HOUR_MS, day + 2 * HOUR_MS + 1, 1'000)};
  const auto rows = metered::resampleMeteredQuotaBalanceHistory(source, metered::METERED_QUOTA_BALANCE_HISTORY_DAILY_BUCKET_MS, 365); ASSERT_EQ(rows.size(), 2u); EXPECT_EQ(rows[0].bucket_start_at_ms, day - DAY_MS); EXPECT_EQ(rows[1].bucket_start_at_ms, day); EXPECT_EQ(rows[1].balance_quota, 1'000);
  const double start = 30 * 60'000, end = start + 365 * DAY_MS; std::vector<metered::MeteredQuotaBalanceSample> oversized; for (int i = 0; i < 365 * 24 + 1; ++i) oversized.push_back(sample(start + i * HOUR_MS, start + i * HOUR_MS, i)); const auto capped = metered::resampleMeteredQuotaBalanceHistory(oversized, 25 * HOUR_MS, 365); EXPECT(capped.size() <= 365); ASSERT(!capped.empty()); EXPECT_EQ(capped.front().observed_at_ms, start + 24 * HOUR_MS); EXPECT_EQ(capped.back().observed_at_ms, end);
}
TEST("balance window accepts supported values and defaults invalid input to seven days") {
  EXPECT_EQ(metered::normalizeMeteredQuotaBalanceWindowDays(std::nullopt), 7); for (const std::string value : {"", "invalid", "0", "8", "7.5", "7days"}) EXPECT_EQ(metered::normalizeMeteredQuotaBalanceWindowDays(value), 7); for (const double value : {7, 30, 90, 365}) EXPECT_EQ(metered::normalizeMeteredQuotaBalanceWindowDays(rt::numberToString(value)), value);
}
ASYNC_TEST("settlement writes an hourly rollup and retains the raw row for one year") {
  const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow); QuotaScope scope(store); co_await seedKeyRecord(store); const double now = rt::dateNow(); AWAIT_QUOTA(reservation, reserve("qp-settled-a", now)); RUN_QUOTA(settleSurplus(reservation)); AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); ASSERT_EQ(rows.size(), 1u); const auto& row = rows[0]; EXPECT_EQ(row.model, std::string("gpt-5.6-sol")); EXPECT_EQ(row.provider, std::string("surplus")); EXPECT_EQ(row.request_count, 1); EXPECT_EQ(row.quota_sum, 86); EXPECT_EQ(row.spend_microcredits, 172); EXPECT_EQ(row.input_tokens, 100); EXPECT_EQ(row.cached_input_tokens, 20); EXPECT_EQ(row.output_tokens, 10); EXPECT_EQ(row.bucket_start_at_ms, rt::Math::floor(reservation.created_at_ms / HOUR_MS) * HOUR_MS);
  const auto raw = store->database()->row(ledger::paidFallbackRequestV3Key(keyId, reservation.request_id)); ASSERT(raw); EXPECT(raw->expirationMs > 0); EXPECT(rt::Math::abs(raw->expirationMs - (reservation.created_at_ms + PAID_FALLBACK_REQUEST_LOG_RETENTION_MS)) < 5'000); RUN_QUOTA(settleSurplus(reservation)); AWAIT_QUOTA(after, listRollups(store, now - 30 * DAY_MS, now)); ASSERT_EQ(after.size(), 1u); EXPECT_EQ(after[0].request_count, 1); EXPECT_EQ(after[0].quota_sum, 86); co_await settleBackground();
}
ASYNC_TEST("two requests in one hour bucket accumulate across rollup shards") {
  const auto store = isolatedKv(); QuotaScope scope(store); co_await seedKeyRecord(store); const double now = rt::dateNow(), bucket = rt::Math::floor(now / HOUR_MS) * HOUR_MS; AWAIT_QUOTA(first, reserve("qp-merge-a", bucket + 1'000)); AWAIT_QUOTA(second, reserve("qp-merge-b", bucket + 2'000)); RUN_QUOTA(settleSurplus(first)); RUN_QUOTA(settleSurplus(second)); AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); double requests = 0, quota = 0, spend = 0; for (const auto& row : rows) { requests += row.request_count; quota += row.quota_sum; spend += row.spend_microcredits; } EXPECT_EQ(requests, 2); EXPECT_EQ(quota, 172); EXPECT_EQ(spend, 344); co_await settleBackground();
}
ASYNC_TEST("backfill folds pre-existing settled rows into rollups and applies the TTL once") {
  const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow); co_await seedKeyRecord(store); const double now = rt::dateNow(); const auto key = ledger::paidFallbackRequestV3Key(keyId, "bf-1"); auto writing = setRow(store, key, settledRequestRow("bf-1", now - 2 * HOUR_MS)); co_await std::move(writing);
  const backfill::BackfillPaidFallbackUsageRollupsOptions options{.nowMs = now}; AWAIT_QUOTA(first, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT_EQ(first.scanned, 1); EXPECT_EQ(first.processed, 1); EXPECT_EQ(first.rollups_written, 1); EXPECT(!first.truncated); AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); ASSERT_EQ(rows.size(), 1u); EXPECT_EQ(rows[0].request_count, 1); EXPECT_EQ(rows[0].quota_sum, 60);
  AWAIT_QUOTA(row, store->get(key)); EXPECT_EQ(rt::getNumber(row.value.toJson(), "usage_rollup_at_ms"), std::optional<double>(now)); const auto raw = store->database()->row(key); ASSERT(raw); EXPECT(raw->expirationMs > 0); AWAIT_QUOTA(second, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT_EQ(second.processed, 0); AWAIT_QUOTA(after, listRollups(store, now - 30 * DAY_MS, now)); ASSERT_EQ(after.size(), 1u); EXPECT_EQ(after[0].request_count, 1); EXPECT_EQ(after[0].quota_sum, 60);
}
ASYNC_TEST("settlement marks the row so a later backfill never double counts") {
  const auto store = isolatedKv(); QuotaScope scope(store); co_await seedKeyRecord(store); const double now = rt::dateNow(); AWAIT_QUOTA(reservation, reserve("qp-live-then-backfill", now)); RUN_QUOTA(settleSurplus(reservation)); AWAIT_QUOTA(row, store->get(ledger::paidFallbackRequestV3Key(keyId, reservation.request_id))); EXPECT_EQ(rt::getNumber(row.value.toJson(), "usage_rollup_at_ms"), rt::getNumber(row.value.toJson(), "settled_at_ms"));
  const backfill::BackfillPaidFallbackUsageRollupsOptions options{.nowMs = now}; AWAIT_QUOTA(result, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT_EQ(result.processed, 0); EXPECT_EQ(result.rollups_written, 0); AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); ASSERT_EQ(rows.size(), 1u); EXPECT_EQ(rows[0].request_count, 1); EXPECT_EQ(rows[0].quota_sum, 86); co_await settleBackground();
}
ASYNC_TEST("backfill is resumable with a limit and marks non-billable rows") {
  const auto store = isolatedKv(); co_await seedKeyRecord(store); const double now = rt::dateNow(); auto firstWriting = setRow(store, ledger::paidFallbackRequestV3Key(keyId, "bf-a"), settledRequestRow("bf-a", now - 2 * HOUR_MS)); co_await std::move(firstWriting); auto pending = settledRequestRow("bf-b", now - HOUR_MS); pending["billing_state"] = "pending"; pending["spend_microcredits"] = nullptr; pending["provider_quota"] = nullptr; auto secondWriting = setRow(store, ledger::paidFallbackRequestV3Key(keyId, "bf-b"), pending); co_await std::move(secondWriting);
  const backfill::BackfillPaidFallbackUsageRollupsOptions options{.limit = 1, .nowMs = now}; AWAIT_QUOTA(partial, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT_EQ(partial.processed, 1); EXPECT_EQ(partial.rollups_written, 1); EXPECT(partial.truncated); AWAIT_QUOTA(cursor, store->get(ledger::paidFallbackBackfillCursorV3Key())); EXPECT(!cursor.value.isNull());
  AWAIT_QUOTA(remainder, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT(!remainder.truncated); EXPECT_EQ(remainder.rollups_written, 0); EXPECT_EQ(remainder.processed, 1); AWAIT_QUOTA(cleared, store->get(ledger::paidFallbackBackfillCursorV3Key())); EXPECT(cleared.value.isNull()); AWAIT_QUOTA(complete, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT(!complete.truncated); EXPECT_EQ(complete.processed, 0);
  AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); ASSERT_EQ(rows.size(), 1u); EXPECT_EQ(rows[0].request_count, 1); EXPECT_EQ(rows[0].quota_sum, 60);
}
ASYNC_TEST("backfill scan budget trips before the deadline and keeps forward progress") {
  const auto store = isolatedKv(); co_await seedKeyRecord(store); const double now = rt::dateNow();
  for (int i = 0; i < 10; ++i) { const std::string request = "bf-m" + std::to_string(i); auto marked = settledRequestRow(request, now - 30 * HOUR_MS); marked["usage_rollup_at_ms"] = rt::makeNumber(now); auto writing = setRow(store, ledger::paidFallbackRequestV3Key(keyId, request), marked); co_await std::move(writing); }
  auto writing = setRow(store, ledger::paidFallbackRequestV3Key(keyId, "bf-z"), settledRequestRow("bf-z", now - 2 * HOUR_MS)); co_await std::move(writing); const backfill::BackfillPaidFallbackUsageRollupsOptions options{.limit = 1, .nowMs = now}; AWAIT_QUOTA(first, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT(first.truncated); EXPECT_EQ(first.processed, 0); AWAIT_QUOTA(cursor, store->get(ledger::paidFallbackBackfillCursorV3Key())); EXPECT(!cursor.value.isNull());
  AWAIT_QUOTA(second, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT(!second.truncated); EXPECT_EQ(second.processed, 1); EXPECT_EQ(second.rollups_written, 1); AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); double requests = 0; for (const auto& row : rows) requests += row.request_count; EXPECT_EQ(requests, 1);
}
ASYNC_TEST("window TTL backfill is resumable across batches") {
  const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow); co_await seedKeyRecord(store); const double now = rt::dateNow(); const std::vector<double> windows{now - 2 * DAY_MS, now + DAY_MS};
  for (const double at : windows) { const json::object value{{"v", 3}, {"key_id", keyId}, {"policy_version", "60000:123"}, {"window_reset_at_ms", rt::makeNumber(at)}, {"limit_microcredits", 5'000'000}, {"settled_microcredits", 100}, {"reserved_microcredits", 0}, {"pending_count", 0}, {"updated_at_ms", rt::makeNumber(now)}}; auto writing = setRow(store, ledger::paidFallbackWindowV3Key(keyId, at), value); co_await std::move(writing); }
  const backfill::BackfillPaidFallbackWindowTtlsOptions options{.limit = 1, .nowMs = now}; AWAIT_QUOTA(partial, backfill::backfillPaidFallbackWindowTtls(store, options)); EXPECT_EQ(partial.rewritten, 1); EXPECT(partial.truncated); AWAIT_QUOTA(cursor, store->get(ledger::paidFallbackBackfillWindowCursorV3Key())); EXPECT(!cursor.value.isNull());
  AWAIT_QUOTA(remainder, backfill::backfillPaidFallbackWindowTtls(store, options)); EXPECT_EQ(remainder.rewritten, 1); EXPECT(!remainder.truncated); AWAIT_QUOTA(cleared, store->get(ledger::paidFallbackBackfillWindowCursorV3Key())); EXPECT(cleared.value.isNull()); for (const double at : windows) { const auto raw = store->database()->row(ledger::paidFallbackWindowV3Key(keyId, at)); ASSERT(raw); EXPECT(raw->expirationMs > 0); }
}
ASYNC_TEST("backfill retries a row whose shard CAS failed instead of advancing past it") {
  const auto store = std::make_shared<ConflictOnceKv>(isolatedKv()); co_await seedKeyRecord(store); const double now = rt::dateNow(); auto writing = setRow(store, ledger::paidFallbackRequestV3Key(keyId, "bf-x"), settledRequestRow("bf-x", now - 2 * HOUR_MS)); co_await std::move(writing); store->atomicConflictOnce = true; const backfill::BackfillPaidFallbackUsageRollupsOptions options{.nowMs = now}; AWAIT_QUOTA(first, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT_EQ(first.failed, 1); EXPECT_EQ(first.processed, 0); EXPECT(first.truncated);
  AWAIT_QUOTA(second, backfill::backfillPaidFallbackUsageRollups(store, options)); EXPECT_EQ(second.failed, 0); EXPECT_EQ(second.processed, 1); EXPECT_EQ(second.rollups_written, 1); EXPECT(!second.truncated); AWAIT_QUOTA(rows, listRollups(store, now - 30 * DAY_MS, now)); double requests = 0, quota = 0; for (const auto& row : rows) { requests += row.request_count; quota += row.quota_sum; } EXPECT_EQ(requests, 1); EXPECT_EQ(quota, 60);
}
ASYNC_TEST("settlement records measured ledger bytes in the daily growth counters") {
  const auto store = isolatedKv(); QuotaScope scope(store); co_await seedKeyRecord(store); const double now = rt::dateNow(); const auto dayKey = paidFallbackLedgerStatsDayKey(paidFallbackLedgerStatsDayStart(now)); AWAIT_QUOTA(before, store->get(dayKey)); AWAIT_QUOTA(reservation, reserve("qp-growth-settled", now)); RUN_QUOTA(settleSurplus(reservation)); AWAIT_QUOTA(after, store->get(dayKey)); EXPECT(isPaidFallbackLedgerDailyStats(after.value)); const auto beforeValue = before.value.isNull() ? json::value(json::object{}) : before.value.toJson(); const auto afterValue = after.value.toJson();
  const double rows = *rt::getNumber(afterValue, "settled_rows") - rt::getNumber(beforeValue, "settled_rows").value_or(0), bytes = *rt::getNumber(afterValue, "settled_row_bytes") - rt::getNumber(beforeValue, "settled_row_bytes").value_or(0), writes = *rt::getNumber(afterValue, "rollup_writes") - rt::getNumber(beforeValue, "rollup_writes").value_or(0), rollupBytes = *rt::getNumber(afterValue, "rollup_bytes") - rt::getNumber(beforeValue, "rollup_bytes").value_or(0); EXPECT_EQ(rows, 1); EXPECT_EQ(writes, 1); EXPECT(bytes > 400 && bytes < 2'000); EXPECT(rollupBytes > 0); AWAIT_QUOTA(row, store->get(ledger::paidFallbackRequestV3Key(keyId, reservation.request_id))); EXPECT_EQ(bytes, estimatePaidFallbackRecordBytes(row.value.toJson()));
  RUN_QUOTA(settleSurplus(reservation)); AWAIT_QUOTA(replayed, store->get(dayKey)); EXPECT_EQ(rt::getNumber(replayed.value.toJson(), "settled_rows"), rt::getNumber(afterValue, "settled_rows")); EXPECT_EQ(rt::getNumber(replayed.value.toJson(), "settled_row_bytes"), rt::getNumber(afterValue, "settled_row_bytes")); co_await settleBackground();
}

ASYNC_TEST("admin projection tags settled rows paid_fallback and terminal accounting observability") {
  const auto store = isolatedKv(); QuotaScope scope(store);
  rt::GlobalFetchOverride offline{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { throw rt::JsError("offline projection test"); co_return rt::Response(); })};
  const double now = rt::dateNow(), bucket = rt::Math::floor(now / HOUR_MS) * HOUR_MS;
  const PaidFallbackUsageRollup settled{.v = 1, .bucket_start_at_ms = bucket, .model = "gpt-5.6-sol", .provider = "metered", .request_count = 1, .quota_sum = 50, .input_tokens = 100, .cached_input_tokens = 0, .output_tokens = 10, .spend_microcredits = 25, .first_request_at_ms = now, .last_request_at_ms = now, .updated_at_ms = now};
  const rt::KvValue settledValue(json::value_from(settled)); co_await store->set(rollups::paidFallbackUsageRollupKey(bucket, "gpt-5.6-sol", "metered", 0), settledValue);
  const TerminalUsageRollupInput input{.model = "gpt-5.6-sol", .provider = "chatgpt_codex", .request_id = "terminal-usage-source", .request_created_at_ms = now, .input_tokens = 40, .cached_input_tokens = std::nullopt, .output_tokens = 5};
  AWAIT_QUOTA(recorded, recordObservation(input, store)); EXPECT(recorded);
  AWAIT_QUOTA(response, uos::admin::kernel::handleAdminProvidersQuotaProjection(rt::Request("https://ai.ubq.fi/admin/providers/quota-projection?window_days=30"))); EXPECT_EQ(response.status(), 200);
  AWAIT_QUOTA(payload, response.json()); const auto* models = rt::getArray(payload, "models"); ASSERT(models); std::optional<std::string> meteredSource, codexSource;
  for (const auto& row : *models) { if (rt::getString(row, "provider") == std::optional<std::string_view>("metered")) meteredSource = std::string(*rt::getString(row, "usage_source")); if (rt::getString(row, "provider") == std::optional<std::string_view>("chatgpt_codex")) codexSource = std::string(*rt::getString(row, "usage_source")); }
  EXPECT_EQ(meteredSource, std::optional<std::string>("paid_fallback")); EXPECT_EQ(codexSource, std::optional<std::string>("observability"));
}

ASYNC_TEST("admin quota projection returns the inferred-credit refill forecast") {
  const auto store = isolatedKv(); QuotaScope scope(store);
  constexpr double start = 1'800'000'000'000, now = start + 16 * HOUR_MS;
  rt::globalDateNow() = [] { return now; };
  rt::GlobalFetchOverride offline{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { throw rt::JsError("offline refill projection test"); co_return rt::Response(); })};
  std::optional<metered::MeteredQuotaState> state; std::vector<metered::MeteredQuotaBalanceSample> history;
  const std::vector<std::array<double, 3>> observations{{0, 200, 0}, {8, 230, 10}, {16, 260, 20}};
  for (const auto& point : observations) {
    const metered::MeteredQuotaObservation observation{.balance_quota = point[1], .used_quota = point[2], .quota_per_credit = 1, .observed_at_ms = start + point[0] * HOUR_MS, .latest_refill = point[0] == 0 ? std::optional<metered::MeteredRefillObservation>({.id = "old-refill", .amount_credits = 100, .completed_at_ms = start}) : std::nullopt};
    state = metered::updateMeteredQuotaState(state, observation);
    history.push_back({.v = 1, .bucket_start_at_ms = rt::Math::floor(state->observed_at_ms / HOUR_MS) * HOUR_MS, .observed_at_ms = state->observed_at_ms, .balance_quota = state->current_balance_quota, .baseline_quota = state->post_refill_baseline_quota, .quota_per_credit = state->quota_per_credit, .remaining_percent = std::nullopt});
  }
  co_await seedKeyRecord(store); const rt::KvValue stateValue(json::value_from(*state)); co_await store->set(metered::METERED_QUOTA_STATE_KEY, stateValue);
  const metered::MeteredAccountCredentials credentials{.apiKey = "test-metered-key"}; AWAIT_QUOTA(fingerprint, metered::meterQuotaAccountFingerprint(credentials)); ASSERT(fingerprint);
  for (const auto& sampleValue : history) { const rt::KvValue sampleRow(json::value_from(sampleValue)); const rt::KvKey key{metered::METERED_QUOTA_BALANCE_HISTORY_PREFIX, *fingerprint, sampleValue.bucket_start_at_ms}; co_await store->set(key, sampleRow); }
  const PaidFallbackUsageRollup rollup{.v = 1, .bucket_start_at_ms = now - HOUR_MS, .model = "gpt-5.6-sol", .provider = "metered", .request_count = 168, .quota_sum = 1680, .input_tokens = 100, .cached_input_tokens = 0, .output_tokens = 10, .spend_microcredits = 0, .first_request_at_ms = now - HOUR_MS, .last_request_at_ms = now - 1, .updated_at_ms = now - 1};
  const rt::KvValue rollupValue(json::value_from(rollup)); co_await store->set(rollups::paidFallbackUsageRollupKey(rollup.bucket_start_at_ms, rollup.model, rollup.provider, 0), rollupValue);
  AWAIT_QUOTA(response, uos::admin::kernel::handleAdminProvidersQuotaProjection(rt::Request("https://ai.ubq.fi/admin/providers/quota-projection?window_days=7"))); EXPECT_EQ(response.status(), 200); AWAIT_QUOTA(payload, response.json()); const auto* quota = rt::getObject(payload, "quota"); ASSERT(quota); EXPECT(!rt::hasOwn(*quota, "last_inferred_credit_quota")); const auto* models = rt::getArray(payload, "models"); ASSERT(models); const json::value* estimate = nullptr;
  for (const auto& row : *models) if (rt::getString(row, "provider") == std::optional<std::string_view>("metered")) { const auto* estimates = rt::getArray(row, "estimates"); if (estimates && !estimates->empty()) estimate = &estimates->front(); }
  ASSERT(estimate); EXPECT_EQ(rt::getNumber(*estimate, "refill_amount_credits"), std::optional(40.0)); EXPECT_EQ(rt::getNumber(*estimate, "refill_rate_quota_per_hour"), std::optional(5.0)); EXPECT_EQ(rt::getString(*estimate, "refill_aware_status"), std::optional<std::string_view>("exhausts_with_refill")); EXPECT_EQ(rt::getNumber(*estimate, "refill_aware_exhausted_at_ms"), std::optional(now + 46 * HOUR_MS)); EXPECT_EQ(rt::getNumber(*estimate, "time_remaining_ms"), std::optional(26 * HOUR_MS));
}

ASYNC_TEST("admin quota projection records and exposes the measured ledger growth") {
  const auto store = isolatedKv(); QuotaScope scope(store); co_await seedKeyRecord(store);
  rt::GlobalFetchOverride offline{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { throw rt::JsError("offline projection test"); co_return rt::Response(); })};
  const auto dayKey = paidFallbackLedgerStatsDayKey(paidFallbackLedgerStatsDayStart(rt::dateNow())); AWAIT_QUOTA(before, store->get(dayKey)); AWAIT_QUOTA(response, uos::admin::kernel::handleAdminProvidersQuotaProjection(rt::Request("https://ai.ubq.fi/admin/providers/quota-projection?window_days=30"))); EXPECT_EQ(response.status(), 200); AWAIT_QUOTA(payload, response.json()); const auto* growth = rt::getObject(payload, "ledger_growth"); ASSERT(growth); EXPECT_EQ(rt::getString(*growth, "scan"), std::optional<std::string_view>("ok")); const auto* projections = rt::getArray(*growth, "projections"); ASSERT(projections); json::array windows; for (const auto& entry : *projections) windows.push_back(*rt::get(entry, "window_days")); EXPECT(rt::deepEqual(windows, json::array{7, 30, 90}));
  AWAIT_QUOTA(after, store->get(dayKey)); EXPECT(!after.value.isNull()); const auto beforeValue = before.value.isNull() ? json::value(json::object()) : before.value.toJson(); const auto afterValue = after.value.toJson(); auto counter = [](const json::value& row, std::string_view name, std::string_view key) { const auto* window = rt::get(row, name); return window ? rt::getNumber(*window, key).value_or(0) : 0; };
  EXPECT_EQ(counter(afterValue, "projection_30d", "views") - counter(beforeValue, "projection_30d", "views"), 1); EXPECT(counter(afterValue, "projection_30d", "read_units") - counter(beforeValue, "projection_30d", "read_units") >= 4); EXPECT_EQ(counter(afterValue, "projection_7d", "views"), counter(beforeValue, "projection_7d", "views")); EXPECT_EQ(counter(afterValue, "projection_90d", "views"), counter(beforeValue, "projection_90d", "views"));
}
