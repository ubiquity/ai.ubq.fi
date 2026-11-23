#pragma once
// Port of src/sentinel/incident-outbox.ts.
//
// Values read back from KV are `unknown` in the TS: the validators take an rt::KvValue (a stored
// value may hold key parts JSON cannot), which a json::value converts to implicitly. The records
// are structs; a stored record's undeclared members do not survive a C++ rewrite (the TS spread
// keeps them).
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::sentinel::incident_outbox {
  inline const rt::KvKey SENTINEL_INCIDENT_CAPTURE_REF_PREFIX{"uos_ai", "sentinel_incident", "v1", "capture_ref"};
  inline const rt::KvKey SENTINEL_INCIDENT_EVENT_PREFIX{"uos_ai", "sentinel_incident", "v1", "event"};
  inline constexpr double SENTINEL_INCIDENT_TTL_MS = 48 * 60 * 60 * 1'000;

  struct SentinelIncidentCaptureReference {
    double version = 1;
    rt::KvKey manifest_key;
    friend bool operator==(const SentinelIncidentCaptureReference&, const SentinelIncidentCaptureReference&) = default;
  };

  struct SentinelIncidentFailureEvent {
    double version = 1;
    std::string incident_id;
    std::string state;  // "capturing" | "ready"
    double observed_at_ms;
    double created_at_ms;
    std::optional<double> ready_at_ms;
    std::string capture_status;  // "pending" | "stored" | "duplicate" | "unavailable"
    std::optional<std::string> capture_fingerprint;
    std::optional<rt::KvKey> manifest_key;
    friend bool operator==(const SentinelIncidentFailureEvent&, const SentinelIncidentFailureEvent&) = default;
  };

  // The stored values (members in declaration order, which is the order the TS writes them).
  rt::KvValue toKvValue(const SentinelIncidentCaptureReference& reference);
  rt::KvValue toKvValue(const SentinelIncidentFailureEvent& event);

  // `Readonly<{ get(name: string): string | undefined }>`
  struct EnvironmentReader {
    std::function<std::optional<std::string>(const std::string& name)> get;
  };

  // Members are `?:`: an empty function is undefined.
  struct SentinelIncidentDependencies {
    std::function<double()> now{};
    std::function<std::string()> randomUuid{};
    rt::Fetch fetcher{};
    std::function<rt::AbortSignal(double milliseconds)> createTimeoutSignal{};
    std::function<std::string()> randomAckNonce{};
  };

  // `value: unknown`
  bool isSentinelIncidentId(const rt::KvValue& value);
  bool isSentinelIncidentCaptureReference(const rt::KvValue& value);
  bool isSentinelIncidentFailureEvent(const rt::KvValue& value);

  // `environment` defaults to the trimmed process environment.
  bool isSentinelProductionRuntime();
  bool isSentinelProductionRuntime(const EnvironmentReader& environment);

  // `dependencies: Pick<SentinelIncidentDependencies, "randomUuid">`; the overload without it is `{}`.
  rt::Task<rt::KvEntry<SentinelIncidentFailureEvent>> createSentinelIncidentFailureEvent(rt::Kv& kv, double observedAtMs);
  rt::Task<rt::KvEntry<SentinelIncidentFailureEvent>> createSentinelIncidentFailureEvent(rt::Kv& kv, double observedAtMs, SentinelIncidentDependencies dependencies);

  rt::Task<std::string> recordSentinelProviderDegradation(rt::Kv& kv, double observedAtMs);
  rt::Task<std::string> recordSentinelProviderDegradation(rt::Kv& kv, double observedAtMs, SentinelIncidentDependencies dependencies);

  // `IncidentCaptureCompletion`: `{ status: "stored" | "duplicate"; fingerprint; manifestKey }` or `{ status: "unavailable" }`.
  struct IncidentCaptureCompletionCaptured {
    std::string status;  // "stored" | "duplicate"
    std::string fingerprint;
    rt::KvKey manifestKey;
  };
  struct IncidentCaptureCompletionUnavailable {
    static constexpr std::string_view status = "unavailable";
  };
  using IncidentCaptureCompletion = std::variant<IncidentCaptureCompletionCaptured, IncidentCaptureCompletionUnavailable>;

  SentinelIncidentFailureEvent readySentinelIncidentFailureEvent(const rt::KvEntry<SentinelIncidentFailureEvent>& entry, double readyAtMs,
                                                                 const IncidentCaptureCompletion& capture);

  rt::Task<bool> completeSentinelIncidentFailureEvent(rt::Kv& kv, rt::KvEntry<SentinelIncidentFailureEvent> entry, double readyAtMs, IncidentCaptureCompletion capture);

  rt::Task<void> linkSentinelReplayToIncident(rt::Kv& kv, std::string incidentId, std::string fingerprint, rt::KvKey manifestKey);

  // `environment` defaults to the trimmed process environment; std::nullopt is null.
  rt::Task<std::optional<rt::KvEntry<SentinelIncidentFailureEvent>>> createSentinelIncidentFailureEventFromEnvironment(rt::Kv& kv, double observedAtMs);
  rt::Task<std::optional<rt::KvEntry<SentinelIncidentFailureEvent>>> createSentinelIncidentFailureEventFromEnvironment(rt::Kv& kv, double observedAtMs,
                                                                                                                     EnvironmentReader environment);

  // `Readonly<{ environment?; kv?; randomUuid? }>`: an empty member is undefined.
  struct SentinelProviderDegradationDependencies {
    std::optional<EnvironmentReader> environment{};
    std::shared_ptr<rt::Kv> kv{};
    std::function<std::string()> randomUuid{};
  };
  rt::Task<bool> recordSentinelProviderDegradationFromEnvironment(double observedAtMs);
  rt::Task<bool> recordSentinelProviderDegradationFromEnvironment(double observedAtMs, SentinelProviderDegradationDependencies dependencies);

  // ---------------------------------------------------------------------------
  // Durable passive incident index (m06). Index rows have NO TTL so durable
  // discovery survives the 48-hour capture lifetime; they are never derived from
  // the retired transient event/control/ack records. The row key is a stable
  // failure group: SHA-256 over a canonical, explicitly safe classification
  // (recognized endpoint/method, numeric status, boolean stream/completion/
  // framing, fixed terminal category). No raw body, request ID, compatibility
  // header, query string, user/model input or error text is hashed or copied to
  // public records.
  // ---------------------------------------------------------------------------

  inline const rt::KvKey SENTINEL_INCIDENT_INDEX_PREFIX{"uos_ai", "sentinel_incident", "v1", "index"};
  // Frozen gateway query contract: server page limit is any integer 1..100.
  inline constexpr double SENTINEL_INCIDENT_INDEX_MAX_PAGE_LIMIT = 100;
  inline constexpr double SENTINEL_INCIDENT_INDEX_DEFAULT_PAGE_LIMIT = 20;
  inline constexpr double SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS = 8;

  inline constexpr std::array<std::string_view, 8> SENTINEL_INCIDENT_TERMINAL_CATEGORIES{
    "stream_framing", "http_error", "response_failed", "response_incomplete", "error", "deadline", "eof", "unknown",
  };
  using SentinelIncidentTerminalCategory = std::string;

  bool isSentinelIncidentTerminalCategory(const rt::KvValue& value);

  struct SentinelIncidentIndexObservation {
    double status;
    bool stream;
    bool completed;
    bool framing_valid;
    std::optional<std::string> terminal_type;
  };

  struct SentinelIncidentIndexEvidenceRef {
    std::string ref;
    std::optional<std::string> digest;
    friend bool operator==(const SentinelIncidentIndexEvidenceRef&, const SentinelIncidentIndexEvidenceRef&) = default;
  };

  /** Bounded classification of the most recent observation in one incident group. */
  struct SentinelIncidentIndexLatest {
    double observed_at_ms;
    std::optional<std::string> revision;
    std::optional<std::string> provider;
    std::string route;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    std::optional<std::string> failure_kind;
    SentinelIncidentTerminalCategory terminal;
    double status;
    bool stream;
    friend bool operator==(const SentinelIncidentIndexLatest&, const SentinelIncidentIndexLatest&) = default;
  };

  struct SentinelIncidentIndexRow {
    // `Readonly<{ message: string; location: null; sample: readonly string[] }>`
    struct Context {
      std::string message;
      std::nullptr_t location = nullptr;
      std::vector<std::string> sample;
      friend bool operator==(const Context&, const Context&) = default;
    };
    // `Readonly<{ endpoint: string; captured_at_ms: number; captured_by: null }>`
    struct Provenance {
      std::string endpoint;
      double captured_at_ms;
      std::nullptr_t captured_by = nullptr;
      friend bool operator==(const Provenance&, const Provenance&) = default;
    };
    double version = 1;
    std::string incident_id;
    std::string fingerprint;
    std::string severity;  // "P2"
    double first_seen_at_ms;
    double last_seen_at_ms;
    double count;
    std::optional<std::string> failing_revision;
    std::string error_type;
    Context context;
    Provenance provenance;
    std::optional<SentinelIncidentIndexEvidenceRef> evidence_ref;
    std::optional<double> evidence_expires_at_ms;
    std::optional<SentinelIncidentIndexLatest> latest{};
    friend bool operator==(const SentinelIncidentIndexRow&, const SentinelIncidentIndexRow&) = default;
  };

  // The stored rows: value_from writes the members in declaration order; value_to is strict.
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexEvidenceRef& value);
  SentinelIncidentIndexEvidenceRef tag_invoke(const json::value_to_tag<SentinelIncidentIndexEvidenceRef>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexLatest& value);
  SentinelIncidentIndexLatest tag_invoke(const json::value_to_tag<SentinelIncidentIndexLatest>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexRow::Context& value);
  SentinelIncidentIndexRow::Context tag_invoke(const json::value_to_tag<SentinelIncidentIndexRow::Context>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexRow::Provenance& value);
  SentinelIncidentIndexRow::Provenance tag_invoke(const json::value_to_tag<SentinelIncidentIndexRow::Provenance>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelIncidentIndexRow& value);
  SentinelIncidentIndexRow tag_invoke(const json::value_to_tag<SentinelIncidentIndexRow>&, const json::value& value);

  bool isSentinelIncidentIndexEvidenceRef(const rt::KvValue& value);

  bool isSentinelIncidentIndexEndpoint(const rt::KvValue& value);

  bool isSentinelIncidentIndexRow(const rt::KvValue& value);

  /** Normalize a captured endpoint: recognized path without query, else a fixed safe label. */
  std::string normalizeSentinelIncidentEndpoint(const std::string& endpoint);

  std::string normalizeSentinelIncidentMethod(const std::string& method);

  /** Fixed safe terminal/failure category; truly unrecognized values are "unknown". */
  SentinelIncidentTerminalCategory classifySentinelIncidentTerminal(const SentinelIncidentIndexObservation& observation);

  // `input: { endpoint: string; method: string; observation: SentinelIncidentIndexObservation }`
  struct SentinelIncidentClassificationInput {
    std::string endpoint;
    std::string method;
    SentinelIncidentIndexObservation observation;
  };

  std::string sentinelIncidentClassification(const SentinelIncidentClassificationInput& input);

  /** Stable group fingerprint: SHA-256 of the canonical safe classification. */
  // Not a coroutine: an invalid status throws when it is called, as the TS throws.
  rt::Task<std::string> sentinelIncidentFingerprint(const SentinelIncidentClassificationInput& input);

  /** Bounded latest-observation classification accepted by the index writer. */
  struct SentinelIncidentIndexClassification {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    std::optional<std::string> failure_kind;
  };

  // The `input` of recordSentinelIncidentIndexObservation.
  struct SentinelIncidentIndexObservationInput {
    std::string endpoint;
    std::string method;
    std::string gitSha;
    double observedAtMs;
    SentinelIncidentIndexObservation observation;
    std::optional<SentinelIncidentIndexClassification> classification{};
  };

  /**
   * Record one actual observation for the stable failure group. Bounded
   * optimistic CAS (8 attempts) guarantees a single group row under concurrent
   * observations; each call counts exactly once and first/last times stay
   * monotonic. Later observations never retag evidence or revision metadata.
   */
  rt::Task<rt::KvEntry<SentinelIncidentIndexRow>> recordSentinelIncidentIndexObservation(rt::Kv& kv, SentinelIncidentIndexObservationInput input);
  rt::Task<rt::KvEntry<SentinelIncidentIndexRow>> recordSentinelIncidentIndexObservation(rt::Kv& kv, SentinelIncidentIndexObservationInput input,
                                                                                        SentinelIncidentDependencies dependencies);

  /** Input accepted by {@link bindSentinelIncidentIndexEvidence}. */
  struct SentinelIncidentIndexEvidenceBinding {
    double observedAtMs;
    std::string captureId;
    std::optional<std::string> gitSha;
    std::string referenceFingerprint;
    rt::KvKey manifestKey;
    std::optional<std::string> manifestVersionstamp;
    double capturedAtMs;
    std::string digest;
    double expiresAtMs;
  };

  /**
   * Bind a real capture to the stable index row. `evidence` is the SHA-256 of
   * the decoded concatenated ciphertext bytes (never the manifest HMAC
   * fingerprint or JSON hash); `ref` is opaque (`capture:<capture_id>`).
   * The binding is exact: `gitSha` is the winning capture's original plaintext
   * revision (valid full SHA or null) and `capturedAtMs` is that manifest's
   * captured_at_ms — never the index's first/last observation or the current
   * duplicate request. The atomic CAS-checks the winning manifest versionstamp
   * so an observed binding cannot silently attach to a replaced/deleted
   * manifest; ref/evidence/index are one transaction, no non-atomic pairing.
   */
  rt::Task<rt::KvEntry<SentinelIncidentIndexRow>> bindSentinelIncidentIndexEvidence(rt::Kv& kv, std::string indexFingerprint, SentinelIncidentIndexEvidenceBinding input);

  // `options: Readonly<{ incidentId?: string; cursor?: string; limit?: number }>`; the overload without it is `{}`.
  struct SentinelIncidentIndexListOptions {
    std::optional<std::string> incidentId{};
    std::optional<std::string> cursor{};
    std::optional<double> limit{};
  };
  // `Readonly<{ rows: SentinelIncidentIndexRow[]; cursor: string | null }>`
  struct SentinelIncidentIndexPage {
    std::vector<SentinelIncidentIndexRow> rows;
    std::optional<std::string> cursor;
  };

  /**
   * Read-only bounded pagination over the durable index. Every success is a
   * complete page: `cursor` is null only when the scan is exhausted, never when
   * a record is malformed or storage fails (callers fail closed). An incident
   * filter still obeys bounded pagination: a matching row after this page's
   * bound is reached by following the non-null cursor, never silently dropped.
   */
  rt::Task<SentinelIncidentIndexPage> listSentinelIncidentIndexRows(rt::Kv& kv);
  rt::Task<SentinelIncidentIndexPage> listSentinelIncidentIndexRows(rt::Kv& kv, SentinelIncidentIndexListOptions options);
}
