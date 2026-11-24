#include "uos/sentinel/replay_store.hpp"

#include "uos/kv.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_envelope.hpp"
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/utils.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace uos::sentinel::replay_store {
  namespace {
    using incident_outbox::IncidentCaptureCompletion;
    using incident_outbox::IncidentCaptureCompletionCaptured;
    using incident_outbox::IncidentCaptureCompletionUnavailable;
    using incident_outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX;
    using incident_outbox::SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS;
    using incident_outbox::SENTINEL_INCIDENT_INDEX_PREFIX;
    using incident_outbox::SENTINEL_INCIDENT_TTL_MS;
    using incident_outbox::SentinelIncidentCaptureReference;
    using incident_outbox::SentinelIncidentFailureEvent;
    using replay_envelope::ReplayMetadata;
    using replay_model::AcceptedSentinelReplayInput;
    using replay_model::SENTINEL_REPLAY_CHUNK_PREFIX;
    using replay_model::SENTINEL_REPLAY_DEDUPE_PREFIX;
    using replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
    using replay_model::SENTINEL_REPLAY_STATUS_TTL_MS;
    using replay_model::SENTINEL_REPLAY_TTL_MS;
    using replay_model::SentinelClientFailureObservation;
    using replay_model::SentinelFailureObservation;
    using replay_model::SentinelReplayCaptureStatus;
    using replay_model::SentinelReplayCaptureStatusRow;
    using replay_model::SentinelReplayManifest;
    using replay_model::SentinelReplayPlaintext;
    using replay_model::SentinelReplayUnavailableReason;
    using replay_observation::PersistDependencies;
    using replay_observation::SentinelReplayPersistResult;
    using replay_observation::SentinelReplayPersistResultDisabled;
    using replay_observation::SentinelReplayPersistResultDuplicate;
    using replay_observation::SentinelReplayPersistResultIncomplete;
    using replay_observation::SentinelReplayPersistResultStored;
    using replay_retention_schema::SENTINEL_REPLAY_STAGING_BATCH_CHUNKS;
    using replay_retention_schema::SENTINEL_REPLAY_STORAGE_FULL_REASON;
    using replay_retention_schema::SentinelReplayAccountingRow;
    using replay_retention_schema::SentinelReplayPublication;

    // `x.fill(0)` on a buffer that held key, plaintext or ciphertext bytes.
    void zero(rt::Bytes& bytes) {
      if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    }

    // `/^[0-9a-f]{40}$/`
    const rt::Regex& FULL_SHA() {
      static const rt::Regex pattern("^[0-9a-f]{40}$", "");
      return pattern;
    }

    std::string storageFull() { return std::string(SENTINEL_REPLAY_STORAGE_FULL_REASON); }

    rt::Task<void> bindWinnerIndexEvidence(rt::Kv& kv, std::string indexFingerprint, double observedAtMs, std::string referenceFingerprint, rt::KvKey manifestKey,
                                           rt::Bytes keyBytes) {
      // This frame's copy of the replay key.
      auto keyCleanup = rt::scopeExit([&keyBytes] { zero(keyBytes); });
      const rt::KvEntryMaybe<> manifestEntry = co_await kv.get(manifestKey);
      if (!manifestEntry.value.truthy() || !replay_read::isSentinelReplayManifest(manifestEntry.value))
        throw rt::JsError("Sentinel incident replay manifest is unavailable");
      const SentinelReplayManifest manifest = replay_read::toSentinelReplayManifest(manifestEntry.value);
      if (manifest.fingerprint != referenceFingerprint || !replay_read::manifestMatchesKey(manifestKey, manifest))
        throw rt::JsError("Sentinel incident replay manifest is unavailable");
      auto reading = replay_read::getChunks(kv, manifest);
      std::vector<rt::Bytes> chunks = co_await std::move(reading);
      // `plaintext?.body`
      std::shared_ptr<rt::Bytes> plaintextBody;
      rt::Reason failure;
      try {
        auto digesting = replay_envelope::ciphertextDigest(replay_model::concatBytes(chunks));
        const std::string digest = co_await std::move(digesting);
        std::vector<std::string> encodedChunks;
        encodedChunks.reserve(chunks.size());
        for (const rt::Bytes& chunk : chunks) encodedChunks.push_back(utils::base64UrlEncode(chunk));
        const replay_model::ExportedSentinelReplayCapture exported{.manifest = manifest, .chunks = std::move(encodedChunks)};
        auto decrypting = replay_read::decryptExportedSentinelReplay(exported, keyBytes);
        const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
        plaintextBody = plaintext.body;
        const incident_outbox::SentinelIncidentIndexEvidenceBinding binding{
          .observedAtMs = observedAtMs,
          .captureId = manifest.capture_id,
          .gitSha = FULL_SHA().test(plaintext.git_sha) ? std::optional<std::string>(plaintext.git_sha) : std::nullopt,
          .referenceFingerprint = referenceFingerprint,
          .manifestKey = manifestKey,
          .manifestVersionstamp = manifestEntry.versionstamp,
          .capturedAtMs = manifest.captured_at_ms,
          .digest = digest,
          .expiresAtMs = manifest.expires_at_ms,
        };
        auto bindingEvidence = incident_outbox::bindSentinelIncidentIndexEvidence(kv, indexFingerprint, binding);
        co_await std::move(bindingEvidence);
      } catch (...) {
        failure = std::current_exception();
      }
      if (plaintextBody) zero(*plaintextBody);
      for (rt::Bytes& chunk : chunks) zero(chunk);
      if (failure) std::rethrow_exception(failure);
    }

    // `event: KvEntry<SentinelIncidentFailureEvent> | undefined` (std::nullopt is undefined).
    rt::Task<void> completeReplayIncidentEvent(rt::Kv& kv, std::optional<rt::KvEntry<SentinelIncidentFailureEvent>> event, double readyAtMs,
                                               IncidentCaptureCompletion capture) {
      if (!event) co_return;
      auto completing = incident_outbox::completeSentinelIncidentFailureEvent(kv, *event, readyAtMs, capture);
      const bool completed = co_await std::move(completing);
      if (!completed) throw rt::JsError("Sentinel incident capture completion conflicted");
    }

    rt::Task<std::optional<std::string>> resolveIndexFingerprint(incident_outbox::SentinelIncidentClassificationInput classification) {
      std::optional<std::string> fingerprint;
      try {
        auto fingerprinting = incident_outbox::sentinelIncidentFingerprint(classification);
        fingerprint = co_await std::move(fingerprinting);
      } catch (...) {
        // The index is best-effort for direct callers: the environment producer
        // already recorded this observation before the key lookup.
        fingerprint = std::nullopt;
      }
      co_return fingerprint;
    }

    // `{ endpoint, method, observation: clientObservation }`: the observation members the index classifies.
    incident_outbox::SentinelIncidentIndexObservation indexObservationOf(const SentinelClientFailureObservation& observation) {
      return {
        .status = observation.status,
        .stream = observation.stream,
        .completed = observation.completed,
        .framing_valid = observation.framing_valid,
        .terminal_type = observation.terminal_type,
      };
    }

    // The `input` of captureStatusRow.
    struct CaptureStatusInput {
      std::string requestId;
      SentinelReplayCaptureStatus status;
      std::optional<std::string> reason;
      double capturedAtMs;
      std::optional<rt::KvKey> manifestKey;
      std::optional<std::string> fingerprint;
      std::optional<double> expiresAtMs;
    };

    SentinelReplayCaptureStatusRow captureStatusRow(const CaptureStatusInput& input) {
      SentinelReplayCaptureStatusRow row{
        .version = 1,
        .request_id = input.requestId,
        .status = input.status,
        .reason = input.reason,
        .captured_at_ms = input.capturedAtMs,
        .manifest_key = input.manifestKey,
        .fingerprint = input.fingerprint,
        .expires_at_ms = input.expiresAtMs,
      };
      if (!replay_observation::isSentinelReplayCaptureStatusRow(replay_model::toKvValue(row))) throw rt::JsError("Sentinel replay capture status is invalid");
      return row;
    }

    // The `duplicate` of completeDuplicateCapture. `captureStatus`: "ready" | "incomplete".
    struct DuplicateCapture {
      std::string fingerprint;
      rt::KvKey manifestKey;
      std::optional<rt::KvKey> indexKey;
      std::optional<std::string> indexFingerprint;
      std::string requestId;
      std::string captureStatus;
      double capturedAtMs;
      double expiresAtMs;
    };

    /**
     * Settles a capture that already exists (or lost the CAS race): the durable
     * index row, when present, is bound to the winning capture, and the incident
     * outbox event is completed. Nothing pretends the duplicate created evidence.
     */
    rt::Task<SentinelReplayPersistResult> completeDuplicateCapture(std::shared_ptr<const PersistDependencies> dependencies, DuplicateCapture duplicate,
                                                                   double now) {
      rt::Kv& kv = *dependencies->kv;
      if (duplicate.indexKey && duplicate.indexFingerprint) {
        const rt::KvEntryMaybe<> indexEntry = co_await kv.get(*duplicate.indexKey);
        if (!indexEntry.value.isNull()) {
          if (!incident_outbox::isSentinelIncidentIndexRow(indexEntry.value)) {
            throw rt::JsError("Sentinel incident index record is invalid");
          }
          auto binding = bindWinnerIndexEvidence(kv, *duplicate.indexFingerprint, now, duplicate.fingerprint, duplicate.manifestKey, dependencies->keyBytes);
          co_await std::move(binding);
        }
      }
      const IncidentCaptureCompletion completion =
        IncidentCaptureCompletionCaptured{.status = "duplicate", .fingerprint = duplicate.fingerprint, .manifestKey = duplicate.manifestKey};
      auto completing = completeReplayIncidentEvent(kv, dependencies->incidentEvent, now, completion);
      co_await std::move(completing);
      // The request still resolves to real evidence: keep its status row pointing
      // at the winning capture rather than leaving the request unaccounted for.
      // (The row is built before the write: an invalid row throws past the `.catch`.)
      const SentinelReplayCaptureStatusRow statusRow = captureStatusRow({
        .requestId = duplicate.requestId,
        .status = duplicate.captureStatus,
        .reason = std::nullopt,
        .capturedAtMs = duplicate.capturedAtMs,
        .manifestKey = duplicate.manifestKey,
        .fingerprint = duplicate.fingerprint,
        .expiresAtMs = duplicate.expiresAtMs,
      });
      try {
        auto writing = writeSentinelReplayCaptureStatus(kv, statusRow);
        co_await std::move(writing);
      } catch (...) {
      }
      co_return SentinelReplayPersistResultDuplicate{.fingerprint = duplicate.fingerprint, .manifest_key = duplicate.manifestKey};
    }

    /** Encrypts the replay plaintext envelope under the request key. */
    rt::Task<rt::Bytes> encryptReplayPlaintext(std::shared_ptr<const ReplayMetadata> metadata, std::shared_ptr<const rt::Bytes> bodySnapshot, rt::Bytes iv,
                                               rt::Bytes keyBytes, std::string fingerprint) {
      // This frame's copy of the replay key.
      auto keyCleanup = rt::scopeExit([&keyBytes] { zero(keyBytes); });
      rt::Bytes encodedPlaintext = replay_envelope::encodePlaintext(*metadata, *bodySnapshot);
      rt::Bytes compressed;
      rt::Reason failure;
      try {
        auto compressing = replay_envelope::gzip(encodedPlaintext);
        compressed = co_await std::move(compressing);
      } catch (...) {
        failure = std::current_exception();
      }
      zero(encodedPlaintext);
      if (failure) std::rethrow_exception(failure);
      if (static_cast<double>(compressed.size()) + 16 > replay_model::MAX_REPLAY_CIPHERTEXT_BYTES) {
        zero(compressed);
        throw rt::JsError("Sentinel replay compressed payload is too large");
      }
      rt::Bytes encrypted;
      try {
        const rt::crypto::Algorithm algorithm{.name = "AES-GCM", .iv = iv, .additionalData = replay_envelope::encryptionAdditionalData(fingerprint)};
        auto importing = replay_envelope::importAesKey(keyBytes);
        const rt::crypto::CryptoKey key = co_await std::move(importing);
        encrypted = rt::crypto::subtle::encrypt(algorithm, key, compressed);
        co_await rt::microtask();  // `await crypto.subtle.encrypt(...)`
      } catch (...) {
        failure = std::current_exception();
      }
      zero(compressed);
      if (failure) std::rethrow_exception(failure);
      co_return encrypted;
    }
  }

  namespace {
    rt::Task<void> writeSentinelReplayCaptureStatusTask(rt::Kv& kv, SentinelReplayCaptureStatusRow row) {
      const rt::KvValue value = replay_model::toKvValue(row);
      if (!replay_observation::isSentinelReplayCaptureStatusRow(value)) throw rt::JsError("Sentinel replay capture status is invalid");
      // The row outlives its payload so `expired` stays reportable; this is bounded
      // non-sensitive status metadata, never captured request or response content.
      const replay_retention::SentinelReplayStatusMetadataInput input{
        .key = replay_model::requestStatusKey(row.request_id),
        .row = value,
        .now_ms = rt::dateNow(),
        .ttl_ms = SENTINEL_REPLAY_STATUS_TTL_MS,
      };
      auto admitting = replay_retention::admitSentinelReplayStatusMetadata(kv, input);
      co_await std::move(admitting);
    }

    rt::Task<void> recordSentinelReplayOmissionFromEnvironmentTask(std::string requestId, std::string reason, double nowMs) {
      if (!replay_observation::isSentinelReplayRequestId(rt::KvValue(requestId))) co_return;
      try {
        auto opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) co_return;
        const SentinelReplayCaptureStatusRow row = captureStatusRow({
          .requestId = requestId,
          .status = "disabled",
          .reason = reason,
          .capturedAtMs = nowMs,
          .manifestKey = std::nullopt,
          .fingerprint = std::nullopt,
          .expiresAtMs = std::nullopt,
        });
        auto writing = writeSentinelReplayCaptureStatus(*kv, row);
        co_await std::move(writing);
      } catch (...) {
        // Diagnostic only: a status row can never replace the real outcome.
      }
    }
  }

  rt::Task<void> writeSentinelReplayCaptureStatus(rt::Kv& kv, const SentinelReplayCaptureStatusRow& row) {
    return writeSentinelReplayCaptureStatusTask(kv, row);
  }

  rt::Task<void> recordSentinelReplayOmissionFromEnvironment(const std::string& requestId, const replay_model::SentinelReplayCaptureOmissionReason& reason) {
    return recordSentinelReplayOmissionFromEnvironmentTask(requestId, reason, rt::dateNow());
  }

  rt::Task<void> recordSentinelReplayOmissionFromEnvironment(const std::string& requestId, const replay_model::SentinelReplayCaptureOmissionReason& reason,
                                                             double nowMs) {
    return recordSentinelReplayOmissionFromEnvironmentTask(requestId, reason, nowMs);
  }

  namespace {
    // `status: Readonly<{ requestId: string; captureStatus: "ready" | "incomplete" }>`
    struct ReplayStoreStatus {
      std::string requestId;
      std::string captureStatus;
    };

    /** The dedupe/manifest/incident-event writes every store attempt starts from. */
    std::unique_ptr<rt::AtomicOperation> replayStoreOperation(const PersistDependencies& dependencies, const rt::KvKey& dedupeKey, const rt::KvKey& manifestKey,
                                                              const SentinelReplayManifest& manifest, const std::string& fingerprint, double now,
                                                              const ReplayStoreStatus& status, const SentinelReplayPublication& publication) {
      std::unique_ptr<rt::AtomicOperation> operation = dependencies.kv->atomic();
      rt::KvSetOptions payloadOptions;
      payloadOptions.expireIn = SENTINEL_REPLAY_TTL_MS;
      rt::KvSetOptions statusOptions;
      statusOptions.expireIn = SENTINEL_REPLAY_STATUS_TTL_MS;
      operation->check(rt::AtomicCheck(dedupeKey, std::nullopt))
        .set(dedupeKey, replay_envelope::dedupeRecordFor(manifestKey, manifest), payloadOptions)
        .set(manifestKey, rt::KvValue(json::value_from(manifest)), payloadOptions)
        // The accounted key IS the written key: the publication status_key names the
        // exact row whose replaced bytes the ledger delta already subtracted.
        .set(publication.status_key,
             replay_model::toKvValue(captureStatusRow({
               .requestId = status.requestId,
               .status = status.captureStatus,
               .reason = std::nullopt,
               .capturedAtMs = manifest.captured_at_ms,
               .manifestKey = manifestKey,
               .fingerprint = fingerprint,
               .expiresAtMs = manifest.expires_at_ms,
             })),
             statusOptions)
        // The budget transition and the accounting-row publish commit with the
        // manifest, so no manifest can ever be visible before it is accounted. The
        // accounting row's exact versionstamp pins state==="reserved", this writer's
        // fence and a still-unexpired row; the ledger versionstamp pins the counters.
        .check(rt::AtomicCheck(publication.ledger_key, publication.ledger_versionstamp))
        .check(rt::AtomicCheck(publication.accounting_key, publication.accounting_versionstamp))
        .check(rt::AtomicCheck(publication.status_key, publication.status_versionstamp))
        .set(publication.ledger_key, replay_retention_schema::sentinelReplayLedgerValue(publication.ledger))
        .set(publication.accounting_key, replay_retention_schema::sentinelReplayAccountingValue(publication.accounting));
      if (dependencies.incidentEvent) {
        const rt::KvEntry<SentinelIncidentFailureEvent>& event = *dependencies.incidentEvent;
        const IncidentCaptureCompletion stored = IncidentCaptureCompletionCaptured{.status = "stored", .fingerprint = fingerprint, .manifestKey = manifestKey};
        const SentinelIncidentFailureEvent readyEvent = incident_outbox::readySentinelIncidentFailureEvent(event, now, stored);
        rt::KvSetOptions eventOptions;
        eventOptions.expireIn = SENTINEL_INCIDENT_TTL_MS;
        operation->check(rt::AtomicCheck(event.key, event.versionstamp)).set(event.key, incident_outbox::toKvValue(readyEvent), eventOptions);
      }
      return operation;
    }

    // `Readonly<{ key: KvKey; entry: KvEntry<SentinelIncidentIndexRow> }>`
    struct IncidentIndexEntry {
      rt::KvKey key;
      rt::KvEntryMaybe<> entry;
    };

    // The `context` of withIncidentIndexEvidence.
    struct IncidentIndexEvidenceContext {
      std::string gitSha;
      std::string captureId;
      std::string evidenceDigest;
      rt::KvKey manifestKey;
      double capturedAtMs;
      double expiresAtMs;
      double now;
      std::string fingerprint;
    };

    /** Adds the evidence row and capture reference for an existing index observation. */
    rt::AtomicOperation& withIncidentIndexEvidence(rt::AtomicOperation& operation, const IncidentIndexEntry& indexRow, const IncidentIndexEvidenceContext& context) {
      if (!incident_outbox::isSentinelIncidentIndexRow(indexRow.entry.value)) {
        throw rt::JsError("Sentinel incident index record is invalid");
      }
      rt::KvObject next = indexRow.entry.value.asObject(), provenance = next.at("provenance").asObject();
      provenance.set("captured_at_ms", rt::KvValue(context.capturedAtMs));
      next.set("failing_revision", FULL_SHA().test(context.gitSha) ? rt::KvValue(context.gitSha) : rt::KvValue(nullptr));
      next.set("provenance", rt::KvValue(std::move(provenance)));
      const incident_outbox::SentinelIncidentIndexEvidenceRef evidence{.ref = "capture:" + context.captureId, .digest = context.evidenceDigest};
      next.set("evidence_ref", rt::KvValue(json::value_from(evidence))); next.set("evidence_expires_at_ms", rt::KvValue(context.expiresAtMs));
      const rt::KvValue nextValue(std::move(next));
      if (!incident_outbox::isSentinelIncidentIndexRow(nextValue)) throw rt::JsError("Sentinel incident index record is invalid");
      const SentinelIncidentCaptureReference reference{.version = 1, .manifest_key = context.manifestKey};
      rt::KvSetOptions referenceOptions;
      referenceOptions.expireIn = context.expiresAtMs - context.now;
      return operation.check(rt::AtomicCheck(indexRow.key, indexRow.entry.versionstamp))
        .set(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, nextValue["incident_id"].asString(), context.fingerprint}, incident_outbox::toKvValue(reference), referenceOptions)
        .set(indexRow.key, nextValue);
    }

    /** Reads the durable index observation once, when one exists. */
    // `indexKey: KvKey | null`; std::nullopt is null.
    rt::Task<std::optional<IncidentIndexEntry>> readIncidentIndexEntry(rt::Kv& kv, std::optional<rt::KvKey> indexKey) {
      if (!indexKey) co_return std::nullopt;
      const rt::KvEntryMaybe<> entry = co_await kv.get(*indexKey);
      if (entry.value.isNull()) co_return std::nullopt;
      co_return IncidentIndexEntry{.key = *indexKey, .entry = entry};
    }

    // The `context` of storeReplayEnvelope. `chunks` belong to persistEncryptedSentinelReplay, which zeroes them.
    struct StoreReplayEnvelopeContext {
      std::shared_ptr<const PersistDependencies> dependencies;
      std::string gitSha;
      std::vector<rt::Bytes> chunks;
      rt::KvKey dedupeKey;
      rt::KvKey manifestKey;
      SentinelReplayManifest manifest;
      std::optional<rt::KvKey> indexKey;
      std::optional<std::string> indexFingerprint;
      std::string captureId;
      std::string evidenceDigest;
      double now;
      double expiresAtMs;
      std::string requestId;
      std::string captureStatus;  // "ready" | "incomplete"
      SentinelReplayAccountingRow accounting;
      rt::KvKey accountingKey;
      double actualCharge;
    };
    using StoreReplayEnvelope = std::shared_ptr<const StoreReplayEnvelopeContext>;

    // `dependencies.currentNow?.() ?? dependencies.now?.() ?? Date.now()`
    double currentNow(const PersistDependencies& dependencies) {
      if (dependencies.currentNow) return dependencies.currentNow();
      if (dependencies.now) return dependencies.now();
      return rt::dateNow();
    }

    rt::Task<void> cleanupChunks(StoreReplayEnvelope context) {
      std::vector<rt::Promise<void>> deletions;
      deletions.reserve(context->chunks.size());
      for (std::size_t index = 0; index < context->chunks.size(); ++index) {
        rt::KvKey key{SENTINEL_REPLAY_CHUNK_PREFIX, context->captureId, static_cast<double>(index)};
        deletions.push_back(rt::Promise<void>(context->dependencies->kv->delete_(std::move(key))));
      }
      auto deleted = rt::all(std::move(deletions));
      co_await deleted.wait();
    }

    rt::Task<void> abandon(StoreReplayEnvelope context) {
      try {
        auto cleaning = cleanupChunks(context);
        co_await std::move(cleaning);
      } catch (...) {
      }
      try {
        const replay_retention::SentinelReplayAbandonOptions options{.now_ms = context->now};
        auto abandoning = replay_retention::abandonSentinelReplayAccounting(*context->dependencies->kv, context->accounting, context->accountingKey, options);
        co_await std::move(abandoning);
      } catch (...) {
      }
    }

    // The `try` block of storeReplayEnvelope.
    rt::Task<SentinelReplayPersistResult> storeReplayEnvelopeAttempts(StoreReplayEnvelope context) {
      const PersistDependencies& dependencies = *context->dependencies;
      rt::Kv& kv = *dependencies.kv;
      const std::string& fingerprint = context->manifest.fingerprint;
      const std::size_t batchChunks = static_cast<std::size_t>(SENTINEL_REPLAY_STAGING_BATCH_CHUNKS);
      for (std::size_t offset = 0; offset < context->chunks.size(); offset += batchChunks) {
        const replay_retention::SentinelReplayStagingFenceInput fenceInput{
          .accounting_key = context->accountingKey,
          .fence = context->accounting.fence,
          .now_ms = currentNow(dependencies),
          .budget_bytes = dependencies.budgetBytes,
        };
        auto advancing = replay_retention::advanceSentinelReplayStagingFence(kv, fenceInput);
        const replay_retention::SentinelReplayStagingFence fence = co_await std::move(advancing);
        const replay_retention::SentinelReplayStagingFenceOk* advanced = std::get_if<replay_retention::SentinelReplayStagingFenceOk>(&fence);
        if (!advanced) {
          // The fence commit failed: write nothing further and abandon instead of
          // staging bytes a reaper has already fenced off.
          auto abandoning = abandon(context);
          co_await std::move(abandoning);
          co_return SentinelReplayPersistResultIncomplete{.reason = storageFull()};
        }
        // `chunks.slice(offset, offset + SENTINEL_REPLAY_STAGING_BATCH_CHUNKS).reduce(...)`
        const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(rt::AtomicCheck(context->accountingKey, advanced->versionstamp));
        rt::KvSetOptions chunkOptions;
        chunkOptions.expireIn = SENTINEL_REPLAY_TTL_MS;
        const std::size_t end = std::min(context->chunks.size(), offset + batchChunks);
        for (std::size_t index = offset; index < end; ++index) {
          operation->set(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, context->captureId, static_cast<double>(index)}, rt::KvValue(context->chunks[index]),
                         chunkOptions);
        }
        const rt::KvCommitResult staged = co_await operation->commit();
        if (!staged.ok) {
          auto abandoning = abandon(context);
          co_await std::move(abandoning);
          co_return SentinelReplayPersistResultIncomplete{.reason = storageFull()};
        }
      }
      for (double attempt = 0; attempt < SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS; attempt += 1) {
        // Fresh ledger and accounting row on every attempt: a revoked, expired or
        // already-published row can never publish unaccounted bytes.
        const SentinelReplayCaptureStatusRow statusRow = captureStatusRow({
          .requestId = context->requestId,
          .status = context->captureStatus,
          .reason = std::nullopt,
          .capturedAtMs = context->manifest.captured_at_ms,
          .manifestKey = context->manifestKey,
          .fingerprint = fingerprint,
          .expiresAtMs = context->manifest.expires_at_ms,
        });
        const replay_retention::SentinelReplayPublicationOptions publicationOptions{
          .now_ms = context->now,
          .lease_now_ms = currentNow(dependencies),
          .status_key = replay_model::requestStatusKey(context->requestId),
          .status_bytes = replay_retention_schema::sentinelReplayStatusMetadataBytes(replay_model::toKvValue(statusRow)),
          .budget_bytes = dependencies.budgetBytes,
        };
        auto preparing =
          replay_retention::prepareSentinelReplayPublication(kv, context->accounting, context->accountingKey, context->actualCharge, publicationOptions);
        const std::optional<SentinelReplayPublication> publication = co_await std::move(preparing);
        if (!publication) {
          auto abandoning = abandon(context);
          co_await std::move(abandoning);
          co_return SentinelReplayPersistResultIncomplete{.reason = storageFull()};
        }
        auto readingIndex = readIncidentIndexEntry(kv, context->indexKey);
        const std::optional<IncidentIndexEntry> indexRow = co_await std::move(readingIndex);
        const ReplayStoreStatus status{.requestId = context->requestId, .captureStatus = context->captureStatus};
        const std::unique_ptr<rt::AtomicOperation> operation =
          replayStoreOperation(dependencies, context->dedupeKey, context->manifestKey, context->manifest, fingerprint, context->now, status, *publication);
        if (indexRow) {
          const IncidentIndexEvidenceContext evidence{
            .gitSha = context->gitSha,
            .captureId = context->captureId,
            .evidenceDigest = context->evidenceDigest,
            .manifestKey = context->manifestKey,
            .capturedAtMs = context->manifest.captured_at_ms,
            .expiresAtMs = context->expiresAtMs,
            .now = context->now,
            .fingerprint = fingerprint,
          };
          withIncidentIndexEvidence(*operation, *indexRow, evidence);
        }
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return SentinelReplayPersistResultStored{.manifest = context->manifest, .manifest_key = context->manifestKey};
      }
      auto abandoning = abandon(context);
      co_await std::move(abandoning);
      const rt::KvEntryMaybe<> winningDedupe = co_await kv.get(context->dedupeKey);
      const std::optional<rt::KvKey> winningManifestKey = replay_envelope::dedupeManifestKey(winningDedupe.value);
      if (!winningManifestKey) throw rt::JsError("Sentinel replay dedupe winner is unavailable");
      const rt::KvEntryMaybe<> winner = co_await kv.get(*winningManifestKey);
      if (!replay_read::isSentinelReplayManifest(winner.value)) {
        // The winner vanished (evicted or expired) between the CAS races; the
        // caller's fresh capture is the only evidence and cannot be reported as
        // a duplicate of nothing.
        co_return SentinelReplayPersistResultIncomplete{.reason = storageFull()};
      }
      const SentinelReplayManifest winnerManifest = replay_read::toSentinelReplayManifest(winner.value);
      const DuplicateCapture duplicate{
        .fingerprint = fingerprint,
        .manifestKey = *winningManifestKey,
        .indexKey = context->indexKey,
        .indexFingerprint = context->indexFingerprint,
        .requestId = context->requestId,
        .captureStatus = context->captureStatus,
        .capturedAtMs = winnerManifest.captured_at_ms,
        .expiresAtMs = winnerManifest.expires_at_ms,
      };
      auto completing = completeDuplicateCapture(context->dependencies, duplicate, context->now);
      SentinelReplayPersistResult result = co_await std::move(completing);
      co_return result;
    }

    /**
     * Write the chunks in bounded, fence-guarded batches, then commit the envelope
     * with a bounded CAS retry loop.
     *
     * FENCED STAGING: before each batch of at most
     * SENTINEL_REPLAY_STAGING_BATCH_CHUNKS chunks, one atomic commit checks the
     * accounting row's exact versionstamp and requires state==="reserved",
     * fence===this writer's fence and an unexpired row, incrementing `stage`. Only
     * after that commit succeeds may one chunk transaction check its committed
     * versionstamp and write the batch. Revoke or release changes that row, so a
     * paused writer cannot append chunks after cleanup has released its charge.
     */
    rt::Task<SentinelReplayPersistResult> storeReplayEnvelope(StoreReplayEnvelope context) {
      rt::Reason failure;
      try {
        auto storing = storeReplayEnvelopeAttempts(context);
        SentinelReplayPersistResult result = co_await std::move(storing);
        co_return result;
      } catch (...) {
        failure = std::current_exception();
      }
      auto abandoning = abandon(context);
      co_await std::move(abandoning);
      std::rethrow_exception(failure);
    }

    /** Everything the duplicate/stale branch needs that is not already in the dedupe record. */
    struct ExistingDedupeContext {
      rt::KvKey dedupeKey;
      std::string fingerprint;
      std::optional<rt::KvKey> indexKey;
      std::optional<std::string> indexFingerprint;
      std::string requestId;
      std::string captureStatus;  // "ready" | "incomplete"
      double now;
    };

    /**
     * Resolve an already-present dedupe row before storing a new capture.
     *
     * A dedupe row ALWAYS means this request is a duplicate: the row names the
     * winning manifest, and the duplicate must inherit that winner's exact identity
     * and expiry rather than inventing a newer now + TTL window. It is never cleared
     * here. A winner whose evidence is already gone either fails closed inside
     * `completeDuplicateCapture` (when a recorded observation still references it,
     * so a fresh capture would silently replace evidence) or reports `duplicate`
     * with nothing bound (when nothing was ever observed to attach to). Eviction and
     * expiry remove their own dedupe row by CAS, so a re-capture after real
     * retention still stores. Returns null only when there is no row to resolve.
     */
    // std::nullopt is null.
    rt::Task<std::optional<SentinelReplayPersistResult>> resolveExistingDedupe(std::shared_ptr<const PersistDependencies> dependencies,
                                                                               rt::KvEntryMaybe<> existingDedupe, ExistingDedupeContext context) {
      if (existingDedupe.value.isNull()) co_return std::nullopt;
      const std::optional<rt::KvKey> manifestKey = replay_envelope::dedupeManifestKey(existingDedupe.value);
      if (!manifestKey) throw rt::JsError("Sentinel replay dedupe record is invalid");
      const std::optional<replay_envelope::DedupeWinnerIdentity> identity = replay_envelope::dedupeWinnerIdentity(existingDedupe.value);
      double capturedAtMs = identity ? identity->capturedAtMs : context.now;
      double expiresAtMs = identity ? identity->expiresAtMs : context.now + SENTINEL_REPLAY_TTL_MS;
      if (!identity) {
        // Legacy dedupe rows predate the recorded identity, so the winner manifest is
        // the only remaining source for its real timestamp and expiry.
        const rt::KvEntryMaybe<> winnerEntry = co_await dependencies->kv->get(*manifestKey);
        if (replay_read::isSentinelReplayManifest(winnerEntry.value)) {
          const SentinelReplayManifest winner = replay_read::toSentinelReplayManifest(winnerEntry.value);
          capturedAtMs = winner.captured_at_ms;
          expiresAtMs = winner.expires_at_ms;
        }
      }
      const DuplicateCapture duplicate{
        .fingerprint = context.fingerprint,
        .manifestKey = *manifestKey,
        .indexKey = context.indexKey,
        .indexFingerprint = context.indexFingerprint,
        .requestId = context.requestId,
        .captureStatus = context.captureStatus,
        .capturedAtMs = capturedAtMs,
        .expiresAtMs = expiresAtMs,
      };
      auto completing = completeDuplicateCapture(dependencies, duplicate, context.now);
      SentinelReplayPersistResult result = co_await std::move(completing);
      co_return result;
    }

    struct PersistencePolicy {
      bool requestOnly = false;
      bool deliveryInterrupted = false;

      bool eligible(const SentinelFailureObservation& observation, const SentinelClientFailureObservation& clientObservation) const {
        return requestOnly ? replay_observation::shouldPersistFailedRequestPayload(observation, clientObservation, deliveryInterrupted)
                           : replay_observation::shouldPersistSentinelReplay(observation, clientObservation);
      }
    };

    rt::Task<SentinelReplayPersistResult> persistEncryptedSentinelReplayTask(AcceptedSentinelReplayInput input, SentinelFailureObservation observation,
                                                                            std::shared_ptr<PersistDependencies> dependencies,
                                                                            SentinelClientFailureObservation clientObservation,
                                                                            PersistencePolicy policy = {}) {
      // This frame's copy of the replay key (the caller's own buffer is the caller's to zero).
      auto keyCleanup = rt::scopeExit([dependencies] { zero(dependencies->keyBytes); });
      if (!policy.eligible(observation, clientObservation)) {
        throw rt::JsError("A successful request cannot be persisted as a sentinel replay");
      }
      if (policy.requestOnly) {
        input.endpoint = input.endpoint.substr(0, input.endpoint.find('?'));
        input.compatibility_headers.clear();
        input.upstream.reset();
        dependencies->incidentEvent.reset();
        clientObservation.terminal_body_base64.reset();
        clientObservation.terminal_body_truncated = false;
        if (policy.deliveryInterrupted && !clientObservation.failure_kind) clientObservation.failure_kind = "delivery_interrupted";
      }
      if (static_cast<double>(dependencies->keyBytes.size()) != replay_model::REPLAY_KEY_BYTES) throw rt::JsError("Sentinel replay key must be 32 bytes");
      if (!replay_envelope::isCompatibilityHeaders(json::value(input.compatibility_headers))) {
        throw rt::JsError("Sentinel replay compatibility headers contain a disallowed value");
      }

      // Cancellation cleanup can zero the request-owned buffer while KV and
      // cryptographic operations are pending. One synchronous snapshot must feed
      // the digests and encrypted envelope so a capture cannot disagree with its
      // own manifest. Request-only callers emit the required empty upstream trace
      // with all truncation flags false; it is never labeled captured coverage.
      const upstream_capture::SentinelUpstreamTrace upstreamTrace =
        input.upstream ? upstream_capture::parseSentinelUpstreamTrace(*input.upstream) : upstream_capture::emptySentinelUpstreamTrace();
      if (!input.body) throw rt::TypeError("undefined is not an object (evaluating 'value.slice')");
      const std::shared_ptr<rt::Bytes> bodySnapshot = std::make_shared<rt::Bytes>(replay_model::cloneBytes(*input.body));
      AcceptedSentinelReplayInput snapshotInput = input;
      snapshotInput.body = bodySnapshot;
      auto bodyCleanup = rt::scopeExit([bodySnapshot] { zero(*bodySnapshot); });
      const std::shared_ptr<const PersistDependencies> shared = dependencies;
      rt::Kv& kv = *dependencies->kv;
      const double now = dependencies->now ? dependencies->now() : rt::dateNow();
      const std::string failureSignature = replay_observation::sentinelFailureSignature(clientObservation);
      auto fingerprinting = replay_envelope::hmacHex(dependencies->keyBytes, "fingerprint",
                                                     replay_envelope::fingerprintParts(snapshotInput, failureSignature, "fingerprint", upstreamTrace));
      const std::string fingerprint = co_await std::move(fingerprinting);
      auto grouping = replay_envelope::hmacHex(dependencies->keyBytes, "case-group", replay_envelope::fingerprintParts(snapshotInput, failureSignature, "case-group"));
      const std::string caseGroupDigest = co_await std::move(grouping);
      const std::vector<SentinelReplayUnavailableReason> unavailable = replay_envelope::replayUnavailableReasons(upstreamTrace, clientObservation);
      const rt::KvKey dedupeKey{SENTINEL_REPLAY_DEDUPE_PREFIX, fingerprint};
      const incident_outbox::SentinelIncidentClassificationInput classification{
        .endpoint = input.endpoint,
        .method = input.method,
        .observation = indexObservationOf(clientObservation),
      };
      std::optional<std::string> indexFingerprint;
      if (!policy.requestOnly) {
        auto resolving = resolveIndexFingerprint(classification);
        indexFingerprint = co_await std::move(resolving);
      }
      const std::optional<rt::KvKey> indexKey =
        indexFingerprint ? std::optional<rt::KvKey>(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, *indexFingerprint}) : std::nullopt;
      const std::string captureStatus = unavailable.empty() ? "ready" : "incomplete";
      const rt::KvEntryMaybe<> existingDedupe = co_await kv.get(dedupeKey);
      const ExistingDedupeContext dedupeContext{
        .dedupeKey = dedupeKey,
        .fingerprint = fingerprint,
        .indexKey = indexKey,
        .indexFingerprint = indexFingerprint,
        .requestId = input.request_id,
        .captureStatus = captureStatus,
        .now = now,
      };
      auto resolvingDuplicate = resolveExistingDedupe(shared, existingDedupe, dedupeContext);
      const std::optional<SentinelReplayPersistResult> duplicate = co_await std::move(resolvingDuplicate);
      if (duplicate) co_return *duplicate;

      const std::string captureId = dependencies->randomUuid ? dependencies->randomUuid() : rt::crypto::randomUUID();
      const rt::Bytes iv =
        dependencies->randomBytes ? dependencies->randomBytes(replay_model::AES_GCM_IV_BYTES) : replay_model::randomBytes(replay_model::AES_GCM_IV_BYTES);
      if (static_cast<double>(iv.size()) != replay_model::AES_GCM_IV_BYTES) throw rt::JsError("Sentinel replay IV must be 12 bytes");
      // The metadata literal's members in order; `body_sha256` is its one await.
      std::optional<replay_model::SentinelReplaySettings> settings = replay_envelope::requestSettingsFromBody(*bodySnapshot, observation.provider_route, observation.stream);
      std::optional<std::string> replayCoverage = replay_envelope::replayCoverageFor(upstreamTrace, clientObservation, unavailable);
      auto digestingBody = replay_envelope::ciphertextDigest(*bodySnapshot);
      const std::string bodySha256 = co_await std::move(digestingBody);
      const std::shared_ptr<const ReplayMetadata> metadata = std::make_shared<const ReplayMetadata>(ReplayMetadata{
        .version = replay_model::REPLAY_PLAINTEXT_VERSION,
        .captured_at_ms = now,
        .endpoint = input.endpoint,
        .method = input.method,
        .content_type = input.content_type,
        .compatibility_headers = input.compatibility_headers,
        .failure_signature = failureSignature,
        .observation = observation,
        .client_observation = clientObservation,
        .request_id = input.request_id,
        .git_sha = input.git_sha,
        .deno_revision = input.deno_revision,
        .upstream = upstreamTrace,
        .settings = std::move(settings),
        .capture_status = captureStatus,
        .replay_coverage = std::move(replayCoverage),
        .unavailable = unavailable,
        .body_sha256 = bodySha256,
        .body_bytes = static_cast<double>(bodySnapshot->size()),
        .downstream = replay_envelope::downstreamObservation(clientObservation),
      });
      const double metadataJsonBytes = static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(json::value_from(*metadata))));
      if (metadataJsonBytes > replay_model::MAX_REPLAY_METADATA_BYTES) throw rt::JsError("Sentinel replay metadata is too large");
      // Reserve the encoded envelope capacity BEFORE any chunk is written. A
      // refusal is a normal bounded outcome, never an unaccounted store.
      const replay_retention::SentinelReplayCapacityInput capacity{
        .capture_id = captureId,
        .request_id = input.request_id,
        .plaintext_bytes = 4 + metadataJsonBytes + static_cast<double>(bodySnapshot->size()),
        .metadata_bytes = metadataJsonBytes,
        .fingerprint = fingerprint,
        .now_ms = now,
        .budget_bytes = dependencies->budgetBytes,
      };
      auto reserving = replay_retention::reserveSentinelReplayCapacity(kv, capacity);
      const replay_retention_schema::SentinelReplayAdmission admission = co_await std::move(reserving);
      if (const auto* refused = std::get_if<replay_retention_schema::SentinelReplayAdmissionNotOk>(&admission)) {
        co_return SentinelReplayPersistResultIncomplete{.reason = refused->reason};
      }
      const replay_retention_schema::SentinelReplayAdmissionOk& admitted = std::get<replay_retention_schema::SentinelReplayAdmissionOk>(admission);
      const SentinelReplayAccountingRow accounting = admitted.accounting;
      const rt::KvKey accountingKey = admitted.accounting_key;
      rt::Reason failure;
      try {
        auto encrypting = encryptReplayPlaintext(metadata, bodySnapshot, iv, dependencies->keyBytes, fingerprint);
        rt::Bytes encrypted = co_await std::move(encrypting);
        auto encryptedCleanup = rt::scopeExit([&encrypted] { zero(encrypted); });
        std::vector<rt::Bytes> chunks = replay_envelope::splitChunks(encrypted);
        const double expiresAtMs = now + SENTINEL_REPLAY_TTL_MS;
        const double actualCharge = replay_limits::sentinelReplayStoredCharge(static_cast<double>(encrypted.size()), metadataJsonBytes);
        const SentinelReplayManifest manifest{
          .version = replay_model::ENVELOPE_VERSION,
          .capture_id = captureId,
          .fingerprint = fingerprint,
          .case_group_digest = caseGroupDigest,
          .captured_at_ms = now,
          .expires_at_ms = expiresAtMs,
          .algorithm = "AES-256-GCM",
          .compression = "gzip",
          .iv = utils::base64UrlEncode(iv),
          .chunk_count = static_cast<double>(chunks.size()),
          .ciphertext_bytes = static_cast<double>(encrypted.size()),
          .stored_bytes = actualCharge,
          .request_id = input.request_id,
        };

        const rt::KvKey manifestKey{SENTINEL_REPLAY_MANIFEST_PREFIX, now, fingerprint, captureId};
        auto digestingEvidence = replay_envelope::ciphertextDigest(encrypted);
        const std::string evidenceDigest = co_await std::move(digestingEvidence);
        const std::shared_ptr<StoreReplayEnvelopeContext> context = std::make_shared<StoreReplayEnvelopeContext>(StoreReplayEnvelopeContext{
          .dependencies = shared,
          .gitSha = input.git_sha,
          .chunks = std::move(chunks),
          .dedupeKey = dedupeKey,
          .manifestKey = manifestKey,
          .manifest = manifest,
          .indexKey = indexKey,
          .indexFingerprint = indexFingerprint,
          .captureId = captureId,
          .evidenceDigest = evidenceDigest,
          .now = now,
          .expiresAtMs = expiresAtMs,
          .requestId = input.request_id,
          .captureStatus = captureStatus,
          .accounting = accounting,
          .accountingKey = accountingKey,
          .actualCharge = actualCharge,
        });
        auto chunksCleanup = rt::scopeExit([context] {
          for (rt::Bytes& chunk : context->chunks) zero(chunk);
        });
        auto storing = storeReplayEnvelope(context);
        SentinelReplayPersistResult result = co_await std::move(storing);
        co_return result;
      } catch (...) {
        failure = std::current_exception();
      }
      try {
        const replay_retention::SentinelReplayAbandonOptions options{.now_ms = now};
        auto abandoning = replay_retention::abandonSentinelReplayAccounting(kv, accounting, accountingKey, options);
        co_await std::move(abandoning);
      } catch (...) {
      }
      std::rethrow_exception(failure);
    }
  }

  rt::Task<SentinelReplayPersistResult> persistEncryptedSentinelReplay(const AcceptedSentinelReplayInput& input, const SentinelFailureObservation& observation,
                                                                       const PersistDependencies& dependencies) {
    return persistEncryptedSentinelReplay(input, observation, dependencies, replay_observation::resolveSentinelClientFailureObservation(observation));
  }

  rt::Task<SentinelReplayPersistResult> persistEncryptedSentinelReplay(const AcceptedSentinelReplayInput& input, const SentinelFailureObservation& observation,
                                                                       const PersistDependencies& dependencies,
                                                                       const SentinelClientFailureObservation& clientObservation) {
    return persistEncryptedSentinelReplayTask(input, observation, std::make_shared<PersistDependencies>(dependencies), clientObservation);
  }

  namespace {
    std::optional<rt::Bytes> readReplayKeyFromEnvironment() {
      try {
        const std::optional<std::string> value = rt::env::get("SENTINEL_REPLAY_KEY");
        if (!value) return std::nullopt;
        const std::string raw = rt::trim(*value);
        return !raw.empty() ? replay_envelope::decodeSentinelReplayKey(raw) : std::nullopt;
      } catch (...) {
        return std::nullopt;
      }
    }

    // `console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason }))`
    void warnDeferred(std::string_view reason) {
      json::object payload;
      payload["status"] = "deferred";
      payload["reason"] = json::string(reason);
      rt::console::warn("[ai.ubq.fi] sentinel_incident", rt::jsonStringify(json::value(std::move(payload))));
    }

    // `recordStatus`: `kv` is the handler's handle (a null pointer while unset or null).
    rt::Task<void> recordStatus(std::shared_ptr<rt::Kv> kv, std::string requestId, std::string status, std::string reason, double capturedAtMs) {
      if (!kv || !replay_observation::isSentinelReplayRequestId(rt::KvValue(requestId))) co_return;
      try {
        const SentinelReplayCaptureStatusRow row = captureStatusRow({
          .requestId = requestId,
          .status = status,
          .reason = reason,
          .capturedAtMs = capturedAtMs,
          .manifestKey = std::nullopt,
          .fingerprint = std::nullopt,
          .expiresAtMs = std::nullopt,
        });
        auto writing = writeSentinelReplayCaptureStatus(*kv, row);
        co_await std::move(writing);
      } catch (...) {
        // A status row is diagnostic: its own failure must never replace the real
        // persist outcome the caller already has.
      }
    }

    rt::Task<SentinelReplayPersistResult> persistSentinelReplayFromEnvironmentTask(AcceptedSentinelReplayInput input, SentinelFailureObservation observation,
                                                                                  SentinelClientFailureObservation resolvedClientObservation,
                                                                                  PersistencePolicy policy = {}) {
      std::optional<rt::Bytes> keyBytes;
      std::shared_ptr<rt::Kv> kv;
      std::optional<rt::KvEntry<SentinelIncidentFailureEvent>> incidentEvent;
      const double now = rt::dateNow();
      auto cleanup = rt::scopeExit([&keyBytes, &input] {
        if (keyBytes) zero(*keyBytes);
        replay_observation::zeroSentinelReplayInput(&input);
      });
      if (!policy.eligible(observation, resolvedClientObservation)) {
        throw rt::JsError("A successful request cannot be persisted as a sentinel replay");
      }
      auto opening = ::uos::kv::getKv();
      kv = co_await opening.wait();
      if (!kv) co_return SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"};
      if (!policy.requestOnly && replay_observation::shouldSignalSentinelIncident(observation, resolvedClientObservation)) {
        // The durable index observation is recorded BEFORE the key lookup and
        // encryption: even a missing key still leaves a discoverable incident
        // row with evidence_ref null. Unlike the transient event helper this
        // passive path is never environment-gated (it must work in the real
        // handler tests without a production deployment flag).
        bool indexFailed = false;
        try {
          const incident_outbox::SentinelIncidentIndexObservationInput indexInput{
            .endpoint = input.endpoint,
            .method = input.method,
            .gitSha = input.git_sha,
            .observedAtMs = now,
            .observation = indexObservationOf(resolvedClientObservation),
            .classification =
              incident_outbox::SentinelIncidentIndexClassification{
                .provider = resolvedClientObservation.provider_route,
                .model = std::nullopt,
                .reasoning = std::nullopt,
                .failure_kind = resolvedClientObservation.failure_kind,
              },
          };
          auto recording = incident_outbox::recordSentinelIncidentIndexObservation(*kv, indexInput);
          co_await std::move(recording);
        } catch (...) {
          indexFailed = true;
        }
        if (indexFailed) warnDeferred("index_write_failed");
        bool outboxFailed = false;
        try {
          auto creating = incident_outbox::createSentinelIncidentFailureEventFromEnvironment(*kv, now);
          std::optional<rt::KvEntry<SentinelIncidentFailureEvent>> created = co_await std::move(creating);
          incidentEvent = std::move(created);
        } catch (...) {
          outboxFailed = true;
        }
        if (outboxFailed) warnDeferred("outbox_write_failed");
      }
      keyBytes = readReplayKeyFromEnvironment();
      if (!keyBytes) {
        bool completionFailed = false;
        try {
          const IncidentCaptureCompletion unavailable = IncidentCaptureCompletionUnavailable{};
          auto completing = completeReplayIncidentEvent(*kv, incidentEvent, now, unavailable);
          co_await std::move(completing);
        } catch (...) {
          completionFailed = true;
        }
        if (completionFailed) warnDeferred("capture_completion_failed");
        // A missing key must be visible on its request, never a silently empty
        // replay history.
        auto recording = recordStatus(kv, input.request_id, "disabled", "key_missing", now);
        co_await std::move(recording);
        co_return SentinelReplayPersistResultDisabled{.reason = "key_missing"};
      }
      rt::Reason failure;
      try {
        PersistDependencies dependencies{
          .kv = kv,
          .keyBytes = *keyBytes,
          .now = [now] { return now; },
          .currentNow = [] { return rt::dateNow(); },
          .incidentEvent = incidentEvent,
        };
        auto persisting = persistEncryptedSentinelReplayTask(input, observation, std::make_shared<PersistDependencies>(dependencies),
                                                            resolvedClientObservation, policy);
        zero(dependencies.keyBytes);
        SentinelReplayPersistResult result = co_await std::move(persisting);
        if (const auto* incomplete = std::get_if<SentinelReplayPersistResultIncomplete>(&result)) {
          // Retention refused the capture without growing above budget: make the
          // skip visible on its request instead of looking like an empty history.
          auto recording = recordStatus(kv, input.request_id, "disabled", incomplete->reason, now);
          co_await std::move(recording);
        }
        co_return result;
      } catch (...) {
        failure = std::current_exception();
      }
      bool completionFailed = false;
      try {
        const IncidentCaptureCompletion unavailable = IncidentCaptureCompletionUnavailable{};
        auto completing = completeReplayIncidentEvent(*kv, incidentEvent, rt::dateNow(), unavailable);
        co_await std::move(completing);
      } catch (...) {
        completionFailed = true;
      }
      if (completionFailed) warnDeferred("capture_completion_failed");
      auto recording = recordStatus(kv, input.request_id, "failed", "persist_failed", now);
      co_await std::move(recording);
      std::rethrow_exception(failure);
    }
  }

  rt::Task<SentinelReplayPersistResult> persistSentinelReplayFromEnvironment(const AcceptedSentinelReplayInput& input, const SentinelFailureObservation& observation) {
    return persistSentinelReplayFromEnvironment(input, observation, std::nullopt);
  }

  rt::Task<SentinelReplayPersistResult> persistSentinelReplayFromEnvironment(const AcceptedSentinelReplayInput& input, const SentinelFailureObservation& observation,
                                                                             const std::optional<SentinelClientFailureObservation>& clientObservation) {
    return persistSentinelReplayFromEnvironmentTask(
      input, observation, clientObservation ? *clientObservation : replay_observation::resolveSentinelClientFailureObservation(observation));
  }

  rt::Task<SentinelReplayPersistResult> persistFailedRequestPayloadFromEnvironment(const AcceptedSentinelReplayInput& input,
                                                                                 const SentinelFailureObservation& observation,
                                                                                 const std::optional<SentinelClientFailureObservation>& clientObservation,
                                                                                 bool deliveryInterrupted) {
    return persistSentinelReplayFromEnvironmentTask(
      input, observation, clientObservation ? *clientObservation : replay_observation::resolveSentinelClientFailureObservation(observation),
      PersistencePolicy{.requestOnly = true, .deliveryInterrupted = deliveryInterrupted});
  }
}
