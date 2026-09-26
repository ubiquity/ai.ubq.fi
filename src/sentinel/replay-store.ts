// Sentinel replay persistence, split out of src/sentinel_replay_capture.ts.

import { getKv } from "../kv.ts";
import { emptySentinelUpstreamTrace, parseSentinelUpstreamTrace } from "./upstream-capture.ts";
import {
  bindSentinelIncidentIndexEvidence,
  completeSentinelIncidentFailureEvent,
  createSentinelIncidentFailureEventFromEnvironment,
  isSentinelIncidentIndexRow,
  readySentinelIncidentFailureEvent,
  recordSentinelIncidentIndexObservation,
  SENTINEL_INCIDENT_CAPTURE_REF_PREFIX,
  SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS,
  SENTINEL_INCIDENT_INDEX_PREFIX,
  SENTINEL_INCIDENT_TTL_MS,
  type SentinelIncidentCaptureReference,
  type SentinelIncidentFailureEvent,
  sentinelIncidentFingerprint,
  type SentinelIncidentIndexRow,
} from "./incident-outbox.ts";
import { base64UrlEncode } from "../utils.ts";
import type {
  AcceptedSentinelReplayInput,
  SentinelClientFailureObservation,
  SentinelFailureObservation,
  SentinelReplayCaptureOmissionReason,
  SentinelReplayCaptureStatus,
  SentinelReplayCaptureStatusRow,
  SentinelReplayManifest,
  SentinelReplayPlaintext,
} from "./replay-model.ts";
import {
  AES_GCM_IV_BYTES,
  ENVELOPE_VERSION,
  MAX_REPLAY_CIPHERTEXT_BYTES,
  REPLAY_KEY_BYTES,
  REPLAY_PLAINTEXT_VERSION,
  SENTINEL_REPLAY_CHUNK_PREFIX,
  SENTINEL_REPLAY_DEDUPE_PREFIX,
  SENTINEL_REPLAY_MANIFEST_PREFIX,
  SENTINEL_REPLAY_STATUS_TTL_MS,
  SENTINEL_REPLAY_TTL_MS,
  cloneBytes,
  concatBytes,
  randomBytes,
  requestStatusKey,
} from "./replay-model.ts";
import type { PersistDependencies, SentinelReplayPersistResult } from "./replay-observation.ts";
import {
  isSentinelReplayCaptureStatusRow,
  isSentinelReplayRequestId,
  resolveSentinelClientFailureObservation,
  sentinelFailureSignature,
  shouldPersistSentinelReplay,
  shouldSignalSentinelIncident,
  zeroSentinelReplayInput,
} from "./replay-observation.ts";
import type { ReplayMetadata } from "./replay-envelope.ts";
import {
  ciphertextDigest,
  decodeSentinelReplayKey,
  dedupeManifestKey,
  downstreamObservation,
  encodePlaintext,
  encryptionAdditionalData,
  fingerprintParts,
  gzip,
  hmacHex,
  importAesKey,
  isCompatibilityHeaders,
  replayCoverageFor,
  replayUnavailableReasons,
  requestSettingsFromBody,
  splitChunks,
} from "./replay-envelope.ts";
import { decryptExportedSentinelReplay, getChunks, isSentinelReplayManifest, manifestMatchesKey } from "./replay-read.ts";

const bindWinnerIndexEvidence = async (
  kv: Deno.Kv,
  indexFingerprint: string,
  observedAtMs: number,
  referenceFingerprint: string,
  manifestKey: Deno.KvKey,
  keyBytes: Uint8Array<ArrayBuffer>
): Promise<void> => {
  const manifestEntry = await kv.get<SentinelReplayManifest>(manifestKey);
  if (
    !manifestEntry.value ||
    !isSentinelReplayManifest(manifestEntry.value) ||
    manifestEntry.value.fingerprint !== referenceFingerprint ||
    !manifestMatchesKey(manifestKey, manifestEntry.value)
  )
    throw new Error("Sentinel incident replay manifest is unavailable");
  const chunks = await getChunks(kv, manifestEntry.value);
  let plaintext: SentinelReplayPlaintext | null = null;
  try {
    const digest = await ciphertextDigest(concatBytes(chunks));
    plaintext = await decryptExportedSentinelReplay({ manifest: manifestEntry.value, chunks: chunks.map(base64UrlEncode) }, keyBytes);
    await bindSentinelIncidentIndexEvidence(kv, indexFingerprint, {
      observedAtMs,
      captureId: manifestEntry.value.capture_id,
      gitSha: /^[0-9a-f]{40}$/.test(plaintext.git_sha) ? plaintext.git_sha : null,
      referenceFingerprint,
      manifestKey,
      manifestVersionstamp: manifestEntry.versionstamp,
      capturedAtMs: manifestEntry.value.captured_at_ms,
      digest,
      expiresAtMs: manifestEntry.value.expires_at_ms,
    });
  } finally {
    plaintext?.body.fill(0);
    for (const chunk of chunks) chunk.fill(0);
  }
};

const completeReplayIncidentEvent = async (
  kv: Deno.Kv,
  event: Deno.KvEntry<SentinelIncidentFailureEvent> | undefined,
  readyAtMs: number,
  capture: Readonly<{ status: "stored" | "duplicate"; fingerprint: string; manifestKey: Deno.KvKey }> | Readonly<{ status: "unavailable" }>
): Promise<void> => {
  if (!event) return;
  if (!(await completeSentinelIncidentFailureEvent(kv, event, readyAtMs, capture))) {
    throw new Error("Sentinel incident capture completion conflicted");
  }
};

const resolveIndexFingerprint = async (input: AcceptedSentinelReplayInput, clientObservation: SentinelClientFailureObservation): Promise<string | null> => {
  try {
    return await sentinelIncidentFingerprint({
      endpoint: input.endpoint,
      method: input.method,
      observation: clientObservation,
    });
  } catch {
    // The index is best-effort for direct callers: the environment producer
    // already recorded this observation before the key lookup.
    return null;
  }
};

/**
 * Settles a capture that already exists (or lost the CAS race): the durable
 * index row, when present, is bound to the winning capture, and the incident
 * outbox event is completed. Nothing pretends the duplicate created evidence.
 */
const completeDuplicateCapture = async (
  dependencies: PersistDependencies,
  duplicate: Readonly<{
    fingerprint: string;
    manifestKey: Deno.KvKey;
    indexKey: Deno.KvKey | null;
    indexFingerprint: string | null;
    requestId: string;
    captureStatus: "ready" | "incomplete";
    capturedAtMs: number;
    expiresAtMs: number;
  }>,
  now: number
): Promise<SentinelReplayPersistResult> => {
  if (duplicate.indexKey !== null && duplicate.indexFingerprint !== null) {
    const indexEntry = await dependencies.kv.get<SentinelIncidentIndexRow>(duplicate.indexKey);
    if (indexEntry.value !== null) {
      if (!isSentinelIncidentIndexRow(indexEntry.value)) {
        throw new Error("Sentinel incident index record is invalid");
      }
      await bindWinnerIndexEvidence(dependencies.kv, duplicate.indexFingerprint, now, duplicate.fingerprint, duplicate.manifestKey, dependencies.keyBytes);
    }
  }
  await completeReplayIncidentEvent(dependencies.kv, dependencies.incidentEvent, now, {
    status: "duplicate",
    fingerprint: duplicate.fingerprint,
    manifestKey: duplicate.manifestKey,
  });
  // The request still resolves to real evidence: keep its status row pointing
  // at the winning capture rather than leaving the request unaccounted for.
  await writeSentinelReplayCaptureStatus(
    dependencies.kv,
    captureStatusRow({
      requestId: duplicate.requestId,
      status: duplicate.captureStatus,
      reason: null,
      capturedAtMs: duplicate.capturedAtMs,
      manifestKey: duplicate.manifestKey,
      fingerprint: duplicate.fingerprint,
      expiresAtMs: duplicate.expiresAtMs,
    })
  ).catch(() => {});
  return { status: "duplicate", fingerprint: duplicate.fingerprint, manifest_key: duplicate.manifestKey };
};

/** Encrypts the replay plaintext envelope under the request key. */
const encryptReplayPlaintext = async (
  metadata: ReplayMetadata,
  bodySnapshot: Uint8Array<ArrayBuffer>,
  iv: Uint8Array<ArrayBuffer>,
  keyBytes: Uint8Array<ArrayBuffer>,
  fingerprint: string
): Promise<Uint8Array<ArrayBuffer>> => {
  const encodedPlaintext = encodePlaintext(metadata, bodySnapshot);
  let compressed: Uint8Array<ArrayBuffer>;
  try {
    compressed = await gzip(encodedPlaintext);
  } finally {
    encodedPlaintext.fill(0);
  }
  if (compressed.byteLength + 16 > MAX_REPLAY_CIPHERTEXT_BYTES) {
    compressed.fill(0);
    throw new Error("Sentinel replay compressed payload is too large");
  }
  try {
    return new Uint8Array(
      await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: encryptionAdditionalData(fingerprint) }, await importAesKey(keyBytes), compressed)
    );
  } finally {
    compressed.fill(0);
  }
};

const captureStatusRow = (
  input: Readonly<{
    requestId: string;
    status: SentinelReplayCaptureStatus;
    reason: string | null;
    capturedAtMs: number;
    manifestKey: Deno.KvKey | null;
    fingerprint: string | null;
    expiresAtMs: number | null;
  }>
): SentinelReplayCaptureStatusRow => {
  const row: SentinelReplayCaptureStatusRow = {
    version: 1,
    request_id: input.requestId,
    status: input.status,
    reason: input.reason,
    captured_at_ms: input.capturedAtMs,
    manifest_key: input.manifestKey === null ? null : [...input.manifestKey],
    fingerprint: input.fingerprint,
    expires_at_ms: input.expiresAtMs,
  };
  if (!isSentinelReplayCaptureStatusRow(row)) throw new Error("Sentinel replay capture status is invalid");
  return row;
};

/** Best-effort per-request status write; never replaces the caller's outcome. */
export const writeSentinelReplayCaptureStatus = async (kv: Deno.Kv, row: SentinelReplayCaptureStatusRow): Promise<void> => {
  if (!isSentinelReplayCaptureStatusRow(row)) throw new Error("Sentinel replay capture status is invalid");
  // The row outlives its payload so `expired` stays reportable; this is bounded
  // non-sensitive status metadata, never captured request or response content.
  await kv.set(requestStatusKey(row.request_id), row, { expireIn: SENTINEL_REPLAY_STATUS_TTL_MS });
};

/**
 * Publish the explicit per-request status for a request that produced no
 * encrypted payload: an accepted body the fixed cap could not carry, or an
 * authenticated request rejected before capture setup. Best effort by design —
 * the caller's response or exception is never replaced by a status failure.
 */
export const recordSentinelReplayOmissionFromEnvironment = async (
  requestId: string,
  reason: SentinelReplayCaptureOmissionReason,
  nowMs: number = Date.now()
): Promise<void> => {
  if (!isSentinelReplayRequestId(requestId)) return;
  try {
    const kv = await getKv();
    if (!kv) return;
    await writeSentinelReplayCaptureStatus(
      kv,
      captureStatusRow({
        requestId,
        status: "disabled",
        reason,
        capturedAtMs: nowMs,
        manifestKey: null,
        fingerprint: null,
        expiresAtMs: null,
      })
    );
  } catch {
    // Diagnostic only: a status row can never replace the real outcome.
  }
};

/** The dedupe/manifest/incident-event writes every store attempt starts from. */
const replayStoreOperation = (
  dependencies: PersistDependencies,
  dedupeKey: Deno.KvKey,
  manifestKey: Deno.KvKey,
  manifest: SentinelReplayManifest,
  fingerprint: string,
  now: number,
  status: Readonly<{ requestId: string; captureStatus: "ready" | "incomplete" }>
): Deno.AtomicOperation => {
  let operation = dependencies.kv
    .atomic()
    .check({ key: dedupeKey, versionstamp: null })
    .set(dedupeKey, { manifest_key: manifestKey }, { expireIn: SENTINEL_REPLAY_TTL_MS })
    .set(manifestKey, manifest, { expireIn: SENTINEL_REPLAY_TTL_MS })
    .set(
      requestStatusKey(status.requestId),
      captureStatusRow({
        requestId: status.requestId,
        status: status.captureStatus,
        reason: null,
        capturedAtMs: manifest.captured_at_ms,
        manifestKey,
        fingerprint,
        expiresAtMs: manifest.expires_at_ms,
      }),
      { expireIn: SENTINEL_REPLAY_STATUS_TTL_MS }
    );
  if (dependencies.incidentEvent) {
    const readyEvent = readySentinelIncidentFailureEvent(dependencies.incidentEvent, now, {
      status: "stored",
      fingerprint,
      manifestKey,
    });
    operation = operation
      .check({ key: dependencies.incidentEvent.key, versionstamp: dependencies.incidentEvent.versionstamp })
      .set(dependencies.incidentEvent.key, readyEvent, { expireIn: SENTINEL_INCIDENT_TTL_MS });
  }
  return operation;
};

/** Adds the evidence row and capture reference for an existing index observation. */
const withIncidentIndexEvidence = (
  operation: Deno.AtomicOperation,
  indexRow: Readonly<{ key: Deno.KvKey; entry: Deno.KvEntry<SentinelIncidentIndexRow> }>,
  context: Readonly<{
    gitSha: string;
    captureId: string;
    evidenceDigest: string;
    manifestKey: Deno.KvKey;
    capturedAtMs: number;
    expiresAtMs: number;
    now: number;
    fingerprint: string;
  }>
): Deno.AtomicOperation => {
  if (!isSentinelIncidentIndexRow(indexRow.entry.value)) {
    throw new Error("Sentinel incident index record is invalid");
  }
  const next: SentinelIncidentIndexRow = {
    ...indexRow.entry.value,
    failing_revision: /^[0-9a-f]{40}$/.test(context.gitSha) ? context.gitSha : null,
    provenance: { ...indexRow.entry.value.provenance, captured_at_ms: context.capturedAtMs },
    evidence_ref: { ref: `capture:${context.captureId}`, digest: context.evidenceDigest },
    evidence_expires_at_ms: context.expiresAtMs,
  };
  if (!isSentinelIncidentIndexRow(next)) throw new Error("Sentinel incident index record is invalid");
  const reference: SentinelIncidentCaptureReference = { version: 1, manifest_key: [...context.manifestKey] };
  return operation
    .check({ key: indexRow.key, versionstamp: indexRow.entry.versionstamp })
    .set([...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, next.incident_id, context.fingerprint], reference, { expireIn: context.expiresAtMs - context.now })
    .set(indexRow.key, next);
};

/** Reads the durable index observation once, when one exists. */
const readIncidentIndexEntry = async (
  kv: Deno.Kv,
  indexKey: Deno.KvKey | null
): Promise<Readonly<{ key: Deno.KvKey; entry: Deno.KvEntry<SentinelIncidentIndexRow> }> | null> => {
  if (indexKey === null) return null;
  const entry = await kv.get<SentinelIncidentIndexRow>(indexKey);
  return entry.value === null ? null : { key: indexKey, entry };
};

/** Writes the chunks, then commits the envelope with a bounded CAS retry loop. */
const storeReplayEnvelope = async (
  context: Readonly<{
    dependencies: PersistDependencies;
    gitSha: string;
    chunks: readonly Uint8Array<ArrayBuffer>[];
    dedupeKey: Deno.KvKey;
    manifestKey: Deno.KvKey;
    manifest: SentinelReplayManifest;
    indexKey: Deno.KvKey | null;
    indexFingerprint: string | null;
    captureId: string;
    evidenceDigest: string;
    now: number;
    expiresAtMs: number;
    requestId: string;
    captureStatus: "ready" | "incomplete";
  }>
): Promise<SentinelReplayPersistResult> => {
  const { dependencies, chunks, dedupeKey, manifestKey, manifest, indexKey, indexFingerprint, captureId, evidenceDigest, now, expiresAtMs } = context;
  const fingerprint = manifest.fingerprint;
  const cleanupChunks = async (): Promise<void> => {
    await Promise.all(chunks.map((_chunk, index) => dependencies.kv.delete([...SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index])));
  };
  try {
    await Promise.all(
      chunks.map((chunk, index) =>
        dependencies.kv.set([...SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index], chunk, {
          expireIn: SENTINEL_REPLAY_TTL_MS,
        })
      )
    );
    let committed: Deno.KvCommitResult | Deno.KvCommitError | null = null;
    for (let attempt = 0; attempt < SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS; attempt += 1) {
      const indexRow = await readIncidentIndexEntry(dependencies.kv, indexKey);
      let operation = replayStoreOperation(dependencies, dedupeKey, manifestKey, manifest, fingerprint, now, {
        requestId: context.requestId,
        captureStatus: context.captureStatus,
      });
      if (indexRow !== null) {
        operation = withIncidentIndexEvidence(operation, indexRow, {
          gitSha: context.gitSha,
          captureId,
          evidenceDigest,
          manifestKey,
          capturedAtMs: manifest.captured_at_ms,
          expiresAtMs,
          now,
          fingerprint,
        });
      }
      committed = await operation.commit();
      if (committed.ok) return { status: "stored", manifest, manifest_key: manifestKey };
    }
    await cleanupChunks().catch(() => {});
    const winningDedupe = await dependencies.kv.get(dedupeKey);
    const winningManifestKey = dedupeManifestKey(winningDedupe.value);
    if (!winningManifestKey) throw new Error("Sentinel replay dedupe winner is unavailable");
    const winningManifestEntry = await dependencies.kv.get<SentinelReplayManifest>(winningManifestKey);
    const winningExpiresAtMs =
      winningManifestEntry.value && typeof winningManifestEntry.value.expires_at_ms === "number"
        ? winningManifestEntry.value.expires_at_ms
        : expiresAtMs;
    return await completeDuplicateCapture(
      dependencies,
      {
        fingerprint,
        manifestKey: winningManifestKey,
        indexKey,
        indexFingerprint,
        requestId: context.requestId,
        captureStatus: context.captureStatus,
        capturedAtMs: manifest.captured_at_ms,
        expiresAtMs: winningExpiresAtMs,
      },
      now
    );
  } catch (error) {
    await cleanupChunks().catch(() => {});
    throw error;
  }
};

export const persistEncryptedSentinelReplay = async (
  input: AcceptedSentinelReplayInput,
  observation: SentinelFailureObservation,
  dependencies: PersistDependencies,
  clientObservation: SentinelClientFailureObservation = resolveSentinelClientFailureObservation(observation)
): Promise<SentinelReplayPersistResult> => {
  if (!shouldPersistSentinelReplay(observation, clientObservation)) {
    throw new Error("A successful request cannot be persisted as a sentinel replay");
  }
  if (dependencies.keyBytes.byteLength !== REPLAY_KEY_BYTES) throw new Error("Sentinel replay key must be 32 bytes");
  if (!isCompatibilityHeaders(input.compatibility_headers)) {
    throw new Error("Sentinel replay compatibility headers contain a disallowed value");
  }

  // Cancellation cleanup can zero the request-owned buffer while KV and
  // cryptographic operations are pending. One synchronous snapshot must feed
  // the digests and encrypted envelope so a capture cannot disagree with its
  // own manifest. Request-only callers emit the required empty upstream trace
  // with all truncation flags false; it is never labeled captured coverage.
  const upstreamTrace = input.upstream !== undefined ? parseSentinelUpstreamTrace(input.upstream) : emptySentinelUpstreamTrace();
  const bodySnapshot = cloneBytes(input.body);
  const snapshotInput: AcceptedSentinelReplayInput = { ...input, body: bodySnapshot };
  try {
    const now = dependencies.now?.() ?? Date.now();
    const failureSignature = sentinelFailureSignature(clientObservation);
    const fingerprint = await hmacHex(dependencies.keyBytes, "fingerprint", fingerprintParts(snapshotInput, failureSignature, "fingerprint", upstreamTrace));
    const caseGroupDigest = await hmacHex(dependencies.keyBytes, "case-group", fingerprintParts(snapshotInput, failureSignature, "case-group"));
    const unavailable = replayUnavailableReasons(upstreamTrace, clientObservation);
    const dedupeKey = [...SENTINEL_REPLAY_DEDUPE_PREFIX, fingerprint] as const;
    const indexFingerprint = await resolveIndexFingerprint(input, clientObservation);
    const indexKey: Deno.KvKey | null = indexFingerprint === null ? null : [...SENTINEL_INCIDENT_INDEX_PREFIX, indexFingerprint];
    const existingDedupe = await dependencies.kv.get(dedupeKey);
    if (existingDedupe.value !== null) {
      const manifestKey = dedupeManifestKey(existingDedupe.value);
      if (!manifestKey) throw new Error("Sentinel replay dedupe record is invalid");
      const winningManifestEntry = await dependencies.kv.get<SentinelReplayManifest>(manifestKey);
      const winningExpiresAtMs =
        winningManifestEntry.value && typeof winningManifestEntry.value.expires_at_ms === "number"
          ? winningManifestEntry.value.expires_at_ms
          : now + SENTINEL_REPLAY_TTL_MS;
      return await completeDuplicateCapture(
        dependencies,
        {
          fingerprint,
          manifestKey,
          indexKey,
          indexFingerprint,
          requestId: input.request_id,
          captureStatus: unavailable.length === 0 ? "ready" : "incomplete",
          capturedAtMs: now,
          expiresAtMs: winningExpiresAtMs,
        },
        now
      );
    }

    const captureId = dependencies.randomUuid?.() ?? crypto.randomUUID();
    const iv = dependencies.randomBytes?.(AES_GCM_IV_BYTES) ?? randomBytes(AES_GCM_IV_BYTES);
    if (iv.byteLength !== AES_GCM_IV_BYTES) throw new Error("Sentinel replay IV must be 12 bytes");
    const metadata: ReplayMetadata = {
      version: REPLAY_PLAINTEXT_VERSION,
      captured_at_ms: now,
      endpoint: input.endpoint,
      method: input.method,
      content_type: input.content_type,
      compatibility_headers: input.compatibility_headers,
      failure_signature: failureSignature,
      observation,
      client_observation: clientObservation,
      request_id: input.request_id,
      git_sha: input.git_sha,
      deno_revision: input.deno_revision,
      upstream: upstreamTrace,
      settings: requestSettingsFromBody(bodySnapshot, observation.provider_route, observation.stream),
      capture_status: unavailable.length === 0 ? "ready" : "incomplete",
      replay_coverage: replayCoverageFor(upstreamTrace, clientObservation, unavailable),
      unavailable,
      body_sha256: await ciphertextDigest(bodySnapshot),
      body_bytes: bodySnapshot.byteLength,
      downstream: downstreamObservation(clientObservation),
    };
    const encrypted = await encryptReplayPlaintext(metadata, bodySnapshot, iv, dependencies.keyBytes, fingerprint);
    try {
      const chunks = splitChunks(encrypted);
      const expiresAtMs = now + SENTINEL_REPLAY_TTL_MS;
      const manifest: SentinelReplayManifest = {
        version: ENVELOPE_VERSION,
        capture_id: captureId,
        fingerprint,
        case_group_digest: caseGroupDigest,
        captured_at_ms: now,
        expires_at_ms: expiresAtMs,
        algorithm: "AES-256-GCM",
        compression: "gzip",
        iv: base64UrlEncode(iv),
        chunk_count: chunks.length,
        ciphertext_bytes: encrypted.byteLength,
      };

      const manifestKey = [...SENTINEL_REPLAY_MANIFEST_PREFIX, now, fingerprint, captureId] as const;
      const evidenceDigest = await ciphertextDigest(encrypted);
      try {
        return await storeReplayEnvelope({
          dependencies,
          gitSha: input.git_sha,
          chunks,
          dedupeKey,
          manifestKey,
          manifest,
          indexKey,
          indexFingerprint,
          captureId,
          evidenceDigest,
          now,
          expiresAtMs,
          requestId: input.request_id,
          captureStatus: unavailable.length === 0 ? "ready" : "incomplete",
        });
      } finally {
        for (const chunk of chunks) chunk.fill(0);
      }
    } finally {
      encrypted.fill(0);
    }
  } finally {
    bodySnapshot.fill(0);
  }
};

const readReplayKeyFromEnvironment = (): Uint8Array<ArrayBuffer> | null => {
  try {
    const raw = Deno.env.get("SENTINEL_REPLAY_KEY")?.trim();
    return raw ? decodeSentinelReplayKey(raw) : null;
  } catch {
    return null;
  }
};

export const persistSentinelReplayFromEnvironment = async (
  input: AcceptedSentinelReplayInput,
  observation: SentinelFailureObservation,
  clientObservation?: SentinelClientFailureObservation
): Promise<SentinelReplayPersistResult> => {
  let keyBytes: Uint8Array<ArrayBuffer> | null = null;
  let kv: Deno.Kv | null;
  let incidentEvent: Deno.KvEntry<SentinelIncidentFailureEvent> | undefined;
  const resolvedClientObservation = clientObservation ?? resolveSentinelClientFailureObservation(observation);
  const now = Date.now();
  const recordStatus = async (status: "disabled" | "failed", reason: string, capturedAtMs: number): Promise<void> => {
    if (!kv || !isSentinelReplayRequestId(input.request_id)) return;
    try {
      await writeSentinelReplayCaptureStatus(
        kv,
        captureStatusRow({
          requestId: input.request_id,
          status,
          reason,
          capturedAtMs,
          manifestKey: null,
          fingerprint: null,
          expiresAtMs: null,
        })
      );
    } catch {
      // A status row is diagnostic: its own failure must never replace the real
      // persist outcome the caller already has.
    }
  };
  try {
    if (!shouldPersistSentinelReplay(observation, resolvedClientObservation)) {
      throw new Error("A successful request cannot be persisted as a sentinel replay");
    }
    kv = await getKv();
    if (!kv) return { status: "disabled", reason: "kv_unavailable" };
    if (shouldSignalSentinelIncident(observation, resolvedClientObservation)) {
      // The durable index observation is recorded BEFORE the key lookup and
      // encryption: even a missing key still leaves a discoverable incident
      // row with evidence_ref null. Unlike the transient event helper this
      // passive path is never environment-gated (it must work in the real
      // handler tests without a production deployment flag).
      try {
        await recordSentinelIncidentIndexObservation(kv, {
          endpoint: input.endpoint,
          method: input.method,
          gitSha: input.git_sha,
          observedAtMs: now,
          observation: resolvedClientObservation,
          classification: {
            provider: resolvedClientObservation.provider_route,
            model: null,
            reasoning: null,
            failure_kind: resolvedClientObservation.failure_kind,
          },
        });
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "index_write_failed" }));
      }
      try {
        incidentEvent = (await createSentinelIncidentFailureEventFromEnvironment(kv, now)) ?? undefined;
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "outbox_write_failed" }));
      }
    }
    keyBytes = readReplayKeyFromEnvironment();
    if (!keyBytes) {
      try {
        await completeReplayIncidentEvent(kv, incidentEvent, now, { status: "unavailable" });
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "capture_completion_failed" }));
      }
      // A missing key must be visible on its request, never a silently empty
      // replay history.
      await recordStatus("disabled", "key_missing", now);
      return { status: "disabled", reason: "key_missing" };
    }
    try {
      return await persistEncryptedSentinelReplay(input, observation, { kv, keyBytes, now: () => now, incidentEvent }, resolvedClientObservation);
    } catch (error) {
      try {
        await completeReplayIncidentEvent(kv, incidentEvent, Date.now(), { status: "unavailable" });
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "capture_completion_failed" }));
      }
      await recordStatus("failed", "persist_failed", now);
      throw error;
    }
  } finally {
    keyBytes?.fill(0);
    zeroSentinelReplayInput(input);
  }
};
