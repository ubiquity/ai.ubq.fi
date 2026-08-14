/**
 * Marketplace authentication account management.
 *
 * This is the bounded account CRUD surface from
 * `marketplace-platform-handoff.md`. Marketplace accounts are not part of the
 * production Codex routing pool until the handoff's routing and ledger phases
 * are implemented together.
 */

import { authenticateClient } from "./auth.ts";
import { json, openaiError, withCors } from "./http.ts";
import { getKv } from "./kv.ts";
import { sha256Hex } from "./utils.ts";

type MarketplaceAuthAccount = Readonly<{
  id: string;
  ownerUserId: string;
  provider: string;
  encryptedAuthJson: string;
  status: string;
  pricing: unknown;
  maxConcurrent: number | null;
  health: unknown;
  enabled: boolean;
  labels: unknown;
  createdAt: number;
  updatedAt: number;
}>;

type MarketplaceDeps = Readonly<{
  authenticateClient?: typeof authenticateClient;
  kv?: Deno.Kv | null;
  now?: () => number;
  uuid?: () => string;
}>;

const MUTABLE_FIELDS = new Set(["pricing", "maxConcurrent", "status", "enabled", "labels"]);
const MARKETPLACE_AUTH_STATUSES = new Set(["enabled", "disabled", "error"]);
const DEFAULT_MARKETPLACE_PAGE_LIMIT = 50;
const MAX_MARKETPLACE_PAGE_LIMIT = 100;

const marketplacePage = (
  req: Request,
): { ok: true; limit: number; cursor?: string } | { ok: false; response: Response } => {
  const url = new URL(req.url);
  const rawLimit = url.searchParams.get("limit");
  const limit = rawLimit === null ? DEFAULT_MARKETPLACE_PAGE_LIMIT : Number(rawLimit);
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > MAX_MARKETPLACE_PAGE_LIMIT) {
    return {
      ok: false,
      response: openaiError(
        400,
        `limit must be an integer between 1 and ${MAX_MARKETPLACE_PAGE_LIMIT}`,
        "invalid_request_error",
        { param: "limit" },
      ),
    };
  }
  const cursor = url.searchParams.get("cursor")?.trim() || undefined;
  return { ok: true, limit, ...(cursor ? { cursor } : {}) };
};

export const authAccountKey = (id: string) => ["ubq_ai", "marketplace", "auth_accounts", id] as const;
export const authAccountByOwnerKey = (ownerUserId: string, id: string) =>
  ["ubq_ai", "marketplace", "auth_accounts_by_owner", ownerUserId, id] as const;

const marketplacePrincipal = async (
  authResult: Extract<Awaited<ReturnType<typeof authenticateClient>>, { ok: true }>,
): Promise<string | null> => {
  const { token, method } = authResult;
  switch (method.kind) {
    case "kv_api_key":
      return `api-key:${method.key_id}`;
    case "github_token":
      return null;
    case "passkey_session":
      return `passkey-user:${method.user_id}`;
    case "auth_tokens_allowlist":
    case "admin_allowlist":
    case "deno_deploy_token":
      return token ? `bearer-sha256:${await sha256Hex(token)}` : `auth-method:${method.kind}`;
    case "disabled":
      return token ? `bearer-sha256:${await sha256Hex(token)}` : "local-auth-disabled";
  }
};

const authenticateOwner = async (
  req: Request,
  deps: MarketplaceDeps,
): Promise<{ ok: true; ownerUserId: string } | { ok: false; response: Response }> => {
  const authResult = await (deps.authenticateClient ?? authenticateClient)(req);
  if (!authResult.ok) return { ok: false, response: authResult.response };
  const ownerUserId = await marketplacePrincipal(authResult);
  if (!ownerUserId) {
    return {
      ok: false,
      response: openaiError(
        403,
        "Marketplace account ownership requires user-specific authentication",
        "forbidden",
      ),
    };
  }
  return { ok: true, ownerUserId };
};

const resolveKv = async (deps: MarketplaceDeps): Promise<Deno.Kv | null> =>
  deps.kv !== undefined ? deps.kv : await getKv();

const unavailableKv = (): Response => withCors(openaiError(503, "Deno KV unavailable", "server_error"));
const invalidCursor = (): Response =>
  withCors(openaiError(400, "cursor is invalid for this marketplace listing", "invalid_request_error", {
    param: "cursor",
  }));

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readObject = async (req: Request): Promise<Record<string, unknown> | null> => {
  try {
    const value: unknown = await req.json();
    return isObject(value) ? value : null;
  } catch {
    return null;
  }
};

const validateMarketplaceMetadata = (body: Record<string, unknown>): string | null => {
  if (body.pricing !== undefined && body.pricing !== null && !isObject(body.pricing)) {
    return "pricing must be an object or null";
  }
  if (
    body.labels !== undefined && body.labels !== null &&
    (!isObject(body.labels) || Object.values(body.labels).some((value) => typeof value !== "string"))
  ) {
    return "labels must be an object with string values or null";
  }
  return null;
};

const validateMutableFields = (body: Record<string, unknown>): string | null => {
  const unknown = Object.keys(body).find((key) => !MUTABLE_FIELDS.has(key));
  if (unknown) return `Field is not mutable: ${unknown}`;
  if (Object.keys(body).length === 0) return "At least one mutable field is required";
  const metadataError = validateMarketplaceMetadata(body);
  if (metadataError) return metadataError;
  if (
    body.status !== undefined &&
    (typeof body.status !== "string" || !MARKETPLACE_AUTH_STATUSES.has(body.status.trim()))
  ) {
    return "status must be enabled, disabled, or error";
  }
  if (body.enabled !== undefined && typeof body.enabled !== "boolean") return "enabled must be a boolean";
  if (
    typeof body.status === "string" && typeof body.enabled === "boolean" &&
    body.enabled !== (body.status.trim() === "enabled")
  ) {
    return "status and enabled must describe the same account state";
  }
  if (
    body.maxConcurrent !== undefined && body.maxConcurrent !== null &&
    (typeof body.maxConcurrent !== "number" || !Number.isInteger(body.maxConcurrent) || body.maxConcurrent < 1)
  ) {
    return "maxConcurrent must be a positive integer or null";
  }
  return null;
};

export const handleMarketplaceCreateAuth = async (req: Request, deps: MarketplaceDeps = {}): Promise<Response> => {
  const owner = await authenticateOwner(req, deps);
  if (!owner.ok) return withCors(owner.response);
  const body = await readObject(req);
  if (!body) return withCors(openaiError(400, "Invalid JSON payload", "invalid_request_error"));
  if (typeof body.provider !== "string" || !body.provider.trim()) {
    return withCors(openaiError(400, "provider must be a non-empty string", "invalid_request_error"));
  }
  if (typeof body.encryptedAuthJson !== "string" || !body.encryptedAuthJson.trim()) {
    return withCors(openaiError(400, "encryptedAuthJson must be a non-empty string", "invalid_request_error"));
  }
  if (
    body.maxConcurrent !== undefined && body.maxConcurrent !== null &&
    (typeof body.maxConcurrent !== "number" || !Number.isInteger(body.maxConcurrent) || body.maxConcurrent < 1)
  ) {
    return withCors(openaiError(400, "maxConcurrent must be a positive integer or null", "invalid_request_error"));
  }
  const metadataError = validateMarketplaceMetadata(body);
  if (metadataError) return withCors(openaiError(400, metadataError, "invalid_request_error"));

  const kv = await resolveKv(deps);
  if (!kv) return unavailableKv();
  const id = `auth_${(deps.uuid ?? (() => crypto.randomUUID()))()}`;
  const now = (deps.now ?? Date.now)();
  const account: MarketplaceAuthAccount = {
    id,
    ownerUserId: owner.ownerUserId,
    provider: body.provider.trim(),
    encryptedAuthJson: body.encryptedAuthJson,
    status: "enabled",
    pricing: body.pricing ?? null,
    maxConcurrent: typeof body.maxConcurrent === "number" ? body.maxConcurrent : null,
    health: null,
    enabled: true,
    labels: body.labels ?? null,
    createdAt: now,
    updatedAt: now,
  };
  const accountEntry = await kv.get(authAccountKey(id));
  const ownerEntry = await kv.get(authAccountByOwnerKey(owner.ownerUserId, id));
  const committed = await kv.atomic()
    .check(accountEntry)
    .check(ownerEntry)
    .set(authAccountKey(id), account)
    .set(authAccountByOwnerKey(owner.ownerUserId, id), null)
    .commit();
  if (!committed.ok) return withCors(openaiError(409, "Auth account id conflict", "conflict"));
  return withCors(json(201, { id }));
};

export const handleMarketplaceListAuth = async (req: Request, deps: MarketplaceDeps = {}): Promise<Response> => {
  const owner = await authenticateOwner(req, deps);
  if (!owner.ok) return withCors(owner.response);
  const page = marketplacePage(req);
  if (!page.ok) return withCors(page.response);
  const kv = await resolveKv(deps);
  if (!kv) return unavailableKv();
  const prefix = ["ubq_ai", "marketplace", "auth_accounts_by_owner", owner.ownerUserId] as const;
  const accounts: MarketplaceAuthAccount[] = [];
  let listedEntries = 0;
  let nextCursor: string | null = null;
  try {
    const entries = kv.list(
      { prefix },
      { limit: page.limit, ...(page.cursor ? { cursor: page.cursor } : {}) },
    );
    for await (const entry of entries) {
      listedEntries += 1;
      const id = entry.key.at(-1);
      if (typeof id === "string") {
        const account = await kv.get<MarketplaceAuthAccount>(authAccountKey(id));
        if (account.value?.ownerUserId === owner.ownerUserId) accounts.push(account.value);
      }
      if (listedEntries >= page.limit) break;
    }
    nextCursor = listedEntries === page.limit && entries.cursor ? entries.cursor : null;
  } catch (error) {
    if (page.cursor && error instanceof TypeError) return invalidCursor();
    throw error;
  }
  return withCors(json(200, {
    auths: accounts,
    next_cursor: nextCursor,
  }, { "Cache-Control": "no-store" }));
};

export const handleMarketplaceUpdateAuth = async (
  req: Request,
  id: string,
  deps: MarketplaceDeps = {},
): Promise<Response> => {
  const owner = await authenticateOwner(req, deps);
  if (!owner.ok) return withCors(owner.response);
  const kv = await resolveKv(deps);
  if (!kv) return unavailableKv();
  const existing = await kv.get<MarketplaceAuthAccount>(authAccountKey(id));
  if (!existing.value) return withCors(openaiError(404, "Auth account not found", "invalid_request_error"));
  if (existing.value.ownerUserId !== owner.ownerUserId) {
    return withCors(openaiError(403, "Auth account belongs to another owner", "forbidden"));
  }
  const body = await readObject(req);
  if (!body) return withCors(openaiError(400, "Invalid JSON payload", "invalid_request_error"));
  const validationError = validateMutableFields(body);
  if (validationError) return withCors(openaiError(400, validationError, "invalid_request_error"));

  const requestedStatus = typeof body.status === "string" ? body.status.trim() : undefined;
  const requestedEnabled = typeof body.enabled === "boolean" ? body.enabled : undefined;
  const status = requestedStatus ??
    (requestedEnabled === undefined ? existing.value.status : requestedEnabled ? "enabled" : "disabled");
  const enabled = requestedEnabled ??
    (requestedStatus === undefined ? existing.value.enabled : requestedStatus === "enabled");

  const updated: MarketplaceAuthAccount = {
    ...existing.value,
    ...(body.pricing !== undefined ? { pricing: body.pricing } : {}),
    ...(body.maxConcurrent !== undefined ? { maxConcurrent: body.maxConcurrent as number | null } : {}),
    status,
    enabled,
    ...(body.labels !== undefined ? { labels: body.labels } : {}),
    updatedAt: (deps.now ?? Date.now)(),
  };
  const committed = await kv.atomic().check(existing).set(authAccountKey(id), updated).commit();
  if (!committed.ok) return withCors(openaiError(409, "Auth account changed; retry the update", "conflict"));
  return withCors(json(200, { id }));
};

export const handleMarketplaceDisableAuth = async (
  req: Request,
  id: string,
  deps: MarketplaceDeps = {},
): Promise<Response> => {
  const owner = await authenticateOwner(req, deps);
  if (!owner.ok) return withCors(owner.response);
  const kv = await resolveKv(deps);
  if (!kv) return unavailableKv();
  const existing = await kv.get<MarketplaceAuthAccount>(authAccountKey(id));
  if (!existing.value) return withCors(openaiError(404, "Auth account not found", "invalid_request_error"));
  if (existing.value.ownerUserId !== owner.ownerUserId) {
    return withCors(openaiError(403, "Auth account belongs to another owner", "forbidden"));
  }
  const updated: MarketplaceAuthAccount = {
    ...existing.value,
    enabled: false,
    status: "disabled",
    updatedAt: (deps.now ?? Date.now)(),
  };
  const committed = await kv.atomic().check(existing).set(authAccountKey(id), updated).commit();
  if (!committed.ok) return withCors(openaiError(409, "Auth account changed; retry the update", "conflict"));
  return withCors(json(200, { id }));
};

export const handleMarketplacePublicCatalog = async (
  req: Request,
  deps: MarketplaceDeps = {},
): Promise<Response> => {
  const page = marketplacePage(req);
  if (!page.ok) return withCors(page.response);
  const kv = await resolveKv(deps);
  if (!kv) return unavailableKv();
  const accounts: Array<Record<string, unknown>> = [];
  let nextCursor: string | null = null;
  try {
    const entries = kv.list<MarketplaceAuthAccount>(
      { prefix: ["ubq_ai", "marketplace", "auth_accounts"] },
      { limit: page.limit, ...(page.cursor ? { cursor: page.cursor } : {}) },
    );
    for await (const entry of entries) {
      const account = entry.value;
      accounts.push({
        id: account.id,
        provider: account.provider,
        status: account.status,
        pricing: account.pricing,
        maxConcurrent: account.maxConcurrent,
        health: account.health,
        enabled: account.enabled,
        labels: account.labels,
      });
      if (accounts.length >= page.limit) break;
    }
    nextCursor = accounts.length === page.limit && entries.cursor ? entries.cursor : null;
  } catch (error) {
    if (page.cursor && error instanceof TypeError) return invalidCursor();
    throw error;
  }
  return withCors(json(200, {
    auths: accounts,
    next_cursor: nextCursor,
  }));
};
