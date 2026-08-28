import {
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelFailureConstraint,
  type SentinelBootstrapActivationPointerV1,
  type SentinelFailureConstraintV1,
} from "./contracts.ts";
import { assertBootstrapActivationFence, SENTINEL_BOOTSTRAP_POLICY } from "./policy.ts";

export const SENTINEL_BOOTSTRAP_ACTIVATION_KEY = [
  "uos_ai",
  "sentinel_bootstrap",
  "v1",
  "activation",
] as const;

export const SENTINEL_BOOTSTRAP_CONSTRAINT_PREFIX = [
  "uos_ai",
  "sentinel_bootstrap",
  "v1",
  "constraint",
] as const;

export type SentinelBootstrapActivationSnapshot = Readonly<{
  pointer: SentinelBootstrapActivationPointerV1 | null;
  versionstamp: string | null;
}>;

export interface SentinelBootstrapStateStore {
  readActivation(): Promise<SentinelBootstrapActivationSnapshot>;
  compareAndSetActivation(
    expectedVersionstamp: string | null,
    next: SentinelBootstrapActivationPointerV1,
  ): Promise<boolean>;
  putConstraintIfAbsent(constraint: SentinelFailureConstraintV1): Promise<boolean>;
}

export class SentinelBootstrapStateConflict extends Error {
  constructor(message = "Sentinel bootstrap state changed during the operation") {
    super(message);
    this.name = "SentinelBootstrapStateConflict";
  }
}

const isVersionstamp = (value: string | null): boolean => value === null || /^[0-9]+$/.test(value);

const nowIso = (): string => new Date().toISOString();

export const initialSentinelBootstrapActivation = (
  releaseValue: unknown,
  updatedAt = nowIso(),
): SentinelBootstrapActivationPointerV1 => {
  const release = parseBootstrapReleaseRecord(releaseValue);
  const activeSha = release.candidate_sha ?? release.stable_sha;
  return parseSentinelBootstrapActivationPointer({
    schema_version: 1,
    active_sha: activeSha,
    generation: release.generation,
    fenced_generations: [],
    updated_at: updatedAt,
    reason: null,
  });
};

export const assertActivationMatchesRelease = (
  pointerValue: unknown,
  releaseValue: unknown,
): SentinelBootstrapActivationPointerV1 => {
  const pointer = parseSentinelBootstrapActivationPointer(pointerValue);
  const release = parseBootstrapReleaseRecord(releaseValue);
  const allowedActiveShas = new Set(
    [release.stable_sha, release.candidate_sha].filter((sha): sha is string => sha !== null),
  );
  if (!allowedActiveShas.has(pointer.active_sha)) {
    throw new Error("Sentinel bootstrap activation points to an unregistered SHA");
  }
  if (pointer.active_sha === release.candidate_sha && pointer.generation !== release.generation) {
    throw new Error("Sentinel bootstrap candidate generation is stale");
  }
  if (pointer.generation < release.generation) {
    throw new Error("Sentinel bootstrap activation generation is stale");
  }
  if (pointer.fenced_generations.length > SENTINEL_BOOTSTRAP_POLICY.maximumFencedGenerations) {
    throw new Error("Sentinel bootstrap activation fence history is too large");
  }
  return pointer;
};

export const selectStableRollbackSha = (
  pointerValue: unknown,
  releaseValue: unknown,
): string | null => {
  const pointer = assertActivationMatchesRelease(pointerValue, releaseValue);
  const release = parseBootstrapReleaseRecord(releaseValue);
  if (pointer.active_sha === release.stable_sha) return null;
  if (release.candidate_sha === null || pointer.active_sha !== release.candidate_sha) {
    throw new Error("Sentinel bootstrap has no proven stable rollback target");
  }
  return release.stable_sha;
};

export const createRollbackActivation = (
  pointerValue: unknown,
  releaseValue: unknown,
  updatedAt = nowIso(),
): SentinelBootstrapActivationPointerV1 => {
  const pointer = assertActivationMatchesRelease(pointerValue, releaseValue);
  const stableSha = selectStableRollbackSha(pointer, releaseValue);
  if (stableSha === null) return pointer;
  const fenced = [...pointer.fenced_generations, pointer.generation];
  const bounded = fenced.length > SENTINEL_BOOTSTRAP_POLICY.maximumFencedGenerations
    ? fenced.slice(-SENTINEL_BOOTSTRAP_POLICY.maximumFencedGenerations)
    : fenced;
  return parseSentinelBootstrapActivationPointer({
    schema_version: 1,
    active_sha: stableSha,
    generation: pointer.generation + 1,
    fenced_generations: bounded,
    updated_at: updatedAt,
    reason: "authoritative_failure_rollback",
  });
};

export const assertCurrentBootstrapFence = (
  pointerValue: unknown,
  expected: Readonly<{ generation: number; activeSha: string }>,
): void => {
  const pointer = parseSentinelBootstrapActivationPointer(pointerValue);
  assertBootstrapActivationFence(pointer, expected);
};

export const createDenoKvBootstrapStateStore = (kv: Deno.Kv): SentinelBootstrapStateStore => ({
  async readActivation(): Promise<SentinelBootstrapActivationSnapshot> {
    const entry = await kv.get<unknown>(SENTINEL_BOOTSTRAP_ACTIVATION_KEY);
    return {
      pointer: entry.value === null ? null : parseSentinelBootstrapActivationPointer(entry.value),
      versionstamp: entry.versionstamp,
    };
  },

  async compareAndSetActivation(
    expectedVersionstamp: string | null,
    next: SentinelBootstrapActivationPointerV1,
  ): Promise<boolean> {
    if (!isVersionstamp(expectedVersionstamp)) throw new Error("Sentinel bootstrap versionstamp is invalid");
    const pointer = parseSentinelBootstrapActivationPointer(next);
    const committed = await kv.atomic()
      .check({ key: SENTINEL_BOOTSTRAP_ACTIVATION_KEY, versionstamp: expectedVersionstamp })
      .set(SENTINEL_BOOTSTRAP_ACTIVATION_KEY, pointer)
      .commit();
    return committed.ok;
  },

  async putConstraintIfAbsent(constraint: SentinelFailureConstraintV1): Promise<boolean> {
    const parsed = parseSentinelFailureConstraint(constraint);
    const key = [...SENTINEL_BOOTSTRAP_CONSTRAINT_PREFIX, parsed.failure_fingerprint] as const;
    const existing = await kv.get<unknown>(key);
    if (existing.value !== null) {
      parseSentinelFailureConstraint(existing.value);
      return false;
    }
    const committed = await kv.atomic()
      .check({ key, versionstamp: existing.versionstamp })
      .set(key, parsed)
      .commit();
    return committed.ok;
  },
});
