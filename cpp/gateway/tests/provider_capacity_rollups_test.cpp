// Links: tests/provider_capacity_rollups_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/admin/api_key_audit.cpp src/admin/api_key_mutations.cpp src/admin/api_keys.cpp src/admin/capacity_observation_sync.cpp src/admin/chart_source_proxy.cpp src/admin/claude_code.cpp src/admin/codex.cpp src/admin/codex_reset.cpp src/admin/defaults.cpp src/admin/error_log.cpp
// src/admin/kernel.cpp src/admin/sentinel_incident.cpp src/agent_messages.cpp src/analytics.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/index.cpp src/auth/local_admin.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp
// src/auth/passport.cpp src/auth/passport_sessions.cpp src/auth/relay.cpp src/authority/native_api_key.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/kv_migration_base.cpp src/cache/kv_migration_paid_fallback.cpp
// src/cache/kv_migration_usage.cpp src/cache/kv_migration_validation.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/scope_experiment.cpp src/cache/scope_experiment_model.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_targets.cpp src/cache/telemetry_gate.cpp
// src/catalog/index.cpp src/catalog/models.cpp src/catalog/promotion.cpp src/catalog/store.cpp src/catalog/types.cpp src/chat/completions.cpp src/chat/envelope.cpp src/chat/stream_translation.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
// src/codex/index.cpp src/codex/manual_reset.cpp src/codex/manual_reset_state.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/quota.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp
// src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/codex/supervisor.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp
// src/codex/supervisor_follow.cpp src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp src/config.cpp src/debug_routing.cpp src/decisions/handlers.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp
// src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/deepseek/waterfall.cpp src/deepseek/waterfall_handler.cpp
// src/defaults.cpp src/embeddings/handlers.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp src/embeddings/voyage.cpp src/handler/admission.cpp src/handler/http.cpp src/handler/index.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp
// src/health.cpp src/http.cpp src/images.cpp src/inference_admission.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kernel/attestation.cpp src/kernel/policy_queue.cpp src/kernel/quota_reservations.cpp src/kernel/quota_reserve.cpp
// src/kernel/quota_v2.cpp src/kernel/quota_write.cpp src/kernel/usage.cpp src/kv.cpp src/kv_conversion/profile_json.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp
// src/live/calls.cpp src/live/handler.cpp src/live/routes.cpp src/live/sideband.cpp src/live/upstream.cpp src/metered_quota.cpp src/models/catalog.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
// src/models/codex_models_whitelist.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/cerebras.cpp src/provider/cerebras_handlers.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp
// src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/quota_projection.cpp src/recent_model_context.cpp src/release.cpp src/request.cpp src/request_policy.cpp src/responses_attempts.cpp src/responses_buffered.cpp
// src/responses_failover_stream.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/sentinel/incident_outbox.cpp
// src/sentinel/replay_admin.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_limits.cpp src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_retention_schema.cpp
// src/sentinel/replay_store.cpp src/sentinel/upstream_capture.cpp src/static.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/systemone/handlers.cpp src/types.cpp src/upstream_wire.cpp
// src/utils.cpp tests/helpers/counting_kv.cpp
// Port of tests/provider-capacity-rollups.test.ts, including the real request-router authentication gate.
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include <array>
#include "uos/handler/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  struct T2PublicAssets {
    std::vector<std::string> createdPaths;
    T2PublicAssets() {
      const auto temporary = uos::rt::env::get("TMPDIR");
      const auto directory = uos::rt::process::cwd();
      if (!temporary || !directory.starts_with(uos::rt::path::dirname(*temporary) + "/"))
        throw uos::rt::JsError("T2 public assets require the wrapper task workdir");
      const auto root = uos::rt::path::resolve(uos::rt::path::dirname(__FILE__), "../../..");
      const std::array<std::string, 3> names{"static", "favicon.png", "favicon-32.png"};
      for (const auto& name : names) {
        const auto target = uos::rt::path::join(root, name);
        const auto assetPath = uos::rt::path::join(directory, name);
        if (uos::rt::fs::existsSync(assetPath)) {
          if (uos::rt::fs::realpathSync(assetPath) != uos::rt::fs::realpathSync(target))
            throw uos::rt::JsError("T2 public asset fixture refuses an unexpected existing path");
        } else {
          uos::rt::fs::symlinkSync(target, assetPath);
          createdPaths.push_back(assetPath);
        }
      }
    }
    ~T2PublicAssets() {
      for (const auto& assetPath : createdPaths) uos::rt::fs::rmSync(assetPath);
    }
  };
  T2PublicAssets& t2PublicAssets() {
    static T2PublicAssets assets;
    return assets;
  }
  struct T2IngressScope {
    uos::config::Config savedConfig = uos::config::config;
    uos::rt::env::ScopedOverride credentials{{"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    uos::rt::GlobalFetchOverride blocked{uos::rt::Fetch([](uos::rt::RequestInfo, uos::rt::RequestInit) -> uos::rt::Response {
      throw uos::rt::JsError("T2 ingress fixture forbids an unstubbed upstream call");
    })};
    explicit T2IngressScope(std::shared_ptr<uos::rt::Kv> database = nullptr) {
      (void)t2PublicAssets();
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.authTokens.clear();
      uos::config::config.adminTokens.clear();
      if (!database) database = std::make_shared<uos::rt::MemoryKv>(uos::rt::dateNow);
      uos::kv::setKvForTest(std::move(database));
    }
    ~T2IngressScope() {
      uos::kv::setKvForTest(nullptr);
      uos::config::config = savedConfig;
    }
  };
}
#include "uos/provider/capacity_rollups.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/codex/index.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/config.hpp"
#include "uos/metered_quota.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::provider::capacity_rollups;
  using uos::codex::index::CODEX_AUTH_POOL_KV_KEY;
  using uos::metered_quota::METERED_QUOTA_STATE_KEY;
  using uos::tests::helpers::counting_kv::CountingKv;

  constexpr double nowMs = 1'800'000'000'000;

  const std::shared_ptr<CountingKv>& kv() {
    static const std::shared_ptr<CountingKv> instance = std::make_shared<CountingKv>();
    return instance;
  }

  void seed() {
    kv()->clearData();
    kv()->clearMeasurements();
    json::object pool;
    json::array accounts;
    for (const char* suffix : {"one", "two"}) {
      json::object account;
      account["access_token"] = std::string("token-") + suffix;
      account["refresh_token"] = std::string("refresh-") + suffix;
      account["account_id"] = std::string("account-") + suffix;
      account["updated_at_ms"] = rt::makeNumber(nowMs);
      accounts.push_back(std::move(account));
    }
    pool["accounts"] = std::move(accounts);
    pool["updated_at_ms"] = rt::makeNumber(nowMs);
    kv()->seed(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value(std::move(pool))));
    json::object metered;
    metered["current_balance_quota"] = 750;
    metered["post_refill_baseline_quota"] = 1'000;
    metered["last_observed_used_quota"] = 250;
    metered["quota_per_credit"] = 100;
    metered["observed_at_ms"] = rt::makeNumber(nowMs - 1'000);
    metered["cycle_started_at_ms"] = rt::makeNumber(nowMs - 5'000);
    metered["confidence"] = "refill_observed";
    metered["last_known_debits_quota"] = 0;
    metered["last_inferred_credit_quota"] = 1'000;
    metered["last_credit_at_ms"] = rt::makeNumber(nowMs - 5'000);
    metered["latest_refill_id"] = "refill-one";
    metered["latest_refill_amount_credits"] = 10;
    metered["latest_refill_completed_at_ms"] = rt::makeNumber(nowMs - 5'000);
    kv()->seed(METERED_QUOTA_STATE_KEY, rt::KvValue(json::value(std::move(metered))));
    rt::env::set("METERED_API_KEY", "metered-api-key");
    uos::codex::index::resetCodexAuthCacheForTest();
    uos::codex::account_routing::resetCodexAccountRoutingForTest();
    uos::kv::setKvForTest(kv());
  }

  struct TestScope {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt},
                                        {"CLAUDE_API_KEY", std::nullopt}, {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt},
                                        {"CLAUDE_CODE_WORKSPACE_ID", std::nullopt}, {"METERED_API_KEY", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    TestScope() { uos::config::config.codexAuthJsonB64.clear(); seed(); }
    ~TestScope() { uos::kv::setKvForTest(nullptr); uos::config::config.codexAuthJsonB64 = savedSeed; }
  };

  rt::Fetch createFetcher(double primaryOne, double secondaryOne, double primaryTwo, double secondaryTwo) {
    return rt::Fetch([=](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
      const auto* url = std::get_if<std::string>(&input);
      if (!url) throw rt::JsError("expected a string capacity URL");
      if (*url == "https://api.openlux.ai/api/usage/token/")
        return rt::Response::json(rt::jsonParse(R"({"success":true,"data":{"total_available":750,"total_granted":1000,"total_used":250,"unlimited_quota":false}})"));
      const rt::Headers headers = init.headers.value_or(rt::Headers());
      const bool first = headers.get("ChatGPT-Account-ID") == std::optional<std::string>("account-one");
      json::object primary{{"limit_window_seconds", 10'800}, {"used_percent", rt::makeNumber(first ? primaryOne : primaryTwo)}, {"reset_at", 1'800'010'000}};
      json::object secondary{{"limit_window_seconds", 86'400}, {"used_percent", rt::makeNumber(first ? secondaryOne : secondaryTwo)}, {"reset_at", 1'800'020'000}};
      return rt::Response::json(json::object{{"rate_limit", json::object{{"primary_window", primary}, {"secondary_window", secondary}}}});
    });
  }

  uos::provider::capacity_contract::ProviderCapacitySnapshotOptions optionsAt(double at, rt::Fetch fetcher = {}) {
    uos::provider::capacity_contract::ProviderCapacitySnapshotOptions options;
    options.kv = kv();
    options.fetcher = std::move(fetcher);
    options.now = [at] { return at; };
    return options;
  }

  // `rollupTestInput(bucketStartAtMs, sampledAtMs, primaryUsedPercent, state = "available")`
  ProviderCapacityRollupInput rollupTestInput(double bucketStartAtMs, double sampledAtMs, double primaryUsedPercent, const std::string& state = "available") {
    std::optional<ProviderCapacityRollupWindowInput> primary;
    if (state == "available") primary = ProviderCapacityRollupWindowInput{.limit_window_seconds = 10'800, .used_percent = primaryUsedPercent, .reset_at_ms = sampledAtMs + 10'800'000};
    return {
      .bucket_start_at_ms = bucketStartAtMs,
      .sampled_at_ms = sampledAtMs,
      .slots = {ProviderCapacityRollupSlotInput{.slot = 1, .state = state, .primary = primary, .secondary = std::nullopt},
                ProviderCapacityRollupSlotInput{.slot = 2, .state = "unavailable", .primary = std::nullopt, .secondary = std::nullopt}},
    };
  }

  rt::KvValue stored(const ProviderCapacityRollupPoint& point) { return rt::KvValue(json::value_from(point)); }

  std::vector<double> bucketStarts(const std::vector<ProviderCapacityRollupPoint>& points) {
    std::vector<double> out;
    for (const ProviderCapacityRollupPoint& entry : points) out.push_back(entry.bucket_start_at_ms);
    return out;
  }
}

ASYNC_TEST("research rollups keep hourly capacity summaries beyond the seven-day raw history window") {
  TestScope scope;
  const double researchNowMs = nowMs + 30.0 * 24 * 60 * 60'000;
  const double oldBucketStartAtMs = providerCapacityRollupBucketStartAtMs(nowMs - 30.0 * 24 * 60 * 60'000);
  kv()->seed(providerCapacityRollupKey(oldBucketStartAtMs), stored(mergeProviderCapacityRollup(std::nullopt, rollupTestInput(oldBucketStartAtMs, oldBucketStartAtMs + 1'000, 42))));
  const double nowBucketStartAtMs = providerCapacityRollupBucketStartAtMs(researchNowMs);
  kv()->seed(providerCapacityRollupKey(nowBucketStartAtMs),
             stored(mergeProviderCapacityRollup(std::nullopt, rollupTestInput(nowBucketStartAtMs, nowBucketStartAtMs + 1'000, 7, "unavailable"))));

  const ListProviderCapacityRollupsOptions longOptions{.sinceMs = researchNowMs - 90.0 * 24 * 60 * 60'000, .nowMs = researchNowMs};
  auto listingLong = listProviderCapacityRollups(kv(), longOptions);
  const std::vector<ProviderCapacityRollupPoint> longPoints = co_await std::move(listingLong);
  EXPECT_EQ(bucketStarts(longPoints), (std::vector<double>{oldBucketStartAtMs, nowBucketStartAtMs}));
  ASSERT_EQ(longPoints.size(), std::size_t{2});
  EXPECT_EQ(longPoints[0].slots[0].primary.max_used_percent, std::optional<double>(42));
  EXPECT_EQ(longPoints[1].slots[0].last_state, std::string("unavailable"));

  const ListProviderCapacityRollupsOptions recentOptions{.sinceMs = researchNowMs - 7.0 * 24 * 60 * 60'000, .nowMs = researchNowMs};
  auto listingRecent = listProviderCapacityRollups(kv(), recentOptions);
  const std::vector<ProviderCapacityRollupPoint> recent = co_await std::move(listingRecent);
  EXPECT_EQ(bucketStarts(recent), (std::vector<double>{nowBucketStartAtMs}));
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("capacity sampling folds an hourly capacity rollup into the persisted history atomic") {
  TestScope scope;
  auto options = optionsAt(nowMs, createFetcher(12.5, 38, 67, 81.25));
  auto refreshing = uos::provider::capacity::refreshProviderCapacity(options);
  const auto snapshot = co_await std::move(refreshing);
  (void)snapshot;
  const double bucket = providerCapacityRollupBucketStartAtMs(nowMs);
  const auto storedPoint = co_await kv()->get(providerCapacityRollupKey(bucket));
  EXPECT(!storedPoint.value.isNullish());
  ListProviderCapacityRollupsOptions listOptions{.sinceMs = nowMs - PROVIDER_CAPACITY_ROLLUP_BUCKET_MS, .nowMs = nowMs};
  auto listing = listProviderCapacityRollups(kv(), listOptions);
  const auto first = co_await std::move(listing);
  ASSERT_EQ(first.size(), std::size_t{1});
  EXPECT_EQ(first[0].bucket_start_at_ms, bucket);
  EXPECT_EQ(first[0].sample_count, 1);
  EXPECT_EQ(first[0].slots[0].slot, 1);
  EXPECT_EQ(first[0].slots[0].last_state, std::string("available"));
  EXPECT_EQ(first[0].slots[0].primary.last_used_percent, std::optional<double>(12.5));
  EXPECT_EQ(first[0].slots[0].primary.min_used_percent, std::optional<double>(12.5));
  EXPECT_EQ(first[0].slots[0].secondary.last_used_percent, std::optional<double>(38));
  EXPECT_EQ(first[0].slots[1].primary.last_used_percent, std::optional<double>(67));
  options = optionsAt(nowMs + 60'000, createFetcher(20.5, 30, 10, 90));
  refreshing = uos::provider::capacity::refreshProviderCapacity(options);
  const auto laterSnapshot = co_await std::move(refreshing);
  (void)laterSnapshot;
  listOptions.nowMs = nowMs + 60'000;
  listing = listProviderCapacityRollups(kv(), listOptions);
  const auto merged = co_await std::move(listing);
  ASSERT_EQ(merged.size(), std::size_t{1});
  EXPECT_EQ(merged[0].sample_count, 2);
  EXPECT_EQ(merged[0].slots[0].primary.min_used_percent, std::optional<double>(12.5));
  EXPECT_EQ(merged[0].slots[0].primary.max_used_percent, std::optional<double>(20.5));
  EXPECT_EQ(merged[0].slots[0].primary.last_used_percent, std::optional<double>(20.5));
  EXPECT_EQ(merged[0].slots[1].primary.min_used_percent, std::optional<double>(10));
  EXPECT_EQ(merged[0].slots[1].primary.max_used_percent, std::optional<double>(67));
}

ASYNC_TEST("the event sampler records the hourly rollup on the request-terminal hook path") {
  TestScope scope;
  auto options = optionsAt(nowMs, createFetcher(5, 15, 25, 35));
  options.createLeaseOwner = [] { return std::string("event-sampler"); };
  auto sampling = uos::provider::capacity::sampleProviderCapacityOnEvent(options);
  co_await std::move(sampling);
  const ListProviderCapacityRollupsOptions listOptions{.sinceMs = nowMs - PROVIDER_CAPACITY_ROLLUP_BUCKET_MS, .nowMs = nowMs};
  auto listing = listProviderCapacityRollups(kv(), listOptions);
  const auto points = co_await std::move(listing);
  ASSERT_EQ(points.size(), std::size_t{1});
  EXPECT_EQ(points[0].sample_count, 1);
  EXPECT_EQ(points[0].slots[0].last_state, std::string("available"));
  EXPECT_EQ(points[0].slots[0].primary.last_used_percent, std::optional<double>(5));
  EXPECT_EQ(points[0].slots[1].primary.last_used_percent, std::optional<double>(25));
}

ASYNC_TEST("capacity rollups research route returns the requested window") {
  TestScope scope;
  const double bucket = providerCapacityRollupBucketStartAtMs(nowMs);
  kv()->seed(providerCapacityRollupKey(bucket), stored(mergeProviderCapacityRollup(std::nullopt, rollupTestInput(bucket, nowMs, 33))));
  auto options = optionsAt(nowMs);
  rt::Request request("https://ai.ubq.fi/admin/providers/capacity/rollups?window_days=365");
  auto handling = uos::provider::capacity::handleProviderCapacityRollups(request, options);
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  auto reading = response.json();
  const auto body = co_await std::move(reading);
  EXPECT_EQ(rt::getNumber(body, "window_days"), std::optional<double>(365));
  EXPECT_EQ(rt::getString(body, "rollup_scan"), std::optional<std::string>("ok"));
  EXPECT_EQ(rt::getNumber(body.at("retention"), "rollup_bucket_ms"), std::optional<double>(PROVIDER_CAPACITY_ROLLUP_BUCKET_MS));
  EXPECT_EQ(rt::getNumber(body.at("retention"), "rollup_window_ms"), std::optional<double>(365.0 * 24 * 60 * 60'000));
  const auto& rollups = *rt::getArray(body, "rollups");
  ASSERT_EQ(rollups.size(), std::size_t{1});
  EXPECT_EQ(rt::getNumber(rollups[0], "bucket_start_at_ms"), std::optional<double>(bucket));
  EXPECT_EQ(rt::getNumber(rollups[0].at("slots").at(0).at("primary"), "last_used_percent"), std::optional<double>(33));
  const std::vector<std::pair<std::string, double>> windows{{"nope", uos::provider::capacity::PROVIDER_CAPACITY_RESEARCH_DEFAULT_WINDOW_DAYS},
                                                        {"100000", uos::provider::capacity::PROVIDER_CAPACITY_RESEARCH_MAX_WINDOW_DAYS}};
  for (const auto& [window, expected] : windows) {
    request = rt::Request("https://ai.ubq.fi/admin/providers/capacity/rollups?window_days=" + window);
    handling = uos::provider::capacity::handleProviderCapacityRollups(request, options);
    response = co_await std::move(handling);
    reading = response.json();
    const auto projected = co_await std::move(reading);
    EXPECT_EQ(rt::getNumber(projected, "window_days"), std::optional<double>(expected));
  }
}

ASYNC_TEST("capacity rollups route requires admin authentication through the request router") {
  T2IngressScope ingress;
  const rt::Request request("https://ai.ubq.fi/admin/providers/capacity/rollups");
  auto handling = uos::handler::index::handler(request);
  const auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 401);
}

TEST("additive Claude rollups retain source order, true zero and cohort/reset boundaries") {
  namespace claude = uos::provider::claude_capacity_rollups;
  using uos::provider::capacity_contract::ClaudeCapacityHistoryPoint;
  auto point = json::value_to<ClaudeCapacityHistoryPoint>(rt::jsonParse(R"({"v":1,"point_id":"one","bucket_start_at_ms":1800000000000,"account_id":"local-a","account_cohort_id":"cohort-a","credential_kind":"oauth","workspace_id":null,"sampled_at_ms":1800000000000,"source_observed_at_ms":1800000000000,"state":"available","response_status":200,"windows":{"five_hour":{"status":"allowed","used_percent":0,"reset_at_ms":1800018000000,"observed_at_ms":1800000000000},"seven_day":null},"standard":[]})"));
  point.bucket_start_at_ms = rt::Math::floor(nowMs / 900'000) * 900'000;
  const auto first = claude::mergeClaudeCapacityRollup(std::nullopt, point, "five_hour");
  EXPECT_EQ(first.min_used_percent, std::optional<double>(0));
  EXPECT_EQ(first.last_used_percent, std::optional<double>(0));
  EXPECT_EQ(first.samples, 1);
  auto next = point;
  next.point_id = "two";
  next.sampled_at_ms = nowMs + 1'000;
  next.source_observed_at_ms = nowMs + 1'000;
  next.windows.five_hour->used_percent = 25;
  next.windows.five_hour->observed_at_ms = nowMs + 1'000;
  const auto second = claude::mergeClaudeCapacityRollup(first, next, "five_hour");
  EXPECT_EQ(second.samples, 2);
  EXPECT_EQ(second.min_used_percent, std::optional<double>(0));
  EXPECT_EQ(second.max_used_percent, std::optional<double>(25));
  EXPECT_EQ(second.last_used_percent, std::optional<double>(25));
  const auto late = claude::mergeClaudeCapacityRollup(second, point, "five_hour");
  EXPECT_EQ(late.last_used_percent, std::optional<double>(25));
  const auto reread = claude::readClaudeCapacityRollup(rt::KvValue(json::value_from(second)));
  ASSERT(reread.has_value());
  EXPECT_EQ(reread->last_used_percent, std::optional<double>(25));
  const std::vector<json::object> patches{{{"samples", 99}}, {{"min_used_percent", 80}, {"max_used_percent", 20}}, {{"last_used_percent", 26}},
                                         {{"last_source_observed_at_ms", rt::makeNumber(second.last_sampled_at_ms + 1)}}, {{"first_source_observed_at_ms", nullptr}}};
  for (const auto& patch : patches) {
    const json::value malformed(rt::objectSpread(json::value_from(second).get_object(), patch));
    EXPECT(!claude::readClaudeCapacityRollup(rt::KvValue(malformed)).has_value());
  }
  auto reset = point;
  reset.windows.five_hour->reset_at_ms = nowMs + 36'000'000;
  EXPECT(claude::claudeCapacityRollupKey(reset, "five_hour") != claude::claudeCapacityRollupKey(point, "five_hour"));
  EXPECT_THROWS_MATCHING(claude::mergeClaudeCapacityRollup(first, reset, "five_hour"), "binding mismatch");
  point.account_cohort_id = "replacement";
  EXPECT_THROWS_MATCHING(claude::mergeClaudeCapacityRollup(first, point, "five_hour"), "binding mismatch");
}
