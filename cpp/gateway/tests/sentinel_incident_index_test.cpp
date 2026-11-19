// Links: tests/sentinel_incident_index_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//        src/utils.cpp src/sentinel/incident_outbox.cpp src/kv.cpp src/auth/passkeys.cpp src/http.cpp src/provider/cerebras_rate_limits.cpp src/auth/index.cpp src/sentinel/replay_model.cpp src/config.cpp src/api_keys.cpp src/api_key_policy.cpp src/agent_messages.cpp src/auth/passport.cpp src/authority/native_api_key.cpp src/storage/kv.cpp
//        src/auth/passport_sessions.cpp src/handler/index.cpp src/admin/codex.cpp src/admin/kernel.cpp src/health.cpp src/provider/metered.cpp src/models/codex_models_fetch.cpp src/metered_quota.cpp src/paid_fallback/ledger_backfill.cpp src/authority/native_api_key_policy.cpp src/codex/dispatch.cpp src/authority/native_paid_policy.cpp
//        src/auth/local_admin.cpp src/provider/lithos.cpp src/models/codex_models.cpp src/admin/error_log.cpp src/provider/surplus.cpp src/admin/defaults.cpp src/models/catalog.cpp src/runtime_config.cpp src/provider/health.cpp src/models/codex_models_availability.cpp src/authority/native_paid_contracts.cpp
//        src/codex/banked_reset_provider.cpp src/kernel/quota_v2.cpp src/input_normalization.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp src/authority/native_kernel.cpp src/provider/claude_code_accounts.cpp src/codex/auth.cpp src/provider/cerebras.cpp src/codex/native_auth.cpp src/paid_fallback/rollups.cpp
//        src/provider/surplus_claude_chat_request.cpp src/models/openrouter_models.cpp src/request.cpp src/authority/native_paid.cpp src/codex/external_auth.cpp src/provider/claude_code_credentials.cpp src/codex/native_auth_transport.cpp src/sentinel/replay_read.cpp src/admin/claude_code.cpp src/runtime/websocket.cpp src/admin/api_keys.cpp
//        src/types.cpp src/admin/api_key_audit.cpp src/codex/experiment.cpp src/codex/overage_settings.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/kernel/usage.cpp src/kernel/quota_reservations.cpp src/request_policy.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//        src/paid_fallback/ledger_settlement.cpp src/provider/claude_code_models.cpp src/catalog/index.cpp src/codex/auth_refresh.cpp src/kernel/attestation.cpp src/provider/selection.cpp src/sentinel/replay_retention_schema.cpp src/authority/native_api_key_audit.cpp src/codex/supervisor.cpp src/sentinel/replay_store.cpp src/catalog/models.cpp
//        src/sentinel/replay_retention.cpp src/models/metadata.cpp src/provider/surplus_claude_request.cpp src/sentinel/replay_retention_bootstrap.cpp src/cache/scope_experiment_targets.cpp src/provider/capacity_history.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/anthropic_chat_responses.cpp
//        src/codex/rate_limit_429.cpp src/storage/sqlite_readonly.cpp src/debug_routing.cpp src/sentinel/replay_envelope.cpp src/catalog/store.cpp src/codex/routing_mutations.cpp src/codex/capacity_routing.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_mutations.cpp src/provider/anthropic_response_wire.cpp
//        src/codex/reset_settings.cpp src/responses_stream.cpp src/catalog/types.cpp src/authority/native_kernel_contracts.cpp src/sentinel/replay_retention_metadata.cpp src/cache/kv_migration_base.cpp src/admin/sentinel_incident.cpp src/admin/capacity_observation_sync.cpp src/cache/scope_experiment_model.cpp src/authority/native_codex_429.cpp
//        src/codex/routing_state.cpp src/sentinel/replay_observation.cpp src/authority/native_codex_contracts.cpp src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/admin/chart_source_proxy.cpp src/defaults.cpp src/codex/supervisor_config.cpp src/cache/prompt_analytics_core.cpp src/kv_conversion/profile_json.cpp
//        src/auth/relay.cpp src/sentinel/replay_admin.cpp src/sentinel/upstream_capture.cpp src/admin/api_key_mutations.cpp src/openai_telemetry.cpp src/paid_fallback/ledger_state.cpp src/models/codex_models_whitelist.cpp src/deepseek/index.cpp src/provider/claude_code_access.cpp src/kernel/policy_queue.cpp src/codex/supervisor_brief.cpp
//        src/auth/local_development_key.cpp src/provider/capacity_analytics.cpp src/admin/codex_reset.cpp src/analytics.cpp src/kernel/quota_write.cpp src/quota_projection.cpp src/responses_failover_stream.cpp src/codex/supervisor_log.cpp src/provider/lithos_rate_limits.cpp src/codex/routing_serial.cpp src/paid_fallback/index.cpp
//        src/live/handler.cpp src/provider/claude_capacity_history.cpp src/codex/manual_reset.cpp src/codex/routing_evaluation.cpp src/live/calls.cpp src/provider/lithos_models.cpp src/provider/anthropic_replay.cpp src/codex/banked_reset.cpp src/provider/claude_capacity_rollups.cpp src/live/upstream.cpp src/paid_fallback/ledger_admission.cpp
//        src/codex/supervisor_transport.cpp src/static.cpp src/cache/scope_experiment.cpp src/sentinel/replay_limits.cpp src/provider/claude_code_usage_schema.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex.cpp src/live/routes.cpp src/provider/openrouter.cpp src/recent_model_context.cpp src/cache/scope_targets.cpp
//        src/codex/manual_reset_state.cpp src/handler/http.cpp src/codex/responses_error.cpp src/codex/banked_reset_pool.cpp src/bounded_response_body.cpp src/catalog/promotion.cpp src/codex/supervisor_secret.cpp src/cache/telemetry_gate.cpp src/codex/banked_reset_claim.cpp src/handler/terminal_route.cpp src/cache/kv_migration_validation.cpp
//        src/deepseek/waterfall.cpp src/provider/presentation.cpp src/provider/anthropic_chat_stream.cpp src/embeddings/handlers.cpp src/cache/kv_migration_usage.cpp src/storage/native_kv.cpp src/cache/prompt_analytics_queue.cpp src/cache/kv_migration_paid_fallback.cpp src/release.cpp src/embeddings/ledger.cpp src/codex/supervisor_inventory.cpp
//        src/embeddings/voyage.cpp src/chat/envelope.cpp src/provider/anthropic_responses.cpp src/responses_handler.cpp src/responses_attempts.cpp src/responses_request.cpp src/provider/cerebras_responses.cpp src/provider/removed_provider_circuit.cpp src/handler/terminal_log.cpp src/codex/supervisor_follow.cpp src/openai.cpp
//        src/deepseek/handlers.cpp src/upstream_wire.cpp src/embeddings/jobs.cpp src/provider/stream_relay.cpp src/provider/lithos_handlers.cpp src/responses_native_headers.cpp src/deepseek/stream.cpp src/provider/cerebras_handlers.cpp src/deepseek/responses.cpp src/paid_fallback/health.cpp src/paid_fallback/routing.cpp
//        src/provider/openrouter_request_controls.cpp src/paid_fallback/removed_provider.cpp src/provider/removed_provider_telemetry.cpp src/systemone/handlers.cpp src/handler/admission.cpp src/live/sideband.cpp src/provider/lithos_streams.cpp src/provider/openrouter_handlers.cpp src/kernel/quota_reserve.cpp src/provider/openrouter_web_tools.cpp
//        src/deepseek/flash_aliases.cpp src/inference_deadline.cpp src/codex/banked_reset_submission.cpp src/codex/quota.cpp src/deepseek/chat_projection.cpp src/deepseek/responses_stream.cpp src/deepseek/responses_payload.cpp src/inference_admission.cpp src/chat/stream_translation.cpp src/provider/openrouter_streams.cpp
//        src/optional_telemetry_queue.cpp src/deepseek/waterfall_handler.cpp src/runtime/http_early_sse.cpp src/decisions/handlers.cpp src/deepseek/forwarded_payload_policy.cpp src/images.cpp src/deepseek/context_admission_failure.cpp src/chat/completions.cpp src/lib/jev_compaction/codex_items.cpp src/runtime/http_tasks.cpp src/codex/index.cpp
//        src/openai_schema.cpp src/responses_buffered.cpp src/paid_fallback/claude_code_route.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp src/lib/jev_compaction/compact.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/request.cpp
// Port of all thirteen original tests/sentinel-incident-index.test.ts cases, plus stored-member regression evidence.
#include "uos/kv.hpp"
#include "uos/api_keys.hpp"
#include "uos/auth/passkeys.hpp"
#include "uos/debug_routing.hpp"
#include "uos/handler/index.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "tests/helpers/terminal_log_harness.hpp"
#include "uos/rt/url.hpp"
#include <algorithm>
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_capture.hpp"
#include "uos/utils.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace kvfacade = uos::kv;
using namespace uos::sentinel::incident_outbox;

namespace {
  using uos::sentinel::replay_capture::AcceptedSentinelReplayInput;
  using uos::sentinel::replay_capture::persistEncryptedSentinelReplay;
  using uos::sentinel::replay_capture::SENTINEL_REPLAY_MANIFEST_PREFIX;
  using uos::sentinel::replay_capture::SentinelFailureObservation;
  using uos::sentinel::replay_model::SentinelClientFailureObservation;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::SentinelReplayPersistResult;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultDuplicate;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;

  // `clientObservationFor(status)`: the members the index reads.
  SentinelIncidentIndexObservation clientObservationFor(double status) {
    return {.status = status, .stream = false, .completed = false, .framing_valid = true, .terminal_type = std::string("http.error")};
  }

  rt::Task<std::vector<rt::KvKey>> listedKeys(rt::Kv& kv, rt::KvKey prefix) {
    rt::KvListSelector selector;
    selector.prefix = std::move(prefix);
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(selector);
    std::vector<rt::KvKey> keys;
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    co_return keys;
  }

  // `const kv = await openKv(":memory:"); setKvForTest(kv); try { ... } finally { kv.close(); setKvForTest(null); }`
  struct InstalledStore {
    std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    InstalledStore() { kvfacade::setKvForTest(kv); }
    ~InstalledStore() {
      kv->close();
      kvfacade::setKvForTest(nullptr);
    }
    InstalledStore(const InstalledStore&) = delete;
    InstalledStore& operator=(const InstalledStore&) = delete;
  };

  rt::Bytes newSyntheticKey() {
    rt::Bytes keyBytes(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(keyBytes));
    return keyBytes;
  }

  // new TextEncoder().encode(text)
  std::shared_ptr<rt::Bytes> encoded(std::string_view text) { return std::make_shared<rt::Bytes>(rt::encodeUtf8(text)); }

  SentinelFailureObservation failureObservation(double status = 502) {
    return {
      .status = status,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "test-provider",
    };
  }

  AcceptedSentinelReplayInput syntheticInput(std::shared_ptr<rt::Bytes> bytes, const std::string& requestId, const std::string& endpoint = "/v1/responses") {
    return {
      .endpoint = endpoint,
      .method = "POST",
      .body = std::move(bytes),
      .content_type = "application/json",
      .compatibility_headers = json::object{{"x-codex-client-version", "9.9.9-secret-version"}},
      .request_id = requestId,
      .git_sha = "synthetic-git-sha",
      .deno_revision = "synthetic-revision",
    };
  }

  rt::Bytes twelveByteIv(double) { return rt::Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}; }

  // `clientObservationFor(status)` as persistEncryptedSentinelReplay reads it.
  SentinelClientFailureObservation clientFailureObservationFor(double status) {
    return {
      .status = status,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .framing_valid = true,
      .provider_route = "test-provider",
      .error_code = std::nullopt,
      .error_param = std::nullopt,
      .terminal_body_base64 = std::nullopt,
      .terminal_body_truncated = false,
    };
  }

  // `{ kv, keyBytes, now: () => nowMs, randomUuid: () => captureId, randomBytes: twelveByteIv }`
  PersistDependencies dependenciesFor(std::shared_ptr<rt::Kv> kv, const rt::Bytes& keyBytes, double nowMs, std::string captureId) {
    return {
      .kv = std::move(kv),
      .keyBytes = keyBytes,
      .now = [nowMs] { return nowMs; },
      .randomUuid = [captureId] { return captureId; },
      .randomBytes = twelveByteIv,
    };
  }

  // `{ endpoint: input.endpoint, method: input.method, gitSha, observedAtMs, observation: clientObservationFor(status) }`
  SentinelIncidentIndexObservationInput observationInput(const AcceptedSentinelReplayInput& input, const std::string& gitSha, double observedAtMs, double status) {
    return {.endpoint = input.endpoint, .method = input.method, .gitSha = gitSha, .observedAtMs = observedAtMs, .observation = clientObservationFor(status)};
  }

  // `row.evidence_ref?.ref`
  std::optional<std::string> evidenceRefOf(const SentinelIncidentIndexRow& row) {
    return row.evidence_ref ? std::optional<std::string>(row.evidence_ref->ref) : std::nullopt;
  }

  // `(await listSentinelIncidentIndexRows(kv, options)).rows`
  rt::Task<std::vector<SentinelIncidentIndexRow>> listedRows(std::shared_ptr<rt::Kv> kv, std::optional<std::string> incidentId, double limit) {
    SentinelIncidentIndexListOptions options;
    options.incidentId = std::move(incidentId);
    options.limit = limit;
    auto listing = listSentinelIncidentIndexRows(*kv, std::move(options));
    const SentinelIncidentIndexPage page = co_await std::move(listing);
    co_return page.rows;
  }

  // `assert.rejects(promise, /pattern/)` tests the pattern against String(reason): the text, or std::nullopt when
  // the task fulfils.
  template <class T> rt::Task<std::optional<std::string>> rejectionText(rt::Task<T> task) {
    std::optional<std::string> text;
    try {
      co_await std::move(task);
    } catch (...) {
      text = rt::test::thrownText();
    }
    co_return text;
  }
  const std::string SUPER_ADMIN_TOKEN = "sentinel-incident-index-super-admin";
  const std::string MARKER = "sentinel-incident-index-marker-7b1d4e";
  const rt::Bytes CAPTURE_BODY = rt::encodeUtf8(R"({"model":"gpt-5.6-sol","stream":false,"input":"sentinel-incident-index-marker-7b1d4e","user":"incident-index-test-user"})");

  const json::value& field(const json::value& value, std::string_view key) {
    const auto* member = rt::get(value, key);
    if (!member) throw rt::JsError("missing synthetic response field");
    return *member;
  }


  struct T3StaticAssets {
    std::vector<std::string> createdPaths;
    T3StaticAssets() {
      const auto temporary = uos::rt::env::get("TMPDIR");
      const auto directory = uos::rt::process::cwd();
      if (!temporary || !directory.starts_with(uos::rt::path::dirname(*temporary) + "/"))
        throw uos::rt::JsError("T3 static assets require the wrapper task workdir");
      const auto repository = uos::rt::path::resolve(uos::rt::path::dirname(__FILE__), "../../..");
      const std::vector<std::string> names{"static", "favicon.png", "favicon-32.png"};
      for (const auto& name : names) {
        const auto target = uos::rt::path::join(repository, name);
        const auto assetPath = uos::rt::path::join(directory, name);
        if (uos::rt::fs::existsSync(assetPath)) {
          if (uos::rt::fs::realpathSync(assetPath) != uos::rt::fs::realpathSync(target))
            throw uos::rt::JsError("T3 static asset fixture refuses an unexpected existing path");
        } else {
          uos::rt::fs::symlinkSync(target, assetPath);
          createdPaths.push_back(assetPath);
        }
      }
    }
    ~T3StaticAssets() {
      for (const auto& path : createdPaths) uos::rt::fs::rmSync(path);
    }
  };

  void prepareT3StaticAssets() {
    static T3StaticAssets assets;
  }

  struct IndexIngress {
    uos::tests::helpers::terminal_log_harness::Isolation isolation;
    InstalledStore installed;
    IndexIngress() {
      prepareT3StaticAssets();
      uos::config::config.adminTokens.insert(SUPER_ADMIN_TOKEN);
      uos::debug_routing::resetDebugRoutingCacheForTest();
    }
    ~IndexIngress() {
      uos::cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();
      uos::debug_routing::resetDebugRoutingCacheForTest();
      uos::config::config.adminTokens.erase(SUPER_ADMIN_TOKEN);
    }
  };

  rt::Request adminRequest(std::string url) {
    const rt::RequestInit init{.headers = rt::Headers{{"Authorization", "Bearer " + SUPER_ADMIN_TOKEN}}};
    return rt::Request(std::move(url), init);
  }

  std::string indexUrl(const std::vector<rt::URLSearchParams::Entry>& params = {}) {
    const std::string query = rt::URLSearchParams::fromRecord(params).toString();
    return "https://ai.ubq.fi/admin/sentinel/incidents" + (query.empty() ? "" : "?" + query);
  }

  std::string exportUrl(std::string query) { return "https://ai.ubq.fi/admin/sentinel/replay-captures?" + query; }

  rt::Request captureRequest(std::string token, std::string url = "https://ai.ubq.fi/v1/responses") {
    const rt::RequestInit init{.method = "POST", .headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"},
      {"x-codex-client-version", "9.9.9-secret-version"}}, .body = CAPTURE_BODY};
    return rt::Request(std::move(url), init);
  }

  void assertIndexRowShape(const json::value& row, double count) {
    EXPECT(rt::getString(row, "incident_id").has_value());
    EXPECT(isSentinelIncidentId(rt::KvValue(field(row, "incident_id"))));
    const rt::Regex hex64("^[0-9a-f]{64}$", "");
    EXPECT(hex64.test(rt::getString(row, "fingerprint").value_or("")));
    EXPECT_EQ(rt::getString(row, "severity"), std::optional<std::string_view>("P2"));
    EXPECT(rt::getNumber(row, "first_seen_at_ms").has_value());
    EXPECT(rt::getNumber(row, "last_seen_at_ms").has_value());
    EXPECT(rt::getNumber(row, "last_seen_at_ms").value_or(0) >= rt::getNumber(row, "first_seen_at_ms").value_or(1));
    EXPECT_EQ(rt::getNumber(row, "count"), std::optional<double>(count));
    if (!field(row, "failing_revision").is_null()) {
      const rt::Regex revision("^[0-9a-f]{40}$", "");
      EXPECT(revision.test(rt::getString(row, "failing_revision").value_or("")));
    }
    EXPECT(rt::getString(row, "error_type").has_value());
    EXPECT(!rt::getString(row, "error_type").value_or("").empty());
    const auto& context = field(row, "context");
    EXPECT(rt::getString(context, "message").has_value());
    EXPECT(field(context, "location").is_null());
    EXPECT(rt::deepEqual(field(context, "sample"), json::array{}));
    const auto& provenance = field(row, "provenance");
    EXPECT_EQ(rt::getString(provenance, "endpoint"), std::optional<std::string_view>("https://ai.ubq.fi/v1/responses"));
    EXPECT(rt::getNumber(provenance, "captured_at_ms").has_value());
    EXPECT(field(provenance, "captured_by").is_null());
    const auto& evidenceRef = field(row, "evidence_ref");
    if (!evidenceRef.is_null()) {
      EXPECT(rt::getString(evidenceRef, "ref").has_value());
      const std::string ref(rt::getString(evidenceRef, "ref").value_or(""));
      EXPECT(!ref.empty());
      const std::string prefix = "artifact://sentinel/" + std::string(rt::getString(row, "incident_id").value_or("")) + "/";
      EXPECT(rt::startsWith(ref, prefix));
      const rt::Regex captureId("^[A-Za-z0-9_-]{1,128}$", "");
      EXPECT(captureId.test(ref.substr(prefix.size())));
      EXPECT(!rt::startsWith(ref, "capture:"));
      if (!field(evidenceRef, "digest").is_null()) EXPECT(hex64.test(rt::getString(evidenceRef, "digest").value_or("")));
    }
    EXPECT_EQ(field(row, "evidence_expires_at_ms").is_null(), evidenceRef.is_null());
  }

  std::string exportedDigest(const uos::sentinel::replay_model::ExportedSentinelReplayCapture& capture) {
    rt::Bytes ciphertext;
    for (const auto& chunk : capture.chunks) {
      const auto part = uos::utils::base64UrlDecode(chunk);
      ciphertext.insert(ciphertext.end(), part.begin(), part.end());
    }
    return uos::utils::encodeHex(rt::crypto::subtle::digest("SHA-256", ciphertext));
  }
  rt::Task<void> seedAuthenticatedKey(std::shared_ptr<rt::Kv> kv, std::string token) {
    const double now = rt::dateNow();
    auto hashing = uos::utils::sha256Base64Url(token);
    const std::string hash = co_await std::move(hashing);
    const std::string id = "incident-index-" + hash.substr(0, 12);
    json::object policyBase{{"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 100}, {"usage_requests", 0},
      {"usage_reset_at_ms", now + 60'000}, {"window_ms", 60'000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", true},
      {"paid_fallback_limit_microcredits", 1'000'000}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0},
      {"paid_fallback_reservation_request_id", nullptr}};
    json::object hashed{{"id", id}};
    rt::objectAssign(hashed, policyBase);
    auto writingHash = kv->set(uos::api_keys::apiKeyHashKey(hash), rt::KvValue(hashed));
    co_await std::move(writingHash);
    json::object record{{"id", id}, {"name", "incident index test key"}, {"prefix", token.substr(0, 12)}, {"hash", hash}, {"created_at_ms", now}};
    rt::objectAssign(record, policyBase);
    record["paid_fallback_model_ids"] = json::array{"gpt-5.6-sol"};
    record["paid_fallback_quota_per_credit"] = 500'000;
    record["paid_fallback_max_exposure_microcredits"] = json::object{{"gpt-5.6-sol", 250'000}};
    record["paid_fallback_pricing_checked_at_ms"] = now;
    auto writingRecord = kv->set(uos::api_keys::apiKeyIdKey(id), rt::KvValue(record));
    co_await std::move(writingRecord);
  }


}


ASYNC_TEST("real handler failure flow: durable index, incident-filtered export, byte-exact decryption") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  const auto keyBytes = newSyntheticKey();
  const rt::env::ScopedOverride replayKey({{"SENTINEL_REPLAY_KEY", uos::utils::base64UrlEncode(keyBytes)}});
  const std::string token = uos::api_keys::generateApiKeyToken();
  auto seeding = seedAuthenticatedKey(kv, token);
  co_await std::move(seeding);
  const rt::KvValue scenario(json::object{{"scenario", "codex_429"}, {"expires_at_ms", nullptr}, {"updated_at_ms", rt::dateNow()}});
  auto writingScenario = kv->set(uos::debug_routing::DEBUG_ROUTING_KEY, scenario);
  co_await std::move(writingScenario);
  auto handling = uos::handler::index::handler(captureRequest(token, "https://ai.ubq.fi/v1/responses?incident_index_secret_query=1"));
  auto response = co_await std::move(handling);
  EXPECT(response.status() >= 400);
  const auto responseText = co_await response.text();
  EXPECT(!rt::includes(responseText, MARKER));
  auto indexing = uos::handler::index::handler(adminRequest(indexUrl()));
  auto indexResponse = co_await std::move(indexing);
  EXPECT_EQ(indexResponse.status(), 200);
  EXPECT_EQ(indexResponse.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
  const auto indexBody = co_await indexResponse.json();
  EXPECT(rt::deepEqual(field(indexBody, "coverage"), json::object{{"status", "complete"}}));
  EXPECT(field(indexBody, "cursor").is_null());
  const auto& rows = field(indexBody, "data").as_array();
  ASSERT_EQ(rows.size(), std::size_t{1});
  const auto& row = rows[0];
  assertIndexRowShape(row, 1);
  const auto indexText = rt::jsonStringify(indexBody);
  const std::vector<std::string> forbidden{MARKER, "incident_index_secret_query", "9.9.9-secret-version", "incident-index-test-user", "rate_limit_error", "chatgpt_codex"};
  for (const auto& secret : forbidden) EXPECT(!rt::includes(indexText, secret));
  EXPECT(!field(row, "evidence_ref").is_null());
  const std::string incidentId(rt::getString(row, "incident_id").value());
  const std::string interval = "after_ms=0&before_ms=" + rt::numberToString(rt::dateNow() + 1);
  auto filtering = uos::handler::index::handler(adminRequest(exportUrl(interval + "&incident_id=" + incidentId)));
  auto filtered = co_await std::move(filtering);
  EXPECT_EQ(filtered.status(), 200);
  const auto filteredBody = co_await filtered.json();
  const auto& captures = field(filteredBody, "data").as_array();
  ASSERT_EQ(captures.size(), std::size_t{1});
  const auto capture = json::value_to<uos::sentinel::replay_model::ExportedSentinelReplayCapture>(captures[0]);
  const std::string wireRef = "artifact://sentinel/" + incidentId + "/" + capture.manifest.capture_id;
  EXPECT_EQ(rt::getString(field(row, "evidence_ref"), "ref"), std::optional<std::string_view>(wireRef));
  auto readingInternal = listedRows(kv, incidentId, 1);
  const auto internalRows = co_await std::move(readingInternal);
  ASSERT_EQ(internalRows.size(), std::size_t{1});
  EXPECT_EQ(evidenceRefOf(internalRows[0]), std::optional<std::string>("capture:" + capture.manifest.capture_id));
  const std::string digest = exportedDigest(capture);
  EXPECT_EQ(rt::getString(field(row, "evidence_ref"), "digest"), std::optional<std::string_view>(digest));
  EXPECT_EQ(rt::getNumber(row, "evidence_expires_at_ms"), std::optional<double>(capture.manifest.expires_at_ms));
  auto decrypting = uos::sentinel::replay_read::decryptExportedSentinelReplay(capture, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, CAPTURE_BODY);
  EXPECT_EQ(rt::decodeUtf8(*plaintext.body), rt::decodeUtf8(CAPTURE_BODY));
  EXPECT_EQ(plaintext.observation.status, static_cast<double>(response.status()));
  EXPECT_EQ(plaintext.client_observation.terminal_type, std::optional<std::string>("http.error"));
  auto unfiltering = uos::handler::index::handler(adminRequest(exportUrl(interval)));
  auto unfiltered = co_await std::move(unfiltering);
  EXPECT_EQ(unfiltered.status(), 200);
  const auto unfilteredBody = co_await unfiltered.json();
  const auto& unfilteredCaptures = field(unfilteredBody, "data").as_array();
  ASSERT_EQ(unfilteredCaptures.size(), std::size_t{1});
  EXPECT(rt::deepEqual(field(unfilteredCaptures[0], "manifest"), json::value_from(capture.manifest)));
}

ASYNC_TEST("duplicate observations keep one stable incident id, exact digest, and original expiry") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = newSyntheticKey();
  const double now1 = rt::dateNow() - 60'000;
  const double now2 = now1 + 60'000;
  SentinelIncidentDependencies indexDependencies;
  indexDependencies.randomUuid = [] { return std::string("00000000-0000-4000-8000-000000000001"); };
  const AcceptedSentinelReplayInput input = syntheticInput(encoded(R"({"model":"gpt-5.6-sol","input":"dup"})"), "incident-index-duplicate");
  const SentinelFailureObservation observation = failureObservation(502);
  const SentinelClientFailureObservation clientObservation = clientFailureObservationFor(502);

  auto recordingFirst = recordSentinelIncidentIndexObservation(*kv, observationInput(input, input.git_sha, now1, 502), indexDependencies);
  const rt::KvEntry<SentinelIncidentIndexRow> first = co_await std::move(recordingFirst);
  auto recordingSecond = recordSentinelIncidentIndexObservation(*kv, observationInput(input, input.git_sha, now2, 502), indexDependencies);
  const rt::KvEntry<SentinelIncidentIndexRow> second = co_await std::move(recordingSecond);
  EXPECT_EQ(second.value.incident_id, first.value.incident_id);  // a stable group must keep one incident id
  EXPECT_EQ(second.value.count, 2.0);
  EXPECT_EQ(second.value.first_seen_at_ms, now1);
  EXPECT_EQ(second.value.last_seen_at_ms, now2);

  auto persistingStored = persistEncryptedSentinelReplay(input, observation, dependenciesFor(kv, keyBytes, now1, "duplicate-capture-one"), clientObservation);
  const SentinelReplayPersistResult storedResult = co_await std::move(persistingStored);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(storedResult));
  const SentinelReplayPersistResultStored& stored = std::get<SentinelReplayPersistResultStored>(storedResult);

  const double originalExpiry = stored.manifest.expires_at_ms;

  auto persistingDuplicate = persistEncryptedSentinelReplay(input, observation, dependenciesFor(kv, keyBytes, now2, "duplicate-capture-two"), clientObservation);
  const SentinelReplayPersistResult duplicateResult = co_await std::move(persistingDuplicate);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultDuplicate>(duplicateResult));
  const SentinelReplayPersistResultDuplicate& duplicate = std::get<SentinelReplayPersistResultDuplicate>(duplicateResult);

  EXPECT_EQ(duplicate.manifest_key, stored.manifest_key);  // a duplicate must reference the winning manifest

  auto listingManifests = listedKeys(*kv, SENTINEL_REPLAY_MANIFEST_PREFIX);
  const std::vector<rt::KvKey> manifestKeys = co_await std::move(listingManifests);
  EXPECT_EQ(manifestKeys.size(), std::size_t{1});  // duplicate observations must not create new evidence
  auto listingReferences = listedKeys(*kv, SENTINEL_INCIDENT_CAPTURE_REF_PREFIX);
  const std::vector<rt::KvKey> referenceKeys = co_await std::move(listingReferences);
  EXPECT_EQ(referenceKeys.size(), std::size_t{1});  // one capture ref per stable incident

  auto listingRows = listedRows(kv, first.value.incident_id, 1);
  const std::vector<SentinelIncidentIndexRow> rows = co_await std::move(listingRows);
  ASSERT_EQ(rows.size(), std::size_t{1});
  const SentinelIncidentIndexRow& row = rows[0];
  EXPECT_EQ(row.count, 2.0);  // each actual observation is counted exactly once
  EXPECT_EQ(evidenceRefOf(row), std::optional<std::string>("capture:duplicate-capture-one"));
  EXPECT_EQ(row.evidence_expires_at_ms, std::optional<double>(originalExpiry));  // a duplicate must retain the original expiry

  // Capture deletion (manifest + chunks + ref) keeps the durable index row
  // with its evidence metadata so the consumer can report the expiry.
  ASSERT(stored.manifest_key.has_value());  // a stored capture must expose its manifest key
  auto deletingManifest = kv->delete_(*stored.manifest_key);
  co_await std::move(deletingManifest);
  for (double index = 0; index < stored.manifest.chunk_count; index += 1) {
    auto deletingChunk = kv->delete_(rt::KvKey{"uos_ai", "sentinel_replay", "v1", "chunk", stored.manifest.capture_id, index});
    co_await std::move(deletingChunk);
  }
  rt::KvListSelector referenceSelector;
  referenceSelector.prefix = SENTINEL_INCIDENT_CAPTURE_REF_PREFIX;
  const std::unique_ptr<rt::KvListIterator> references = kv->list(referenceSelector);
  for (;;) {
    const std::optional<rt::KvEntry<>> entry = co_await references->next();
    if (!entry) break;
    auto deletingReference = kv->delete_(entry->key);
    co_await std::move(deletingReference);
  }
  auto listingAfterDelete = listedRows(kv, first.value.incident_id, 1);
  const std::vector<SentinelIncidentIndexRow> afterDelete = co_await std::move(listingAfterDelete);
  ASSERT_EQ(afterDelete.size(), std::size_t{1});  // durable discovery must survive capture deletion
  EXPECT_EQ(evidenceRefOf(afterDelete[0]), std::optional<std::string>("capture:duplicate-capture-one"));
  EXPECT_EQ(afterDelete[0].evidence_expires_at_ms, std::optional<double>(originalExpiry));
}

ASYNC_TEST("durable index survives a clock beyond the capture lifetime with expired evidence metadata") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = newSyntheticKey();
  const double capturedAt = rt::dateNow() - 49.0 * 60 * 60 * 1'000;
  SentinelIncidentDependencies indexDependencies;
  indexDependencies.randomUuid = [] { return std::string("00000000-0000-4000-8000-000000000002"); };
  const SentinelClientFailureObservation clientObservation = clientFailureObservationFor(503);
  AcceptedSentinelReplayInput input = syntheticInput(encoded(R"({"input":"expiry"})"), "incident-index-expiry");
  input.git_sha = "1234567890abcdef1234567890abcdef12345678";
  auto recording = recordSentinelIncidentIndexObservation(*kv, observationInput(input, "1234567890abcdef1234567890abcdef12345678", capturedAt, 503), indexDependencies);
  co_await std::move(recording);
  auto persisting = persistEncryptedSentinelReplay(input, failureObservation(503), dependenciesFor(kv, keyBytes, capturedAt, "expired-capture"), clientObservation);
  const SentinelReplayPersistResult stored = co_await std::move(persisting);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(stored));

  auto listing = listedRows(kv, std::nullopt, 20);
  const std::vector<SentinelIncidentIndexRow> rows = co_await std::move(listing);
  ASSERT_EQ(rows.size(), std::size_t{1});
  const SentinelIncidentIndexRow& row = rows[0];
  EXPECT_EQ(evidenceRefOf(row), std::optional<std::string>("capture:expired-capture"));
  EXPECT(row.evidence_expires_at_ms.value_or(0) < rt::dateNow());  // expired evidence must not claim validity
  EXPECT_EQ(row.failing_revision, std::optional<std::string>("1234567890abcdef1234567890abcdef12345678"));
}


ASYNC_TEST("real handler with a missing capture key leaves a discoverable row without evidence") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  const std::string token = uos::api_keys::generateApiKeyToken();
  auto seeding = seedAuthenticatedKey(kv, token);
  co_await std::move(seeding);
  const rt::KvValue scenario(json::object{{"scenario", "codex_429"}, {"expires_at_ms", nullptr}, {"updated_at_ms", rt::dateNow()}});
  auto writingScenario = kv->set(uos::debug_routing::DEBUG_ROUTING_KEY, scenario);
  co_await std::move(writingScenario);
  const rt::env::ScopedOverride replayKey({{"SENTINEL_REPLAY_KEY", std::nullopt}});
  auto handling = uos::handler::index::handler(captureRequest(token));
  const auto response = co_await std::move(handling);
  EXPECT(response.status() >= 400);
  auto indexing = uos::handler::index::handler(adminRequest(indexUrl()));
  auto indexResponse = co_await std::move(indexing);
  EXPECT_EQ(indexResponse.status(), 200);
  const auto body = co_await indexResponse.json();
  const auto& rows = field(body, "data").as_array();
  ASSERT_EQ(rows.size(), std::size_t{1});
  EXPECT(field(rows[0], "evidence_ref").is_null());
  EXPECT(field(rows[0], "evidence_expires_at_ms").is_null());
  const std::string incidentId(rt::getString(rows[0], "incident_id").value());
  const std::string interval = "after_ms=0&before_ms=" + rt::numberToString(rt::dateNow() + 1) + "&incident_id=" + incidentId;
  auto filtering = uos::handler::index::handler(adminRequest(exportUrl(interval)));
  auto filtered = co_await std::move(filtering);
  EXPECT_EQ(filtered.status(), 200);
  const auto filteredBody = co_await filtered.json();
  EXPECT(rt::deepEqual(field(filteredBody, "data"), json::array{}));
}

ASYNC_TEST("a duplicate binds the exact original capture revision, timestamp, digest, and expiry") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = newSyntheticKey();
  const double now1 = 1'700'000'000'000;
  const double now2 = now1 + 60'000;
  const double now3 = now2 + 60'000;
  const std::string shaA = rt::repeat("a", 40);
  const std::string shaB = rt::repeat("b", 40);
  const std::string shaC = rt::repeat("c", 40);
  const SentinelFailureObservation observation = failureObservation(502);
  const SentinelClientFailureObservation clientObservation = clientFailureObservationFor(502);
  const AcceptedSentinelReplayInput inputA = syntheticInput(encoded(R"({"input":"original-a"})"), "revision-request-a");
  const AcceptedSentinelReplayInput inputB = syntheticInput(encoded(R"({"input":"newer-b"})"), "revision-request-b");

  auto recordingA = recordSentinelIncidentIndexObservation(*kv, observationInput(inputA, shaA, now1, 502));
  co_await std::move(recordingA);
  AcceptedSentinelReplayInput revisionA = inputA;
  revisionA.git_sha = shaA;
  auto persistingA = persistEncryptedSentinelReplay(revisionA, observation, dependenciesFor(kv, keyBytes, now1, "revision-capture-a"), clientObservation);
  const SentinelReplayPersistResult resultA = co_await std::move(persistingA);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(resultA));
  const SentinelReplayPersistResultStored& storedA = std::get<SentinelReplayPersistResultStored>(resultA);

  auto recordingB = recordSentinelIncidentIndexObservation(*kv, observationInput(inputB, shaB, now2, 502));
  co_await std::move(recordingB);
  AcceptedSentinelReplayInput revisionB = inputB;
  revisionB.git_sha = shaB;
  auto persistingB = persistEncryptedSentinelReplay(revisionB, observation, dependenciesFor(kv, keyBytes, now2, "revision-capture-b"), clientObservation);
  const SentinelReplayPersistResult resultB = co_await std::move(persistingB);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(resultB));

  auto listingAfterB = listedRows(kv, std::nullopt, 1);
  const std::vector<SentinelIncidentIndexRow> rowsAfterB = co_await std::move(listingAfterB);
  ASSERT(!rowsAfterB.empty());
  const SentinelIncidentIndexRow& afterB = rowsAfterB[0];
  EXPECT_EQ(afterB.failing_revision, std::optional<std::string>(shaB));  // freshly captured evidence carries its own revision
  EXPECT_EQ(afterB.provenance.captured_at_ms, now2);  // fresh binding provenance is the capture timestamp

  // Same body as A but a newer request revision: the capture must remain a
  // duplicate of A, so the index keeps A's exact revision/provenance and
  // never the newer B revision or the duplicate request's git_sha. The
  // observation is recorded first exactly like the environment producer.
  auto recordingC = recordSentinelIncidentIndexObservation(*kv, observationInput(inputA, shaC, now3, 502));
  co_await std::move(recordingC);
  AcceptedSentinelReplayInput revisionC = inputA;
  revisionC.git_sha = shaC;
  auto persistingDuplicate = persistEncryptedSentinelReplay(revisionC, observation, dependenciesFor(kv, keyBytes, now3, "revision-capture-c"), clientObservation);
  const SentinelReplayPersistResult duplicateResult = co_await std::move(persistingDuplicate);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultDuplicate>(duplicateResult));
  const SentinelReplayPersistResultDuplicate& duplicateA = std::get<SentinelReplayPersistResultDuplicate>(duplicateResult);

  EXPECT_EQ(duplicateA.manifest_key, storedA.manifest_key);  // duplicate must reference the winning A manifest

  auto listingRow = listedRows(kv, std::nullopt, 1);
  const std::vector<SentinelIncidentIndexRow> rows = co_await std::move(listingRow);
  ASSERT(!rows.empty());
  const SentinelIncidentIndexRow& row = rows[0];
  EXPECT_EQ(row.count, 3.0);  // observation history still counts every observation
  EXPECT_EQ(row.first_seen_at_ms, now1);
  EXPECT_EQ(row.last_seen_at_ms, now3);
  EXPECT_EQ(evidenceRefOf(row), std::optional<std::string>("capture:revision-capture-a"));  // duplicate remains bound to the original capture
  EXPECT_EQ(row.failing_revision, std::optional<std::string>(shaA));  // the older duplicate must bind the referenced capture's exact revision
  EXPECT_EQ(row.provenance.captured_at_ms, now1);  // index provenance must match the referenced capture timestamp
  EXPECT_EQ(row.evidence_expires_at_ms, std::optional<double>(storedA.manifest.expires_at_ms));  // a duplicate must never renew evidence

  std::vector<rt::Bytes> parts;
  for (double index = 0; index < storedA.manifest.chunk_count; index += 1) {
    auto reading = kv->get(rt::KvKey{"uos_ai", "sentinel_replay", "v1", "chunk", "revision-capture-a", index});
    const rt::KvEntryMaybe<> chunk = co_await std::move(reading);
    ASSERT(chunk.value.isBytes());  // capture A chunk must still exist
    parts.push_back(chunk.value.asBytes());
  }
  rt::Bytes merged;
  for (const rt::Bytes& part : parts) merged.insert(merged.end(), part.begin(), part.end());
  const rt::Bytes digest = rt::crypto::subtle::digest("SHA-256", merged);
  co_await rt::microtask();
  const std::string digestA = uos::utils::encodeHex(digest);
  ASSERT(row.evidence_ref.has_value());
  EXPECT_EQ(row.evidence_ref->digest, std::optional<std::string>(digestA));  // duplicate digest must be capture A's exact ciphertext digest

  // Missing winning evidence fails closed and never erases useful evidence.
  ASSERT(storedA.manifest_key.has_value());  // a stored capture must expose its manifest key
  auto deletingManifest = kv->delete_(*storedA.manifest_key);
  co_await std::move(deletingManifest);
  auto persistingMissing = persistEncryptedSentinelReplay(revisionC, observation, dependenciesFor(kv, keyBytes, now3, "revision-capture-d"), clientObservation);
  auto rejecting = rejectionText(std::move(persistingMissing));
  const std::optional<std::string> rejection = co_await std::move(rejecting);
  static const rt::Regex MANIFEST_UNAVAILABLE("Sentinel incident replay manifest is unavailable", "");
  EXPECT(rejection.has_value() && MANIFEST_UNAVAILABLE.test(*rejection));
  auto listingPreserved = listedRows(kv, std::nullopt, 1);
  const std::vector<SentinelIncidentIndexRow> preserved = co_await std::move(listingPreserved);
  ASSERT(!preserved.empty());
  EXPECT_EQ(evidenceRefOf(preserved[0]), std::optional<std::string>("capture:revision-capture-a"));  // a failed duplicate must not erase useful existing evidence
}

ASYNC_TEST("direct duplicate persistence without a durable index row still returns duplicate and binds nothing") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = newSyntheticKey();
  const double now1 = 1'700'000'000'000;
  const double now2 = now1 + 60'000;
  const AcceptedSentinelReplayInput input = syntheticInput(encoded(R"({"input":"no-index-direct-caller"})"), "no-index-direct-request");
  const SentinelFailureObservation observation = failureObservation(502);
  const SentinelClientFailureObservation clientObservation = clientFailureObservationFor(502);
  auto persistingStored = persistEncryptedSentinelReplay(input, observation, dependenciesFor(kv, keyBytes, now1, "no-index-capture"), clientObservation);
  const SentinelReplayPersistResult storedResult = co_await std::move(persistingStored);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(storedResult));
  const SentinelReplayPersistResultStored& stored = std::get<SentinelReplayPersistResultStored>(storedResult);

  auto persistingDuplicate = persistEncryptedSentinelReplay(input, observation, dependenciesFor(kv, keyBytes, now2, "no-index-capture-two"), clientObservation);
  const SentinelReplayPersistResult duplicateResult = co_await std::move(persistingDuplicate);
  // The optional passive index must not make a direct duplicate fail.
  ASSERT(std::holds_alternative<SentinelReplayPersistResultDuplicate>(duplicateResult));
  EXPECT_EQ(std::get<SentinelReplayPersistResultDuplicate>(duplicateResult).manifest_key, stored.manifest_key);

  auto listingIndex = listedKeys(*kv, SENTINEL_INCIDENT_INDEX_PREFIX);
  const std::vector<rt::KvKey> indexKeys = co_await std::move(listingIndex);
  EXPECT_EQ(indexKeys.size(), std::size_t{0});  // the low-level encryptor must never create an implicit index row
  auto listingReferences = listedKeys(*kv, SENTINEL_INCIDENT_CAPTURE_REF_PREFIX);
  const std::vector<rt::KvKey> referenceKeys = co_await std::move(listingReferences);
  EXPECT_EQ(referenceKeys.size(), std::size_t{0});  // no capture ref may be written without a recorded index observation

  // With no index row the optional passive integration stays absent even
  // when evidence is gone: duplicates still report duplicate instead of a
  // storage error, because nothing was ever observed to attach to.
  ASSERT(stored.manifest_key.has_value());  // a stored capture must expose its manifest key
  auto deletingManifest = kv->delete_(*stored.manifest_key);
  co_await std::move(deletingManifest);
  auto persistingAfterDelete = persistEncryptedSentinelReplay(input, observation, dependenciesFor(kv, keyBytes, now2, "no-index-capture-three"), clientObservation);
  const SentinelReplayPersistResult afterDelete = co_await std::move(persistingAfterDelete);
  EXPECT(std::holds_alternative<SentinelReplayPersistResultDuplicate>(afterDelete));
}

ASYNC_TEST("index evidence binding CAS-checks the winning manifest versionstamp") {
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  kvfacade::setKvForTest(kv);
  const SentinelIncidentIndexObservationInput input{.endpoint = "/v1/responses", .method = "POST", .gitSha = rt::repeat("aa", 20),
                                                    .observedAtMs = 1'700'000'000'000, .observation = clientObservationFor(500)};
  const rt::KvEntry<SentinelIncidentIndexRow> observed = co_await recordSentinelIncidentIndexObservation(*kv, input);
  const rt::KvKey manifestKey{SENTINEL_REPLAY_MANIFEST_PREFIX, 1'700'000'000'000, rt::repeat("ff", 32), "cas-capture"};
  const rt::KvValue original = rt::KvValue::object({{"version", rt::KvValue(1)}, {"signature", rt::KvValue("original")}});
  co_await kv->set(manifestKey, original);
  const rt::KvEntryMaybe<> first = co_await kv->get(manifestKey);
  const rt::KvValue replaced = rt::KvValue::object({{"version", rt::KvValue(1)}, {"signature", rt::KvValue("replaced")}});
  co_await kv->set(manifestKey, replaced);
  const SentinelIncidentIndexEvidenceBinding binding{
    .observedAtMs = 1'700'000'000'000,
    .captureId = "cas-capture",
    .gitSha = rt::repeat("aa", 20),
    .referenceFingerprint = rt::repeat("ff", 32),
    .manifestKey = manifestKey,
    .manifestVersionstamp = first.versionstamp,
    .capturedAtMs = 1'700'000'000'000,
    .digest = rt::repeat("cd", 32),
    .expiresAtMs = 1'700'000'000'000 + 48 * 60 * 60 * 1'000,
  };
  EXPECT_REJECTS_MESSAGE(bindSentinelIncidentIndexEvidence(*kv, observed.value.fingerprint, binding), rt::JsError,
                         "Sentinel incident index evidence binding conflicted repeatedly");
  SentinelIncidentIndexListOptions options;
  options.incidentId = observed.value.incident_id;
  options.limit = 1;
  const SentinelIncidentIndexPage page = co_await listSentinelIncidentIndexRows(*kv, options);
  ASSERT_EQ(page.rows.size(), std::size_t{1});
  const SentinelIncidentIndexRow& after = page.rows[0];
  // A replaced manifest must never silently attach evidence.
  EXPECT(!after.evidence_ref.has_value());
  EXPECT(!after.evidence_expires_at_ms.has_value());
  // Observation provenance is untouched before a successful binding.
  EXPECT_EQ(after.provenance.captured_at_ms, 1'700'000'000'000.0);
  kv->close();
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("concurrent observations converge to one group row with one count per observation") {
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  kvfacade::setKvForTest(kv);
  SentinelIncidentDependencies dependencies;
  dependencies.randomUuid = [] { return rt::crypto::randomUUID(); };
  std::vector<rt::Promise<rt::KvEntry<SentinelIncidentIndexRow>>> observations;
  for (int index = 0; index < 6; index += 1) {
    SentinelIncidentIndexObservation observation = clientObservationFor(529);
    observation.terminal_type = "error";
    SentinelIncidentIndexObservationInput input{.endpoint = "/v1/responses", .method = "POST", .gitSha = "abcdef0123456789abcdef0123456789abcdef01",
                                                .observedAtMs = 1'700'000'000'000.0 + index, .observation = observation};
    observations.push_back(rt::Promise(recordSentinelIncidentIndexObservation(*kv, std::move(input), dependencies)));
  }
  auto all = rt::all(std::move(observations));
  co_await all.wait();
  SentinelIncidentIndexListOptions options;
  options.limit = 20;
  const SentinelIncidentIndexPage page = co_await listSentinelIncidentIndexRows(*kv, options);
  // One stable failure group must yield exactly one row.
  ASSERT_EQ(page.rows.size(), std::size_t{1});
  // Each actual observation must count exactly once.
  EXPECT_EQ(page.rows[0].count, 6.0);
  EXPECT_EQ(page.rows[0].first_seen_at_ms, 1'700'000'000'000.0);
  EXPECT_EQ(page.rows[0].last_seen_at_ms, 1'700'000'000'005.0);
  // Observations alone must never create orphan capture references.
  const std::vector<rt::KvKey> referenceKeys = co_await listedKeys(*kv, SENTINEL_INCIDENT_CAPTURE_REF_PREFIX);
  EXPECT_EQ(referenceKeys.size(), std::size_t{0});
  const std::vector<rt::KvKey> manifestKeys = co_await listedKeys(*kv, SENTINEL_REPLAY_MANIFEST_PREFIX);
  EXPECT_EQ(manifestKeys.size(), std::size_t{0});
  kv->close();
  kvfacade::setKvForTest(nullptr);
}


ASYNC_TEST("incident index requires admin authorization and then reaches storage") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  const rt::Request anonymousRequest(indexUrl());
  auto anonymousTask = uos::handler::index::handler(anonymousRequest);
  auto anonymous = co_await std::move(anonymousTask);
  EXPECT_EQ(anonymous.status(), 401);
  const auto payload = co_await anonymous.json();
  EXPECT_EQ(rt::getString(field(payload, "error"), "code"), std::optional<std::string_view>("invalid_api_key"));
  const double now = rt::dateNow();
  const std::string userId = "incident-index-passkey-user";
  const std::string handle = "incident-index-passkey-handle";
  const std::string sessionToken = "incident-index-passkey-session-token";
  const rt::KvValue user(json::object{{"id", userId}, {"handle", handle}, {"is_admin", true},
    {"credential_ids", json::array{"incident-index-credential"}}, {"created_at_ms", now}, {"updated_at_ms", now}});
  auto writingUser = kv->set(uos::auth::passkeys::passkeyUserKey(userId), user);
  co_await std::move(writingUser);
  auto writingHandle = kv->set(uos::auth::passkeys::passkeyHandleKey(handle), rt::KvValue(userId));
  co_await std::move(writingHandle);
  const rt::KvValue session(json::object{{"token", sessionToken}, {"user_id", userId}, {"created_at_ms", now}, {"expires_at_ms", now + 3'600'000}});
  auto writingSession = kv->set(uos::auth::passkeys::passkeySessionKey(sessionToken), session);
  co_await std::move(writingSession);
  const rt::RequestInit init{.headers = rt::Headers{{"Cookie", std::string(uos::auth::passkeys::PASSKEY_RELAY_COOKIE_NAME) + "=" + rt::encodeURIComponent(sessionToken)}}};
  const rt::Request request(indexUrl(), init);
  auto handling = uos::handler::index::handler(request);
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  const auto adminPayload = co_await response.json();
  EXPECT(field(adminPayload, "data").is_array());
}

ASYNC_TEST("incident index pagination is bounded and an incident filter never silently misses a row") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  std::vector<std::string> incidentIds;
  SentinelIncidentDependencies dependencies;
  dependencies.randomUuid = [] { return rt::crypto::randomUUID(); };
  for (int index = 0; index < 25; ++index) {
    auto observation = clientObservationFor(400 + index);
    observation.terminal_type = index % 2 == 0 ? std::optional<std::string>("error") : std::nullopt;
    const SentinelIncidentIndexObservationInput input{.endpoint = index == 24 ? "/v1/chat/completions" : "/v1/responses", .method = "POST", .gitSha = "",
      .observedAtMs = 1'700'000'000'000.0 + index, .observation = observation};
    auto recording = recordSentinelIncidentIndexObservation(*kv, input, dependencies);
    const auto entry = co_await std::move(recording);
    incidentIds.push_back(entry.value.incident_id);
  }
  auto firstTask = uos::handler::index::handler(adminRequest(indexUrl({{"limit", "20"}})));
  auto pageOne = co_await std::move(firstTask);
  EXPECT_EQ(pageOne.status(), 200);
  const auto firstBody = co_await pageOne.json();
  EXPECT_EQ(field(firstBody, "data").as_array().size(), std::size_t{20});
  EXPECT(rt::getString(firstBody, "cursor").has_value());
  const auto nextCursor = rt::getString(firstBody, "cursor");
  ASSERT(nextCursor && !nextCursor->empty());
  auto secondTask = uos::handler::index::handler(adminRequest(indexUrl({{"limit", "20"}, {"cursor", std::string(*nextCursor)}})));
  auto pageTwo = co_await std::move(secondTask);
  const auto secondBody = co_await pageTwo.json();
  EXPECT_EQ(field(secondBody, "data").as_array().size(), std::size_t{5});
  EXPECT(field(secondBody, "cursor").is_null());
  const std::string target = incidentIds[24];
  std::optional<std::string> cursor;
  bool found = false;
  for (int page = 0; page < 64; ++page) {
    std::vector<rt::URLSearchParams::Entry> params{{"limit", "1"}, {"incident_id", target}};
    if (cursor) params.emplace_back("cursor", *cursor);
    auto task = uos::handler::index::handler(adminRequest(indexUrl(params)));
    auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), 200);
    const auto body = co_await response.json();
    const auto& rows = field(body, "data").as_array();
    if (std::any_of(rows.begin(), rows.end(), [&target](const auto& row) { return rt::getString(row, "incident_id") == target; })) {
      found = true;
      break;
    }
    const auto next = rt::getString(body, "cursor");
    cursor = next ? std::optional<std::string>(std::string(*next)) : std::nullopt;
    if (!cursor) break;
  }
  EXPECT(found);
}

ASYNC_TEST("incident index rejects invalid inputs and fails closed on corrupt storage") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  const std::vector<std::string> limits{"1", "20", "21", "100"};
  for (const auto& limit : limits) {
    auto task = uos::handler::index::handler(adminRequest(indexUrl() + "?limit=" + limit));
    auto accepted = co_await std::move(task);
    EXPECT_EQ(accepted.status(), 200);
    if (auto body = accepted.body()) co_await body->cancel();
  }
  const std::vector<std::string> queries{"limit=0", "limit=101", "limit=", "limit=abc", "limit=1.5", "limit=1e1", "limit=1.0", "cursor=", "cursor=!!",
    "cursor=" + rt::repeat("a", 3'000), "incident_id=", "incident_id=not-an-incident-id", "unexpected=1", "limit=1&limit=2", "cursor=cursor-1&cursor=cursor-2"};
  for (const auto& query : queries) {
    auto task = uos::handler::index::handler(adminRequest(indexUrl() + "?" + query));
    auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), 400);
    if (auto body = response.body()) co_await body->cancel();
  }
  const rt::KvKey key{SENTINEL_INCIDENT_INDEX_PREFIX, rt::repeat("f", 64)};
  const rt::KvValue corruptRow(json::object{{"version", 999}, {"corrupted", true}});
  auto writing = kv->set(key, corruptRow);
  co_await std::move(writing);
  auto task = uos::handler::index::handler(adminRequest(indexUrl()));
  auto corrupt = co_await std::move(task);
  EXPECT_EQ(corrupt.status(), 503);
  const auto payload = co_await corrupt.json();
  EXPECT_EQ(rt::getString(field(payload, "error"), "code"), std::optional<std::string_view>("sentinel_incidents_unavailable"));
  EXPECT(!rt::hasOwn(payload.as_object(), "data"));
}

ASYNC_TEST("index rows never carry internal KV keys or metadata into wire output") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  const SentinelIncidentIndexObservationInput input{.endpoint = "/v1/responses?internal_query=zzz", .method = "POST", .gitSha = "",
    .observedAtMs = rt::dateNow(), .observation = clientObservationFor(500)};
  auto recording = recordSentinelIncidentIndexObservation(*kv, input);
  co_await std::move(recording);
  auto task = uos::handler::index::handler(adminRequest(indexUrl()));
  auto response = co_await std::move(task);
  EXPECT_EQ(response.status(), 200);
  const auto text = co_await response.text();
  EXPECT(!rt::includes(text, "internal_query"));
  EXPECT(!rt::includes(text, "uos_ai"));
  EXPECT(!rt::includes(text, "sentinel_replay"));
}

ASYNC_TEST("wire provenance is a fixed canonical allowlist: known/other classifications, hostile request, query, and index origins cannot alter the emitted URL") {
  const IndexIngress fixture;
  const auto kv = fixture.installed.kv;
  const auto keyBytes = newSyntheticKey();
  const double observedAtMs = 1'700'000'200'000;
  const double capturedAtMs = 1'700'000'300'000;
  struct Case { std::string endpoint; double status; std::string expected; };
  const std::vector<Case> cases{{"/v1/responses", 500, "https://ai.ubq.fi/v1/responses"}, {"/v1/chat/completions", 501, "https://ai.ubq.fi/v1/chat/completions"},
    {"/v1/images/generations", 502, "https://ai.ubq.fi/"}, {"https://evil.example/v1/responses", 503, "https://ai.ubq.fi/"},
    {"/v1/responses?origin=https://evil.example", 504, "https://ai.ubq.fi/v1/responses"}};
  SentinelIncidentDependencies dependencies;
  dependencies.randomUuid = [] { return rt::crypto::randomUUID(); };
  const SentinelIncidentIndexObservationInput firstInput{.endpoint = cases[0].endpoint, .method = "POST", .gitSha = "", .observedAtMs = observedAtMs,
    .observation = clientObservationFor(cases[0].status)};
  auto recordingFirst = recordSentinelIncidentIndexObservation(*kv, firstInput, dependencies);
  const auto first = co_await std::move(recordingFirst);
  for (std::size_t index = 1; index < cases.size(); ++index) {
    const auto& item = cases[index];
    const SentinelIncidentIndexObservationInput input{.endpoint = item.endpoint, .method = "POST", .gitSha = "", .observedAtMs = observedAtMs + item.status,
      .observation = clientObservationFor(item.status)};
    auto recording = recordSentinelIncidentIndexObservation(*kv, input, dependencies);
    co_await std::move(recording);
  }
  const auto input = syntheticInput(encoded(R"({"input":"fixed-allowlist"})"), "fixed-allowlist-request", "/v1/responses");
  auto persisting = persistEncryptedSentinelReplay(input, failureObservation(cases[0].status), dependenciesFor(kv, keyBytes, capturedAtMs, "fixed-allowlist-capture"),
    clientFailureObservationFor(cases[0].status));
  const auto storedResult = co_await std::move(persisting);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(storedResult));
  const auto stored = std::get<SentinelReplayPersistResultStored>(storedResult);
  auto task = uos::handler::index::handler(adminRequest("https://evil.example/admin/sentinel/incidents"));
  auto response = co_await std::move(task);
  EXPECT_EQ(response.status(), 200);
  const auto body = co_await response.json();
  const auto& wireRows = field(body, "data").as_array();
  EXPECT_EQ(wireRows.size(), cases.size());
  EXPECT(!rt::includes(rt::jsonStringify(body), "evil.example"));
  std::vector<std::string> actualEndpoints;
  for (const auto& row : wireRows) actualEndpoints.emplace_back(rt::getString(field(row, "provenance"), "endpoint").value_or(""));
  std::vector<std::string> expectedEndpoints;
  for (const auto& item : cases) expectedEndpoints.push_back(item.expected);
  std::sort(actualEndpoints.begin(), actualEndpoints.end());
  std::sort(expectedEndpoints.begin(), expectedEndpoints.end());
  EXPECT_EQ(actualEndpoints, expectedEndpoints);
  auto hostileTask = uos::handler::index::handler(adminRequest("https://ai.ubq.fi/admin/sentinel/incidents?origin=https://evil.example"));
  const auto hostile = co_await std::move(hostileTask);
  EXPECT_EQ(hostile.status(), 400);
  auto listing = listedRows(kv, std::nullopt, 100);
  const auto storedRows = co_await std::move(listing);
  EXPECT_EQ(storedRows.size(), cases.size());
  for (const auto& row : storedRows) {
    const auto wire = std::find_if(wireRows.begin(), wireRows.end(), [&row](const auto& value) { return rt::getString(value, "incident_id") == row.incident_id; });
    ASSERT(wire != wireRows.end());
    const auto& provenance = field(*wire, "provenance");
    EXPECT_EQ(rt::getNumber(provenance, "captured_at_ms"), std::optional<double>(row.provenance.captured_at_ms));
    EXPECT(rt::deepEqual(field(provenance, "captured_by"), json::value(row.provenance.captured_by)));
    const auto& wireEvidence = field(*wire, "evidence_ref");
    EXPECT_EQ(wireEvidence.is_null(), !row.evidence_ref.has_value());
    if (!wireEvidence.is_null() && row.evidence_ref) {
      const std::string wireRef = "artifact://sentinel/" + row.incident_id + "/" + row.evidence_ref->ref.substr(std::string("capture:").size());
      EXPECT_EQ(rt::getString(wireEvidence, "ref"), std::optional<std::string_view>(wireRef));
      EXPECT(rt::deepEqual(field(wireEvidence, "digest"), row.evidence_ref->digest ? json::value(*row.evidence_ref->digest) : json::value(nullptr)));
    }
  }
  const auto boundWire = std::find_if(wireRows.begin(), wireRows.end(), [&first](const auto& row) { return rt::getString(row, "incident_id") == first.value.incident_id; });
  ASSERT(boundWire != wireRows.end());
  EXPECT_EQ(rt::getNumber(field(*boundWire, "provenance"), "captured_at_ms"), std::optional<double>(capturedAtMs));
  auto readingChunks = uos::sentinel::replay_read::getChunks(*kv, stored.manifest);
  const auto chunks = co_await std::move(readingChunks);
  rt::Bytes ciphertext;
  for (const auto& chunk : chunks) ciphertext.insert(ciphertext.end(), chunk.begin(), chunk.end());
  const std::string digest = uos::utils::encodeHex(rt::crypto::subtle::digest("SHA-256", ciphertext));
  EXPECT_EQ(rt::getString(field(*boundWire, "evidence_ref"), "digest"), std::optional<std::string_view>(digest));
  const auto boundStored = std::find_if(storedRows.begin(), storedRows.end(), [&first](const auto& row) { return row.incident_id == first.value.incident_id; });
  ASSERT(boundStored != storedRows.end());
  EXPECT_EQ(evidenceRefOf(*boundStored), std::optional<std::string>("capture:fixed-allowlist-capture"));
  const std::string boundRef = "artifact://sentinel/" + first.value.incident_id + "/fixed-allowlist-capture";
  EXPECT_EQ(rt::getString(field(*boundWire, "evidence_ref"), "ref"), std::optional<std::string_view>(boundRef));
  const rt::KvKey corruptKey{SENTINEL_INCIDENT_INDEX_PREFIX, storedRows[0].fingerprint};
  const auto raw = co_await kv->get(corruptKey);
  rt::KvObject corrupted = raw.value.asObject();
  rt::KvObject provenance = raw.value["provenance"].asObject();
  provenance.set("endpoint", rt::KvValue("https://evil.example"));
  corrupted.set("provenance", rt::KvValue(std::move(provenance)));
  const rt::KvValue corruptValue(std::move(corrupted));
  auto writingCorrupt = kv->set(corruptKey, corruptValue);
  co_await std::move(writingCorrupt);
  auto corruptTask = uos::handler::index::handler(adminRequest(indexUrl()));
  auto corrupt = co_await std::move(corruptTask);
  EXPECT_EQ(corrupt.status(), 503);
  const auto corruptText = co_await corrupt.text();
  EXPECT(!rt::includes(corruptText, "evil.example"));
}

// Current-TypeScript differential: incident_stored_fields_oracle.ts (credential-free Bun 1.4.0).
ASYNC_TEST("capture publication, observation merge and duplicate binding preserve opaque incident properties") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const double nowMs = rt::dateNow();
  const rt::Bytes keyBytes = newSyntheticKey();
  const AcceptedSentinelReplayInput input = syntheticInput(encoded(R"({"input":"opaque-incident-fixture"})"), "opaque-incident-fixture");
  const SentinelClientFailureObservation client = clientFailureObservationFor(502);
  auto recording = recordSentinelIncidentIndexObservation(*kv, observationInput(input, input.git_sha, nowMs, 502));
  const rt::KvEntry<SentinelIncidentIndexRow> index = co_await std::move(recording);
  const rt::KvEntryMaybe<> entry = co_await kv->get(index.key);
  rt::KvObject object;
  object.set("extra_first", rt::KvValue(rt::BigInt(7)));
  for (const auto& [name, value] : entry.value.asObject()) object.set(name, value);
  rt::KvObject provenance;
  provenance.set("extra_nested", rt::KvValue(rt::BigInt(11)));
  for (const auto& [name, value] : entry.value["provenance"].asObject()) provenance.set(name, value);
  provenance.set("extra_nested_undefined", rt::KvValue());
  object.set("provenance", rt::KvValue(provenance));
  rt::KvObject latest = entry.value["latest"].asObject();
  latest.set("extra_latest_bytes", rt::KvValue(rt::KvBytes{3, 4}));
  object.set("latest", rt::KvValue(std::move(latest)));
  object.set("extra_bytes", rt::KvValue(rt::KvBytes{0, 255}));
  object.set("extra_undefined", rt::KvValue());
  std::vector<std::string> outerKeys;
  for (const auto& [name, _] : object) outerKeys.push_back(name);
  std::vector<std::string> provenanceKeys;
  for (const auto& [name, _] : provenance) provenanceKeys.push_back(name);
  const rt::KvValue seeded(std::move(object));
  auto seeding = kv->set(index.key, seeded);
  co_await std::move(seeding);
  const auto expectExtras = [&outerKeys, &provenanceKeys](const rt::KvValue& value) {
    EXPECT_EQ(value["extra_first"], rt::KvValue(rt::BigInt(7)));
    EXPECT_EQ(value["extra_bytes"], rt::KvValue(rt::KvBytes{0, 255}));
    EXPECT(value.asObject().contains("extra_undefined"));
    EXPECT(value["extra_undefined"].isUndefined());
    EXPECT_EQ(value["provenance"]["extra_nested"], rt::KvValue(rt::BigInt(11)));
    EXPECT(value["provenance"].asObject().contains("extra_nested_undefined"));
    std::vector<std::string> currentOuter;
    for (const auto& [name, _] : value.asObject()) currentOuter.push_back(name);
    std::vector<std::string> currentProvenance;
    for (const auto& [name, _] : value["provenance"].asObject()) currentProvenance.push_back(name);
    EXPECT_EQ(currentOuter, outerKeys);
    EXPECT_EQ(currentProvenance, provenanceKeys);
  };
  auto persisting = persistEncryptedSentinelReplay(input, failureObservation(502), dependenciesFor(kv, keyBytes, nowMs, "opaque-capture"), client);
  const SentinelReplayPersistResult first = co_await std::move(persisting);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(first));
  const rt::KvEntryMaybe<> published = co_await kv->get(index.key);
  expectExtras(published.value);
  auto recordingOlder = recordSentinelIncidentIndexObservation(*kv, observationInput(input, input.git_sha, nowMs - 1, 502));
  co_await std::move(recordingOlder);
  const rt::KvEntryMaybe<> merged = co_await kv->get(index.key);
  expectExtras(merged.value);
  EXPECT_EQ(merged.value["count"], rt::KvValue(2));
  EXPECT_EQ(merged.value["latest"]["extra_latest_bytes"], rt::KvValue(rt::KvBytes{3, 4}));
  auto duplicating = persistEncryptedSentinelReplay(input, failureObservation(502), dependenciesFor(kv, keyBytes, nowMs + 1, "unused-duplicate"), client);
  const SentinelReplayPersistResult duplicate = co_await std::move(duplicating);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultDuplicate>(duplicate));
  const rt::KvEntryMaybe<> rebound = co_await kv->get(index.key);
  expectExtras(rebound.value);
  EXPECT_EQ(rebound.value["evidence_ref"]["ref"], rt::KvValue("capture:opaque-capture"));
  auto listing = listedRows(kv, index.value.incident_id, 1);
  const std::vector<SentinelIncidentIndexRow> rows = co_await std::move(listing);
  ASSERT_EQ(rows.size(), std::size_t{1});
  EXPECT_EQ(rows[0].count, 2.0);
}
