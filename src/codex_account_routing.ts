import { getKv } from "./kv.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type { CodexAuthPoolState, CodexAuthState } from "./types.ts";

/**
 * Durable, slot-indexed routing state.  It intentionally contains neither
 * account ids nor credentials: an auth-pool replacement can therefore never
 * accidentally make a previous account eligible.
 */
export const CODEX_ACCOUNT_ROUTING_KV_KEY = ["uos_ai", "codex_account_routing", "v1"] as const;
export const CODEX_HALF_OPEN_LEASE_MS = 30_000;
export const CODEX_DEFAULT_429_COOLDOWN_MS = 60_000;
const MAX_429_BODY_BYTES = 64 * 1024;
// A warm isolate avoids per-request routing reads, but it must eventually
// observe circuits opened by another isolate. This bounded revalidation keeps
// normal traffic off KV while limiting cross-isolate stale routing decisions.
const ROUTING_CACHE_REVALIDATE_MS = 5_000;

export type CodexQuotaBlockSource = "resets_at" | "header_reset_at" | "header_retry_after" | "cooldown";

export type CodexRoutingSlot = Readonly<{
  credential_version: string;
  quota_blocked_until_ms: number | null;
  quota_block_source: CodexQuotaBlockSource | null;
  invalid_credential_version: string | null;
  primary_used_percent: number | null;
  secondary_used_percent: number | null;
  observed_reset_at_ms: number | null;
  generation: number;
  probe_lease: Readonly<{ token: string; expires_at_ms: number; generation: number }> | null;
}>;

export type CodexAccountRoutingState = Readonly<{
  v: 1;
  updated_at_ms: number;
  slots: readonly CodexRoutingSlot[];
}>;

export type RoutingAccount = Readonly<{
  auth: CodexAuthState;
  slot: number;
  credentialVersion: string;
  quotaHeadroom: number | null;
  probeGeneration: number | null;
  probeToken: string | null;
}>;

export type RouteSelection =
  | Readonly<{ kind: "eligible"; accounts: readonly RoutingAccount[]; skippedSlots: readonly number[] }>
  | Readonly<{ kind: "quota_blocked"; skippedSlots: readonly number[]; retryAtMs: number | null }>
  | Readonly<{ kind: "credentials_invalid"; skippedSlots: readonly number[] }>;

const isSafeMs = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const parseSlot = (value: unknown): CodexRoutingSlot | null => {
  if (!isRecord(value) || typeof value.credential_version !== "string") return null;
  const source = value.quota_block_source;
  if (
    source !== null && source !== "resets_at" && source !== "header_reset_at" && source !== "header_retry_after" &&
    source !== "cooldown"
  ) return null;
  const lease = value.probe_lease;
  const parsedLease = lease === null
    ? null
    : isRecord(lease) && typeof lease.token === "string" && isSafeMs(lease.expires_at_ms) &&
        typeof lease.generation === "number" && Number.isSafeInteger(lease.generation)
    ? { token: lease.token, expires_at_ms: lease.expires_at_ms, generation: lease.generation }
    : null;
  if (lease !== null && !parsedLease) return null;
  return {
    credential_version: value.credential_version,
    quota_blocked_until_ms: value.quota_blocked_until_ms === null || isSafeMs(value.quota_blocked_until_ms)
      ? value.quota_blocked_until_ms as number | null
      : null,
    quota_block_source: source as CodexQuotaBlockSource | null,
    invalid_credential_version: typeof value.invalid_credential_version === "string"
      ? value.invalid_credential_version
      : null,
    primary_used_percent: typeof value.primary_used_percent === "number" && Number.isFinite(value.primary_used_percent)
      ? value.primary_used_percent
      : null,
    secondary_used_percent:
      typeof value.secondary_used_percent === "number" && Number.isFinite(value.secondary_used_percent)
        ? value.secondary_used_percent
        : null,
    observed_reset_at_ms: value.observed_reset_at_ms === null || isSafeMs(value.observed_reset_at_ms)
      ? value.observed_reset_at_ms as number | null
      : null,
    generation: typeof value.generation === "number" && Number.isSafeInteger(value.generation) && value.generation >= 0
      ? value.generation
      : 0,
    probe_lease: parsedLease,
  };
};

export const parseCodexAccountRoutingState = (value: unknown): CodexAccountRoutingState | null => {
  if (!isRecord(value) || value.v !== 1 || !isSafeMs(value.updated_at_ms) || !Array.isArray(value.slots)) return null;
  const slots = value.slots.map(parseSlot);
  if (slots.some((slot) => !slot)) return null;
  return { v: 1, updated_at_ms: value.updated_at_ms, slots: slots as CodexRoutingSlot[] };
};

export const codexCredentialVersion = async (auth: CodexAuthState): Promise<string> =>
  await sha256Hex(`${auth.account_id}\u0000${auth.access_token}\u0000${auth.refresh_token}`);

const neutralSlot = (credentialVersion: string): CodexRoutingSlot => ({
  credential_version: credentialVersion,
  quota_blocked_until_ms: null,
  quota_block_source: null,
  invalid_credential_version: null,
  primary_used_percent: null,
  secondary_used_percent: null,
  observed_reset_at_ms: null,
  generation: 0,
  probe_lease: null,
});

export const normalizeRoutingState = async (
  raw: CodexAccountRoutingState | null,
  pool: CodexAuthPoolState,
  now = Date.now(),
): Promise<CodexAccountRoutingState> => {
  const versions = await Promise.all(pool.accounts.map(codexCredentialVersion));
  const slots = versions.map((version, index) => {
    const prior = raw?.slots[index];
    return prior?.credential_version === version ? prior : neutralSlot(version);
  });
  return { v: 1, updated_at_ms: now, slots };
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
    const credentialChanged = normalized.slots.length !== cachedState.slots.length ||
      normalized.slots.some((slot, index) => slot !== cachedState!.slots[index]);
    if (credentialChanged) cachedState = normalized;
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
  state.slots[account.slot] ?? neutralSlot(account.credentialVersion);

const withSlot = (
  state: CodexAccountRoutingState,
  index: number,
  slot: CodexRoutingSlot,
): CodexAccountRoutingState => {
  const slots = [...state.slots];
  while (slots.length <= index) slots.push(neutralSlot(slot.credential_version));
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

const futureUnixSecondsToMs = (value: unknown, now: number): number | null => {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) return null;
  const ms = value * 1000;
  return Number.isSafeInteger(ms) && ms > now ? ms : null;
};

const futureHeaderDeadline = (
  headers: Headers,
  now: number,
): { deadline: number; source: CodexQuotaBlockSource } | null => {
  for (const name of ["x-codex-primary-reset-at", "x-codex-secondary-reset-at"]) {
    const seconds = Number(headers.get(name));
    const deadline = futureUnixSecondsToMs(seconds, now);
    if (deadline) return { deadline, source: "header_reset_at" };
  }
  const retryAfterHeader = headers.get("retry-after");
  const retryAfter = Number(retryAfterHeader);
  if (Number.isFinite(retryAfter) && retryAfter > 0 && retryAfter <= 24 * 60 * 60) {
    return { deadline: now + Math.ceil(retryAfter * 1000), source: "header_retry_after" };
  }
  if (retryAfterHeader) {
    const deadline = Date.parse(retryAfterHeader);
    if (Number.isSafeInteger(deadline) && deadline > now) {
      return { deadline, source: "header_retry_after" };
    }
  }
  return null;
};

/**
 * Read a bounded error body and replace the response so callers retain the
 * OpenAI-compatible upstream payload after routing has classified it.
 */
export const readCodex429 = async (response: Response): Promise<{ response: Response; resetsAtMs: number | null }> => {
  const headers = new Headers(response.headers);
  const chunks: Uint8Array[] = [];
  let length = 0;
  let complete = !response.body;
  const reader = response.body?.getReader();
  let cancelPromise: Promise<void> | null = null;
  const cancelReader = (): void => {
    if (!reader || cancelPromise) return;
    try {
      // Do not await cancellation here: a broken upstream may leave its
      // pending read unsettled, but invoking cancel is enough to propagate
      // the caller's abort and must not extend the bounded classification
      // latency. The continuation releases the lock exactly once.
      cancelPromise = reader.cancel("Codex 429 classified").catch(() => {}).then(() => {
        try {
          reader.releaseLock();
        } catch {
          // A reader can already be released after an upstream failure.
        }
      });
    } catch {
      try {
        reader.releaseLock();
      } catch {
        // Best effort only.
      }
    }
  };
  try {
    if (reader) {
      // Error payloads are small but may be fragmented. Read up to the fixed
      // ceiling under one short deadline so classification is both accurate
      // for normal JSON and bounded for a broken infinite response body.
      const deadline = AbortSignal.timeout(1_000);
      let timedOut = false;
      while (length < MAX_429_BODY_BYTES && !timedOut) {
        let timeoutListener: (() => void) | undefined;
        const next = await Promise.race([
          reader.read(),
          new Promise<never>((_, reject) => {
            timeoutListener = () => {
              timedOut = true;
              reject(new DOMException("Codex 429 body read timed out.", "TimeoutError"));
            };
            deadline.addEventListener("abort", timeoutListener, { once: true });
          }),
        ]).finally(() => {
          if (timeoutListener) deadline.removeEventListener("abort", timeoutListener);
        });
        if (next.done) {
          complete = true;
          break;
        }
        const remaining = MAX_429_BODY_BYTES - length;
        const part = next.value.slice(0, remaining);
        chunks.push(part);
        length += part.byteLength;
        if (next.value.byteLength > part.byteLength) break;
      }
    }
  } catch {
    // A response body is optional for classification; preserve what was read.
  } finally {
    cancelReader();
  }
  const bytes = new Uint8Array(Math.min(length, MAX_429_BODY_BYTES));
  let offset = 0;
  for (const chunk of chunks) {
    const part = chunk.slice(0, Math.max(0, bytes.byteLength - offset));
    bytes.set(part, offset);
    offset += part.byteLength;
  }
  let resetsAtMs: number | null = null;
  if (complete) {
    try {
      const body = JSON.parse(new TextDecoder().decode(bytes));
      const error = isRecord(body) && isRecord(body.error) ? body.error : null;
      if (getString(error?.type) === "usage_limit_reached") {
        resetsAtMs = futureUnixSecondsToMs(error?.resets_at, Date.now());
      }
    } catch {
      // Header fallbacks below cover non-JSON payloads.
    }
  }
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
      resetsAtMs: null,
    };
  }
  return {
    response: new Response(bytes, { status: response.status, statusText: response.statusText, headers }),
    resetsAtMs,
  };
};

export const markCodexQuotaBlocked = async (
  account: RoutingAccount,
  response: Response,
  now = Date.now(),
): Promise<Response> => {
  const parsed = await readCodex429(response);
  const fallback = futureHeaderDeadline(parsed.response.headers, now);
  const candidate = parsed.resetsAtMs
    ? { deadline: parsed.resetsAtMs, source: "resets_at" as const }
    : fallback ?? { deadline: now + CODEX_DEFAULT_429_COOLDOWN_MS, source: "cooldown" as const };
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (current.credential_version !== account.credentialVersion) return null;
    if (
      account.probeGeneration !== null &&
      (current.generation !== account.probeGeneration || current.probe_lease?.token !== account.probeToken)
    ) return null;
    const deadline = Math.max(current.quota_blocked_until_ms ?? 0, candidate.deadline);
    const nextSlot: CodexRoutingSlot = {
      ...current,
      quota_blocked_until_ms: deadline,
      quota_block_source: deadline === current.quota_blocked_until_ms ? current.quota_block_source : candidate.source,
      primary_used_percent: parseFinitePercent(parsed.response.headers.get("x-codex-primary-used-percent")) ??
        current.primary_used_percent,
      secondary_used_percent: parseFinitePercent(parsed.response.headers.get("x-codex-secondary-used-percent")) ??
        current.secondary_used_percent,
      observed_reset_at_ms: parsed.resetsAtMs ?? current.observed_reset_at_ms,
      generation: current.generation + 1,
      probe_lease: null,
    };
    return withSlot(state, account.slot, nextSlot);
  });
  return parsed.response;
};

export const markCodexCredentialInvalid = async (account: RoutingAccount): Promise<void> => {
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (current.credential_version !== account.credentialVersion) return null;
    if (
      account.probeGeneration !== null &&
      (current.generation !== account.probeGeneration || current.probe_lease?.token !== account.probeToken)
    ) return null;
    return withSlot(state, account.slot, { ...current, invalid_credential_version: account.credentialVersion });
  });
};

/**
 * OAuth refresh can replace a token while an inference attempt is in flight.
 * Treat that replacement as a new credential version before recording its
 * result, so an old circuit or invalid marker cannot poison the new token.
 */
export const reconcileCodexRoutingAccount = async (
  account: RoutingAccount,
  auth: CodexAuthState,
): Promise<RoutingAccount> => {
  const credentialVersion = await codexCredentialVersion(auth);
  // The normal refresh check can return unchanged credentials. Preserve any
  // half-open probe fence so a successful response can clear the quota
  // circuit claimed for this request.
  if (credentialVersion === account.credentialVersion) return { ...account, auth };

  const reconciled: RoutingAccount = {
    ...account,
    auth,
    credentialVersion,
    probeGeneration: null,
    probeToken: null,
  };

  // Credential rotation is exceptional and must durably neutralize only its
  // slot and release its obsolete probe fence before the retried response is
  // classified. The CAS protects a newer cross-isolate replacement from an
  // in-flight request carrying old auth.
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (current.credential_version === credentialVersion) return null;
    return withSlot(state, account.slot, neutralSlot(credentialVersion));
  });
  return reconciled;
};

export const markCodexSuccess = async (account: RoutingAccount): Promise<void> => {
  if (account.probeGeneration === null || !account.probeToken) return;
  await updateRoutingState((state) => {
    const current = slotFor(state, account);
    if (
      current.credential_version !== account.credentialVersion ||
      current.generation !== account.probeGeneration ||
      current.probe_lease?.generation !== account.probeGeneration ||
      current.probe_lease?.token !== account.probeToken
    ) return null;
    return withSlot(state, account.slot, {
      ...current,
      quota_blocked_until_ms: null,
      quota_block_source: null,
      probe_lease: null,
    });
  });
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
      current.credential_version !== account.credentialVersion ||
      !current.quota_blocked_until_ms ||
      current.quota_blocked_until_ms > now ||
      (current.probe_lease?.expires_at_ms ?? 0) > now
    ) return null;
    const lease = {
      token: crypto.randomUUID(),
      expires_at_ms: now + CODEX_HALF_OPEN_LEASE_MS,
      generation: current.generation,
    };
    return { next: withSlot(base, account.slot, { ...current, probe_lease: lease }), lease };
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
      probeGeneration: claimed.lease!.generation,
      probeToken: claimed.lease!.token,
    };
  }
};

export const selectCodexRoutingAccounts = async (
  pool: CodexAuthPoolState,
  orderedAccounts: readonly CodexAuthState[],
  now = Date.now(),
): Promise<RouteSelection> => {
  const state = await loadCodexAccountRouting(pool);
  const versions = await Promise.all(pool.accounts.map(codexCredentialVersion));
  const byId = new Map(
    pool.accounts.map((auth, slot) => [auth.account_id, { slot, credentialVersion: versions[slot] }]),
  );
  const available: RoutingAccount[] = [];
  const expired: RoutingAccount[] = [];
  const skipped: number[] = [];
  let retryAt: number | null = null;
  let hasQuotaBlock = false;
  for (const auth of orderedAccounts) {
    const mapped = byId.get(auth.account_id);
    if (!mapped) continue;
    const account: RoutingAccount = {
      auth,
      slot: mapped.slot,
      credentialVersion: mapped.credentialVersion,
      quotaHeadroom: null,
      probeGeneration: null,
      probeToken: null,
    };
    const storedSlot = slotFor(state, account);
    const slot = storedSlot.credential_version === account.credentialVersion
      ? storedSlot
      : neutralSlot(account.credentialVersion);
    const routedAccount = { ...account, quotaHeadroom: quotaHeadroomFor(slot) };
    if (slot.invalid_credential_version === account.credentialVersion) {
      skipped.push(mapped.slot + 1);
      continue;
    }
    if (slot.quota_blocked_until_ms && slot.quota_blocked_until_ms > now) {
      skipped.push(mapped.slot + 1);
      hasQuotaBlock = true;
      retryAt = retryAt === null ? slot.quota_blocked_until_ms : Math.min(retryAt, slot.quota_blocked_until_ms);
      continue;
    }
    if (slot.quota_blocked_until_ms) expired.push(routedAccount);
    else available.push(routedAccount);
  }
  // A reset deserves exactly one controlled recovery probe even while another
  // account is healthy. Concurrent callers that lose the lease continue on
  // the healthy account below rather than stampeding the recovered slot.
  for (const candidate of expired) {
    const claimed = await claimExpiredProbe(state, candidate, now);
    if (claimed) {
      // Probe the recovered account first, but preserve any healthy sibling
      // for this same request if that controlled probe returns 401/429.
      return { kind: "eligible", accounts: [claimed, ...available], skippedSlots: skipped };
    }
    skipped.push(candidate.slot + 1);
    hasQuotaBlock = true;
  }
  if (available.length) return { kind: "eligible", accounts: available, skippedSlots: skipped };
  if (hasQuotaBlock) return { kind: "quota_blocked", skippedSlots: skipped, retryAtMs: retryAt };
  return { kind: "credentials_invalid", skippedSlots: skipped };
};

/** Makes a manually redeemed reset eligible for one normal-request probe. */
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
    generation: current.generation + 1,
    probe_lease: null,
  });
  cachedVersionstamp = undefined;
  cachedStateLoadedAtMs = Date.now();
  return true;
};
