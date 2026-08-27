import {
  decodeSentinelAuthStateKey,
  decryptSentinelAuthStateArtifact,
  encryptSentinelAuthStateArtifact,
} from "./artifact-crypto.ts";
import { GitHubActionsClient } from "./github.ts";
import {
  type CodexAuthDocument,
  type CodexAuthSlot,
  type CodexUsageFailureCode,
  type CodexUsageProbeDependencies,
  parseCodexAuthJsonB64ForMaintenance,
  selectCodexAccountForInvocation,
} from "./quota.ts";
import { runChecked } from "./validation.ts";

const TEXT_ENCODER = new TextEncoder();
const FATAL_TEXT_DECODER = new TextDecoder("utf-8", { fatal: true });
const AUTH_STATE_PURPOSE = "sentinel_codex_auth_state";
const PREPARED_STATE_PURPOSE = "sentinel_codex_auth_state_prepared";
const AUTH_STATE_ARTIFACT_PREFIX = "sentinel-codex-auth-state-v1-";
const AUTH_STATE_MANIFEST_PATH = "manifest.json";
const PREPARED_STATE_CONTROL_PATH = "auth-state-control-v1.json";
const MAX_AUTH_JSON_BYTES = 128 * 1024;
const MAX_PREPARED_STATE_CONTROL_BYTES = 16 * 1024;
const AUTH_MAINTENANCE_INTERVAL_MS = 7 * 24 * 60 * 60 * 1_000;
const AUTH_EXPIRY_MAINTENANCE_MS = 24 * 60 * 60 * 1_000;

export const SENTINEL_CODEX_AUTH_STATE_ENVELOPE_FILENAME = "sentinel-codex-auth-state-v1.json";
export const SENTINEL_CODEX_AUTH_STATE_MAX_ARTIFACT_BYTES = 2 * 1024 * 1024;
export const SENTINEL_CODEX_AUTH_PROBE_MINIMUM_VALIDITY_MS = 50 * 60_000;

export type SentinelCodexAuthStateDocument = Readonly<{
  slot: CodexAuthSlot;
  rawJson: string;
  accountId: string;
  lastRefresh: string;
  lastRefreshMs: number;
  accessTokenExpiresAtMs: number;
}>;

export type SentinelCodexAuthStateArtifactMetadata = Readonly<{
  id: number;
  name: string;
  sizeInBytes: number;
  expired: boolean;
}>;

export type SentinelCodexAuthStateIdentity = Readonly<{
  generation: string;
  runId: number;
  runAttempt: number;
}>;

export type SentinelCodexAuthStateWriterIdentity = Readonly<{
  runId: number;
  runAttempt: number;
}>;

export type SelectedSentinelCodexAuthStateArtifact = Readonly<{
  artifact: SentinelCodexAuthStateArtifactMetadata;
  identity: SentinelCodexAuthStateIdentity;
}>;

export type SentinelCodexAuthStateSnapshot = Readonly<{
  repository: string;
  generation: string;
  runId: number;
  runAttempt: number;
  documents: readonly SentinelCodexAuthStateDocument[];
}>;

export type RestoredSentinelCodexAuthState = Readonly<{
  source: "artifact" | "bootstrap";
  sourceIdentity: SentinelCodexAuthStateIdentity | null;
  repository: string;
  generation: string;
  documents: readonly SentinelCodexAuthStateDocument[];
  needsMaintenance: boolean;
}>;

export type SentinelCodexAuthStateFailureCode =
  | "invalid_generation"
  | "invalid_repository"
  | "invalid_run_identity"
  | "invalid_artifact_name"
  | "invalid_artifact_metadata"
  | "ambiguous_artifact_identity"
  | "artifact_not_older_than_writer"
  | "artifact_expired"
  | "artifact_too_large"
  | "artifact_download_failed"
  | "artifact_invalid"
  | "seed_missing"
  | "seed_invalid"
  | "invalid_auth_document"
  | "account_identity_changed"
  | "invalid_manifest"
  | "manifest_identity_mismatch"
  | "invalid_state_directory"
  | "prepared_state_invalid"
  | "accounts_unavailable";

export class SentinelCodexAuthStateError extends Error {
  readonly code: SentinelCodexAuthStateFailureCode;

  constructor(code: SentinelCodexAuthStateFailureCode, options: ErrorOptions = {}) {
    super(`Sentinel Codex auth state stopped (${code}).`, options);
    this.name = "SentinelCodexAuthStateError";
    this.code = code;
  }
}

type ManifestSlot = Readonly<{
  slot: CodexAuthSlot;
  account_id_sha256: string;
}>;

type AuthStateManifest = Readonly<{
  schema_version: 1;
  purpose: typeof AUTH_STATE_PURPOSE;
  repository: string;
  generation: string;
  source_run_id: number;
  source_run_attempt: number;
  slots: readonly ManifestSlot[];
}>;

type PreparedStateControl = Readonly<{
  schema_version: 1;
  purpose: typeof PREPARED_STATE_PURPOSE;
  repository: string;
  generation: string;
  source: "artifact" | "bootstrap";
  source_run_id: number | null;
  source_run_attempt: number | null;
  slots: readonly ManifestSlot[];
}>;

type ExpectedAccountDigests = Readonly<Partial<Record<CodexAuthSlot, string>>>;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const hasExactKeys = (value: Record<string, unknown>, expected: readonly string[]): boolean =>
  Object.keys(value).sort().join("\u0000") === [...expected].sort().join("\u0000");

const positiveSafeInteger = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null;

const parsePositiveIntegerText = (value: string): number | null => {
  if (!/^[1-9][0-9]*$/u.test(value)) return null;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed > 0 ? parsed : null;
};

export const parseSentinelCodexAuthStateGeneration = (value: string): string => {
  if (
    value.length < 1 || value.length > 64 ||
    !/^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,62}[A-Za-z0-9])?$/u.test(value)
  ) {
    throw new SentinelCodexAuthStateError("invalid_generation");
  }
  return value;
};

const parseRepository = (value: string): string => {
  if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u.test(value)) {
    throw new SentinelCodexAuthStateError("invalid_repository");
  }
  return value;
};

const parseRunIdentity = (runId: number, runAttempt: number): Readonly<{ runId: number; runAttempt: number }> => {
  if (!positiveSafeInteger(runId) || !positiveSafeInteger(runAttempt)) {
    throw new SentinelCodexAuthStateError("invalid_run_identity");
  }
  return { runId, runAttempt };
};

export const assertSentinelCodexAuthStateArtifactPrecedesWriter = (
  artifactIdentity: SentinelCodexAuthStateIdentity,
  writerIdentity: SentinelCodexAuthStateWriterIdentity,
): void => {
  const artifact = parseRunIdentity(artifactIdentity.runId, artifactIdentity.runAttempt);
  const writer = parseRunIdentity(writerIdentity.runId, writerIdentity.runAttempt);
  if (
    artifact.runId > writer.runId ||
    (artifact.runId === writer.runId && artifact.runAttempt >= writer.runAttempt)
  ) {
    throw new SentinelCodexAuthStateError("artifact_not_older_than_writer");
  }
};

export const sentinelCodexAuthStateArtifactName = (identity: SentinelCodexAuthStateIdentity): string => {
  const generation = parseSentinelCodexAuthStateGeneration(identity.generation);
  const { runId, runAttempt } = parseRunIdentity(identity.runId, identity.runAttempt);
  return `${AUTH_STATE_ARTIFACT_PREFIX}${generation}-r${runId}-a${runAttempt}`;
};

export const parseSentinelCodexAuthStateArtifactName = (
  name: string,
): SentinelCodexAuthStateIdentity | null => {
  if (!name.startsWith(AUTH_STATE_ARTIFACT_PREFIX)) return null;
  const match = name.match(/^sentinel-codex-auth-state-v1-(.+)-r([1-9][0-9]*)-a([1-9][0-9]*)$/u);
  if (!match) throw new SentinelCodexAuthStateError("invalid_artifact_name");
  let generation: string;
  try {
    generation = parseSentinelCodexAuthStateGeneration(match[1]!);
  } catch {
    throw new SentinelCodexAuthStateError("invalid_artifact_name");
  }
  const runId = parsePositiveIntegerText(match[2]!);
  const runAttempt = parsePositiveIntegerText(match[3]!);
  if (runId === null || runAttempt === null) throw new SentinelCodexAuthStateError("invalid_artifact_name");
  return { generation, runId, runAttempt };
};

const validateArtifactMetadata = (
  artifact: SentinelCodexAuthStateArtifactMetadata,
): SentinelCodexAuthStateArtifactMetadata => {
  if (
    !positiveSafeInteger(artifact.id) || typeof artifact.name !== "string" ||
    typeof artifact.sizeInBytes !== "number" || !Number.isSafeInteger(artifact.sizeInBytes) ||
    artifact.sizeInBytes < 0 || typeof artifact.expired !== "boolean"
  ) {
    throw new SentinelCodexAuthStateError("invalid_artifact_metadata");
  }
  return artifact;
};

export const selectNewestSentinelCodexAuthStateArtifact = (
  artifacts: readonly SentinelCodexAuthStateArtifactMetadata[],
  expectedGeneration: string,
): SelectedSentinelCodexAuthStateArtifact | null => {
  const generation = parseSentinelCodexAuthStateGeneration(expectedGeneration);
  const matching: SelectedSentinelCodexAuthStateArtifact[] = [];
  for (const candidate of artifacts) {
    if (typeof candidate.name !== "string") throw new SentinelCodexAuthStateError("invalid_artifact_metadata");
    const identity = parseSentinelCodexAuthStateArtifactName(candidate.name);
    if (identity === null || identity.generation !== generation) continue;
    matching.push({ artifact: validateArtifactMetadata(candidate), identity });
  }
  matching.sort((left, right) =>
    right.identity.runId - left.identity.runId || right.identity.runAttempt - left.identity.runAttempt ||
    right.artifact.id - left.artifact.id
  );
  for (let index = 1; index < matching.length; index++) {
    const previous = matching[index - 1]!;
    const current = matching[index]!;
    if (
      previous.identity.runId === current.identity.runId &&
      previous.identity.runAttempt === current.identity.runAttempt
    ) {
      throw new SentinelCodexAuthStateError("ambiguous_artifact_identity");
    }
  }
  return matching[0] ?? null;
};

const encodeStandardBase64 = (value: Uint8Array): string => {
  let binary = "";
  for (let offset = 0; offset < value.byteLength; offset += 0x8000) {
    binary += String.fromCharCode(...value.subarray(offset, offset + 0x8000));
  }
  return btoa(binary);
};

const documentFromParsed = (parsed: CodexAuthDocument): SentinelCodexAuthStateDocument => ({
  slot: parsed.slot,
  rawJson: parsed.rawJson,
  accountId: parsed.tokens.account_id,
  lastRefresh: parsed.lastRefresh,
  lastRefreshMs: Date.parse(parsed.lastRefresh),
  accessTokenExpiresAtMs: parsed.accessTokenExpiresAtMs,
});

export const parseSentinelCodexAuthStateDocument = (
  rawJson: string,
  slot: CodexAuthSlot,
): SentinelCodexAuthStateDocument => {
  const bytes = TEXT_ENCODER.encode(rawJson);
  if (bytes.byteLength < 1 || bytes.byteLength > MAX_AUTH_JSON_BYTES) {
    bytes.fill(0);
    throw new SentinelCodexAuthStateError("invalid_auth_document");
  }
  try {
    return documentFromParsed(parseCodexAuthJsonB64ForMaintenance(encodeStandardBase64(bytes), slot));
  } catch (error) {
    throw new SentinelCodexAuthStateError("invalid_auth_document", { cause: error });
  } finally {
    bytes.fill(0);
  }
};

const documentFromSeed = (encoded: string, slot: CodexAuthSlot): SentinelCodexAuthStateDocument => {
  try {
    const parsed = parseCodexAuthJsonB64ForMaintenance(encoded, slot);
    return parseSentinelCodexAuthStateDocument(parsed.rawJson, slot);
  } catch (error) {
    throw new SentinelCodexAuthStateError("seed_invalid", { cause: error });
  }
};

const sha256Hex = async (value: string): Promise<string> =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", TEXT_ENCODER.encode(value))))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");

export const sentinelCodexAuthAccountDigest = (accountId: string): Promise<string> =>
  sha256Hex(`uos-sentinel-codex-auth-account-v1\u0000${accountId}`);

const parseManifestSlot = (value: unknown): ManifestSlot => {
  if (
    !isRecord(value) || !hasExactKeys(value, ["account_id_sha256", "slot"]) ||
    (value.slot !== 1 && value.slot !== 2) || typeof value.account_id_sha256 !== "string" ||
    !/^[0-9a-f]{64}$/u.test(value.account_id_sha256)
  ) {
    throw new SentinelCodexAuthStateError("invalid_manifest");
  }
  return { slot: value.slot, account_id_sha256: value.account_id_sha256 };
};

const parseManifestSlots = (value: unknown): readonly ManifestSlot[] => {
  if (!Array.isArray(value) || value.length < 1 || value.length > 2) {
    throw new SentinelCodexAuthStateError("invalid_manifest");
  }
  const slots = value.map(parseManifestSlot);
  for (let index = 0; index < slots.length; index++) {
    if (slots[index]!.slot !== index + 1 && (index > 0 || slots[index]!.slot !== 2)) {
      throw new SentinelCodexAuthStateError("invalid_manifest");
    }
    if (index > 0 && slots[index - 1]!.slot >= slots[index]!.slot) {
      throw new SentinelCodexAuthStateError("invalid_manifest");
    }
  }
  return slots;
};

const parseAuthStateManifest = (bytes: Uint8Array<ArrayBuffer>): AuthStateManifest => {
  let value: unknown;
  try {
    value = JSON.parse(FATAL_TEXT_DECODER.decode(bytes));
  } catch (error) {
    throw new SentinelCodexAuthStateError("invalid_manifest", { cause: error });
  }
  if (
    !isRecord(value) ||
    !hasExactKeys(value, [
      "generation",
      "purpose",
      "repository",
      "schema_version",
      "slots",
      "source_run_attempt",
      "source_run_id",
    ]) ||
    value.schema_version !== 1 || value.purpose !== AUTH_STATE_PURPOSE ||
    typeof value.repository !== "string" || typeof value.generation !== "string"
  ) {
    throw new SentinelCodexAuthStateError("invalid_manifest");
  }
  let repository: string;
  let generation: string;
  try {
    repository = parseRepository(value.repository);
    generation = parseSentinelCodexAuthStateGeneration(value.generation);
  } catch (error) {
    throw new SentinelCodexAuthStateError("invalid_manifest", { cause: error });
  }
  const runId = positiveSafeInteger(value.source_run_id);
  const runAttempt = positiveSafeInteger(value.source_run_attempt);
  if (runId === null || runAttempt === null) throw new SentinelCodexAuthStateError("invalid_manifest");
  return {
    schema_version: 1,
    purpose: AUTH_STATE_PURPOSE,
    repository,
    generation,
    source_run_id: runId,
    source_run_attempt: runAttempt,
    slots: parseManifestSlots(value.slots),
  };
};

const normalizeDocuments = async (
  documents: readonly SentinelCodexAuthStateDocument[],
  expectedAccountDigests: ExpectedAccountDigests = {},
): Promise<Readonly<{ documents: readonly SentinelCodexAuthStateDocument[]; slots: readonly ManifestSlot[] }>> => {
  if (documents.length < 1 || documents.length > 2) {
    throw new SentinelCodexAuthStateError("invalid_auth_document");
  }
  const normalized = documents
    .map((document) => parseSentinelCodexAuthStateDocument(document.rawJson, document.slot))
    .sort((left, right) => left.slot - right.slot);
  if (normalized.length === 2 && normalized[0]!.slot === normalized[1]!.slot) {
    throw new SentinelCodexAuthStateError("invalid_auth_document");
  }
  const slots: ManifestSlot[] = [];
  for (const document of normalized) {
    const digest = await sentinelCodexAuthAccountDigest(document.accountId);
    const expected = expectedAccountDigests[document.slot];
    if (expected !== undefined && expected !== digest) {
      throw new SentinelCodexAuthStateError("account_identity_changed");
    }
    slots.push({ slot: document.slot, account_id_sha256: digest });
  }
  for (const slot of [1, 2] as const) {
    if (expectedAccountDigests[slot] !== undefined && !normalized.some((document) => document.slot === slot)) {
      throw new SentinelCodexAuthStateError("account_identity_changed");
    }
  }
  return { documents: normalized, slots };
};

export const serializeSentinelCodexAuthState = async (
  input: Readonly<{
    repository: string;
    generation: string;
    runId: number;
    runAttempt: number;
    documents: readonly SentinelCodexAuthStateDocument[];
    expectedAccountDigests?: ExpectedAccountDigests;
  }>,
  keyBytes: Uint8Array<ArrayBuffer>,
  suppliedIv?: Uint8Array<ArrayBuffer>,
): Promise<Uint8Array<ArrayBuffer>> => {
  const repository = parseRepository(input.repository);
  const generation = parseSentinelCodexAuthStateGeneration(input.generation);
  const { runId, runAttempt } = parseRunIdentity(input.runId, input.runAttempt);
  const normalized = await normalizeDocuments(input.documents, input.expectedAccountDigests);
  const manifest: AuthStateManifest = {
    schema_version: 1,
    purpose: AUTH_STATE_PURPOSE,
    repository,
    generation,
    source_run_id: runId,
    source_run_attempt: runAttempt,
    slots: normalized.slots,
  };
  const files = [
    { path: AUTH_STATE_MANIFEST_PATH, bytes: TEXT_ENCODER.encode(JSON.stringify(manifest)) },
    ...normalized.documents.map((document) => ({
      path: `slots/${document.slot}/auth.json`,
      bytes: TEXT_ENCODER.encode(document.rawJson),
    })),
  ];
  try {
    return await encryptSentinelAuthStateArtifact(files, keyBytes, suppliedIv);
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
};

export const deserializeSentinelCodexAuthState = async (
  encryptedBytes: Uint8Array<ArrayBuffer>,
  keyBytes: Uint8Array<ArrayBuffer>,
  expected: Readonly<{
    repository: string;
    generation: string;
    runId: number;
    runAttempt: number;
  }>,
): Promise<SentinelCodexAuthStateSnapshot> => {
  const expectedRepository = parseRepository(expected.repository);
  const expectedGeneration = parseSentinelCodexAuthStateGeneration(expected.generation);
  const expectedIdentity = parseRunIdentity(expected.runId, expected.runAttempt);
  const files = await decryptSentinelAuthStateArtifact(encryptedBytes, keyBytes);
  try {
    const manifestFile = files.find((file) => file.path === AUTH_STATE_MANIFEST_PATH);
    if (!manifestFile) throw new SentinelCodexAuthStateError("invalid_manifest");
    const manifest = parseAuthStateManifest(manifestFile.bytes);
    if (
      manifest.repository !== expectedRepository || manifest.generation !== expectedGeneration ||
      manifest.source_run_id !== expectedIdentity.runId ||
      manifest.source_run_attempt !== expectedIdentity.runAttempt
    ) {
      throw new SentinelCodexAuthStateError("manifest_identity_mismatch");
    }
    const expectedPaths = new Set([
      AUTH_STATE_MANIFEST_PATH,
      ...manifest.slots.map((slot) => `slots/${slot.slot}/auth.json`),
    ]);
    if (files.length !== expectedPaths.size || files.some((file) => !expectedPaths.has(file.path))) {
      throw new SentinelCodexAuthStateError("invalid_manifest");
    }
    const documents: SentinelCodexAuthStateDocument[] = [];
    for (const manifestSlot of manifest.slots) {
      const file = files.find((candidate) => candidate.path === `slots/${manifestSlot.slot}/auth.json`);
      if (!file) throw new SentinelCodexAuthStateError("invalid_manifest");
      let rawJson: string;
      try {
        rawJson = FATAL_TEXT_DECODER.decode(file.bytes);
      } catch (error) {
        throw new SentinelCodexAuthStateError("invalid_auth_document", { cause: error });
      }
      const document = parseSentinelCodexAuthStateDocument(rawJson, manifestSlot.slot);
      if (await sentinelCodexAuthAccountDigest(document.accountId) !== manifestSlot.account_id_sha256) {
        throw new SentinelCodexAuthStateError("account_identity_changed");
      }
      documents.push(document);
    }
    return {
      repository: manifest.repository,
      generation: manifest.generation,
      runId: manifest.source_run_id,
      runAttempt: manifest.source_run_attempt,
      documents,
    };
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
};

export const sentinelCodexAuthStateNeedsMaintenance = (
  documents: readonly SentinelCodexAuthStateDocument[],
  nowMs = Date.now(),
): boolean => {
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new TypeError("nowMs is invalid");
  return documents.some((document) =>
    document.lastRefreshMs + AUTH_MAINTENANCE_INTERVAL_MS <= nowMs ||
    document.accessTokenExpiresAtMs <= nowMs + AUTH_EXPIRY_MAINTENANCE_MS
  );
};

export const restoreSentinelCodexAuthState = async (
  input: Readonly<{
    artifacts: readonly SentinelCodexAuthStateArtifactMetadata[];
    repository: string;
    generation: string;
    currentWriterIdentity: SentinelCodexAuthStateWriterIdentity;
    keyBytes: Uint8Array<ArrayBuffer>;
    seeds?: Readonly<{ slot1B64?: string; slot2B64?: string }>;
    downloadEnvelope: (
      artifact: SentinelCodexAuthStateArtifactMetadata,
      maximumBytes: number,
    ) => Promise<Uint8Array<ArrayBuffer>>;
    nowMs?: number;
  }>,
): Promise<RestoredSentinelCodexAuthState> => {
  const repository = parseRepository(input.repository);
  const generation = parseSentinelCodexAuthStateGeneration(input.generation);
  const currentWriterIdentity = parseRunIdentity(
    input.currentWriterIdentity.runId,
    input.currentWriterIdentity.runAttempt,
  );
  const selected = selectNewestSentinelCodexAuthStateArtifact(input.artifacts, generation);
  if (selected === null) {
    const documents: SentinelCodexAuthStateDocument[] = [];
    if (input.seeds?.slot1B64) documents.push(documentFromSeed(input.seeds.slot1B64, 1));
    if (input.seeds?.slot2B64) documents.push(documentFromSeed(input.seeds.slot2B64, 2));
    if (documents.length === 0) throw new SentinelCodexAuthStateError("seed_missing");
    return {
      source: "bootstrap",
      sourceIdentity: null,
      repository,
      generation,
      documents,
      needsMaintenance: true,
    };
  }
  assertSentinelCodexAuthStateArtifactPrecedesWriter(selected.identity, currentWriterIdentity);
  if (selected.artifact.expired) throw new SentinelCodexAuthStateError("artifact_expired");
  if (selected.artifact.sizeInBytes > SENTINEL_CODEX_AUTH_STATE_MAX_ARTIFACT_BYTES) {
    throw new SentinelCodexAuthStateError("artifact_too_large");
  }
  let encrypted: Uint8Array<ArrayBuffer>;
  try {
    encrypted = await input.downloadEnvelope(selected.artifact, SENTINEL_CODEX_AUTH_STATE_MAX_ARTIFACT_BYTES);
  } catch (error) {
    throw new SentinelCodexAuthStateError("artifact_download_failed", { cause: error });
  }
  try {
    if (encrypted.byteLength > SENTINEL_CODEX_AUTH_STATE_MAX_ARTIFACT_BYTES) {
      throw new SentinelCodexAuthStateError("artifact_too_large");
    }
    let snapshot: SentinelCodexAuthStateSnapshot;
    try {
      snapshot = await deserializeSentinelCodexAuthState(encrypted, input.keyBytes, {
        repository,
        generation,
        runId: selected.identity.runId,
        runAttempt: selected.identity.runAttempt,
      });
    } catch (error) {
      if (error instanceof SentinelCodexAuthStateError && error.code === "artifact_too_large") throw error;
      throw new SentinelCodexAuthStateError("artifact_invalid", { cause: error });
    }
    return {
      source: "artifact",
      sourceIdentity: selected.identity,
      repository,
      generation,
      documents: snapshot.documents,
      needsMaintenance: sentinelCodexAuthStateNeedsMaintenance(snapshot.documents, input.nowMs),
    };
  } finally {
    encrypted.fill(0);
  }
};

const absolutePrivateDirectory = (value: string): string => {
  if (!value.startsWith("/") || value.includes("\u0000") || value.includes("\n") || value.endsWith("/")) {
    throw new SentinelCodexAuthStateError("invalid_state_directory");
  }
  return value;
};

const controlForRestoredState = async (restored: RestoredSentinelCodexAuthState): Promise<PreparedStateControl> => ({
  schema_version: 1,
  purpose: PREPARED_STATE_PURPOSE,
  repository: restored.repository,
  generation: restored.generation,
  source: restored.source,
  source_run_id: restored.sourceIdentity?.runId ?? null,
  source_run_attempt: restored.sourceIdentity?.runAttempt ?? null,
  slots: await Promise.all(restored.documents.map(async (document) => ({
    slot: document.slot,
    account_id_sha256: await sentinelCodexAuthAccountDigest(document.accountId),
  }))),
});

export const writePreparedSentinelCodexAuthState = async (
  stateDirectory: string,
  restored: RestoredSentinelCodexAuthState,
): Promise<void> => {
  const directory = absolutePrivateDirectory(stateDirectory);
  let created = false;
  try {
    await Deno.mkdir(directory, { mode: 0o700 });
    created = true;
    for (const document of restored.documents) {
      const slotDirectory = `${directory}/slots/${document.slot}`;
      await Deno.mkdir(slotDirectory, { recursive: true, mode: 0o700 });
      await Deno.writeTextFile(`${slotDirectory}/auth.json`, document.rawJson, { createNew: true, mode: 0o600 });
    }
    await Deno.writeTextFile(
      `${directory}/${PREPARED_STATE_CONTROL_PATH}`,
      JSON.stringify(await controlForRestoredState(restored)),
      { createNew: true, mode: 0o600 },
    );
  } catch (error) {
    if (created) await Deno.remove(directory, { recursive: true }).catch(() => undefined);
    throw error;
  }
};

const parsePreparedStateControl = (raw: string): PreparedStateControl => {
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch (error) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid", { cause: error });
  }
  if (
    !isRecord(value) ||
    !hasExactKeys(value, [
      "generation",
      "purpose",
      "repository",
      "schema_version",
      "slots",
      "source",
      "source_run_attempt",
      "source_run_id",
    ]) ||
    value.schema_version !== 1 || value.purpose !== PREPARED_STATE_PURPOSE ||
    typeof value.repository !== "string" || typeof value.generation !== "string" ||
    (value.source !== "artifact" && value.source !== "bootstrap")
  ) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid");
  }
  let repository: string;
  let generation: string;
  try {
    repository = parseRepository(value.repository);
    generation = parseSentinelCodexAuthStateGeneration(value.generation);
  } catch (error) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid", { cause: error });
  }
  if (
    value.source === "bootstrap" &&
    (value.source_run_id !== null || value.source_run_attempt !== null)
  ) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid");
  }
  const sourceRunId = value.source_run_id === null ? null : positiveSafeInteger(value.source_run_id);
  const sourceRunAttempt = value.source_run_attempt === null ? null : positiveSafeInteger(value.source_run_attempt);
  if (
    (value.source === "bootstrap" && (sourceRunId !== null || sourceRunAttempt !== null)) ||
    (value.source === "artifact" && (sourceRunId === null || sourceRunAttempt === null))
  ) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid");
  }
  let slots: readonly ManifestSlot[];
  try {
    slots = parseManifestSlots(value.slots);
  } catch (error) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid", { cause: error });
  }
  return {
    schema_version: 1,
    purpose: PREPARED_STATE_PURPOSE,
    repository,
    generation,
    source: value.source,
    source_run_id: sourceRunId,
    source_run_attempt: sourceRunAttempt,
    slots,
  };
};

const readPreparedControl = async (stateDirectory: string): Promise<PreparedStateControl> => {
  const directory = absolutePrivateDirectory(stateDirectory);
  try {
    const directoryInfo = await Deno.lstat(directory);
    if (!directoryInfo.isDirectory || directoryInfo.isSymlink) {
      throw new SentinelCodexAuthStateError("prepared_state_invalid");
    }
    return parsePreparedStateControl(
      await readBoundedRegularTextFile(
        `${directory}/${PREPARED_STATE_CONTROL_PATH}`,
        MAX_PREPARED_STATE_CONTROL_BYTES,
      ),
    );
  } catch (error) {
    if (error instanceof SentinelCodexAuthStateError) throw error;
    throw new SentinelCodexAuthStateError("prepared_state_invalid", { cause: error });
  }
};

const readBoundedRegularTextFile = async (path: string, maximumBytes: number): Promise<string> => {
  const info = await Deno.lstat(path);
  if (
    !info.isFile || info.isSymlink || !Number.isSafeInteger(info.size) ||
    info.size < 1 || info.size > maximumBytes
  ) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid");
  }
  const raw = await Deno.readTextFile(path);
  const bytes = TEXT_ENCODER.encode(raw);
  try {
    if (bytes.byteLength < 1 || bytes.byteLength > maximumBytes) {
      throw new SentinelCodexAuthStateError("prepared_state_invalid");
    }
  } finally {
    bytes.fill(0);
  }
  return raw;
};

export type SentinelCodexAuthReadinessCode =
  | "ready"
  | "missing"
  | "invalid_document"
  | "account_identity_changed";

export type SentinelCodexAuthProbeSlotResult = Readonly<{
  slot: CodexAuthSlot;
  code: "available" | CodexUsageFailureCode | SentinelCodexAuthReadinessCode;
  httpStatus: number | null;
  headroomPercent: number | null;
}>;

export type SentinelCodexAuthProbeResult = Readonly<{
  usable: boolean;
  selectedSlot: CodexAuthSlot | null;
  slots: readonly SentinelCodexAuthProbeSlotResult[];
}>;

export const readPreparedSentinelCodexAuthState = async (
  stateDirectory: string,
): Promise<
  Readonly<{
    control: PreparedStateControl;
    documents: readonly SentinelCodexAuthStateDocument[];
    readiness: Readonly<Partial<Record<CodexAuthSlot, SentinelCodexAuthReadinessCode>>>;
  }>
> => {
  const directory = absolutePrivateDirectory(stateDirectory);
  const control = await readPreparedControl(directory);
  const documents: SentinelCodexAuthStateDocument[] = [];
  const readiness: Partial<Record<CodexAuthSlot, SentinelCodexAuthReadinessCode>> = {};
  for (const expected of control.slots) {
    let rawJson: string;
    try {
      for (const expectedDirectory of [`${directory}/slots`, `${directory}/slots/${expected.slot}`]) {
        const info = await Deno.lstat(expectedDirectory);
        if (!info.isDirectory || info.isSymlink) {
          throw new SentinelCodexAuthStateError("prepared_state_invalid");
        }
      }
      rawJson = await readBoundedRegularTextFile(
        `${directory}/slots/${expected.slot}/auth.json`,
        MAX_AUTH_JSON_BYTES,
      );
    } catch (error) {
      readiness[expected.slot] = error instanceof Deno.errors.NotFound ? "missing" : "invalid_document";
      continue;
    }
    let document: SentinelCodexAuthStateDocument;
    try {
      document = parseSentinelCodexAuthStateDocument(rawJson, expected.slot);
    } catch {
      readiness[expected.slot] = "invalid_document";
      continue;
    }
    if (await sentinelCodexAuthAccountDigest(document.accountId) !== expected.account_id_sha256) {
      readiness[expected.slot] = "account_identity_changed";
      continue;
    }
    readiness[expected.slot] = "ready";
    documents.push(document);
  }
  return { control, documents, readiness };
};

const encodeAuthDocumentForProbe = (rawJson: string): string => {
  const bytes = TEXT_ENCODER.encode(rawJson);
  try {
    return encodeStandardBase64(bytes);
  } finally {
    bytes.fill(0);
  }
};

export const probeSentinelCodexAuthDocuments = async (
  documents: readonly SentinelCodexAuthStateDocument[],
  dependencies: CodexUsageProbeDependencies = {},
): Promise<SentinelCodexAuthProbeResult> => {
  if (documents.length > 2) throw new SentinelCodexAuthStateError("invalid_auth_document");
  const encodedBySlot: Partial<Record<CodexAuthSlot, string>> = {};
  for (const document of documents) {
    const parsed = parseSentinelCodexAuthStateDocument(document.rawJson, document.slot);
    if (encodedBySlot[parsed.slot] !== undefined) {
      throw new SentinelCodexAuthStateError("invalid_auth_document");
    }
    encodedBySlot[parsed.slot] = encodeAuthDocumentForProbe(parsed.rawJson);
  }
  const selection = await selectCodexAccountForInvocation({
    ...dependencies,
    slots: { slot1B64: encodedBySlot[1], slot2B64: encodedBySlot[2] },
    model: null,
    minimumValidityMs: SENTINEL_CODEX_AUTH_PROBE_MINIMUM_VALIDITY_MS,
  });
  const slots: SentinelCodexAuthProbeSlotResult[] = selection.probes.map((probe) =>
    probe.kind === "available"
      ? {
        slot: probe.slot,
        code: "available",
        httpStatus: 200,
        headroomPercent: probe.headroomPercent,
      }
      : {
        slot: probe.slot,
        code: probe.failure,
        httpStatus: probe.status,
        headroomPercent: null,
      }
  );
  return {
    usable: selection.kind === "selected",
    selectedSlot: selection.kind === "selected" ? selection.slot : null,
    slots,
  };
};

export const probePreparedSentinelCodexAuthState = async (
  stateDirectory: string,
  dependencies: CodexUsageProbeDependencies = {},
): Promise<SentinelCodexAuthProbeResult> => {
  const prepared = await readPreparedSentinelCodexAuthState(stateDirectory);
  const probed = await probeSentinelCodexAuthDocuments(prepared.documents, dependencies);
  const slots = probed.slots.map((result): SentinelCodexAuthProbeSlotResult => {
    const readiness = prepared.readiness[result.slot];
    return readiness === undefined || readiness === "ready" ? result : {
      slot: result.slot,
      code: readiness,
      httpStatus: null,
      headroomPercent: null,
    };
  });
  const selectedSlot = probed.selectedSlot !== null &&
      slots.some((result) => result.slot === probed.selectedSlot && result.code === "available")
    ? probed.selectedSlot
    : null;
  return { usable: selectedSlot !== null, selectedSlot, slots };
};

export const sealPreparedSentinelCodexAuthState = async (
  stateDirectory: string,
  identity: Readonly<{ repository: string; generation: string; runId: number; runAttempt: number }>,
  keyBytes: Uint8Array<ArrayBuffer>,
  suppliedIv?: Uint8Array<ArrayBuffer>,
): Promise<Readonly<{ artifactName: string; encrypted: Uint8Array<ArrayBuffer>; slotCount: number }>> => {
  const prepared = await readPreparedSentinelCodexAuthState(stateDirectory);
  const repository = parseRepository(identity.repository);
  const generation = parseSentinelCodexAuthStateGeneration(identity.generation);
  const writerIdentity = parseRunIdentity(identity.runId, identity.runAttempt);
  if (
    prepared.control.repository !== repository ||
    prepared.control.generation !== generation ||
    prepared.control.slots.some((slot) => prepared.readiness[slot.slot] !== "ready")
  ) {
    throw new SentinelCodexAuthStateError("prepared_state_invalid");
  }
  if (prepared.control.source === "artifact") {
    assertSentinelCodexAuthStateArtifactPrecedesWriter(
      {
        generation,
        runId: prepared.control.source_run_id!,
        runAttempt: prepared.control.source_run_attempt!,
      },
      writerIdentity,
    );
  }
  const expectedAccountDigests: Partial<Record<CodexAuthSlot, string>> = {};
  for (const slot of prepared.control.slots) expectedAccountDigests[slot.slot] = slot.account_id_sha256;
  const encrypted = await serializeSentinelCodexAuthState(
    {
      repository: identity.repository,
      generation: identity.generation,
      runId: writerIdentity.runId,
      runAttempt: writerIdentity.runAttempt,
      documents: prepared.documents,
      expectedAccountDigests,
    },
    keyBytes,
    suppliedIv,
  );
  return {
    artifactName: sentinelCodexAuthStateArtifactName(identity),
    encrypted,
    slotCount: prepared.documents.length,
  };
};

const unzipAuthStateEnvelope = async (
  archiveBytes: Uint8Array<ArrayBuffer>,
): Promise<Uint8Array<ArrayBuffer>> => {
  if (archiveBytes.byteLength > SENTINEL_CODEX_AUTH_STATE_MAX_ARTIFACT_BYTES) {
    throw new SentinelCodexAuthStateError("artifact_too_large");
  }
  const runnerTemp = absolutePrivateDirectory(requiredEnvironment("RUNNER_TEMP"));
  const directory = await Deno.makeTempDir({
    dir: runnerTemp,
    prefix: "sentinel-auth-state-artifact-",
  });
  await Deno.chmod(directory, 0o700);
  const archivePath = `${directory}/artifact.zip`;
  try {
    await Deno.writeFile(archivePath, archiveBytes, { createNew: true, mode: 0o600 });
    const listing = await runChecked({
      command: "unzip",
      args: ["-Z1", archivePath],
      cwd: directory,
      maximumOutputBytes: 16 * 1024,
    });
    const entries = FATAL_TEXT_DECODER.decode(listing.stdout).split(/\r?\n/u).filter(Boolean);
    if (entries.length !== 1 || entries[0] !== SENTINEL_CODEX_AUTH_STATE_ENVELOPE_FILENAME) {
      throw new SentinelCodexAuthStateError("artifact_invalid");
    }
    const extracted = await runChecked({
      command: "unzip",
      args: ["-p", archivePath, SENTINEL_CODEX_AUTH_STATE_ENVELOPE_FILENAME],
      cwd: directory,
      maximumOutputBytes: SENTINEL_CODEX_AUTH_STATE_MAX_ARTIFACT_BYTES,
    });
    return extracted.stdout;
  } catch (error) {
    if (error instanceof SentinelCodexAuthStateError) throw error;
    throw new SentinelCodexAuthStateError("artifact_invalid", { cause: error });
  } finally {
    await Deno.remove(directory, { recursive: true }).catch(() => undefined);
  }
};

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name);
  if (!value) throw new SentinelCodexAuthStateError("prepared_state_invalid");
  return value;
};

const numericEnvironment = (name: string): number => {
  const parsed = parsePositiveIntegerText(requiredEnvironment(name));
  if (parsed === null) throw new SentinelCodexAuthStateError("invalid_run_identity");
  return parsed;
};

const appendSafeOutputs = async (values: Readonly<Record<string, string>>): Promise<void> => {
  for (const [name, value] of Object.entries(values)) {
    if (!/^[a-z][a-z0-9_]*$/u.test(name) || value.includes("\n") || value.includes("\r")) {
      throw new SentinelCodexAuthStateError("prepared_state_invalid");
    }
  }
  const outputPath = Deno.env.get("GITHUB_OUTPUT");
  const rendered = Object.entries(values).map(([name, value]) => `${name}=${value}`).join("\n") + "\n";
  if (outputPath) {
    await Deno.writeTextFile(outputPath, rendered, { append: true });
  } else {
    console.log(JSON.stringify(values));
  }
};

const prepareFromEnvironment = async (stateDirectory: string): Promise<void> => {
  const repository = parseRepository(requiredEnvironment("GITHUB_REPOSITORY"));
  const generation = parseSentinelCodexAuthStateGeneration(requiredEnvironment("SENTINEL_CODEX_AUTH_GENERATION"));
  const key = decodeSentinelAuthStateKey(requiredEnvironment("SENTINEL_CODEX_AUTH_STATE_KEY"));
  try {
    const currentWriterIdentity = {
      runId: numericEnvironment("GITHUB_RUN_ID"),
      runAttempt: numericEnvironment("GITHUB_RUN_ATTEMPT"),
    };
    const github = new GitHubActionsClient({
      repository,
      token: requiredEnvironment("GITHUB_TOKEN"),
    });
    const restored = await restoreSentinelCodexAuthState({
      artifacts: await github.listRepositoryArtifacts({ includeExpired: true }),
      repository,
      generation,
      currentWriterIdentity,
      keyBytes: key,
      seeds: {
        slot1B64: Deno.env.get("SENTINEL_CODEX_AUTH_SLOT_1_B64"),
        slot2B64: Deno.env.get("SENTINEL_CODEX_AUTH_SLOT_2_B64"),
      },
      downloadEnvelope: async (artifact, maximumBytes) => {
        const archive = new Uint8Array(await github.downloadArtifact(artifact.id, maximumBytes));
        try {
          return await unzipAuthStateEnvelope(archive);
        } finally {
          archive.fill(0);
        }
      },
    });
    await writePreparedSentinelCodexAuthState(stateDirectory, restored);
    await appendSafeOutputs({
      artifact_source: restored.source,
      needs_maintenance: String(restored.needsMaintenance),
    });
  } finally {
    key.fill(0);
  }
};

const sealFromEnvironment = async (stateDirectory: string, outputPath: string): Promise<void> => {
  if (
    !outputPath.startsWith("/") || outputPath.includes("\u0000") || outputPath.includes("\n") ||
    !outputPath.endsWith(`/${SENTINEL_CODEX_AUTH_STATE_ENVELOPE_FILENAME}`)
  ) {
    throw new SentinelCodexAuthStateError("invalid_state_directory");
  }
  const key = decodeSentinelAuthStateKey(requiredEnvironment("SENTINEL_CODEX_AUTH_STATE_KEY"));
  try {
    const identity = {
      repository: requiredEnvironment("GITHUB_REPOSITORY"),
      generation: requiredEnvironment("SENTINEL_CODEX_AUTH_GENERATION"),
      runId: numericEnvironment("GITHUB_RUN_ID"),
      runAttempt: numericEnvironment("GITHUB_RUN_ATTEMPT"),
    };
    const sealed = await sealPreparedSentinelCodexAuthState(stateDirectory, identity, key);
    try {
      await Deno.writeFile(outputPath, sealed.encrypted, { createNew: true, mode: 0o600 });
    } finally {
      sealed.encrypted.fill(0);
    }
    await appendSafeOutputs({
      artifact_name: sealed.artifactName,
      envelope_path: outputPath,
      slot_count: String(sealed.slotCount),
    });
  } finally {
    key.fill(0);
  }
};

const readinessFromDirectory = async (stateDirectory: string): Promise<void> => {
  const prepared = await readPreparedSentinelCodexAuthState(stateDirectory);
  const values: Record<string, string> = {};
  let ready = true;
  for (const expected of prepared.control.slots) {
    const code = prepared.readiness[expected.slot] ?? "missing";
    values[`slot_${expected.slot}`] = code;
    if (code !== "ready") ready = false;
  }
  await appendSafeOutputs(values);
  if (!ready) throw new SentinelCodexAuthStateError("prepared_state_invalid");
};

const probeFromDirectory = async (stateDirectory: string): Promise<void> => {
  const result = await probePreparedSentinelCodexAuthState(stateDirectory);
  const values: Record<string, string> = {
    auth_usable: String(result.usable),
    selected_slot: result.selectedSlot === null ? "none" : String(result.selectedSlot),
  };
  for (const slot of result.slots) {
    values[`slot_${slot.slot}_code`] = slot.code;
    values[`slot_${slot.slot}_http_status`] = slot.httpStatus === null ? "none" : String(slot.httpStatus);
    values[`slot_${slot.slot}_headroom_percent`] = slot.headroomPercent === null
      ? "none"
      : String(slot.headroomPercent);
  }
  await appendSafeOutputs(values);
  if (!result.usable) throw new SentinelCodexAuthStateError("accounts_unavailable");
};

if (import.meta.main) {
  const [command, stateDirectory, outputPath, ...extra] = Deno.args;
  try {
    if (!stateDirectory || extra.length > 0) throw new SentinelCodexAuthStateError("invalid_state_directory");
    if (command === "prepare" && outputPath === undefined) {
      await prepareFromEnvironment(stateDirectory);
    } else if (command === "seal" && outputPath !== undefined) {
      await sealFromEnvironment(stateDirectory, outputPath);
    } else if (command === "readiness" && outputPath === undefined) {
      await readinessFromDirectory(stateDirectory);
    } else if (command === "probe" && outputPath === undefined) {
      await probeFromDirectory(stateDirectory);
    } else {
      throw new SentinelCodexAuthStateError("invalid_state_directory");
    }
  } catch (error) {
    console.error(
      error instanceof SentinelCodexAuthStateError ? error.message : "Sentinel Codex auth-state command failed.",
    );
    Deno.exit(1);
  }
}
