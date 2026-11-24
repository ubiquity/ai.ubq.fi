#pragma once
// Port of src/sentinel/replay-observation.ts.
// Sentinel replay capture and client observation, split out of src/sentinel_replay_capture.ts.
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace uos::sentinel::replay_observation {
  // `value: unknown`
  bool isSentinelReplayRequestId(const rt::KvValue& value);

  bool isSentinelReplayCaptureStatusRow(const rt::KvValue& value);

  struct SentinelReplayPersistResultStored {
    static constexpr std::string_view status = "stored";
    replay_model::SentinelReplayManifest manifest;
    std::optional<rt::KvKey> manifest_key{};
  };
  struct SentinelReplayPersistResultDuplicate {
    static constexpr std::string_view status = "duplicate";
    std::string fingerprint;
    std::optional<rt::KvKey> manifest_key{};
  };
  struct SentinelReplayPersistResultDisabled {
    static constexpr std::string_view status = "disabled";
    std::string reason;  // "key_missing" | "kv_unavailable"
  };
  struct SentinelReplayPersistResultIncomplete {
    static constexpr std::string_view status = "incomplete";
    std::string reason;  // SENTINEL_REPLAY_STORAGE_FULL_REASON | SENTINEL_REPLAY_ACCOUNTING_REASON
  };
  using SentinelReplayPersistResult = std::variant<SentinelReplayPersistResultStored, SentinelReplayPersistResultDuplicate,
                                                   SentinelReplayPersistResultDisabled, SentinelReplayPersistResultIncomplete>;

  // Optional members (`?:`) are empty functions / std::nullopt when undefined.
  struct PersistDependencies {
    std::shared_ptr<rt::Kv> kv;
    rt::Bytes keyBytes;
    std::function<double()> now{};
    /** Fresh wall-clock seam for reservation lease checks. */
    std::function<double()> currentNow{};
    std::function<std::string()> randomUuid{};
    std::function<rt::Bytes(double length)> randomBytes{};
    std::optional<rt::KvEntry<incident_outbox::SentinelIncidentFailureEvent>> incidentEvent{};
    /** Test seam: an injectable small retention budget; production uses the fixed 1 GiB default. */
    std::optional<double> budgetBytes{};
  };

  replay_model::SentinelCompatibilityHeaders normalizeSentinelCompatibilityHeaders(const rt::Headers& headers);

  // A null pointer is null.
  std::shared_ptr<replay_model::SentinelReplayCaptureCandidate> captureAcceptedSentinelReplayInput(const rt::Request& req, const std::string& requestId);

  /**
   * Materialize the accepted body. A declined body is never silent: the
   * candidate keeps a bounded omission reason so a caller can publish a capture
   * status instead of an empty replay history.
   */
  std::optional<replay_model::AcceptedSentinelReplayInput> materializeSentinelReplayInput(const std::shared_ptr<replay_model::SentinelReplayCaptureCandidate>& candidate);

  // A null pointer / std::nullopt is null or undefined.
  void zeroSentinelReplayInput(const replay_model::AcceptedSentinelReplayInput* input);
  void zeroSentinelReplayInput(const std::optional<replay_model::AcceptedSentinelReplayInput>& input);

  /**
   * Narrow application-terminal snapshot helper: copy the accepted body and
   * seal the request-owned upstream recorder together, before the original body
   * is zeroed or any await. HMAC, encryption and the plaintext metadata all use
   * this same immutable trace; the recorder is disposed so zeroing the original
   * never erases the snapshot and no later read retains additional bytes.
   */
  replay_model::AcceptedSentinelReplayInput snapshotSentinelReplayInput(const replay_model::AcceptedSentinelReplayInput& input);

  /** Discard a recorder that never reached a snapshot (zero retained bytes). */
  // A null pointer / std::nullopt is null or undefined.
  void disposeSentinelUpstreamRecorder(const replay_model::AcceptedSentinelReplayInput* input);
  void disposeSentinelUpstreamRecorder(const std::optional<replay_model::AcceptedSentinelReplayInput>& input);

  // A null pointer is null or undefined.
  void discardSentinelReplayCaptureCandidate(const std::shared_ptr<replay_model::SentinelReplayCaptureCandidate>& candidate);

  std::string sentinelFailureSignature(const replay_model::SentinelClientFailureObservation& observation);

  replay_model::SentinelClientBodyObservation inspectSentinelBufferedResponse(double status, const std::string& contentType, std::span<const std::uint8_t> bytes);

  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<replay_model::SentinelClientBodyObservation>> inspectSentinelBufferedResponseBody(rt::Response response);

  // `Readonly<{ push(bytes); finish(termination?: "eof" | "read_error") }>`: copies share the
  // inspector's state. `finish(std::nullopt)` is `finish()` ("eof").
  struct SentinelSseInspector {
    std::function<void(std::span<const std::uint8_t> bytes)> push;
    std::function<replay_model::SentinelClientBodyObservation(const std::optional<std::string>& termination)> finish;
  };

  SentinelSseInspector createSentinelSseInspector();

  replay_model::SentinelClientBodyObservation inspectSentinelSse(std::span<const std::uint8_t> bytes);

  // The overload without `body` is undefined; a null pointer is null.
  replay_model::SentinelClientFailureObservation resolveSentinelClientFailureObservation(const replay_model::SentinelFailureObservation& internal);
  replay_model::SentinelClientFailureObservation resolveSentinelClientFailureObservation(const replay_model::SentinelFailureObservation& internal,
                                                                                        const replay_model::SentinelClientBodyObservation* body);
  replay_model::SentinelClientFailureObservation resolveSentinelClientFailureObservation(const replay_model::SentinelFailureObservation& internal,
                                                                                        const std::optional<replay_model::SentinelClientBodyObservation>& body);

  // The overload without `clientObservation` is undefined.
  bool shouldPersistSentinelReplay(const replay_model::SentinelFailureObservation& observation);
  bool shouldPersistSentinelReplay(const replay_model::SentinelFailureObservation& observation,
                                   const replay_model::SentinelClientFailureObservation& clientObservation);

  /** Encrypted diagnostic retention includes terminal failures without causal replay coverage. */
  bool shouldPersistFailedRequestPayload(const replay_model::SentinelFailureObservation& observation,
                                         const replay_model::SentinelClientFailureObservation& clientObservation,
                                         bool deliveryInterrupted = false);

  bool shouldSignalSentinelIncident(const replay_model::SentinelFailureObservation& observation,
                                    const replay_model::SentinelClientFailureObservation& clientObservation);

  /** A client-visible validation discriminator: bounded, printable, non-secret. */
  // `value: unknown`: a null pointer is undefined.
  std::optional<std::string> boundedErrorParam(const json::value& value);
  std::optional<std::string> boundedErrorParam(const json::value* value);
  std::optional<std::string> boundedFailureKind(const json::value& value);
  std::optional<std::string> boundedFailureKind(const json::value* value);
}
