// Port of tests/admin-api-keys-coverage.test.ts.
// All source admin mutations and existing create/history/Sentinel cases are retained.
// Links: tests/admin_api_keys_coverage_test.cpp
// src/admin/api_keys.cpp src/api_keys.cpp src/sentinel/incident_outbox.cpp src/authority/native_api_key_policy.cpp
// tests/helpers/admin_auth_harness.cpp src/admin/api_key_audit.cpp src/paid_fallback/ledger_state.cpp src/authority/native_api_key_audit.cpp
// src/authority/native_paid_policy.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/paid_fallback/ledger_backfill.cpp
// src/types.cpp src/utils.cpp src/paid_fallback/ledger_settlement.cpp src/authority/native_api_key.cpp
// src/api_key_policy.cpp src/admin/sentinel_incident.cpp src/kv.cpp src/storage/kv.cpp
// src/request.cpp src/paid_fallback/index.cpp src/models/codex_models_fetch.cpp src/codex/dispatch.cpp
// src/authority/native_codex_contracts.cpp src/runtime_config.cpp src/http.cpp src/config.cpp
// src/paid_fallback/ledger_admission.cpp src/defaults.cpp src/provider/surplus.cpp src/models/codex_models_availability.cpp
// src/models/codex_models.cpp src/storage/sqlite_readonly.cpp src/codex/auth_refresh.cpp src/storage/native_kv.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/metered.cpp src/codex/auth.cpp src/codex/native_auth.cpp
// src/provider/surplus_claude_request.cpp src/paid_fallback/ledger_stats.cpp src/provider/health.cpp src/codex/external_auth.cpp
// src/provider/capacity_events.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp
// src/codex/capacity_routing.cpp src/release.cpp src/authority/native_codex_capacity.cpp src/auth/relay.cpp
// src/provider/capacity.cpp src/provider/anthropic_chat_stream.cpp src/provider/capacity_rollups.cpp src/provider/capacity_contract.cpp
// src/provider/capacity_parse.cpp src/provider/claude_capacity_rollups.cpp src/bounded_response_body.cpp src/responses_stream.cpp
// src/provider/claude_capacity_history.cpp src/provider/claude_code_accounts.cpp src/codex/routing_state.cpp src/authority/native_codex_evaluation.cpp
// src/codex/banked_reset.cpp src/codex/routing_mutations.cpp src/provider/claude_code_credentials.cpp src/codex/native_auth_transport.cpp
// src/provider/claude_code_quota.cpp src/provider/cerebras_rate_limits.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_core.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/capacity_analytics.cpp src/provider/capacity_history.cpp src/authority/native_codex_mutations.cpp
// src/codex/rate_limit_429.cpp src/provider/anthropic_responses.cpp src/authority/native_codex_429.cpp src/codex/experiment.cpp
// src/runtime/websocket.cpp src/provider/claude_code_usage_schema.cpp src/metered_quota.cpp src/codex/banked_reset_provider.cpp
// src/codex/reset_settings.cpp src/codex/account_routing.cpp src/codex/routing_evaluation.cpp src/codex/routing_serial.cpp
// src/authority/native_codex.cpp src/provider/selection.cpp src/codex/overage_settings.cpp
// src/admin/api_key_mutations.cpp src/analytics.cpp src/auth/local_development_key.cpp
#include "tests/helpers/admin_auth_harness.hpp"
#include "uos/admin/sentinel_incident.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/admin/api_key_mutations.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/runtime_config.hpp"
#include "uos/provider/metered.hpp"
#include "uos/api_keys.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/string.hpp"
#include "uos/sentinel/incident_outbox.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace harness = uos::tests::helpers::admin_auth_harness;
  using uos::admin::sentinel_incident::handleAdminSentinelIncidents;
  using uos::admin::sentinel_incident::SentinelIncidentAdminDependencies;
  using namespace uos::sentinel::incident_outbox;

  // beforeEach(resetHarness) / afterEach(cleanupHarness)
  struct HarnessScope {
    HarnessScope() { harness::resetHarness(); }
    ~HarnessScope() { harness::cleanupHarness(); }
    HarnessScope(const HarnessScope&) = delete;
    HarnessScope& operator=(const HarnessScope&) = delete;
  };

  rt::Task<json::value> payloadOf(rt::Response response) {
    auto reading = response.json();
    json::value payload = co_await std::move(reading);
    co_return payload;
  }

  std::optional<std::string> errorField(const json::value& payload, std::string_view field) {
    const json::value* error = rt::get(payload, "error");
    const std::optional<std::string_view> text = rt::getString(rt::get(error, field));
    return text ? std::optional<std::string>(std::string(*text)) : std::nullopt;
  }

  using ListFunction = std::function<rt::Task<SentinelIncidentIndexPage>(rt::Kv&, SentinelIncidentIndexListOptions)>;

  SentinelIncidentAdminDependencies listing(ListFunction list) {
    SentinelIncidentAdminDependencies dependencies;
    dependencies.listSentinelIncidentIndexRows = std::move(list);
    return dependencies;
  }

  SentinelIncidentIndexRow sentinelRow(const std::function<void(SentinelIncidentIndexRow&)>& overrides = {}) {
    SentinelIncidentIndexRow row{
      .version = 1,
      .incident_id = "provider-2f1c4a4e-5b6d-4c7e-8f90-1a2b3c4d5e6f",
      .fingerprint = std::string(64, 'a'),
      .severity = "P2",
      .first_seen_at_ms = 1'000,
      .last_seen_at_ms = 2'000,
      .count = 3,
      .failing_revision = std::string(40, 'b'),
      .error_type = "response_failed",
      .context = {.message = "Gateway observed a provider failure for this incident group.", .location = nullptr, .sample = {"line one", "line two"}},
      .provenance = {.endpoint = "/v1/responses", .captured_at_ms = 1'500, .captured_by = nullptr},
      .evidence_ref = SentinelIncidentIndexEvidenceRef{.ref = "capture:cap-1", .digest = std::string(64, 'c')},
      .evidence_expires_at_ms = 9'000,
    };
    if (overrides) overrides(row);
    return row;
  }

  rt::Task<SentinelIncidentIndexPage> resolvedPage(SentinelIncidentIndexPage page) {
    co_await rt::microtask();
    co_return page;
  }
}

ASYNC_TEST("Sentinel incident index rejects malformed pagination and unknown query keys") {
  HarnessScope scope;
  const ListFunction unavailableIndex = [](rt::Kv&, SentinelIncidentIndexListOptions) -> rt::Task<SentinelIncidentIndexPage> {
    throw rt::JsError("must not be reached");
    co_return SentinelIncidentIndexPage{};
  };
  const std::vector<std::string> cases = {
    "https://ai.ubq.fi/admin/sentinel/incidents?limit=0",
    "https://ai.ubq.fi/admin/sentinel/incidents?limit=101",
    "https://ai.ubq.fi/admin/sentinel/incidents?limit=abc",
    "https://ai.ubq.fi/admin/sentinel/incidents?cursor=",
    "https://ai.ubq.fi/admin/sentinel/incidents?cursor=has%20space",
    "https://ai.ubq.fi/admin/sentinel/incidents?incident_id=not-an-incident",
    "https://ai.ubq.fi/admin/sentinel/incidents?unexpected=1",
    "https://ai.ubq.fi/admin/sentinel/incidents?limit=5&limit=6",
  };
  for (const std::string& url : cases) {
    const rt::Request request(url);
    const SentinelIncidentAdminDependencies dependencies = listing(unavailableIndex);
    auto handling = handleAdminSentinelIncidents(request, dependencies);
    const rt::Response response = co_await std::move(handling);
    EXPECT_EQ(response.status(), 400);
    auto parsing = payloadOf(response);
    const json::value payload = co_await std::move(parsing);
    EXPECT_EQ(errorField(payload, "type"), std::optional<std::string>("invalid_request_error"));
    EXPECT(rt::Regex("limit must be an integer from 1 to 100").test(errorField(payload, "message").value_or("")));
  }

  auto seen = std::make_shared<std::vector<SentinelIncidentIndexListOptions>>();
  const ListFunction recording = [seen](rt::Kv&, SentinelIncidentIndexListOptions options) -> rt::Task<SentinelIncidentIndexPage> {
    seen->push_back(options);
    return resolvedPage(SentinelIncidentIndexPage{.rows = {}, .cursor = std::nullopt});
  };
  const rt::Request acceptedRequest("https://ai.ubq.fi/admin/sentinel/incidents");
  const SentinelIncidentAdminDependencies acceptedDependencies = listing(recording);
  auto acceptedHandling = handleAdminSentinelIncidents(acceptedRequest, acceptedDependencies);
  const rt::Response accepted = co_await std::move(acceptedHandling);
  EXPECT_EQ(accepted.status(), 200);
  EXPECT_EQ(accepted.headers().get("cache-control"), std::optional<std::string>("no-store"));
  auto acceptedParsing = payloadOf(accepted);
  const json::value acceptedPayload = co_await std::move(acceptedParsing);
  EXPECT(rt::deepEqual(acceptedPayload, rt::jsonParse(R"({"data":[],"cursor":null,"coverage":{"status":"complete"}})")));
  ASSERT_EQ(seen->size(), 1u);
  EXPECT_EQ(seen->at(0).incidentId, std::nullopt);
  EXPECT_EQ(seen->at(0).limit, std::optional<double>(20));
  EXPECT_EQ(seen->at(0).cursor, std::nullopt);
}

ASYNC_TEST("Sentinel incident index fails closed when its storage or reader is unavailable") {
  HarnessScope scope;
  SentinelIncidentAdminDependencies noKvDependencies;
  noKvDependencies.getKv = [] { return rt::Promise<std::shared_ptr<rt::Kv>>::resolve(std::shared_ptr<rt::Kv>()); };
  const rt::Request noKvRequest("https://ai.ubq.fi/admin/sentinel/incidents?limit=1");
  auto noKvHandling = handleAdminSentinelIncidents(noKvRequest, noKvDependencies);
  const rt::Response noKv = co_await std::move(noKvHandling);
  EXPECT_EQ(noKv.status(), 503);
  auto noKvParsing = payloadOf(noKv);
  const json::value noKvPayload = co_await std::move(noKvParsing);
  EXPECT_EQ(errorField(noKvPayload, "code"), std::optional<std::string>("sentinel_incidents_unavailable"));

  const ListFunction failing = [](rt::Kv&, SentinelIncidentIndexListOptions) -> rt::Task<SentinelIncidentIndexPage> {
    throw rt::JsError("index unavailable");
    co_return SentinelIncidentIndexPage{};
  };
  const rt::Request readerRequest("https://ai.ubq.fi/admin/sentinel/incidents?limit=1");
  const SentinelIncidentAdminDependencies readerDependencies = listing(failing);
  auto readerHandling = handleAdminSentinelIncidents(readerRequest, readerDependencies);
  const rt::Response readerFailure = co_await std::move(readerHandling);
  EXPECT_EQ(readerFailure.status(), 503);
  auto readerParsing = payloadOf(readerFailure);
  const json::value readerPayload = co_await std::move(readerParsing);
  EXPECT_EQ(errorField(readerPayload, "message"), std::optional<std::string>("Sentinel incident index is unavailable"));
}

ASYNC_TEST("Sentinel incident index projects rows onto the frozen wire contract") {
  HarnessScope scope;
  auto optionsMatched = std::make_shared<bool>(false);
  const ListFunction relied = [optionsMatched](rt::Kv&, SentinelIncidentIndexListOptions options) -> rt::Task<SentinelIncidentIndexPage> {
    *optionsMatched = options.incidentId == std::optional<std::string>("provider-2f1c4a4e-5b6d-4c7e-8f90-1a2b3c4d5e6f") &&
                      options.limit == std::optional<double>(5) && options.cursor == std::optional<std::string>("abc");
    return resolvedPage(SentinelIncidentIndexPage{.rows = {sentinelRow()}, .cursor = "next-page"});
  };
  const rt::Request request("https://ai.ubq.fi/admin/sentinel/incidents?limit=5&cursor=abc&incident_id=provider-2f1c4a4e-5b6d-4c7e-8f90-1a2b3c4d5e6f");
  const SentinelIncidentAdminDependencies dependencies = listing(relied);
  auto handling = handleAdminSentinelIncidents(request, dependencies);
  const rt::Response response = co_await std::move(handling);
  EXPECT(*optionsMatched);
  EXPECT_EQ(response.status(), 200);
  auto parsing = payloadOf(response);
  const json::value payload = co_await std::move(parsing);
  EXPECT(rt::deepEqual(*rt::get(payload, "cursor"), json::value("next-page")));
  EXPECT(rt::deepEqual(*rt::get(*rt::get(payload, "coverage"), "status"), json::value("complete")));
  const json::value& row = rt::get(payload, "data")->as_array().at(0);
  EXPECT(rt::deepEqual(*rt::get(row, "incident_id"), json::value("provider-2f1c4a4e-5b6d-4c7e-8f90-1a2b3c4d5e6f")));
  EXPECT(rt::deepEqual(*rt::get(row, "severity"), json::value("P2")));
  EXPECT(rt::deepEqual(*rt::get(*rt::get(row, "provenance"), "endpoint"), json::value("https://ai.ubq.fi/v1/responses")));
  EXPECT(rt::deepEqual(*rt::get(*rt::get(row, "evidence_ref"), "ref"),
                       json::value("artifact://sentinel/provider-2f1c4a4e-5b6d-4c7e-8f90-1a2b3c4d5e6f/cap-1")));
  EXPECT(rt::deepEqual(*rt::get(*rt::get(row, "evidence_ref"), "digest"), json::value(std::string(64, 'c'))));
  EXPECT(rt::deepEqual(*rt::get(*rt::get(row, "context"), "sample"), rt::jsonParse(R"(["line one","line two"])")));
  EXPECT_EQ(rt::hasOwn(row.as_object(), "version"), false);
  EXPECT_EQ(rt::hasOwn(row.as_object(), "latest"), false);

  const ListFunction classNames = [](rt::Kv&, SentinelIncidentIndexListOptions) -> rt::Task<SentinelIncidentIndexPage> {
    return resolvedPage(SentinelIncidentIndexPage{
      .rows =
        {
          sentinelRow([](SentinelIncidentIndexRow& value) {
            value.fingerprint = std::string(64, 'd');
            value.provenance = {.endpoint = "/v1/chat/completions", .captured_at_ms = 1, .captured_by = nullptr};
            value.evidence_ref = std::nullopt;
            value.evidence_expires_at_ms = std::nullopt;
          }),
          sentinelRow([](SentinelIncidentIndexRow& value) {
            value.fingerprint = std::string(64, 'e');
            value.provenance = {.endpoint = "other", .captured_at_ms = 1, .captured_by = nullptr};
          }),
        },
      .cursor = std::nullopt,
    });
  };
  const rt::Request classRequest("https://ai.ubq.fi/admin/sentinel/incidents");
  const SentinelIncidentAdminDependencies classDependencies = listing(classNames);
  auto classHandling = handleAdminSentinelIncidents(classRequest, classDependencies);
  const rt::Response className = co_await std::move(classHandling);
  EXPECT_EQ(className.status(), 200);
  auto classParsing = payloadOf(className);
  const json::value classPayload = co_await std::move(classParsing);
  const json::array& data = rt::get(classPayload, "data")->as_array();
  EXPECT(rt::deepEqual(*rt::get(*rt::get(data.at(0), "provenance"), "endpoint"), json::value("https://ai.ubq.fi/v1/chat/completions")));
  EXPECT(rt::get(data.at(0), "evidence_ref")->is_null());
  EXPECT(rt::deepEqual(*rt::get(*rt::get(data.at(1), "provenance"), "endpoint"), json::value("https://ai.ubq.fi/")));
}

ASYNC_TEST("Sentinel incident index rejects an unvalidated evidence reference") {
  HarnessScope scope;
  const ListFunction unvalidated = [](rt::Kv&, SentinelIncidentIndexListOptions) -> rt::Task<SentinelIncidentIndexPage> {
    return resolvedPage(SentinelIncidentIndexPage{
      .rows = {sentinelRow([](SentinelIncidentIndexRow& value) { value.evidence_ref = SentinelIncidentIndexEvidenceRef{.ref = "https://evil.test/1", .digest = std::nullopt}; })},
      .cursor = std::nullopt,
    });
  };
  const rt::Request request("https://ai.ubq.fi/admin/sentinel/incidents");
  const SentinelIncidentAdminDependencies dependencies = listing(unvalidated);
  auto handling = handleAdminSentinelIncidents(request, dependencies);
  const rt::Response response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 503);
  auto parsing = payloadOf(response);
  const json::value payload = co_await std::move(parsing);
  EXPECT_EQ(errorField(payload, "code"), std::optional<std::string>("sentinel_incidents_unavailable"));
}

ASYNC_TEST("Sentinel incident index reads the durable KV index through its default dependencies") {
  HarnessScope scope;
  harness::kvStore.clear();
  // The KV-backed reader validates the canonical row shape, which carries an
  // empty context sample.
  const SentinelIncidentIndexRow readable = sentinelRow([](SentinelIncidentIndexRow& value) {
    value.fingerprint = std::string(64, 'f');
    value.context = {.message = "Gateway observed a provider failure for this incident group.", .location = nullptr, .sample = {}};
  });
  harness::kvStore.set(harness::keyToString(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, readable.fingerprint}), rt::KvValue(json::value_from(readable)));
  const rt::Request request("https://ai.ubq.fi/admin/sentinel/incidents?limit=1");
  auto handling = handleAdminSentinelIncidents(request);
  const rt::Response response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  auto parsing = payloadOf(response);
  const json::value payload = co_await std::move(parsing);
  const json::array& data = rt::get(payload, "data")->as_array();
  EXPECT_EQ(data.size(), 1u);
  EXPECT(!data.empty() && rt::deepEqual(*rt::get(data.at(0), "fingerprint"), json::value(std::string(64, 'f'))));
  EXPECT(rt::get(payload, "cursor")->is_null());

  rt::KvObject broken;
  broken.set("version", rt::KvValue(1));
  harness::kvStore.set(harness::keyToString(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, std::string(64, 'g')}), rt::KvValue(broken));
  const rt::Request brokenRequest("https://ai.ubq.fi/admin/sentinel/incidents?limit=10");
  auto brokenHandling = handleAdminSentinelIncidents(brokenRequest);
  const rt::Response brokenResponse = co_await std::move(brokenHandling);
  EXPECT_EQ(brokenResponse.status(), 503);
  auto brokenParsing = payloadOf(brokenResponse);
  const json::value brokenPayload = co_await std::move(brokenParsing);
  EXPECT_EQ(errorField(brokenPayload, "code"), std::optional<std::string>("sentinel_incidents_unavailable"));
}

namespace {
  namespace adminKeys = uos::admin::api_keys;
  rt::Request adminKeyRequest(const json::value& body) { return rt::Request("https://ai.ubq.fi/admin/api-keys", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(body))}); }
  rt::Request invalidAdminKeyRequest() { return rt::Request("https://ai.ubq.fi/admin/api-keys", rt::RequestInit{.method = "POST", .body = rt::BodyInit(std::string("{"))}); }
  class FailingLedgerKv : public rt::ForwardingKv {
  public:
    explicit FailingLedgerKv(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override { return rt::fulfilledTask(rt::KvEntryMaybe<>{key, rt::KvValue(json::object{{"id", "broken"}}), std::string("00000000000000000001")}); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions = {}) override { throw rt::JsError("ledger unavailable"); }
  };
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
ASYNC_TEST("admin API key create validates its fields and reports persistence failures") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope;
  AWAIT(invalidJson, adminKeys::handleAdminApiKeysCreate(invalidAdminKeyRequest())); EXPECT_EQ(invalidJson.status(), 400); AWAIT(invalidBody, invalidJson.json()); EXPECT_EQ(errorField(invalidBody, "message"), std::optional<std::string>("Invalid JSON body"));
  const std::vector<std::pair<json::object, std::string>> cases{
    {{{"name", 5}}, "name must be a non-empty string (<=80 chars)"}, {{{"name", "has\nnewline"}}, "name must be a non-empty string (<=80 chars)"},
    {{{"name", "token case"}, {"token", "nope"}}, "token must use the u_ prefix followed by 64 lowercase hexadecimal characters"},
    {{{"name", "expiry case"}, {"expires_at_ms", 5}}, "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1"},
    {{{"name", "limit case"}, {"usage_limit_requests", "many"}}, "usage_limit_requests must be a positive number or -1 for unlimited"},
    {{{"name", "window case"}, {"window_ms", "soon"}}, "window_ms must be a positive number"},
    {{{"name", "fallback case"}, {"paid_fallback_enabled", "yes"}}, "paid_fallback_enabled must be a boolean"},
    {{{"name", "fallback limit case"}, {"paid_fallback_limit_credits", "lots"}}, "paid_fallback_limit_credits must be a non-negative number or -1"},
    {{{"name", "fallback zero case"}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 0}}, "paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled"}
  };
  for (const auto& [input, message] : cases) { AWAIT(response, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(input))); EXPECT_EQ(response.status(), 400); AWAIT(body, response.json()); EXPECT_EQ(errorField(body, "message"), std::optional(message)); }
  AWAIT(created, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(json::object{{"name", "Persistence case"}, {"usage_limit_requests", -1}}))); EXPECT_EQ(created.status(), 200); AWAIT(body, created.json()); const auto token = rt::getString(body, "token"); ASSERT(token); static const rt::Regex pattern("^u_[0-9a-f]{64}$", ""); EXPECT(pattern.test(std::string(*token)));
  AWAIT(duplicate, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(json::object{{"name", "Duplicate"}, {"token", std::string(*token)}}))); EXPECT_EQ(duplicate.status(), 409); AWAIT(duplicateBody, duplicate.json()); EXPECT_EQ(errorField(duplicateBody, "message"), std::optional<std::string>("API key already exists"));
  harness::setAtomicCommitsToFail(1); AWAIT(failed, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(json::object{{"name", "Commit failure"}}))); EXPECT_EQ(failed.status(), 500); AWAIT(failedBody, failed.json()); EXPECT_EQ(errorField(failedBody, "message"), std::optional<std::string>("Failed to persist API key"));
}
ASYNC_TEST("retired API-key banked reset settings are rejected without mutation") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope;
  AWAIT(created, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(json::object{{"name", "Retired setting"}, {"banked_resets_enabled", false}}))); EXPECT_EQ(created.status(), 400); AWAIT(body, created.json()); EXPECT_EQ(errorField(body, "message"), std::optional<std::string>("banked_resets_enabled is retired; configure banked resets per Codex subscription")); EXPECT_EQ(harness::kvStore.size(), std::size_t(0));
}
ASYNC_TEST("admin API key create reports an unusable Metered pricing response") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope;
  rt::GlobalFetchOverride mock(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { throw rt::JsError("metered pricing is unreachable"); co_return rt::Response(); }));
  const json::object input{{"name", "Metered outage"}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 5}}; AWAIT(missing, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(input))); EXPECT_EQ(missing.status(), 503); AWAIT(missingBody, missing.json()); EXPECT_EQ(errorField(missingBody, "code"), std::optional<std::string>("metered_api_key_missing")); EXPECT_EQ(errorField(missingBody, "type"), std::optional<std::string>("server_error")); EXPECT(!harness::kvStoreHasPrefix(uos::api_keys::API_KEY_ID_PREFIX));
  rt::env::set("METERED_API_KEY", "metered-coverage-key"); AWAIT(noCatalog, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(input))); EXPECT_EQ(noCatalog.status(), 503); AWAIT(noCatalogBody, noCatalog.json()); EXPECT_EQ(errorField(noCatalogBody, "code"), std::optional<std::string>("metered_pricing_invalid")); EXPECT(rt::includes(errorField(noCatalogBody, "message").value_or(""), "Codex model catalog is initialized"));
}
ASYNC_TEST("admin API key handlers report a missing KV store") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope; uos::kv::setKvForTest(nullptr);
  AWAIT(created, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(json::object{{"name", "kv-less"}}))); EXPECT_EQ(created.status(), 500); AWAIT(listed, adminKeys::handleAdminApiKeysList(rt::Request("https://ai.ubq.fi/admin/api-keys"))); EXPECT_EQ(listed.status(), 500); AWAIT(history, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/id/paid-fallbacks"), "id")); EXPECT_EQ(history.status(), 500);
  const json::object input{{"id", "kv-less"}};
  AWAIT(updated, uos::admin::api_key_mutations::handleAdminApiKeysUpdate(adminKeyRequest(input))); EXPECT_EQ(updated.status(), 500);
  AWAIT(revoked, uos::admin::api_key_mutations::handleAdminApiKeysRevoke(adminKeyRequest(input))); EXPECT_EQ(revoked.status(), 500);
  AWAIT(unrevoked, uos::admin::api_key_mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(input))); EXPECT_EQ(unrevoked.status(), 500);
  AWAIT(deleted, uos::admin::api_key_mutations::handleAdminApiKeysDelete(adminKeyRequest(input))); EXPECT_EQ(deleted.status(), 500);
  uos::kv::setKvForTest(harness::kvStub());
}
ASYNC_TEST("admin API key paid fallback history validates the key id and limit") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope;
  AWAIT(created, adminKeys::handleAdminApiKeysCreate(adminKeyRequest(json::object{{"name", "History key"}}))); EXPECT_EQ(created.status(), 200); AWAIT(body, created.json()); const auto id = std::string(*rt::getString(body, "id"));
  AWAIT(blank, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/%20/paid-fallbacks"), " ")); EXPECT_EQ(blank.status(), 400); AWAIT(overlong, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/x/paid-fallbacks"), std::string(201, 'k'))); EXPECT_EQ(overlong.status(), 400); AWAIT(unknown, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/none/paid-fallbacks"), "none")); EXPECT_EQ(unknown.status(), 404);
  const std::vector<std::string> limits{"0", "not-a-number", "9999999999999999999999"}; for (const auto& limit : limits) { AWAIT(response, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/" + id + "/paid-fallbacks?limit=" + limit), id)); EXPECT_EQ(response.status(), 400); AWAIT(payload, response.json()); EXPECT_EQ(errorField(payload, "message"), std::optional<std::string>("limit must be a positive integer")); }
  AWAIT(listed, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/" + id + "/paid-fallbacks"), id)); EXPECT_EQ(listed.status(), 200); AWAIT(payload, listed.json()); EXPECT(rt::deepEqual(payload, json::object{{"object", "list"}, {"data", json::array()}})); co_await rt::sleep(1);
}
ASYNC_TEST("admin API key paid fallback history reports a ledger read failure") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope; auto failing = std::make_shared<FailingLedgerKv>(*harness::kvStub()); AWAIT(response, adminKeys::handleAdminApiKeysPaidFallbacks(rt::Request("https://ai.ubq.fi/admin/api-keys/broken/paid-fallbacks"), "broken", failing)); EXPECT_EQ(response.status(), 500); AWAIT(payload, response.json()); EXPECT_EQ(errorField(payload, "message"), std::optional<std::string>("Failed to load paid fallbacks")); co_await rt::sleep(1);
}
TEST("estimateJsonSize reports non-serializable values as null") {
  EXPECT_EQ(adminKeys::estimateJsonSize(json::object{{"model_ids", json::array{"gpt-5"}}}), std::optional(23.0)); const rt::KvValue value = rt::KvValue::object({{"value", rt::KvValue(rt::BigInt(1))}}); EXPECT(!adminKeys::estimateJsonSize(value));
}

ASYNC_TEST("admin API key listing preserves legacy raw fields and source coercion") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); HarnessScope scope;
  const auto vectors = rt::jsonParse(
#include "tests/admin_api_keys_list_vectors.inc"
  );
  const rt::KvKey key{uos::api_keys::API_KEY_ID_PREFIX, "legacy"}; harness::kvStore.set(harness::keyToString(key), rt::KvValue(*rt::get(vectors, "record")));
  for (const auto& row : *rt::getArray(vectors, "rows")) {
    const auto suffix = std::string(*rt::getString(row, "suffix")); AWAIT(response, adminKeys::handleAdminApiKeysList(rt::Request("https://ai.ubq.fi/admin/api-keys" + suffix))); EXPECT_EQ(response.status(), static_cast<int>(*rt::getNumber(row, "status"))); AWAIT(body, response.json()); EXPECT(rt::deepEqual(body, *rt::get(row, "body")));
  }
}

namespace {
  namespace mutations = uos::admin::api_key_mutations;
  struct MutationFixture {
    rt::env::ScopedEnvironment environment{std::initializer_list<std::pair<std::string, std::string>>()};
    HarnessScope harnessScope;
    MutationFixture() { uos::config::config.codexAuthJsonB64.clear(); uos::api_key_policy::resetApiKeyPolicyCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest(); }
  };
  struct SeededKey { std::string id; std::string hash; double windowMs; double windowStartMs; json::object record; };
  SeededKey seedMutationKey(const json::object& overrides = {}, bool hashRecord = true) {
    static int sequence = 0; const auto suffix = std::to_string(++sequence); const auto id = "coverage-key-" + suffix; const auto hash = "coverage-hash-" + suffix;
    const auto now = rt::dateNow();
    json::object record{{"id", id}, {"name", "Coverage key"}, {"prefix", "u_coverage00"}, {"hash", hash}, {"created_at_ms", rt::makeNumber(now - 5000)},
      {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 25}, {"usage_requests", 0}, {"usage_reset_at_ms", rt::makeNumber(now + 60000)},
      {"window_ms", 60000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0},
      {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr},
      {"paid_fallback_model_ids", json::array()}, {"paid_fallback_quota_per_credit", 0}, {"paid_fallback_pricing_checked_at_ms", nullptr}};
    for (const auto& member : overrides) record[member.key()] = member.value();
    harness::kvStore.set(harness::keyToString(uos::api_keys::apiKeyIdKey(id)), rt::KvValue(record));
    if (hashRecord) {
      json::object hashValue{{"id", id}};
      for (const auto field : {"expires_at_ms", "revoked_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms", "usage_quota_version",
        "paid_fallback_enabled", "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits", "paid_fallback_reserved_microcredits", "paid_fallback_reservation_request_id"}) hashValue[field] = record.at(field);
      harness::kvStore.set(harness::keyToString(uos::api_keys::apiKeyHashKey(hash)), rt::KvValue(hashValue));
    }
    return {id, hash, 60000, *rt::getNumber(record, "usage_reset_at_ms") - 60000, record};
  }
  rt::KvValue storedMutationKey(const SeededKey& key) { return *harness::kvStore.get(harness::keyToString(uos::api_keys::apiKeyIdKey(key.id))); }
  json::object patch(const SeededKey& key, const json::object& fields = {}) { auto body = fields; body["id"] = key.id; return body; }
  void seedMutationWindow(const SeededKey& key, double used, double reserved = 0) {
    const rt::KvKey window{"uos_ai", "api_key_usage", "v3", "window", key.id, "v3:60000", key.windowStartMs};
    harness::kvStore.set(harness::keyToString(window), rt::KvValue(json::object{{"v", 3}, {"key_id", key.id}, {"policy_version", "v3:60000"},
      {"window_start_ms", rt::makeNumber(key.windowStartMs)}, {"window_reset_at_ms", rt::makeNumber(key.windowStartMs + key.windowMs)}, {"committed_requests", rt::makeNumber(used)},
      {"reserved_requests", rt::makeNumber(reserved)}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}}));
  }
  void seedMutationReservation(const SeededKey& key, const std::string& requestId, bool malformed = false, bool live = false) {
    const rt::KvKey requestKey{"uos_ai", "api_key_usage", "v3", "request", key.id, "v3:60000", key.windowStartMs, requestId};
    const auto now = rt::dateNow();
    const json::object record = malformed ? json::object() : json::object{{"v", 3}, {"key_id", key.id}, {"request_id", requestId}, {"route", "responses"},
      {"state", "reserved"}, {"reserved_at_ms", rt::makeNumber(now - (live ? 1000 : 600000))}, {"lease_expires_at_ms", rt::makeNumber(now + (live ? 300000 : -300000))},
      {"provider", nullptr}, {"dispatched_at_ms", nullptr}, {"released_at_ms", nullptr}, {"release_reason", nullptr}};
    harness::kvStore.set(harness::keyToString(requestKey), rt::KvValue(record));
  }
}
ASYNC_TEST("admin API key update rejects every malformed field and non-strict records") {
  MutationFixture fixture; const auto key = seedMutationKey();
  AWAIT(invalid, mutations::handleAdminApiKeysUpdate(invalidAdminKeyRequest())); EXPECT_EQ(invalid.status(), 400); AWAIT(invalidBody, invalid.json()); EXPECT_EQ(errorField(invalidBody, "message"), std::optional<std::string>("Invalid JSON body"));
  AWAIT(missing, mutations::handleAdminApiKeysUpdate(adminKeyRequest(json::object()))); EXPECT_EQ(missing.status(), 400); AWAIT(missingBody, missing.json()); EXPECT_EQ(errorField(missingBody, "message"), std::optional<std::string>("id is required"));
  AWAIT(unknown, mutations::handleAdminApiKeysUpdate(adminKeyRequest(json::object{{"id", "missing-key"}}))); EXPECT_EQ(unknown.status(), 404); AWAIT(unknownBody, unknown.json()); EXPECT_EQ(errorField(unknownBody, "code"), std::optional<std::string>("not_found"));
  auto legacy = seedMutationKey(); rt::objectDelete(legacy.record, "paid_fallback_pricing_checked_at_ms"); harness::kvStore.set(harness::keyToString(uos::api_keys::apiKeyIdKey(legacy.id)), rt::KvValue(legacy.record));
  AWAIT(migration, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(legacy, json::object{{"name", "renamed"}})))); EXPECT_EQ(migration.status(), 503); AWAIT(migrationBody, migration.json()); EXPECT_EQ(errorField(migrationBody, "message"), std::optional<std::string>("API key paid fallback migration is incomplete"));
  const std::vector<std::pair<json::object, std::string>> cases{
    {{{"name", "   "}}, "name must be a non-empty string (<=80 chars)"}, {{{"name", std::string(81, 'x')}}, "name must be a non-empty string (<=80 chars)"},
    {{{"expires_at_ms", 1}}, "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1"},
    {{{"usage_limit_requests", "5"}}, "usage_limit_requests must be a non-negative number or -1 for unlimited"}, {{{"window_ms", 0}}, "window_ms must be a positive number"},
    {{{"paid_fallback_enabled", "enabled"}}, "paid_fallback_enabled must be a boolean"}, {{{"paid_fallback_limit_credits", -5}}, "paid_fallback_limit_credits must be a non-negative number or -1"},
    {{{"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 0}}, "paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled"},
    {{{"reset_usage", "yes"}}, "reset_usage must be a boolean"}};
  for (const auto& [fields, message] : cases) { AWAIT(result, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key, fields)))); EXPECT_EQ(result.status(), 400); AWAIT(body, result.json()); EXPECT_EQ(errorField(body, "message"), std::optional(message)); }
  AWAIT(accepted, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key, json::object{{"name", "Renamed"}, {"usage_limit_requests", -1}, {"window_ms", 30000}, {"expires_at_ms", -1}})))); EXPECT_EQ(accepted.status(), 200); AWAIT(body, accepted.json()); EXPECT_EQ(rt::getString(body, "name"), std::optional<std::string_view>("Renamed")); EXPECT_EQ(rt::getNumber(body, "usage_limit_requests"), std::optional(-1.0)); EXPECT_EQ(rt::getNumber(body, "window_ms"), std::optional(30000.0)); EXPECT_EQ(storedMutationKey(key)["name"], rt::KvValue("Renamed"));
}
ASYNC_TEST("admin API key update answers a no-op patch from the live V3 aggregate") {
  MutationFixture fixture; const auto key = seedMutationKey(); seedMutationWindow(key, 3);
  AWAIT(result, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key)))); EXPECT_EQ(result.status(), 200); EXPECT_EQ(result.headers().get("x-uos-upstream"), std::optional<std::string>("chatgpt_codex")); AWAIT(body, result.json()); EXPECT_EQ(rt::getString(body, "id"), std::optional<std::string_view>(key.id)); EXPECT_EQ(rt::getString(body, "name"), std::optional<std::string_view>("Coverage key")); EXPECT_EQ(rt::getNumber(body, "usage_requests"), std::optional(3.0)); EXPECT_EQ(rt::getNumber(body, "window_ms"), std::optional(key.windowMs)); EXPECT(rt::get(body, "revoked_at_ms")->is_null());
}
ASYNC_TEST("admin API key update fails closed on a broken quota ledger and reports commit conflicts") {
  MutationFixture fixture; const auto key = seedMutationKey(); seedMutationWindow(key, 0, 1); seedMutationReservation(key, "expired-lease"); seedMutationReservation(key, "malformed", true);
  AWAIT(failed, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key, json::object{{"reset_usage", true}})))); EXPECT_EQ(failed.status(), 503); AWAIT(failedBody, failed.json()); EXPECT_EQ(errorField(failedBody, "message"), std::optional<std::string>("API key quota ledger is unavailable")); EXPECT_EQ(storedMutationKey(key)["usage_requests"], rt::KvValue(0));
  for (const auto id : {"expired-lease", "malformed"}) { const rt::KvKey row{"uos_ai", "api_key_usage", "v3", "request", key.id, "v3:60000", key.windowStartMs, id}; harness::kvStore.delete_(harness::keyToString(row)); }
  const std::vector<json::object> conflicts{json::object{{"name", "Racing rename"}}, json::object{{"reset_usage", true}}};
  for (const auto& fields : conflicts) { harness::setAtomicCommitsToFail(1); AWAIT(result, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key, fields)))); EXPECT_EQ(result.status(), 409); AWAIT(body, result.json()); EXPECT_EQ(errorField(body, "message"), std::optional<std::string>("API key was modified concurrently; retry")); EXPECT_EQ(storedMutationKey(key)["name"], rt::KvValue("Coverage key")); }
  const auto legacy = seedMutationKey(json::object{{"usage_quota_version", 2}}); AWAIT(migration, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(legacy, json::object{{"name", "Renamed legacy"}})))); EXPECT_EQ(migration.status(), 503); AWAIT(body, migration.json()); EXPECT_EQ(errorField(body, "message"), std::optional<std::string>("API key quota migration is incomplete")); EXPECT_EQ(storedMutationKey(legacy)["name"], rt::KvValue("Coverage key"));
}
ASYNC_TEST("admin API key revoke validates input, preserves the first revocation and reports conflicts") {
  MutationFixture fixture;
  AWAIT(invalid, mutations::handleAdminApiKeysRevoke(invalidAdminKeyRequest())); EXPECT_EQ(invalid.status(), 400); AWAIT(missing, mutations::handleAdminApiKeysRevoke(adminKeyRequest(json::object()))); EXPECT_EQ(missing.status(), 400); AWAIT(unknown, mutations::handleAdminApiKeysRevoke(adminKeyRequest(json::object{{"id", "nope"}}))); EXPECT_EQ(unknown.status(), 404);
  auto legacy = seedMutationKey(); rt::objectDelete(legacy.record, "paid_fallback_pricing_checked_at_ms"); harness::kvStore.set(harness::keyToString(uos::api_keys::apiKeyIdKey(legacy.id)), rt::KvValue(legacy.record)); AWAIT(migration, mutations::handleAdminApiKeysRevoke(adminKeyRequest(patch(legacy)))); EXPECT_EQ(migration.status(), 503);
  const auto key = seedMutationKey(); AWAIT(revoked, mutations::handleAdminApiKeysRevoke(adminKeyRequest(patch(key)))); EXPECT_EQ(revoked.status(), 200); AWAIT(body, revoked.json()); const auto at = rt::getNumber(body, "revoked_at_ms"); ASSERT(at); EXPECT_EQ(storedMutationKey(key)["revoked_at_ms"], rt::KvValue(*at)); const auto hash = harness::kvStore.get(harness::keyToString(uos::api_keys::apiKeyHashKey(key.hash))); ASSERT(hash); EXPECT_EQ((*hash)["revoked_at_ms"], rt::KvValue(*at));
  AWAIT(again, mutations::handleAdminApiKeysRevoke(adminKeyRequest(patch(key)))); EXPECT_EQ(again.status(), 200); AWAIT(againBody, again.json()); EXPECT_EQ(rt::getNumber(againBody, "revoked_at_ms"), at);
  const auto unmirrored = seedMutationKey({}, false); AWAIT(mirrored, mutations::handleAdminApiKeysRevoke(adminKeyRequest(patch(unmirrored)))); EXPECT_EQ(mirrored.status(), 200); EXPECT(harness::kvStore.has(harness::keyToString(uos::api_keys::apiKeyHashKey(unmirrored.hash))));
  const auto racing = seedMutationKey(); harness::setAtomicCommitsToFail(1); AWAIT(race, mutations::handleAdminApiKeysRevoke(adminKeyRequest(patch(racing)))); EXPECT_EQ(race.status(), 409); EXPECT(storedMutationKey(racing)["revoked_at_ms"].isNull());
}
ASYNC_TEST("admin API key unrevoke validates input, honors the deletion guard and clears revocation") {
  MutationFixture fixture;
  AWAIT(invalid, mutations::handleAdminApiKeysUnrevoke(invalidAdminKeyRequest())); EXPECT_EQ(invalid.status(), 400); AWAIT(missing, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(json::object()))); EXPECT_EQ(missing.status(), 400); AWAIT(unknown, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(json::object{{"id", "nope"}}))); EXPECT_EQ(unknown.status(), 404);
  auto legacy = seedMutationKey(); rt::objectDelete(legacy.record, "paid_fallback_pricing_checked_at_ms"); harness::kvStore.set(harness::keyToString(uos::api_keys::apiKeyIdKey(legacy.id)), rt::KvValue(legacy.record)); AWAIT(migration, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(patch(legacy)))); EXPECT_EQ(migration.status(), 503);
  const auto active = seedMutationKey(); AWAIT(unchanged, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(patch(active)))); EXPECT_EQ(unchanged.status(), 200); AWAIT(unchangedBody, unchanged.json()); EXPECT(rt::deepEqual(unchangedBody, json::object{{"id", active.id}, {"revoked_at_ms", nullptr}}));
  const auto revoked = seedMutationKey(json::object{{"revoked_at_ms", rt::makeNumber(rt::dateNow() - 1000)}}); AWAIT(cleared, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(patch(revoked)))); EXPECT_EQ(cleared.status(), 200); EXPECT(storedMutationKey(revoked)["revoked_at_ms"].isNull()); EXPECT((*harness::kvStore.get(harness::keyToString(uos::api_keys::apiKeyHashKey(revoked.hash))))["revoked_at_ms"].isNull());
  const auto guarded = seedMutationKey(json::object{{"revoked_at_ms", 1}}); const rt::KvKey guard{"uos_ai", "paid_fallback", "v3", "deletion_guard", guarded.id}; harness::kvStore.set(harness::keyToString(guard), rt::KvValue(json::object{{"created_at_ms", 1}})); AWAIT(blocked, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(patch(guarded)))); EXPECT_EQ(blocked.status(), 409); AWAIT(blockedBody, blocked.json()); EXPECT_EQ(errorField(blockedBody, "code"), std::optional<std::string>("paid_fallback_deletion_in_progress")); EXPECT_EQ(storedMutationKey(guarded)["revoked_at_ms"], rt::KvValue(1));
  const auto racing = seedMutationKey(json::object{{"revoked_at_ms", 1}}); harness::setAtomicCommitsToFail(1); AWAIT(race, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(patch(racing)))); EXPECT_EQ(race.status(), 409); EXPECT_EQ(storedMutationKey(racing)["revoked_at_ms"], rt::KvValue(1));
}
ASYNC_TEST("admin API key delete validates state and fails closed when paid fallback state cannot be read") {
  MutationFixture fixture;
  AWAIT(invalid, mutations::handleAdminApiKeysDelete(invalidAdminKeyRequest())); EXPECT_EQ(invalid.status(), 400); AWAIT(missing, mutations::handleAdminApiKeysDelete(adminKeyRequest(json::object()))); EXPECT_EQ(missing.status(), 400); AWAIT(unknown, mutations::handleAdminApiKeysDelete(adminKeyRequest(json::object{{"id", "nope"}}))); EXPECT_EQ(unknown.status(), 404);
  const auto active = seedMutationKey(); AWAIT(notRevoked, mutations::handleAdminApiKeysDelete(adminKeyRequest(patch(active)))); EXPECT_EQ(notRevoked.status(), 400); AWAIT(notRevokedBody, notRevoked.json()); EXPECT_EQ(errorField(notRevokedBody, "message"), std::optional<std::string>("Only revoked keys can be deleted"));
  const auto broken = seedMutationKey(json::object{{"revoked_at_ms", 1}}); const rt::KvKey unreadableKey{"uos_ai", "paid_fallback", "v3", "request", broken.id, "unreadable-row"}; harness::kvStore.set(harness::keyToString(unreadableKey), rt::KvValue(nullptr)); AWAIT(unreadable, mutations::handleAdminApiKeysDelete(adminKeyRequest(patch(broken)))); EXPECT_EQ(unreadable.status(), 500); AWAIT(unreadableBody, unreadable.json()); EXPECT_EQ(errorField(unreadableBody, "message"), std::optional<std::string>("Failed to prepare paid fallback state for API key deletion")); EXPECT(harness::kvStore.has(harness::keyToString(uos::api_keys::apiKeyIdKey(broken.id)))); harness::kvStore.delete_(harness::keyToString(unreadableKey));
  const auto racing = seedMutationKey(json::object{{"revoked_at_ms", 1}}); harness::setAtomicCommitsToFail(1); AWAIT(race, mutations::handleAdminApiKeysDelete(adminKeyRequest(patch(racing)))); EXPECT_EQ(race.status(), 409);
  const rt::KvKey pending{"uos_ai", "paid_fallback", "v3", "pending", broken.id, "pending-marker"}; harness::kvStore.set(harness::keyToString(pending), rt::KvValue(json::object{{"created_at_ms", 1}})); AWAIT(blocked, mutations::handleAdminApiKeysDelete(adminKeyRequest(patch(broken)))); EXPECT_EQ(blocked.status(), 409); AWAIT(blockedBody, blocked.json()); EXPECT_EQ(errorField(blockedBody, "code"), std::optional<std::string>("paid_fallback_billing_outstanding")); harness::kvStore.delete_(harness::keyToString(pending));
  AWAIT(deleted, mutations::handleAdminApiKeysDelete(adminKeyRequest(patch(broken)))); EXPECT_EQ(deleted.status(), 200); AWAIT(body, deleted.json()); EXPECT(rt::deepEqual(body, json::object{{"id", broken.id}})); EXPECT(!harness::kvStore.has(harness::keyToString(uos::api_keys::apiKeyIdKey(broken.id)))); EXPECT(!harness::kvStore.has(harness::keyToString(uos::api_keys::apiKeyHashKey(broken.hash))));
}
ASYNC_TEST("retired API-key banked reset PATCH is rejected without mutation") {
  MutationFixture fixture; const auto key = seedMutationKey(); const auto before = storedMutationKey(key); const auto size = harness::kvStore.size();
  AWAIT(result, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key, json::object{{"banked_resets_enabled", false}})))); EXPECT_EQ(result.status(), 400); AWAIT(body, result.json()); EXPECT_EQ(errorField(body, "message"), std::optional<std::string>("banked_resets_enabled is retired; configure banked resets per Codex subscription")); EXPECT_EQ(harness::kvStore.size(), size); EXPECT_EQ(storedMutationKey(key), before);
}
ASYNC_TEST("API key mutations preserve raw legacy values and opaque stored members") {
  MutationFixture fixture; auto key = seedMutationKey(json::object{{"expires_at_ms", "-1"}, {"window_ms", "60000"}});
  auto row = rt::KvValue(key.record).as<rt::KvObject>(); row.set("opaque", rt::KvValue::object({{"bytes", rt::KvValue(rt::KvBytes{1, 2})}, {"large", rt::KvValue(rt::BigInt(123))}})); const rt::KvValue raw(row); harness::kvStore.set(harness::keyToString(uos::api_keys::apiKeyIdKey(key.id)), raw);
  AWAIT(noop, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key)))); EXPECT_EQ(noop.status(), 200); EXPECT_EQ(storedMutationKey(key), raw);
  AWAIT(renamed, mutations::handleAdminApiKeysUpdate(adminKeyRequest(patch(key, json::object{{"name", "Legacy rename"}})))); EXPECT_EQ(renamed.status(), 200); EXPECT_EQ(storedMutationKey(key)["opaque"], raw["opaque"]); EXPECT_EQ(storedMutationKey(key)["window_ms"], rt::KvValue(uos::api_keys::USAGE_RESET_PERIOD_MS)); EXPECT_EQ(storedMutationKey(key)["expires_at_ms"], rt::KvValue(-1));
  AWAIT(revoked, mutations::handleAdminApiKeysRevoke(adminKeyRequest(patch(key)))); EXPECT_EQ(revoked.status(), 200); EXPECT_EQ(storedMutationKey(key)["opaque"], raw["opaque"]); AWAIT(unrevoked, mutations::handleAdminApiKeysUnrevoke(adminKeyRequest(patch(key)))); EXPECT_EQ(unrevoked.status(), 200); EXPECT_EQ(storedMutationKey(key)["opaque"], raw["opaque"]);
}

ASYNC_TEST("raw API key mutation responses match four isolated Bun oracle outputs") {
  MutationFixture fixture; rt::globalDateNow() = [] { return 1800000000000.0; };
  auto seeded = seedMutationKey(json::object{{"id", "legacy-mutation"}, {"name", "Legacy mutation"}, {"prefix", "u_legacy"}, {"hash", "legacy-hash"}, {"expires_at_ms", "-1"}, {"window_ms", "60000"}}); harness::kvStore.clear(); auto object = rt::KvValue(seeded.record).as<rt::KvObject>(); object.set("opaque", rt::KvValue::object({{"bytes", rt::KvValue(rt::KvBytes{1, 2})}, {"large", rt::KvValue(rt::BigInt(123))}})); const rt::KvValue original(object); const auto idKey = uos::api_keys::apiKeyIdKey("legacy-mutation"); harness::kvStore.set(harness::keyToString(idKey), original);
  const auto vectors = rt::jsonParse(R"oracle([{"name":"Update","input":{},"status":200,"body":{"id":"legacy-mutation","name":"Legacy mutation","prefix":"u_legacy","created_at_ms":1799999995000,"expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":25,"usage_requests":0,"usage_reset_at_ms":1800000060000,"window_ms":604800000,"paid_fallback_enabled":false,"paid_fallback_limit_credits":0,"paid_fallback_spent_credits":0,"paid_fallback_reserved_credits":0,"paid_fallback_pending_count":0,"paid_fallback_provider_usage":{"metered":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}},"paid_fallback_model_ids":[],"paid_fallback_pricing_checked_at_ms":null}},{"name":"Update","input":{"name":"Legacy rename"},"status":200,"body":{"id":"legacy-mutation","name":"Legacy rename","prefix":"u_legacy","created_at_ms":1799999995000,"expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":25,"usage_requests":0,"usage_reset_at_ms":1800000060000,"window_ms":604800000,"paid_fallback_enabled":false,"paid_fallback_limit_credits":0,"paid_fallback_spent_credits":0,"paid_fallback_reserved_credits":0,"paid_fallback_pending_count":0,"paid_fallback_provider_usage":{"metered":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0},"surplus":{"request_count":0,"input_tokens":0,"output_tokens":0,"total_tokens":0,"spend_microcredits":0}},"paid_fallback_model_ids":[],"paid_fallback_pricing_checked_at_ms":null}},{"name":"Revoke","input":{},"status":200,"body":{"id":"legacy-mutation","revoked_at_ms":1800000000000}},{"name":"Unrevoke","input":{},"status":200,"body":{"id":"legacy-mutation","revoked_at_ms":null}}])oracle");
  for (const auto& vector : vectors.as_array()) {
    auto input = rt::get(vector, "input")->as_object(); input["id"] = "legacy-mutation"; const auto name = rt::getString(vector, "name"); using Mutation = rt::Task<rt::Response> (*)(rt::Request); const Mutation call = name == "Revoke" ? static_cast<Mutation>(mutations::handleAdminApiKeysRevoke) : name == "Unrevoke" ? static_cast<Mutation>(mutations::handleAdminApiKeysUnrevoke) : static_cast<Mutation>(mutations::handleAdminApiKeysUpdate);
    AWAIT(result, call(adminKeyRequest(input))); EXPECT_EQ(result.status(), static_cast<int>(*rt::getNumber(vector, "status"))); AWAIT(text, result.text()); EXPECT_EQ(text, rt::jsonStringify(*rt::get(vector, "body"))); const auto* retained = harness::kvStore.get(harness::keyToString(idKey)); ASSERT(retained); EXPECT_EQ((*retained)["opaque"], original["opaque"]);
  }
}
