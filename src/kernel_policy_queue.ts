import { getKv } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";
import type { KernelPolicyQueueItem } from "./types.ts";

const UOS_KERNEL_POLICY_QUEUE_KEY = ["uos_ai", "kernel_policy_queue"] as const;
const MAX_KERNEL_POLICY_QUEUE_ITEMS = 200;
const MAX_KV_RETRIES = 3;

const toNumber = (value: unknown, fallback: number): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  return Math.trunc(value);
};

const normalizeOwnerRepo = (value: unknown): string => {
  const raw = getString(value);
  const trimmed = raw?.trim() ?? "";
  return trimmed;
};

const normalizeQueueItem = (value: unknown, nowMs: number): KernelPolicyQueueItem | null => {
  if (!isRecord(value)) return null;
  const owner = normalizeOwnerRepo(value.owner);
  const repo = normalizeOwnerRepo(value.repo);
  if (!owner || !repo) return null;
  const requestCount = Math.max(0, toNumber(value.request_count, 0));
  const firstSeen = toNumber(value.first_seen_at_ms, nowMs);
  const lastSeen = toNumber(value.last_seen_at_ms, nowMs);
  const lastRouteRaw = normalizeOwnerRepo(value.last_route);
  return {
    owner,
    repo,
    request_count: requestCount,
    first_seen_at_ms: firstSeen,
    last_seen_at_ms: lastSeen,
    last_route: lastRouteRaw ? lastRouteRaw : null,
  };
};

export const recordKernelPolicyQueue = async (
  owner: string,
  repo: string,
  route: string,
): Promise<void> => {
  try {
    const ownerName = normalizeOwnerRepo(owner);
    const repoName = normalizeOwnerRepo(repo);
    if (!ownerName || !repoName) return;
    const routeValue = normalizeOwnerRepo(route);
    const routeEntry = routeValue ? routeValue : null;

    const kv = await getKv();
    if (!kv) return;
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const entry = await kv.get<KernelPolicyQueueItem[]>(UOS_KERNEL_POLICY_QUEUE_KEY);
      const existing = Array.isArray(entry.value) ? entry.value : [];
      const normalized = existing
        .map((item) => normalizeQueueItem(item, nowMs))
        .filter((item): item is KernelPolicyQueueItem => Boolean(item));
      const index = normalized.findIndex((item) => item.owner === ownerName && item.repo === repoName);
      let next = normalized;

      if (index >= 0) {
        const current = normalized[index];
        const updated: KernelPolicyQueueItem = {
          ...current,
          request_count: current.request_count + 1,
          last_seen_at_ms: nowMs,
          last_route: routeEntry ?? current.last_route,
        };
        next = [...normalized];
        next[index] = updated;
      } else {
        next = [
          {
            owner: ownerName,
            repo: repoName,
            request_count: 1,
            first_seen_at_ms: nowMs,
            last_seen_at_ms: nowMs,
            last_route: routeEntry,
          },
          ...normalized,
        ];
      }

      next.sort((a, b) => b.last_seen_at_ms - a.last_seen_at_ms);
      if (next.length > MAX_KERNEL_POLICY_QUEUE_ITEMS) {
        next = next.slice(0, MAX_KERNEL_POLICY_QUEUE_ITEMS);
      }

      const commit = await kv.atomic().check(entry).set(UOS_KERNEL_POLICY_QUEUE_KEY, next).commit();
      if (commit.ok) return;
    }

    console.warn("[ai.ubq.fi] Failed to record kernel policy queue after retries:", `${ownerName}/${repoName}`);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record kernel policy queue:", error);
  }
};

export const listKernelPolicyQueue = async (): Promise<KernelPolicyQueueItem[] | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const nowMs = Date.now();
    const entry = await kv.get<KernelPolicyQueueItem[]>(UOS_KERNEL_POLICY_QUEUE_KEY);
    const existing = Array.isArray(entry.value) ? entry.value : [];
    const normalized = existing
      .map((item) => normalizeQueueItem(item, nowMs))
      .filter((item): item is KernelPolicyQueueItem => Boolean(item));
    normalized.sort((a, b) => b.last_seen_at_ms - a.last_seen_at_ms);
    return normalized;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to list kernel policy queue:", error);
    return null;
  }
};
