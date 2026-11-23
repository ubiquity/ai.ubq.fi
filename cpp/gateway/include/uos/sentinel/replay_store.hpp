#pragma once
// Port of src/sentinel/replay-store.ts.
// Sentinel replay persistence, split out of src/sentinel_replay_capture.ts.
//
// The store is `rt::Kv&` (writeSentinelReplayCaptureStatus) or the dependencies' shared handle; it
// must outlive the returned task. The exported functions taking a struct are plain functions
// returning the task, so `co_await f(x)` with named arguments is safe under the GCC 12.2 coroutine
// rules. Optional TS parameters are overloads: `nowMs = Date.now()` is read when the function is
// called, `clientObservation = resolveSentinelClientFailureObservation(observation)` likewise.
// Buffers the TS zeroes (the body snapshot, ciphertext, chunks, decrypted bodies, key bytes) are
// zeroed with OPENSSL_cleanse, and so are this port's own copies of the replay key.
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::sentinel::replay_store {
  /**
   * Bounded per-request status write; never replaces the caller's outcome. The row
   * is admitted through the capture-owned metadata bound (a CAS on the ledger's
   * status_records/metadata_bytes counters, pruning the oldest rows first), so
   * status/tombstone metadata can never grow without bound. A refusal to admit
   * drops this best-effort row instead of exceeding the bound.
   */
  [[nodiscard]] rt::Task<void> writeSentinelReplayCaptureStatus(rt::Kv& kv, const replay_model::SentinelReplayCaptureStatusRow& row);

  /**
   * Publish the explicit per-request status for a request that produced no
   * encrypted payload: an accepted body the fixed cap could not carry, or an
   * authenticated request rejected before capture setup. Best effort by design —
   * the caller's response or exception is never replaced by a status failure.
   */
  // `reason`: SentinelReplayCaptureOmissionReason. The two-argument overload reads Date.now() when called.
  [[nodiscard]] rt::Task<void> recordSentinelReplayOmissionFromEnvironment(const std::string& requestId,
                                                                           const replay_model::SentinelReplayCaptureOmissionReason& reason);
  [[nodiscard]] rt::Task<void> recordSentinelReplayOmissionFromEnvironment(const std::string& requestId,
                                                                           const replay_model::SentinelReplayCaptureOmissionReason& reason, double nowMs);

  // The overload without `clientObservation` resolves it from `observation` when called.
  [[nodiscard]] rt::Task<replay_observation::SentinelReplayPersistResult> persistEncryptedSentinelReplay(
    const replay_model::AcceptedSentinelReplayInput& input, const replay_model::SentinelFailureObservation& observation,
    const replay_observation::PersistDependencies& dependencies);
  [[nodiscard]] rt::Task<replay_observation::SentinelReplayPersistResult> persistEncryptedSentinelReplay(
    const replay_model::AcceptedSentinelReplayInput& input, const replay_model::SentinelFailureObservation& observation,
    const replay_observation::PersistDependencies& dependencies, const replay_model::SentinelClientFailureObservation& clientObservation);

  // `clientObservation?`: the two-argument overload and std::nullopt are undefined. `input`'s body is
  // the caller's shared buffer: it is zeroed when the persist settles, as the TS zeroes it.
  [[nodiscard]] rt::Task<replay_observation::SentinelReplayPersistResult> persistSentinelReplayFromEnvironment(
    const replay_model::AcceptedSentinelReplayInput& input, const replay_model::SentinelFailureObservation& observation);
  [[nodiscard]] rt::Task<replay_observation::SentinelReplayPersistResult> persistSentinelReplayFromEnvironment(
    const replay_model::AcceptedSentinelReplayInput& input, const replay_model::SentinelFailureObservation& observation,
    const std::optional<replay_model::SentinelClientFailureObservation>& clientObservation);
}
