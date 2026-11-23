// Links: tests/admin_index_harness_test.cpp
// tests/helpers/kv_budget_harness.cpp src/config.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_backfill.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp tests/helpers/admin_auth_harness.cpp src/kv.cpp
// src/authority/native_paid_policy.cpp src/paid_fallback/ledger_state.cpp src/provider/capacity_events.cpp src/storage/kv.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_admission.cpp src/codex/banked_reset.cpp src/utils.cpp
// src/storage/native_kv.cpp src/api_keys.cpp src/types.cpp src/authority/native_api_key_policy.cpp
// src/storage/sqlite_readonly.cpp src/api_key_policy.cpp src/provider/metered.cpp
// src/provider/capacity_contract.cpp src/runtime_config.cpp src/authority/native_contracts.cpp src/authority/native_api_key.cpp
// src/codex/auth.cpp src/provider/anthropic_responses.cpp src/models/codex_models.cpp src/codex/capacity_routing.cpp
// src/provider/surplus.cpp src/authority/native_codex_capacity.cpp src/kv_conversion/profile_json.cpp src/defaults.cpp
// src/codex/external_auth.cpp src/codex/routing_mutations.cpp src/models/codex_models_fetch.cpp src/provider/surplus_claude_request.cpp
// src/paid_fallback/ledger_stats.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_mutations.cpp src/http.cpp
// src/codex/routing_state.cpp src/codex/reset_settings.cpp src/codex/dispatch.cpp src/codex/auth_refresh.cpp
// src/provider/anthropic_replay.cpp src/responses_stream.cpp src/bounded_response_body.cpp src/release.cpp
// src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/provider/cerebras_rate_limits.cpp src/models/codex_models_availability.cpp
// src/provider/capacity.cpp src/provider/capacity_history.cpp src/provider/claude_code_accounts.cpp src/codex/experiment.cpp
// src/provider/capacity_rollups.cpp src/provider/claude_code_quota.cpp src/provider/capacity_parse.cpp src/provider/anthropic_response_wire.cpp
// src/provider/claude_capacity_history.cpp src/provider/health.cpp src/provider/claude_code_credentials.cpp src/runtime/websocket.cpp
// src/provider/claude_capacity_rollups.cpp src/codex/rate_limit_429.cpp src/provider/capacity_analytics.cpp src/codex/banked_reset_provider.cpp
// src/authority/native_codex_429.cpp src/provider/anthropic_chat_responses.cpp src/provider/surplus_claude_chat_request.cpp src/authority/native_codex_evaluation.cpp
// src/metered_quota.cpp src/codex/account_routing.cpp src/codex/routing_evaluation.cpp src/cache/prompt_analytics_read.cpp
// src/provider/anthropic_chat_stream.cpp src/cache/prompt_analytics_core.cpp src/codex/routing_serial.cpp src/auth/relay.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/selection.cpp src/codex/overage_settings.cpp src/authority/native_codex.cpp
// Isolated Bun-recorded observations for the helper files directly importing admin/index.ts.
// Source oracles block real KV and live fetches; all credentials below are synthetic.
#include "tests/helpers/admin_auth_harness.hpp"
#include "tests/helpers/kv_budget_harness.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json;
  namespace h = uos::tests::helpers::admin_auth_harness;
  namespace b = uos::tests::helpers::kv_budget_harness;
#include "admin_index_harness_vectors.inc"
  const json::value& field(const json::value& value, std::string_view key) { const auto* result = rt::get(value, key); if (!result) throw rt::JsError("missing test field: " + std::string(key)); return *result; }
  const json::value& expected(std::string_view group, std::string_view key) { static const auto value = rt::jsonParse(HARNESS_ORACLE); return field(field(value, group), key); }
  json::array keysJson(const std::vector<rt::KvKey>& keys) { json::array result; for (const auto& key : keys) result.push_back(rt::kvValueFromKey(key).toJson()); return result; }
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w08c2-harness"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    Fixture() { rt::globalDateNow() = [] { return 1800000000000.0; }; uos::config::config.codexAuthJsonB64.clear(); h::resetHarness(); b::kv->values.clear(); b::kv->resetCounts(); static_cast<void>(b::now()); }
    ~Fixture() { h::cleanupHarness(); b::cleanupHarness(); uos::config::config.codexAuthJsonB64 = saved; }
  };
  template<class T> rt::Task<std::string> errorOf(rt::Task<T> task) { std::string result; try { static_cast<void>(co_await std::move(task)); } catch (...) { result = rt::errorMessage(std::current_exception()).value_or(""); } co_return result; }
  template<class F> std::string synchronousError(F callback) { try { callback(); } catch (...) { return rt::errorMessage(std::current_exception()).value_or(""); } return ""; }
}
ASYNC_TEST("admin auth harness exposes source constants, request builders and metered metadata") {
  Fixture fixture;
  const json::array constants{h::DISABLED_FALLBACK_TOKEN, h::ENABLED_FALLBACK_TOKEN, h::FAILED_FALLBACK_TOKEN, h::authPayload}; EXPECT(rt::deepEqual(constants, expected("admin-harness-oracle", "constants")));
  EXPECT_EQ(h::stringEntryLine(rt::KvKey{"probe", 2, true}, "text"), rt::toString(expected("admin-harness-oracle", "line")));
  const auto request = h::makeRequest(json::object{{"tokens", json::object{{"access_token", "synthetic"}}}}); AWAIT(body, request.json()); const json::object requestValue{{"url", request.url()}, {"method", request.method()}, {"contentType", request.headers().get("Content-Type").value_or("")}, {"body", body}}; EXPECT(rt::deepEqual(requestValue, expected("admin-harness-oracle", "request")));
  const auto ratioResponse = h::meteredMetadataResponse("https://api.openlux.ai/api/ratio_config"); AWAIT(ratio, ratioResponse.json()); const auto statusResponse = h::meteredMetadataResponse("https://api.openlux.ai/api/status"); AWAIT(status, statusResponse.json()); EXPECT(rt::deepEqual(json::array{ratio, status}, expected("admin-harness-oracle", "metadata"))); EXPECT_EQ(synchronousError([] { h::meteredMetadataResponse("https://fixture.invalid"); }), rt::toString(expected("admin-harness-oracle", "metadataError")));
}
TEST("admin auth harness seeds the real runtime configuration and clears shared caches") {
  Fixture fixture; const auto snapshot = expected("admin-harness-oracle", "seededSnapshot").as_object(); h::seedCodexSnapshot(snapshot);
  const auto* storedSnapshot = h::kvStore.get(h::keyToString(rt::KvKey{"ubq_ai", "codex_models"})); ASSERT(storedSnapshot); EXPECT(rt::deepEqual(storedSnapshot->toJson(), expected("admin-harness-oracle", "seededSnapshot"))); const auto* storedRuntime = h::kvStore.get(h::keyToString(rt::KvKey{"uos_ai", "runtime_config", "v2"})); ASSERT(storedRuntime); EXPECT(rt::deepEqual(storedRuntime->toJson(), expected("admin-harness-oracle", "seededRuntime")));
  h::kvStore.clear(); EXPECT(rt::deepEqual(json::object{{"entries", h::kvStore.size()}, {"atomicCalls", h::atomicSetCalls.size()}}, expected("admin-harness-oracle", "cleared")));
}
TEST("KV budget encodings preserve the source JSON, byte and BigInt forms") {
  Fixture fixture; const rt::KvValue fingerprint = rt::KvValue::object({{"n", rt::KvValue(rt::BigInt(5))}, {"array", rt::KvValue::array({rt::KvValue(rt::BigInt(1)), nullptr})}});
  const json::object output{{"key", b::encodeKey(rt::KvKey{"probe", 2, true})}, {"base64", b::encodeBase64Url(rt::Bytes{0, 127, 128, 255})}, {"json", b::encodeJsonBase64Url(json::object{{"x", "😀"}, {"n", 2}})}, {"pem", b::toPublicKeyPem(rt::Bytes{1, 2, 3})}, {"fingerprint", b::kvFingerprint(fingerprint)}}; EXPECT(rt::deepEqual(output, expected("kv-budget-oracle", "encoding")));
}
ASYNC_TEST("KV budget store matches source revisions, CAS, counters, sums and reset quirks") {
  Fixture fixture; auto store = std::make_shared<b::CountingKv>(); const rt::KvKey key{"probe", 1}; store->values.set(b::encodeKey(key), json::object{{"x", 1}}); AWAIT(first, store->get(key)); store->values.get(b::encodeKey(key))->asObject().set("x", rt::KvValue(2)); AWAIT(second, store->get(key)); auto staleOperation = store->atomic(); staleOperation->check(first).set(rt::KvKey{"stale"}, rt::KvValue(true)); AWAIT(stale, staleOperation->commit()); auto acceptedOperation = store->atomic(); acceptedOperation->check(second).set(rt::KvKey{"written"}, rt::KvValue(json::object{{"n", 3}})); AWAIT(accepted, acceptedOperation->commit()); const std::vector<rt::KvKey> requested{key, rt::KvKey{"missing"}}; AWAIT(many, store->getMany(requested)); json::array entries; for (const auto& row : many) entries.push_back(json::object{{"key", rt::kvValueFromKey(row.key).toJson()}, {"value", row.value.toJson()}, {"versionstamp", row.versionstamp ? json::value(*row.versionstamp) : json::value(nullptr)}});
  const json::object cas{{"first", first.versionstamp.value_or("")}, {"second", second.versionstamp.value_or("")}, {"stale", stale.ok}, {"accepted", accepted.ok}, {"many", entries}, {"reads", store->reads}, {"readUnits", store->readUnits}, {"writes", store->writes}, {"readKeys", keysJson(store->readKeys)}, {"writeKeys", keysJson(store->writeKeys)}}; EXPECT(rt::deepEqual(cas, expected("kv-budget-oracle", "cas")));
  store->failNextSumCommits = 1; auto failedSum = store->atomic(); failedSum->sum(rt::KvKey{"sum"}, 2); AWAIT(sumError, errorOf(failedSum->commit())); auto successfulSum = store->atomic(); successfulSum->sum(rt::KvKey{"sum"}, 2); AWAIT(sumResult, successfulSum->commit()); EXPECT(sumResult.ok); AWAIT(summed, store->get(rt::KvKey{"sum"})); EXPECT(rt::deepEqual(json::object{{"error", sumError}, {"value", summed.value.asU64().toBigInt().toString()}, {"attempts", store->sumCommitAttempts}, {"sums", store->sums}, {"writes", store->writes}}, expected("kv-budget-oracle", "sum")));
  store->failNextCommits = 1; auto refused = store->atomic(); refused->set(rt::KvKey{"refused"}, rt::KvValue(true)); AWAIT(failed, refused->commit()); const rt::KvListSelector selector{.prefix = rt::KvKey{"probe"}}; auto iterator = store->list(selector); json::array listed; while (auto row = co_await iterator->next()) listed.push_back(json::object{{"key", rt::kvValueFromKey(row->key).toJson()}, {"value", row->value.toJson()}}); EXPECT(rt::deepEqual(json::object{{"failed", failed.ok}, {"rows", listed}, {"calls", store->listCalls}}, expected("kv-budget-oracle", "list")));
  store->failApiKeyV3Reads = true; AWAIT(readError, errorOf(store->get(rt::KvKey{"uos_ai", "api_key_usage", "v3", "x"}))); store->resetCounts(); EXPECT(rt::deepEqual(json::object{{"reads", store->reads}, {"writes", store->writes}, {"readError", readError}, {"preservedApiKeyFailure", store->failApiKeyV3Reads}, {"entries", store->values.size()}}, expected("kv-budget-oracle", "reset")));
}
ASYNC_TEST("KV budget gates keep reads and dispatch mutations suspended until explicitly released") {
  Fixture fixture; auto store = std::make_shared<b::CountingKv>(); const auto gate = b::deferred(); store->kernelQuotaReadGate = gate.promise; const rt::KvKey key{"uos_ai", "kernel_quota", "v2", "repo_policy", "acme", "demo"}; rt::Promise<rt::KvEntryMaybe<>> reading(store->get(key)); EXPECT_EQ(store->reads, 0); gate.resolve(); const auto row = co_await reading.wait(); EXPECT(row.value.isNull()); EXPECT_EQ(store->reads, 1);
  const auto dispatchGate = b::deferred(); store->apiKeyV3DispatchCommitGate = dispatchGate.promise; double callbacks = 0; store->onApiKeyV3DispatchCommit = [&] { ++callbacks; }; auto operation = store->atomic(); const rt::KvKey requestKey{"uos_ai", "api_key_usage", "v3", "request", "fixture"}; operation->set(requestKey, rt::KvValue(json::object{{"state", "dispatched"}})); rt::Promise<rt::KvCommitResult> committing(operation->commit()); EXPECT_EQ(callbacks, 1); EXPECT_EQ(store->writes, 0); dispatchGate.resolve(); const auto result = co_await committing.wait(); EXPECT(result.ok); EXPECT_EQ(store->writes, 1);
  store->failKernelQuotaReads = true; AWAIT(readError, errorOf(store->get(key))); EXPECT_EQ(readError, "injected Kernel quota read failure");
}
ASYNC_TEST("KV budget reservation hooks and settlement failures preserve atomic state") {
  Fixture fixture; auto store = std::make_shared<b::CountingKv>(); double hooks = 0; store->onKernelReservationCommit = [&] { ++hooks; }; const rt::KvKey key{"uos_ai", "kernel_quota", "v2", "repo_reservation", "fixture"}; auto reserved = store->atomic(); reserved->set(key, rt::KvValue(json::object{{"state", "reserved"}})); AWAIT(reservation, reserved->commit()); EXPECT(reservation.ok); EXPECT_EQ(hooks, 1); EXPECT(!store->onKernelReservationCommit);
  store->failNextKernelSettlementCommits = 1; auto settlement = store->atomic(); settlement->set(key, rt::KvValue(json::object{{"state", "committed"}})); AWAIT(error, errorOf(settlement->commit())); EXPECT_EQ(error, "injected Kernel quota settlement failure"); EXPECT_EQ(store->values.get(b::encodeKey(key))->operator[]("state").asString(), "reserved"); EXPECT_EQ(store->writes, 1);
  const std::vector<b::CountingKvMutation> mutations{{"sum", {}, rt::KvValue(rt::BigInt(1))}, {"set", key, rt::KvValue(json::object{{"state", "reserved"}})}, {"set", key, rt::KvValue(json::object{{"state", "released"}})}, {"set", rt::KvKey{"uos_ai", "api_key_usage", "v3", "request"}, rt::KvValue(json::object{{"state", "dispatched"}})}}; const json::array predicates{b::isSumMutation(mutations[0]), b::isKernelReservationMutation(mutations[1]), b::isKernelSettlementMutation(mutations[2]), b::isApiKeyV3DispatchMutation(mutations[3])}; EXPECT(rt::deepEqual(predicates, expected("kv-budget-oracle", "predicates")));
}
ASYNC_TEST("KV budget request, SSE and authoritative quota fixtures match source bytes") {
  Fixture fixture; const auto request = b::request("synthetic-token"), streaming = b::streamingRequest("synthetic-token", "chat"); AWAIT(body, request.json()); AWAIT(streamBody, streaming.json()); EXPECT(rt::deepEqual(json::array{json::object{{"url", request.url()}, {"method", request.method()}, {"body", body}}, json::object{{"url", streaming.url()}, {"method", streaming.method()}, {"body", streamBody}}}, expected("kv-budget-oracle", "requests"))); const auto response = b::sse(); AWAIT(event, response.text()); EXPECT(rt::deepEqual(json::array{b::completedSseEvent(3, 4), b::semanticSseEvent("text"), event}, expected("kv-budget-oracle", "events")));
  const auto limited = b::authoritativeCodexQuotaResponse(); AWAIT(limitedBody, limited.json()); EXPECT(rt::deepEqual(json::object{{"status", limited.status()}, {"retry", limited.headers().get("Retry-After").value_or("")}, {"body", limitedBody}}, expected("kv-budget-oracle", "limited"))); const auto stream = b::validStreamingSse(); AWAIT(streamText, stream.text()); EXPECT(rt::includes(streamText, "response.created")); EXPECT(rt::includes(streamText, "response.output_text.delta")); EXPECT(rt::includes(streamText, "response.completed"));
}
ASYNC_TEST("KV budget key setup uses real source policy and deterministic default rows") {
  Fixture fixture; b::resetHarness(); AWAIT(prepared, b::prepareApiKeyInference("a", "key-fixture", 5)); const json::object actual{{"token", prepared.token}, {"hash", prepared.hash}, {"record", prepared.record}, {"policy", json::value_from(prepared.policy)}}; EXPECT(rt::deepEqual(actual, expected("kv-budget-oracle", "prepared"))); b::seedKernelDefaultLimit(7, 30000); const auto* limit = b::kv->values.get(b::encodeKey(b::DEFAULT_KERNEL_POLICY_LIMIT_KEY)); const auto* window = b::kv->values.get(b::encodeKey(b::DEFAULT_KERNEL_POLICY_WINDOW_KEY)); ASSERT(limit && window); EXPECT(rt::deepEqual(json::array{limit->toJson(), window->toJson()}, expected("kv-budget-oracle", "defaults")));
  const rt::KvKey key = uos::api_key_policy::apiKeyUsageV3WindowKey(prepared.policy); const json::object counters{{"committed_requests", 2}, {"reserved_requests", 1}, {"window_reset_at_ms", 1800000060000.0}}; b::kv->values.set(b::encodeKey(key), counters); EXPECT(rt::deepEqual(b::usageWindow(prepared.policy), counters)); AWAIT(paid, b::seedPaidFallbackKey("synthetic-paid-token", "paid-key")); EXPECT_EQ(rt::getBool(paid.record, "paid_fallback_enabled"), std::optional(true)); EXPECT(b::kv->values.has(b::encodeKey(rt::KvKey{"ubq_ai", "api_keys", "id", "paid-key"})));
}
ASYNC_TEST("KV budget kernel fixtures sign and verify the original RS256 claims") {
  Fixture fixture; b::resetHarness(); co_await b::seedKernelTestPublicKey(); AWAIT(token, b::makeKernelTestToken("synthetic-token", "acme", "demo")); const auto parts = rt::split(token, "."); ASSERT_EQ(parts.size(), std::size_t(3)); const auto header = rt::jsonParse(rt::decodeUtf8(rt::fromBase64(parts[0]))); auto claims = rt::jsonParse(rt::decodeUtf8(rt::fromBase64(parts[1]))); EXPECT(rt::startsWith(rt::toString(field(claims, "jti")), "jti_")); EXPECT(rt::startsWith(rt::toString(field(claims, "state_id")), "state_")); rt::objectDelete(claims.as_object(), "jti"); rt::objectDelete(claims.as_object(), "state_id"); const auto signature = rt::fromBase64(parts[2]); const bool verified = rt::crypto::subtle::verify("RSASSA-PKCS1-v1_5", b::kernelTestKeyPair().publicKey, signature, rt::encodeUtf8(parts[0] + "." + parts[1])); const auto* stored = b::kv->values.get(b::encodeKey(rt::KvKey{"uos_ai", "kernel_pubkeys"})); ASSERT(stored); EXPECT(rt::deepEqual(json::object{{"header", header}, {"claims", claims}, {"verified", verified}, {"pemPrefix", rt::startsWith(stored->asArray()[0]["pem"].asString(), "-----BEGIN PUBLIC KEY-----\n")}}, expected("kv-budget-oracle", "kernel")));
  const auto original = b::request("synthetic-token"); AWAIT(withToken, b::withKernelTestToken(original, "synthetic-token", "acme", "demo")); EXPECT(withToken.headers().has("X-Ubiquity-Kernel-Token")); EXPECT_EQ(withToken.headers().get("Authorization"), original.headers().get("Authorization"));
}
ASYNC_TEST("KV budget bounded waits and terminal timing validation preserve source failures") {
  Fixture fixture; bool ready = false; const auto timer = rt::setTimeout([&] { ready = true; }, 12); static_cast<void>(timer); auto predicate = [&] { return ready; }; auto waiting = b::waitFor(predicate, "ready", 100); co_await std::move(waiting); auto never = [] { return false; }; AWAIT(timeout, errorOf(b::waitFor(never, "fixture", 10))); EXPECT_EQ(timeout, "Timed out waiting for fixture");
  const json::object times{{"first_codex_dispatch_ms", 1}, {"first_codex_headers_ms", 2}, {"first_upstream_sse_event_ms", 3}, {"first_semantic_commitment_ms", 4}, {"stream_terminal_ms", 5}, {"latency_ms", 9}, {"downstream_drain_ms", 3}}; b::assertOrderedTerminalTimings(times, true); const auto error = synchronousError([] { b::requiredTerminalTiming(json::object{{"latency_ms", -1}}, "latency_ms"); }); EXPECT(rt::deepEqual(json::object{{"valid", b::requiredTerminalTiming(times, "latency_ms")}, {"error", error}}, expected("kv-budget-oracle", "timings"))); auto withoutDrain = times; withoutDrain["downstream_drain_ms"] = nullptr; b::assertOrderedTerminalTimings(withoutDrain, false); EXPECT_THROWS(b::requiredTerminalTiming(json::object(), "latency_ms"), rt::JsError);
}
