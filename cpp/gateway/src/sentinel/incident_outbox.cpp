#include "uos/sentinel/incident_outbox.hpp"

#include "uos/kv.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <utility>

namespace uos::sentinel::incident_outbox {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    const rt::Regex& INCIDENT_ID() {
      static const rt::Regex pattern("^provider-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", "");
      return pattern;
    }
    const rt::Regex& FULL_SHA() {
      static const rt::Regex pattern("^[0-9a-f]{40}$", "");
      return pattern;
    }
    // `/^[0-9a-f]{64}$/`
    const rt::Regex& SHA256_HEX() {
      static const rt::Regex pattern("^[0-9a-f]{64}$", "");
      return pattern;
    }

    // ---- JavaScript semantics over a stored (`unknown`) value ----------------------------------
    // `typeof value === "object" && value !== null` (utils.isRecord): arrays and every object kind.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }
    bool isNumber(const rt::KvValue& value, double number) { return value.isNumber() && value.asNumber() == number; }
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }
    // `typeof value === "string" && pattern.test(value)`
    bool matches(const rt::KvValue& value, const rt::Regex& pattern) { return value.isString() && pattern.test(value.asString()); }
    // `Array.isArray(value) && value.length === length`
    bool isArrayOfLength(const rt::KvValue& value, std::size_t length) { return value.isArray() && value.asArray().size() == length; }

    rt::KvValue textOrNull(const std::optional<std::string>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }
    rt::KvValue numberOrNull(const std::optional<double>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }

    // `[...key]` as a stored array.
    rt::KvValue keyValue(const rt::KvKey& key) {
      rt::KvArray parts;
      for (const rt::KvKeyPart& part : key) parts.push_back(std::visit([](const auto& value) { return rt::KvValue(value); }, part.value));
      return rt::KvValue(std::move(parts));
    }

    const EnvironmentReader& safeEnvironment() {
      static const EnvironmentReader reader{[](const std::string& name) -> std::optional<std::string> {
        try {
          const std::optional<std::string> value = rt::env::get(name);
          if (!value) return std::nullopt;
          std::string trimmed = rt::trim(*value);
          if (trimmed.empty()) return std::nullopt;
          return trimmed;
        } catch (...) {
          return std::nullopt;
        }
      }};
      return reader;
    }

    bool positiveInteger(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() > 0; }
    bool positiveInteger(double value) { return rt::Number::isSafeInteger(value) && value > 0; }

    std::string defaultRandomUuid() { return rt::crypto::randomUUID(); }

    rt::KvKey incidentEventKey(const std::string& incidentId) { return rt::KvKey{SENTINEL_INCIDENT_EVENT_PREFIX, incidentId}; }

    std::string newIncidentId(const SentinelIncidentDependencies& dependencies) {
      const std::string uuid = dependencies.randomUuid ? dependencies.randomUuid() : defaultRandomUuid();
      return "provider-" + rt::toLowerCase(uuid);
    }

    // ---- index ---------------------------------------------------------------------------------
    const rt::Regex& INDEX_CURSOR() {
      static const rt::Regex pattern("^[A-Za-z0-9_-]+={0,2}$", "");
      return pattern;
    }
    const rt::Regex& INDEX_REF() {
      static const rt::Regex pattern("^capture:[A-Za-z0-9_-]{1,128}$", "");
      return pattern;
    }
    constexpr std::string_view INDEX_MESSAGE = "Gateway observed a provider failure for this incident group.";
    constexpr std::array<std::string_view, 2> INDEX_ENDPOINTS{"/v1/responses", "/v1/chat/completions"};
    bool isIndexEndpoint(std::string_view value) { return std::find(INDEX_ENDPOINTS.begin(), INDEX_ENDPOINTS.end(), value) != INDEX_ENDPOINTS.end(); }

    const rt::Regex& INDEX_SLUG() {
      static const rt::Regex pattern("^[A-Za-z0-9_.:-]{1,128}$", "");
      return pattern;
    }
    const rt::Regex& INDEX_REASONING() {
      static const rt::Regex pattern("^[A-Za-z0-9_.:-]{1,32}$", "");
      return pattern;
    }

    // `boundedIndexText(value: unknown, pattern)`
    std::optional<std::string> boundedIndexText(const std::optional<std::string>& value, const rt::Regex& pattern) {
      return value && pattern.test(*value) ? value : std::nullopt;
    }
    bool boundedIndexText(const rt::KvValue& value, const rt::Regex& pattern) { return matches(value, pattern); }

    bool isSentinelIncidentIndexLatest(const rt::KvValue& value) {
      return isRecord(value) && positiveInteger(value["observed_at_ms"]) && (value["revision"].isNull() || matches(value["revision"], FULL_SHA())) &&
             (value["provider"].isNull() || boundedIndexText(value["provider"], INDEX_SLUG())) && isSentinelIncidentIndexEndpoint(value["route"]) &&
             (value["model"].isNull() || boundedIndexText(value["model"], INDEX_SLUG())) &&
             (value["reasoning"].isNull() || boundedIndexText(value["reasoning"], INDEX_REASONING())) &&
             (value["failure_kind"].isNull() || boundedIndexText(value["failure_kind"], INDEX_SLUG())) && isSentinelIncidentTerminalCategory(value["terminal"]) &&
             value["status"].isNumber() && rt::Number::isSafeInteger(value["status"].asNumber()) && value["status"].asNumber() >= 0 &&
             value["status"].asNumber() <= 599 && value["stream"].isBool();
    }

    rt::KvKey indexRowKey(const std::string& fingerprint) { return rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, fingerprint}; }

    rt::KvValue rowValue(const SentinelIncidentIndexRow& row) { return rt::KvValue(json::value_from(row)); }

    rt::KvValue observedIndexRowValue(const rt::KvValue& original, const SentinelIncidentIndexRow& next, double observedAtMs) {
      rt::KvObject stored = original.asObject();
      stored.set("first_seen_at_ms", rt::KvValue(next.first_seen_at_ms));
      stored.set("last_seen_at_ms", rt::KvValue(next.last_seen_at_ms));
      stored.set("count", rt::KvValue(next.count));
      stored.set("latest", original["last_seen_at_ms"].asNumber() > observedAtMs ? original["latest"] : rt::KvValue(json::value_from(*next.latest)));
      return rt::KvValue(std::move(stored));
    }

    rt::KvValue evidenceIndexRowValue(const rt::KvValue& original, const SentinelIncidentIndexRow& next) {
      rt::KvObject stored = original.asObject(), provenance = original["provenance"].asObject();
      provenance.set("captured_at_ms", rt::KvValue(next.provenance.captured_at_ms));
      stored.set("failing_revision", textOrNull(next.failing_revision));
      stored.set("provenance", rt::KvValue(std::move(provenance)));
      stored.set("evidence_ref", rt::KvValue(json::value_from(*next.evidence_ref)));
      stored.set("evidence_expires_at_ms", numberOrNull(next.evidence_expires_at_ms));
      return rt::KvValue(std::move(stored));
    }

    struct LatestInput {
      double observedAtMs;
      std::string gitSha;
      std::string endpoint;
      SentinelIncidentIndexObservation observation;
      std::optional<SentinelIncidentIndexClassification> classification;
    };

    std::optional<std::string> classificationField(const std::optional<SentinelIncidentIndexClassification>& classification,
                                                   std::optional<std::string> SentinelIncidentIndexClassification::*field) {
      return classification ? (*classification).*field : std::nullopt;
    }

    /**
     * The bounded latest classification of one observation. Every field is
     * sanitized against its own pattern; a value that does not fit is recorded as
     * absent rather than copied, and no body, header, request id or error text is
     * ever part of it.
     */
    SentinelIncidentIndexLatest latestObservation(const LatestInput& input) {
      const std::optional<SentinelIncidentIndexClassification>& classification = input.classification;
      return {
        .observed_at_ms = input.observedAtMs,
        .revision = FULL_SHA().test(input.gitSha) ? std::optional<std::string>(input.gitSha) : std::nullopt,
        .provider = boundedIndexText(classificationField(classification, &SentinelIncidentIndexClassification::provider), INDEX_SLUG()),
        .route = normalizeSentinelIncidentEndpoint(input.endpoint),
        .model = boundedIndexText(classificationField(classification, &SentinelIncidentIndexClassification::model), INDEX_SLUG()),
        .reasoning = boundedIndexText(classificationField(classification, &SentinelIncidentIndexClassification::reasoning), INDEX_REASONING()),
        .failure_kind = boundedIndexText(classificationField(classification, &SentinelIncidentIndexClassification::failure_kind), INDEX_SLUG()),
        .terminal = classifySentinelIncidentTerminal(input.observation),
        .status = input.observation.status,
        .stream = input.observation.stream,
      };
    }

    LatestInput latestInputOf(const SentinelIncidentIndexObservationInput& input) {
      return {.observedAtMs = input.observedAtMs, .gitSha = input.gitSha, .endpoint = input.endpoint, .observation = input.observation, .classification = input.classification};
    }

    SentinelIncidentIndexRow newIndexRow(const SentinelIncidentIndexObservationInput& input, const std::string& fingerprint, const std::string& incidentId) {
      return {
        .version = 1,
        .incident_id = incidentId,
        .fingerprint = fingerprint,
        .severity = "P2",
        .first_seen_at_ms = input.observedAtMs,
        .last_seen_at_ms = input.observedAtMs,
        .count = 1,
        .failing_revision = FULL_SHA().test(input.gitSha) ? std::optional<std::string>(input.gitSha) : std::nullopt,
        .error_type = classifySentinelIncidentTerminal(input.observation),
        .context = {.message = std::string(INDEX_MESSAGE), .location = nullptr, .sample = {}},
        .provenance = {.endpoint = normalizeSentinelIncidentEndpoint(input.endpoint), .captured_at_ms = input.observedAtMs, .captured_by = nullptr},
        .evidence_ref = std::nullopt,
        .evidence_expires_at_ms = std::nullopt,
        .latest = latestObservation(latestInputOf(input)),
      };
    }

    struct NextIndexRow {
      SentinelIncidentIndexRow next;
      std::optional<std::string> checkVersionstamp;
    };

    /**
     * The next index row for one observation: create a new group, or merge into the
     * existing one. First-seen identity stays; only the newest observation refreshes
     * the latest classification.
     */
    NextIndexRow nextSentinelIncidentIndexRow(const rt::KvEntryMaybe<>& entry, const SentinelIncidentIndexObservationInput& input, const std::string& fingerprint,
                                              const SentinelIncidentIndexLatest& latest, const std::function<std::string()>& randomUuid) {
      if (entry.value.isNull()) {
        const std::string incidentId = "provider-" + rt::toLowerCase(randomUuid());
        if (!isSentinelIncidentId(incidentId)) throw rt::JsError("Sentinel incident UUID is invalid");
        return {newIndexRow(input, fingerprint, incidentId), std::nullopt};
      }
      if (!isSentinelIncidentIndexRow(entry.value)) throw rt::JsError("Sentinel incident index record is invalid");
      SentinelIncidentIndexRow next = json::value_to<SentinelIncidentIndexRow>(rt::storedJsonView(entry.value));
      const SentinelIncidentIndexRow& stored = next;
      const double firstSeenAtMs = rt::Math::min(stored.first_seen_at_ms, input.observedAtMs);
      const double lastSeenAtMs = rt::Math::max(stored.last_seen_at_ms, input.observedAtMs);
      const bool keepLatest = stored.last_seen_at_ms > input.observedAtMs;
      next.first_seen_at_ms = firstSeenAtMs;
      next.count = stored.count + 1;
      if (!keepLatest) next.latest = latest;
      next.last_seen_at_ms = lastSeenAtMs;
      return {std::move(next), entry.versionstamp};
    }

    bool isSentinelIncidentIndexEvidenceBinding(const std::string& indexFingerprint, const SentinelIncidentIndexEvidenceBinding& input) {
      if (!SHA256_HEX().test(indexFingerprint)) return false;
      if (!SHA256_HEX().test(input.referenceFingerprint)) return false;
      if (!SHA256_HEX().test(input.digest)) return false;
      if (!positiveInteger(input.observedAtMs)) return false;
      if (!positiveInteger(input.capturedAtMs)) return false;
      if (!positiveInteger(input.expiresAtMs)) return false;
      // `Array.isArray(input.manifestKey)` and `manifestVersionstamp === null || typeof ... === "string"`
      // hold by type.
      if (input.manifestKey.size() != 7) return false;
      return true;
    }

    /** One bounded CAS attempt; returns null when the attempt lost the race. */
    rt::Task<std::optional<rt::KvEntry<SentinelIncidentIndexRow>>> commitSentinelIncidentIndexEvidence(rt::Kv& kv, rt::KvKey key, SentinelIncidentIndexEvidenceBinding input,
                                                                                                        SentinelIncidentCaptureReference reference, double remainingMs) {
      const rt::KvEntryMaybe<> entry = co_await kv.get(key);
      if (entry.value.isNull()) throw rt::JsError("Sentinel incident index record is missing");
      if (!isSentinelIncidentIndexRow(entry.value)) throw rt::JsError("Sentinel incident index record is invalid");
      const SentinelIncidentIndexEvidenceRef evidence{.ref = "capture:" + input.captureId, .digest = input.digest};
      if (!isSentinelIncidentIndexEvidenceRef(rt::KvValue(json::value_from(evidence)))) throw rt::JsError("Sentinel incident index evidence is invalid");
      SentinelIncidentIndexRow next = json::value_to<SentinelIncidentIndexRow>(rt::storedJsonView(entry.value));
      next.failing_revision = input.gitSha && FULL_SHA().test(*input.gitSha) ? input.gitSha : std::nullopt;
      next.provenance.captured_at_ms = input.capturedAtMs;
      next.evidence_ref = evidence;
      next.evidence_expires_at_ms = input.expiresAtMs;
      const rt::KvValue nextValue = evidenceIndexRowValue(entry.value, next);
      if (!isSentinelIncidentIndexRow(nextValue)) throw rt::JsError("Sentinel incident index record is invalid");
      std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck{key, entry.versionstamp}).set(key, nextValue);
      if (input.manifestVersionstamp) operation->check(rt::AtomicCheck{input.manifestKey, input.manifestVersionstamp});
      if (remainingMs > 0) {
        rt::KvSetOptions options;
        options.expireIn = remainingMs;
        operation->set(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, next.incident_id, input.referenceFingerprint}, toKvValue(reference), options);
      }
      const rt::KvCommitResult committed = co_await operation->commit();
      if (!committed.ok) co_return std::nullopt;
      rt::KvEntry<SentinelIncidentIndexRow> bound{key, std::move(next), committed.versionstamp};
      co_return bound;
    }
  }

  rt::KvValue toKvValue(const SentinelIncidentCaptureReference& reference) {
    rt::KvObject object;
    object.set("version", rt::KvValue(reference.version));
    object.set("manifest_key", keyValue(reference.manifest_key));
    return rt::KvValue(std::move(object));
  }

  rt::KvValue toKvValue(const SentinelIncidentFailureEvent& event) {
    rt::KvObject object;
    object.set("version", rt::KvValue(event.version));
    object.set("incident_id", rt::KvValue(event.incident_id));
    object.set("state", rt::KvValue(event.state));
    object.set("observed_at_ms", rt::KvValue(event.observed_at_ms));
    object.set("created_at_ms", rt::KvValue(event.created_at_ms));
    object.set("ready_at_ms", numberOrNull(event.ready_at_ms));
    object.set("capture_status", rt::KvValue(event.capture_status));
    object.set("capture_fingerprint", textOrNull(event.capture_fingerprint));
    object.set("manifest_key", event.manifest_key ? keyValue(*event.manifest_key) : rt::KvValue(nullptr));
    return rt::KvValue(std::move(object));
  }

  bool isSentinelIncidentId(const rt::KvValue& value) { return matches(value, INCIDENT_ID()); }

  bool isSentinelIncidentCaptureReference(const rt::KvValue& value) {
    return isRecord(value) && isNumber(value["version"], 1) && isArrayOfLength(value["manifest_key"], 7);
  }

  bool isSentinelIncidentFailureEvent(const rt::KvValue& value) {
    if (!isRecord(value) || !isNumber(value["version"], 1) || !isSentinelIncidentId(value["incident_id"]) || !positiveInteger(value["observed_at_ms"]) ||
        !positiveInteger(value["created_at_ms"]) || value["created_at_ms"].asNumber() < value["observed_at_ms"].asNumber() ||
        (!value["ready_at_ms"].isNull() && !positiveInteger(value["ready_at_ms"])) ||
        (!value["capture_fingerprint"].isNull() && !matches(value["capture_fingerprint"], SHA256_HEX())) ||
        (!value["manifest_key"].isNull() && !isArrayOfLength(value["manifest_key"], 7)))
      return false;
    if (isText(value["state"], "capturing")) {
      return value["ready_at_ms"].isNull() && isText(value["capture_status"], "pending") && value["capture_fingerprint"].isNull() && value["manifest_key"].isNull();
    }
    if (!isText(value["state"], "ready") || value["ready_at_ms"].isNull() || value["ready_at_ms"].asNumber() < value["created_at_ms"].asNumber()) return false;
    if (isText(value["capture_status"], "unavailable")) {
      return value["capture_fingerprint"].isNull() && value["manifest_key"].isNull();
    }
    return (isText(value["capture_status"], "stored") || isText(value["capture_status"], "duplicate")) && !value["capture_fingerprint"].isNull() &&
           !value["manifest_key"].isNull();
  }

  bool isSentinelProductionRuntime() { return isSentinelProductionRuntime(safeEnvironment()); }

  bool isSentinelProductionRuntime(const EnvironmentReader& environment) {
    return environment.get("DENO_DEPLOY_ORG_SLUG") == "ubiquity-dao" && environment.get("DENO_DEPLOY_APP_SLUG") == "ai-ubq-fi" &&
           environment.get("DENO_TIMELINE") == "production";
  }

  rt::Task<rt::KvEntry<SentinelIncidentFailureEvent>> createSentinelIncidentFailureEvent(rt::Kv& kv, double observedAtMs) {
    return createSentinelIncidentFailureEvent(kv, observedAtMs, SentinelIncidentDependencies());
  }

  rt::Task<rt::KvEntry<SentinelIncidentFailureEvent>> createSentinelIncidentFailureEvent(rt::Kv& kv, double observedAtMs, SentinelIncidentDependencies dependencies) {
    if (!positiveInteger(observedAtMs)) throw rt::JsError("Sentinel incident timestamp is invalid");
    const std::string incidentId = newIncidentId(dependencies);
    if (!isSentinelIncidentId(incidentId)) throw rt::JsError("Sentinel incident UUID is invalid");
    const rt::KvKey key = incidentEventKey(incidentId);
    SentinelIncidentFailureEvent value{
      .version = 1,
      .incident_id = incidentId,
      .state = "capturing",
      .observed_at_ms = observedAtMs,
      .created_at_ms = observedAtMs,
      .ready_at_ms = std::nullopt,
      .capture_status = "pending",
      .capture_fingerprint = std::nullopt,
      .manifest_key = std::nullopt,
    };
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    rt::KvSetOptions options;
    options.expireIn = SENTINEL_INCIDENT_TTL_MS;
    operation->check(rt::AtomicCheck{key, std::nullopt}).set(key, toKvValue(value), options);
    const rt::KvCommitResult committed = co_await operation->commit();
    if (!committed.ok) throw rt::JsError("Sentinel incident event identifier conflicted");
    rt::KvEntry<SentinelIncidentFailureEvent> created{key, std::move(value), committed.versionstamp};
    co_return created;
  }

  rt::Task<std::string> recordSentinelProviderDegradation(rt::Kv& kv, double observedAtMs) {
    return recordSentinelProviderDegradation(kv, observedAtMs, SentinelIncidentDependencies());
  }

  rt::Task<std::string> recordSentinelProviderDegradation(rt::Kv& kv, double observedAtMs, SentinelIncidentDependencies dependencies) {
    if (!positiveInteger(observedAtMs)) throw rt::JsError("Sentinel incident timestamp is invalid");
    const std::string incidentId = newIncidentId(dependencies);
    if (!isSentinelIncidentId(incidentId)) throw rt::JsError("Sentinel incident UUID is invalid");
    const rt::KvKey key = incidentEventKey(incidentId);
    const SentinelIncidentFailureEvent value{
      .version = 1,
      .incident_id = incidentId,
      .state = "ready",
      .observed_at_ms = observedAtMs,
      .created_at_ms = observedAtMs,
      .ready_at_ms = observedAtMs,
      .capture_status = "unavailable",
      .capture_fingerprint = std::nullopt,
      .manifest_key = std::nullopt,
    };
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    rt::KvSetOptions options;
    options.expireIn = SENTINEL_INCIDENT_TTL_MS;
    operation->check(rt::AtomicCheck{key, std::nullopt}).set(key, toKvValue(value), options);
    const rt::KvCommitResult committed = co_await operation->commit();
    if (!committed.ok) throw rt::JsError("Sentinel incident event identifier conflicted");
    co_return incidentId;
  }

  SentinelIncidentFailureEvent readySentinelIncidentFailureEvent(const rt::KvEntry<SentinelIncidentFailureEvent>& entry, double readyAtMs,
                                                                 const IncidentCaptureCompletion& capture) {
    if (!isSentinelIncidentFailureEvent(toKvValue(entry.value)) || entry.value.state != "capturing") {
      throw rt::JsError("Sentinel incident event is not awaiting capture");
    }
    if (!positiveInteger(readyAtMs) || readyAtMs < entry.value.created_at_ms) {
      throw rt::JsError("Sentinel incident ready timestamp is invalid");
    }
    SentinelIncidentFailureEvent value = entry.value;
    value.state = "ready";
    value.ready_at_ms = readyAtMs;
    if (std::holds_alternative<IncidentCaptureCompletionUnavailable>(capture)) {
      value.capture_status = "unavailable";
    } else {
      const IncidentCaptureCompletionCaptured& captured = std::get<IncidentCaptureCompletionCaptured>(capture);
      value.capture_status = captured.status;
      value.capture_fingerprint = captured.fingerprint;
      value.manifest_key = captured.manifestKey;
    }
    if (!isSentinelIncidentFailureEvent(toKvValue(value))) throw rt::JsError("Sentinel incident capture completion is invalid");
    return value;
  }

  rt::Task<bool> completeSentinelIncidentFailureEvent(rt::Kv& kv, rt::KvEntry<SentinelIncidentFailureEvent> entry, double readyAtMs, IncidentCaptureCompletion capture) {
    const SentinelIncidentFailureEvent value = readySentinelIncidentFailureEvent(entry, readyAtMs, capture);
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    rt::KvSetOptions options;
    options.expireIn = SENTINEL_INCIDENT_TTL_MS;
    operation->check(rt::AtomicCheck{entry.key, entry.versionstamp}).set(entry.key, toKvValue(value), options);
    const rt::KvCommitResult committed = co_await operation->commit();
    co_return committed.ok;
  }

  rt::Task<void> linkSentinelReplayToIncident(rt::Kv& kv, std::string incidentId, std::string fingerprint, rt::KvKey manifestKey) {
    if (!isSentinelIncidentId(incidentId) || !SHA256_HEX().test(fingerprint)) {
      throw rt::JsError("Sentinel incident capture reference is invalid");
    }
    const SentinelIncidentCaptureReference reference{.version = 1, .manifest_key = manifestKey};
    const rt::KvValue stored = toKvValue(reference);
    if (!isSentinelIncidentCaptureReference(stored)) throw rt::JsError("Sentinel replay manifest key is invalid");
    const rt::KvKey key{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, fingerprint};
    rt::KvSetOptions options;
    options.expireIn = SENTINEL_INCIDENT_TTL_MS;
    co_await kv.set(key, stored, options);
  }

  rt::Task<std::optional<rt::KvEntry<SentinelIncidentFailureEvent>>> createSentinelIncidentFailureEventFromEnvironment(rt::Kv& kv, double observedAtMs) {
    return createSentinelIncidentFailureEventFromEnvironment(kv, observedAtMs, safeEnvironment());
  }

  rt::Task<std::optional<rt::KvEntry<SentinelIncidentFailureEvent>>> createSentinelIncidentFailureEventFromEnvironment(rt::Kv& kv, double observedAtMs,
                                                                                                                     EnvironmentReader environment) {
    if (!isSentinelProductionRuntime(environment)) co_return std::nullopt;
    rt::Task<rt::KvEntry<SentinelIncidentFailureEvent>> creating = createSentinelIncidentFailureEvent(kv, observedAtMs);
    rt::KvEntry<SentinelIncidentFailureEvent> created = co_await std::move(creating);
    std::optional<rt::KvEntry<SentinelIncidentFailureEvent>> result(std::move(created));
    co_return result;
  }

  rt::Task<bool> recordSentinelProviderDegradationFromEnvironment(double observedAtMs) {
    return recordSentinelProviderDegradationFromEnvironment(observedAtMs, SentinelProviderDegradationDependencies());
  }

  rt::Task<bool> recordSentinelProviderDegradationFromEnvironment(double observedAtMs, SentinelProviderDegradationDependencies dependencies) {
    const EnvironmentReader environment = dependencies.environment ? *dependencies.environment : safeEnvironment();
    if (!isSentinelProductionRuntime(environment)) co_return false;
    std::shared_ptr<rt::Kv> kv = dependencies.kv;
    if (!kv) {
      rt::Promise<std::shared_ptr<rt::Kv>> opened = kv::getKv();
      kv = co_await opened.wait();
    }
    if (!kv) co_return false;
    SentinelIncidentDependencies forwarded;
    forwarded.randomUuid = dependencies.randomUuid;
    rt::Task<std::string> recording = recordSentinelProviderDegradation(*kv, observedAtMs, forwarded);
    co_await std::move(recording);
    co_return true;
  }

  // ---- index -----------------------------------------------------------------------------------

  bool isSentinelIncidentTerminalCategory(const rt::KvValue& value) {
    return value.isString() &&
           std::find(SENTINEL_INCIDENT_TERMINAL_CATEGORIES.begin(), SENTINEL_INCIDENT_TERMINAL_CATEGORIES.end(), value.asString()) != SENTINEL_INCIDENT_TERMINAL_CATEGORIES.end();
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexEvidenceRef& value) {
    out = Writer().required("ref", value.ref).orNull("digest", value.digest).done();
  }
  SentinelIncidentIndexEvidenceRef tag_invoke(const json::value_to_tag<SentinelIncidentIndexEvidenceRef>&, const json::value& value) {
    const Reader in(value, "SentinelIncidentIndexEvidenceRef");
    return {.ref = in.required<std::string>("ref"), .digest = in.orNull<std::string>("digest")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexLatest& value) {
    out = Writer()
            .required("observed_at_ms", value.observed_at_ms)
            .orNull("revision", value.revision)
            .orNull("provider", value.provider)
            .required("route", value.route)
            .orNull("model", value.model)
            .orNull("reasoning", value.reasoning)
            .orNull("failure_kind", value.failure_kind)
            .required("terminal", value.terminal)
            .required("status", value.status)
            .required("stream", value.stream)
            .done();
  }
  SentinelIncidentIndexLatest tag_invoke(const json::value_to_tag<SentinelIncidentIndexLatest>&, const json::value& value) {
    const Reader in(value, "SentinelIncidentIndexRow.latest");
    return {
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .revision = in.orNull<std::string>("revision"),
      .provider = in.orNull<std::string>("provider"),
      .route = in.required<std::string>("route"),
      .model = in.orNull<std::string>("model"),
      .reasoning = in.orNull<std::string>("reasoning"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .terminal = in.required<std::string>("terminal"),
      .status = in.required<double>("status"),
      .stream = in.required<bool>("stream"),
    };
  }

  namespace {
    // A member whose type is `null`.
    void requireNull(const Reader& in, std::string_view key) {
      const json::value member = in.required<json::value>(key);
      if (!member.is_null()) in.fail(key, &member, "null");
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexRow::Context& value) {
    out = Writer().required("message", value.message).orNull("location", std::optional<std::string>()).required("sample", value.sample).done();
  }
  SentinelIncidentIndexRow::Context tag_invoke(const json::value_to_tag<SentinelIncidentIndexRow::Context>&, const json::value& value) {
    const Reader in(value, "SentinelIncidentIndexRow.context");
    std::string message = in.required<std::string>("message");
    requireNull(in, "location");
    return {.message = std::move(message), .location = nullptr, .sample = in.required<std::vector<std::string>>("sample")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexRow::Provenance& value) {
    out = Writer().required("endpoint", value.endpoint).required("captured_at_ms", value.captured_at_ms).orNull("captured_by", std::optional<std::string>()).done();
  }
  SentinelIncidentIndexRow::Provenance tag_invoke(const json::value_to_tag<SentinelIncidentIndexRow::Provenance>&, const json::value& value) {
    const Reader in(value, "SentinelIncidentIndexRow.provenance");
    std::string endpoint = in.required<std::string>("endpoint");
    const double capturedAtMs = in.required<double>("captured_at_ms");
    requireNull(in, "captured_by");
    return {.endpoint = std::move(endpoint), .captured_at_ms = capturedAtMs, .captured_by = nullptr};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexRow& value) {
    out = Writer()
            .required("version", value.version)
            .required("incident_id", value.incident_id)
            .required("fingerprint", value.fingerprint)
            .required("severity", value.severity)
            .required("first_seen_at_ms", value.first_seen_at_ms)
            .required("last_seen_at_ms", value.last_seen_at_ms)
            .required("count", value.count)
            .orNull("failing_revision", value.failing_revision)
            .required("error_type", value.error_type)
            .required("context", value.context)
            .required("provenance", value.provenance)
            .orNull("evidence_ref", value.evidence_ref)
            .orNull("evidence_expires_at_ms", value.evidence_expires_at_ms)
            .orUndefined("latest", value.latest)
            .done();
  }
  SentinelIncidentIndexRow tag_invoke(const json::value_to_tag<SentinelIncidentIndexRow>&, const json::value& value) {
    const Reader in(value, "SentinelIncidentIndexRow");
    return {
      .version = in.required<double>("version"),
      .incident_id = in.required<std::string>("incident_id"),
      .fingerprint = in.required<std::string>("fingerprint"),
      .severity = in.required<std::string>("severity"),
      .first_seen_at_ms = in.required<double>("first_seen_at_ms"),
      .last_seen_at_ms = in.required<double>("last_seen_at_ms"),
      .count = in.required<double>("count"),
      .failing_revision = in.orNull<std::string>("failing_revision"),
      .error_type = in.required<std::string>("error_type"),
      .context = in.required<SentinelIncidentIndexRow::Context>("context"),
      .provenance = in.required<SentinelIncidentIndexRow::Provenance>("provenance"),
      .evidence_ref = in.orNull<SentinelIncidentIndexEvidenceRef>("evidence_ref"),
      .evidence_expires_at_ms = in.orNull<double>("evidence_expires_at_ms"),
      .latest = in.orUndefined<SentinelIncidentIndexLatest>("latest"),
    };
  }

  bool isSentinelIncidentIndexEvidenceRef(const rt::KvValue& value) {
    return isRecord(value) && matches(value["ref"], INDEX_REF()) && (value["digest"].isNull() || matches(value["digest"], SHA256_HEX()));
  }

  bool isSentinelIncidentIndexEndpoint(const rt::KvValue& value) {
    return value.isString() && (isIndexEndpoint(value.asString()) || value.asString() == "other");
  }

  bool isSentinelIncidentIndexRow(const rt::KvValue& value) {
    if (!isRecord(value)) return false;
    const rt::KvValue& context = value["context"];
    const rt::KvValue& provenance = value["provenance"];
    return isNumber(value["version"], 1) && isSentinelIncidentId(value["incident_id"]) && matches(value["fingerprint"], SHA256_HEX()) && isText(value["severity"], "P2") &&
           positiveInteger(value["first_seen_at_ms"]) && positiveInteger(value["last_seen_at_ms"]) &&
           value["last_seen_at_ms"].asNumber() >= value["first_seen_at_ms"].asNumber() && positiveInteger(value["count"]) &&
           (value["failing_revision"].isNull() || matches(value["failing_revision"], FULL_SHA())) && isSentinelIncidentTerminalCategory(value["error_type"]) &&
           isRecord(context) && isText(context["message"], INDEX_MESSAGE) && context["location"].isNull() && isArrayOfLength(context["sample"], 0) &&
           isRecord(provenance) && isSentinelIncidentIndexEndpoint(provenance["endpoint"]) && positiveInteger(provenance["captured_at_ms"]) &&
           provenance["captured_by"].isNull() && (value["evidence_ref"].isNull() || isSentinelIncidentIndexEvidenceRef(value["evidence_ref"])) &&
           (value["evidence_expires_at_ms"].isNull() || positiveInteger(value["evidence_expires_at_ms"])) &&
           value["evidence_ref"].isNull() == value["evidence_expires_at_ms"].isNull() &&
           (value["latest"].isUndefined() || isSentinelIncidentIndexLatest(value["latest"]));
  }

  std::string normalizeSentinelIncidentEndpoint(const std::string& endpoint) {
    const std::vector<std::string> parts = rt::split(endpoint, "?", 1);
    const std::string path = parts.empty() ? std::string() : parts[0];
    return isIndexEndpoint(path) ? path : "other";
  }

  std::string normalizeSentinelIncidentMethod(const std::string& method) { return method == "POST" ? "POST" : "other"; }

  SentinelIncidentTerminalCategory classifySentinelIncidentTerminal(const SentinelIncidentIndexObservation& observation) {
    if (observation.stream && !observation.framing_valid) return "stream_framing";
    const std::string_view terminal = observation.terminal_type ? std::string_view(*observation.terminal_type) : std::string_view();
    if (!observation.terminal_type) return "unknown";
    if (terminal == "http.error") return "http_error";
    if (terminal == "response.failed") return "response_failed";
    if (terminal == "response.incomplete") return "response_incomplete";
    if (terminal == "error") return "error";
    if (terminal == "deadline") return "deadline";
    if (terminal == "eof") return "eof";
    return "unknown";
  }

  std::string sentinelIncidentClassification(const SentinelIncidentClassificationInput& input) {
    if (!rt::Number::isSafeInteger(input.observation.status) || input.observation.status < 0 || input.observation.status > 599)
      throw rt::JsError("Sentinel incident classification status is invalid");
    json::object classification;
    classification["endpoint"] = normalizeSentinelIncidentEndpoint(input.endpoint);
    classification["method"] = normalizeSentinelIncidentMethod(input.method);
    classification["status"] = rt::makeNumber(input.observation.status);
    classification["stream"] = input.observation.stream;
    classification["completed"] = input.observation.completed;
    classification["framing_valid"] = input.observation.framing_valid;
    classification["terminal"] = classifySentinelIncidentTerminal(input.observation);
    return rt::jsonStringify(json::value(std::move(classification)));
  }

  rt::Task<std::string> sentinelIncidentFingerprint(const SentinelIncidentClassificationInput& input) {
    return utils::sha256Hex(sentinelIncidentClassification(input));
  }

  rt::Task<rt::KvEntry<SentinelIncidentIndexRow>> recordSentinelIncidentIndexObservation(rt::Kv& kv, SentinelIncidentIndexObservationInput input) {
    return recordSentinelIncidentIndexObservation(kv, std::move(input), SentinelIncidentDependencies());
  }

  rt::Task<rt::KvEntry<SentinelIncidentIndexRow>> recordSentinelIncidentIndexObservation(rt::Kv& kv, SentinelIncidentIndexObservationInput input,
                                                                                        SentinelIncidentDependencies dependencies) {
    if (!positiveInteger(input.observedAtMs)) throw rt::JsError("Sentinel incident timestamp is invalid");
    const SentinelIncidentClassificationInput classified{.endpoint = input.endpoint, .method = input.method, .observation = input.observation};
    rt::Task<std::string> hashing = sentinelIncidentFingerprint(classified);
    const std::string fingerprint = co_await std::move(hashing);
    const rt::KvKey key = indexRowKey(fingerprint);
    const SentinelIncidentIndexLatest latest = latestObservation(latestInputOf(input));
    const std::function<std::string()> randomUuid = dependencies.randomUuid ? dependencies.randomUuid : std::function<std::string()>(defaultRandomUuid);
    for (double attempt = 0; attempt < SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::KvEntryMaybe<> entry = co_await kv.get(key);
      NextIndexRow decided = nextSentinelIncidentIndexRow(entry, input, fingerprint, latest, randomUuid);
      const rt::KvValue nextValue = entry.value.isNull() ? rowValue(decided.next) : observedIndexRowValue(entry.value, decided.next, input.observedAtMs);
      if (!isSentinelIncidentIndexRow(nextValue)) throw rt::JsError("Sentinel incident index record is invalid");
      std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck{key, decided.checkVersionstamp}).set(key, nextValue);
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) {
        rt::KvEntry<SentinelIncidentIndexRow> recorded{key, std::move(decided.next), committed.versionstamp};
        co_return recorded;
      }
    }
    throw rt::JsError("Sentinel incident index update conflicted repeatedly");
  }

  rt::Task<rt::KvEntry<SentinelIncidentIndexRow>> bindSentinelIncidentIndexEvidence(rt::Kv& kv, std::string indexFingerprint, SentinelIncidentIndexEvidenceBinding input) {
    if (!isSentinelIncidentIndexEvidenceBinding(indexFingerprint, input)) {
      throw rt::JsError("Sentinel incident index evidence binding is invalid");
    }
    const rt::KvKey key = indexRowKey(indexFingerprint);
    const double remainingMs = input.expiresAtMs - input.observedAtMs;
    const SentinelIncidentCaptureReference reference{.version = 1, .manifest_key = input.manifestKey};
    if (!isSentinelIncidentCaptureReference(toKvValue(reference))) throw rt::JsError("Sentinel replay manifest key is invalid");
    for (double attempt = 0; attempt < SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS; attempt += 1) {
      rt::Task<std::optional<rt::KvEntry<SentinelIncidentIndexRow>>> attempting = commitSentinelIncidentIndexEvidence(kv, key, input, reference, remainingMs);
      std::optional<rt::KvEntry<SentinelIncidentIndexRow>> bound = co_await std::move(attempting);
      if (bound) co_return std::move(*bound);
    }
    throw rt::JsError("Sentinel incident index evidence binding conflicted repeatedly");
  }

  rt::Task<SentinelIncidentIndexPage> listSentinelIncidentIndexRows(rt::Kv& kv) { return listSentinelIncidentIndexRows(kv, SentinelIncidentIndexListOptions()); }

  rt::Task<SentinelIncidentIndexPage> listSentinelIncidentIndexRows(rt::Kv& kv, SentinelIncidentIndexListOptions options) {
    const double limit = options.limit.value_or(SENTINEL_INCIDENT_INDEX_DEFAULT_PAGE_LIMIT);
    if (!rt::Number::isSafeInteger(limit) || limit < 1 || limit > SENTINEL_INCIDENT_INDEX_MAX_PAGE_LIMIT ||
        (options.cursor && (rt::length(*options.cursor) < 1 || rt::length(*options.cursor) > 2'048 || !INDEX_CURSOR().test(*options.cursor))))
      throw rt::JsError("Sentinel incident index pagination is invalid");
    rt::KvListSelector selector;
    selector.prefix = SENTINEL_INCIDENT_INDEX_PREFIX;
    rt::KvListOptions listOptions;
    listOptions.limit = limit;
    listOptions.cursor = options.cursor;
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(selector, listOptions);
    SentinelIncidentIndexPage page;
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      // `entry.value.fingerprint` is read before the row is validated: a stored null or undefined
      // throws JavaScriptCore's TypeError there.
      if (entry->value.isNullish()) {
        const std::string kind = entry->value.isNull() ? "null" : "undefined";
        throw rt::TypeError(kind + " is not an object (evaluating 'entry.value.fingerprint')");
      }
      const rt::KvValue& fingerprint = entry->value["fingerprint"];
      bool keyMatches = entry->key.size() == SENTINEL_INCIDENT_INDEX_PREFIX.size() + 1;
      for (std::size_t index = 0; keyMatches && index < SENTINEL_INCIDENT_INDEX_PREFIX.size(); ++index)
        keyMatches = entry->key[index].isString() && entry->key[index].string() == SENTINEL_INCIDENT_INDEX_PREFIX[index].string();
      // `entry.key[last] !== entry.value.fingerprint`
      if (keyMatches) {
        const rt::KvKeyPart& last = entry->key.back();
        keyMatches = fingerprint.isString() ? last.isString() && last.string() == fingerprint.asString()
                     : fingerprint.isNumber() ? last.isNumber() && last.number() == fingerprint.asNumber()
                     : fingerprint.isBool()   ? last.isBool() && last.boolean() == fingerprint.asBool()
                     : fingerprint.isBigInt() ? last.isBigInt() && last.bigint() == fingerprint.asBigInt()
                                              : false;
      }
      if (!isSentinelIncidentIndexRow(entry->value) || !keyMatches) throw rt::JsError("Sentinel incident index record is invalid");
      SentinelIncidentIndexRow row = json::value_to<SentinelIncidentIndexRow>(rt::storedJsonView(entry->value));
      if (options.incidentId && row.incident_id != *options.incidentId) continue;
      page.rows.push_back(std::move(row));
      if (static_cast<double>(page.rows.size()) >= limit) {
        co_await iterator->return_();
        break;
      }
    }
    const std::string cursor = iterator->cursor();
    page.cursor = cursor.empty() ? std::nullopt : std::optional<std::string>(cursor);
    co_return page;
  }
}
