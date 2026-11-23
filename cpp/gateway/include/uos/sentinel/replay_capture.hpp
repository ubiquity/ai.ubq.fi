#pragma once
// Port of src/sentinel/replay-capture.ts.
// Sentinel replay capture: public surface re-exported from the split modules.
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_store.hpp"

namespace uos::sentinel::replay_capture {
  using replay_model::SENTINEL_REPLAY_CHUNK_PREFIX;
  using replay_model::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT;
  using replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
  using replay_model::AcceptedSentinelReplayInput;
  using replay_model::ExportedSentinelReplayCapture;
  using replay_model::SentinelClientBodyObservation;
  using replay_model::SentinelFailureObservation;
  using replay_model::SentinelReplayCaptureOmissionReason;
  using replay_observation::captureAcceptedSentinelReplayInput;
  using replay_observation::createSentinelSseInspector;
  using replay_observation::discardSentinelReplayCaptureCandidate;
  using replay_observation::disposeSentinelUpstreamRecorder;
  using replay_observation::inspectSentinelBufferedResponseBody;
  using replay_observation::inspectSentinelSse;
  using replay_observation::isSentinelReplayRequestId;
  using replay_observation::materializeSentinelReplayInput;
  using replay_observation::resolveSentinelClientFailureObservation;
  using replay_observation::shouldPersistSentinelReplay;
  using replay_observation::snapshotSentinelReplayInput;
  using replay_observation::zeroSentinelReplayInput;
  using replay_read::decryptExportedSentinelReplay;
  using replay_read::listEncryptedSentinelIncidentReplays;
  using replay_read::listEncryptedSentinelReplays;
  using replay_read::listEncryptedSentinelReplaysByRequestId;
  using replay_store::persistEncryptedSentinelReplay;
  using replay_store::persistSentinelReplayFromEnvironment;
  using replay_store::recordSentinelReplayOmissionFromEnvironment;
}
