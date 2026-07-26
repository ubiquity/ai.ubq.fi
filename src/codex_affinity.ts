import { sha256Hex } from "./utils.ts";

export type WeightedCandidate<T> = Readonly<{ value: T; id: string; weight?: number }>;

const MAX_AFFINITY_ENTRIES = 4_096;
const AFFINITY_TTL_MS = 6 * 60 * 60_000;

/** Hashes only the selection input; raw session identifiers never leave RAM. */
export const deriveCodexAffinityKey = async (body: unknown): Promise<string | null> => {
  if (!body || typeof body !== "object" || Array.isArray(body)) return null;
  const record = body as Record<string, unknown>;
  const metadata = record.client_metadata;
  const session = metadata && typeof metadata === "object" && !Array.isArray(metadata)
    ? (metadata as Record<string, unknown>).session_id
    : undefined;
  const raw = typeof record.prompt_cache_key === "string" && record.prompt_cache_key
    ? record.prompt_cache_key
    : typeof session === "string" && session
    ? session
    : typeof record.thread_id === "string" && record.thread_id
    ? record.thread_id
    : null;
  return raw ? await sha256Hex(raw) : null;
};

/** Weighted rendezvous hashing: the lowest score wins and equal/unknown weights are fair. */
export const selectWeightedRendezvous = async <T>(
  affinityKey: string,
  candidates: readonly WeightedCandidate<T>[],
): Promise<T | null> => {
  let selected: T | null = null;
  let selectedScore = Infinity;
  for (const candidate of candidates) {
    const hash = await sha256Hex(`${affinityKey}\u0000${candidate.id}`);
    // Thirteen hexadecimal digits contain 52 bits. Keep the denominator in
    // the same range: using the 53-bit maximum would quietly halve every
    // draw and distort weighted rendezvous placement.
    const unit = Number.parseInt(hash.slice(0, 13), 16) / 0x0fffffffffffff;
    const weight = Number.isFinite(candidate.weight) && (candidate.weight ?? 0) > 0 ? candidate.weight! : 1;
    // -ln(u) / weight gives the standard weighted rendezvous ordering.
    const score = -Math.log(Math.max(Number.MIN_VALUE, unit)) / weight;
    if (score < selectedScore) {
      selected = candidate.value;
      selectedScore = score;
    }
  }
  return selected;
};

type CachedAssignment = Readonly<{ accountId: string; expiresAtMs: number }>;

export class CodexAffinityCache {
  #entries = new Map<string, CachedAssignment>();

  get(key: string, eligibleAccountIds: ReadonlySet<string>, now = Date.now()): string | null {
    const item = this.#entries.get(key);
    if (!item || item.expiresAtMs <= now || !eligibleAccountIds.has(item.accountId)) {
      this.#entries.delete(key);
      return null;
    }
    this.#entries.delete(key);
    this.#entries.set(key, item);
    return item.accountId;
  }

  set(key: string, accountId: string, now = Date.now()): void {
    this.#entries.delete(key);
    this.#entries.set(key, { accountId, expiresAtMs: now + AFFINITY_TTL_MS });
    while (this.#entries.size > MAX_AFFINITY_ENTRIES) this.#entries.delete(this.#entries.keys().next().value!);
  }

  clear(): void {
    this.#entries.clear();
  }
}
