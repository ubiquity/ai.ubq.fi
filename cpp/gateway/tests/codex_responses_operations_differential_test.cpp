// Differential cases for the context operations of src/codex/responses-{state,operations,runners}.ts that the
// end-to-end cases (tests/codex_responses_differential_test.cpp) do not isolate: fences, reset inputs, reselection
// bookkeeping, auth-warning decoration, candidate capture, probe recovery and verified-reset telemetry, called directly
// on a created context. Vectors recorded under bun 1.4.0 (fixed clock, deterministic KV), credential-isolated (HOME,
// CODEX_HOME and CODEX_AUTH_JSON_B64 unset).
// Links: tests/codex_responses_operations_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp
//        src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp
//        src/release.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp
//        src/codex/index.cpp src/codex/responses_state.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp
//        src/codex/banked_reset_pool.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_claim.cpp
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/responses_operations.hpp"
#include "uos/codex/responses_runners.hpp"
#include "uos/codex/responses_state.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include "tests/codex_responses_operations_differential_vectors.inc"

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace auth = uos::codex::auth;
  namespace dispatch = uos::codex::dispatch;
  namespace responses_state = uos::codex::responses_state;
  namespace routing_state = uos::codex::routing_state;
  namespace capacity_events = uos::provider::capacity_events;
  using uos::codex::auth::CodexError;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::tests::helpers::codex_diff_kv::plain;
  using uos::tests::helpers::codex_diff_kv::routingAccountJson;
  using uos::types::CodexAuthState;
  using Operations = responses_state::CodexResponseOperations;

  constexpr double NOW = 1'790'000'000'000;

  const json::object& vectors() {
    static const json::value parsed = rt::jsonParse(CODEX_RESPONSES_OPERATIONS_VECTORS);
    return parsed.as_object();
  }

  bool same(const json::value& expected, const json::value& actual, const std::string& label) {
    if (rt::deepEqual(expected, actual)) return true;
    std::fprintf(stderr, "    %s differs\n      expected: %s\n      actual:   %s\n", label.c_str(), rt::jsonStringify(expected).c_str(), rt::jsonStringify(actual).c_str());
    return false;
  }

  json::value caught(const rt::Reason& error) {
    json::object out;
    out["name"] = rt::errorName(error).value_or("Error");
    out["message"] = rt::errorMessage(error).value_or("");
    try {
      std::rethrow_exception(error);
    } catch (const CodexError& codexError) {
      out["code"] = codexError.code;
      out["status"] = rt::makeNumber(codexError.status);
    } catch (...) {
    }
    return out;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(json::value(part.string()));
    return parts;
  }

  json::value swallow(const std::function<bool()>& check) {
    try {
      return check();
    } catch (...) {
      return json::object{{"error", caught(std::current_exception())}};
    }
  }

  std::string selectionKind(const routing_state::RouteSelection& selection) {
    return std::string(std::visit([](const auto& arm) { return std::string_view(std::decay_t<decltype(arm)>::kind); }, selection));
  }

  rt::Task<json::value> responseJson(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    auto reading = response.text();
    const std::string body = co_await std::move(reading);
    co_return json::object{{"status", response.status()}, {"headers", std::move(headers)}, {"body", body}};
  }

  rt::Task<void> settle(int turns) {
    for (int index = 0; index < turns; ++index) co_await rt::microtask();
  }

  // Credential isolation (mandatory): nothing here may reach this host's ~/.codex/auth.json.
  struct CredentialIsolation {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::string savedBaseUrl = uos::config::config.codexBaseUrl;
    bool savedDeploy = uos::config::config.isDeploy;
    CredentialIsolation() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.codexBaseUrl = "https://chatgpt.com/backend-api/codex";
      uos::config::config.isDeploy = false;
      rt::globalDateNow() = [] { return NOW; };
      capacity_events::setProviderCapacitySampleTriggerForTest([](const capacity_events::ProviderCapacitySampleTriggerOptions&) {});
    }
    ~CredentialIsolation() {
      capacity_events::setProviderCapacitySampleTriggerForTest({});
      uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest();
      uos::config::config.codexAuthJsonB64 = savedSeed;
      uos::config::config.codexBaseUrl = savedBaseUrl;
      uos::config::config.isDeploy = savedDeploy;
    }
  };

  uos::types::CodexResetRedemptionRecord recordOf(uos::types::CodexResetRedemptionState state, std::string idempotency, std::optional<double> verifiedAtMs) {
    return uos::types::CodexResetRedemptionRecord{.v = 1,
                                                  .account_id_hash = "hash",
                                                  .credential_version = "cred",
                                                  .quota_generation = "v1:gen",
                                                  .routing_generation = 4,
                                                  .idempotency_key_hash = std::move(idempotency),
                                                  .state = state,
                                                  .owner_token = "owner",
                                                  .fence = 1,
                                                  .lease_expires_at_ms = NOW,
                                                  .provider_receipt_id = std::nullopt,
                                                  .created_at_ms = NOW - 10,
                                                  .updated_at_ms = NOW - 5,
                                                  .submitted_at_ms = NOW - 5,
                                                  .verified_at_ms = verifiedAtMs,
                                                  .last_error_code = std::nullopt};
  }
}

ASYNC_TEST("responses operations differential: context state, fences, reselection, warnings, capture and telemetry") {
  const CredentialIsolation isolation;
  static const rt::Regex UUID("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", "");
  const CodexAuthState accountA{.access_token = "access-a", .refresh_token = "refresh-a", .account_id = "acct-a", .updated_at_ms = NOW - 10'000};
  const CodexAuthState accountB{.access_token = "access-b", .refresh_token = "refresh-b", .account_id = "acct-b", .updated_at_ms = NOW - 10'000};
  const uos::types::CodexAuthPoolState pool{.accounts = {accountA, accountB}, .updated_at_ms = NOW - 5'000};
  const json::value poolJson = json::value_from(pool);
  const auto kv = std::make_shared<DiffKv>();
  uos::kv::setKvForTest(kv);
  auth::resetCodexAuthCacheForTest();
  uos::codex::overage_settings::resetOverageUsageCacheForTest();
  uos::provider::health::resetProviderHealthThrottleForTest();
  rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(poolJson));
  co_await std::move(setting);
  rt::Promise<dispatch::PreparedCodexSubscriptionRequest> preparing(
    dispatch::prepareCodexSubscriptionRequest(json::object{{"model", "gpt-5"}, {"input", "hi"}}, std::nullopt));
  auto prepared = std::make_shared<const dispatch::PreparedCodexSubscriptionRequest>(co_await preparing.wait());
  const dispatch::CodexProviderDispatchCoordinator coordinator = dispatch::createCodexProviderDispatchCoordinator({});
  dispatch::FetchCodexResponsesOptions options;
  options.requestId = "req-ops";
  options.clientVersion = "0.101.0";
  rt::Promise<responses_state::CreateCodexResponseStateResult> creating(responses_state::createCodexResponseState(prepared, options, coordinator));
  const responses_state::CreateCodexResponseStateResult created = co_await creating.wait();
  const std::shared_ptr<responses_state::CodexResponseContext> ctx = std::get<responses_state::CreateCodexResponseStateResultState>(created).state;
  uos::codex::responses_operations::installCodexResponseOperations(ctx);
  uos::codex::responses_runners::installCodexResponseRunners(ctx);
  const routing_state::RoutingAccount routing = ctx->accountEntries.at(0).routing.value();

  {
    json::array headers;
    for (const auto& [name, value] : ctx->baseHeaders.entries()) headers.push_back(json::array{json::value(name), json::value(UUID.test(value) ? "<uuid>" : value)});
    const auto configJson = [](const uos::codex::banked_reset::CodexBankedResetConfig& config) {
      return json::object{{"enabled", config.enabled}, {"mode", config.mode}, {"maxPerAccountPerDay", config.maxPerAccountPerDay}, {"maxPerAccountPerWindow", config.maxPerAccountPerWindow}};
    };
    const json::object state{{"url", ctx->url},
                             {"serializedBody", ctx->prepared->serializedBody},
                             {"baseHeaders", std::move(headers)},
                             {"selectedKind", selectionKind(ctx->selected)},
                             {"accountEntries", static_cast<double>(ctx->accountEntries.size())},
                             {"requestedModel", ctx->requestedModel.has_value() ? json::value(*ctx->requestedModel) : json::value(nullptr)},
                             {"config", configJson(ctx->bankedResetDependencies.config)},
                             {"reloaded", configJson(ctx->reloadConfiguredBankedResetConfig())},
                             {"routing", routingAccountJson(routing)}};
    EXPECT(same(vectors().at("state"), state, "state"));
  }

  const uos::codex::dispatch::CodexBankedResetCandidate candidate{
    .accountEntry = ctx->accountEntries.at(0), .auth = accountA, .routing = routing, .quotaResetAtMs = NOW + 3'600'000, .routingGeneration = 1};
  {
    uos::types::CodexAuthPoolState rotatedPool = pool;
    rotatedPool.accounts[0].access_token = "access-a2";
    rotatedPool.updated_at_ms = NOW;
    const json::value rotated = json::value_from(rotatedPool);
    const responses_state::BankedResetFence authFence = ctx->authPoolFence(candidate);
    const responses_state::BankedResetFence exact = ctx->exactValueFence(routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, rt::jsonStringify(poolJson));
    const responses_state::BankedResetFence nullExact = ctx->exactValueFence(routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, "null");
    const json::value activeRow = kv->row(routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY);
    const responses_state::BankedResetFence snapshotFence = ctx->activeSnapshotFence(routing_state::CodexActiveAccountSnapshot{});
    const responses_state::BankedResetFence activeFence = ctx->activeSelectionFence(candidate);
    const json::object fences{
      {"authKey", keyJson(authFence.key)},
      {"authCurrent", authFence.isCurrent(poolJson)},
      {"authRotated", authFence.isCurrent(rotated)},
      {"authNull", authFence.isCurrent(json::value(nullptr))},
      {"authBroken", swallow([&] { return authFence.isCurrent(json::object{{"accounts", "x"}}); })},
      {"authShort", authFence.isCurrent(json::object{{"accounts", json::array{}}, {"updated_at_ms", 1}})},
      {"exactKey", keyJson(exact.key)},
      {"exactSame", exact.isCurrent(poolJson)},
      {"exactOther", exact.isCurrent(rotated)},
      {"exactNullExpected", nullExact.isCurrent(json::value(nullptr))},
      {"activeRow", plain(activeRow)},
      {"snapshotNull", snapshotFence.isCurrent(json::value(nullptr))},
      {"snapshotRow", snapshotFence.isCurrent(activeRow)},
      {"activeFenceRow", activeFence.isCurrent(activeRow)},
      {"activeFenceNull", activeFence.isCurrent(json::value(nullptr))},
    };
    EXPECT(same(vectors().at("fences"), fences, "fences"));

    const routing_state::CodexActiveAccountSnapshot originalActive{};
    const responses_state::CodexResetCohortSnapshot cohort{.poolJson = rt::jsonStringify(poolJson), .capacityJson = "null"};
    const std::vector<responses_state::BankedResetFence> resetFences = ctx->resetFences(candidate, 4, originalActive, cohort);
    const std::vector<responses_state::BankedResetFence> elected = ctx->electedResetFences(candidate, 4);
    const responses_state::BankedResetInput input = ctx->resetInput(candidate, 4, originalActive, cohort);
    json::array fenceKeys, fenceOnNull, fenceOnPool, electedKeys, electedOnNull;
    for (const auto& fence : resetFences) {
      fenceKeys.push_back(keyJson(fence.key));
      fenceOnNull.push_back(swallow([&] { return fence.isCurrent(json::value(nullptr)); }));
      fenceOnPool.push_back(swallow([&] { return fence.isCurrent(poolJson); }));
    }
    for (const auto& fence : elected) {
      electedKeys.push_back(keyJson(fence.key));
      electedOnNull.push_back(swallow([&] { return fence.isCurrent(json::value(nullptr)); }));
    }
    const json::object reset{
      {"fenceKeys", std::move(fenceKeys)},
      {"fenceOnNull", std::move(fenceOnNull)},
      {"fenceOnPool", std::move(fenceOnPool)},
      {"electedKeys", std::move(electedKeys)},
      {"electedOnNull", std::move(electedOnNull)},
      {"input", json::object{{"accountId", input.accountId},
                             {"credentialVersion", input.credentialVersion},
                             {"quotaResetAtMs", input.quotaResetAtMs},
                             {"routingGeneration", input.routingGeneration},
                             {"requestId", input.requestId.has_value() ? json::value(*input.requestId) : json::value(nullptr)},
                             {"signal", input.signal.has_value() ? "set" : "undefined"},
                             {"fences", static_cast<double>(input.fences.size())}}},
      {"providerContract", ctx->resetDependenciesForCandidate(candidate).provider->contract},
    };
    EXPECT(same(vectors().at("reset"), reset, "reset"));
  }

  {
    json::array steps;
    const responses_state::CodexRetryCandidate retry{
      .accountEntry = ctx->accountEntries.at(0), .auth = accountA, .routing = routing, .delayMs = 100, .readyAtMs = NOW + 100, .expiresAtMs = NOW + 2'000};
    const auto snapshot = [&ctx](const std::string& step) {
      json::object out{{"step", step}, {"activeReselectionRequested", ctx->activeReselectionRequested}};
      out["retry"] = ctx->retryState.candidate.has_value() ? json::value(ctx->retryState.candidate->delayMs) : json::value(nullptr);
      out["preserved"] = ctx->preservedShortRetryState.current.has_value()
                           ? json::value(json::object{{"accountIdHash", ctx->preservedShortRetryState.current->accountIdHash},
                                                      {"activeGeneration", ctx->preservedShortRetryState.current->activeGeneration}})
                           : json::value(nullptr);
      return out;
    };
    const auto preserving = [&routing](std::optional<double> generation) {
      Operations::RequestActiveReselectionOptions options_;
      options_.preserveRetry = true;
      routing_state::RoutingAccount withGeneration = routing;
      withGeneration.activeGeneration = generation;
      options_.routing = withGeneration;
      return options_;
    };
    ctx->retryState.candidate = retry;
    ctx->requestActiveReselection(preserving(1));
    steps.push_back(snapshot("preserve"));
    const std::optional<responses_state::CodexResponseRunners::ReselectedActiveIdentityResult> identity = ctx->reselectedActiveIdentity();
    steps.push_back(json::object{
      {"step", "identity"},
      {"identity", identity.has_value()
                     ? json::value(json::object{{"accountIdHash", identity->accountIdHash.has_value() ? json::value(*identity->accountIdHash) : json::value(nullptr)},
                                                {"generation", identity->generation.has_value() ? json::value(*identity->generation) : json::value(nullptr)}})
                     : json::value(nullptr)}});
    const bool restoredSame = ctx->restorePreservedShortRetry();
    json::object restoreSame = snapshot("restore_same");
    restoreSame["restored"] = restoredSame;
    steps.push_back(std::move(restoreSame));
    ctx->requestActiveReselection(preserving(9));
    steps.push_back(snapshot("preserve_other_generation"));
    const bool restoredOther = ctx->restorePreservedShortRetry();
    json::object restoreOther = snapshot("restore_other");
    restoreOther["restored"] = restoredOther;
    steps.push_back(std::move(restoreOther));
    ctx->retryState.candidate = retry;
    ctx->requestActiveReselection(preserving(1.5));
    steps.push_back(snapshot("fractional_generation"));
    ctx->retryState.candidate = retry;
    ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
    steps.push_back(snapshot("no_options"));
    Operations::RequestActiveReselectionOptions preserveOnly;
    preserveOnly.preserveRetry = true;
    ctx->requestActiveReselection(preserveOnly);
    steps.push_back(snapshot("no_candidate"));
    EXPECT(same(vectors().at("reselection"), steps, "reselection"));
  }

  {
    json::array steps;
    const auto warningJson = [&ctx] { return ctx->authWarning.has_value() ? json::value(*ctx->authWarning) : json::value(nullptr); };
    rt::Task<json::value> plainFailure = responseJson(ctx->authFailureResponse(std::nullopt));
    json::value plainResponse = co_await std::move(plainFailure);
    steps.push_back(json::object{{"step", "plain"}, {"response", plainResponse}, {"warning", warningJson()}});
    ctx->noteCodexAuthFailure(rt::makeReason(rt::JsError("x")));
    steps.push_back(json::object{{"step", "note_plain"},
                                 {"warning", warningJson()},
                                 {"error", ctx->authFailureState.error.has_value() ? json::value(ctx->authFailureState.error->code) : json::value(nullptr)}});
    ctx->authWarning = std::nullopt;
    ctx->noteCodexAuthFailure(rt::makeReason(CodexError("reused token", "refresh_token_reused", 401)));
    steps.push_back(json::object{{"step", "note_reused"},
                                 {"warning", warningJson()},
                                 {"error", ctx->authFailureState.error.has_value() ? json::value(ctx->authFailureState.error->code) : json::value(nullptr)}});
    ctx->noteCodexAuthFailure(rt::makeReason(CodexError("later", "codex_auth_invalid", 401)));
    steps.push_back(json::object{{"step", "note_second"}, {"warning", warningJson()}, {"error", ctx->authFailureState.error->message}});
    rt::Task<json::value> failure = responseJson(ctx->authFailureResponse(ctx->authFailureState.error));
    json::value failureResponse = co_await std::move(failure);
    steps.push_back(json::object{{"step", "failure_response"}, {"response", failureResponse}});
    rt::Task<json::value> invalid = responseJson(ctx->authFailureResponse(CodexError("bad creds", "codex_auth_invalid", 401)));
    json::value invalidResponse = co_await std::move(invalid);
    steps.push_back(json::object{{"step", "failure_invalid"}, {"response", invalidResponse}});
    rt::ResponseInit init;
    init.headers = rt::Headers{{"x-uos-warning", "existing"}};
    rt::Task<json::value> decorated = responseJson(ctx->decorateAuthWarning(rt::Response(rt::BodyInit(std::string("ok")), init)));
    json::value decoratedResponse = co_await std::move(decorated);
    steps.push_back(json::object{{"step", "decorate"}, {"response", decoratedResponse}});
    ctx->authWarning = std::string();
    rt::Task<json::value> decoratedEmpty = responseJson(ctx->decorateAuthWarning(rt::Response(rt::BodyInit(std::string("ok")))));
    json::value decoratedEmptyResponse = co_await std::move(decoratedEmpty);
    steps.push_back(json::object{{"step", "decorate_empty"}, {"response", decoratedEmptyResponse}});
    ctx->authWarning = std::nullopt;
    EXPECT(same(vectors().at("warnings"), steps, "warnings"));
  }

  {
    json::array steps;
    const std::vector<std::pair<std::string, Operations::CaptureBankedResetCandidateDisposition>> dispositions{
      {"not_usage_limit", {.usageLimitReached = false, .retryAtMs = NOW + 1'000, .resetDeadlineIsStable = true}},
      {"no_deadline", {.usageLimitReached = true, .retryAtMs = std::nullopt, .resetDeadlineIsStable = true}},
      {"unstable", {.usageLimitReached = true, .retryAtMs = NOW + 1'000, .resetDeadlineIsStable = false}},
      {"no_fence", {.usageLimitReached = true, .retryAtMs = NOW + 1'000, .resetDeadlineIsStable = true}},
    };
    for (const auto& [name, disposition] : dispositions) {
      ctx->bankedResetCandidates.set(routing.slot, candidate);
      rt::Promise<std::string> capturing(ctx->captureBankedResetCandidate(ctx->accountEntries.at(0), routing, accountA, disposition));
      const std::string outcome = co_await capturing.wait();
      steps.push_back(json::object{{"name", name}, {"outcome", outcome}, {"size", static_cast<double>(ctx->bankedResetCandidates.size())}});
    }
    EXPECT(same(vectors().at("capture"), steps, "capture"));

    routing_state::RoutingAccount probe = routing;
    probe.probeRequired = true;
    probe.probeGeneration = 3;
    probe.probeToken = "token-3";
    probe.probeCircuit = std::string("quota");
    probe.auth = accountB;
    const uos::codex::dispatch::CodexBankedResetCandidate recovered = ctx->withResetRecoveryProbe(candidate, probe);
    const json::object recovery{{"routing", routingAccountJson(recovered.routing)},
                                {"accountEntryRouting", routingAccountJson(recovered.accountEntry.routing)},
                                {"accountEntryAuth", recovered.accountEntry.auth.account_id},
                                {"auth", recovered.auth.account_id}};
    EXPECT(same(vectors().at("recovery"), recovery, "recovery"));

    using State = uos::types::CodexResetRedemptionState;
    const std::vector<std::tuple<std::string, double, std::optional<uos::types::CodexResetRedemptionRecord>>> records{
      {"slot_1", 0, recordOf(State::verified, "idem-1", NOW - 1)},
      {"slot_2", 1, recordOf(State::verified, "idem-2", NOW - 1)},
      {"slot_3", 2, recordOf(State::verified, "idem-3", NOW - 1)},
      {"not_verified", 0, recordOf(State::submitted, "idem-4", NOW - 1)},
      {"no_verified_time", 0, recordOf(State::verified, "idem-5", std::nullopt)},
      {"null_record", 0, std::nullopt},
    };
    json::array persisted;
    for (const auto& [name, slot, record] : records) {
      const double before = kv->version;
      uos::codex::dispatch::CodexBankedResetCandidate slotted = candidate;
      slotted.routing.slot = slot;
      rt::Promise<void> persisting(ctx->persistVerifiedCapacityReset(slotted, record));
      co_await persisting.wait();
      rt::Task<void> settling = settle(40);
      co_await std::move(settling);
      json::array writes;
      for (const auto& [key, row] : kv->rows) {
        if (rt::stringToNumber(row.versionstamp) > before) writes.push_back(json::object{{"key", key}, {"value", row.value.toJson()}});
      }
      persisted.push_back(json::object{{"name", name}, {"writes", std::move(writes)}});
    }
    EXPECT(same(vectors().at("persist"), persisted, "persist"));

    uos::codex::banked_reset::CodexBankedResetPoolOutcome skipped{.kind = "skipped", .reason = "x", .selected = std::nullopt, .reset = std::nullopt};
    uos::codex::banked_reset::CodexBankedResetOutcome verified{.kind = "verified", .reason = "y", .accountIdHash = std::nullopt, .quotaGeneration = std::nullopt,
                                                              .idempotencyKeyHash = std::nullopt, .record = std::nullopt};
    uos::codex::banked_reset::CodexBankedResetPoolCandidate slot0{.slot = 0, .candidate = {}, .provider = nullptr};
    uos::codex::banked_reset::CodexBankedResetPoolCandidate slot1{.slot = 1, .candidate = {}, .provider = nullptr};
    const auto resetJson = [](const std::optional<responses_state::EvaluatedBlockedReset>& evaluated) -> json::value {
      if (!evaluated.has_value()) return nullptr;
      return json::object{{"kind", evaluated->reset.kind}, {"reason", evaluated->reset.reason}};
    };
    const std::vector<uos::codex::dispatch::CodexBankedResetCandidate> locals{candidate};
    json::array selected;
    selected.push_back(resetJson(ctx->selectedBankedResetCandidate(skipped, locals, routing_state::CodexActiveAccountSnapshot{})));
    selected.push_back(resetJson(ctx->selectedBankedResetCandidate(
      uos::codex::banked_reset::CodexBankedResetPoolOutcome{.kind = "verified", .reason = "y", .selected = slot0, .reset = verified}, locals, routing_state::CodexActiveAccountSnapshot{})));
    selected.push_back(resetJson(ctx->selectedBankedResetCandidate(
      uos::codex::banked_reset::CodexBankedResetPoolOutcome{.kind = "verified", .reason = "y", .selected = slot1, .reset = verified}, locals, routing_state::CodexActiveAccountSnapshot{})));
    EXPECT(same(vectors().at("selected"), selected, "selected"));
  }
  uos::kv::setKvForTest(nullptr);
}
