// Links: tests/cache_scope_experiment_model_coverage_test.cpp src/cache/scope_experiment_model.cpp src/utils.cpp src/openai_telemetry.cpp src/responses_stream.cpp
//        src/codex/auth.cpp src/codex/native_auth.cpp src/types.cpp src/provider/removed_provider_telemetry.cpp
//        src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp
//        src/authority/native_codex_contracts.cpp src/codex/native_auth_transport.cpp src/config.cpp src/codex/routing_state.cpp
//        src/codex/rate_limit_429.cpp src/authority/native_codex_429.cpp src/responses_failover_stream.cpp src/kv.cpp
//        src/http.cpp src/codex/external_auth.cpp src/provider/cerebras_rate_limits.cpp src/provider/health.cpp
//        src/bounded_response_body.cpp src/authority/native_codex_evaluation.cpp src/auth/relay.cpp src/release.cpp
//        src/runtime/websocket.cpp src/codex/responses_error.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
// Port of tests/cache-scope-experiment-model-coverage.test.ts. Additional terminal cases exercise the real SSE reader and Codex response metadata.
#include "uos/cache/scope_experiment_model.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace model = uos::cache::scope_experiment_model;
  const std::string SCOPE_MODEL = "gpt-5.6-sol";
  model::PromptCacheScopeTargetBinding targetBinding() {
    return {"target-1", std::string(model::PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER), "chatgpt_codex", "codex_account_pool", SCOPE_MODEL,
      std::string(uos::models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE), "capability-1", "inventory-1", "catalog-1", "runtime-1", "auth-pool-1", "identity-1", std::nullopt};
  }
  model::ConcreteScopeObservation observation() {
    return {std::string(uos::models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE), "shared", "preserved", "independent", SCOPE_MODEL};
  }
  std::vector<model::CacheSignal> signals(bool independent = false) {
    return independent ? std::vector<model::CacheSignal>{"write", "read", "read", "read", "read", "read", "read", "read", "read", "read"}
      : std::vector<model::CacheSignal>{"write", "read", "write", "read", "read", "read", "read", "write", "read", "read"};
  }
  std::vector<model::PromptCacheScopeSample> probeCycleSamples(bool independent = false) {
    std::vector<model::PromptCacheScopeSample> out;
    const auto sequence = signals(independent);
    for (std::size_t i = 0; i < sequence.size(); ++i) {
      const model::NormalizedUsage usage{2'560, sequence[i] == "read" ? 2'560.0 : 0.0, sequence[i] == "write" ? 2'560.0 : 0.0, 12, 2'572};
      out.push_back({std::string(model::CACHE_SCOPE_STEP_NAMES[i]), model::CACHE_SCOPE_EXPECTED_SLOTS[i], usage, usage, 120});
    }
    return out;
  }
  model::ConcreteScopeObservation probeClassification(bool independent = false) {
    return *model::classifyCycle(SCOPE_MODEL, probeCycleSamples(independent), signals(independent));
  }
  model::ExperimentState state(double nextCycle = 1) {
    std::vector<model::ConcreteScopeObservation> classifications;
    for (int i = 0; static_cast<double>(i) < nextCycle - 1; ++i) classifications.push_back(observation());
    return {3, targetBinding(), "owner-1", rt::dateNow() - 60'000, rt::dateNow() + 60'000, "auth-pool-1", nextCycle, classifications,
      nextCycle == 4 ? std::optional<model::ConcreteScopeObservation>(observation()) : std::nullopt};
  }
  model::StoredEvidence evidence(bool independent = false) {
    return {3, targetBinding(), "in_progress", rt::dateNow() - 60'000, rt::dateNow(),
      {{1, probeCycleSamples(independent), probeClassification(independent), std::nullopt}}, std::nullopt};
  }
  json::value changed(json::value value, std::string_view key, json::value replacement) {
    rt::objectSet(value.as_object(), key, std::move(replacement));
    return value;
  }
  void rejectTarget(const json::value& value) { EXPECT(!model::parseTargetBinding(value)); }
  void rejectState(const json::value& value) { EXPECT(!model::parseState(value)); }
  void rejectEvidence(const json::value& value) { EXPECT(!model::parseEvidence(value)); }
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c2"}}};
    std::string prior = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(nullptr); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = prior; uos::kv::setKvForTest(nullptr); }
  };
  rt::Response responseFor(const json::value& response, const std::string& type = "response.completed") {
    const json::object event{{"type", type}, {"response", response}};
    const std::string body = "event: " + type + "\ndata: " + rt::jsonStringify(event) + "\n\n";
    return rt::Response(body, rt::ResponseInit{.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  }
  json::value completed() {
    return json::object{{"model", SCOPE_MODEL}, {"usage", json::object{{"input_tokens", 2'560}, {"input_tokens_details", json::object{{"cached_tokens", 2'560}, {"cache_write_tokens", 0}}},
      {"output_tokens", 12}, {"total_tokens", 2'572}}}};
  }
}

TEST("scope experiment: the failure error keeps its own name and message") {
  const model::PromptCacheScopeExperimentFailedError error("campaign aborted");
  EXPECT_EQ(error.name, "PromptCacheScopeExperimentFailedError");
  EXPECT_EQ(error.message, "campaign aborted");
  EXPECT(dynamic_cast<const rt::JsError*>(&error));
}

TEST("scope experiment: a complete target binding round-trips and every literal fence is enforced") {
  const auto fixture = json::value_from(targetBinding());
  const auto parsed = model::parseTargetBinding(fixture);
  EXPECT(parsed);
  EXPECT_EQ(parsed->id, "target-1"); EXPECT_EQ(parsed->model, SCOPE_MODEL); EXPECT(!parsed->catalog_client_version);
  const auto client = model::parseTargetBinding(changed(fixture, "catalog_client_version", " 1.2.3 "));
  EXPECT(client); EXPECT_EQ(client->catalog_client_version, std::optional<std::string>("1.2.3"));
  for (const auto& value : std::vector<json::value>{nullptr, "target", json::array{}, changed(fixture, "provider", "openai"),
    changed(fixture, "telemetry_provider", "openrouter"), changed(fixture, "topology_kind", "single_account"), changed(fixture, "probe_profile", "other-profile"),
    changed(fixture, "extra", true), changed(fixture, "id", 7), changed(fixture, "auth_pool_identity_fingerprint", nullptr), changed(fixture, "catalog_client_version", "  ")}) rejectTarget(value);
  auto missing = fixture; rt::objectDelete(missing.as_object(), "id"); rejectTarget(missing);
  for (auto field : {"id", "model", "capability_fingerprint", "inventory_fingerprint", "catalog_versionstamp", "runtime_versionstamp", "auth_pool_versionstamp"})
    rejectTarget(changed(fixture, field, "   "));
}

TEST("scope experiment: state parsing accepts a fresh campaign and rejects drifted counters") {
  const auto fixture = json::value_from(state()); const auto parsed = model::parseState(fixture);
  EXPECT(parsed); EXPECT_EQ(parsed->campaign_owner, "owner-1"); EXPECT_EQ(parsed->next_cycle, 1); EXPECT(!parsed->pending_scope);
  const auto obs = json::value_from(observation());
  for (const auto& value : std::vector<json::value>{nullptr, "state", changed(fixture, "v", 2), changed(fixture, "extra", true), changed(fixture, "next_cycle", "1"),
    changed(fixture, "next_cycle", 0), changed(changed(fixture, "next_cycle", 3), "classifications", json::array{obs}),
    changed(changed(fixture, "next_cycle", 2), "classifications", json::array{obs, obs}), changed(fixture, "pending_scope", json::object{}),
    changed(json::value_from(state(2)), "pending_scope", obs), changed(fixture, "started_at_ms", 0), changed(fixture, "started_at_ms", rt::makeNumber(rt::dateNow() + 60'000)),
    changed(fixture, "expires_at_ms", rt::makeNumber(rt::dateNow() - 120'000)), changed(fixture, "auth_pool_versionstamp", "other")}) rejectState(value);
  auto missing = fixture; rt::objectDelete(missing.as_object(), "target"); rejectState(missing);
}

TEST("scope experiment: a finished campaign requires the pending scope to agree with all three cycles") {
  const auto fixture = json::value_from(state(4)); const auto parsed = model::parseState(fixture);
  EXPECT(parsed); EXPECT_EQ(parsed->pending_scope, std::optional<model::ConcreteScopeObservation>(observation())); EXPECT_EQ(parsed->classifications, state(4).classifications);
  auto disagree = observation(); disagree.account_slots = "account_scoped";
  rejectState(changed(fixture, "pending_scope", json::value_from(disagree)));
  auto missing = fixture; rt::objectDelete(missing.as_object(), "pending_scope"); rejectState(missing);
  disagree = observation(); disagree.token_refresh = "changed";
  rejectState(changed(fixture, "classifications", json::array{json::value_from(observation()), json::value_from(observation()), json::value_from(disagree)}));
}

TEST("scope experiment: sharedObservation agrees only on a non-empty unanimous sequence") {
  EXPECT(!model::sharedObservation({})); EXPECT_EQ(model::sharedObservation({observation(), observation()}), std::optional<model::ConcreteScopeObservation>(observation()));
  auto other = observation(); other.conversation_id = "scoped"; EXPECT(!model::sharedObservation({observation(), other}));
}

TEST("scope experiment: cycle classification derives the concrete scope from the discriminator steps") {
  const auto samples = probeCycleSamples(); const auto sequence = signals();
  const auto classified = model::classifyCycle(SCOPE_MODEL, samples, sequence); EXPECT(classified);
  EXPECT_EQ(classified->account_slots, "account_scoped"); EXPECT_EQ(classified->token_refresh, "preserved"); EXPECT_EQ(classified->conversation_id, "scoped");
  auto shortened = samples; shortened.pop_back(); auto shortSignals = sequence; shortSignals.pop_back(); EXPECT(!model::classifyCycle(SCOPE_MODEL, shortened, shortSignals));
  auto modified = samples; for (auto& row : modified) row.slot = 2; EXPECT(!model::classifyCycle(SCOPE_MODEL, modified, sequence));
  modified = samples; modified[4].usage.input_tokens += 1; EXPECT(!model::classifyCycle(SCOPE_MODEL, modified, sequence));
  auto wrongSignals = sequence; wrongSignals[3] = "write"; EXPECT(!model::classifyCycle(SCOPE_MODEL, samples, wrongSignals));
  for (const auto discriminator : std::array<std::size_t, 3>{2, 5, 7}) {
    auto missing = sequence; missing[discriminator] = ""; EXPECT(!model::classifyCycle(SCOPE_MODEL, samples, missing));
    modified = samples; modified[discriminator].usage.cached_tokens = 2'560; modified[discriminator].usage.cache_write_tokens = 2'560;
    EXPECT(!model::classifyCycle(SCOPE_MODEL, modified, sequence));
  }
  modified = samples; for (auto& row : modified) { row.usage = samples[0].usage; row.usage.cached_tokens = 2'560; row.usage.cache_write_tokens = 0; }
  EXPECT(!model::classifyCycle(SCOPE_MODEL, modified, sequence));
  for (auto& row : modified) { row.usage.cache_write_tokens = 2'560; }
  EXPECT(!model::classifyCycle(SCOPE_MODEL, modified, sequence));
}

TEST("scope experiment: the reusable counter matcher accepts zero or the exact cycle counter") {
  auto usage = probeCycleSamples()[0].usage; EXPECT(model::matchesCycleReusableCounter(usage, 2'560));
  usage.cached_tokens = 0; usage.cache_write_tokens = 0; EXPECT(model::matchesCycleReusableCounter(usage, 2'560));
  usage.cached_tokens = 2'500; EXPECT(!model::matchesCycleReusableCounter(usage, 2'560));
}

TEST("scope experiment: stored evidence accepts a coherent campaign and rejects incomplete cycles") {
  const auto fixture = json::value_from(evidence()); const auto parsed = model::parseEvidence(fixture);
  EXPECT(parsed); EXPECT_EQ(parsed->cycles.size(), 1U); EXPECT_EQ(parsed->outcome, "in_progress"); EXPECT_EQ(parsed->cycles[0].classification, evidence().cycles[0].classification);
  const auto cycle = json::value_from(evidence().cycles[0]);
  std::vector<json::value> rejects{nullptr, "evidence", changed(fixture, "v", 2), changed(fixture, "extra", true), changed(fixture, "outcome", "unknown"),
    changed(fixture, "verified_at_ms", rt::makeNumber(rt::dateNow() - 120'000)), changed(fixture, "started_at_ms", rt::makeNumber(rt::dateNow() + 60'000))};
  for (const auto& broken : std::vector<json::value>{changed(cycle, "cycle", 2), changed(cycle, "cycle", 0),
    changed(cycle, "inconclusive_reason", "slot_drift"), changed(cycle, "samples", json::array{}), changed(cycle, "classification", json::value_from(observation())),
    json::object{{"cycle", 1}, {"samples", json::array{}}, {"inconclusive_reason", "not_a_reason"}}}) rejects.push_back(changed(fixture, "cycles", json::array{broken}));
  auto neither = cycle; rt::objectDelete(neither.as_object(), "classification"); rejects.push_back(changed(fixture, "cycles", json::array{neither}));
  auto noSignals = evidence(); for (auto& sample : noSignals.cycles[0].samples) { sample.usage.cached_tokens = 0; sample.usage.cache_write_tokens = 0; sample.raw_usage = sample.usage; }
  rejects.push_back(json::value_from(noSignals)); for (const auto& value : rejects) rejectEvidence(value);
}

TEST("scope experiment: sample rows must carry a known step, matching slot and identical usage tuples") {
  auto one = evidence(); one.cycles[0].samples.resize(1); const auto valid = json::value_from(one); rejectEvidence(valid);
  const auto sample = json::value_from(one.cycles[0].samples[0]);
  for (const auto& modified : std::vector<json::value>{changed(sample, "step", "unknown_step"), changed(sample, "slot", 2), changed(sample, "elapsed_ms", -1),
    changed(sample, "raw_usage", changed(json::value_from(one.cycles[0].samples[0].usage), "cached_tokens", 1)), changed(sample, "extra", true)}) {
    auto value = valid; value.as_object()["cycles"].as_array()[0].as_object()["samples"] = json::array{modified}; rejectEvidence(value);
  }
}

TEST("scope experiment: an inconclusive cycle must carry a reason while an unclassified one must match the evidence") {
  auto fixture = evidence(); fixture.outcome = "inconclusive"; fixture.inconclusive_reason = "slot_drift";
  fixture.cycles = {{1, {}, std::nullopt, "slot_drift"}};
  const auto parsed = model::parseEvidence(json::value_from(fixture)); EXPECT(parsed); EXPECT_EQ(parsed->inconclusive_reason, fixture.inconclusive_reason);
  fixture.cycles[0].inconclusive_reason = "lease_lost"; rejectEvidence(json::value_from(fixture));
  fixture.cycles = {{1, {}, std::nullopt, "slot_drift"}, {2, {}, std::nullopt, "slot_drift"}}; rejectEvidence(json::value_from(fixture));
  fixture.outcome = "failed"; fixture.cycles = {}; rejectEvidence(json::value_from(fixture));
}

TEST("scope experiment: active state and evidence must describe one campaign") {
  auto active = state(2); auto stored = evidence(true); stored.started_at_ms = active.started_at_ms;
  EXPECT_EQ(stored.cycles[0].classification, std::optional<model::ConcreteScopeObservation>(observation()));
  EXPECT(model::activeStateEvidenceIsConsistent(active, stored));
  auto empty = active; empty.classifications.clear(); EXPECT(model::activeStateEvidenceIsConsistent(empty, std::nullopt));
  EXPECT(!model::activeStateEvidenceIsConsistent(empty, stored)); EXPECT(!model::activeStateEvidenceIsConsistent(active, std::nullopt));
  auto drift = stored; drift.target.inventory_fingerprint = "inventory-2"; EXPECT(!model::activeStateEvidenceIsConsistent(active, drift));
  drift = stored; drift.started_at_ms += 30'000; EXPECT(!model::activeStateEvidenceIsConsistent(active, drift));
  drift = evidence(); drift.started_at_ms = active.started_at_ms; EXPECT(!model::activeStateEvidenceIsConsistent(active, drift));
  EXPECT(!model::activeStateEvidenceIsConsistent(state(), stored));
}

TEST("scope experiment: an aborted probe signal raises the supplied reason or a DOMException") {
  model::throwIfAborted(rt::AbortController().signal());
  rt::AbortController controller; const auto reason = rt::makeReason(rt::JsError("campaign cancelled")); controller.abort(reason);
  rt::Reason caught; try { model::throwIfAborted(controller.signal()); } catch (...) { caught = std::current_exception(); } EXPECT_EQ(caught, reason);
  rt::AbortController bare; bare.abort(); EXPECT_THROWS_MATCHING(model::throwIfAborted(bare.signal()), "AbortError");
}

ASYNC_TEST("scope experiment: an unreadable upstream response fails closed with a cancellation attempt") {
  CredentialScope isolation; std::vector<std::string> cancelled;
  rt::UnderlyingSource<rt::ByteChunk> source; source.cancel = [&](rt::Reason) { cancelled.push_back("cancelled"); };
  const rt::ByteStream body(source); const rt::Response response(body, rt::ResponseInit{.status = 502});
  auto reading = model::readPromptCacheScopeExperimentCompletedUsage(response, 1, SCOPE_MODEL, rt::performanceNow(), rt::AbortController().signal());
  EXPECT_REJECTS_MESSAGE(std::move(reading), uos::codex::experiment::CodexCacheScopeExperimentError, "Prompt-cache scope experiment did not receive a readable upstream response.");
  EXPECT_EQ(cancelled, std::vector<std::string>{"cancelled"});
}

ASYNC_TEST("scope experiment: a cancellation failure while discarding the body does not mask the failure reason") {
  CredentialScope isolation; rt::UnderlyingSource<rt::ByteChunk> source; source.cancel = [](rt::Reason) { throw rt::JsError("stream already gone"); };
  const rt::ByteStream body(source); const rt::Response response(body, rt::ResponseInit{.status = 502});
  auto reading = model::readPromptCacheScopeExperimentCompletedUsage(response, 1, SCOPE_MODEL, rt::performanceNow(), rt::AbortController().signal());
  EXPECT_REJECTS_MESSAGE(std::move(reading), uos::codex::experiment::CodexCacheScopeExperimentError, "Prompt-cache scope experiment did not receive a readable upstream response.");
}

ASYNC_TEST("scope experiment: a response without a registered slot is reported as slot drift") {
  CredentialScope isolation; const rt::Response response("data: {}\n\n", rt::ResponseInit{.status = 200, .headers = rt::Headers{{"Content-Type", "text/event-stream"}}});
  auto reading = model::readPromptCacheScopeExperimentCompletedUsage(response, 3, SCOPE_MODEL, rt::performanceNow(), rt::AbortController().signal());
  const auto result = co_await std::move(reading); EXPECT_EQ(std::get<model::ReadSampleResultInconclusive>(result).reason, "slot_drift");
}

ASYNC_TEST("scope experiment: completed terminals classify all telemetry and model refusal paths") {
  CredentialScope isolation;
  std::vector<std::pair<json::value, std::string>> scenarios{{completed(), "sample"}, {changed(completed(), "model", "other-model"), "effective_model_drift"},
    {changed(completed(), "usage", nullptr), "incomplete_telemetry"}};
  auto invalidSize = completed(); invalidSize.as_object()["usage"].as_object()["input_tokens"] = 1'000; invalidSize.as_object()["usage"].as_object()["total_tokens"] = 1'012; invalidSize.as_object()["usage"].as_object()["input_tokens_details"].as_object()["cached_tokens"] = 1'000;
  scenarios.emplace_back(invalidSize, "invalid_input_size");
  auto invalidSignal = completed(); invalidSignal.as_object()["usage"].as_object()["input_tokens_details"].as_object()["cached_tokens"] = 0;
  scenarios.emplace_back(invalidSignal, "invalid_cache_signal");
  for (const auto& [payload, expected] : scenarios) {
    const auto response = responseFor(payload); uos::codex::auth::setCodexResponseAccountTelemetry(response, 1, "fixture-account");
    auto reading = model::readPromptCacheScopeExperimentCompletedUsage(response, 1, SCOPE_MODEL, -120, rt::AbortController().signal());
    const auto result = co_await std::move(reading);
    if (expected == "sample") { const auto& sample = std::get<model::ReadSampleResultSample>(result); EXPECT_EQ(sample.signal, "read"); EXPECT_EQ(sample.sample.slot, 1); EXPECT_EQ(sample.sample.usage.input_tokens, 2'560); }
    else EXPECT_EQ(std::get<model::ReadSampleResultInconclusive>(result).reason, expected);
  }
}

ASYNC_TEST("scope experiment: non-completed and malformed streams retain their exact failures") {
  CredentialScope isolation;
  const auto failed = responseFor(completed(), "response.failed"); uos::codex::auth::setCodexResponseAccountTelemetry(failed, 1, "fixture-account");
  auto reading = model::readPromptCacheScopeExperimentCompletedUsage(failed, 1, SCOPE_MODEL, 0, rt::AbortController().signal());
  EXPECT_REJECTS_MESSAGE(std::move(reading), uos::codex::experiment::CodexCacheScopeExperimentError, "Prompt-cache scope experiment did not receive a completed terminal response.");
  const rt::Response malformed("data: not-json\n\n"); uos::codex::auth::setCodexResponseAccountTelemetry(malformed, 1, "fixture-account");
  auto malformedReading = model::readPromptCacheScopeExperimentCompletedUsage(malformed, 1, SCOPE_MODEL, 0, rt::AbortController().signal());
  bool rejected = false;
  try { auto ignored = co_await std::move(malformedReading); (void)ignored; }
  catch (const uos::codex::experiment::CodexCacheScopeExperimentError& error) { rejected = rt::startsWith(error.message, "Prompt-cache scope experiment stream failed: "); }
  EXPECT(rejected);
}

TEST("scope experiment: durable key fences leases and reusable request shape retain source behavior") {
  auto target = targetBinding(); EXPECT_EQ(model::stateKey(target).size(), 9U); EXPECT_EQ(model::campaignLeaseKey(target).size(), 7U);
  auto other = target; other.model = "other"; EXPECT(model::stateKey(target) != model::stateKey(other)); EXPECT_EQ(model::campaignLeaseKey(target), model::campaignLeaseKey(other));
  other.catalog_versionstamp = "new-catalog"; EXPECT(model::sameTargetDefinition(target, other) == false); other = target; other.catalog_versionstamp = "new-catalog";
  EXPECT(model::sameTargetDefinition(target, other));
  const auto active = state(); const json::value lease = json::object{{"owner", "owner-1"}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 1'000)}};
  EXPECT(model::ownsLease(lease, "owner-1")); EXPECT(!model::ownsLease(lease, "other"));
  auto campaign = lease; campaign.as_object()["target_id"] = "target-1"; campaign.as_object()["inventory_fingerprint"] = "inventory-1";
  EXPECT(model::ownsCampaignLease(campaign, active)); campaign.as_object()["inventory_fingerprint"] = "drift"; EXPECT(!model::ownsCampaignLease(campaign, active));
  const auto request = model::buildExperimentRequest(SCOPE_MODEL, "cycle", "key"); EXPECT_EQ(rt::getString(request, "model"), std::optional<std::string_view>(SCOPE_MODEL));
  EXPECT_EQ(rt::getBool(request, "store"), std::optional<bool>(false)); EXPECT_EQ(rt::getBool(request, "stream"), std::optional<bool>(true));
  const auto& prefix = request.at("input").as_array()[0].as_object().at("content").as_array()[0].as_object().at("text");
  EXPECT(rt::startsWith(std::string_view(prefix.as_string()), "cache-scope-cycle:cycle\n\ncache cache"));
}

TEST("scope experiment: stored undefined members retain exact-key and optional-field oracle semantics") {
  rt::KvValue target(json::value_from(targetBinding())); target.asObject().set("extra", rt::KvUndefined{});
  EXPECT(!model::parseTargetBinding(target));
  rt::KvValue active(json::value_from(state())); active.asObject().set("pending_scope", rt::KvUndefined{});
  EXPECT(model::parseState(active)); active.asObject().set("extra", rt::KvUndefined{}); EXPECT(!model::parseState(active));
}
