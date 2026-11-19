// Links: tests/sentinel_request_capture_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//        src/utils.cpp src/models/catalog.cpp src/sentinel/replay_model.cpp src/provider/surplus.cpp src/deepseek/waterfall.cpp src/models/metadata.cpp src/deepseek/index.cpp src/handler/terminal_log.cpp src/provider/surplus_claude_chat_request.cpp src/catalog/index.cpp src/catalog/store.cpp src/input_normalization.cpp
//        src/provider/claude_code_credentials.cpp src/provider/selection.cpp src/handler/http.cpp src/provider/metered.cpp src/request_policy.cpp src/provider/claude_code_models.cpp src/provider/claude_code_accounts.cpp src/catalog/types.cpp src/debug_routing.cpp src/handler/index.cpp src/admin/defaults.cpp src/sentinel/replay_observation.cpp
//        src/auth/passport.cpp src/catalog/models.cpp src/admin/codex.cpp src/auth/passkeys.cpp src/admin/api_key_mutations.cpp src/kernel/quota_reservations.cpp src/models/openrouter_models.cpp src/authority/native_kernel.cpp src/admin/error_log.cpp src/auth/index.cpp src/runtime_config.cpp src/api_key_policy.cpp src/kv.cpp
//        src/cache/prompt_analytics_core.cpp src/admin/api_keys.cpp src/models/codex_models_fetch.cpp src/auth/passport_sessions.cpp src/cache/telemetry_gate.cpp src/cache/kv_migration_base.cpp src/provider/capacity.cpp src/provider/capacity_history.cpp src/codex/quota.cpp src/types.cpp src/authority/native_api_key_policy.cpp
//        src/auth/local_development_key.cpp src/codex/auth_refresh.cpp src/provider/capacity_contract.cpp src/sentinel/replay_read.cpp src/responses_native_headers.cpp src/sentinel/incident_outbox.cpp src/cache/prompt_analytics_queue.cpp src/provider/capacity_parse.cpp src/models/codex_models_whitelist.cpp src/paid_fallback/ledger_state.cpp
//        src/provider/capacity_events.cpp src/analytics.cpp src/provider/anthropic_chat_responses.cpp src/admin/claude_code.cpp src/metered_quota.cpp src/kernel/quota_v2.cpp src/optional_telemetry_queue.cpp src/authority/native_paid_contracts.cpp src/authority/native_kernel_contracts.cpp src/models/codex_models.cpp
//        src/admin/capacity_observation_sync.cpp src/authority/native_api_key.cpp src/kernel/attestation.cpp src/admin/kernel.cpp src/provider/lithos_models.cpp src/kv_conversion/profile_json.cpp src/provider/surplus_claude_request.cpp src/cache/scope_experiment_targets.cpp src/paid_fallback/ledger_stats.cpp src/agent_messages.cpp
//        src/api_keys.cpp src/codex/reset_settings.cpp src/config.cpp src/defaults.cpp src/live/handler.cpp src/provider/claude_capacity_history.cpp src/paid_fallback/rollups.cpp src/admin/api_key_audit.cpp src/provider/claude_capacity_rollups.cpp src/live/sideband.cpp src/authority/native_paid_policy.cpp src/sentinel/replay_envelope.cpp
//        src/authority/native_paid.cpp src/provider/cerebras.cpp src/models/codex_models_availability.cpp src/deepseek/context_admission_failure.cpp src/paid_fallback/index.cpp src/http.cpp src/codex/auth.cpp src/codex/banked_reset_provider.cpp src/auth/local_admin.cpp src/openai_telemetry.cpp src/quota_projection.cpp
//        src/sentinel/upstream_capture.cpp src/codex/native_auth.cpp src/provider/anthropic_response_wire.cpp src/live/routes.cpp src/provider/capacity_rollups.cpp src/request.cpp src/admin/codex_reset.cpp src/recent_model_context.cpp src/health.cpp src/sentinel/replay_store.cpp src/cache/prompt_analytics_read.cpp src/handler/terminal_route.cpp
//        src/sentinel/replay_limits.cpp src/provider/capacity_analytics.cpp src/provider/claude_code_quota.cpp src/kernel/quota_write.cpp src/static.cpp src/cache/scope_targets.cpp src/provider/health.cpp src/authority/native_api_key_audit.cpp src/provider/presentation.cpp src/provider/anthropic_responses.cpp src/cache/scope_experiment_model.cpp
//        src/paid_fallback/ledger_backfill.cpp src/provider/anthropic_replay.cpp src/provider/openrouter.cpp src/provider/anthropic_chat_stream.cpp src/kernel/usage.cpp src/decisions/handlers.cpp src/live/upstream.cpp src/storage/kv.cpp src/admin/sentinel_incident.cpp src/codex/dispatch.cpp src/provider/claude_code_usage_schema.cpp
//        src/codex/supervisor.cpp src/provider/cerebras_rate_limits.cpp src/sentinel/replay_retention.cpp src/provider/claude_code_access.cpp src/paid_fallback/ledger_settlement.cpp src/cache/scope_experiment.cpp src/chat/envelope.cpp src/openai.cpp src/sentinel/replay_retention_schema.cpp src/deepseek/handlers.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/kernel/quota_reserve.cpp src/provider/stream_relay.cpp src/chat/completions.cpp src/chat/stream_translation.cpp src/codex/manual_reset.cpp src/codex/experiment.cpp src/upstream_wire.cpp src/codex/banked_reset.cpp
//        src/authority/native_codex_contracts.cpp src/codex/supervisor_config.cpp src/codex/external_auth.cpp src/codex/overage_settings.cpp src/codex/supervisor_inventory.cpp src/authority/native_codex.cpp src/responses_stream.cpp src/codex/routing_mutations.cpp src/codex/capacity_routing.cpp src/storage/sqlite_readonly.cpp
//        src/authority/native_codex_mutations.cpp src/authority/native_codex_capacity.cpp src/paid_fallback/ledger_admission.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/chat_projection.cpp src/codex/supervisor_follow.cpp src/inference_deadline.cpp src/storage/native_kv.cpp src/provider/lithos_handlers.cpp src/deepseek/stream.cpp
//        src/openai_schema.cpp src/auth/relay.cpp src/admin/chart_source_proxy.cpp src/provider/lithos_rate_limits.cpp src/handler/admission.cpp src/inference_admission.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp src/provider/lithos.cpp src/provider/lithos_streams.cpp src/runtime/http_tasks.cpp
//        src/provider/claude_code.cpp src/provider/openrouter_handlers.cpp src/deepseek/responses_payload.cpp src/runtime/http_early_sse.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_claim.cpp src/responses_failover_stream.cpp src/codex/banked_reset_pool.cpp src/runtime/websocket.cpp src/codex/supervisor_brief.cpp
//        src/deepseek/responses_stream.cpp src/embeddings/voyage.cpp src/lib/jev_compaction/codex_items.cpp src/responses_handler.cpp src/codex/native_auth_transport.cpp src/kernel/policy_queue.cpp src/codex/supervisor_transport.cpp src/cache/kv_migration_validation.cpp src/release.cpp src/codex/routing_state.cpp src/lib/jev_compaction/state.cpp
//        src/paid_fallback/health.cpp src/cache/kv_migration_usage.cpp src/sentinel/replay_admin.cpp src/deepseek/waterfall_handler.cpp src/provider/removed_provider_telemetry.cpp src/responses_attempts.cpp src/cache/kv_migration_paid_fallback.cpp src/codex/account_routing.cpp src/images.cpp src/deepseek/responses.cpp src/codex/routing_serial.cpp
//        src/bounded_response_body.cpp src/authority/native_codex_evaluation.cpp src/provider/removed_provider_circuit.cpp src/paid_fallback/removed_provider.cpp src/deepseek/flash_aliases.cpp src/responses_request.cpp src/live/calls.cpp src/paid_fallback/routing.cpp src/systemone/handlers.cpp src/embeddings/handlers.cpp
//        src/provider/openrouter_request_controls.cpp src/provider/cerebras_handlers.cpp src/responses_buffered.cpp src/provider/openrouter_streams.cpp src/catalog/promotion.cpp src/codex/routing_evaluation.cpp src/codex/rate_limit_429.cpp src/codex/manual_reset_state.cpp src/paid_fallback/claude_code_route.cpp src/lib/jev_compaction/types.cpp
//        src/lib/jev_compaction/compact.cpp src/provider/cerebras_responses.cpp src/codex/supervisor_secret.cpp src/provider/openrouter_web_tools.cpp src/codex/supervisor_log.cpp src/authority/native_codex_429.cpp src/codex/responses_error.cpp src/codex/index.cpp src/codex/responses_runners.cpp src/codex/responses_operations.cpp
//        src/lib/jev_compaction/request.cpp src/codex/responses_state.cpp
// Port of all five original tests/sentinel-request-capture.test.ts cases.
// Existing Sentinel encryption, replay/store and upstream fixtures remain unchanged.
#include "tests/helpers/terminal_log_harness.hpp"
#include "uos/api_keys.hpp"
#include "uos/debug_routing.hpp"
#include "uos/handler/index.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/sentinel/replay_capture.hpp"
#include "uos/utils.hpp"
#include "uos/rt/crypto.hpp"

using namespace uos;
namespace fixture = tests::helpers::terminal_log_harness;
namespace terminal = handler::terminal_log;

namespace {
  const std::string SUPER_ADMIN_TOKEN = "sentinel-request-capture-super-admin";
  const std::string MARKER = "sentinel-request-capture-exact-bytes-marker-9f4c2b";
  const rt::Bytes CAPTURE_BODY = rt::encodeUtf8(R"({"model":"gpt-5.6-sol","stream":false,"input":"sentinel-request-capture-exact-bytes-marker-9f4c2b"})");

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

  struct CaptureStore {
    std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    CaptureStore() {
      prepareT3StaticAssets();
      uos::kv::setKvForTest(kv);
      config::config.adminTokens.insert(SUPER_ADMIN_TOKEN);
      debug_routing::resetDebugRoutingCacheForTest();
    }
    ~CaptureStore() {
      cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();
      debug_routing::resetDebugRoutingCacheForTest();
      config::config.adminTokens.erase(SUPER_ADMIN_TOKEN);
      kv->close();
      uos::kv::setKvForTest(nullptr);
    }
  };

  rt::Task<void> seedAuthenticatedKey(std::shared_ptr<rt::Kv> kv, std::string token) {
    const double now = rt::dateNow();
    auto hashing = utils::sha256Base64Url(token);
    const std::string hash = co_await std::move(hashing);
    const std::string id = "request-capture-" + hash.substr(0, 12);
    json::object policyBase{{"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 100}, {"usage_requests", 0},
      {"usage_reset_at_ms", now + 60'000}, {"window_ms", 60'000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", true},
      {"paid_fallback_limit_microcredits", 1'000'000}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0},
      {"paid_fallback_reservation_request_id", nullptr}};
    json::object hashed{{"id", id}};
    rt::objectAssign(hashed, policyBase);
    auto writingHash = kv->set(api_keys::apiKeyHashKey(hash), rt::KvValue(hashed));
    co_await std::move(writingHash);
    json::object record{{"id", id}, {"name", "sentinel request capture test key"}, {"prefix", token.substr(0, 12)}, {"hash", hash}, {"created_at_ms", now}};
    rt::objectAssign(record, policyBase);
    record["paid_fallback_model_ids"] = json::array{"gpt-5.6-sol"};
    record["paid_fallback_quota_per_credit"] = 500'000;
    record["paid_fallback_max_exposure_microcredits"] = json::object{{"gpt-5.6-sol", 250'000}};
    record["paid_fallback_pricing_checked_at_ms"] = now;
    auto writingRecord = kv->set(api_keys::apiKeyIdKey(id), rt::KvValue(record));
    co_await std::move(writingRecord);
  }

  rt::Request captureRequest(std::optional<std::string> token) {
    rt::Headers headers{{"Content-Type", "application/json"}};
    if (token) headers.set("Authorization", "Bearer " + *token);
    const rt::RequestInit init{.method = "POST", .headers = headers, .body = CAPTURE_BODY};
    return rt::Request("https://ai.ubq.fi/v1/responses", init);
  }

  rt::Request adminRequest(std::string path) {
    const rt::RequestInit init{.headers = rt::Headers{{"Authorization", "Bearer " + SUPER_ADMIN_TOKEN}}};
    return rt::Request("https://ai.ubq.fi" + path, init);
  }

  rt::Task<std::size_t> countManifests(std::shared_ptr<rt::Kv> kv) {
    const rt::KvListSelector selector{.prefix = sentinel::replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX};
    const auto entries = kv->list(selector);
    std::size_t count = 0;
    while (co_await entries->next()) ++count;
    co_return count;
  }

  rt::Response successResponse() {
    return fixture::jsonResponse(R"({"id":"resp_synthetic","object":"response","status":"completed","model":"gpt-5.6-sol","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}]})");
  }
}

ASYNC_TEST("real handler failure capture persists exact accepted bytes through producer encryption and decryption") {
  fixture::Isolation isolation;
  const CaptureStore installed;
  rt::console::Capture logs({rt::console::Method::info});
  rt::Bytes keyBytes(32);
  rt::crypto::getRandomValues(std::span<std::uint8_t>(keyBytes));
  const rt::env::ScopedOverride replayKey({{"SENTINEL_REPLAY_KEY", utils::base64UrlEncode(keyBytes)}});
  const std::string token = api_keys::generateApiKeyToken();
  auto seeding = seedAuthenticatedKey(installed.kv, token);
  co_await std::move(seeding);
  const rt::KvValue scenario(json::object{{"scenario", "codex_429"}, {"expires_at_ms", nullptr}, {"updated_at_ms", rt::dateNow()}});
  auto writingScenario = installed.kv->set(debug_routing::DEBUG_ROUTING_KEY, scenario);
  co_await std::move(writingScenario);
  auto handling = handler::index::handler(captureRequest(token));
  auto response = co_await std::move(handling);
  EXPECT(response.status() >= 400);
  const auto responseText = co_await response.text();
  EXPECT(!rt::includes(responseText, MARKER));
  const std::string interval = "after_ms=0&before_ms=" + rt::numberToString(rt::dateNow() + 1);
  auto exporting = handler::index::handler(adminRequest("/admin/sentinel/replay-captures?" + interval));
  auto page = co_await std::move(exporting);
  EXPECT_EQ(page.status(), 200);
  const auto exported = co_await page.json();
  const auto& captures = field(exported, "data").as_array();
  ASSERT_EQ(captures.size(), std::size_t{1});
  const auto capture = json::value_to<sentinel::replay_model::ExportedSentinelReplayCapture>(captures[0]);
  EXPECT(!rt::includes(rt::jsonStringify(exported), MARKER));
  auto decrypting = sentinel::replay_read::decryptExportedSentinelReplay(capture, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.endpoint, std::string("/v1/responses"));
  EXPECT_EQ(plaintext.method, std::string("POST"));
  EXPECT_EQ(plaintext.content_type, std::optional<std::string>("application/json"));
  EXPECT(!plaintext.request_id.empty());
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, CAPTURE_BODY);
  EXPECT_EQ(rt::decodeUtf8(*plaintext.body), rt::decodeUtf8(CAPTURE_BODY));
  EXPECT_EQ(plaintext.observation.status, static_cast<double>(response.status()));
  EXPECT_EQ(plaintext.client_observation.terminal_type, std::optional<std::string>("http.error"));
  auto listing = handler::index::handler(adminRequest("/admin/sentinel/incidents"));
  auto incidents = co_await std::move(listing);
  EXPECT_EQ(incidents.status(), 200);
  const auto incidentsBody = co_await incidents.json();
  EXPECT(!rt::includes(rt::jsonStringify(incidentsBody), MARKER));
  EXPECT(field(incidentsBody, "cursor").is_null());
  const auto& rows = field(incidentsBody, "data").as_array();
  ASSERT_EQ(rows.size(), std::size_t{1});
  const auto& incident = rows[0];
  EXPECT_EQ(rt::getString(incident, "severity"), std::optional<std::string_view>("P2"));
  EXPECT_EQ(rt::getNumber(incident, "count"), std::optional<double>(1));
  const auto& provenance = field(incident, "provenance");
  EXPECT_EQ(rt::getString(provenance, "endpoint"), std::optional<std::string_view>("https://ai.ubq.fi/v1/responses"));
  EXPECT(field(provenance, "captured_by").is_null());
  const std::string incidentId(rt::getString(incident, "incident_id").value());
  const std::string artifactRef = "artifact://sentinel/" + incidentId + "/" + capture.manifest.capture_id;
  EXPECT_EQ(rt::getString(field(incident, "evidence_ref"), "ref"), std::optional<std::string_view>(artifactRef));
  EXPECT_EQ(rt::getNumber(incident, "evidence_expires_at_ms"), std::optional<double>(capture.manifest.expires_at_ms));
  auto filtering = handler::index::handler(adminRequest("/admin/sentinel/replay-captures?" + interval + "&incident_id=" + incidentId));
  auto scoped = co_await std::move(filtering);
  EXPECT_EQ(scoped.status(), 200);
  const auto scopedBody = co_await scoped.json();
  const auto& scopedCaptures = field(scopedBody, "data").as_array();
  ASSERT_EQ(scopedCaptures.size(), std::size_t{1});
  const auto scopedCapture = json::value_to<sentinel::replay_model::ExportedSentinelReplayCapture>(scopedCaptures[0]);
  rt::Bytes ciphertext;
  for (const auto& chunk : scopedCapture.chunks) {
    const auto part = utils::base64UrlDecode(chunk);
    ciphertext.insert(ciphertext.end(), part.begin(), part.end());
  }
  const std::string digest = utils::encodeHex(rt::crypto::subtle::digest("SHA-256", ciphertext));
  EXPECT_EQ(rt::getString(field(incident, "evidence_ref"), "digest"), std::optional<std::string_view>(digest));
  EXPECT_EQ(scopedCapture.manifest, capture.manifest);
  auto decryptingScoped = sentinel::replay_read::decryptExportedSentinelReplay(scopedCapture, keyBytes);
  const auto scopedPlaintext = co_await std::move(decryptingScoped);
  ASSERT(scopedPlaintext.body);
  EXPECT_EQ(*scopedPlaintext.body, CAPTURE_BODY);
}

ASYNC_TEST("unauthorized requests are never captured") {
  fixture::Isolation isolation;
  const CaptureStore installed;
  auto handlingAnonymous = handler::index::handler(captureRequest(std::nullopt));
  const auto anonymous = co_await std::move(handlingAnonymous);
  EXPECT_EQ(anonymous.status(), 401);
  auto countingAnonymous = countManifests(installed.kv);
  const auto anonymousCount = co_await std::move(countingAnonymous);
  EXPECT_EQ(anonymousCount, std::size_t{0});
  const std::string unknownKey = api_keys::generateApiKeyToken();
  auto handlingInvalid = handler::index::handler(captureRequest(unknownKey));
  const auto invalid = co_await std::move(handlingInvalid);
  EXPECT_EQ(invalid.status(), 401);
  auto countingInvalid = countManifests(installed.kv);
  const auto invalidCount = co_await std::move(countingInvalid);
  EXPECT_EQ(invalidCount, std::size_t{0});
}

ASYNC_TEST("successful terminal responses are never persisted and release captured bytes") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  auto input = fixture::acceptedInput();
  const auto originalBytes = *input.body;
  int persistenceCalls = 0;
  auto options = fixture::terminalOptions("request-capture-success");
  options.sentinelReplayInput = input;
  options.persistSentinelReplay = [&persistenceCalls](auto, auto, auto) {
    ++persistenceCalls;
    sentinel::replay_observation::SentinelReplayPersistResult result = sentinel::replay_observation::SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"};
    return rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>::resolve(result);
  };
  auto wrapped = terminal::withTerminalRequestLog(successResponse(), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(persistenceCalls, 0);
  EXPECT(fixture::zeroed(input.body));
  EXPECT_EQ(input.body->size(), originalBytes.size());
}

ASYNC_TEST("a persistable failure with no carried body publishes its explicit omission status") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  struct Recorded { std::string requestId; std::string reason; double nowMs; };
  std::vector<Recorded> recorded;
  auto options = fixture::terminalOptions("request-capture-omitted-body");
  options.sentinelReplayOmission = "body_over_limit";
  options.recordSentinelReplayOmission = [&recorded](std::string requestId, std::string reason, double nowMs) {
    recorded.push_back({std::move(requestId), std::move(reason), nowMs}); return rt::Promise<void>::resolve();
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse(
    R"({"error":{"message":"request body is too large","type":"invalid_request_error","code":"request_too_large"}})", 413), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 413);
  auto bytes = co_await response.bytes();
  EXPECT(!bytes.empty());
  ASSERT(recorded.size() == 1);
  EXPECT_EQ(recorded[0].requestId, std::string("request-capture-omitted-body"));
  EXPECT_EQ(recorded[0].reason, std::string("body_over_limit"));
  EXPECT(rt::Number::isFinite(recorded[0].nowMs));
}

ASYNC_TEST("a successful response never publishes an omission status") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  int recorded = 0;
  auto options = fixture::terminalOptions("request-capture-omitted-success");
  options.sentinelReplayOmission = "body_over_limit";
  options.recordSentinelReplayOmission = [&recorded](auto, auto, double) { ++recorded; return rt::Promise<void>::resolve(); };
  auto wrapped = terminal::withTerminalRequestLog(successResponse(), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 200);
  auto bytes = co_await response.bytes();
  EXPECT(!bytes.empty());
  EXPECT_EQ(recorded, 0);
}
