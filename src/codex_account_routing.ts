import { getKv } from "./kv.ts";
import { readBoundedResponseBody } from "./bounded_response_body.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type { CodexAuthPoolState, CodexAuthState } from "./types.ts";

/**
 * Durable, slot-indexed routing state. It intentionally contains neither raw
 * account ids nor credentials; an opaque account-scope hash keeps a circuit
 * with the same account through a pool reorder without letting a replacement
 * account inherit it.
 */
export const CODEX_ACCOUNT_ROUTING_KV_KEY = ["uos_ai", "codex_account_routing", "v2"] as const;
export const CODEX_HALF_OPEN_LEASE_MS = 30_000;
// A warm isolate avoids per-request routing reads, but it must eventually
// observe circuits opened by another isolate. This bounded revalidation keeps
// normal traffic off KV while limiting cross-isolate stale routing decisions.
const ROUTING_CACHE_REVALIDATE_MS = 5_000;

export type CodexQuotaBlockSource = "body_resets_at" | "header_retry_after";

export type CodexRoutingSlot = Readonly<{
  /** Opaque account-scope hash; durable state never stores a raw account id. */
  account_id_hash: string | null;
  credential_version: string;
  quota_blocked_until_ms: number | null;
  quota_block_source: CodexQuotaBlockSource | null;
  invalid_credential_version: string | null;
  primary_used_percent: number | null;
  secondary_used_percent: number | null;
  observed_reset_at_ms: number | null;
  /** True only when the upstream supplied a canonical absolute reset deadline. */
  observed_reset_at_is_stable: boolean;
  /**
   * A stable reset deadline plus any later conflicting or relative value cannot
   * be proved to name a new provider quota generation. While it remains true,
   * ordinary routing continues but banked-reset claims fail closed and
   * existing records are lookup-only.
   */
  banked_reset_generation_ambiguous: boolean;
  generation: number;
  probe_lease: Readonly<{ token: string; expires_at_ms: number; generation: number }> | null;
}>;

export type CodexAccountRoutingState = Readonly<{
  v: 2;
  updated_at_ms: number;
  /** A pre-account-hash stable identity could not be matched to a current account. */
  banked_reset_legacy_identity_unresolved: boolean;
  slots: readonly CodexRoutingSlot[];
}>;

export type RoutingAccount = Readonly<{
  auth: CodexAuthState;
  slot: number;
  /** Opaque account-scope hash used only to keep durable slots attached to an account across reordering. */
  accountIdHash: string;
  credentialVersion: string;
  quotaHeadroom: number | null;
  probeRequired: boolean;
  probeGeneration: number | null;
  probeToken: string | null;
}>;

/** A durable provider-deadline circuit that may have an existing reset record to reconcile. */
export type CodexBlockedRoutingAccount =
  & RoutingAccount
  & Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
  }>;

export type RouteSelection =
  | Readonly<{
    kind: "eligible";
    accounts: readonly RoutingAccount[];
    skippedSlots: readonly number[];
    /** Stable absolute quota fences omitted from ordinary eligible routing. */
    blockedAccounts: readonly CodexBlockedRoutingAccount[];
  }>
  | Readonly<{
    kind: "quota_blocked";
    skippedSlots: readonly number[];
    retryAtMs: number | null;
    blockedAccounts: readonly CodexBlockedRoutingAccount[];
  }>
  | Readonly<{ kind: "credentials_invalid"; skippedSlots: readonly number[] }>;

export type StrongRouteSelection =
  | RouteSelection
  | Readonly<{ kind: "routing_unavailable" }>;

const isSafeMs = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isLegacyStableResetIdentity = (slot: CodexRoutingSlot): boolean =>
  slot.account_id_hash === null && slot.observed_reset_at_ms !== null && slot.observed_reset_at_is_stable;

const hasUnresolvedLegacyResetIdentity = (state: CodexAccountRoutingState | null): boolean =>
  state?.banked_reset_legacy_identity_unresolved === true ||
  state?.slots.some(isLegacyStableResetIdentity) === true;

const parseSlot = (value: unknown, allowLegacyNeutralRepair: boolean): CodexRoutingSlot | null => {
  if (!isRecord(value) || typeof value.credential_version !== "string") return null;
  const source = value.quota_block_source;
  if (source !== null && source !== "body_resets_at" && source !== "header_retry_after") return null;
  const lease = value.probe_lease;
  const parsedLease = lease === null
    ? null
    : isRecord(lease) && typeof lease.token === "string" && isSafeMs(lease.expires_at_ms) &&
        typeof lease.generation === "number" && Number.isSafeInteger(lease.generation)
    ? { token: lease.token, expires_at_ms: lease.expires_at_ms, generation: lease.generation }
    : null;
  if (lease !== null && !parsedLease) return null;
  const accountIdHash = typeof value.account_id_hash === "string" && value.account_id_hash.length > 0
    ? value.account_id_hash
    : null;
  const quotaBlockedUntilMs = value.quota_blocked_until_ms === null || isSafeMs(value.quota_blocked_until_ms)
    ? value.quota_blocked_until_ms as number | null
    : null;
  const invalidCredentialVersion = typeof value.invalid_credential_version === "string"
    ? value.invalid_credential_version
    : null;
  const observedResetAtMs = value.observed_reset_at_ms === null || isSafeMs(value.observed_reset_at_ms)
    ? value.observed_reset_at_ms as number | null
    : null;
  const observedResetAtIsStable = value.observed_reset_at_is_stable === true;
  const generation =
    typeof value.generation === "number" && Number.isSafeInteger(value.generation) && value.generation >= 0
      ? value.generation
      : 0;
  const isExactLegacyNeutralSlot = allowLegacyNeutralRepair &&
    !("account_id_hash" in value) &&
    !("observed_reset_at_is_stable" in value) &&
    !("banked_reset_generation_ambiguous" in value) &&
    value.generation === 0 &&
    value.quota_blocked_until_ms === null &&
    source === null &&
    value.invalid_credential_version === null &&
    value.primary_used_percent === null &&
    value.secondary_used_percent === null &&
    value.observed_reset_at_ms === null &&
    lease === null;
  // The first body-derived fence written after an exact legacy-neutral slot
  // inherited the old parser's synthetic ambiguity. Generation one proves
  // there was no prior quota transition; every real revision, recheck,
  // credential rotation, or recovery transition increments it again.
  const isLegacyNeutralFirstBodyFence = allowLegacyNeutralRepair &&
    value.banked_reset_generation_ambiguous === true &&
    value.generation === 1 &&
    accountIdHash !== null &&
    source === "body_resets_at" &&
    quotaBlockedUntilMs !== null &&
    observedResetAtMs === quotaBlockedUntilMs &&
    observedResetAtIsStable &&
    lease === null &&
    value.invalid_credential_version === null;
  const bankedResetGenerationAmbiguous = isExactLegacyNeutralSlot || isLegacyNeutralFirstBodyFence
    ? false
    : value.banked_reset_generation_ambiguous !== false;
  return {
    account_id_hash: accountIdHash,
    credential_version: value.credential_version,
    quota_blocked_until_ms: quotaBlockedUntilMs,
    quota_block_source: source as CodexQuotaBlockSource | null,
    invalid_credential_version: invalidCredentialVersion,
    primary_used_percent: typeof value.primary_used_percent === "number" && Number.isFinite(value.primary_used_percent)
      ? value.primary_used_percent
      : null,
    secondary_used_percent:
      typeof value.secondary_used_percent === "number" && Number.isFinite(value.secondary_used_percent)
        ? value.secondary_used_percent
        : null,
    observed_reset_at_ms: observedResetAtMs,
    // Older routing records did not carry this flag. Treat their header
    // deadline as unsuitable for an expensive reset rather than guessing its
    // identity from a relative timeout.
    observed_reset_at_is_stable: observedResetAtIsStable,
    // A record written before this fence cannot prove that its deadline was
    // never revised. The two exact legacy-contamination repairs above are the
    // only exceptions; every other old or malformed record remains fail closed.
    banked_reset_generation_ambiguous: bankedResetGenerationAmbiguous,
    generation,
    probe_lease: parsedLease,
  };
};

export const parseCodexAccountRoutingState = (value: unknown): CodexAccountRoutingState | null => {
  if (!isRecord(value) || value.v !== 2 || !isSafeMs(value.updated_at_ms) || !Array.isArray(value.slots)) return null;
  const legacyIdentityUnresolved = value.banked_reset_legacy_identity_unresolved === true;
  const slots = value.slots.map((slot) => parseSlot(slot, !legacyIdentityUnresolved));
  if (slots.some((slot) => !slot)) return null;
  return {
    v: 2,
    updated_at_ms: value.updated_at_ms,
    // The field is deliberately opt-in here: normalization can still attach a
    // legacy stable identity when its credential version proves the account.
    // Exact reset fences independently reject any un-hashed stable slot.
    banked_reset_legacy_identity_unresolved: legacyIdentityUnresolved,
    slots: slots as CodexRoutingSlot[],
  };
};

export const codexCredentialVersion = async (auth: CodexAuthState): Promise<string> =>
  await sha256Hex(`${auth.account_id}\u0000${auth.access_token}\u0000${auth.refresh_token}`);

const codexRoutingAccountIdHash = async (auth: CodexAuthState): Promise<string> =>
  await sha256Hex(`uos_ai\u0000codex_routing_account\u0000${auth.account_id}`);

type CodexRoutingAccountIdentity = Readonly<{
  accountIdHash: string;
  credentialVersion: string;
}>;

const routingAccountIdentity = async (auth: CodexAuthState): Promise<CodexRoutingAccountIdentity> => {
  const [accountIdHash, credentialVersion] = await Promise.all([
    codexRoutingAccountIdHash(auth),
    codexCredentialVersion(auth),
  ]);
  return { accountIdHash, credentialVersion };
};

const neutralSlot = (credentialVersion: string, accountIdHash: string | null): CodexRoutingSlot => ({
  account_id_hash: accountIdHash,
  credential_version: credentialVersion,
  quota_blocked_until_ms: null,
  quota_block_source: null,
  invalid_credential_version: null,
  primary_used_percent: null,
  secondary_used_percent: null,
  observed_reset_at_ms: null,
  observed_reset_at_is_stable: false,
  banked_reset_generation_ambiguous: false,
  generation: 0,
  probe_lease: null,
});

const preservesStableResetIdentity = (slot: CodexRoutingSlot): boolean =>
  slot.banked_reset_generation_ambiguous ||
  (slot.observed_reset_at_ms !== null && slot.observed_reset_at_is_stable);

/**
 * A token refresh stays in the same provider quota scope. Release ordinary
 * routing for the refreshed credential, but keep any pre-refresh stable reset
 * observation lookup-only until a successful recovery probe proves the
 * account recovered.
 */
const rotateCredentialForSameAccount = (
  slot: CodexRoutingSlot,
  identity: CodexRoutingAccountIdentity,
): CodexRoutingSlot => ({
  ...slot,
  account_id_hash: identity.accountIdHash,
  credential_version: identity.credentialVersion,
  quota_blocked_until_ms: null,
  quota_block_source: null,
  invalid_credential_version: null,
  primary_used_percent: null,
  secondary_used_percent: null,
  banked_reset_generation_ambiguous: preservesStableResetIdentity(slot),
  generation: slot.generation + 1,
  probe_lease: null,
});

const attachLegacyAccountIdentity = (
  slot: CodexRoutingSlot,
  identity: CodexRoutingAccountIdentity,
): CodexRoutingSlot => ({
  ...slot,
  account_id_hash: identity.accountIdHash,
  // A pre-account-identity record cannot establish that its stable deadline
  // belongs to this account after a pool transition. Keep it lookup-only.
  banked_reset_generation_ambiguous: preservesStableResetIdentity(slot),
});

export const normalizeRoutingState = async (
  raw: CodexAccountRoutingState | null,
  pool: CodexAuthPoolState,
  now = Date.now(),
): Promise<CodexAccountRoutingState> => {
  const identities = await Promise.all(pool.accounts.map(routingAccountIdentity));
  const priorSlots = raw?.slots ?? [];
  const usedPriorSlots = new Set<number>();
  const takePrior = (matches: (slot: CodexRoutingSlot) => boolean): CodexRoutingSlot | null => {
    const index = priorSlots.findIndex((slot, candidateIndex) => !usedPriorSlots.has(candidateIndex) && matches(slot));
    if (index < 0) return null;
    usedPriorSlots.add(index);
    return priorSlots[index]!;
  };
  const slots = identities.map((identity, index) => {
    const direct = priorSlots[index];
    let prior: CodexRoutingSlot | null = null;
    if (
      direct && !usedPriorSlots.has(index) &&
      (direct.account_id_hash === identity.accountIdHash ||
        (direct.account_id_hash === null && direct.credential_version === identity.credentialVersion))
    ) {
      usedPriorSlots.add(index);
      prior = direct;
    }
    prior ??= takePrior((slot) => slot.account_id_hash === identity.accountIdHash);
    // Version equality is the only safe way to attach an account identity to a
    // legacy record that predates the opaque account hash.
    prior ??= takePrior((slot) =>
      slot.account_id_hash === null && slot.credential_version === identity.credentialVersion
    );
    if (!prior) return neutralSlot(identity.credentialVersion, identity.accountIdHash);
    if (prior.credential_version === identity.credentialVersion) {
      return prior.account_id_hash === identity.accountIdHash ? prior : attachLegacyAccountIdentity(prior, identity);
    }
    return rotateCredentialForSameAccount(prior, identity);
  });
  // Do not attach an unidentifiable legacy identity to whichever account now
  // occupies its old slot. A global claim fence keeps ordinary routing usable
  // while preventing a same-account token rotation from minting a new key.
  const legacyIdentityUnresolved = raw?.banked_reset_legacy_identity_unresolved === true ||
    priorSlots.some((slot, index) => !usedPriorSlots.has(index) && isLegacyStableResetIdentity(slot));
  return {
    v: 2,
    updated_at_ms: now,
    banked_reset_legacy_identity_unresolved: legacyIdentityUnresolved,
    slots,
  };
};

let cachedState: CodexAccountRoutingState | null = null;
// `undefined` means this isolate only has synthesized/local state, so it
// must refresh before attempting a durable compare-and-set. `null` is a real
// KV versionstamp for an absent routing record and is safe to check directly.
let cachedVersionstamp: string | null | undefined = undefined;
let cachedStateLoadedAtMs = 0;

export const resetCodexAccountRoutingForTest = (): void => {
  cachedState = null;
  cachedVersionstamp = undefined;
  cachedStateLoadedAtMs = 0;
};

/** Loads routing alongside a cold auth read.  KV failure deliberately fails open. */
export const loadCodexAccountRouting = async (pool: CodexAuthPoolState): Promise<CodexAccountRoutingState> => {
  if (cachedState && Date.now() - cachedStateLoadedAtMs < ROUTING_CACHE_REVALIDATE_MS) {
    const normalized = await normalizeRoutingState(cachedState, pool);
    // `normalizeRoutingState` preserves matching slot references, so this is
    // a cheap way to spot an auth-pool rotation without persisting anything
    // until a later routing transition actually needs to write it.
    const routingStateChanged = normalized.banked_reset_legacy_identity_unresolved !==
        cachedState.banked_reset_legacy_identity_unresolved ||
      normalized.slots.length !== cachedState.slots.length ||
      normalized.slots.some((slot, index) => slot !== cachedState!.slots[index]);
    if (routingStateChanged) cachedState = normalized;
    return cachedState;
  }
  try {
    const kv = await getKv();
    if (!kv) {
      const normalized = await normalizeRoutingState(cachedState, pool);
      cachedState = normalized;
      cachedVersionstamp = undefined;
      cachedStateLoadedAtMs = Date.now();
      return normalized;
    }
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
    cachedState = await normalizeRoutingState(parseCodexAccountRoutingState(entry.value), pool);
    cachedVersionstamp = entry.versionstamp;
    cachedStateLoadedAtMs = Date.now();
    return cachedState;
  } catch {
    const normalized = await normalizeRoutingState(cachedState, pool);
    cachedState = normalized;
    cachedVersionstamp = undefined;
    cachedStateLoadedAtMs = Date.now();
    return normalized;
  }
};

/**
 * Apply a small state transition against the latest durable record. This is
 * deliberately compare-and-set rather than a blind `set`: independent slot
 * transitions must not erase each other when different isolates observe
 * failures at the same time.
 */
const updateRoutingState = async (
  transform: (state: CodexAccountRoutingState) => CodexAccountRoutingState | null,
): Promise<CodexAccountRoutingState | null> => {
  const applyLocally = (): CodexAccountRoutingState | null => {
    if (!cachedState) return null;
    const next = transform(cachedState);
    if (next) {
      cachedState = next;
      // The local update was not committed, so its old versionstamp can no
      // longer safely authorize a future write.
      cachedVersionstamp = undefined;
      cachedStateLoadedAtMs = Date.now();
    }
    return next ?? cachedState;
  };

  let kv: Deno.Kv | null = null;
  try {
    kv = await getKv();
  } catch {
    return applyLocally();
  }
  if (!kv) return applyLocally();

  // A cold auth hydration already obtained this record. Reuse that
  // versionstamp for the first transition instead of paying a second routing
  // read on a request that just received a 401/429. A concurrent writer makes
  // this CAS fail, after which the strong-read retry below preserves its work.
  if (cachedState && cachedVersionstamp !== undefined) {
    const next = transform(cachedState);
    if (!next) return cachedState;
    try {
      const committed = await kv.atomic()
        .check({ key: CODEX_ACCOUNT_ROUTING_KV_KEY, versionstamp: cachedVersionstamp })
        .set(CODEX_ACCOUNT_ROUTING_KV_KEY, next)
        .commit();
      if (committed.ok) {
        cachedState = next;
        cachedVersionstamp = committed.versionstamp;
        cachedStateLoadedAtMs = Date.now();
        return next;
      }
    } catch {
      return applyLocally();
    }
  }

  for (let attempt = 0; attempt < 3; attempt += 1) {
    try {
      const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, {
        consistency: "strong",
      });
      const durable = parseCodexAccountRoutingState(entry.value);
      const base = durable ?? cachedState;
      if (!base) return null;
      const next = transform(base);
      if (!next) {
        cachedState = base;
        cachedVersionstamp = entry.versionstamp;
        cachedStateLoadedAtMs = Date.now();
        return base;
      }
      const committed = await kv.atomic().check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, next).commit();
      if (committed.ok) {
        cachedState = next;
        cachedVersionstamp = committed.versionstamp;
        cachedStateLoadedAtMs = Date.now();
        return next;
      }
    } catch {
      return applyLocally();
    }
  }
  return null;
};

const slotFor = (state: CodexAccountRoutingState, account: RoutingAccount): CodexRoutingSlot =>
  state.slots[account.slot] ?? neutralSlot(account.credentialVersion, account.accountIdHash);

const slotMatchesRoutingAccount = (slot: CodexRoutingSlot, account: RoutingAccount): boolean =>
  slot.credential_version === account.credentialVersion &&
  // Exact credential-version equality is the legacy proof of account scope;
  // every successful transition writes the opaque account hash immediately.
  (slot.account_id_hash === account.accountIdHash || slot.account_id_hash === null);

const withSlot = (
  state: CodexAccountRoutingState,
  index: number,
  slot: CodexRoutingSlot,
): CodexAccountRoutingState => {
  const slots = [...state.slots];
  while (slots.length <= index) slots.push(neutralSlot(slot.credential_version, slot.account_id_hash));
  slots[index] = slot;
  return { ...state, updated_at_ms: Date.now(), slots };
};

const parseFinitePercent = (value: string | null): number | null => {
  if (value === null) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 && parsed <= 100 ? parsed : null;
};

const quotaHeadroomFor = (slot: CodexRoutingSlot): number | null => {
  const used = [slot.primary_used_percent, slot.secondary_used_percent]
    .filter((value): value is number => value !== null);
  return used.length ? Math.max(0, Math.min(...used.map((value) => 100 - value))) : null;
};

const IMF_FIXDATE_PATTERN =
  /^(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} \d{2}:\d{2}:\d{2} GMT$/;

type RetryAfterDeadline = Readonly<{
  deadlineMs: number;
  /** A delta timeout is useful for routing, but not a durable reset identity. */
  isStable: boolean;
}>;

const futureRetryAfterDeadline = (headers: Headers, now: number): RetryAfterDeadline | null => {
  const raw = headers.get("retry-after");
  if (raw === null) return null;
  const value = raw.trim();
  if (/^\d+$/.test(value)) {
    const seconds = Number(value);
    const deltaMs = seconds * 1_000;
    const deadline = now + deltaMs;
    return Number.isSafeInteger(seconds) && Number.isSafeInteger(deltaMs) &&
        Number.isSafeInteger(deadline) && deadline > now
      ? { deadlineMs: deadline, isStable: false }
      : null;
  }
  if (!IMF_FIXDATE_PATTERN.test(value)) return null;
  const deadline = Date.parse(value);
  return Number.isSafeInteger(deadline) && deadline > now && new Date(deadline).toUTCString() === value
    ? { deadlineMs: deadline, isStable: true }
    : null;
};

export type Codex429Classification = Readonly<{
  response: Response;
  usageLimitReached: boolean;
  retryAtMs: number | null;
  quotaBlockSource: CodexQuotaBlockSource | null;
  /** Whether `retryAtMs` is a canonical absolute deadline (not a provider generation by itself). */
  resetDeadlineIsStable: boolean;
  /** Conflicting deadline signals permanently fence this observation from redemption. */
  resetDeadlineConflict: boolean;
}>;

type JsonObjectKeyScanResult = "valid" | "invalid" | "duplicate";

/**
 * JSON.parse intentionally accepts repeated object keys and keeps only the
 * last one. A quota decision cannot rely on that ambiguous interpretation, so
 * scan the complete JSON grammar first and reject any repeated key.
 */
const hasDuplicateJsonObjectKeys = (source: string): boolean => {
  let index = 0;
  const skipWhitespace = (): void => {
    while (index < source.length && /[\t\n\r ]/.test(source[index]!)) index += 1;
  };
  const parseString = (): string | null => {
    if (source[index] !== '"') return null;
    const start = index;
    index += 1;
    while (index < source.length) {
      const code = source.charCodeAt(index);
      if (code <= 0x1f) return null;
      if (code === 0x5c) {
        index += 2;
        continue;
      }
      index += 1;
      if (code !== 0x22) continue;
      try {
        const value: unknown = JSON.parse(source.slice(start, index));
        return typeof value === "string" ? value : null;
      } catch {
        return null;
      }
    }
    return null;
  };
  const parseLiteral = (literal: string): boolean => {
    if (!source.startsWith(literal, index)) return false;
    index += literal.length;
    return true;
  };
  const parseNumber = (): boolean => {
    const match = /^(?:-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/.exec(source.slice(index));
    if (!match) return false;
    index += match[0].length;
    return true;
  };
  function parseValue(): JsonObjectKeyScanResult {
    skipWhitespace();
    switch (source[index]) {
      case "{":
        return parseObject();
      case "[":
        return parseArray();
      case '"':
        return parseString() === null ? "invalid" : "valid";
      case "t":
        return parseLiteral("true") ? "valid" : "invalid";
      case "f":
        return parseLiteral("false") ? "valid" : "invalid";
      case "n":
        return parseLiteral("null") ? "valid" : "invalid";
      default:
        return parseNumber() ? "valid" : "invalid";
    }
  }
  function parseArray(): JsonObjectKeyScanResult {
    index += 1;
    skipWhitespace();
    if (source[index] === "]") {
      index += 1;
      return "valid";
    }
    while (index < source.length) {
      const value = parseValue();
      if (value !== "valid") return value;
      skipWhitespace();
      if (source[index] === "]") {
        index += 1;
        return "valid";
      }
      if (source[index] !== ",") return "invalid";
      index += 1;
    }
    return "invalid";
  }
  function parseObject(): JsonObjectKeyScanResult {
    index += 1;
    skipWhitespace();
    if (source[index] === "}") {
      index += 1;
      return "valid";
    }
    const keys = new Set<string>();
    while (index < source.length) {
      const key = parseString();
      if (key === null) return "invalid";
      if (keys.has(key)) return "duplicate";
      keys.add(key);
      skipWhitespace();
      if (source[index] !== ":") return "invalid";
      index += 1;
      const value = parseValue();
      if (value !== "valid") return value;
      skipWhitespace();
      if (source[index] === "}") {
        index += 1;
        return "valid";
      }
      if (source[index] !== ",") return "invalid";
      index += 1;
      skipWhitespace();
    }
    return "invalid";
  }

  return parseValue() === "duplicate";
};

/**
 * Read a bounded error body and replace the response so callers retain the
 * OpenAI-compatible upstream payload after routing has classified it.
 */
export const readCodex429 = async (
  response: Response,
  now = Date.now(),
): Promise<Codex429Classification> => {
  const headers = new Headers(response.headers);
  // Preserve a complete upstream body for the final response while refusing
  // to classify or forward an incomplete body.
  const { bytes, complete } = await readBoundedResponseBody(response, {
    cancellationReason: "Codex 429 classified",
  });
  let usageLimitReached = false;
  let bodyResetAtMs: number | null = null;
  if (complete) {
    try {
      const bodyText = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
      // Do not let a malformed byte sequence or JSON.parse's last-key-wins
      // behavior turn an ambiguous upstream body into a durable reset signal.
      if (!hasDuplicateJsonObjectKeys(bodyText)) {
        const body = JSON.parse(bodyText);
        const error = isRecord(body) && isRecord(body.error) ? body.error : null;
        usageLimitReached = getString(error?.type) === "usage_limit_reached";
        const resetsAtSeconds = error?.resets_at;
        if (
          usageLimitReached && typeof resetsAtSeconds === "number" && Number.isSafeInteger(resetsAtSeconds) &&
          resetsAtSeconds >= 0
        ) {
          const deadlineMs = resetsAtSeconds * 1_000;
          if (Number.isSafeInteger(deadlineMs) && deadlineMs > now) bodyResetAtMs = deadlineMs;
        }
      }
    } catch {
      // A valid UTF-8, unambiguous, fully parsed OpenAI error is required
      // before routing can persist a block.
    }
  }
  const retryAfter = futureRetryAfterDeadline(headers, now);
  const absoluteHeaderConflict = bodyResetAtMs !== null && retryAfter?.isStable === true &&
    retryAfter.deadlineMs !== bodyResetAtMs;
  const relativeHeaderExtendsPastBody = bodyResetAtMs !== null && retryAfter?.isStable === false &&
    retryAfter.deadlineMs > bodyResetAtMs;
  const resetDeadlineConflict = absoluteHeaderConflict || relativeHeaderExtendsPastBody;
  const retryAtMs = bodyResetAtMs === null
    ? retryAfter?.deadlineMs ?? null
    : resetDeadlineConflict
    ? Math.max(bodyResetAtMs, retryAfter!.deadlineMs)
    : bodyResetAtMs;
  const quotaBlockSource: CodexQuotaBlockSource | null = bodyResetAtMs !== null
    ? "body_resets_at"
    : retryAfter
    ? "header_retry_after"
    : null;
  const resetDeadlineIsStable = bodyResetAtMs !== null ? !resetDeadlineConflict : retryAfter?.isStable === true;
  if (!complete) {
    headers.set("Content-Type", "application/json");
    return {
      response: new Response(
        JSON.stringify({
          error: {
            message: "Codex returned an oversized or incomplete rate-limit response.",
            type: "rate_limit_error",
            code: "codex_rate_limit_response_truncated",
            param: null,
          },
        }),
        { status: response.status, statusText: response.statusText, headers },
      ),
      usageLimitReached: false,
      retryAtMs,
      quotaBlockSource,
      resetDeadlineIsStable,
      resetDeadlineConflict,
    };
  }
  return {
    response: new Response(bytes, { status: response.status, statusText: response.statusText, headers }),
    usageLimitReached,
    retryAtMs,
    quotaBlockSource,
    resetDeadlineIsStable,
    resetDeadlineConflict,
  };
};

const markCodexQuotaBlockedWithMode = async (
  account: RoutingAccount,
  response: Response,
  now = Date.now(),
  recoveryProbe = false,
): Promise<Codex429Classification> => {
  const parsed = await readCodex429(response, now);
  const retryAtMs = parsed.retryAtMs;
  const quotaBlockSource = parsed.quotaBlockSource;
  if (!parsed.usageLimitReached || retryAtMs === null || quotaBlockSource === null) {
    // An expired circuit is represented by a fenced half-open probe. A
    // non-blocking 429 must release that old circuit, while the generation and
    // token checks prevent a stale probe from clearing a newer claim.
    if (account.probeGeneration !== null && account.probeToken) {
      await updateRoutingState((state) => {
        const current = slotFor(state, account);
        if (
          !slotMatchesRoutingAccount(current, account) ||
          current.generation !== account.probeGeneration ||
          current.probe_lease?.generation !== account.probeGeneration ||
          current.probe_lease?.token !== account.probeToken
        ) return null;
        return withSlot(state, account.slot, {
          ...current,
          account_id_hash: account.accountIdHash,
          quota_blocked_until_ms: null,
          quota_block_source: null,
          probe_lease: null,
        });
      });
    }
    return parsed;
  }
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (!slotMatchesRoutingAccount(current, account)) return null;
    // An ordinary request can predate a foreign half-open claim. It must not
    // replace that lease or admit a parallel probe.
    if (account.probeGeneration === null && current.probe_lease !== null) return null;
    if (
      account.probeGeneration !== null &&
      (current.generation !== account.probeGeneration || current.probe_lease?.token !== account.probeToken)
    ) return null;
    const priorDeadline = current.quota_blocked_until_ms ?? 0;
    const boundedRecoveryProbe = recoveryProbe ||
      (current.banked_reset_generation_ambiguous && account.probeGeneration !== null);
    // A verified reset can take a short time to propagate to the inference
    // endpoint. A failed recovery probe must not turn that transient 429 into
    // the old, week-long circuit. Keep the account fenced and retry a bounded
    // probe after the normal half-open lease interval. The verified redemption
    // record prevents this path from spending another reset for the same quota
    // episode.
    const deadline = boundedRecoveryProbe ? now + CODEX_HALF_OPEN_LEASE_MS : Math.max(priorDeadline, retryAtMs);
    const hasStableObservation = current.observed_reset_at_ms !== null && current.observed_reset_at_is_stable;
    // A stable absolute deadline is not by itself a provider-proven new
    // quota-window generation.
    // Once a stable observation exists, a changed date *or any later relative
    // delay* cannot prove a new provider quota generation. Keep the first
    // stable observation lookup-only and fence claims until a successful
    // half-open probe clears it. Expiry and administrative rechecks are not
    // proof that the provider advanced the quota generation.
    const generationAmbiguous = boundedRecoveryProbe || current.banked_reset_generation_ambiguous ||
      parsed.resetDeadlineConflict ||
      (hasStableObservation &&
        (!parsed.resetDeadlineIsStable || retryAtMs !== current.observed_reset_at_ms));
    const preserveStableObservation = hasStableObservation && generationAmbiguous;
    // The reset observation must describe the actual circuit deadline. A
    // shorter later Retry-After cannot overwrite the identity of an earlier,
    // longer block and thereby let a stale reset clear that longer circuit.
    const observedResetAtMs = preserveStableObservation
      ? current.observed_reset_at_ms
      : retryAtMs >= priorDeadline
      ? retryAtMs
      : current.observed_reset_at_ms;
    const observedResetAtIsStable = preserveStableObservation
      ? current.observed_reset_at_is_stable
      : retryAtMs >= priorDeadline
      ? parsed.resetDeadlineIsStable
      : current.observed_reset_at_is_stable;
    const nextSlot: CodexRoutingSlot = {
      ...current,
      account_id_hash: account.accountIdHash,
      quota_blocked_until_ms: deadline,
      quota_block_source: quotaBlockSource,
      primary_used_percent: parseFinitePercent(parsed.response.headers.get("x-codex-primary-used-percent")) ??
        current.primary_used_percent,
      secondary_used_percent: parseFinitePercent(parsed.response.headers.get("x-codex-secondary-used-percent")) ??
        current.secondary_used_percent,
      observed_reset_at_ms: observedResetAtMs,
      observed_reset_at_is_stable: observedResetAtIsStable,
      banked_reset_generation_ambiguous: generationAmbiguous,
      generation: current.generation + 1,
      probe_lease: null,
    };
    return withSlot(state, account.slot, nextSlot);
  });
  return parsed;
};

export const markCodexQuotaBlocked = async (
  account: RoutingAccount,
  response: Response,
  now = Date.now(),
): Promise<Codex429Classification> => await markCodexQuotaBlockedWithMode(account, response, now, false);

/**
 * Record a failed probe immediately after a verified banked reset. Keep the
 * account fenced, but schedule a short half-open retry because reset
 * propagation can lag the provider's terminal consume response.
 */
export const markCodexRecoveryProbeQuotaBlocked = async (
  account: RoutingAccount,
  response: Response,
  now = Date.now(),
): Promise<Codex429Classification> => await markCodexQuotaBlockedWithMode(account, response, now, true);

export const markCodexCredentialInvalid = async (account: RoutingAccount): Promise<void> => {
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (!slotMatchesRoutingAccount(current, account)) return null;
    if (
      account.probeGeneration !== null &&
      (current.generation !== account.probeGeneration || current.probe_lease?.token !== account.probeToken)
    ) return null;
    return withSlot(state, account.slot, {
      ...current,
      account_id_hash: account.accountIdHash,
      quota_blocked_until_ms: null,
      quota_block_source: null,
      invalid_credential_version: account.credentialVersion,
      probe_lease: null,
    });
  });
};

/**
 * OAuth refresh can replace a token while an inference attempt is in flight.
 * Treat the token as a new credential version while retaining the account's
 * stable reset observation, so a refresh cannot manufacture a second key.
 */
export const reconcileCodexRoutingAccount = async (
  account: RoutingAccount,
  auth: CodexAuthState,
): Promise<RoutingAccount> => {
  const { accountIdHash, credentialVersion } = await routingAccountIdentity(auth);
  // The normal refresh check can return unchanged credentials. Preserve any
  // half-open probe fence so a successful response can clear the quota
  // circuit claimed for this request.
  if (credentialVersion === account.credentialVersion) return { ...account, auth, accountIdHash };

  const reconciled: RoutingAccount = {
    ...account,
    auth,
    accountIdHash,
    credentialVersion,
    probeRequired: false,
    probeGeneration: null,
    probeToken: null,
  };

  // Credential rotation is exceptional. For the same account it releases
  // ordinary routing but retains a stable reset identity as lookup-only; only
  // a genuinely different account starts a neutral reset scope.
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (current.credential_version === credentialVersion) return null;
    if (!slotMatchesRoutingAccount(current, account)) return null;
    return withSlot(
      state,
      account.slot,
      auth.account_id === account.auth.account_id
        ? rotateCredentialForSameAccount(current, { accountIdHash, credentialVersion })
        : neutralSlot(credentialVersion, accountIdHash),
    );
  });
  return reconciled;
};

export const releaseCodexRoutingProbe = async (account: RoutingAccount): Promise<void> => {
  if (account.probeGeneration === null || !account.probeToken) return;
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (
      !slotMatchesRoutingAccount(current, account) ||
      current.generation !== account.probeGeneration ||
      current.probe_lease?.generation !== account.probeGeneration ||
      current.probe_lease?.token !== account.probeToken
    ) return null;
    return withSlot(state, account.slot, {
      ...current,
      account_id_hash: account.accountIdHash,
      quota_blocked_until_ms: null,
      quota_block_source: null,
      probe_lease: null,
    });
  });
};

/** A successful recovery probe is the only trusted way to clear reset ambiguity. */
export const markCodexSuccess = async (account: RoutingAccount): Promise<void> => {
  if (account.probeGeneration === null || !account.probeToken) return;
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (
      !slotMatchesRoutingAccount(current, account) ||
      current.generation !== account.probeGeneration ||
      current.probe_lease?.generation !== account.probeGeneration ||
      current.probe_lease?.token !== account.probeToken
    ) return null;
    return withSlot(state, account.slot, {
      ...current,
      account_id_hash: account.accountIdHash,
      quota_blocked_until_ms: null,
      quota_block_source: null,
      observed_reset_at_ms: null,
      observed_reset_at_is_stable: false,
      banked_reset_generation_ambiguous: false,
      probe_lease: null,
    });
  });
};

/**
 * Pure exact-fence predicate used by both strong-read helpers and the banked
 * reset's atomic KV checks. Equality (rather than `>=`) prevents a shorter
 * later Retry-After from authorizing a reset against a longer existing block;
 * an unresolved legacy account association denies every new claim.
 */
export const isCodexQuotaBlockFenceCurrent = (
  value: unknown,
  account: RoutingAccount,
  quotaResetAtMs: number,
  routingGeneration: number,
): boolean => {
  if (
    !isSafeMs(quotaResetAtMs) || !Number.isSafeInteger(routingGeneration) || routingGeneration < 0
  ) return false;
  const state = parseCodexAccountRoutingState(value);
  const current = state?.slots[account.slot];
  return !hasUnresolvedLegacyResetIdentity(state) && current?.credential_version === account.credentialVersion &&
    current.account_id_hash === account.accountIdHash &&
    current.generation === routingGeneration &&
    (current.quota_block_source === "body_resets_at" || current.quota_block_source === "header_retry_after") &&
    current.observed_reset_at_ms === quotaResetAtMs &&
    current.observed_reset_at_is_stable === true &&
    current.banked_reset_generation_ambiguous === false &&
    current.quota_blocked_until_ms === quotaResetAtMs;
};

/**
 * Returns a durable routing fence for a fully persisted quota observation.
 * Account routing otherwise fails open when KV is down; banked redemption is
 * stricter and must refuse to start without this strong-read proof.
 */
export const getCodexQuotaBlockFence = async (
  account: RoutingAccount,
  quotaResetAtMs: number,
): Promise<number | null> => {
  if (!isSafeMs(quotaResetAtMs)) return null;
  let kv: Deno.Kv | null;
  try {
    kv = await getKv();
  } catch {
    return null;
  }
  if (!kv) return null;
  try {
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
    const state = parseCodexAccountRoutingState(entry.value);
    const generation = state?.slots[account.slot]?.generation;
    return generation !== undefined && isCodexQuotaBlockFenceCurrent(entry.value, account, quotaResetAtMs, generation)
      ? generation
      : null;
  } catch {
    return null;
  }
};

/**
 * An additional immutable record that must remain current while a verified
 * reset clears its routing circuit. The gateway uses this for the auth-pool
 * slot, so a credential rotation cannot turn a verified reset into a retry
 * with a removed credential.
 */
export type CodexQuotaResetReconciliationFence = Readonly<{
  key: Deno.KvKey;
  isCurrent: (value: unknown) => boolean;
}>;

const readCurrentReconciliationFences = async (
  kv: Deno.Kv,
  fences: readonly CodexQuotaResetReconciliationFence[],
): Promise<readonly Deno.KvEntryMaybe<unknown>[] | null> => {
  if (
    !Array.isArray(fences) ||
    !fences.every((fence) => isRecord(fence) && Array.isArray(fence.key) && typeof fence.isCurrent === "function")
  ) return null;
  const entries: Deno.KvEntryMaybe<unknown>[] = [];
  for (const fence of fences) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get<unknown>(fence.key, { consistency: "strong" });
      if (!fence.isCurrent(entry.value)) return null;
    } catch {
      return null;
    }
    entries.push(entry);
  }
  return entries;
};

const withReconciliationFences = (
  operation: Deno.AtomicOperation,
  entries: readonly Deno.KvEntryMaybe<unknown>[],
): Deno.AtomicOperation => {
  let next = operation;
  for (const entry of entries) next = next.check(entry);
  return next;
};

/**
 * A verified reset may release only the exact circuit observation that caused
 * it. Preserve that stable observation as an ambiguous tombstone and claim a
 * fenced recovery probe for the one post-reset inference retry. The lease
 * prevents an older ordinary request from turning its delayed 429 into a new
 * quota identity before that retry proves the account healthy. Credential and
 * generation checks also prevent an old provider transaction from admitting a
 * newly rotated credential or a later quota window.
 */
export const reconcileCodexQuotaAfterVerifiedReset = async (
  account: RoutingAccount,
  input: Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
    fences?: readonly CodexQuotaResetReconciliationFence[];
  }>,
): Promise<RoutingAccount | null> => {
  if (
    !isSafeMs(input.quotaResetAtMs) || !Number.isSafeInteger(input.routingGeneration) || input.routingGeneration < 0
  ) {
    return null;
  }
  let kv: Deno.Kv | null;
  try {
    kv = await getKv();
  } catch {
    return null;
  }
  if (!kv) return null;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    try {
      const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
      const state = parseCodexAccountRoutingState(entry.value);
      if (
        !state || !isCodexQuotaBlockFenceCurrent(entry.value, account, input.quotaResetAtMs, input.routingGeneration)
      ) {
        return null;
      }
      const fenceEntries = input.fences === undefined ? [] : await readCurrentReconciliationFences(kv, input.fences);
      if (!fenceEntries) return null;
      const current = state.slots[account.slot]!;
      const nextGeneration = current.generation + 1;
      const nowMs = Date.now();
      const probeExpiresAtMs = nowMs + CODEX_HALF_OPEN_LEASE_MS;
      if (!Number.isSafeInteger(nextGeneration) || !isSafeMs(nowMs) || !isSafeMs(probeExpiresAtMs)) return null;
      const recoveryLease = {
        token: crypto.randomUUID(),
        expires_at_ms: probeExpiresAtMs,
        generation: nextGeneration,
      };
      const next = withSlot(state, account.slot, {
        ...current,
        quota_blocked_until_ms: null,
        quota_block_source: null,
        // The verified reset makes normal routing eligible, but an absolute
        // Retry-After cannot prove whether a delayed response names this old
        // window or a new one. Keep D1 lookup-only until a recovery probe
        // independently proves the circuit healthy.
        observed_reset_at_ms: current.observed_reset_at_ms,
        observed_reset_at_is_stable: current.observed_reset_at_is_stable,
        banked_reset_generation_ambiguous: true,
        generation: nextGeneration,
        probe_lease: recoveryLease,
      });
      const committed = await withReconciliationFences(kv.atomic().check(entry), fenceEntries)
        .set(CODEX_ACCOUNT_ROUTING_KV_KEY, next)
        .commit();
      if (!committed.ok) continue;
      cachedState = next;
      cachedVersionstamp = committed.versionstamp;
      cachedStateLoadedAtMs = Date.now();
      return {
        ...account,
        quotaHeadroom: quotaHeadroomFor(next.slots[account.slot]!),
        probeRequired: false,
        probeGeneration: recoveryLease.generation,
        probeToken: recoveryLease.token,
      };
    } catch {
      return null;
    }
  }
  return null;
};

/**
 * Recover an account whose verified reset record predates later routing
 * transitions. The ledger proves the reset was already spent, so this path
 * only fences a new inference probe; it never submits another reset.
 */
export const reconcileCodexQuotaAfterStaleVerifiedReset = async (
  account: RoutingAccount,
  input: Readonly<{
    quotaResetAtMs: number;
    routingGeneration: number;
    fences?: readonly CodexQuotaResetReconciliationFence[];
  }>,
): Promise<RoutingAccount | null> => {
  if (
    !isSafeMs(input.quotaResetAtMs) || !Number.isSafeInteger(input.routingGeneration) || input.routingGeneration < 0
  ) return null;
  let kv: Deno.Kv | null;
  try {
    kv = await getKv();
  } catch {
    return null;
  }
  if (!kv) return null;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    try {
      const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
      const state = parseCodexAccountRoutingState(entry.value);
      const current = state?.slots[account.slot];
      const nowMs = Date.now();
      if (
        !state || state.banked_reset_legacy_identity_unresolved || !current ||
        !slotMatchesRoutingAccount(current, account) || current.account_id_hash !== account.accountIdHash ||
        current.credential_version !== account.credentialVersion || current.generation !== input.routingGeneration ||
        current.invalid_credential_version === account.credentialVersion ||
        current.quota_blocked_until_ms === null || current.quota_blocked_until_ms < input.quotaResetAtMs ||
        current.observed_reset_at_ms !== input.quotaResetAtMs || !current.observed_reset_at_is_stable ||
        (current.quota_block_source !== "body_resets_at" && current.quota_block_source !== "header_retry_after") ||
        (current.probe_lease?.expires_at_ms ?? 0) > nowMs
      ) return null;
      const fenceEntries = input.fences === undefined ? [] : await readCurrentReconciliationFences(kv, input.fences);
      if (!fenceEntries) return null;
      const nextGeneration = current.generation + 1;
      const probeExpiresAtMs = nowMs + CODEX_HALF_OPEN_LEASE_MS;
      if (!Number.isSafeInteger(nextGeneration) || !isSafeMs(nowMs) || !isSafeMs(probeExpiresAtMs)) return null;
      const recoveryLease = {
        token: crypto.randomUUID(),
        expires_at_ms: probeExpiresAtMs,
        generation: nextGeneration,
      };
      const next = withSlot(state, account.slot, {
        ...current,
        quota_blocked_until_ms: null,
        quota_block_source: null,
        banked_reset_generation_ambiguous: true,
        generation: nextGeneration,
        probe_lease: recoveryLease,
      });
      const committed = await withReconciliationFences(kv.atomic().check(entry), fenceEntries)
        .set(CODEX_ACCOUNT_ROUTING_KV_KEY, next)
        .commit();
      if (!committed.ok) continue;
      cachedState = next;
      cachedVersionstamp = committed.versionstamp;
      cachedStateLoadedAtMs = Date.now();
      return {
        ...account,
        quotaHeadroom: quotaHeadroomFor(next.slots[account.slot]!),
        probeRequired: false,
        probeGeneration: recoveryLease.generation,
        probeToken: recoveryLease.token,
      };
    } catch {
      return null;
    }
  }
  return null;
};

const claimExpiredProbe = async (
  state: CodexAccountRoutingState,
  account: RoutingAccount,
  now: number,
): Promise<RoutingAccount | null> => {
  const buildClaim = (
    base: CodexAccountRoutingState,
  ): { next: CodexAccountRoutingState; lease: CodexRoutingSlot["probe_lease"] } | null => {
    const current = slotFor(base, account);
    if (
      !slotMatchesRoutingAccount(current, account) ||
      current.invalid_credential_version === account.credentialVersion ||
      !current.quota_blocked_until_ms ||
      current.quota_blocked_until_ms > now ||
      (current.probe_lease?.expires_at_ms ?? 0) > now
    ) return null;
    const lease = {
      token: crypto.randomUUID(),
      expires_at_ms: now + CODEX_HALF_OPEN_LEASE_MS,
      generation: current.generation,
    };
    return {
      next: withSlot(base, account.slot, {
        ...current,
        account_id_hash: account.accountIdHash,
        probe_lease: lease,
      }),
      lease,
    };
  };
  try {
    const kv = await getKv();
    if (kv) {
      for (let attempt = 0; attempt < 3; attempt += 1) {
        const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
        const durable = parseCodexAccountRoutingState(entry.value);
        const claimed = buildClaim(durable ?? state);
        if (!claimed) return null;
        const commit = await kv.atomic().check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, claimed.next).commit();
        if (!commit.ok) continue;
        cachedState = claimed.next;
        cachedVersionstamp = commit.versionstamp;
        cachedStateLoadedAtMs = Date.now();
        return {
          ...account,
          probeRequired: false,
          probeGeneration: claimed.lease!.generation,
          probeToken: claimed.lease!.token,
        };
      }
      return null;
    }
    const claimed = buildClaim(state);
    if (!claimed) return null;
    cachedState = claimed.next;
    cachedVersionstamp = undefined;
    cachedStateLoadedAtMs = Date.now();
    return {
      ...account,
      probeRequired: false,
      probeGeneration: claimed.lease!.generation,
      probeToken: claimed.lease!.token,
    };
  } catch {
    // Fail open if KV itself is unavailable. This retains availability but not cross-isolate coordination.
    const claimed = buildClaim(state);
    if (!claimed) return null;
    cachedState = claimed.next;
    cachedVersionstamp = undefined;
    cachedStateLoadedAtMs = Date.now();
    return {
      ...account,
      probeRequired: false,
      probeGeneration: claimed.lease!.generation,
      probeToken: claimed.lease!.token,
    };
  }
};

export const claimCodexRoutingProbe = async (
  pool: CodexAuthPoolState,
  account: RoutingAccount,
  now = Date.now(),
): Promise<RoutingAccount | null> => {
  if (!account.probeRequired) return account;
  const state = await loadCodexAccountRouting(pool);
  return await claimExpiredProbe(state, account, now);
};

const selectCodexRoutingAccountsFromState = async (
  state: CodexAccountRoutingState,
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now: number,
): Promise<RouteSelection> => {
  const identities = await Promise.all(pool.accounts.map(routingAccountIdentity));
  const byId = new Map(
    pool.accounts.map((auth, slot) => [auth.account_id, { slot, ...identities[slot]! }]),
  );
  const available: RoutingAccount[] = [];
  const blockedAccounts: CodexBlockedRoutingAccount[] = [];
  const skipped: number[] = [];
  let retryAt: number | null = null;
  let hasQuotaBlock = false;
  for (const auth of orderedAccounts) {
    const mapped = byId.get(auth.account_id);
    if (!mapped) continue;
    const account: RoutingAccount = {
      auth,
      slot: mapped.slot,
      accountIdHash: mapped.accountIdHash,
      credentialVersion: mapped.credentialVersion,
      quotaHeadroom: null,
      probeRequired: false,
      probeGeneration: null,
      probeToken: null,
    };
    const storedSlot = slotFor(state, account);
    const slot = slotMatchesRoutingAccount(storedSlot, account)
      ? storedSlot
      : neutralSlot(account.credentialVersion, account.accountIdHash);
    const routedAccount = { ...account, quotaHeadroom: quotaHeadroomFor(slot) };
    if (slot.invalid_credential_version === account.credentialVersion) {
      skipped.push(mapped.slot + 1);
      continue;
    }
    if (slot.quota_blocked_until_ms && slot.quota_blocked_until_ms > now) {
      skipped.push(mapped.slot + 1);
      hasQuotaBlock = true;
      retryAt = retryAt === null ? slot.quota_blocked_until_ms : Math.min(retryAt, slot.quota_blocked_until_ms);
      if (
        (slot.quota_block_source === "body_resets_at" || slot.quota_block_source === "header_retry_after") &&
        slot.observed_reset_at_ms !== null &&
        slot.observed_reset_at_is_stable
      ) {
        blockedAccounts.push({
          ...routedAccount,
          quotaResetAtMs: slot.observed_reset_at_ms,
          routingGeneration: slot.generation,
        });
      }
      continue;
    }
    // A verified banked reset releases the quota deadline but retains its
    // recovery-probe lease. Ordinary routing stays unavailable until that
    // exact fenced probe succeeds, fails, or expires.
    if ((slot.probe_lease?.expires_at_ms ?? 0) > now) {
      skipped.push(mapped.slot + 1);
      hasQuotaBlock = true;
      retryAt = retryAt === null ? slot.probe_lease!.expires_at_ms : Math.min(retryAt, slot.probe_lease!.expires_at_ms);
      continue;
    }
    if (slot.quota_blocked_until_ms) {
      // Claim the half-open lease only if request execution actually reaches
      // this slot. This preserves first/second order without abandoning a
      // secondary lease when the healthy first account returns directly.
      available.push({ ...routedAccount, probeRequired: true });
      continue;
    }
    available.push(routedAccount);
  }
  if (available.length) return { kind: "eligible", accounts: available, skippedSlots: skipped, blockedAccounts };
  if (hasQuotaBlock) return { kind: "quota_blocked", skippedSlots: skipped, retryAtMs: retryAt, blockedAccounts };
  return { kind: "credentials_invalid", skippedSlots: skipped };
};

export const selectCodexRoutingAccounts = async (
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now = Date.now(),
): Promise<RouteSelection> =>
  await selectCodexRoutingAccountsFromState(
    await loadCodexAccountRouting(pool),
    pool,
    orderedAccounts,
    now,
  );

/**
 * Re-selects accounts from a fresh durable routing record. Unlike ordinary
 * routing, this path never synthesizes or reuses local state: it guards the
 * fallback dispatch after a partial banked-reset preflight may have awaited
 * work long enough for another isolate to block a sibling.
 */
export const selectCodexRoutingAccountsStrong = async (
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now = Date.now(),
): Promise<StrongRouteSelection> => {
  try {
    const kv = await getKv();
    if (!kv) return { kind: "routing_unavailable" };
    const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      consistency: "strong",
    });
    const durable = parseCodexAccountRoutingState(entry.value);
    if (!durable) return { kind: "routing_unavailable" };
    const normalized = await normalizeRoutingState(durable, pool);
    cachedState = normalized;
    cachedVersionstamp = entry.versionstamp;
    cachedStateLoadedAtMs = Date.now();
    return await selectCodexRoutingAccountsFromState(normalized, pool, orderedAccounts, now);
  } catch {
    return { kind: "routing_unavailable" };
  }
};

/**
 * Makes a manually redeemed reset eligible for one normal-request probe.
 * This never clears reset-generation ambiguity: only a successful probe can
 * prove recovery and authorize a later provisional identity.
 */
export const recheckCodexRoutingSlot = async (slotNumber: number): Promise<boolean> => {
  if (!Number.isInteger(slotNumber) || slotNumber < 1) return false;

  // Rechecks are rare, administrative transitions. Always use a strong read
  // when available so a cold isolate can release a persisted circuit and a
  // stale local cache cannot overwrite a newer quota deadline.
  let kv: Deno.Kv | null = null;
  try {
    kv = await getKv();
  } catch {
    kv = null;
  }
  if (kv) {
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const entry = await kv.get<CodexAccountRoutingState>(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" });
      const state = parseCodexAccountRoutingState(entry.value);
      if (!state || slotNumber > state.slots.length) return false;
      const index = slotNumber - 1;
      const current = state.slots[index]!;
      if (!current.quota_blocked_until_ms) {
        cachedState = state;
        cachedVersionstamp = entry.versionstamp;
        cachedStateLoadedAtMs = Date.now();
        return true;
      }
      const next = withSlot(state, index, {
        ...current,
        quota_blocked_until_ms: Date.now(),
        // An administrative deadline mutation is not provider proof of the
        // same quota generation. A later recovery probe is required before a
        // stable identity may authorize a banked claim again.
        banked_reset_generation_ambiguous: current.banked_reset_generation_ambiguous ||
          (current.observed_reset_at_ms !== null && current.observed_reset_at_is_stable),
        generation: current.generation + 1,
        probe_lease: null,
      });
      const committed = await kv.atomic().check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, next).commit();
      if (committed.ok) {
        cachedState = next;
        cachedVersionstamp = committed.versionstamp;
        cachedStateLoadedAtMs = Date.now();
        return true;
      }
    }
    return false;
  }

  const state = cachedState;
  if (!state || slotNumber > state.slots.length) return false;
  const index = slotNumber - 1;
  const current = state.slots[index]!;
  if (!current.quota_blocked_until_ms) return true;
  cachedState = withSlot(state, index, {
    ...current,
    quota_blocked_until_ms: Date.now(),
    banked_reset_generation_ambiguous: current.banked_reset_generation_ambiguous ||
      (current.observed_reset_at_ms !== null && current.observed_reset_at_is_stable),
    generation: current.generation + 1,
    probe_lease: null,
  });
  cachedVersionstamp = undefined;
  cachedStateLoadedAtMs = Date.now();
  return true;
};
