// Links: tests/sentinel_replay_coverage_test.cpp tests/helpers/sentinel_kv_stub.cpp src/sentinel/replay_store.cpp
//        src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp
//        src/sentinel/replay_read.cpp src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp
//        src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-coverage.test.ts, in full (all 20 cases). Storage is `openSentinelTestKv()`, the
// runtime's own `:memory:` KV (rt::MemoryKv), or tests/helpers/sentinel_kv_stub's SentinelKvStub where the TS
// constructs it. The TS Proxies are rt::ForwardingKv / rt::ForwardingAtomicOperation subclasses. The environment
// cases clear SENTINEL_REPLAY_KEY and the three production-runtime variables for their scope, so the agent's own
// environment cannot change their outcome (the TS assumes they are unset).
#include "uos/kv.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/utils.hpp"
#include "tests/helpers/sentinel_kv_stub.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace kvfacade = uos::kv;
using namespace uos::sentinel::incident_outbox;

namespace {
  constexpr double NOW = 1'800'000'000'000;
  const std::string GIT_SHA(40, 'b');
  const std::string INCIDENT_UUID = "12345678-1234-4abc-8def-1234567890ab";
  using uos::sentinel::replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
  using uos::sentinel::replay_model::SENTINEL_REPLAY_TTL_MS;

  std::shared_ptr<rt::Kv> openSentinelTestKv() {
    return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  }
  void closeKv(rt::Kv& kv) {
    try {
      kv.close();
    } catch (...) {
      // The stub and a real in-memory KV both allow an idempotent close.
    }
  }

  /**
   * A store whose every atomic commit loses the CAS race. Each chained call stays
   * on the wrapper, so the outcome does not depend on which operation the caller
   * happens to chain first.
   */
  class FailingOperation final : public rt::ForwardingAtomicOperation {
  public:
    using rt::ForwardingAtomicOperation::ForwardingAtomicOperation;
    rt::Task<rt::KvCommitResult> commit() override {
      rt::KvCommitResult lost;
      co_return lost;
    }
  };
  class AlwaysConflictingKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<FailingOperation>(inner.atomic()); }
  };

  // `await assert.rejects(task, /text/)`: the task rejects with an Error whose message contains `text`.
  rt::Task<bool> rejectsWith(rt::Task<void> task, std::string text) {
    std::string message = "<resolved>";
    try {
      co_await std::move(task);
    } catch (const rt::JsError& error) {
      message = error.message;
    }
    if (!rt::includes(message, text)) std::fprintf(stderr, "    expected a rejection containing \"%s\", got \"%s\"\n", text.c_str(), message.c_str());
    co_return rt::includes(message, text);
  }
  template <class T> rt::Task<void> discard(rt::Task<T> task) { co_await std::move(task); }

  SentinelIncidentDependencies uuid(std::string value) {
    SentinelIncidentDependencies dependencies;
    dependencies.randomUuid = [value] { return value; };
    return dependencies;
  }

  // A stored value read back as JSON.
  json::value stored(const rt::KvEntryMaybe<>& entry) { return entry.value.toJson(); }

  // `[...key]` as the JSON array a stored key part list reads back as (string and number parts).
  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(part.isString() ? json::value(part.string()) : rt::makeNumber(part.number()));
    return parts;
  }

  EnvironmentReader environmentOf(std::map<std::string, std::string> values) {
    return EnvironmentReader{[values](const std::string& name) -> std::optional<std::string> {
      const auto found = values.find(name);
      if (found == values.end()) return std::nullopt;
      return found->second;
    }};
  }

  SentinelIncidentIndexObservation observationOf(double status, std::optional<std::string> terminalType = std::string("http.error")) {
    return {.status = status, .stream = false, .completed = false, .framing_valid = true, .terminal_type = std::move(terminalType)};
  }

  SentinelIncidentIndexObservationInput inputOf(double observedAtMs, SentinelIncidentIndexObservation observation, std::string endpoint = "/v1/responses") {
    return {.endpoint = std::move(endpoint), .method = "POST", .gitSha = GIT_SHA, .observedAtMs = observedAtMs, .observation = std::move(observation)};
  }

  // `{ ...value, [key]: member }` on JSON.
  json::value with(const json::value& value, std::string_view key, json::value member) {
    json::object out = value.as_object();
    rt::objectDefine(out, key, std::move(member));
    return out;
  }
}

ASYNC_TEST("incident outbox events are created, completed and conflict-checked") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const std::string fingerprint(64, 'e');
  const rt::KvKey manifestKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, fingerprint, "capture-1"};
  std::vector<double> timestamps;
  timestamps.push_back(0);
  timestamps.push_back(-1);
  timestamps.push_back(1.5);
  timestamps.push_back(rt::Number::NaN);
  for (const double timestamp : timestamps) {
    const bool rejected = co_await rejectsWith(discard(createSentinelIncidentFailureEvent(*kv, timestamp)), "Sentinel incident timestamp is invalid");
    EXPECT(rejected);
  }
  const bool badUuid = co_await rejectsWith(discard(createSentinelIncidentFailureEvent(*kv, NOW, uuid("not-a-uuid"))), "Sentinel incident UUID is invalid");
  EXPECT(badUuid);

  // A runtime UUID is accepted in any case and normalized to lower case.
  const rt::KvEntry<SentinelIncidentFailureEvent> event = co_await createSentinelIncidentFailureEvent(*kv, NOW, uuid(rt::toUpperCase(INCIDENT_UUID)));
  EXPECT_EQ(event.value.incident_id, "provider-" + INCIDENT_UUID);
  EXPECT_EQ(event.value.state, std::string("capturing"));
  EXPECT_EQ(event.value.capture_status, std::string("pending"));
  EXPECT(event.key == (rt::KvKey{SENTINEL_INCIDENT_EVENT_PREFIX, "provider-" + INCIDENT_UUID}));
  EXPECT(isSentinelIncidentFailureEvent(toKvValue(event.value)));
  const bool conflicted =
    co_await rejectsWith(discard(createSentinelIncidentFailureEvent(*kv, NOW, uuid(INCIDENT_UUID))), "Sentinel incident event identifier conflicted");
  EXPECT(conflicted);

  // An unavailable completion keeps the event without evidence.
  const SentinelIncidentFailureEvent unavailable = readySentinelIncidentFailureEvent(event, NOW + 1, IncidentCaptureCompletionUnavailable{});
  EXPECT_EQ(unavailable.state, std::string("ready"));
  EXPECT_EQ(unavailable.capture_status, std::string("unavailable"));
  EXPECT(!unavailable.capture_fingerprint.has_value());
  EXPECT(!unavailable.manifest_key.has_value());
  EXPECT(isSentinelIncidentFailureEvent(toKvValue(unavailable)));
  rt::KvEntry<SentinelIncidentFailureEvent> completedEntry = event;
  completedEntry.value = unavailable;
  EXPECT_THROWS_MESSAGE(readySentinelIncidentFailureEvent(completedEntry, NOW + 2, IncidentCaptureCompletionUnavailable{}), rt::JsError,
                        "Sentinel incident event is not awaiting capture");
  EXPECT_THROWS_MESSAGE(readySentinelIncidentFailureEvent(event, NOW - 1, IncidentCaptureCompletionUnavailable{}), rt::JsError,
                        "Sentinel incident ready timestamp is invalid");

  const IncidentCaptureCompletion duplicate = IncidentCaptureCompletionCaptured{.status = "duplicate", .fingerprint = fingerprint, .manifestKey = manifestKey};
  const bool completed = co_await completeSentinelIncidentFailureEvent(*kv, event, NOW + 1, duplicate);
  EXPECT(completed);
  const rt::KvEntryMaybe<> storedEvent = co_await kv->get(event.key);
  ASSERT(!storedEvent.value.isNullish());
  EXPECT(storedEvent.value["state"] == rt::KvValue("ready"));
  EXPECT(storedEvent.value["capture_status"] == rt::KvValue("duplicate"));
  EXPECT(storedEvent.value["capture_fingerprint"] == rt::KvValue(fingerprint));
  EXPECT(rt::deepEqual(storedEvent.value["manifest_key"].toJson(), keyJson(manifestKey)));
  // The stale entry cannot complete an already completed event.
  const IncidentCaptureCompletion unavailableCompletion = IncidentCaptureCompletionUnavailable{};
  const bool stale = co_await completeSentinelIncidentFailureEvent(*kv, event, NOW + 2, unavailableCompletion);
  EXPECT(!stale);
  const rt::KvEntryMaybe<> afterStale = co_await kv->get(event.key);
  EXPECT(afterStale.value["capture_status"] == rt::KvValue("duplicate"));

  // Capture references validate their incident id, fingerprint and key.
  EXPECT(isSentinelIncidentCaptureReference(toKvValue(SentinelIncidentCaptureReference{.version = 1, .manifest_key = manifestKey})));
  EXPECT(!isSentinelIncidentCaptureReference(rt::jsonParse(R"({"version":1,"manifest_key":["short"]})")));
  EXPECT(!isSentinelIncidentCaptureReference(with(stored(rt::KvEntryMaybe<>{{}, toKvValue(SentinelIncidentCaptureReference{.version = 1, .manifest_key = manifestKey}), {}}),
                                                  "version", 2)));
  EXPECT(isSentinelIncidentId("provider-" + INCIDENT_UUID));
  EXPECT(!isSentinelIncidentId(rt::toUpperCase("provider-" + INCIDENT_UUID)));
  EXPECT(!isSentinelIncidentId(rt::KvValue(7)));
  bool invalidIncident = co_await rejectsWith(linkSentinelReplayToIncident(*kv, "not-an-incident", fingerprint, manifestKey), "Sentinel incident capture reference is invalid");
  EXPECT(invalidIncident);
  bool invalidFingerprint =
    co_await rejectsWith(linkSentinelReplayToIncident(*kv, "provider-" + INCIDENT_UUID, "not-hex", manifestKey), "Sentinel incident capture reference is invalid");
  EXPECT(invalidFingerprint);
  const rt::KvKey shortKey{"too", "short"};
  bool invalidKey = co_await rejectsWith(linkSentinelReplayToIncident(*kv, "provider-" + INCIDENT_UUID, fingerprint, shortKey), "Sentinel replay manifest key is invalid");
  EXPECT(invalidKey);
  co_await linkSentinelReplayToIncident(*kv, "provider-" + INCIDENT_UUID, fingerprint, manifestKey);
  const rt::KvEntryMaybe<> reference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, "provider-" + INCIDENT_UUID, fingerprint});
  json::object expectedReference;
  expectedReference["version"] = 1;
  expectedReference["manifest_key"] = keyJson(manifestKey);
  EXPECT(rt::deepEqual(stored(reference), json::value(expectedReference)));
  closeKv(*kv);
}

ASYNC_TEST("provider degradation is recorded and only published in a production runtime") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const EnvironmentReader production =
    environmentOf({{"DENO_DEPLOY_ORG_SLUG", "ubiquity-dao"}, {"DENO_DEPLOY_APP_SLUG", "ai-ubq-fi"}, {"DENO_TIMELINE", "production"}});
  const EnvironmentReader development =
    environmentOf({{"DENO_DEPLOY_ORG_SLUG", "ubiquity-dao"}, {"DENO_DEPLOY_APP_SLUG", "ai-ubq-fi"}, {"DENO_TIMELINE", "development"}});
  const std::string degradationUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  EXPECT(isSentinelProductionRuntime(production));
  EXPECT(!isSentinelProductionRuntime(development));
  EXPECT(!isSentinelProductionRuntime(environmentOf({})));

  bool invalidTime = co_await rejectsWith(discard(recordSentinelProviderDegradation(*kv, 0)), "Sentinel incident timestamp is invalid");
  EXPECT(invalidTime);
  bool invalidUuid = co_await rejectsWith(discard(recordSentinelProviderDegradation(*kv, NOW, uuid("nope"))), "Sentinel incident UUID is invalid");
  EXPECT(invalidUuid);
  const std::string incidentId = co_await recordSentinelProviderDegradation(*kv, NOW, uuid(degradationUuid));
  EXPECT_EQ(incidentId, "provider-" + degradationUuid);
  const rt::KvEntryMaybe<> row = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_EVENT_PREFIX, incidentId});
  ASSERT(!row.value.isNullish());
  EXPECT(row.value["state"] == rt::KvValue("ready"));
  EXPECT(row.value["capture_status"] == rt::KvValue("unavailable"));
  EXPECT(row.value["ready_at_ms"] == rt::KvValue(NOW));
  bool conflicted = co_await rejectsWith(discard(recordSentinelProviderDegradation(*kv, NOW, uuid(degradationUuid))), "Sentinel incident event identifier conflicted");
  EXPECT(conflicted);

  // The environment entry point refuses a non-production runtime and
  // reports success only when the event really landed.
  SentinelProviderDegradationDependencies developmentDependencies;
  developmentDependencies.environment = development;
  developmentDependencies.kv = kv;
  const bool unpublished = co_await recordSentinelProviderDegradationFromEnvironment(NOW, developmentDependencies);
  EXPECT(!unpublished);
  SentinelProviderDegradationDependencies productionDependencies;
  productionDependencies.environment = production;
  productionDependencies.kv = kv;
  productionDependencies.randomUuid = [] { return std::string("ffffffff-ffff-4fff-8fff-ffffffffffff"); };
  const bool published = co_await recordSentinelProviderDegradationFromEnvironment(NOW, productionDependencies);
  EXPECT(published);
  const rt::KvEntryMaybe<> publishedRow = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_EVENT_PREFIX, "provider-ffffffff-ffff-4fff-8fff-ffffffffffff"});
  EXPECT(publishedRow.value.truthy());
  const std::optional<rt::KvEntry<SentinelIncidentFailureEvent>> skipped = co_await createSentinelIncidentFailureEventFromEnvironment(*kv, NOW, development);
  EXPECT(!skipped.has_value());
  const std::optional<rt::KvEntry<SentinelIncidentFailureEvent>> environmentEvent = co_await createSentinelIncidentFailureEventFromEnvironment(*kv, NOW, production);
  EXPECT(environmentEvent.has_value() && environmentEvent->value.state == "capturing");
  closeKv(*kv);

  // Without any runtime storage the environment helper must not claim success.
  kvfacade::setKvForTest(nullptr);
  SentinelProviderDegradationDependencies withoutKv;
  withoutKv.environment = production;
  const bool unstored = co_await recordSentinelProviderDegradationFromEnvironment(NOW, withoutKv);
  EXPECT(!unstored);
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("incident classification and index rows are bounded to their fixed categories") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelIncidentIndexObservation observation = observationOf(502);
  SentinelIncidentIndexObservation framing = observation;
  framing.stream = true;
  framing.framing_valid = false;
  EXPECT_EQ(classifySentinelIncidentTerminal(framing), std::string("stream_framing"));
  const std::vector<std::pair<std::optional<std::string>, std::string>> terminals = {
    {"http.error", "http_error"}, {"response.failed", "response_failed"}, {"response.incomplete", "response_incomplete"}, {"error", "error"},
    {"deadline", "deadline"},     {"eof", "eof"},                         {"response.completed", "unknown"},           {std::nullopt, "unknown"},
  };
  for (const auto& [terminalType, expected] : terminals) {
    SentinelIncidentIndexObservation typed = observation;
    typed.terminal_type = terminalType;
    EXPECT_EQ(classifySentinelIncidentTerminal(typed), expected);
    EXPECT(isSentinelIncidentTerminalCategory(expected));
  }
  EXPECT(!isSentinelIncidentTerminalCategory("not-a-category"));
  EXPECT(!isSentinelIncidentTerminalCategory(rt::KvValue(3)));

  EXPECT_EQ(normalizeSentinelIncidentEndpoint("/v1/responses?after_ms=0"), std::string("/v1/responses"));
  EXPECT_EQ(normalizeSentinelIncidentEndpoint("/v1/chat/completions"), std::string("/v1/chat/completions"));
  EXPECT_EQ(normalizeSentinelIncidentEndpoint("/v1/embeddings"), std::string("other"));
  EXPECT_EQ(normalizeSentinelIncidentMethod("POST"), std::string("POST"));
  EXPECT_EQ(normalizeSentinelIncidentMethod("GET"), std::string("other"));
  EXPECT(isSentinelIncidentIndexEndpoint("other"));
  EXPECT(!isSentinelIncidentIndexEndpoint("/v1/embeddings"));
  EXPECT(!isSentinelIncidentIndexEndpoint(rt::KvValue(5)));

  std::vector<double> invalidStatuses;
  invalidStatuses.push_back(-1);
  invalidStatuses.push_back(600);
  invalidStatuses.push_back(1.5);
  invalidStatuses.push_back(rt::Number::NaN);
  for (const double status : invalidStatuses) {
    SentinelIncidentIndexObservation invalid = observation;
    invalid.status = status;
    EXPECT_THROWS_MESSAGE(sentinelIncidentClassification({.endpoint = "/v1/responses", .method = "POST", .observation = invalid}), rt::JsError,
                          "Sentinel incident classification status is invalid");
  }
  const std::string classification = sentinelIncidentClassification({.endpoint = "/v1/responses?x=1", .method = "GET", .observation = observation});
  EXPECT(rt::deepEqual(rt::jsonParse(classification),
                       rt::jsonParse(R"({"endpoint":"/v1/responses","method":"other","status":502,"stream":false,"completed":false,"framing_valid":true,"terminal":"http_error"})")));
  // The fingerprint is stable for the same classification and specific to it.
  const SentinelIncidentClassificationInput classified{.endpoint = "/v1/responses", .method = "POST", .observation = observation};
  const std::string fingerprint = co_await sentinelIncidentFingerprint(classified);
  const std::string again = co_await sentinelIncidentFingerprint(classified);
  EXPECT_EQ(fingerprint, again);
  SentinelIncidentClassificationInput other = classified;
  other.observation.status = 500;
  const std::string different = co_await sentinelIncidentFingerprint(other);
  EXPECT(fingerprint != different);

  EXPECT(isSentinelIncidentIndexEvidenceRef(rt::jsonParse(R"({"ref":"capture:abc","digest":null})")));
  EXPECT(isSentinelIncidentIndexEvidenceRef(rt::jsonParse(R"({"ref":"capture:abc","digest":")" + std::string(64, 'a') + R"("})")));
  EXPECT(!isSentinelIncidentIndexEvidenceRef(rt::jsonParse(R"({"ref":"abc","digest":null})")));
  EXPECT(!isSentinelIncidentIndexEvidenceRef(rt::jsonParse(R"({"ref":"capture:abc","digest":"not-hex"})")));
  EXPECT(!isSentinelIncidentIndexEvidenceRef(rt::KvValue(nullptr)));

  SentinelIncidentIndexObservationInput seededInput = inputOf(NOW, observation);
  seededInput.classification = SentinelIncidentIndexClassification{.provider = "cerebras", .model = "gpt-5", .reasoning = "high", .failure_kind = "upstream_timeout"};
  const rt::KvEntry<SentinelIncidentIndexRow> seeded = co_await recordSentinelIncidentIndexObservation(*kv, seededInput, uuid(INCIDENT_UUID));
  const json::value row = json::value_from(seeded.value);
  EXPECT(isSentinelIncidentIndexRow(row));
  const json::value& context = row.as_object().at("context");
  const json::value& provenance = row.as_object().at("provenance");
  const json::value& latest = row.as_object().at("latest");
  const std::vector<json::value> brokenRows = {
    with(row, "version", 2),
    with(row, "incident_id", "provider-nope"),
    with(row, "fingerprint", "short"),
    with(row, "severity", "P1"),
    with(row, "first_seen_at_ms", 0),
    with(row, "last_seen_at_ms", rt::makeNumber(NOW - 1)),
    with(row, "count", 0),
    with(row, "failing_revision", "not-a-sha"),
    with(row, "error_type", "not-a-category"),
    with(row, "context", with(context, "message", "rewritten")),
    with(row, "context", with(context, "location", "here")),
    with(row, "context", with(context, "sample", json::array{"raw"})),
    with(row, "provenance", with(provenance, "endpoint", "/v1/embeddings")),
    with(row, "provenance", with(provenance, "captured_at_ms", -1)),
    with(row, "evidence_ref", rt::jsonParse(R"({"ref":"capture:abc","digest":null})")),
    with(row, "latest", with(latest, "status", 600)),
    with(row, "latest", with(latest, "revision", "nope")),
    with(row, "latest", with(latest, "provider", "has spaces")),
    with(row, "latest", with(latest, "reasoning", json::value(json::string(std::string(33, 'r'))))),
    with(row, "latest", with(latest, "terminal", "not-a-category")),
    with(row, "latest", with(latest, "stream", "yes")),
  };
  for (const json::value& broken : brokenRows) {
    const json::value untrusted = rt::jsonParse(rt::jsonStringify(broken));
    EXPECT(!isSentinelIncidentIndexRow(untrusted));
  }
  EXPECT(!isSentinelIncidentIndexRow(rt::KvValue(nullptr)));
  closeKv(*kv);
}

ASYNC_TEST("index observations merge into one monotonic group row and reject corrupt storage") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelIncidentIndexObservationInput input = inputOf(NOW, observationOf(502));
  SentinelIncidentIndexObservationInput zero = input;
  zero.observedAtMs = 0;
  bool invalidTime = co_await rejectsWith(discard(recordSentinelIncidentIndexObservation(*kv, zero)), "Sentinel incident timestamp is invalid");
  EXPECT(invalidTime);
  bool invalidUuid = co_await rejectsWith(discard(recordSentinelIncidentIndexObservation(*kv, input, uuid("not-a-uuid"))), "Sentinel incident UUID is invalid");
  EXPECT(invalidUuid);

  SentinelIncidentIndexObservationInput classifiedInput = input;
  classifiedInput.classification = SentinelIncidentIndexClassification{.provider = "cerebras", .model = "gpt-5", .reasoning = "high", .failure_kind = "upstream_timeout"};
  const rt::KvEntry<SentinelIncidentIndexRow> first = co_await recordSentinelIncidentIndexObservation(*kv, classifiedInput, uuid(INCIDENT_UUID));
  EXPECT_EQ(first.value.count, 1.0);
  EXPECT_EQ(first.value.severity, std::string("P2"));
  EXPECT_EQ(first.value.incident_id, "provider-" + INCIDENT_UUID);
  EXPECT_EQ(first.value.first_seen_at_ms, NOW);
  EXPECT_EQ(first.value.last_seen_at_ms, NOW);
  EXPECT_EQ(first.value.failing_revision, std::optional<std::string>(GIT_SHA));
  EXPECT(first.value.context.sample.empty());
  EXPECT(!first.value.evidence_ref.has_value());
  ASSERT(first.value.latest.has_value());
  EXPECT_EQ(first.value.latest->model, std::optional<std::string>("gpt-5"));
  EXPECT_EQ(first.value.latest->reasoning, std::optional<std::string>("high"));
  EXPECT(first.key == (rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, first.value.fingerprint}));

  // The classification is metadata, not group identity: the same observed
  // failure keeps its incident id while the latest classification refreshes.
  SentinelIncidentIndexObservationInput secondInput = input;
  secondInput.observedAtMs = NOW + 1'000;
  secondInput.classification = SentinelIncidentIndexClassification{.provider = "surplus", .model = "gpt-5-mini", .reasoning = std::nullopt, .failure_kind = std::nullopt};
  const rt::KvEntry<SentinelIncidentIndexRow> second = co_await recordSentinelIncidentIndexObservation(*kv, secondInput, uuid("ignored-uuid"));
  EXPECT_EQ(second.value.count, 2.0);
  EXPECT_EQ(second.value.incident_id, first.value.incident_id);
  EXPECT_EQ(second.value.first_seen_at_ms, NOW);
  EXPECT_EQ(second.value.last_seen_at_ms, NOW + 1'000);
  ASSERT(second.value.latest.has_value());
  EXPECT_EQ(second.value.latest->provider, std::optional<std::string>("surplus"));
  EXPECT_EQ(second.value.latest->model, std::optional<std::string>("gpt-5-mini"));
  EXPECT(!second.value.latest->reasoning.has_value());
  EXPECT_EQ(second.value.failing_revision, std::optional<std::string>(GIT_SHA));

  // An observation that arrives out of order still counts, but it never
  // replaces the newest recorded classification.
  SentinelIncidentIndexObservationInput thirdInput = input;
  thirdInput.observedAtMs = NOW + 500;
  const rt::KvEntry<SentinelIncidentIndexRow> third = co_await recordSentinelIncidentIndexObservation(*kv, thirdInput);
  EXPECT_EQ(third.value.count, 3.0);
  EXPECT_EQ(third.value.last_seen_at_ms, NOW + 1'000);
  EXPECT(third.value.latest.has_value() && third.value.latest->provider == "surplus");
  EXPECT(!third.value.evidence_ref.has_value());

  const rt::KvValue corrupt = rt::KvValue::object({{"version", rt::KvValue(1)}});
  co_await kv->set(first.key, corrupt);
  bool invalidRecord = co_await rejectsWith(discard(recordSentinelIncidentIndexObservation(*kv, input)), "Sentinel incident index record is invalid");
  EXPECT(invalidRecord);
  closeKv(*kv);

  const std::shared_ptr<rt::Kv> busyKv = openSentinelTestKv();
  // Every CAS attempt loses: the writer must give up rather than count twice.
  AlwaysConflictingKv conflicting(*busyKv);
  bool repeated = co_await rejectsWith(discard(recordSentinelIncidentIndexObservation(conflicting, input)), "Sentinel incident index update conflicted repeatedly");
  EXPECT(repeated);
  const SentinelIncidentClassificationInput classified{.endpoint = input.endpoint, .method = input.method, .observation = input.observation};
  const std::string fingerprint = co_await sentinelIncidentFingerprint(classified);
  const rt::KvEntryMaybe<> untouched = co_await busyKv->get(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, fingerprint});
  EXPECT(untouched.value.isNull());
  closeKv(*busyKv);
}

ASYNC_TEST("index pagination is bounded, resumable and fails closed on corrupt rows") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  std::vector<double> invalidLimits;
  invalidLimits.push_back(0);
  invalidLimits.push_back(-1);
  invalidLimits.push_back(SENTINEL_INCIDENT_INDEX_MAX_PAGE_LIMIT + 1);
  invalidLimits.push_back(1.5);
  for (const double limit : invalidLimits) {
    SentinelIncidentIndexListOptions options;
    options.limit = limit;
    bool rejected = co_await rejectsWith(discard(listSentinelIncidentIndexRows(*kv, options)), "Sentinel incident index pagination is invalid");
    EXPECT(rejected);
  }
  SentinelIncidentIndexListOptions badCursor;
  badCursor.cursor = "not a cursor";
  bool cursorRejected = co_await rejectsWith(discard(listSentinelIncidentIndexRows(*kv, badCursor)), "Sentinel incident index pagination is invalid");
  EXPECT(cursorRejected);
  std::vector<std::string> incidentIds;
  const std::vector<double> statuses{502, 503, 504};
  for (std::size_t index = 0; index < statuses.size(); ++index) {
    const SentinelIncidentIndexObservationInput input =
      inputOf(NOW + static_cast<double>(index), observationOf(statuses[index]), index == 2 ? "/v1/chat/completions" : "/v1/responses");
    const rt::KvEntry<SentinelIncidentIndexRow> row = co_await recordSentinelIncidentIndexObservation(*kv, input, uuid(std::to_string(index) + INCIDENT_UUID.substr(1)));
    incidentIds.push_back(row.value.incident_id);
  }

  const SentinelIncidentIndexPage singlePage = co_await listSentinelIncidentIndexRows(*kv, SentinelIncidentIndexListOptions());
  EXPECT_EQ(singlePage.rows.size(), std::size_t{3});
  EXPECT(!singlePage.cursor.has_value());

  std::vector<std::string> collected;
  std::optional<std::string> cursor;
  for (int page = 0; page < 4; page += 1) {
    SentinelIncidentIndexListOptions options;
    options.cursor = cursor;
    options.limit = 1;
    const SentinelIncidentIndexPage listed = co_await listSentinelIncidentIndexRows(*kv, options);
    for (const SentinelIncidentIndexRow& row : listed.rows) collected.push_back(row.incident_id);
    if (!listed.cursor) break;
    cursor = listed.cursor;
  }
  const auto byCodeUnits = [](const std::string& left, const std::string& right) { return rt::localeCompare(left, right); };
  std::vector<std::string> expectedIds = incidentIds;
  rt::sort(collected, byCodeUnits);
  rt::sort(expectedIds, byCodeUnits);
  EXPECT(collected == expectedIds);

  SentinelIncidentIndexListOptions filter;
  filter.incidentId = incidentIds[1];
  const SentinelIncidentIndexPage filtered = co_await listSentinelIncidentIndexRows(*kv, filter);
  ASSERT_EQ(filtered.rows.size(), std::size_t{1});
  EXPECT_EQ(filtered.rows[0].incident_id, incidentIds[1]);

  // A row whose stored fingerprint disagrees with its own key is corrupt
  // storage, never a silently skipped page.
  const rt::KvKey corruptKey{SENTINEL_INCIDENT_INDEX_PREFIX, std::string(64, '0')};
  const rt::KvValue corrupt(with(json::value_from(singlePage.rows[0]), "fingerprint", json::value(json::string(std::string(64, '1')))));
  co_await kv->set(corruptKey, corrupt);
  bool invalid = co_await rejectsWith(discard(listSentinelIncidentIndexRows(*kv, SentinelIncidentIndexListOptions())), "Sentinel incident index record is invalid");
  EXPECT(invalid);
  closeKv(*kv);
}

ASYNC_TEST("evidence binding is exact, CAS-checked against the winning manifest and persisted atomically") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const rt::KvEntry<SentinelIncidentIndexRow> row = co_await recordSentinelIncidentIndexObservation(*kv, inputOf(NOW, observationOf(502)), uuid(INCIDENT_UUID));
  const std::string referenceFingerprint(64, '2');
  const std::string digest(64, '3');
  const rt::KvKey manifestKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, referenceFingerprint, "capture-bound"};
  const SentinelIncidentIndexEvidenceBinding binding{
    .observedAtMs = NOW,
    .captureId = "capture-bound",
    .gitSha = GIT_SHA,
    .referenceFingerprint = referenceFingerprint,
    .manifestKey = manifestKey,
    .manifestVersionstamp = std::nullopt,
    .capturedAtMs = NOW,
    .digest = digest,
    .expiresAtMs = NOW + SENTINEL_REPLAY_TTL_MS,
  };
  // Every rejection reason in the binding validator is exercised through
  // the real entry point, not through a reach-in. (`manifestKey: "not-a-key"` and
  // `manifestVersionstamp: 5` have no C++ representation: the binding's types exclude them.)
  std::vector<std::pair<std::string, SentinelIncidentIndexEvidenceBinding>> invalidBindings;
  invalidBindings.emplace_back(std::string(63, '4'), binding);
  SentinelIncidentIndexEvidenceBinding candidate = binding;
  candidate.referenceFingerprint = "short";
  invalidBindings.emplace_back(std::string(64, '4'), candidate);
  candidate = binding;
  candidate.digest = "not-hex";
  invalidBindings.emplace_back(std::string(64, '4'), candidate);
  candidate = binding;
  candidate.observedAtMs = 0;
  invalidBindings.emplace_back(std::string(64, '4'), candidate);
  candidate = binding;
  candidate.capturedAtMs = 1.5;
  invalidBindings.emplace_back(std::string(64, '4'), candidate);
  candidate = binding;
  candidate.expiresAtMs = -1;
  invalidBindings.emplace_back(std::string(64, '4'), candidate);
  candidate = binding;
  candidate.manifestKey = rt::KvKey{"not", "seven", "parts"};
  invalidBindings.emplace_back(std::string(64, '4'), candidate);
  for (const auto& [indexFingerprint, invalid] : invalidBindings) {
    bool rejected = co_await rejectsWith(discard(bindSentinelIncidentIndexEvidence(*kv, indexFingerprint, invalid)), "Sentinel incident index evidence binding is invalid");
    EXPECT(rejected);
  }
  bool missing = co_await rejectsWith(discard(bindSentinelIncidentIndexEvidence(*kv, std::string(64, '5'), binding)), "Sentinel incident index record is missing");
  EXPECT(missing);
  const rt::KvValue corrupt = rt::KvValue::object({{"version", rt::KvValue(1)}});
  co_await kv->set(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, std::string(64, '6')}, corrupt);
  bool invalidRecord = co_await rejectsWith(discard(bindSentinelIncidentIndexEvidence(*kv, std::string(64, '6'), binding)), "Sentinel incident index record is invalid");
  EXPECT(invalidRecord);

  const rt::KvEntry<SentinelIncidentIndexRow> bound = co_await bindSentinelIncidentIndexEvidence(*kv, row.value.fingerprint, binding);
  ASSERT(bound.value.evidence_ref.has_value());
  EXPECT_EQ(bound.value.evidence_ref->ref, std::string("capture:capture-bound"));
  EXPECT_EQ(bound.value.evidence_ref->digest, std::optional<std::string>(digest));
  EXPECT_EQ(bound.value.evidence_expires_at_ms, std::optional<double>(NOW + SENTINEL_REPLAY_TTL_MS));
  EXPECT_EQ(bound.value.provenance.captured_at_ms, NOW);
  EXPECT_EQ(bound.value.failing_revision, std::optional<std::string>(GIT_SHA));
  const rt::KvEntryMaybe<> reference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, row.value.incident_id, referenceFingerprint});
  json::object expectedReference;
  expectedReference["version"] = 1;
  expectedReference["manifest_key"] = keyJson(manifestKey);
  EXPECT(rt::deepEqual(stored(reference), json::value(expectedReference)));

  // An already-expired binding writes the row but no expiring reference,
  // and a null revision is recorded as absent rather than guessed.
  SentinelIncidentIndexEvidenceBinding expiredBinding = binding;
  expiredBinding.gitSha = std::nullopt;
  expiredBinding.referenceFingerprint = std::string(64, '7');
  expiredBinding.expiresAtMs = NOW;
  const rt::KvEntry<SentinelIncidentIndexRow> expired = co_await bindSentinelIncidentIndexEvidence(*kv, row.value.fingerprint, expiredBinding);
  EXPECT(!expired.value.failing_revision.has_value());
  EXPECT(expired.value.evidence_ref.has_value() && expired.value.evidence_ref->ref == "capture:capture-bound");
  const rt::KvEntryMaybe<> expiredReference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, row.value.incident_id, std::string(64, '7')});
  EXPECT(expiredReference.value.isNull());

  // A manifest versionstamp that no longer matches can never be bound.
  SentinelIncidentIndexEvidenceBinding staleBinding = binding;
  staleBinding.referenceFingerprint = std::string(64, '8');
  staleBinding.manifestVersionstamp = "00000000000000000000";
  bool stale = co_await rejectsWith(discard(bindSentinelIncidentIndexEvidence(*kv, row.value.fingerprint, staleBinding)),
                                    "Sentinel incident index evidence binding conflicted repeatedly");
  EXPECT(stale);
  const rt::KvEntryMaybe<> staleReference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, row.value.incident_id, std::string(64, '8')});
  EXPECT(staleReference.value.isNull());
  closeKv(*kv);
}

// ---- the replay-store cases (src/sentinel/replay-store.ts) --------------------------------------------------------

namespace {
  namespace sentinel = uos::sentinel;
  using sentinel::replay_model::AcceptedSentinelReplayInput;
  using sentinel::replay_model::SENTINEL_REPLAY_CHUNK_PREFIX;
  using sentinel::replay_model::SENTINEL_REPLAY_DEDUPE_PREFIX;
  using sentinel::replay_model::SENTINEL_REPLAY_REQUEST_PREFIX;
  using sentinel::replay_model::SentinelClientFailureObservation;
  using sentinel::replay_model::SentinelFailureObservation;
  using sentinel::replay_model::SentinelReplayCaptureStatusRow;
  using sentinel::replay_model::SentinelReplayManifest;
  using sentinel::replay_model::SentinelReplayPlaintext;
  using sentinel::replay_observation::PersistDependencies;
  using sentinel::replay_observation::SentinelReplayPersistResult;
  using sentinel::replay_observation::SentinelReplayPersistResultDisabled;
  using sentinel::replay_observation::SentinelReplayPersistResultDuplicate;
  using sentinel::replay_observation::SentinelReplayPersistResultIncomplete;
  using sentinel::replay_observation::SentinelReplayPersistResultStored;
  using sentinel::replay_read::decryptExportedSentinelReplay;
  using sentinel::replay_read::listEncryptedSentinelIncidentReplays;
  using sentinel::replay_read::readSentinelReplayCaptureStatus;
  using sentinel::replay_store::persistEncryptedSentinelReplay;
  using sentinel::replay_store::persistSentinelReplayFromEnvironment;
  using sentinel::replay_store::recordSentinelReplayOmissionFromEnvironment;
  using sentinel::replay_store::writeSentinelReplayCaptureStatus;
  using uos::tests::helpers::sentinel_kv_stub::SentinelKvStub;

  const rt::Bytes KEY_BYTES(32, 0x5a);
  /** Documentation-range client address (RFC 5737 TEST-NET-2), exactly as tests/images.test.ts fixtures use it. */
  const std::string CLIENT_ADDRESS = "198.51.100.7";

  rt::Bytes twelveByteIv(double) { return rt::Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}; }

  SentinelFailureObservation internalObservation() {
    return {
      .status = 502,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "test-provider",
    };
  }
  // `internalObservation({ status: 200, completed: true, terminal_type: "response.completed", failure_kind: null })`
  SentinelFailureObservation successfulInternalObservation() {
    SentinelFailureObservation observation = internalObservation();
    observation.status = 200;
    observation.completed = true;
    observation.terminal_type = "response.completed";
    observation.failure_kind = std::nullopt;
    return observation;
  }

  SentinelClientFailureObservation clientObservation() {
    return {
      .status = 502,
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
  SentinelClientFailureObservation clientObservationWithBody(const std::string& body) {
    SentinelClientFailureObservation observation = clientObservation();
    observation.terminal_body_base64 = rt::btoa(body);
    return observation;
  }
  SentinelClientFailureObservation successfulClientObservation() {
    SentinelClientFailureObservation observation = clientObservation();
    observation.status = 200;
    observation.completed = true;
    observation.terminal_type = "response.completed";
    observation.failure_kind = std::nullopt;
    return observation;
  }

  AcceptedSentinelReplayInput replayInput(const std::string& body, const std::string& requestId) {
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = std::make_shared<rt::Bytes>(rt::encodeUtf8(body)),
      .content_type = "application/json",
      .compatibility_headers = {},
      .request_id = requestId,
      .git_sha = GIT_SHA,
      .deno_revision = "test-deno-revision",
    };
  }

  PersistDependencies storeDependencies(std::shared_ptr<rt::Kv> kv, std::string captureId = "capture-1") {
    return {
      .kv = std::move(kv),
      .keyBytes = KEY_BYTES,
      .now = [] { return NOW; },
      .randomUuid = [captureId] { return captureId; },
      .randomBytes = twelveByteIv,
    };
  }

  rt::Task<SentinelReplayPersistResult> persist(AcceptedSentinelReplayInput input, SentinelFailureObservation observation, PersistDependencies dependencies,
                                                std::optional<SentinelClientFailureObservation> client = std::nullopt) {
    auto persisting = client ? persistEncryptedSentinelReplay(input, observation, dependencies, *client)
                             : persistEncryptedSentinelReplay(input, observation, dependencies);
    SentinelReplayPersistResult result = co_await std::move(persisting);
    co_return result;
  }

  /** One recorded upstream trace, built through the real recorder. */
  rt::Task<sentinel::upstream_capture::SentinelUpstreamTrace> recordedTrace(std::string body) {
    sentinel::upstream_capture::SentinelUpstreamRecorder recorder = sentinel::upstream_capture::createSentinelUpstreamRecorder();
    const sentinel::upstream_capture::SentinelUpstreamAttemptHandle handle = recorder.startAttempt("cerebras");
    rt::ResponseInit init;
    init.status = 200;
    init.headers = rt::Headers{{"content-type", "application/json"}};
    const rt::Response wrapped = handle.wrap(rt::Response(rt::BodyInit(body), init));
    auto reading = wrapped.arrayBuffer();
    co_await std::move(reading);
    sentinel::upstream_capture::SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
    recorder.dispose();
    co_return trace;
  }

  // `withoutRuntimeKv(fn)`: runs with an explicitly unavailable KV binding.
  struct WithoutRuntimeKv {
    WithoutRuntimeKv() { kvfacade::setKvForTest(nullptr); }
    ~WithoutRuntimeKv() { kvfacade::setKvForTest(nullptr); }
  };

  rt::Task<std::vector<rt::KvKey>> requestKeys(rt::Kv& kv) {
    std::vector<rt::KvKey> keys;
    rt::KvListSelector selector;
    selector.prefix = SENTINEL_REPLAY_REQUEST_PREFIX;
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    co_return keys;
  }

  rt::Task<std::vector<rt::KvKey>> keysUnder(rt::Kv& kv, rt::KvKey prefix) {
    std::vector<rt::KvKey> keys;
    rt::KvListSelector selector;
    selector.prefix = std::move(prefix);
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    co_return keys;
  }

  bool zeroed(const rt::Bytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; });
  }

  rt::KvValue keyValue(const rt::KvKey& key) {
    rt::KvArray parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(std::visit([](const auto& value) { return rt::KvValue(value); }, part.value));
    return rt::KvValue(std::move(parts));
  }

  std::string_view statusOf(const SentinelReplayPersistResult& result) {
    return std::visit([](const auto& alternative) -> std::string_view { return std::decay_t<decltype(alternative)>::status; }, result);
  }

  // `{ status: "disabled", reason }`
  bool isDisabled(const SentinelReplayPersistResult& result, std::string_view reason) {
    const auto* disabled = std::get_if<SentinelReplayPersistResultDisabled>(&result);
    return disabled && disabled->reason == reason;
  }

  rt::Task<SentinelReplayCaptureStatusRow> captureStatus(rt::Kv& kv, std::string requestId, std::optional<double> nowMs = std::nullopt) {
    auto reading = nowMs ? readSentinelReplayCaptureStatus(kv, std::move(requestId), *nowMs) : readSentinelReplayCaptureStatus(kv, std::move(requestId));
    SentinelReplayCaptureStatusRow row = co_await std::move(reading);
    co_return row;
  }

  // The rows the environment wrappers read that must not be set by the agent's environment.
  rt::env::ScopedOverride quietEnvironment() {
    return rt::env::ScopedOverride(
      {{"SENTINEL_REPLAY_KEY", std::nullopt}, {"DENO_DEPLOY_ORG_SLUG", std::nullopt}, {"DENO_DEPLOY_APP_SLUG", std::nullopt}, {"DENO_TIMELINE", std::nullopt}});
  }
}

ASYNC_TEST("capture status rows are validated on write") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelReplayCaptureStatusRow valid{
    .version = 1,
    .request_id = "status-row-request",
    .status = "ready",
    .reason = std::nullopt,
    .captured_at_ms = NOW,
    .manifest_key = rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, std::string(64, 'd'), "capture-1"},
    .fingerprint = std::string(64, 'd'),
    .expires_at_ms = NOW + SENTINEL_REPLAY_TTL_MS,
  };
  std::vector<SentinelReplayCaptureStatusRow> invalidRows(12, valid);
  invalidRows[0].version = 2;
  invalidRows[1].request_id = "bad request id";
  invalidRows[2].status = "unknown";
  invalidRows[3].status = "expired";
  invalidRows[4].reason = "not a reason";
  invalidRows[5].captured_at_ms = -1;
  invalidRows[6].captured_at_ms = 1.5;
  invalidRows[7].manifest_key = rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW};
  invalidRows[8].fingerprint = "not-hex";
  invalidRows[9].expires_at_ms = 1.5;
  invalidRows[10].manifest_key = std::nullopt;
  invalidRows[11].status = "disabled";
  for (const SentinelReplayCaptureStatusRow& row : invalidRows) {
    // Stored rows are untrusted input on the way back in: the writer's own
    // validator is the boundary these records are fed through.
    auto writing = writeSentinelReplayCaptureStatus(*kv, row);
    const bool rejected = co_await rejectsWith(std::move(writing), "Sentinel replay capture status is invalid");
    EXPECT(rejected);
  }
  const std::vector<rt::KvKey> none = co_await requestKeys(*kv);
  EXPECT(none.empty());
  auto writing = writeSentinelReplayCaptureStatus(*kv, valid);
  co_await std::move(writing);
  const SentinelReplayCaptureStatusRow stored = co_await captureStatus(*kv, valid.request_id, NOW);
  EXPECT_EQ(stored.status, std::string("ready"));
  EXPECT(!stored.reason.has_value());
  closeKv(*kv);
}

namespace {
  rt::Task<rt::KvCommitResult> rejectedSet() {
    throw rt::JsError("storage unavailable");
    co_return rt::KvCommitResult{};
  }
  // `new Proxy(alwaysConflictingKv(kv), { get: set rejects })`
  class UnwritableKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<FailingOperation>(inner.atomic()); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions = {}) override { return rejectedSet(); }
  };
}

ASYNC_TEST("an omitted capture publishes a bounded status row and never replaces the caller's outcome") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  kvfacade::setKvForTest(kv);
  auto skipping = recordSentinelReplayOmissionFromEnvironment("bad request id", "body_over_limit", NOW);
  co_await std::move(skipping);
  const std::vector<rt::KvKey> none = co_await requestKeys(*kv);
  EXPECT(none.empty());

  auto recording = recordSentinelReplayOmissionFromEnvironment("omitted-request", "body_over_limit", NOW);
  co_await std::move(recording);
  const SentinelReplayCaptureStatusRow omitted = co_await captureStatus(*kv, std::string("omitted-request"), NOW);
  EXPECT_EQ(omitted.status, std::string("disabled"));
  EXPECT_EQ(omitted.reason, std::optional<std::string>("body_over_limit"));
  EXPECT_EQ(omitted.captured_at_ms, NOW);
  EXPECT(!omitted.manifest_key.has_value());
  EXPECT(!omitted.expires_at_ms.has_value());

  // A storage failure is diagnostic only: the call still resolves. The
  // status row is committed through kv.atomic(), so this unwritable seam
  // must fail that channel as well as the plain kv.set path; no row may be
  // written when storage fails.
  kvfacade::setKvForTest(std::make_shared<UnwritableKv>(*kv));
  auto unwritable = recordSentinelReplayOmissionFromEnvironment("unwritable-request", "rejected_before_capture", NOW);
  co_await std::move(unwritable);
  const std::vector<rt::KvKey> keys = co_await requestKeys(*kv);
  EXPECT_EQ(keys, (std::vector<rt::KvKey>{rt::KvKey{"uos_ai", "sentinel_replay", "v1", "request", "omitted-request"}}));
  kvfacade::setKvForTest(nullptr);
  closeKv(*kv);

  {
    const WithoutRuntimeKv unavailable;
    auto runtimeLess = recordSentinelReplayOmissionFromEnvironment("runtime-less-request", "body_unavailable", NOW);
    co_await std::move(runtimeLess);
  }
}

TEST("failed request diagnostics include known semantic and delivery failures") {
  using sentinel::replay_observation::shouldPersistFailedRequestPayload;
  using sentinel::replay_observation::shouldPersistSentinelReplay;
  const SentinelFailureObservation success = successfulInternalObservation();
  const SentinelClientFailureObservation delivered = successfulClientObservation();
  EXPECT(!shouldPersistFailedRequestPayload(success, delivered));
  EXPECT(shouldPersistFailedRequestPayload(success, delivered, true));
  EXPECT(!shouldPersistSentinelReplay(success, delivered));

  for (const std::string terminal : {"response.incomplete", "cancelled"}) {
    SentinelFailureObservation failed = success;
    failed.completed = false;
    failed.terminal_type = terminal;
    SentinelClientFailureObservation client = delivered;
    client.completed = false;
    client.terminal_type = terminal;
    EXPECT(shouldPersistFailedRequestPayload(failed, client));
    EXPECT(!shouldPersistSentinelReplay(failed, client));
  }

  SentinelFailureObservation httpError = success;
  httpError.status = 429;
  EXPECT(shouldPersistFailedRequestPayload(httpError, delivered));
}

ASYNC_TEST("request-only diagnostics authenticate malformed body bytes by original and duplicate request IDs") {
  using sentinel::replay_read::listEncryptedSentinelReplaysByRequestId;
  using sentinel::replay_store::persistFailedRequestPayloadFromEnvironment;
  const rt::env::ScopedOverride environment({{"SENTINEL_REPLAY_KEY", rt::btoa(std::string(32, 'Z'))},
      {"DENO_DEPLOY_ORG_SLUG", std::nullopt}, {"DENO_DEPLOY_APP_SLUG", std::nullopt}, {"DENO_TIMELINE", std::nullopt}});
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  kvfacade::setKvForTest(kv);
  const auto cleanup = rt::scopeExit([&kv] {
    kvfacade::setKvForTest(nullptr);
    closeKv(*kv);
  });

  const rt::Bytes original{'{', '"', 'x', '"', ':', 0xff};
  SentinelFailureObservation failure = internalObservation();
  failure.status = 400;
  SentinelClientFailureObservation client = clientObservationWithBody("synthetic terminal body");
  client.status = 400;
  auto request = [&](std::string id) {
    auto input = replayInput("", id);
    input.endpoint = "/v1/responses?untrusted=synthetic";
    input.body = std::make_shared<rt::Bytes>(original);
    input.compatibility_headers = {{"authorization", "synthetic-only"}};
    return input;
  };

  const auto firstInput = request("diagnostic-first");
  auto firstWrite = persistFailedRequestPayloadFromEnvironment(firstInput, failure, client);
  const auto first = co_await std::move(firstWrite);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(first));
  EXPECT(zeroed(*firstInput.body));

  const auto secondInput = request("diagnostic-second");
  auto secondWrite = persistFailedRequestPayloadFromEnvironment(secondInput, failure, client);
  const auto second = co_await std::move(secondWrite);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultDuplicate>(second));
  EXPECT(zeroed(*secondInput.body));

  auto firstRead = listEncryptedSentinelReplaysByRequestId(*kv, firstInput.request_id);
  const auto firstExport = co_await std::move(firstRead);
  auto secondRead = listEncryptedSentinelReplaysByRequestId(*kv, secondInput.request_id);
  const auto secondExport = co_await std::move(secondRead);
  EXPECT_EQ(secondExport.status.request_id, secondInput.request_id);
  EXPECT_EQ(secondExport.status.status, std::string("incomplete"));
  EXPECT(firstExport.status.manifest_key == secondExport.status.manifest_key);
  EXPECT(firstExport.status.fingerprint == secondExport.status.fingerprint);
  EXPECT(firstExport.status.expires_at_ms == secondExport.status.expires_at_ms);
  ASSERT_EQ(secondExport.captures.size(), std::size_t{1});
  EXPECT(firstExport.captures == secondExport.captures);

  auto decrypting = decryptExportedSentinelReplay(secondExport.captures[0], KEY_BYTES);
  const auto decrypted = co_await std::move(decrypting);
  EXPECT_EQ(decrypted.request_id, firstInput.request_id);
  EXPECT_EQ(decrypted.endpoint, std::string("/v1/responses"));
  EXPECT(decrypted.compatibility_headers.empty());
  EXPECT(decrypted.upstream.attempts.empty());
  EXPECT(!decrypted.client_observation.terminal_body_base64);
  EXPECT(decrypted.body && *decrypted.body == original);
  EXPECT_EQ(decrypted.capture_status, std::optional<std::string>("incomplete"));

  auto emptyInput = replayInput("", "diagnostic-empty");
  emptyInput.bodyStatus = "body_empty";
  auto emptyWrite = persistFailedRequestPayloadFromEnvironment(emptyInput, failure, client);
  const auto empty = co_await std::move(emptyWrite);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(empty));
  auto emptyRead = listEncryptedSentinelReplaysByRequestId(*kv, emptyInput.request_id);
  const auto emptyExport = co_await std::move(emptyRead);
  ASSERT_EQ(emptyExport.captures.size(), std::size_t{1});
  auto decryptingEmpty = decryptExportedSentinelReplay(emptyExport.captures[0], KEY_BYTES);
  const auto emptyDecoded = co_await std::move(decryptingEmpty);
  ASSERT(emptyDecoded.body != nullptr);
  EXPECT(emptyDecoded.body->empty());
  EXPECT_EQ(emptyDecoded.body_bytes, std::optional<double>(0));

  auto findingIncidents = listSentinelIncidentIndexRows(*kv, SentinelIncidentIndexListOptions{});
  const auto incidents = co_await std::move(findingIncidents);
  EXPECT(incidents.rows.empty());
}

ASYNC_TEST("environment persistence reports how it was disabled and zeroes the request body") {
  const rt::env::ScopedOverride environment = quietEnvironment();
  const SentinelFailureObservation failure = internalObservation();

  {
    const WithoutRuntimeKv unavailable;
    const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5"})", "env-kv-unavailable");
    auto persisting = persistSentinelReplayFromEnvironment(input, failure);
    const SentinelReplayPersistResult result = co_await std::move(persisting);
    EXPECT(isDisabled(result, "kv_unavailable"));
    // The request-owned buffer is zeroed on every exit path, including the
    // one where no storage or key was available at all.
    EXPECT(zeroed(*input.body));
  }

  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  kvfacade::setKvForTest(kv);
  const SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5","stream":false})", "env-key-missing");
  auto persisting = persistSentinelReplayFromEnvironment(input, failure, client);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  EXPECT(isDisabled(result, "key_missing"));
  // The request body is zeroed whatever the outcome.
  EXPECT(zeroed(*input.body));

  const SentinelReplayCaptureStatusRow status = co_await captureStatus(*kv, input.request_id);
  EXPECT_EQ(status.status, std::string("disabled"));
  EXPECT_EQ(status.reason, std::optional<std::string>("key_missing"));
  EXPECT(!status.fingerprint.has_value());

  // The durable index observation is recorded before the key lookup, so a
  // missing key still leaves a discoverable row without evidence.
  const SentinelIncidentClassificationInput classification{
    .endpoint = input.endpoint,
    .method = input.method,
    .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                    .terminal_type = client.terminal_type},
  };
  auto fingerprinting = sentinelIncidentFingerprint(classification);
  const std::string indexFingerprint = co_await std::move(fingerprinting);
  const rt::KvEntryMaybe<> indexRow = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, indexFingerprint});
  ASSERT(!indexRow.value.isNull());  // the index observation must be durable
  EXPECT(indexRow.value["evidence_ref"].isNull());
  EXPECT_EQ(indexRow.value["count"], rt::KvValue(1));

  // A successful request is never persisted, and no status row is written
  // for a request id that could not be recorded.
  const AcceptedSentinelReplayInput localOnly = replayInput(R"({"model":"gpt-5"})", "env-successful-request");
  const SentinelFailureObservation success = successfulInternalObservation();
  const std::optional<SentinelClientFailureObservation> successClient = successfulClientObservation();
  auto refusing = persistSentinelReplayFromEnvironment(localOnly, success, successClient);
  const bool refused = co_await rejectsWith(discard(std::move(refusing)), "A successful request cannot be persisted as a sentinel replay");
  EXPECT(refused);
  EXPECT(zeroed(*localOnly.body));

  const AcceptedSentinelReplayInput unnameable = replayInput(R"({"model":"gpt-5"})", "bad request id");
  auto persistingUnnameable = persistSentinelReplayFromEnvironment(unnameable, failure, client);
  const SentinelReplayPersistResult unnameableResult = co_await std::move(persistingUnnameable);
  EXPECT(isDisabled(unnameableResult, "key_missing"));
  const std::vector<rt::KvKey> keys = co_await requestKeys(*kv);
  ASSERT_EQ(keys.size(), std::size_t{1});
  EXPECT(keys[0].back() == rt::KvKeyPart("env-key-missing"));
  kvfacade::setKvForTest(nullptr);
  closeKv(*kv);
}

namespace {
  // A synchronous throw keeps the failure on the awaited call path: no
  // rejected promise can be orphaned away from the caller's try block.
  class AtomicThrowsKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    std::unique_ptr<rt::AtomicOperation> atomic() override { throw rt::JsError("index unavailable"); }
  };
}

ASYNC_TEST("an index write failure is reported as deferred without changing the persist outcome") {
  const rt::env::ScopedOverride environment = quietEnvironment();
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  // The index observation is the first storage write: failing it must leave
  // the disabled outcome intact and must be announced, never silent.
  rt::console::Capture warnings({rt::console::Method::warn});
  kvfacade::setKvForTest(std::make_shared<AtomicThrowsKv>(*kv));
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5"})", "env-index-failure");
  const SentinelFailureObservation failure = internalObservation();
  const std::optional<SentinelClientFailureObservation> client = clientObservationWithBody("x");
  auto persisting = persistSentinelReplayFromEnvironment(input, failure, client);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  EXPECT(isDisabled(result, "key_missing"));
  EXPECT(zeroed(*input.body));
  ASSERT_EQ(warnings.calls().size(), std::size_t{1});
  EXPECT(rt::includes(warnings.calls()[0].text, "sentinel_incident"));
  EXPECT(rt::includes(warnings.calls()[0].text, "index_write_failed"));
  kvfacade::setKvForTest(nullptr);
  closeKv(*kv);
}

ASYNC_TEST("in-memory sentinel KV keeps Deno's versionstamp CAS, ordering and cursor contract") {
  SentinelKvStub kv;
  const rt::KvEntryMaybe<> absent = co_await kv.get(rt::KvKey{"absent"});
  EXPECT(absent == (rt::KvEntryMaybe<>{.key = rt::KvKey{"absent"}, .value = rt::KvValue(nullptr), .versionstamp = std::nullopt}));

  co_await kv.set(rt::KvKey{"m", 2}, rt::KvValue("two"));
  co_await kv.set(rt::KvKey{"m", "b"}, rt::KvValue("string-b"));
  co_await kv.set(rt::KvKey{"m", 10}, rt::KvValue("ten"));
  const std::vector<rt::KvKey> ordered = co_await keysUnder(kv, rt::KvKey{"m"});
  EXPECT_EQ(ordered, (std::vector<rt::KvKey>{rt::KvKey{"m", "b"}, rt::KvKey{"m", 2}, rt::KvKey{"m", 10}}));

  // Stored values are snapshots: mutating the caller's buffer must not
  // rewrite what a later read observes.
  rt::KvBytes original{1, 2, 3};
  const rt::KvValue originalValue(original);
  co_await kv.set(rt::KvKey{"bytes"}, originalValue);
  std::fill(original.begin(), original.end(), 9);
  const rt::KvEntryMaybe<> bytes = co_await kv.get(rt::KvKey{"bytes"});
  EXPECT(bytes.value.isBytes() && bytes.value.asBytes() == (rt::KvBytes{1, 2, 3}));

  // A cursor resumes strictly after the last yielded entry and is exhausted
  // into the empty cursor Deno KV reports.
  SentinelKvStub* store = &kv;
  const auto pageKeys = [store](std::optional<std::string> cursor) -> rt::Task<std::vector<rt::KvKey>> {
    std::vector<rt::KvKey> keys;
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{"m"};
    rt::KvListOptions options;
    options.limit = 1;
    options.cursor = std::move(cursor);
    const std::unique_ptr<rt::KvListIterator> iterator = store->list(std::move(selector), std::move(options));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    co_return keys;
  };
  auto firstPageKeys = pageKeys(std::nullopt);
  const std::vector<rt::KvKey> first = co_await std::move(firstPageKeys);
  EXPECT_EQ(first, (std::vector<rt::KvKey>{rt::KvKey{"m", "b"}}));
  // A consumer that stops after one entry gets the cursor that resumes at
  // the next key; only a page read to its end reports the empty cursor.
  rt::KvListSelector firstSelector;
  firstSelector.prefix = rt::KvKey{"m"};
  rt::KvListOptions firstOptions;
  firstOptions.limit = 1;
  const std::unique_ptr<rt::KvListIterator> firstPage = kv.list(std::move(firstSelector), std::move(firstOptions));
  const std::optional<rt::KvEntry<>> head = co_await firstPage->next();
  ASSERT(head.has_value());
  EXPECT_EQ(head->key, (rt::KvKey{"m", "b"}));
  auto resumedPageKeys = pageKeys(firstPage->cursor());
  const std::vector<rt::KvKey> resumed = co_await std::move(resumedPageKeys);
  EXPECT_EQ(resumed, (std::vector<rt::KvKey>{rt::KvKey{"m", 2}}));
  rt::KvListSelector allSelector;
  allSelector.prefix = rt::KvKey{"m"};
  rt::KvListOptions allOptions;
  allOptions.limit = 10;
  const std::unique_ptr<rt::KvListIterator> exhausted = kv.list(std::move(allSelector), std::move(allOptions));
  double consumedPageEntries = 0;
  while (const std::optional<rt::KvEntry<>> entry = co_await exhausted->next()) {
    EXPECT(entry->key[0] == rt::KvKeyPart("m"));
    consumedPageEntries += 1;
  }
  EXPECT_EQ(consumedPageEntries, 3.0);
  EXPECT_EQ(exhausted->cursor(), std::string(""));

  // `start` is inclusive, exactly as the export scanner assumes.
  rt::KvListSelector startSelector;
  startSelector.prefix = rt::KvKey{"m"};
  startSelector.start = rt::KvKey{"m", 2};
  rt::KvListOptions startOptions;
  startOptions.limit = 10;
  const std::unique_ptr<rt::KvListIterator> fromStart = kv.list(std::move(startSelector), std::move(startOptions));
  std::vector<rt::KvKey> started;
  while (const std::optional<rt::KvEntry<>> entry = co_await fromStart->next()) started.push_back(entry->key);
  EXPECT_EQ(started, (std::vector<rt::KvKey>{rt::KvKey{"m", 2}, rt::KvKey{"m", 10}}));

  // CAS: a check against an absent key succeeds once, then conflicts.
  const std::unique_ptr<rt::AtomicOperation> create = kv.atomic();
  create->check(rt::AtomicCheck(rt::KvKey{"cas"}, std::nullopt)).set(rt::KvKey{"cas"}, rt::KvValue("first"));
  const rt::KvCommitResult created = co_await create->commit();
  EXPECT_EQ(created.ok, true);
  const std::unique_ptr<rt::AtomicOperation> stale = kv.atomic();
  stale->check(rt::AtomicCheck(rt::KvKey{"cas"}, std::nullopt)).set(rt::KvKey{"cas"}, rt::KvValue("second"));
  const rt::KvCommitResult staleResult = co_await stale->commit();
  EXPECT_EQ(staleResult.ok, false);
  const rt::KvEntryMaybe<> afterStale = co_await kv.get(rt::KvKey{"cas"});
  EXPECT_EQ(afterStale.value, rt::KvValue("first"));
  const std::unique_ptr<rt::AtomicOperation> matched = kv.atomic();
  matched->check(rt::AtomicCheck(rt::KvKey{"cas"}, afterStale.versionstamp)).set(rt::KvKey{"cas"}, rt::KvValue("third"));
  const rt::KvCommitResult matchedResult = co_await matched->commit();
  EXPECT_EQ(matchedResult.ok, true);
  const rt::KvEntryMaybe<> afterMatch = co_await kv.get(rt::KvKey{"cas"});
  EXPECT_EQ(afterMatch.value, rt::KvValue("third"));

  std::vector<rt::KvKey> manyKeys;
  manyKeys.push_back(rt::KvKey{"cas"});
  manyKeys.push_back(rt::KvKey{"absent"});
  const std::vector<rt::KvEntryMaybe<>> many = co_await kv.getMany(manyKeys);
  ASSERT_EQ(many.size(), std::size_t{2});
  EXPECT_EQ(many[0].value, rt::KvValue("third"));
  EXPECT(many[1] == (rt::KvEntryMaybe<>{.key = rt::KvKey{"absent"}, .value = rt::KvValue(nullptr), .versionstamp = std::nullopt}));

  co_await kv.delete_(rt::KvKey{"cas"});
  const rt::KvEntryMaybe<> deleted = co_await kv.get(rt::KvKey{"cas"});
  EXPECT(deleted.value.isNull());
  kv.close();
}

ASYNC_TEST("a request-only capture is stored with its dedupe, manifest, chunk and status rows") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  kvfacade::setKvForTest(kv);
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5","stream":false})", "store-basic-request");
  const rt::Bytes acceptedBody = *input.body;
  auto storing = persist(input, internalObservation(), storeDependencies(kv));
  const SentinelReplayPersistResult result = co_await std::move(storing);
  // The assertion narrows the result to the stored variant for the reads below.
  const auto* stored = std::get_if<SentinelReplayPersistResultStored>(&result);
  ASSERT(stored != nullptr);
  EXPECT_EQ(stored->manifest.version, 1.0);
  EXPECT_EQ(stored->manifest.capture_id, std::string("capture-1"));
  EXPECT_EQ(stored->manifest.algorithm, std::string("AES-256-GCM"));
  EXPECT_EQ(stored->manifest.compression, std::string("gzip"));
  EXPECT_EQ(stored->manifest.captured_at_ms, NOW);
  EXPECT_EQ(stored->manifest.expires_at_ms, NOW + SENTINEL_REPLAY_TTL_MS);
  EXPECT_EQ(stored->manifest.chunk_count, 1.0);
  static const rt::Regex IV("^[A-Za-z0-9_-]{16}$", "");
  EXPECT(IV.test(stored->manifest.iv));
  ASSERT(stored->manifest_key.has_value());
  EXPECT_EQ(*stored->manifest_key, (rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, stored->manifest.fingerprint, "capture-1"}));

  // The dedupe row is the only thing a second identical request can see.
  const rt::KvEntryMaybe<> dedupe = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_DEDUPE_PREFIX, stored->manifest.fingerprint});
  EXPECT_EQ(dedupe.value["manifest_key"], keyValue(*stored->manifest_key));
  const rt::KvEntryMaybe<> chunk = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, "capture-1", 0});
  ASSERT(chunk.value.isBytes());
  EXPECT_EQ(static_cast<double>(chunk.value.asBytes().size()), stored->manifest.ciphertext_bytes);

  // The request id resolves to its own status row, and to the capture.
  const SentinelReplayCaptureStatusRow status = co_await captureStatus(*kv, input.request_id);
  EXPECT_EQ(status.status, std::string("incomplete"));
  EXPECT(!status.reason.has_value());
  EXPECT_EQ(status.fingerprint, std::optional<std::string>(stored->manifest.fingerprint));
  EXPECT_EQ(status.expires_at_ms, std::optional<double>(NOW + SENTINEL_REPLAY_TTL_MS));

  sentinel::replay_read::ListEncryptedSentinelIncidentReplaysOptions listing;
  listing.incidentId = "provider-" + INCIDENT_UUID;
  auto listingTask = listEncryptedSentinelIncidentReplays(*kv, listing);
  const sentinel::replay_read::EncryptedSentinelReplayPage listed = co_await std::move(listingTask);
  EXPECT(listed.captures.empty());

  // The stored envelope decrypts back to the exact accepted request.
  const rt::KvEntryMaybe<> manifestEntry = co_await kv->get(*stored->manifest_key);
  ASSERT(!manifestEntry.value.isNull());
  const rt::KvEntryMaybe<> chunks = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, "capture-1", 0});
  ASSERT(chunks.value.isBytes());
  std::vector<std::string> encodedChunks;
  encodedChunks.push_back(uos::utils::base64UrlEncode(chunks.value.asBytes()));
  const sentinel::replay_model::ExportedSentinelReplayCapture exported{.manifest = sentinel::replay_read::toSentinelReplayManifest(manifestEntry.value),
                                                                       .chunks = std::move(encodedChunks)};
  auto decrypting = decryptExportedSentinelReplay(exported, KEY_BYTES);
  const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.version, 3.0);
  EXPECT_EQ(plaintext.endpoint, input.endpoint);
  EXPECT_EQ(plaintext.method, std::string("POST"));
  EXPECT_EQ(plaintext.request_id, input.request_id);
  EXPECT_EQ(plaintext.git_sha, GIT_SHA);
  EXPECT(*plaintext.body == acceptedBody);
  EXPECT_EQ(plaintext.capture_status, std::optional<std::string>("incomplete"));
  EXPECT_EQ(plaintext.replay_coverage, std::optional<std::string>("unavailable"));
  EXPECT_EQ(plaintext.unavailable, (std::optional<std::vector<std::string>>(std::vector<std::string>{"upstream_trace_empty", "downstream_terminal_body_unavailable"})));
  EXPECT(plaintext.upstream.attempts.empty());
  ASSERT(plaintext.settings.has_value());  // a version-3 capture seals its request settings
  EXPECT_EQ(plaintext.settings->source, std::string("recorded_request_body"));
  EXPECT_EQ(plaintext.settings->model_requested, std::optional<std::string>("gpt-5"));
  EXPECT_EQ(plaintext.settings->stream_requested, std::optional<bool>(false));
  EXPECT_EQ(plaintext.observation.failure_kind, std::optional<std::string>("upstream_timeout"));
  EXPECT_EQ(plaintext.client_observation.terminal_type, std::optional<std::string>("http.error"));
  kvfacade::setKvForTest(nullptr);
  closeKv(*kv);
}

ASYNC_TEST("a captured trace binds the durable index row, the incident event and the capture reference") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  client.terminal_body_truncated = false;
  auto tracing = recordedTrace(R"({"error":"upstream failed"})");
  const sentinel::upstream_capture::SentinelUpstreamTrace trace = co_await std::move(tracing);
  AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5","stream":true})", "store-indexed-request");
  input.upstream = trace;
  const rt::Bytes acceptedBody = *input.body;
  SentinelIncidentIndexObservationInput observation{
    .endpoint = input.endpoint,
    .method = input.method,
    .gitSha = GIT_SHA,
    .observedAtMs = NOW - 10,
    .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                    .terminal_type = client.terminal_type},
  };
  observation.classification = SentinelIncidentIndexClassification{.provider = "test-provider", .model = std::nullopt, .reasoning = std::nullopt, .failure_kind = "upstream_timeout"};
  auto recording = recordSentinelIncidentIndexObservation(*kv, observation);
  const rt::KvEntry<SentinelIncidentIndexRow> index = co_await std::move(recording);
  const std::string incidentId = index.value.incident_id;
  auto creating = createSentinelIncidentFailureEvent(*kv, NOW - 5, uuid(INCIDENT_UUID));
  const rt::KvEntry<SentinelIncidentFailureEvent> event = co_await std::move(creating);

  PersistDependencies dependencies = storeDependencies(kv);
  dependencies.incidentEvent = event;
  auto storing = persist(input, internalObservation(), dependencies, client);
  const SentinelReplayPersistResult result = co_await std::move(storing);
  const auto* stored = std::get_if<SentinelReplayPersistResultStored>(&result);
  ASSERT(stored != nullptr);
  ASSERT(stored->manifest_key.has_value());
  EXPECT_EQ(index.key, (rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, index.value.fingerprint}));

  // The index row keeps its identity and gains this capture's exact evidence.
  const rt::KvEntryMaybe<> bound = co_await kv->get(index.key);
  ASSERT(!bound.value.isNull());  // the bound index row must exist
  EXPECT_EQ(bound.value["evidence_ref"]["ref"], rt::KvValue("capture:capture-1"));
  static const rt::Regex DIGEST("^[0-9a-f]{64}$", "");
  EXPECT(bound.value["evidence_ref"]["digest"].isString() && DIGEST.test(bound.value["evidence_ref"]["digest"].asString()));
  EXPECT_EQ(bound.value["failing_revision"], rt::KvValue(GIT_SHA));
  EXPECT_EQ(bound.value["provenance"]["captured_at_ms"], rt::KvValue(NOW));
  EXPECT_EQ(bound.value["evidence_expires_at_ms"], rt::KvValue(NOW + SENTINEL_REPLAY_TTL_MS));

  // The outbox event and the incident capture reference are both durable.
  const rt::KvEntryMaybe<> eventEntry = co_await kv->get(event.key);
  ASSERT(!eventEntry.value.isNull());  // the outbox event must exist
  EXPECT_EQ(eventEntry.value["state"], rt::KvValue("ready"));
  EXPECT_EQ(eventEntry.value["capture_status"], rt::KvValue("stored"));
  EXPECT_EQ(eventEntry.value["capture_fingerprint"], rt::KvValue(stored->manifest.fingerprint));
  EXPECT_EQ(eventEntry.value["manifest_key"], keyValue(*stored->manifest_key));
  EXPECT_EQ(eventEntry.value["ready_at_ms"], rt::KvValue(NOW));
  const rt::KvEntryMaybe<> reference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, stored->manifest.fingerprint});
  rt::KvObject expectedReference;
  expectedReference.set("version", rt::KvValue(1));
  expectedReference.set("manifest_key", keyValue(*stored->manifest_key));
  EXPECT_EQ(reference.value, rt::KvValue(std::move(expectedReference)));

  // Listing by incident returns exactly that capture, and it decrypts to
  // the accepted request plus the recorded upstream attempt.
  sentinel::replay_read::ListEncryptedSentinelIncidentReplaysOptions listing;
  listing.incidentId = incidentId;
  auto listingTask = listEncryptedSentinelIncidentReplays(*kv, listing);
  const sentinel::replay_read::EncryptedSentinelReplayPage listed = co_await std::move(listingTask);
  ASSERT_EQ(listed.captures.size(), std::size_t{1});
  auto decrypting = decryptExportedSentinelReplay(listed.captures[0], KEY_BYTES);
  const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.capture_status, std::optional<std::string>("ready"));
  EXPECT_EQ(plaintext.replay_coverage, std::optional<std::string>("full"));
  EXPECT_EQ(plaintext.unavailable, (std::optional<std::vector<std::string>>(std::vector<std::string>{})));
  EXPECT(plaintext.upstream == trace);
  EXPECT(*plaintext.body == acceptedBody);
  EXPECT(plaintext.downstream.has_value() && plaintext.downstream->terminal_type == std::optional<std::string>("http.error"));
  EXPECT_EQ(plaintext.client_observation.terminal_body_base64, std::optional<std::string>(rt::btoa("upstream failed")));

  SentinelIncidentIndexListOptions rowsOptions;
  rowsOptions.incidentId = incidentId;
  auto rowsTask = listSentinelIncidentIndexRows(*kv, rowsOptions);
  const SentinelIncidentIndexPage rows = co_await std::move(rowsTask);
  ASSERT_EQ(rows.rows.size(), std::size_t{1});
  EXPECT(rows.rows[0].evidence_ref && rows.rows[0].evidence_ref->ref == "capture:capture-1");
  closeKv(*kv);
}

ASYNC_TEST("a duplicate capture is settled against the winning manifest without creating evidence") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  auto tracing = recordedTrace(R"({"error":"upstream failed"})");
  const sentinel::upstream_capture::SentinelUpstreamTrace trace = co_await std::move(tracing);
  const SentinelFailureObservation failure = internalObservation();
  const SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  AcceptedSentinelReplayInput firstInput = replayInput(R"({"model":"gpt-5"})", "duplicate-first-request");
  firstInput.upstream = trace;
  auto storingFirst = persist(firstInput, failure, storeDependencies(kv, "capture-winner"), client);
  const SentinelReplayPersistResult firstResult = co_await std::move(storingFirst);
  const auto* first = std::get_if<SentinelReplayPersistResultStored>(&firstResult);
  ASSERT(first != nullptr);
  ASSERT(first->manifest_key.has_value());

  // A durable index observation appears between the two identical requests.
  SentinelIncidentIndexObservationInput observation{
    .endpoint = "/v1/responses",
    .method = "POST",
    .gitSha = GIT_SHA,
    .observedAtMs = NOW,
    .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                    .terminal_type = client.terminal_type},
  };
  observation.classification = SentinelIncidentIndexClassification{.provider = "test-provider", .model = std::nullopt, .reasoning = std::nullopt, .failure_kind = std::nullopt};
  auto recording = recordSentinelIncidentIndexObservation(*kv, observation);
  const rt::KvEntry<SentinelIncidentIndexRow> index = co_await std::move(recording);
  auto creating = createSentinelIncidentFailureEvent(*kv, NOW, uuid("99999999-9999-4999-8999-999999999999"));
  const rt::KvEntry<SentinelIncidentFailureEvent> duplicateEvent = co_await std::move(creating);

  AcceptedSentinelReplayInput secondInput = replayInput(R"({"model":"gpt-5"})", "duplicate-second-request");
  secondInput.upstream = trace;
  PersistDependencies dependencies = storeDependencies(kv, "capture-loser");
  dependencies.incidentEvent = duplicateEvent;
  auto storingSecond = persist(secondInput, failure, dependencies, client);
  const SentinelReplayPersistResult duplicateResult = co_await std::move(storingSecond);
  const auto* duplicate = std::get_if<SentinelReplayPersistResultDuplicate>(&duplicateResult);
  ASSERT(duplicate != nullptr);
  EXPECT_EQ(duplicate->fingerprint, first->manifest.fingerprint);
  EXPECT(duplicate->manifest_key == first->manifest_key);

  // The loser wrote no second capture, but the index row points at the
  // winner's actual ciphertext.
  const rt::KvEntryMaybe<> loser = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, first->manifest.fingerprint, "capture-loser"});
  EXPECT(loser.value.isNull());
  const rt::KvEntryMaybe<> bound = co_await kv->get(index.key);
  EXPECT_EQ(bound.value["evidence_ref"]["ref"], rt::KvValue("capture:capture-winner"));
  const rt::KvEntryMaybe<> reference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, index.value.incident_id, first->manifest.fingerprint});
  rt::KvObject expectedReference;
  expectedReference.set("version", rt::KvValue(1));
  expectedReference.set("manifest_key", keyValue(*first->manifest_key));
  EXPECT_EQ(reference.value, rt::KvValue(std::move(expectedReference)));

  const rt::KvEntryMaybe<> eventEntry = co_await kv->get(duplicateEvent.key);
  ASSERT(!eventEntry.value.isNull());  // the duplicate request must settle its own outbox event
  EXPECT_EQ(eventEntry.value["state"], rt::KvValue("ready"));
  EXPECT_EQ(eventEntry.value["capture_status"], rt::KvValue("duplicate"));

  // The duplicate request still resolves to the winning capture.
  const SentinelReplayCaptureStatusRow status = co_await captureStatus(*kv, std::string("duplicate-second-request"));
  EXPECT_EQ(status.status, std::string("ready"));
  EXPECT(status.manifest_key == first->manifest_key);
  EXPECT_EQ(status.fingerprint, std::optional<std::string>(first->manifest.fingerprint));
  closeKv(*kv);
}

ASYNC_TEST("a duplicate without a durable index row binds nothing and reports the winner") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelFailureObservation failure = internalObservation();
  const SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5","stream":false})", "unindexed-first-request");
  auto storingFirst = persist(input, failure, storeDependencies(kv, "capture-unindexed"), client);
  const SentinelReplayPersistResult firstResult = co_await std::move(storingFirst);
  const auto* first = std::get_if<SentinelReplayPersistResultStored>(&firstResult);
  ASSERT(first != nullptr);

  AcceptedSentinelReplayInput second = input;
  second.request_id = "unindexed-second-request";
  auto storingSecond = persist(second, failure, storeDependencies(kv, "capture-unindexed-loser"), client);
  const SentinelReplayPersistResult duplicate = co_await std::move(storingSecond);
  EXPECT_EQ(statusOf(duplicate), std::string_view("duplicate"));
  const SentinelIncidentClassificationInput classification{
    .endpoint = input.endpoint,
    .method = input.method,
    .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                    .terminal_type = client.terminal_type},
  };
  auto fingerprinting = sentinelIncidentFingerprint(classification);
  const std::string indexFingerprint = co_await std::move(fingerprinting);
  const rt::KvEntryMaybe<> indexRow = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, indexFingerprint});
  EXPECT(indexRow.value.isNull());
  sentinel::replay_read::ListEncryptedSentinelIncidentReplaysOptions listing;
  listing.incidentId = "provider-" + INCIDENT_UUID;
  auto listingTask = listEncryptedSentinelIncidentReplays(*kv, listing);
  const sentinel::replay_read::EncryptedSentinelReplayPage listed = co_await std::move(listingTask);
  EXPECT(listed.captures.empty());
  const SentinelReplayCaptureStatusRow status = co_await captureStatus(*kv, std::string("unindexed-second-request"));
  EXPECT_EQ(status.status, std::string("incomplete"));
  EXPECT_EQ(status.fingerprint, std::optional<std::string>(first->manifest.fingerprint));
  closeKv(*kv);
}

ASYNC_TEST("a duplicate against a corrupt index row fails closed on the first read") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelFailureObservation failure = internalObservation();
  const SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5"})", "corrupt-index-first");
  auto storingFirst = persist(input, failure, storeDependencies(kv, "capture-corrupt-index"), client);
  const SentinelReplayPersistResult firstResult = co_await std::move(storingFirst);
  const auto* first = std::get_if<SentinelReplayPersistResultStored>(&firstResult);
  ASSERT(first != nullptr);

  const SentinelIncidentClassificationInput classification{
    .endpoint = input.endpoint,
    .method = input.method,
    .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                    .terminal_type = client.terminal_type},
  };
  auto fingerprinting = sentinelIncidentFingerprint(classification);
  const std::string indexFingerprint = co_await std::move(fingerprinting);
  rt::KvObject corrupt;
  corrupt.set("version", rt::KvValue(1));
  corrupt.set("incident_id", rt::KvValue("not-an-incident"));
  const rt::KvValue corruptValue(std::move(corrupt));
  co_await kv->set(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, indexFingerprint}, corruptValue);
  AcceptedSentinelReplayInput second = input;
  second.request_id = "corrupt-index-second";
  auto failing = persist(second, failure, storeDependencies(kv), client);
  const bool rejected = co_await rejectsWith(discard(std::move(failing)), "Sentinel incident index record is invalid");
  EXPECT(rejected);
  // The winning capture is untouched by the rejected duplicate.
  const rt::KvEntryMaybe<> winner = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, first->manifest.fingerprint, "capture-corrupt-index"});
  EXPECT(winner.value.truthy());
  closeKv(*kv);
}

ASYNC_TEST("a corrupt index row rejects the whole store attempt and cleans up its chunks") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5"})", "corrupt-index-store");
  const SentinelIncidentClassificationInput classification{
    .endpoint = input.endpoint,
    .method = input.method,
    .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                    .terminal_type = client.terminal_type},
  };
  auto fingerprinting = sentinelIncidentFingerprint(classification);
  const std::string indexFingerprint = co_await std::move(fingerprinting);
  rt::KvObject corrupt;
  corrupt.set("version", rt::KvValue(1));
  const rt::KvValue corruptValue(std::move(corrupt));
  co_await kv->set(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, indexFingerprint}, corruptValue);

  auto failing = persist(input, internalObservation(), storeDependencies(kv, "capture-corrupt-store"), client);
  const bool rejected = co_await rejectsWith(discard(std::move(failing)), "Sentinel incident index record is invalid");
  EXPECT(rejected);
  const rt::KvEntryMaybe<> chunk = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, "capture-corrupt-store", 0});
  EXPECT(chunk.value.isNull());
  const std::vector<rt::KvKey> manifestKeys = co_await keysUnder(*kv, SENTINEL_REPLAY_MANIFEST_PREFIX);
  EXPECT(manifestKeys.empty());
  const std::vector<rt::KvKey> dedupeEntries = co_await keysUnder(*kv, SENTINEL_REPLAY_DEDUPE_PREFIX);
  EXPECT(dedupeEntries.empty());
  closeKv(*kv);
}

ASYNC_TEST("an unusable dedupe record and an exhausted CAS both fail closed") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelFailureObservation failure = internalObservation();
  const SentinelClientFailureObservation client = clientObservationWithBody("upstream failed");
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5"})", "invalid-dedupe-request");
  auto probing = persist(input, failure, storeDependencies(kv, "capture-invalid-dedupe"), client);
  const SentinelReplayPersistResult probeResult = co_await std::move(probing);
  const auto* probe = std::get_if<SentinelReplayPersistResultStored>(&probeResult);
  ASSERT(probe != nullptr);
  rt::KvObject unusable;
  unusable.set("manifest_key", rt::KvValue("not-a-key"));
  const rt::KvValue unusableValue(std::move(unusable));
  co_await kv->set(rt::KvKey{SENTINEL_REPLAY_DEDUPE_PREFIX, probe->manifest.fingerprint}, unusableValue);
  AcceptedSentinelReplayInput second = input;
  second.request_id = "invalid-dedupe-second";
  auto failing = persist(second, failure, storeDependencies(kv, "capture-invalid-dedupe-2"), client);
  const bool rejected = co_await rejectsWith(discard(std::move(failing)), "Sentinel replay dedupe record is invalid");
  EXPECT(rejected);

  // A reservation commit that never lands leaves no chunks and no manifest
  // behind: accounting is already bootstrap-complete here, so the bounded
  // reservation retries are exhausted and admission reports storage_full
  // instead of staging unaccounted bytes, exactly as PR437's fallthrough does.
  const std::shared_ptr<rt::Kv> failingKv = std::make_shared<AlwaysConflictingKv>(*kv);
  auto refusing = persist(replayInput(R"({"model":"gpt-5-lost-cas"})", "lost-cas-request"), failure, storeDependencies(failingKv, "capture-lost-cas"), client);
  const SentinelReplayPersistResult refused = co_await std::move(refusing);
  const auto* incomplete = std::get_if<SentinelReplayPersistResultIncomplete>(&refused);
  EXPECT(incomplete != nullptr && incomplete->reason == "storage_full");
  const rt::KvEntryMaybe<> chunk = co_await kv->get(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, "capture-lost-cas", 0});
  EXPECT(chunk.value.isNull());
  const std::vector<rt::KvKey> manifestKeys = co_await keysUnder(*kv, SENTINEL_REPLAY_MANIFEST_PREFIX);
  ASSERT_EQ(manifestKeys.size(), std::size_t{1});
  EXPECT(manifestKeys[0].back() == rt::KvKeyPart("capture-invalid-dedupe"));
  closeKv(*kv);
}

ASYNC_TEST("store inputs that cannot represent a failure are rejected before any write") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelClientFailureObservation client = clientObservation();
  const AcceptedSentinelReplayInput input = replayInput(R"({"model":"gpt-5"})", "rejected-store-request");

  PersistDependencies shortKey = storeDependencies(kv);
  shortKey.keyBytes = rt::Bytes(31, 0);
  auto shortKeyTask = persist(input, internalObservation(), shortKey, client);
  const bool shortKeyRejected = co_await rejectsWith(discard(std::move(shortKeyTask)), "Sentinel replay key must be 32 bytes");
  EXPECT(shortKeyRejected);
  AcceptedSentinelReplayInput forwarded = input;
  forwarded.compatibility_headers = json::object{{"x-forwarded-for", CLIENT_ADDRESS}};
  auto headersTask = persist(forwarded, internalObservation(), storeDependencies(kv), client);
  const bool headersRejected = co_await rejectsWith(discard(std::move(headersTask)), "Sentinel replay compatibility headers contain a disallowed value");
  EXPECT(headersRejected);
  auto successTask = persist(input, successfulInternalObservation(), storeDependencies(kv), successfulClientObservation());
  const bool successRejected = co_await rejectsWith(discard(std::move(successTask)), "A successful request cannot be persisted as a sentinel replay");
  EXPECT(successRejected);
  PersistDependencies shortIv = storeDependencies(kv);
  shortIv.randomBytes = [](double) { return rt::Bytes(static_cast<std::size_t>(sentinel::replay_model::AES_GCM_IV_BYTES - 1), 0); };
  auto ivTask = persist(input, internalObservation(), shortIv, client);
  const bool ivRejected = co_await rejectsWith(discard(std::move(ivTask)), "Sentinel replay IV must be 12 bytes");
  EXPECT(ivRejected);
  // Nothing above may have written to storage.
  const std::vector<rt::KvKey> keys = co_await keysUnder(*kv, rt::KvKey{"uos_ai", "sentinel_replay"});
  EXPECT(keys.empty());
  closeKv(*kv);
}

ASYNC_TEST("a request id with no row reads as unknown and a stored row expires at read time") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelReplayCaptureStatusRow unknown = co_await captureStatus(*kv, std::string("never-stored-request"), NOW);
  EXPECT_EQ(unknown.status, std::string("unknown"));
  EXPECT_EQ(unknown.reason, std::optional<std::string>("no_capture_record"));
  EXPECT_EQ(unknown.captured_at_ms, NOW);
  EXPECT(!unknown.manifest_key.has_value());

  auto invalid = readSentinelReplayCaptureStatus(*kv, "bad request id");
  const bool invalidRejected = co_await rejectsWith(discard(std::move(invalid)), "Sentinel replay request ID is invalid");
  EXPECT(invalidRejected);

  const SentinelReplayCaptureStatusRow expiring{
    .version = 1,
    .request_id = "expiring-request",
    .status = "ready",
    .reason = std::nullopt,
    .captured_at_ms = NOW,
    .manifest_key = rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, std::string(64, 'c'), "capture-x"},
    .fingerprint = std::string(64, 'c'),
    .expires_at_ms = NOW + 1'000,
  };
  auto writing = writeSentinelReplayCaptureStatus(*kv, expiring);
  co_await std::move(writing);
  const SentinelReplayCaptureStatusRow live = co_await captureStatus(*kv, std::string("expiring-request"), NOW + 999);
  EXPECT_EQ(live.status, std::string("ready"));
  const SentinelReplayCaptureStatusRow expired = co_await captureStatus(*kv, std::string("expiring-request"), NOW + 1'000);
  EXPECT_EQ(expired.status, std::string("expired"));
  EXPECT_EQ(expired.fingerprint, std::optional<std::string>(std::string(64, 'c')));

  // A corrupt stored row fails closed instead of being reported as unknown.
  rt::KvObject corrupt;
  corrupt.set("version", rt::KvValue(1));
  corrupt.set("request_id", rt::KvValue("corrupt-request"));
  const rt::KvValue corruptValue(std::move(corrupt));
  co_await kv->set(sentinel::replay_model::requestStatusKey("corrupt-request"), corruptValue);
  auto corruptRead = readSentinelReplayCaptureStatus(*kv, "corrupt-request");
  const bool corruptRejected = co_await rejectsWith(discard(std::move(corruptRead)), "Sentinel replay capture status record is invalid");
  EXPECT(corruptRejected);
  closeKv(*kv);
}
