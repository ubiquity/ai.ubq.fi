#include "uos/codex/session.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace uos::codex::session {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    struct CodexSessionRecord {
      double v;
      std::string session_id_hash;
      double first_seen_at_ms;
      double last_seen_at_ms;
    };

    struct CodexKeyActivityRecord {
      double v;
      double last_request_at_ms;
      double last_new_session_at_ms;
    };

    // The records as the TS objects kv.set stores (members in declaration order).
    rt::KvValue storedValue(const CodexSessionRecord& record) {
      json::object out;
      out["v"] = rt::makeNumber(record.v);
      out["session_id_hash"] = record.session_id_hash;
      out["first_seen_at_ms"] = rt::makeNumber(record.first_seen_at_ms);
      out["last_seen_at_ms"] = rt::makeNumber(record.last_seen_at_ms);
      return rt::KvValue(out);
    }

    rt::KvValue storedValue(const CodexKeyActivityRecord& record) {
      json::object out;
      out["v"] = rt::makeNumber(record.v);
      out["last_request_at_ms"] = rt::makeNumber(record.last_request_at_ms);
      out["last_new_session_at_ms"] = rt::makeNumber(record.last_new_session_at_ms);
      return rt::KvValue(out);
    }

    CodexSessionObservation unknownObservation(bool metadataPresent, std::optional<std::string> sessionIdHash = std::nullopt, bool sessionIdPresent = false) {
      return {
        .metadata_present = metadataPresent,
        .session_id_present = sessionIdPresent,
        .session_id_hash = std::move(sessionIdHash),
        .state = "unknown",
        .continuation_age_ms = std::nullopt,
        .continuation_only_candidate = false,
      };
    }

    rt::KvKey keyActivityKey(const std::string& keyIdHash) { return rt::KvKey{CODEX_SESSION_ACTIVITY_PREFIX, "key", keyIdHash}; }
    rt::KvKey sessionActivityKey(const std::string& keyIdHash, const std::string& sessionIdHash) {
      return rt::KvKey{CODEX_SESSION_ACTIVITY_PREFIX, "session", keyIdHash, sessionIdHash};
    }

    // `typeof value === "object" && value !== null` (utils.isRecord) over a stored value.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    // `value.v !== 1`
    bool isVersionOne(const rt::KvValue& value) { return value.isNumber() && value.asNumber() == 1; }

    // `typeof value === "number" && Number.isSafeInteger(value) && !(value < 0)`
    bool isNonNegativeSafeInteger(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && !(value.asNumber() < 0); }

    std::optional<CodexSessionRecord> normalizeSessionRecord(const rt::KvValue& value) {
      if (!isRecord(value) || !isVersionOne(value["v"])) return std::nullopt;
      const rt::KvValue& hash = value["session_id_hash"];
      const std::string sessionIdHash = hash.isString() ? rt::trim(hash.asString()) : std::string();
      const rt::KvValue& firstSeen = value["first_seen_at_ms"];
      const rt::KvValue& lastSeen = value["last_seen_at_ms"];
      if (sessionIdHash.empty() || !isNonNegativeSafeInteger(firstSeen) || !isNonNegativeSafeInteger(lastSeen)) return std::nullopt;
      return CodexSessionRecord{
        .v = 1,
        .session_id_hash = sessionIdHash,
        .first_seen_at_ms = firstSeen.asNumber(),
        .last_seen_at_ms = lastSeen.asNumber(),
      };
    }

    std::optional<CodexKeyActivityRecord> normalizeKeyActivityRecord(const rt::KvValue& value) {
      if (!isRecord(value) || !isVersionOne(value["v"])) return std::nullopt;
      const rt::KvValue& lastRequest = value["last_request_at_ms"];
      const rt::KvValue& lastNewSession = value["last_new_session_at_ms"];
      if (!isNonNegativeSafeInteger(lastRequest) || !isNonNegativeSafeInteger(lastNewSession)) return std::nullopt;
      return CodexKeyActivityRecord{.v = 1, .last_request_at_ms = lastRequest.asNumber(), .last_new_session_at_ms = lastNewSession.asNumber()};
    }

    std::optional<std::string> sessionIdFromMetadata(const json::value* metadata) {
      if (!utils::isRecord(metadata) || metadata->is_array()) return std::nullopt;
      const std::optional<std::string> raw = utils::getString(rt::get(*metadata, "session_id"));
      const std::string sessionId = raw ? rt::trim(*raw) : std::string();
      // An empty or whitespace-only id is absent, not a session named "".
      if (sessionId.empty()) return std::nullopt;
      return sessionId;
    }

    CodexSessionObservation classifyWithState(bool metadataPresent, const std::string& sessionIdHash, const std::optional<CodexSessionRecord>& sessionRecord,
                                              const std::optional<CodexKeyActivityRecord>& keyActivity, double nowMs) {
      if (!sessionRecord || !keyActivity) {
        return {
          .metadata_present = metadataPresent,
          .session_id_present = true,
          .session_id_hash = sessionIdHash,
          .state = "unknown",
          .continuation_age_ms = std::nullopt,
          .continuation_only_candidate = false,
        };
      }
      const double continuationAgeMs = rt::Math::max(0.0, nowMs - keyActivity->last_new_session_at_ms);
      return {
        .metadata_present = metadataPresent,
        .session_id_present = true,
        .session_id_hash = sessionIdHash,
        .state = "continuation",
        .continuation_age_ms = continuationAgeMs,
        .continuation_only_candidate = continuationAgeMs >= CODEX_SESSION_CONTINUATION_THRESHOLD_MS,
      };
    }

    CodexSessionObservation newSessionObservation(bool metadataPresent, const std::string& sessionIdHash) {
      return {
        .metadata_present = metadataPresent,
        .session_id_present = true,
        .session_id_hash = sessionIdHash,
        .state = "new",
        .continuation_age_ms = std::nullopt,
        .continuation_only_candidate = false,
      };
    }

    struct CodexSessionClassification {
      bool metadataPresent;
      std::string sessionIdHash;
      bool malformedState;
      bool isNewSession;
      std::optional<CodexSessionRecord> existingSession;
      std::optional<CodexKeyActivityRecord> existingKeyActivity;
      std::optional<double> lastNewSessionAtMs;
      double nowMs;
    };

    /**
     * Precedence is unchanged from the inline classification: malformed prior state
     * fails closed to `unknown`, a first observation is `new`, recorded key activity
     * is a continuation, and anything else is unknown.
     */
    CodexSessionObservation classifyObservation(const CodexSessionClassification& classification) {
      const auto& [metadataPresent, sessionIdHash, malformedState, isNewSession, existingSession, existingKeyActivity, lastNewSessionAtMs, nowMs] = classification;
      if (malformedState) return unknownObservation(metadataPresent, sessionIdHash, true);
      if (isNewSession) return newSessionObservation(metadataPresent, sessionIdHash);
      if (existingKeyActivity && lastNewSessionAtMs) return classifyWithState(metadataPresent, sessionIdHash, existingSession, existingKeyActivity, nowMs);
      return unknownObservation(metadataPresent);
    }

    // The text the TS hashes: `${prefix}\u0000${value}`.
    std::string domainSeparated(std::string_view prefix, const std::string& value) {
      std::string text(prefix);
      text.push_back('\0');
      text += value;
      return text;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexSessionObservation& value) {
    out = Writer()
            .required("metadata_present", value.metadata_present)
            .required("session_id_present", value.session_id_present)
            .orNull("session_id_hash", value.session_id_hash)
            .required("state", value.state)
            .orNull("continuation_age_ms", value.continuation_age_ms)
            .required("continuation_only_candidate", value.continuation_only_candidate)
            .done();
  }

  CodexSessionObservation tag_invoke(const json::value_to_tag<CodexSessionObservation>&, const json::value& value) {
    const Reader in(value, "CodexSessionObservation");
    return {
      .metadata_present = in.required<bool>("metadata_present"),
      .session_id_present = in.required<bool>("session_id_present"),
      .session_id_hash = in.orNull<std::string>("session_id_hash"),
      .state = in.required<std::string>("state"),
      .continuation_age_ms = in.orNull<double>("continuation_age_ms"),
      .continuation_only_candidate = in.required<bool>("continuation_only_candidate"),
    };
  }

  rt::Task<CodexSessionObservation> observeCodexSession(std::optional<std::string> keyId, std::optional<json::value> clientMetadata) {
    return observeCodexSession(std::move(keyId), std::move(clientMetadata), rt::dateNow());
  }

  rt::Task<CodexSessionObservation> observeCodexSession(std::optional<std::string> keyId, std::optional<json::value> clientMetadata, double nowMs) {
    const bool metadataPresent = clientMetadata.has_value();
    const std::optional<std::string> sessionId = sessionIdFromMetadata(clientMetadata ? &*clientMetadata : nullptr);
    if (!sessionId) co_return unknownObservation(metadataPresent);

    try {
      rt::Task<std::string> hashingSession = utils::sha256Hex(domainSeparated("uos-codex-session-v1", *sessionId));
      const std::string sessionIdHash = co_await std::move(hashingSession);
      if (!keyId || rt::trim(*keyId).empty()) co_return unknownObservation(metadataPresent, sessionIdHash, true);
      rt::Promise<std::shared_ptr<rt::Kv>> pending = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
      if (!kv) co_return unknownObservation(metadataPresent, sessionIdHash, true);

      rt::Task<std::string> hashingKey = utils::sha256Hex(domainSeparated("uos-codex-api-key-v1", *keyId));
      const std::string keyIdHash = co_await std::move(hashingKey);
      const rt::KvKey key = keyActivityKey(keyIdHash);
      const rt::KvKey sessionKey = sessionActivityKey(keyIdHash, sessionIdHash);
      std::vector<rt::KvKey> keys;
      keys.push_back(key);
      keys.push_back(sessionKey);
      rt::KvGetOptions strong;
      strong.consistency = rt::KvConsistencyLevel::strong;
      rt::Task<std::vector<rt::KvEntryMaybe<>>> reading = kv->getMany(std::move(keys), strong);
      const std::vector<rt::KvEntryMaybe<>> entries = co_await std::move(reading);
      const rt::KvEntryMaybe<>& keyEntry = entries.at(0);
      const rt::KvEntryMaybe<>& sessionEntry = entries.at(1);
      const std::optional<CodexKeyActivityRecord> existingKeyActivity = normalizeKeyActivityRecord(keyEntry.value);
      const std::optional<CodexSessionRecord> existingSession = normalizeSessionRecord(sessionEntry.value);
      const bool malformedState = (!keyEntry.value.isNull() && !existingKeyActivity) || (!sessionEntry.value.isNull() && !existingSession);
      const bool isNewSession = !malformedState && !existingSession;
      const std::optional<double> lastNewSessionAtMs =
        isNewSession ? std::optional<double>(nowMs) : (existingKeyActivity ? std::optional<double>(existingKeyActivity->last_new_session_at_ms) : std::nullopt);
      const CodexSessionObservation observation = classifyObservation(CodexSessionClassification{
        .metadataPresent = metadataPresent,
        .sessionIdHash = sessionIdHash,
        .malformedState = malformedState,
        .isNewSession = isNewSession,
        .existingSession = existingSession,
        .existingKeyActivity = existingKeyActivity,
        .lastNewSessionAtMs = lastNewSessionAtMs,
        .nowMs = nowMs,
      });

      const CodexSessionRecord nextSession{
        .v = 1,
        .session_id_hash = sessionIdHash,
        .first_seen_at_ms = existingSession ? existingSession->first_seen_at_ms : nowMs,
        .last_seen_at_ms = nowMs,
      };
      const CodexKeyActivityRecord nextKeyActivity{
        .v = 1,
        .last_request_at_ms = nowMs,
        .last_new_session_at_ms = isNewSession || malformedState ? nowMs : (existingKeyActivity ? existingKeyActivity->last_new_session_at_ms : nowMs),
      };
      rt::KvSetOptions expiring;
      expiring.expireIn = CODEX_SESSION_ACTIVITY_TTL_MS;
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(keyEntry).check(sessionEntry).set(key, storedValue(nextKeyActivity), expiring).set(sessionKey, storedValue(nextSession), expiring);
      rt::Task<rt::KvCommitResult> committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (!committed.ok) {
        // A concurrent request can win the first observation. Returning unknown
        // avoids treating an uncommitted observation as routing evidence.
        co_return unknownObservation(metadataPresent, sessionIdHash, true);
      }
      co_return observation;
    } catch (...) {
    }
    // Session telemetry is best effort and must never reject an inference.
    co_return unknownObservation(metadataPresent);
  }
}
