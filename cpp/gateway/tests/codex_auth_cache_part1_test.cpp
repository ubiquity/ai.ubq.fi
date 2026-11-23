// Links: tests/codex_auth_cache_part1_test.cpp tests/helpers/codex_auth_cache_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-auth-cache-part1.test.ts: all 26 original cases and assertions.
#include "tests/helpers/codex_auth_cache_harness.hpp"
#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <set>
#include <numeric>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace h = uos::tests::helpers::codex_auth_cache_harness;
  namespace ca = uos::codex::auth;
  namespace cr = uos::codex::account_routing;
  namespace rs = uos::codex::routing_state;
  namespace cd = uos::codex::dispatch;
  using namespace h;
  using uos::codex::index::fetchCodexResponses;
  using Deferred = uos::tests::helpers::codex_banked_reset_harness::Deferred<void>;
  using ca::markCodexResponseCompleted;
  using ca::resetCodexAuthCacheForTest;
  using cr::resetCodexAccountRoutingForTest;
  using ca::CodexError;
  using uos::types::CodexAuthState;

  rt::Response respond(const std::string& body = "{}", double status = 200, rt::Headers headers = {}) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = headers;
    return rt::Response(rt::BodyInit(body), init);
  }
  rt::Response jsonResponse(const json::value& value, double status = 200, rt::Headers headers = {}) {
    headers.set("Content-Type", "application/json");
    return respond(rt::jsonStringify(value), status, headers);
  }
  json::value input(const std::string& text) { return json::object{{"input", text}}; }
  json::value modelInput(const std::string& text) { return json::object{{"model", "gpt-5.6-luna"}, {"input", text}}; }
  rt::Headers requestHeaders(const rt::RequestInit& init) { return init.headers.value_or(rt::Headers{}); }
  std::string accountId(const rt::RequestInit& init) { return requestHeaders(init).get("chatgpt-account-id").value_or(""); }
  bool oauth(const rt::RequestInfo& info) { return rt::includes(requestUrl(info), "auth.openai.com/oauth/token"); }
  std::string serialized(const rt::RequestInit& init) {
    if (!init.body || !std::holds_alternative<std::string>(*init.body)) throw rt::JsError("Expected Codex request body to be a serialized string.");
    return std::get<std::string>(*init.body);
  }
  template<class T> rt::Task<rt::Reason> failureOf(rt::Task<T> task) {
    rt::Promise<T> pending(std::move(task));
    try { (void) co_await pending.wait(); }
    catch (...) { co_return std::current_exception(); }
    co_return nullptr;
  }
  rt::Task<void> resolvedSleep(double) { return rt::Promise<void>::resolve().wait(); }
  rt::Response generic429() {
    return jsonResponse(json::object{{"error", json::object{{"type", "rate_limit_error"}}}}, 429, rt::Headers{{"Retry-After", "1"}});
  }
  std::vector<std::string> ids(const std::vector<CodexAuthState>& accounts) {
    std::vector<std::string> result;
    for (const auto& candidate : accounts) result.push_back(candidate.account_id);
    return result;
  }
  const rs::RouteSelectionEligible& eligible(const rs::RouteSelection& selection) {
    if (const auto* result = std::get_if<rs::RouteSelectionEligible>(&selection)) return *result;
    throw rt::JsError("expected eligible routing selection", "AssertionError");
  }
  rt::Response cancellableBody(const std::string& text, bool& cancellationStarted) {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [text](auto controller) { controller.enqueue(rt::encodeUtf8(text)); };
    source.cancel = [&cancellationStarted](rt::Reason) -> rt::Task<void> {
      cancellationStarted = true;
      auto pending = rt::withResolvers<void>();
      co_await pending.promise.wait();
    };
    rt::ByteStream stream(source);
    rt::ResponseInit init;
    init.status = 401;
    return rt::Response(rt::BodyInit(stream), init);
  }
}
#define AWAIT(name, expression) rt::Promise name##Pending((expression)); auto name = co_await name##Pending.wait()
#define WAIT(expression) do { rt::Promise pending((expression)); co_await pending.wait(); } while (false)

TEST("Codex auth account ordering rotates from the selected account") {
  const std::vector<CodexAuthState> accounts{auth("one"), auth("two")};
  EXPECT_EQ(ids(cd::orderCodexAuthAccounts(accounts, 0)), (std::vector<std::string>{"account-one", "account-two"}));
  EXPECT_EQ(ids(cd::orderCodexAuthAccounts(accounts, 1)), (std::vector<std::string>{"account-two", "account-one"}));
}

ASYNC_TEST("repeated requests preserve subscription account order") {
  HarnessScope scope;
  kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); return respond();
  }));
  for (int iteration = 0; iteration < 2; ++iteration) {
    AWAIT(response, fetchCodexResponses(modelInput("stable routing order")));
    EXPECT_EQ(response.status(), 200);
    WAIT(markCodexResponseCompleted(response));
  }
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-one"}));
}

ASYNC_TEST("AuthKv strong getMany snapshots auth and routing rows before a routing-read hook mutates them") {
  HarnessScope scope;
  auto fixture = std::make_shared<AuthKv>(pool(auth("old")));
  const auto routingKey = cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
  json::value initialRoutingRow = json::object{{"v", 1}, {"generation", 1}};
  json::value rotatedRoutingRow = json::object{{"v", 1}, {"generation", 2}};
  fixture->extra.set(keyOf(routingKey), AuthKv::ExtraEntry{rt::KvValue(initialRoutingRow), 3});
  const auto rotatedAuth = pool(auth("rotated"));
  bool mutated = false;
  fixture->onRoutingRead = [&](double) {
    if (!mutated) {
      mutated = true;
      fixture->auth = rotatedAuth;
      ++fixture->authVersion;
      fixture->extra.set(keyOf(routingKey), AuthKv::ExtraEntry{rt::KvValue(rotatedRoutingRow), 4});
    }
    return rt::fulfilledTask();
  };
  std::vector<rt::KvKey> keys{AUTH_KEY, routingKey};
  rt::KvGetOptions options{rt::KvConsistencyLevel::strong};
  AWAIT(snapshot, fixture->getMany(keys, options));
  EXPECT(mutated);
  EXPECT(rt::deepEqual(snapshot[0].value.toJson(), json::value_from(pool(auth("old")))));
  EXPECT_EQ(snapshot[0].versionstamp, std::optional<std::string>("00000000000000000001"));
  EXPECT(rt::deepEqual(snapshot[1].value.toJson(), initialRoutingRow));
  EXPECT_EQ(snapshot[1].versionstamp, std::optional<std::string>("00000000000000000003"));
  AWAIT(fresh, fixture->getMany(keys, options));
  EXPECT(rt::deepEqual(fresh[0].value.toJson(), json::value_from(rotatedAuth)));
  EXPECT_EQ(fresh[0].versionstamp, std::optional<std::string>("00000000000000000002"));
  EXPECT(rt::deepEqual(fresh[1].value.toJson(), rotatedRoutingRow));
  EXPECT_EQ(fresh[1].versionstamp, std::optional<std::string>("00000000000000000004"));
  EXPECT_EQ(fixture->reads, 2);
  EXPECT_EQ(fixture->routingReads, 2);
  fixture->onRoutingRead = {};
}

ASYNC_TEST("retired affinity rows are never written by terminal completion") {
  HarnessScope scope;
  kv->auth = pool(auth("one"), auth("two"));
  rt::KvKey retiredKey{"uos_ai", "codex_account_affinity", "v1", "retired-terminal-fixture"};
  json::value row = json::object{{"account_cohort_hash", std::string("uos-prompt-cache-account-cohort-v1") + std::string(1, char(0)) + "account-one"}, {"expires_at_ms", fixedStartMs + 60'000}};
  WAIT(kv->set(retiredKey, rt::KvValue(row)));
  const auto before = *kv->extra.get(keyOf(retiredKey));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return respond(); }));
  json::value body = json::object{{"input", "terminal affinity"}, {"prompt_cache_key", "retired-terminal-key"}};
  cd::FetchCodexResponsesOptions options;
  options.cacheScope = "api-key:terminal-affinity";
  AWAIT(response, fetchCodexResponses(body, options));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"}));
  EXPECT_EQ(*kv->extra.get(keyOf(retiredKey)), before);
  WAIT(markCodexResponseCompleted(response));
  EXPECT_EQ(*kv->extra.get(keyOf(retiredKey)), before);
  EXPECT_EQ(ca::getCodexResponseActiveTelemetry(response), (ca::CodexResponseActiveTelemetry{1, std::nullopt}));
}

ASYNC_TEST("a durable routing-state outage fails retryably before dispatch") {
  HarnessScope scope;
  kv->auth = pool(auth("one"));
  ca::cacheCodexAuthPool(kv->auth);
  uos::kv::setKvForTest(nullptr);
  int providerCalls = 0, beforeDispatchCalls = 0, transportStarts = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++providerCalls; return respond(); }));
  cd::FetchCodexResponsesOptions options;
  options.beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([&] {
    ++beforeDispatchCalls;
    uos::api_key_policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [&] { ++transportStarts; };
    dispatch.cancelBeforeTransport = [] { return rt::fulfilledTask(); };
    return rt::fulfilledTask(std::optional(dispatch));
  });
  AWAIT(response, fetchCodexResponses(modelInput("wait for KV"), options));
  EXPECT_EQ(response.status(), 503);
  EXPECT_EQ(providerCalls, 0); EXPECT_EQ(beforeDispatchCalls, 0); EXPECT_EQ(transportStarts, 0);
  WAIT(response.arrayBuffer());
  installHarness();
}

ASYNC_TEST("concurrent Codex requests dispatch without gateway admission rejection") {
  HarnessScope scope;
  kv->auth = pool(auth("one"), auth("two"));
  int providerCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    ++providerCalls; co_await rt::sleep(5); co_return jsonResponse(json::object{});
  }));
  std::vector<rt::Promise<rt::Response>> requests;
  for (int index = 0; index < 8; ++index) requests.emplace_back(fetchCodexResponses(modelInput("agent-" + rt::numberToString(index))));
  auto responses = co_await rt::all(std::move(requests)).wait();
  EXPECT_EQ(providerCalls, 8);
  for (const auto& response : responses) { EXPECT_EQ(response.status(), 200); WAIT(markCodexResponseCompleted(response)); }
}

ASYNC_TEST("Codex responses use the native prompt-cache wire contract and stable keyed sessions") {
  HarnessScope scope;
  kv->auth = pool(auth("one"));
  std::vector<rt::Headers> headers;
  std::vector<json::value> bodies;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    headers.push_back(requestHeaders(init)); bodies.push_back(rt::jsonParse(serialized(init))); return respond();
  }));
  json::value body = rt::jsonParse(R"({"model":"gpt-5.6-luna","prompt_cache_key":"stable-cache-key","prompt_cache_options":{"mode":"explicit","ttl":"30m"},"prompt_cache_retention":"24h","max_output_tokens":64,"max_completion_tokens":64,"input":[{"type":"message","role":"user","content":[{"type":"input_text","text":"stable prefix","prompt_cache_breakpoint":{"mode":"explicit"}}]}],"tools":[{"type":"function","name":"cache_schema_fixture","parameters":{"type":"object","properties":{"prompt_cache_breakpoint":{"type":"string"}}}}]})");
  const auto original = body;
  cd::FetchCodexResponsesOptions principalOne, principalTwo;
  principalOne.cacheScope = "principal-one"; principalTwo.cacheScope = "principal-two";
  AWAIT(first, fetchCodexResponses(body, principalOne));
  AWAIT(second, fetchCodexResponses(body, principalOne));
  auto changed = body; changed.at("prompt_cache_key") = "different-cache-key";
  AWAIT(differentKey, fetchCodexResponses(changed, principalOne));
  AWAIT(differentPrincipal, fetchCodexResponses(body, principalTwo));
  AWAIT(noScope1, fetchCodexResponses(body));
  AWAIT(noScope2, fetchCodexResponses(body));
  AWAIT(noKey1, fetchCodexResponses(modelInput("no key"), principalOne));
  AWAIT(noKey2, fetchCodexResponses(modelInput("no key"), principalOne));
  EXPECT(rt::deepEqual(body, original)); EXPECT_EQ(headers.size(), std::size_t{8});
  const auto& firstBody = bodies[0];
  EXPECT_EQ(rt::getString(firstBody, "prompt_cache_key"), std::optional<std::string_view>("stable-cache-key"));
  for (const std::string key : {"prompt_cache_options", "prompt_cache_retention", "max_output_tokens", "max_completion_tokens"}) EXPECT(!rt::hasOwn(firstBody.as_object(), key));
  EXPECT(!rt::hasOwn(firstBody.at("input").as_array()[0].at("content").as_array()[0].as_object(), "prompt_cache_breakpoint"));
  EXPECT(rt::deepEqual(firstBody.at("tools").as_array()[0], body.at("tools").as_array()[0]));
  EXPECT(rt::deepEqual(bodies[1], firstBody));
  const auto identity = headers[0].get("conversation_id");
  EXPECT(rt::Regex("^[0-9a-f]{8}-[0-9a-f]{4}-8[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$").test(identity.value_or("")));
  for (const std::string header : {"conversation_id", "session-id", "thread-id", "x-client-request-id"}) {
    EXPECT_EQ(headers[0].get(header), identity); EXPECT_EQ(headers[1].get(header), identity);
    EXPECT(headers[2].get(header) != identity); EXPECT(headers[3].get(header) != identity);
  }
  for (std::size_t index = 4; index < headers.size(); ++index) {
    EXPECT(!headers[index].get("session-id")); EXPECT(!headers[index].get("thread-id")); EXPECT(!headers[index].get("x-client-request-id"));
  }
  EXPECT(headers[4].get("conversation_id") != headers[5].get("conversation_id"));
  EXPECT(headers[6].get("conversation_id") != headers[7].get("conversation_id"));
  const std::vector<std::string> warnings{"prompt_cache_options_ignored", "prompt_cache_retention_ignored", "max_output_tokens_ignored", "prompt_cache_breakpoint_ignored"};
  for (const auto& response : {first, second, differentKey, differentPrincipal}) {
    auto actual = rt::split(response.headers().get("x-uos-warning").value_or(""), ",");
    for (auto& value : actual) value = rt::trim(value);
    EXPECT_EQ(actual, warnings);
  }
}

ASYNC_TEST("Codex responses retry the same active account after an account-level 429") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init));
    if (accountIds.size() > 2) throw rt::JsError("a bounded 429 retry must not dispatch a third attempt");
    return respond("{}", accountIds.size() == 1 ? 429 : 200);
  }));
  AWAIT(response, fetchCodexResponses(input("balance")));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-one"}));
  EXPECT_EQ(accountIds.size(), std::size_t{2}); EXPECT_EQ(ca::getCodexResponseSlot(response), std::optional<double>(1));
}

ASYNC_TEST("Codex responses keep the durable active account over sibling dashboard headroom") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return respond(); }));
  AWAIT(initial, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs));
  const auto& accounts = eligible(initial).accounts;
  const auto found = std::find_if(accounts.begin(), accounts.end(), [](const auto& account) { return account.auth.account_id == "account-two"; });
  ASSERT(found != accounts.end());
  auto blocked = jsonResponse(json::object{{"error", json::object{{"type", "usage_limit_reached"}}}}, 429,
                              rt::Headers{{"Retry-After", rt::Date(fixedStartMs + 60'000).toUTCString()}});
  WAIT(cr::markCodexQuotaBlocked(*found, blocked, fixedStartMs - 1));
  auto snapshot = rt::jsonParse(R"({"snapshot_at_ms":1000000,"sources":[{"source":"codex","slot":2,"state":"available","source_observed_at_ms":1000000,"snapshot_at_ms":1000000,"windows":{"primary":{"limit_window_seconds":604800,"used_percent":100,"reset_at_ms":605800000},"secondary":null},"additional_rate_limits":[{"limit_name":"GPT-5.3-Codex-Spark","metered_feature":"codex_bengalfox","windows":{"primary":{"limit_window_seconds":18000,"used_percent":50,"reset_at_ms":19000000},"secondary":null}}]}]})");
  WAIT(kv->set(uos::provider::capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY, rt::KvValue(snapshot)));
  resetCodexAccountRoutingForTest();
  json::value request = json::object{{"model", "gpt-5.3-codex-spark"}, {"input", "dashboard-positive"}};
  AWAIT(response, fetchCodexResponses(request));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"}));
}

ASYNC_TEST("Codex responses return a raw 403 without sibling failover") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init));
    if (accountIds.size() > 1) throw rt::JsError("a raw 403 with a valid bearer must not dispatch a sibling");
    return respond("{}", 403);
  }));
  AWAIT(response, fetchCodexResponses(input("forbidden-failover")));
  EXPECT_EQ(response.status(), 403); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"}));
}

ASYNC_TEST("Codex responses do not replay a dispatched transport failure") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
    accountIds.push_back(accountId(init));
    if (accountIds.size() == 1) return rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("upstream socket closed", "TimeoutError"))).wait();
    return rt::fulfilledTask(respond());
  }));
  AWAIT(error, failureOf(fetchCodexResponses(input("timeout-sibling-retry"))));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->code, "gateway_timeout");
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"}));
}

ASYNC_TEST("transport failures preserve retired affinity rows and provider health") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  rt::KvKey retiredKey{"uos_ai", "codex_account_affinity", "v1", "retired-transport-fixture"};
  json::value row = json::object{{"account_cohort_hash", std::string("uos-prompt-cache-account-cohort-v1") + std::string(1, char(0)) + "account-one"}, {"expires_at_ms", fixedStartMs + 60'000}};
  WAIT(kv->set(retiredKey, rt::KvValue(row)));
  const auto before = *kv->extra.get(keyOf(retiredKey));
  std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init));
    return rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("upstream socket closed", "TimeoutError"))).wait();
  }));
  cd::FetchCodexResponsesOptions options; options.cacheScope = "api-key:transport-failure-affinity";
  json::value body = json::object{{"input", "all siblings fail"}, {"prompt_cache_key", "transport-failure-affinity-key"}};
  AWAIT(error, failureOf(fetchCodexResponses(body, options)));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->code, "gateway_timeout"); EXPECT_EQ(codexError->status, 504);
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"})); EXPECT_EQ(*kv->extra.get(keyOf(retiredKey)), before);
  const auto* activeRow = kv->extra.get(keyOf(cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); ASSERT(activeRow);
  const auto active = cr::parseCodexActiveAccountSelection(activeRow->value.toJson()); ASSERT(active); EXPECT_EQ(active->generation, 1); EXPECT_EQ(active->slot, 0);
  for (const auto& encoded : kv->extra.keys()) {
    auto key = rt::jsonParse(encoded); EXPECT(key.as_array()[1] != "provider_health");
  }
}

ASYNC_TEST("post-dispatch client cancellation stops Codex transport") {
  HarnessScope scope; kv->auth = pool(auth("one"));
  Deferred transportStarted; rt::AbortController requestAbort;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    if (!init.signal) throw rt::JsError("transport signal missing", "AssertionError");
    const auto signal = *init.signal;
    return rt::newPromise<rt::Response>([&transportStarted, signal](auto, auto reject) {
      transportStarted.resolve();
      rt::EventListener listener([signal, reject]() { reject(abortReasonError(signal.reason())); });
      if (signal.aborted()) reject(abortReasonError(signal.reason()));
      else signal.addEventListener("abort", listener, {.once = true});
    }).wait();
  }));
  cd::FetchCodexResponsesOptions options; options.signal = requestAbort.signal();
  rt::Promise<rt::Response> pending(fetchCodexResponses(input("cancel after dispatch"), options));
  co_await transportStarted.promise.wait();
  requestAbort.abort(rt::makeReason(rt::DOMException("client disconnected", "AbortError")));
  AWAIT(error, failureOf(pending.wait()));
  EXPECT_EQ(rt::errorName(error), std::optional<std::string>("AbortError"));
}

ASYNC_TEST("Codex responses make one bounded same-active retry when a generic 429 repeats") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds, serializedBodies; std::vector<double> delays;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    serializedBodies.push_back(serialized(init)); accountIds.push_back(accountId(init));
    if (accountIds.size() > 2) throw rt::JsError("a generic 429 gets exactly one same-active bounded retry");
    return generic429();
  }));
  cd::FetchCodexResponsesOptions options; options.requestId = "request-bounded-retry";
  options.retrySleep = [&](double milliseconds) { delays.push_back(milliseconds); return rt::Promise<void>::resolve().wait(); };
  AWAIT(response, fetchCodexResponses(input("bounded-retry"), options));
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-one"})); EXPECT_EQ(delays, (std::vector<double>{1000}));
  const auto expected = rt::jsonStringify(input("bounded-retry")); EXPECT_EQ(rt::utf8ByteLength(expected), std::size_t{25});
  EXPECT_EQ(serializedBodies, (std::vector<std::string>{expected, expected}));
  std::size_t total = 0; for (const auto& body : serializedBodies) { EXPECT_EQ(rt::utf8ByteLength(body), std::size_t{25}); total += rt::utf8ByteLength(body); }
  EXPECT_EQ(total, std::size_t{50}); WAIT(response.arrayBuffer());
}

ASYNC_TEST("Codex 429 retry sleep normalizes a shared timeout as a gateway timeout") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds; rt::AbortController controller;
  const auto timeoutReason = rt::makeReason(rt::DOMException("request deadline exceeded", "TimeoutError"));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return generic429(); }));
  cd::FetchCodexResponsesOptions options; options.signal = controller.signal();
  options.retrySleep = [&](double) {
    rt::queueMicrotask([&] { controller.abort(timeoutReason); });
    return rt::withResolvers<void>().promise.wait();
  };
  AWAIT(error, failureOf(fetchCodexResponses(input("retry-timeout"), options)));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->code, "gateway_timeout");
  EXPECT_EQ(codexError->status, 504); EXPECT(codexError->causeValue == timeoutReason);
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"})); EXPECT_EQ(accountIds.size(), std::size_t{1});
}

ASYNC_TEST("Codex 429 retry sleep preserves ordinary cancellation") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  std::vector<std::string> accountIds; rt::AbortController controller;
  const auto abortReason = rt::makeReason(rt::DOMException("client disconnected", "AbortError"));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return generic429(); }));
  cd::FetchCodexResponsesOptions options; options.signal = controller.signal();
  options.retrySleep = [&](double) {
    rt::queueMicrotask([&] { controller.abort(abortReason); }); return rt::withResolvers<void>().promise.wait();
  };
  AWAIT(error, failureOf(fetchCodexResponses(input("retry-cancelled"), options)));
  EXPECT(error == abortReason); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"})); EXPECT_EQ(accountIds.size(), std::size_t{1});
}

ASYNC_TEST("an expired generic 429 retry preserves the subsequent raw 403 response") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  double now = fixedStartMs; rt::globalDateNow() = [&] { return now; };
  std::vector<std::string> accountIds;
  json::value forbidden = json::object{{"error", json::object{{"message", "Active account forbidden"}, {"type", "invalid_request_error"}, {"code", "active_account_forbidden"}}}};
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init));
    if (accountIds.size() == 1) return jsonResponse(json::object{{"error", json::object{{"message", "Active account temporarily rate limited"}, {"type", "rate_limit_error"}, {"code", "rate_limit_exceeded"}}}}, 429);
    if (accountIds.size() == 2) { now = fixedStartMs + 5000; return jsonResponse(forbidden, 403); }
    throw rt::JsError("a raw 403 is terminal and must not dispatch a third attempt");
  }));
  cd::FetchCodexResponsesOptions options; options.requestId = "request-expired-generic-retry"; options.retrySleep = resolvedSleep;
  AWAIT(response, fetchCodexResponses(input("expired-generic-retry"), options));
  EXPECT_EQ(response.status(), 403); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-one"})); EXPECT_EQ(accountIds.size(), std::size_t{2});
  EXPECT_EQ(now, fixedStartMs + 5000); AWAIT(payload, response.json()); EXPECT(rt::deepEqual(payload, forbidden));
}

ASYNC_TEST("Codex routing logs attempts, refresh, and bounded retry without sensitive values") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  rt::console::Capture capture({rt::console::Method::info}); int inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
    if (oauth(info)) return jsonResponse(json::object{{"access_token", "refreshed-secret-access"}, {"refresh_token", "refreshed-secret-refresh"}});
    ++inferenceCalls; const int status = inferenceCalls == 1 ? 401 : 429;
    return jsonResponse(json::object{{"error", json::object{{"message", "full-sensitive-upstream-error-body"}}}}, status, status == 429 ? rt::Headers{{"Retry-After", "1"}} : rt::Headers{});
  }));
  cd::FetchCodexResponsesOptions options; options.requestId = "request-redacted-logs"; options.retrySleep = resolvedSleep;
  AWAIT(response, fetchCodexResponses(input("redacted-logs"), options)); EXPECT_EQ(response.status(), 429);
  std::string output; for (const auto& call : capture.calls()) output += call.text + "\n";
  for (const std::string pattern : {"\"event\":\"codex_attempt\"", "\"event\":\"codex_token_refresh\"", "\"event\":\"codex_two_second_retry\"", "\"status_class\":\"401\"", "\"status_class\":\"429\""}) EXPECT(rt::includes(output, pattern));
  for (const std::string forbidden : {"account-one", "account-two", "access-one", "refresh-one", "refreshed-secret-access", "refreshed-secret-refresh", "full-sensitive-upstream-error-body"}) EXPECT(!rt::includes(output, forbidden));
}

ASYNC_TEST("Codex responses retry the other account when a 401 cannot refresh") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  // Bun's test runner stays alive for the unreferenced bounded-body deadline.
  const auto keepAlive = rt::setTimeout([] {}, 2000);
  auto clearKeepAlive = rt::scopeExit([keepAlive] { rt::clearTimeout(keepAlive); });
  std::vector<std::string> accountIds; bool firstCancellationStarted = false, refreshCancellationStarted = false;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    if (oauth(info)) return cancellableBody("{\"error\":\"invalid_grant\"}", refreshCancellationStarted);
    accountIds.push_back(accountId(init));
    return accountIds.size() == 1 ? cancellableBody("{}", firstCancellationStarted) : respond();
  }));
  AWAIT(response, fetchCodexResponses(input("auth-failover"))); EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(accountIds.size(), std::size_t{2}); EXPECT_EQ((std::set<std::string>(accountIds.begin(), accountIds.end()).size()), std::size_t{2});
  EXPECT(firstCancellationStarted); EXPECT(refreshCancellationStarted);
}

ASYNC_TEST("a credential replacement landing after 401 is retried without an OAuth refresh") {
  HarnessScope scope; const auto attempted = auth("one"); auto replacement = attempted;
  replacement.access_token = accessToken("replacement"); replacement.refresh_token = "refresh-replacement"; replacement.updated_at_ms = fixedStartMs + 1;
  kv->auth = pool(attempted); ++kv->authVersion;
  std::vector<std::string> authorizationHeaders, serializedBodies; int oauthCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    if (oauth(info)) { ++oauthCalls; return respond("{\"error\":\"must_not_refresh_replacement\"}", 401); }
    serializedBodies.push_back(serialized(init)); authorizationHeaders.push_back(requestHeaders(init).get("Authorization").value_or(""));
    if (authorizationHeaders.size() == 1) { kv->auth = pool(replacement); ++kv->authVersion; return respond("{}", 401); }
    return respond();
  }));
  AWAIT(response, fetchCodexResponses(input("rotation-between-401-and-refresh"))); EXPECT_EQ(response.status(), 200); EXPECT_EQ(oauthCalls, 0);
  EXPECT_EQ(authorizationHeaders, (std::vector<std::string>{"Bearer " + attempted.access_token, "Bearer " + replacement.access_token}));
  const auto expected = rt::jsonStringify(input("rotation-between-401-and-refresh")); EXPECT_EQ(rt::utf8ByteLength(expected), std::size_t{44});
  EXPECT_EQ(serializedBodies, (std::vector<std::string>{expected, expected}));
  std::size_t total = 0; for (const auto& body : serializedBodies) { EXPECT_EQ(rt::utf8ByteLength(body), std::size_t{44}); total += rt::utf8ByteLength(body); }
  EXPECT_EQ(total, std::size_t{88});
}

ASYNC_TEST("Codex responses synthesize 401 only after every account has an invalid refresh credential") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; int refreshCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    if (oauth(info)) { ++refreshCalls; return respond("{\"error\":\"invalid_grant\"}", 401); }
    accountIds.push_back(accountId(init)); return respond("{}", 401);
  }));
  AWAIT(response, fetchCodexResponses(input("auth-exhaustion"))); EXPECT_EQ(response.status(), 401);
  AWAIT(payload, response.json()); EXPECT_EQ(rt::getString(payload.at("error"), "code"), std::optional<std::string_view>("codex_auth_invalid"));
  EXPECT_EQ(accountIds.size(), std::size_t{2}); EXPECT_EQ((std::set<std::string>(accountIds.begin(), accountIds.end()).size()), std::size_t{2}); EXPECT_EQ(refreshCalls, 2);
}

ASYNC_TEST("concurrent proactive refreshes share one OAuth exchange") {
  HarnessScope scope; kv->auth = pool(staleAuth("one")); int refreshCalls = 0, inferenceCalls = 0, routingReads = 0;
  Deferred refreshGate, firstRoutingReadGate;
  auto cleanup = rt::scopeExit([&] { firstRoutingReadGate.resolve(); refreshGate.resolve(); kv->onRoutingRead = {}; });
  kv->onRoutingRead = [&](double) { ++routingReads; return routingReads == 1 ? firstRoutingReadGate.promise.wait() : rt::fulfilledTask(); };
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) -> rt::Task<rt::Response> {
    if (oauth(info)) { ++refreshCalls; co_await refreshGate.promise.wait(); co_return jsonResponse(json::object{{"access_token", "refreshed-access"}, {"refresh_token", "refreshed-refresh"}}); }
    ++inferenceCalls; co_return respond();
  }));
  std::vector<rt::Promise<rt::Response>> requests;
  for (int index = 0; index < 8; ++index) requests.emplace_back(fetchCodexResponses(input("refresh-" + rt::numberToString(index))));
  for (int attempt = 0; attempt < 100 && routingReads == 0; ++attempt) co_await rt::sleep(1);
  EXPECT_EQ(routingReads, 1); firstRoutingReadGate.resolve();
  for (int attempt = 0; attempt < 100 && refreshCalls == 0; ++attempt) co_await rt::sleep(1);
  EXPECT_EQ(refreshCalls, 1); refreshGate.resolve();
  auto responses = co_await rt::all(std::move(requests)).wait();
  EXPECT_EQ(refreshCalls, 1);
  for (auto& response : responses) {
    if (response.status() != 200) { AWAIT(body, response.text()); rt::console::error(body); }
    EXPECT_EQ(response.status(), 200);
  }
  EXPECT_EQ(inferenceCalls, 8);
}

ASYNC_TEST("a failed strong selector read releases the admission queue for the next request") {
  HarnessScope scope; kv->auth = pool(auth("one")); std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return respond(); }));
  kv->onRoutingRead = [](double) -> rt::Task<void> { throw rt::JsError("routing KV unavailable"); };
  AWAIT(failed, fetchCodexResponses(input("selector-failure"))); EXPECT_EQ(failed.status(), 503); WAIT(failed.arrayBuffer());
  kv->onRoutingRead = {};
  AWAIT(recovered, fetchCodexResponses(input("selector-recovery"))); EXPECT_EQ(recovered.status(), 200); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one"})); WAIT(recovered.arrayBuffer());
}

ASYNC_TEST("a deterministic proactive refresh rejection quarantines the credential before inference") {
  HarnessScope scope; kv->auth = pool(staleAuth("invalid")); int refreshCalls = 0, inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
    if (oauth(info)) { ++refreshCalls; return jsonResponse(json::object{{"error", "invalid_grant"}}, 401); }
    ++inferenceCalls; return respond();
  }));
  AWAIT(first, fetchCodexResponses(input("expired-auth"))); AWAIT(second, fetchCodexResponses(input("expired-auth-again")));
  EXPECT_EQ(first.status(), 401); EXPECT_EQ(second.status(), 401);
  EXPECT_EQ(first.headers().get("x-uos-warning"), std::optional<std::string>(ca::CODEX_AUTH_REAUTH_WARNING));
  EXPECT_EQ(second.headers().get("x-uos-warning"), std::optional<std::string>(ca::CODEX_AUTH_REAUTH_WARNING));
  EXPECT_EQ(refreshCalls, 1); EXPECT_EQ(inferenceCalls, 0);
  AWAIT(payload, first.json()); EXPECT_EQ(rt::getString(payload.at("error"), "code"), std::optional<std::string_view>("codex_auth_invalid"));
}

ASYNC_TEST("refresh-token reuse returns an actionable re-auth warning without exposing the OAuth body") {
  HarnessScope scope; kv->auth = pool(staleAuth("reused")); int inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
    if (oauth(info)) return jsonResponse(json::object{{"error", json::object{{"code", "refresh_token_reused"}, {"message", "provider secret must not escape"}}}}, 400);
    ++inferenceCalls; return respond();
  }));
  AWAIT(response, fetchCodexResponses(input("reused-refresh-token"))); EXPECT_EQ(response.status(), 401);
  EXPECT_EQ(response.headers().get("x-uos-warning"), std::optional<std::string>(ca::CODEX_AUTH_REAUTH_WARNING));
  AWAIT(payload, response.json()); EXPECT_EQ(rt::getString(payload.at("error"), "code"), std::optional<std::string_view>("refresh_token_reused"));
  const std::string message(rt::getString(payload.at("error"), "message").value_or(""));
  EXPECT(rt::Regex("already used", "i").test(message)); EXPECT(!rt::includes(message, "provider secret")); EXPECT_EQ(inferenceCalls, 0);
}

ASYNC_TEST("a refresh failure warning survives a later quota-shaped 403 from another account") {
  HarnessScope scope; kv->auth = pool(staleAuth("expired"), auth("quota")); std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    if (oauth(info)) return jsonResponse(json::object{{"error", "invalid_grant"}}, 401);
    const auto id = accountId(init); accountIds.push_back(id);
    return jsonResponse(json::object{{"error", json::object{{"message", "user quota is not enough"}}}}, id == "account-quota" ? 403 : 200);
  }));
  AWAIT(response, fetchCodexResponses(input("expired-auth-with-quota-shaped-403"))); EXPECT_EQ(response.status(), 403);
  EXPECT_EQ(response.headers().get("x-uos-warning"), std::optional<std::string>(ca::CODEX_AUTH_REAUTH_WARNING)); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-quota"}));
}
