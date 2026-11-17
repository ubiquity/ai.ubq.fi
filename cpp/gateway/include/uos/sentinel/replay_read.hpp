#pragma once
// Port of src/sentinel/replay-read.ts.
// Sentinel replay manifest reading and export, split out of src/sentinel_replay_capture.ts.
//
// Values read back from KV are `unknown` in the TS: the validators take an rt::KvValue (a stored
// value may hold parts JSON cannot), which a json::value converts to implicitly. The records are
// structs: a stored row's undeclared members are not carried into them. A stored array used as a KV
// key (a status row's or capture reference's `manifest_key`) is converted part by part; a value no
// key part can be throws the SDK's Error "Unsupported keyPart: <typeof> <String(part)>".
#include "uos/sentinel/replay_model.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::sentinel::replay_read {
  // `value: unknown`
  bool isSentinelReplayManifest(const rt::KvValue& value);

  // `value: unknown`
  bool isExportedSentinelReplayCapture(const rt::KvValue& value);

  // `Readonly<{ afterMs: number; beforeMs: number; cursor?: string; limit?: number }>`
  struct ListEncryptedSentinelReplaysOptions {
    double afterMs;
    double beforeMs;
    std::optional<std::string> cursor{};
    std::optional<double> limit{};
  };

  // `Readonly<{ captures: ExportedSentinelReplayCapture[]; cursor: string }>`
  struct EncryptedSentinelReplayPage {
    std::vector<replay_model::ExportedSentinelReplayCapture> captures;
    std::string cursor;
    friend bool operator==(const EncryptedSentinelReplayPage&, const EncryptedSentinelReplayPage&) = default;
  };

  [[nodiscard]] rt::Task<EncryptedSentinelReplayPage> listEncryptedSentinelReplays(rt::Kv& kv, ListEncryptedSentinelReplaysOptions options);

  // `Readonly<{ incidentId: string; cursor?: string; limit?: number }>`
  struct ListEncryptedSentinelIncidentReplaysOptions {
    std::string incidentId;
    std::optional<std::string> cursor{};
    std::optional<double> limit{};
  };

  /**
   * List one incident's exportable captures, one capture per page.
   *
   * A capture reference whose manifest is gone names a capture the store no
   * longer holds (its payload was evicted or expired). That is a real "gone"
   * state, never a fault: the stale reference is removed with a CAS-guarded
   * delete and the scan continues, so one dangling reference can never turn the
   * whole incident export into an error. A reference record that exists but does
   * not describe a capture is still rejected fail-closed.
   */
  [[nodiscard]] rt::Task<EncryptedSentinelReplayPage> listEncryptedSentinelIncidentReplays(rt::Kv& kv, ListEncryptedSentinelIncidentReplaysOptions options);

  /**
   * Read the capture status for one request id. `expired` is derived at read time
   * from the stored expiry. A request with no row is `unknown` while every
   * capture-owned status row is still retained; once bounded pruning has removed
   * any status/tombstone row, a missing lookup is reported as the distinct
   * `status_not_retained` instead of inventing `evicted`/`expired`/`ready`,
   * because the store can no longer prove the row never existed.
   */
  // The overload without `nowMs` is `Date.now()`.
  [[nodiscard]] rt::Task<replay_model::SentinelReplayCaptureStatusRow> readSentinelReplayCaptureStatus(rt::Kv& kv, std::string requestId);
  [[nodiscard]] rt::Task<replay_model::SentinelReplayCaptureStatusRow> readSentinelReplayCaptureStatus(rt::Kv& kv, std::string requestId, double nowMs);

  // `Readonly<{ captures: ExportedSentinelReplayCapture[]; status: SentinelReplayCaptureStatusRow }>`
  struct EncryptedSentinelRequestReplays {
    std::vector<replay_model::ExportedSentinelReplayCapture> captures;
    replay_model::SentinelReplayCaptureStatusRow status;
    friend bool operator==(const EncryptedSentinelRequestReplays&, const EncryptedSentinelRequestReplays&) = default;
  };

  /** Export the capture a request id points at, if its manifest is still present. */
  // The overload without `nowMs` is `Date.now()`.
  [[nodiscard]] rt::Task<EncryptedSentinelRequestReplays> listEncryptedSentinelReplaysByRequestId(rt::Kv& kv, std::string requestId);
  [[nodiscard]] rt::Task<EncryptedSentinelRequestReplays> listEncryptedSentinelReplaysByRequestId(rt::Kv& kv, std::string requestId, double nowMs);

  // Throws rt::JsError with the reader's messages; the decoded plaintext body is zeroed when its
  // integrity check fails.
  [[nodiscard]] rt::Task<replay_model::SentinelReplayPlaintext> decryptExportedSentinelReplay(replay_model::ExportedSentinelReplayCapture exported,
                                                                                            rt::Bytes keyBytes);

  // ---- the module's `export { getChunks, manifestMatchesKey }` ----------------------------------
  [[nodiscard]] rt::Task<std::vector<rt::Bytes>> getChunks(rt::Kv& kv, replay_model::SentinelReplayManifest manifest);

  bool manifestMatchesKey(const rt::KvKey& key, const replay_model::SentinelReplayManifest& manifest);

  // ---- C++ addition ---------------------------------------------------------------------------
  // The typed view of a value isSentinelReplayManifest accepted: its declared members, read from
  // the stored value without a JSON round trip (an unrelated extra member of any kind is ignored,
  // as the TS ignores it). Callers holding a stored manifest (replay-store's winner reads,
  // replay-retention's eviction) use it to pass the value on as a SentinelReplayManifest.
  replay_model::SentinelReplayManifest toSentinelReplayManifest(const rt::KvValue& value);
}
