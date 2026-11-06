/** Pure managed adoption and a fixed-key transaction protocol; neither opens storage nor refreshes credentials. */
import { createHash } from "node:crypto";
import { isDeepStrictEqual } from "node:util";
import { isAbsolute, resolve } from "node:path";
import { parseCodexAuthFromAuthJson, parseCodexAuthPool, getJwtExpMs, upsertCodexAuthAccount } from "./auth.ts";
import { nativeCodexCredentialGeneration, verifyNativeProgress, type NativeDocument } from "./native-auth.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../types.ts";
import { isRecord } from "../utils.ts";

export const managedAdoptionHash = (value: unknown): string => createHash("sha256").update(JSON.stringify(value)).digest("hex");
export class ManagedAdoptionError extends Error {
  constructor(readonly code: string) {
    super(`Managed native adoption refused: ${code}`);
  }
}
function refuse(code: string): never {
  throw new ManagedAdoptionError(code);
}
export type ManagedAdoptionSelection = Readonly<{ accountSha256: string; slot: number; home: string }>;
export type ManagedAdoptionPlan = Readonly<{
  pool: CodexAuthPoolState;
  beforeSha256: string;
  afterSha256: string;
  accountSha256: string;
  generationSha256: string;
  accessExpiresAtMs: number;
}>;

export const planManagedNativeAdoption = async (
  value: unknown,
  raw: unknown,
  selection: ManagedAdoptionSelection,
  nowMs: number
): Promise<ManagedAdoptionPlan> => {
  const parsed = parseCodexAuthPool(value);
  if (
    !parsed ||
    !isDeepStrictEqual(parsed, value) ||
    parsed.accounts.length !== 2 ||
    selection.slot !== 2 ||
    !/^[a-f0-9]{64}$/.test(selection.accountSha256) ||
    !isAbsolute(selection.home) ||
    resolve(selection.home) !== selection.home ||
    !Number.isSafeInteger(nowMs)
  )
    refuse("source_or_selection_invalid");
  // Validation must not reorder/reconstruct the unaffected sibling or its source fingerprint.
  const pool = value as CodexAuthPoolState;
  const previous = pool.accounts[selection.slot - 1];
  const accountHash = createHash("sha256").update(previous.account_id).digest("hex");
  if (accountHash !== selection.accountSha256 || previous.native_owner) refuse("account_or_owner_changed");
  if (!isRecord(raw) || (raw.auth_mode !== undefined && raw.auth_mode !== "chatgpt") || !isRecord(raw.tokens)) refuse("managed_document_required");
  const auth = parseCodexAuthFromAuthJson(raw);
  const refreshedAt = typeof raw.last_refresh === "string" ? raw.last_refresh : null;
  if (
    !auth?.refresh_token ||
    auth.account_id !== previous.account_id ||
    (raw.last_refresh !== undefined && raw.last_refresh !== null && (!refreshedAt || !Number.isFinite(Date.parse(refreshedAt))))
  )
    refuse("native_document_invalid");
  // Observation metadata is not part of the physical credential generation.
  const normalizedAuth: CodexAuthState = { ...auth, updated_at_ms: nowMs };
  const document: NativeDocument = { auth: normalizedAuth, email: null, refreshedAt };
  verifyNativeProgress(previous, document, true, nowMs);
  const generationSha256 = await nativeCodexCredentialGeneration(normalizedAuth);
  const replacement: CodexAuthState = {
    ...auth,
    updated_at_ms: nowMs,
    native_owner: { codex_home: selection.home, generation_hash: generationSha256, ...(refreshedAt ? { native_refreshed_at: refreshedAt } : {}) },
  };
  const next = upsertCodexAuthAccount(pool, replacement);
  if (next?.accounts.length !== pool.accounts.length || !isDeepStrictEqual(next.accounts[0], pool.accounts[0])) refuse("sibling_changed");
  // The shared upsert uses wall time for normal callers; this pure plan owns its explicit clock.
  const planned = { ...next, updated_at_ms: nowMs };
  return {
    pool: planned,
    beforeSha256: managedAdoptionHash(pool),
    afterSha256: managedAdoptionHash(planned),
    accountSha256: accountHash,
    generationSha256,
    accessExpiresAtMs: getJwtExpMs(auth.access_token) ?? refuse("expiry_missing"),
  };
};

/** Already-owned primary only. This never enters the refresh-capable runtime path. */
export const planOwnedPrimaryNativeAdoption = async (
  value: unknown,
  raw: unknown,
  selection: ManagedAdoptionSelection,
  nowMs: number
): Promise<ManagedAdoptionPlan> => {
  const parsed = parseCodexAuthPool(value);
  if (
    !parsed ||
    !isDeepStrictEqual(parsed, value) ||
    parsed.accounts.length !== 2 ||
    selection.slot !== 1 ||
    !/^[a-f0-9]{64}$/.test(selection.accountSha256) ||
    !isAbsolute(selection.home) ||
    resolve(selection.home) !== selection.home ||
    !Number.isSafeInteger(nowMs)
  )
    refuse("owned_primary_source_invalid");
  const pool = value as CodexAuthPoolState;
  const previous = pool.accounts[0];
  const owner = previous.native_owner;
  const accountHash = createHash("sha256").update(previous.account_id).digest("hex");
  if (
    accountHash !== selection.accountSha256 ||
    owner?.codex_home !== selection.home ||
    owner.auth_mode !== undefined ||
    owner.chatgpt_user_id !== undefined ||
    owner.generation_hash !== (await nativeCodexCredentialGeneration(previous))
  )
    refuse("owned_primary_fence_changed");
  if (!isRecord(raw) || (raw.auth_mode !== undefined && raw.auth_mode !== "chatgpt") || !isRecord(raw.tokens)) refuse("managed_document_required");
  const auth = parseCodexAuthFromAuthJson(raw);
  const refreshedAt = typeof raw.last_refresh === "string" ? raw.last_refresh : null;
  if (
    !auth?.refresh_token ||
    auth.account_id !== previous.account_id ||
    (raw.last_refresh !== undefined && raw.last_refresh !== null && (!refreshedAt || !Number.isFinite(Date.parse(refreshedAt))))
  )
    refuse("native_document_invalid");
  const normalized: CodexAuthState = { ...auth, updated_at_ms: nowMs };
  verifyNativeProgress(previous, { auth: normalized, email: null, refreshedAt }, true, nowMs);
  const generationSha256 = await nativeCodexCredentialGeneration(normalized);
  const next = upsertCodexAuthAccount(pool, {
    ...normalized,
    native_owner: {
      codex_home: selection.home,
      generation_hash: generationSha256,
      ...(refreshedAt ? { native_refreshed_at: refreshedAt } : {}),
    },
  });
  if (next?.accounts.length !== 2 || !isDeepStrictEqual(next.accounts[1], pool.accounts[1])) refuse("sibling_changed");
  const planned = { ...next, updated_at_ms: nowMs };
  return {
    pool: planned,
    beforeSha256: managedAdoptionHash(pool),
    afterSha256: managedAdoptionHash(planned),
    accountSha256: accountHash,
    generationSha256,
    accessExpiresAtMs: getJwtExpMs(normalized.access_token) ?? refuse("expiry_missing"),
  };
};

export type ManagedAdoptionSnapshot = Readonly<{ pool: unknown; versionstamp: string }>;
export type ManagedAdoptionExpectation = Readonly<{ poolVersionstamp: string; poolSha256: string; nativeSha256: string }>;
/** Constructor-owned adapter only: no caller-selected key, path or stopped boolean exists in this protocol. */
export type ManagedAdoptionPort = Readonly<{
  assertOwned: () => Promise<void>;
  read: () => Promise<ManagedAdoptionSnapshot>;
  native: () => Promise<Readonly<{ raw: unknown; sha256: string }>>;
  proveNative: (raw: unknown) => Promise<void>;
  backup: (snapshot: ManagedAdoptionSnapshot) => Promise<void>;
  intent: (plan: ManagedAdoptionPlan) => Promise<void>;
  commit: (versionstamp: string, pool: CodexAuthPoolState) => Promise<boolean>;
}>;
export type ManagedAdoptionReceipt = Readonly<{
  v: 1;
  kind: "managed_native_adoption";
  outcome: "committed" | "reconciled" | "not_committed";
  before_sha256: string;
  after_sha256: string;
  source_versionstamp: string;
  current_versionstamp: string;
  account_sha256: string;
  generation_sha256: string;
  access_expires_at_ms: number;
}>;

const executeAdoptionWithPlanner = async (
  port: ManagedAdoptionPort,
  selection: ManagedAdoptionSelection,
  expected: ManagedAdoptionExpectation,
  nowMs: number,
  planner: typeof planManagedNativeAdoption
): Promise<ManagedAdoptionReceipt> => {
  await port.assertOwned();
  const source = await port.read();
  if (source.versionstamp !== expected.poolVersionstamp || managedAdoptionHash(source.pool) !== expected.poolSha256) refuse("source_changed");
  const native = await port.native();
  if (native.sha256 !== expected.nativeSha256) refuse("home_changed");
  const plan = await planner(source.pool, native.raw, selection, nowMs);
  await port.proveNative(native.raw);
  await port.backup(source);
  await port.intent(plan);
  await port.assertOwned();
  if ((await port.native()).sha256 !== native.sha256) refuse("home_changed");
  let committed: boolean | null;
  try {
    committed = await port.commit(source.versionstamp, plan.pool);
  } catch {
    committed = null;
  }
  const current = await port.read();
  const currentHash = managedAdoptionHash(current.pool);
  if (committed === false) refuse("cas_conflict");
  let outcome: ManagedAdoptionReceipt["outcome"];
  if (currentHash === plan.afterSha256 && current.versionstamp !== source.versionstamp) outcome = committed === true ? "committed" : "reconciled";
  else if (committed === null && currentHash === plan.beforeSha256 && current.versionstamp === source.versionstamp) outcome = "not_committed";
  else refuse("commit_outcome_diverged");
  await port.assertOwned();
  if ((await port.native()).sha256 !== native.sha256) refuse("home_changed_after_commit");
  return {
    v: 1,
    kind: "managed_native_adoption",
    outcome,
    before_sha256: plan.beforeSha256,
    after_sha256: plan.afterSha256,
    source_versionstamp: source.versionstamp,
    current_versionstamp: current.versionstamp,
    account_sha256: plan.accountSha256,
    generation_sha256: plan.generationSha256,
    access_expires_at_ms: plan.accessExpiresAtMs,
  };
};

export const executeManagedNativeAdoption = (
  port: ManagedAdoptionPort,
  selection: ManagedAdoptionSelection,
  expected: ManagedAdoptionExpectation,
  nowMs: number
): Promise<ManagedAdoptionReceipt> => executeAdoptionWithPlanner(port, selection, expected, nowMs, planManagedNativeAdoption);

export const executeOwnedPrimaryNativeAdoption = (
  port: ManagedAdoptionPort,
  selection: ManagedAdoptionSelection,
  expected: ManagedAdoptionExpectation,
  nowMs: number
): Promise<ManagedAdoptionReceipt> => executeAdoptionWithPlanner(port, selection, expected, nowMs, planOwnedPrimaryNativeAdoption);
