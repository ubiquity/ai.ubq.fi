// Links: tests/sentinel_replay_export_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//        src/models/catalog.cpp src/input_normalization.cpp src/deepseek/waterfall.cpp src/provider/surplus.cpp src/provider/claude_code_models.cpp src/provider/metered.cpp src/sentinel/replay_admin.cpp src/sentinel/upstream_capture.cpp src/provider/surplus_claude_chat_request.cpp src/models/codex_models.cpp src/sentinel/replay_read.cpp
//        src/provider/selection.cpp src/sentinel/replay_model.cpp src/config.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/surplus_claude_request.cpp src/defaults.cpp src/types.cpp src/utils.cpp src/sentinel/incident_outbox.cpp src/sentinel/replay_observation.cpp src/responses_stream.cpp
//        src/provider/cerebras.cpp src/auth/passkeys.cpp src/models/metadata.cpp src/request.cpp src/request_policy.cpp src/sentinel/replay_envelope.cpp src/auth/index.cpp src/agent_messages.cpp src/auth/passport.cpp src/http.cpp src/models/openrouter_models.cpp src/provider/anthropic_chat_responses.cpp src/authority/native_api_key_policy.cpp
//        src/auth/passport_sessions.cpp src/models/codex_models_whitelist.cpp src/authority/native_api_key.cpp src/catalog/index.cpp src/kv.cpp src/provider/openrouter.cpp src/cache/prompt_analytics_queue.cpp src/auth/relay.cpp src/sentinel/replay_store.cpp src/sentinel/replay_retention_schema.cpp src/provider/anthropic_replay.cpp
//        src/provider/anthropic_response_wire.cpp src/catalog/store.cpp src/provider/claude_code.cpp src/models/codex_models_fetch.cpp src/cache/prompt_analytics_core.cpp src/provider/anthropic_responses.cpp src/release.cpp src/sentinel/replay_retention.cpp src/provider/lithos.cpp src/codex/auth_refresh.cpp src/catalog/models.cpp
//        src/sentinel/replay_retention_metadata.cpp src/codex/auth.cpp src/codex/dispatch.cpp src/provider/anthropic_chat_stream.cpp src/sentinel/replay_retention_bootstrap.cpp src/catalog/types.cpp src/auth/local_development_key.cpp src/api_keys.cpp src/kernel/quota_reservations.cpp src/kernel/attestation.cpp src/authority/native_paid.cpp
//        src/codex/capacity_routing.cpp src/authority/native_codex_capacity.cpp src/runtime_config.cpp src/analytics.cpp src/models/codex_models_availability.cpp src/authority/native_kernel.cpp src/authority/native_paid_policy.cpp src/provider/capacity_events.cpp src/authority/native_codex_contracts.cpp src/codex/routing_mutations.cpp
//        src/api_key_policy.cpp src/authority/native_codex_mutations.cpp src/sentinel/replay_limits.cpp src/kernel/quota_v2.cpp src/provider/capacity_contract.cpp src/optional_telemetry_queue.cpp src/authority/native_kernel_contracts.cpp src/codex/rate_limit_429.cpp src/provider/cerebras_rate_limits.cpp
//        tests/helpers/sentinel_recorded_upstream.cpp src/provider/claude_code_quota.cpp src/codex/banked_reset.cpp src/kernel/policy_queue.cpp src/authority/native_codex_429.cpp src/provider/claude_code_usage_schema.cpp src/deepseek/index.cpp src/bounded_response_body.cpp src/codex/native_auth.cpp src/handler/index.cpp
//        src/paid_fallback/ledger_state.cpp src/admin/kernel.cpp src/admin/codex.cpp src/cache/scope_experiment.cpp src/openai_telemetry.cpp src/admin/claude_code.cpp src/cache/scope_experiment_model.cpp src/admin/defaults.cpp src/auth/local_admin.cpp src/codex/supervisor.cpp src/provider/lithos_models.cpp src/admin/api_keys.cpp
//        src/cache/scope_experiment_targets.cpp src/metered_quota.cpp src/storage/kv.cpp src/kernel/usage.cpp src/kernel/quota_write.cpp src/recent_model_context.cpp src/codex/experiment.cpp src/codex/routing_state.cpp src/admin/capacity_observation_sync.cpp src/admin/error_log.cpp src/admin/api_key_audit.cpp src/provider/health.cpp
//        src/health.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/ledger_backfill.cpp src/codex/supervisor_config.cpp src/cache/kv_migration_base.cpp src/quota_projection.cpp src/codex/supervisor_inventory.cpp src/provider/capacity_parse.cpp src/paid_fallback/index.cpp src/cache/prompt_analytics_read.cpp src/static.cpp
//        src/handler/terminal_route.cpp src/admin/api_key_mutations.cpp src/authority/native_api_key_audit.cpp src/paid_fallback/ledger_settlement.cpp src/handler/admission.cpp src/provider/capacity.cpp src/live/handler.cpp src/provider/claude_capacity_rollups.cpp src/debug_routing.cpp src/authority/native_codex_evaluation.cpp
//        src/provider/capacity_history.cpp src/codex/banked_reset_provider.cpp src/provider/lithos_rate_limits.cpp src/provider/capacity_analytics.cpp src/codex/overage_settings.cpp src/provider/capacity_rollups.cpp src/codex/external_auth.cpp src/storage/sqlite_readonly.cpp src/cache/kv_migration_validation.cpp src/cache/telemetry_gate.cpp
//        src/live/sideband.cpp src/authority/native_paid_contracts.cpp src/cache/kv_migration_paid_fallback.cpp src/admin/chart_source_proxy.cpp src/codex/reset_settings.cpp src/systemone/handlers.cpp src/cache/scope_targets.cpp src/live/upstream.cpp src/cache/kv_migration_usage.cpp src/provider/claude_code_access.cpp src/decisions/handlers.cpp
//        src/catalog/promotion.cpp src/handler/http.cpp src/provider/claude_capacity_history.cpp src/storage/native_kv.cpp src/paid_fallback/ledger_admission.cpp src/provider/presentation.cpp src/provider/removed_provider_telemetry.cpp src/codex/account_routing.cpp src/kv_conversion/profile_json.cpp src/images.cpp src/codex/banked_reset_pool.cpp
//        src/responses_failover_stream.cpp src/codex/supervisor_transport.cpp src/codex/supervisor_follow.cpp src/codex/supervisor_brief.cpp src/live/routes.cpp src/chat/envelope.cpp src/codex/banked_reset_claim.cpp src/responses_handler.cpp src/codex/quota.cpp src/admin/sentinel_incident.cpp
//        src/codex/responses_error.cpp src/openai.cpp src/inference_deadline.cpp src/embeddings/jobs.cpp src/embeddings/handlers.cpp src/runtime/websocket.cpp src/codex/routing_serial.cpp src/embeddings/voyage.cpp src/paid_fallback/routing.cpp src/handler/terminal_log.cpp src/paid_fallback/health.cpp src/paid_fallback/removed_provider.cpp
//        src/codex/native_auth_transport.cpp src/provider/removed_provider_circuit.cpp src/kernel/quota_reserve.cpp src/authority/native_codex.cpp src/embeddings/ledger.cpp src/chat/stream_translation.cpp src/responses_native_headers.cpp src/admin/codex_reset.cpp src/paid_fallback/rollups.cpp src/codex/manual_reset_state.cpp
//        src/provider/openrouter_request_controls.cpp src/provider/lithos_handlers.cpp src/provider/cerebras_handlers.cpp src/provider/lithos_streams.cpp src/deepseek/forwarded_payload_policy.cpp src/responses_request.cpp src/upstream_wire.cpp src/responses_attempts.cpp src/codex/routing_evaluation.cpp src/provider/openrouter_handlers.cpp
//        src/deepseek/responses_payload.cpp src/inference_admission.cpp src/deepseek/chat_projection.cpp src/chat/completions.cpp src/paid_fallback/claude_code_route.cpp src/runtime/http_early_sse.cpp src/live/calls.cpp src/codex/index.cpp src/codex/manual_reset.cpp src/deepseek/handlers.cpp src/codex/supervisor_secret.cpp
//        src/codex/banked_reset_submission.cpp src/responses_buffered.cpp src/deepseek/responses.cpp src/provider/openrouter_streams.cpp src/deepseek/context_admission_failure.cpp src/deepseek/waterfall_handler.cpp src/codex/responses_state.cpp src/deepseek/flash_aliases.cpp src/openai_schema.cpp src/provider/cerebras_responses.cpp
//        src/codex/supervisor_log.cpp src/codex/responses_operations.cpp src/provider/openrouter_web_tools.cpp src/provider/stream_relay.cpp src/deepseek/responses_stream.cpp src/runtime/http_tasks.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp src/deepseek/stream.cpp src/lib/jev_compaction/codex_items.cpp
//        src/codex/responses_runners.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp
// Port of all twelve original tests/sentinel-replay-export.test.ts registrations.
#include "uos/sentinel/replay_admin.hpp"
#include "uos/sentinel/replay_capture.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/auth/passkeys.hpp"
#include "uos/utils.hpp"
#include "uos/handler/index.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "tests/helpers/terminal_log_harness.hpp"
#include "tests/helpers/sentinel_recorded_upstream.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/base64.hpp"
#include <algorithm>
#include <set>
#include "uos/sentinel/replay_read.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace replay_read = uos::sentinel::replay_read;
  using uos::sentinel::replay_admin::handleAdminSentinelReplayCaptures;
  using uos::sentinel::replay_admin::SentinelReplayAdminDependencies;

  const std::string INCIDENT_ID = "provider-12345678-1234-4abc-8def-1234567890ab";

  // `https://ai.ubq.fi/admin/sentinel/replay-captures?${new URLSearchParams(params).toString()}`
  std::string exportUrl(const std::vector<rt::URLSearchParams::Entry>& params) {
    return "https://ai.ubq.fi/admin/sentinel/replay-captures?" + rt::URLSearchParams::fromRecord(params).toString();
  }

  // `{ list: null } as unknown as Kv`: an object that is not a Kv, so calling any member throws.
  class BrokenKv final : public rt::Kv {
  public:
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions) override { throw rt::TypeError("kv.get is not a function"); }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override {
      throw rt::TypeError("kv.getMany is not a function");
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::TypeError("kv.set is not a function"); }
    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw rt::TypeError("kv.list is not a function"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override { throw rt::TypeError("kv.atomic is not a function"); }
    void close() override { throw rt::TypeError("kv.close is not a function"); }
  };

  // `((await response.json()) as { error?: { code?: string } }).error?.code`
  rt::Task<std::optional<std::string>> errorCode(rt::Response response) {
    auto reading = response.json();
    const json::value payload = co_await std::move(reading);
    const std::optional<std::string_view> code = rt::getString(rt::get(rt::get(payload, "error"), "code"));
    co_return code ? std::optional<std::string>(std::string(*code)) : std::nullopt;
  }
  using uos::sentinel::replay_model::AcceptedSentinelReplayInput;
  using uos::sentinel::replay_model::ExportedSentinelReplayCapture;
  using uos::sentinel::replay_model::SentinelClientFailureObservation;
  using uos::sentinel::replay_model::SentinelFailureObservation;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;
  using namespace uos::sentinel::upstream_capture;
  const std::string SUPER_ADMIN_TOKEN = "sentinel-replay-export-super-admin-token";
  constexpr std::size_t UPSTREAM_CHUNK_BYTES = 64 * 1'024;


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

  struct ExportStore {
    uos::tests::helpers::terminal_log_harness::Isolation isolation;
    std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    ExportStore() {
      prepareT3StaticAssets(); uos::kv::setKvForTest(kv); uos::config::config.adminTokens.insert(SUPER_ADMIN_TOKEN); }
    ~ExportStore() {
      uos::cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();
      uos::config::config.adminTokens.erase(SUPER_ADMIN_TOKEN);
      kv->close();
      uos::kv::setKvForTest(nullptr);
    }
  };

  const json::value& field(const json::value& value, std::string_view key) {
    const auto* member = rt::get(value, key);
    if (!member) throw rt::JsError("missing synthetic response field");
    return *member;
  }

  rt::Bytes newSyntheticKey() {
    rt::Bytes bytes(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(bytes));
    return bytes;
  }

  AcceptedSentinelReplayInput syntheticInput(rt::Bytes bytes, std::string requestId) {
    return {.endpoint = "/v1/responses", .method = "POST", .body = std::make_shared<rt::Bytes>(std::move(bytes)), .content_type = "application/json",
      .compatibility_headers = {}, .request_id = std::move(requestId), .git_sha = "synthetic-git-sha", .deno_revision = "synthetic-revision"};
  }

  SentinelFailureObservation failureObservation() {
    return {.status = 502, .stream = false, .completed = false, .terminal_type = "http.error", .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt, .provider_route = "test-provider"};
  }

  PersistDependencies dependenciesFor(std::shared_ptr<rt::Kv> kv, rt::Bytes keyBytes, double nowMs, std::string captureId) {
    return {.kv = std::move(kv), .keyBytes = std::move(keyBytes), .now = [nowMs] { return nowMs; }, .randomUuid = [captureId] { return captureId; },
      .randomBytes = [](double) { return rt::Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}; }};
  }

  SentinelReplayPersistResultStored expectStored(const uos::sentinel::replay_observation::SentinelReplayPersistResult& result) {
    ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(result));
    return std::get<SentinelReplayPersistResultStored>(result);
  }

  rt::Request adminRequest(std::string url) {
    const rt::RequestInit init{.headers = rt::Headers{{"Authorization", "Bearer " + SUPER_ADMIN_TOKEN}}};
    return rt::Request(std::move(url), init);
  }

  ExportedSentinelReplayCapture firstCapture(const json::value& body) {
    const auto& captures = field(body, "data").as_array();
    ASSERT(!captures.empty());
    return json::value_to<ExportedSentinelReplayCapture>(captures[0]);
  }

  void assertManifestKeys(const json::value& capture) {
    const std::vector<std::string> expected{"version", "capture_id", "fingerprint", "case_group_digest", "captured_at_ms", "expires_at_ms", "algorithm",
      "compression", "iv", "chunk_count", "ciphertext_bytes"};
    EXPECT_EQ(rt::objectKeys(field(capture, "manifest").as_object()), expected);
  }

  rt::Bytes patternedBytes(std::size_t length) {
    rt::Bytes bytes(length);
    for (std::size_t index = 0; index < length; ++index) bytes[index] = static_cast<std::uint8_t>(index % 251);
    return bytes;
  }

  void assertBytesEqual(const rt::Bytes& actual, const rt::Bytes& expected) {
    EXPECT_EQ(actual.size(), expected.size());
    std::optional<std::size_t> difference;
    const auto shared = std::min(actual.size(), expected.size());
    for (std::size_t index = 0; index < shared; ++index) if (actual[index] != expected[index]) { difference = index; break; }
    if (!difference && actual.size() != expected.size()) difference = shared;
    EXPECT_EQ(difference, std::optional<std::size_t>());
  }

  rt::Task<SentinelUpstreamTrace> recordUpstreamTrace(rt::Bytes bytes, std::size_t chunkBytes) {
    const auto recorder = createSentinelUpstreamRecorder();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [bytes, chunkBytes](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      for (std::size_t offset = 0; offset < bytes.size(); offset += chunkBytes) {
        const auto end = std::min(offset + chunkBytes, bytes.size());
        const rt::Bytes chunk(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        controller.enqueue(chunk);
      }
      controller.close();
    };
    const rt::ByteStream stream(source, {.highWaterMark = 0});
    const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}}};
    const auto attempt = recorder.startAttempt("cerebras");
    auto wrapped = attempt.wrap(rt::Response(rt::BodyInit(stream), init));
    co_await wrapped.bytes();
    const auto trace = recorder.snapshotAndSeal();
    recorder.dispose();
    co_return trace;
  }

  json::object REPLAY_ROUTES() {
    return {{"chatgpt_codex", "https://recorded-upstream.invalid/codex/responses"}, {"claude_code", "https://recorded-upstream.invalid/anthropic/v1/messages"},
      {"surplus", "https://recorded-upstream.invalid/surplus/v1/responses"}, {"metered", "https://recorded-upstream.invalid/metered/v1/responses"},
      {"cerebras", "https://recorded-upstream.invalid/cerebras/v1/chat/completions"}, {"deepseek", "https://recorded-upstream.invalid/deepseek/v1/chat/completions"},
      {"lithos", "https://recorded-upstream.invalid/lithos/v1/chat/completions"}, {"openrouter", std::string(uos::provider::openrouter::OPENROUTER_CHAT_COMPLETIONS_URL)}};
  }

  SentinelClientFailureObservation retainedBodyObservation() {
    return {.status = 502, .stream = false, .completed = false, .terminal_type = "http.error", .failure_kind = "upstream_timeout", .framing_valid = true,
      .provider_route = "test-provider", .error_code = std::nullopt, .error_param = std::nullopt, .terminal_body_base64 = rt::btoa("upstream timed out"),
      .terminal_body_truncated = false};
  }

  rt::Task<void> assertReplayedUpstream(SentinelUpstreamTrace trace, rt::Bytes expected) {
    const auto replay = uos::tests::helpers::sentinel_recorded_upstream::createRecordedUpstreamReplay(trace, REPLAY_ROUTES());
    ASSERT(!trace.attempts.empty());
    const std::string provider = trace.attempts[0].provider;
    ASSERT(!provider.empty());
    const std::string url(rt::getString(REPLAY_ROUTES(), provider).value());
    auto fetching = replay.fetch(url);
    auto response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.headers().get("content-type"), std::optional<std::string>("text/event-stream"));
    ASSERT(response.body());
    auto reader = response.body()->getReader();
    std::size_t offset = 0;
    for (;;) {
      const auto read = co_await reader.read();
      if (read.done) break;
      ASSERT(read.value);
      const std::size_t expectedLength = std::min(UPSTREAM_CHUNK_BYTES, expected.size() - offset);
      EXPECT_EQ(read.value->size(), expectedLength);
      const rt::Bytes part(expected.begin() + static_cast<std::ptrdiff_t>(offset), expected.begin() + static_cast<std::ptrdiff_t>(offset + expectedLength));
      assertBytesEqual(*read.value, part);
      offset += read.value->size();
    }
    EXPECT_EQ(offset, expected.size());
    replay.assertComplete();
    const auto snapshot = replay.snapshot();
    EXPECT(!snapshot.failed);
    EXPECT_EQ(snapshot.chunksConsumed, snapshot.chunksTotal);
    EXPECT_EQ(snapshot.chunksTotal, static_cast<std::int64_t>((expected.size() + UPSTREAM_CHUNK_BYTES - 1) / UPSTREAM_CHUNK_BYTES));
  }

  rt::Task<void> runCaptureBound(std::size_t byteLength) {
    const ExportStore fixture;
    const auto keyBytes = newSyntheticKey();
    const double nowMs = 1'400'000'000'000;
    const std::string requestId = "synthetic-envelope-" + std::to_string(byteLength) + "-request";
    const auto upstreamBytes = patternedBytes(byteLength);
    const std::size_t expectedChunks = (byteLength + UPSTREAM_CHUNK_BYTES - 1) / UPSTREAM_CHUNK_BYTES;
    auto recording = recordUpstreamTrace(upstreamBytes, UPSTREAM_CHUNK_BYTES);
    const auto trace = co_await std::move(recording);
    ASSERT(!trace.attempts.empty());
    EXPECT_EQ(trace.attempts[0].chunks_base64.size(), expectedChunks);
    EXPECT(!trace.bytes_truncated);
    EXPECT(!trace.chunks_truncated);
    EXPECT(!trace.attempts_truncated);
    auto input = syntheticInput(rt::encodeUtf8(R"({"model":"gpt-5.6-sol","stream":false})"), requestId);
    input.upstream = trace;
    auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
      dependenciesFor(fixture.kv, keyBytes, nowMs, "synthetic-envelope-capture-" + std::to_string(byteLength)), retainedBodyObservation());
    const auto storedResult = co_await std::move(persisting);
    const auto stored = expectStored(storedResult);
    auto handling = uos::handler::index::handler(adminRequest(exportUrl({{"request_id", requestId}})));
    auto response = co_await std::move(handling);
    EXPECT_EQ(response.status(), 200);
    const auto body = co_await response.json();
    EXPECT_EQ(field(body, "data").as_array().size(), std::size_t{1});
    const auto capture = firstCapture(body);
    EXPECT_EQ(capture.manifest.fingerprint, stored.manifest.fingerprint);
    auto decrypting = replay_read::decryptExportedSentinelReplay(capture, keyBytes);
    const auto plaintext = co_await std::move(decrypting);
    EXPECT_EQ(plaintext.unavailable, std::optional<std::vector<std::string>>(std::vector<std::string>()));
    EXPECT_EQ(plaintext.capture_status, std::optional<std::string>("ready"));
    EXPECT_EQ(plaintext.replay_coverage, std::optional<std::string>("full"));
    EXPECT(!plaintext.upstream.attempts_truncated);
    EXPECT(!plaintext.upstream.bytes_truncated);
    EXPECT(!plaintext.upstream.chunks_truncated);
    ASSERT_EQ(plaintext.upstream.attempts.size(), std::size_t{1});
    EXPECT_EQ(plaintext.upstream.attempts[0].chunks_base64.size(), expectedChunks);
    EXPECT_EQ(plaintext.body_bytes, std::optional<double>(static_cast<double>(input.body->size())));
    ASSERT(plaintext.body);
    assertBytesEqual(*plaintext.body, *input.body);
    auto replaying = assertReplayedUpstream(plaintext.upstream, upstreamBytes);
    co_await std::move(replaying);
  }

}


ASYNC_TEST("anonymous replay-captures requests are rejected before any storage access") {
  uos::tests::helpers::terminal_log_harness::Isolation isolation;
  prepareT3StaticAssets();
  const rt::Request request(exportUrl({{"after_ms", "0"}, {"before_ms", "1"}}));
  auto handling = uos::handler::index::handler(request);
  auto anonymous = co_await std::move(handling);
  EXPECT_EQ(anonymous.status(), 401);
  auto anonymousError = errorCode(anonymous);
  const auto anonymousCode = co_await std::move(anonymousError);
  EXPECT_EQ(anonymousCode, std::optional<std::string>("invalid_api_key"));
  uos::config::config.adminTokens.insert(SUPER_ADMIN_TOKEN);
  const auto restore = rt::scopeExit([] { uos::config::config.adminTokens.erase(SUPER_ADMIN_TOKEN); });
  auto authenticatedTask = uos::handler::index::handler(adminRequest(request.url()));
  auto authenticated = co_await std::move(authenticatedTask);
  EXPECT_EQ(authenticated.status(), 503);
  auto readingError = errorCode(authenticated);
  const auto code = co_await std::move(readingError);
  EXPECT_EQ(code, std::optional<std::string>("sentinel_replay_storage_unavailable"));
}

ASYNC_TEST("an authenticated admin passkey reaches the replay export") {
  const ExportStore fixture;
  const auto kv = fixture.kv;
  const double now = rt::dateNow();
  const std::string userId = "sentinel-replay-passkey-user";
  const std::string handle = "sentinel-replay-passkey-handle";
  const std::string sessionToken = "sentinel-replay-passkey-session-token";
  const rt::KvValue user(json::object{{"id", userId}, {"handle", handle}, {"is_admin", true}, {"credential_ids", json::array{"sentinel-replay-credential"}},
    {"created_at_ms", now}, {"updated_at_ms", now}});
  auto writingUser = kv->set(uos::auth::passkeys::passkeyUserKey(userId), user);
  co_await std::move(writingUser);
  auto writingHandle = kv->set(uos::auth::passkeys::passkeyHandleKey(handle), rt::KvValue(userId));
  co_await std::move(writingHandle);
  const rt::KvValue session(json::object{{"token", sessionToken}, {"user_id", userId}, {"created_at_ms", now}, {"expires_at_ms", now + 3'600'000}});
  auto writingSession = kv->set(uos::auth::passkeys::passkeySessionKey(sessionToken), session);
  co_await std::move(writingSession);
  const rt::RequestInit init{.headers = rt::Headers{{"Cookie", std::string(uos::auth::passkeys::PASSKEY_RELAY_COOKIE_NAME) + "=" + rt::encodeURIComponent(sessionToken)}}};
  const rt::Request request(exportUrl({{"after_ms", "0"}, {"before_ms", "1"}}), init);
  auto handling = uos::handler::index::handler(request);
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  const auto payload = co_await response.json();
  EXPECT(field(payload, "data").is_array());
}

ASYNC_TEST("replay-captures export returns encrypted captures that decrypt to exact synthetic bytes") {
  const ExportStore fixture;
  const auto keyBytes = newSyntheticKey();
  const double nowMs = 1'700'000'000'000;
  const auto exactBytes = rt::encodeUtf8(R"({"model":"gpt-5.6-sol","stream":false,"messages":[{"role":"user","content":"synthetic replay bytes for export round trip"}]})");
  const auto input = syntheticInput(exactBytes, "synthetic-export-request-id");
  auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
    dependenciesFor(fixture.kv, keyBytes, nowMs, "synthetic-capture-1"));
  const auto result = co_await std::move(persisting);
  const auto stored = expectStored(result);
  auto handling = uos::handler::index::handler(adminRequest(exportUrl({{"after_ms", "0"}, {"before_ms", rt::numberToString(nowMs + 1)}})));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
  const auto body = co_await response.json();
  ASSERT_EQ(field(body, "data").as_array().size(), std::size_t{1});
  const auto capture = firstCapture(body);
  EXPECT_EQ(capture.manifest.fingerprint, stored.manifest.fingerprint);
  EXPECT_EQ(capture.manifest.algorithm, std::string("AES-256-GCM"));
  EXPECT_EQ(capture.manifest.compression, std::string("gzip"));
  assertManifestKeys(field(body, "data").as_array()[0]);
  std::string ciphertext;
  for (const auto& part : capture.chunks) ciphertext += part;
  EXPECT(!rt::includes(ciphertext, "synthetic replay bytes for export round trip"));
  auto decrypting = replay_read::decryptExportedSentinelReplay(capture, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.endpoint, input.endpoint);
  EXPECT_EQ(plaintext.method, input.method);
  EXPECT_EQ(plaintext.content_type, input.content_type);
  EXPECT_EQ(plaintext.request_id, input.request_id);
  EXPECT_EQ(plaintext.git_sha, input.git_sha);
  EXPECT_EQ(plaintext.deno_revision, input.deno_revision);
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, exactBytes);
  EXPECT_EQ(rt::decodeUtf8(*plaintext.body), rt::decodeUtf8(exactBytes));
}

ASYNC_TEST("replay capture exports and replays an upstream capture at 256 KiB + 1 byte above the legacy fixed metadata bound") {
  auto testing = runCaptureBound(256 * 1'024 + 1);
  co_await std::move(testing);
}

ASYNC_TEST("replay capture exports and replays an upstream capture at exactly the advertised 4 MiB capture bound") {
  auto testing = runCaptureBound(static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES));
  co_await std::move(testing);
}

ASYNC_TEST("replay capture marks an upstream capture over the advertised 4 MiB bound as explicitly incomplete") {
  const ExportStore fixture;
  const auto keyBytes = newSyntheticKey();
  const double nowMs = 1'410'000'000'000;
  const std::string requestId = "synthetic-over-limit-envelope-request";
  const auto upstreamBytes = patternedBytes(static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES) + UPSTREAM_CHUNK_BYTES);
  const std::size_t retainedChunks = (static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES) + UPSTREAM_CHUNK_BYTES - 1) / UPSTREAM_CHUNK_BYTES;
  auto recording = recordUpstreamTrace(upstreamBytes, UPSTREAM_CHUNK_BYTES);
  const auto trace = co_await std::move(recording);
  ASSERT(!trace.attempts.empty());
  EXPECT_EQ(trace.attempts[0].chunks_base64.size(), retainedChunks);
  EXPECT(trace.bytes_truncated);
  EXPECT(!trace.chunks_truncated);
  EXPECT(!trace.attempts_truncated);
  auto input = syntheticInput(rt::encodeUtf8(R"({"model":"gpt-5.6-sol","stream":false})"), requestId);
  input.upstream = trace;
  auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
    dependenciesFor(fixture.kv, keyBytes, nowMs, "synthetic-over-limit-envelope-capture"), retainedBodyObservation());
  const auto result = co_await std::move(persisting);
  const auto stored = expectStored(result);
  auto handling = uos::handler::index::handler(adminRequest(exportUrl({{"request_id", requestId}})));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  const auto body = co_await response.json();
  EXPECT_EQ(field(body, "data").as_array().size(), std::size_t{1});
  const auto capture = firstCapture(body);
  EXPECT_EQ(capture.manifest.fingerprint, stored.manifest.fingerprint);
  auto decrypting = replay_read::decryptExportedSentinelReplay(capture, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT(plaintext.upstream.bytes_truncated);
  const std::vector<std::string> unavailable{"upstream_trace_truncated"};
  EXPECT_EQ(plaintext.unavailable, std::optional<std::vector<std::string>>(unavailable));
  EXPECT_EQ(plaintext.capture_status, std::optional<std::string>("incomplete"));
  EXPECT_EQ(plaintext.replay_coverage, std::optional<std::string>("partial"));
  ASSERT(!plaintext.upstream.attempts.empty());
  const auto& attempt = plaintext.upstream.attempts[0];
  EXPECT_EQ(attempt.chunks_base64.size(), retainedChunks);
  std::size_t offset = 0;
  for (const auto& chunk : attempt.chunks_base64) {
    const auto decoded = rt::atobBytes(chunk);
    const rt::Bytes expected(upstreamBytes.begin() + static_cast<std::ptrdiff_t>(offset), upstreamBytes.begin() + static_cast<std::ptrdiff_t>(offset + decoded.size()));
    assertBytesEqual(decoded, expected);
    offset += decoded.size();
  }
  EXPECT_EQ(offset, static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES));
  EXPECT_THROWS_MATCHING(uos::tests::helpers::sentinel_recorded_upstream::createRecordedUpstreamReplay(plaintext.upstream, REPLAY_ROUTES()),
    "^Error: Sentinel recorded upstream trace is truncated$");
}

ASYNC_TEST("replay capture still rejects an upstream trace beyond the bounded capture contract") {
  const ExportStore fixture;
  const auto keyBytes = newSyntheticKey();
  const std::string requestId = "synthetic-out-of-contract-request";
  SentinelUpstreamTrace oversizedTrace;
  oversizedTrace.version = 1;
  SentinelUpstreamAttempt attempt;
  attempt.provider = "cerebras";
  attempt.status = 200;
  attempt.content_type = "application/json";
  attempt.chunks_base64.assign(static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_CHUNKS) + 1, "AA==");
  attempt.terminal = "eof";
  oversizedTrace.attempts.push_back(attempt);
  auto input = syntheticInput(rt::encodeUtf8(R"({"model":"gpt-5.6-sol"})"), requestId);
  input.upstream = oversizedTrace;
  auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
    dependenciesFor(fixture.kv, keyBytes, 1'450'000'000'000, "synthetic-out-of-contract-capture"));
  rt::Reason rejection;
  try { co_await std::move(persisting); } catch (...) { rejection = std::current_exception(); }
  EXPECT(rejection && rt::Regex("Sentinel upstream trace has too many chunks").test(rt::reasonToString(rejection)));
  auto handling = uos::handler::index::handler(adminRequest(exportUrl({{"request_id", requestId}})));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  const auto body = co_await response.json();
  EXPECT(rt::deepEqual(field(body, "data"), json::array{}));
}

ASYNC_TEST("replay-captures export paginates multiple captures with an exhaustible cursor") {
  const ExportStore fixture;
  const auto keyBytes = newSyntheticKey();
  const double nowMs = 1'600'000'000'000;
  std::vector<std::string> requestIds;
  for (int index = 1; index <= 3; ++index) {
    const std::string requestId = "synthetic-pagination-request-" + std::to_string(index);
    requestIds.push_back(requestId);
    const json::object content{{"model", "gpt-5.6-sol"}, {"seq", index}, {"content", "synthetic pagination capture " + std::to_string(index)}};
    const auto input = syntheticInput(rt::encodeUtf8(rt::jsonStringify(content)), requestId);
    auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
      dependenciesFor(fixture.kv, keyBytes, nowMs + index * 1'000, "synthetic-pagination-capture-" + std::to_string(index)));
    const auto result = co_await std::move(persisting);
    const auto stored = expectStored(result);
    EXPECT_EQ(stored.manifest.capture_id, "synthetic-pagination-capture-" + std::to_string(index));
  }
  std::vector<ExportedSentinelReplayCapture> collected;
  std::vector<double> pageCapturedAt;
  std::optional<std::string> cursor;
  int pages = 0;
  for (;;) {
    std::vector<rt::URLSearchParams::Entry> params{{"after_ms", "0"}, {"before_ms", rt::numberToString(nowMs + 10'000)}};
    if (cursor) params.emplace_back("cursor", *cursor);
    auto handling = uos::handler::index::handler(adminRequest(exportUrl(params)));
    auto response = co_await std::move(handling);
    EXPECT_EQ(response.status(), 200);
    const auto body = co_await response.json();
    for (const auto& value : field(body, "data").as_array()) {
      const auto capture = json::value_to<ExportedSentinelReplayCapture>(value);
      collected.push_back(capture);
      pageCapturedAt.push_back(capture.manifest.captured_at_ms);
    }
    const auto next = rt::getString(body, "cursor");
    cursor = next ? std::optional<std::string>(std::string(*next)) : std::nullopt;
    ++pages;
    ASSERT(pages <= 10);
    if (!cursor) break;
  }
  EXPECT_EQ(collected.size(), std::size_t{3});
  std::set<std::string> fingerprints;
  for (const auto& capture : collected) fingerprints.insert(capture.manifest.fingerprint);
  EXPECT_EQ(fingerprints.size(), std::size_t{3});
  auto sortedTimes = pageCapturedAt;
  std::sort(sortedTimes.begin(), sortedTimes.end());
  EXPECT_EQ(pageCapturedAt, sortedTimes);
  std::set<std::string> decryptedRequestIds;
  for (const auto& capture : collected) {
    auto decrypting = replay_read::decryptExportedSentinelReplay(capture, keyBytes);
    const auto plaintext = co_await std::move(decrypting);
    decryptedRequestIds.insert(plaintext.request_id);
  }
  EXPECT_EQ(decryptedRequestIds, std::set<std::string>(requestIds.begin(), requestIds.end()));
}

ASYNC_TEST("replay-captures export scopes to a linked incident and stays encrypted") {
  const ExportStore fixture;
  const auto keyBytes = newSyntheticKey();
  const double nowMs = 1'500'000'000'000;
  const auto bytes = rt::encodeUtf8(R"({"model":"gpt-5.6-sol","content":"incident-linked synthetic capture"})");
  const auto input = syntheticInput(bytes, "synthetic-incident-request");
  auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
    dependenciesFor(fixture.kv, keyBytes, nowMs, "synthetic-incident-capture"));
  const auto result = co_await std::move(persisting);
  const auto stored = expectStored(result);
  ASSERT(stored.manifest_key);
  auto linking = uos::sentinel::incident_outbox::linkSentinelReplayToIncident(*fixture.kv, INCIDENT_ID, stored.manifest.fingerprint, *stored.manifest_key);
  co_await std::move(linking);
  auto handling = uos::handler::index::handler(adminRequest(exportUrl({{"after_ms", "0"}, {"before_ms", rt::numberToString(nowMs + 1)}, {"incident_id", INCIDENT_ID}})));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  const auto body = co_await response.json();
  ASSERT_EQ(field(body, "data").as_array().size(), std::size_t{1});
  const auto capture = firstCapture(body);
  EXPECT_EQ(capture.manifest.fingerprint, stored.manifest.fingerprint);
  assertManifestKeys(field(body, "data").as_array()[0]);
  auto decrypting = replay_read::decryptExportedSentinelReplay(capture, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.request_id, std::string("synthetic-incident-request"));
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, bytes);
  auto foreignTask = uos::handler::index::handler(adminRequest(exportUrl({{"after_ms", "0"}, {"before_ms", rt::numberToString(nowMs + 1)},
    {"incident_id", "provider-00000000-0000-4000-8000-000000000000"}})));
  auto foreign = co_await std::move(foreignTask);
  EXPECT_EQ(foreign.status(), 200);
  const auto foreignBody = co_await foreign.json();
  EXPECT(rt::deepEqual(field(foreignBody, "data"), json::array{}));
}

ASYNC_TEST("replay-captures export resolves a request id without any time parameters") {
  const ExportStore fixture;
  const auto keyBytes = newSyntheticKey();
  const double nowMs = rt::dateNow();
  const auto bytes = rt::encodeUtf8(R"({"model":"gpt-5.6-sol","content":"request-id-only synthetic capture"})");
  const auto input = syntheticInput(bytes, "synthetic-request-id-only");
  auto persisting = uos::sentinel::replay_capture::persistEncryptedSentinelReplay(input, failureObservation(),
    dependenciesFor(fixture.kv, keyBytes, nowMs, "synthetic-request-id-capture"));
  const auto result = co_await std::move(persisting);
  const auto stored = expectStored(result);
  auto handling = uos::handler::index::handler(adminRequest(exportUrl({{"request_id", "synthetic-request-id-only"}})));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
  const auto body = co_await response.json();
  EXPECT(field(body, "cursor").is_null());
  EXPECT_EQ(rt::getString(field(body, "capture_status"), "status"), std::optional<std::string_view>("incomplete"));
  EXPECT(field(field(body, "capture_status"), "reason").is_null());
  EXPECT_EQ(field(body, "data").as_array().size(), std::size_t{1});
  const auto capture = firstCapture(body);
  EXPECT_EQ(capture.manifest.fingerprint, stored.manifest.fingerprint);
  auto decrypting = replay_read::decryptExportedSentinelReplay(capture, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.request_id, std::string("synthetic-request-id-only"));
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, bytes);
  EXPECT(plaintext.unavailable && std::find(plaintext.unavailable->begin(), plaintext.unavailable->end(), "upstream_trace_empty") != plaintext.unavailable->end());
  auto malformedTask = uos::handler::index::handler(adminRequest(exportUrl({{"request_id", "not a request id"}})));
  const auto malformed = co_await std::move(malformedTask);
  EXPECT_EQ(malformed.status(), 400);
  auto bothTask = uos::handler::index::handler(adminRequest(exportUrl({{"request_id", "synthetic-request-id-only"}, {"incident_id", INCIDENT_ID}})));
  const auto bothModes = co_await std::move(bothTask);
  EXPECT_EQ(bothModes.status(), 400);
}

ASYNC_TEST("replay-captures export rejects malformed interval, limit, cursor, and incident parameters") {
  uos::tests::helpers::terminal_log_harness::Isolation isolation;
  prepareT3StaticAssets();
  uos::config::config.adminTokens.insert(SUPER_ADMIN_TOKEN);
  const auto restore = rt::scopeExit([] { uos::config::config.adminTokens.erase(SUPER_ADMIN_TOKEN); });
  const std::vector<std::string> queries{"after_ms=abc&before_ms=1", "after_ms=10&before_ms=5", "after_ms=0", "after_ms=0&before_ms=1&limit=0",
    "after_ms=0&before_ms=1&limit=2", "after_ms=0&before_ms=1&limit=abc", "after_ms=0&before_ms=1&cursor=!!",
    "after_ms=0&before_ms=1&cursor=" + rt::repeat("a", 3'000), "after_ms=0&before_ms=1&incident_id=not-an-incident-id"};
  for (const auto& query : queries) {
    const std::string url = "https://ai.ubq.fi/admin/sentinel/replay-captures?" + query;
    auto task = uos::handler::index::handler(adminRequest(url));
    const auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), 400);
  }
}

ASYNC_TEST("replay-captures export fails closed when the storage read fails instead of succeeding empty") {
  const rt::Request request(exportUrl({{"after_ms", "0"}, {"before_ms", "1"}}));
  const std::shared_ptr<rt::Kv> brokenKv = std::make_shared<BrokenKv>();
  SentinelReplayAdminDependencies intervalDependencies;
  intervalDependencies.getKv = [brokenKv] { return rt::Promise<std::shared_ptr<rt::Kv>>::resolve(brokenKv); };
  intervalDependencies.listEncryptedSentinelReplays = [](rt::Kv&, replay_read::ListEncryptedSentinelReplaysOptions) -> rt::Task<replay_read::EncryptedSentinelReplayPage> {
    throw rt::JsError("injected listing failure");
  };
  auto handlingInterval = handleAdminSentinelReplayCaptures(request, intervalDependencies);
  const rt::Response interval = co_await std::move(handlingInterval);
  EXPECT_EQ(interval.status(), 503);
  auto readingInterval = errorCode(interval);
  const std::optional<std::string> intervalCode = co_await std::move(readingInterval);
  EXPECT_EQ(intervalCode, std::optional<std::string>("sentinel_replay_export_failed"));

  const rt::Request incidentRequest(exportUrl({{"after_ms", "0"}, {"before_ms", "1"}, {"incident_id", INCIDENT_ID}}));
  SentinelReplayAdminDependencies incidentDependencies;
  incidentDependencies.getKv = [brokenKv] { return rt::Promise<std::shared_ptr<rt::Kv>>::resolve(brokenKv); };
  incidentDependencies.listEncryptedSentinelIncidentReplays = [](rt::Kv&, replay_read::ListEncryptedSentinelIncidentReplaysOptions)
    -> rt::Task<replay_read::EncryptedSentinelReplayPage> { throw rt::JsError("injected incident listing failure"); };
  auto handlingIncident = handleAdminSentinelReplayCaptures(incidentRequest, incidentDependencies);
  const rt::Response incident = co_await std::move(handlingIncident);
  EXPECT_EQ(incident.status(), 503);
  auto readingIncident = errorCode(incident);
  const std::optional<std::string> incidentCode = co_await std::move(readingIncident);
  EXPECT_EQ(incidentCode, std::optional<std::string>("sentinel_replay_export_failed"));
}
