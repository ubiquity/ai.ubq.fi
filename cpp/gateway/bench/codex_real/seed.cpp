// Offline fixture writer. The driver opens only a new synthetic state directory.
#include "uos/admin/error_log.hpp"
#include "uos/analytics.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/cache/telemetry_gate.hpp"
#include "uos/embeddings/ledger.hpp"
#include "uos/metered_quota.hpp"
#include "uos/provider/capacity_history.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_envelope.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/storage/kv.hpp"
#include "uos/utils.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
  using namespace uos;
  namespace usage = api_key_policy;
  namespace replay = sentinel::replay_model;
  namespace retention = sentinel::replay_retention_schema;
  namespace incident = sentinel::incident_outbox;
  constexpr int keyCount = 8'893;
  constexpr int usageRows = 413'480;
  constexpr int activeUsage = 5'000;
  constexpr double day = 86'400'000;
  constexpr double windowMs = 7 * day;
  const std::string activeToken = "u_" + std::string(64, 'b');

  template <class T> rt::KvValue stored(const T& value) { return rt::KvValue(json::value_from(value)); }
  std::string keyId(int i) { return "bench-key-" + std::to_string(i); }
  std::string requestId(int i) { return "history-" + std::to_string(i); }

  rt::Task<void> commit(rt::AtomicOperation& operation) {
    const auto receipt = co_await operation.commit();
    if (!receipt.ok) throw std::runtime_error("Synthetic KV commit failed");
  }

  rt::Task<void> seedUsage(rt::Kv& kv, double now) {
    constexpr int distributedRows = usageRows - keyCount - activeUsage;
    const double then = now - day;
    int serial = 0;
    for (int i = 0; i < keyCount; ++i) {
      const auto id = keyId(i);
      const auto token = i == 0 ? activeToken : "offline synthetic key " + id;
      auto hashing = utils::sha256Base64Url(token);
      const auto hash = co_await std::move(hashing);
      const auto name = "Synthetic client " + std::to_string(i) + std::string(133, 'x');
      const types::ApiKeyRecord record{.id = id, .name = name, .prefix = token.substr(0, 12), .hash = hash,
        .created_at_ms = then, .expires_at_ms = -1, .revoked_at_ms = {}, .usage_limit_requests = 100'000,
        .usage_requests = 0, .usage_reset_at_ms = then + windowMs, .window_ms = windowMs, .usage_quota_version = 3,
        .paid_fallback_enabled = false, .paid_fallback_limit_microcredits = 0, .paid_fallback_spent_microcredits = 0,
        .paid_fallback_reserved_microcredits = 0, .paid_fallback_reservation_request_id = {}, .paid_fallback_model_ids = {},
        .paid_fallback_quota_per_credit = 0, .paid_fallback_pricing_checked_at_ms = {}};
      const auto hashRecord = authority::native_api_key_policy::apiKeyHashRecordFor(record);
      const auto policy = usage::apiKeyPolicyFromHashRecord(hash, hashRecord, now);
      if (!policy) throw std::runtime_error("Invalid fixture API key policy");
      auto window = usage::makeApiKeyUsageWindowV3(*policy, then);
      const int count = i == 0 ? activeUsage : distributedRows / (keyCount - 1) + (i <= distributedRows % (keyCount - 1));
      window.committed_requests = count;
      const rt::KvSetOptions ttl{.expireIn = usage::apiKeyUsageV3RetentionMs(policy->usage_reset_at_ms, now)};
      auto operation = kv.atomic();
      operation->set(api_keys::apiKeyIdKey(id), stored(record));
      operation->set(api_keys::apiKeyHashKey(hash), stored(hashRecord));
      operation->set(usage::apiKeyUsageV3WindowKey(*policy), stored(window), ttl);
      for (int j = 0; j < count; ++j) {
        if (j != 0 && j % 256 == 0) {
          co_await commit(*operation);
          operation = kv.atomic();
        }
        const auto request = requestId(serial++);
        const types::ApiKeyUsageRequestV3 used{.key_id = id, .request_id = request, .route = "responses", .state = "dispatched",
          .reserved_at_ms = then + j, .lease_expires_at_ms = then + j + usage::API_KEY_USAGE_V3_RESERVATION_LEASE_MS,
          .provider = "chatgpt_codex", .dispatched_at_ms = then + j, .released_at_ms = {}, .release_reason = {}};
        operation->set(usage::apiKeyUsageV3RequestKey(*policy, request), stored(used), ttl);
      }
      co_await commit(*operation);
    }
  }

  rt::Task<void> seedReplay(rt::Kv& kv, double now) {
    constexpr int captures = 2'000;
    constexpr int statusOnly = 594;
    const double then = now - day;
    const rt::KvSetOptions payloadTtl{.expireIn = replay::SENTINEL_REPLAY_TTL_MS - day};
    const rt::KvSetOptions statusTtl{.expireIn = replay::SENTINEL_REPLAY_STATUS_TTL_MS - day};
    retention::SentinelReplayBudgetLedger ledger{};
    ledger.budget_bytes = retention::sentinelReplayBudgetBytes();
    ledger.bootstrap_complete = true;
    ledger.records = captures;
    ledger.status_records = captures + statusOnly;
    auto operation = kv.atomic();
    for (int i = 0; i < captures + statusOnly; ++i) {
      const std::string id = "bench-capture-" + std::to_string(i);
      const auto request = requestId(i);
      replay::SentinelReplayCaptureStatusRow status{.request_id = request, .status = "disabled",
        .reason = "key_missing", .captured_at_ms = then, .manifest_key = {}, .fingerprint = {}, .expires_at_ms = {}};
      if (i < captures) {
        const auto digest = co_await utils::sha256Hex(id);
        const std::size_t length = i == 0 ? 48'000 : 15'000 + static_cast<std::size_t>(i % 64) * 4;
        const double bytes = static_cast<double>(length);
        const double charge = sentinel::replay_limits::sentinelReplayStoredCharge(bytes * 0.75, 4'096);
        const replay::SentinelReplayManifest manifest{.capture_id = id, .fingerprint = digest, .case_group_digest = digest,
          .captured_at_ms = then, .expires_at_ms = then + replay::SENTINEL_REPLAY_TTL_MS, .algorithm = "AES-256-GCM",
          .compression = "gzip", .iv = "AAAAAAAAAAAAAAAA", .chunk_count = 1, .ciphertext_bytes = bytes * 0.75,
          .stored_bytes = charge, .request_id = request};
        const auto manifestKey = retention::sentinelReplayManifestKey(manifest);
        const retention::SentinelReplayAccountingRow account{.capture_id = id, .request_id = request, .fingerprint = digest,
          .bytes = charge, .state = "stored", .fence = 1, .created_at_ms = then, .expires_at_ms = manifest.expires_at_ms,
          .status_expires_at_ms = then + replay::SENTINEL_REPLAY_STATUS_TTL_MS, .stage = 1};
        operation->set(manifestKey, stored(manifest), payloadTtl);
        operation->set(rt::KvKey{replay::SENTINEL_REPLAY_DEDUPE_PREFIX, digest}, sentinel::replay_envelope::dedupeRecordFor(manifestKey, manifest), payloadTtl);
        operation->set(rt::KvKey{replay::SENTINEL_REPLAY_CHUNK_PREFIX, id, 0}, rt::KvValue(std::string(length, 'A')), payloadTtl);
        operation->set(retention::sentinelReplayAccountingKey(then, id), retention::sentinelReplayAccountingValue(account));
        ledger.stored_bytes += charge;
        status.status = "ready";
        status.reason.reset();
        status.manifest_key = manifestKey;
        status.fingerprint = digest;
        status.expires_at_ms = manifest.expires_at_ms;
      }
      const auto value = replay::toKvValue(status);
      ledger.metadata_bytes += retention::sentinelReplayStatusMetadataBytes(value);
      operation->set(replay::requestStatusKey(request), value, statusTtl);
      if ((i + 1) % 16 == 0) {
        co_await commit(*operation);
        operation = kv.atomic();
      }
    }
    operation->set(retention::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, retention::sentinelReplayLedgerValue(ledger));
    co_await commit(*operation);
  }

  rt::Task<void> seedBackground(rt::Kv& kv, double now) {
    const double then = now - day;
    for (int base = 0; base < 47'089; base += 64) {
      auto operation = kv.atomic();
      for (int i = base; i < std::min(base + 64, 47'089); ++i) {
        const auto id = keyId(i % keyCount);
        const auto request = requestId(i);
        const types::ApiKeyRequestLogRecord ledger{.id = request, .key_id = id, .route = "responses",
          .path = "/v1/responses", .method = "POST", .status_code = 200, .stream = true, .model = "gpt-smoke-fixture",
          .reasoning = "none", .created_at_ms = then + i, .provider = "surplus", .fallback_reason = "subscription_capacity",
          .provider_request_id = request, .completed_at_ms = then + i + 40, .latency_ms = 40, .input_tokens = 100,
          .output_tokens = 50, .provider_quota = 10, .quota_per_credit = 1000, .spend_microcredits = 10'000,
          .paid_fallback_window_reset_at_ms = now, .billing_status = "reconciled"};
        // Historical ledger rows omit null/optional metadata that current writers can add.
        auto ledgerValue = json::value_from(ledger);
        auto& fields = ledgerValue.as_object();
        for (const auto* field : {"reasoning", "fallback_reason", "provider_request_id", "paid_fallback_window_reset_at_ms"})
          fields.erase(field);
        operation->set(analytics::apiKeyRequestLogKey(id, ledger.created_at_ms, request), rt::KvValue(std::move(ledgerValue)));
        if (i < 6'948) {
          const admin::error_log::AdminErrorLogRecord error{.request_id = request, .route = "responses", .status = 502,
            .provider = "chatgpt_codex", .model = "gpt-smoke-fixture", .reasoning = "none", .stream = true,
            .terminal_type = "response.failed", .failure_kind = "upstream_error", .delivery_outcome = "delivered",
            .created_at_ms = then + i, .latency_ms = 40, .git_sha = std::string(40, '0'), .deno_revision = {}};
          operation->set(rt::KvKey{admin::error_log::ADMIN_ERROR_LOG_PREFIX, error.created_at_ms, request}, stored(error),
                         {.expireIn = admin::error_log::ADMIN_ERROR_LOG_TTL_MS - day});
        }
        if (i < 3'116) {
          const double bucket = now - (3'116 - i) * 900'000;
          const json::object codex{{"provider", "codex"}, {"status", "available"}, {"observed_at_ms", bucket},
            {"account_ids", json::array{"bench-account"}}, {"accounts", json::array{json::object{
              {"account_id_hash", std::string(64, 'a')}, {"slot", 1}, {"available", true}, {"observed_at_ms", bucket},
              {"primary", json::object{{"used_percent", 33}, {"reset_at_ms", now}}},
              {"secondary", json::object{{"used_percent", 20}, {"reset_at_ms", now}}}}}}, {"primary", json::object{{"used_percent", 33}, {"window_minutes", 300}, {"reset_at_ms", now}}},
            {"secondary", json::object{{"used_percent", 20}, {"window_minutes", 10080}, {"reset_at_ms", now}}}};
          const json::object capacity{{"bucket_start_at_ms", bucket}, {"sampled_at_ms", bucket},
            {"sources", json::array{codex, codex, json::object{{"provider", "metered"}, {"status", "unavailable"}}}}};
          operation->set(provider::capacity_history::providerCapacityHistoryKey(bucket), rt::KvValue(capacity));
        }
        if (i < 848) {
          json::array vector(240, json::value(0.001));
          operation->set(embeddings::ledger::embeddingsCacheKey("bench-embedding-profile", request),
                         rt::KvValue(json::object{{"embedding", std::move(vector)}, {"created_at_ms", then + i}}));
        }
        if (i < 2'051) {
          const double bucket = now - (2'051 - i) * 900'000;
          const metered_quota::MeteredQuotaBalanceSample balance{.bucket_start_at_ms = bucket, .observed_at_ms = bucket,
            .balance_quota = 500'000, .baseline_quota = 1'000'000, .quota_per_credit = 500'000, .remaining_percent = 50};
          operation->set(rt::KvKey{metered_quota::METERED_QUOTA_BALANCE_HISTORY_PREFIX, "bench-account", bucket}, stored(balance));
        }
        if (i < 6'792) {
          const double bucket = now - (6'792 - i) * 60'000;
          operation->set(cache::prompt_analytics_core::promptCacheAnalyticsCounterKey(bucket, "completed_requests"), rt::KvValue(rt::KvU64(7)));
        }
        if (i < 2'848)
          operation->set(rt::KvKey{cache::telemetry_gate::PROMPT_CACHE_TELEMETRY_GATE_KV_PREFIX, std::string(40, '0'),
            "chatgpt_codex", "responses", "gpt-bench-" + std::to_string(i), "completed"}, rt::KvValue(rt::KvU64(7)));
        if (i < 538) {
          const incident::SentinelIncidentFailureEvent event{.incident_id = request, .state = "ready", .observed_at_ms = then,
            .created_at_ms = then, .ready_at_ms = then, .capture_status = "unavailable", .capture_fingerprint = {}, .manifest_key = {}};
          operation->set(rt::KvKey{incident::SENTINEL_INCIDENT_EVENT_PREFIX, request}, incident::toKvValue(event));
        }
        if (i < 72)
          operation->set(rt::KvKey{"ubq_ai", "kernel_auth", "usage", "bench-owner", request}, rt::KvValue(7));
        if (i < 52)
          operation->set(rt::KvKey{provider::health::PROVIDER_HEALTH_KEY_PREFIX, "codex", request},
                         rt::KvValue(json::object{{"event", "success"}, {"status", 200}, {"observed_at_ms", then}}));
      }
      co_await commit(*operation);
    }
  }

  rt::Task<void> run(std::string path, bool verify) {
    auto opening = storage::kv::openKv(std::move(path));
    auto kv = co_await std::move(opening);
    const double now = rt::dateNow();
    if (verify) {
      auto hashing = utils::sha256Base64Url(activeToken);
      const auto hash = co_await std::move(hashing);
      const auto row = co_await kv->get(api_keys::apiKeyHashKey(hash));
      const auto policy = usage::apiKeyPolicyFromHashRecord(hash, rt::storedJsonView(row.value), now);
      if (!policy) throw std::runtime_error("Fixture active key was lost");
      const auto count = co_await usage::getApiKeyUsageV3(*policy, kv);
      std::printf("%.0f\n", count);
    } else {
      co_await seedUsage(*kv, now);
      co_await seedReplay(*kv, now);
      co_await seedBackground(*kv, now);
    }
    kv->close();
  }
}

int main(int argc, char** argv) {
  if (argc != 2 && (argc != 3 || std::string_view(argv[2]) != "--usage")) return 2;
  try {
    uos::rt::blockOn(run(argv[1], argc == 3));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
