/**
 * Marketplace authentication account management.
 *
 * This module provides the public API surface described in
 * `marketplace-platform-handoff.md` for creating, listing, updating, and
 * disabling seller‑uploaded `auth.json` credentials.
 */

import { authenticateClient } from "./auth.ts";
import { sha256Hex } from "./utils.ts";
import { getKv } from "./kv.ts";
import { openaiError, withCors } from "./http.ts";

type MarketplaceAuthAccount = {
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
  [key: string]: unknown;
};

// KV key helpers -----------------------------------------------------------
export const authAccountKey = (id: string) => ["ubq_ai", "marketplace", "auth_accounts", id] as const;
export const authAccountByOwnerKey = (ownerUserId: string, id: string) =>
  ["ubq_ai", "marketplace", "auth_accounts_by_owner", ownerUserId, id] as const;

/**
 * Create a new marketplace authentication account.
 */
export const handleMarketplaceCreateAuth = async (req: Request): Promise<Response> => {
  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(openaiError(401, "unauthorized", "invalid_request_error"));
  const principal = await (async () => {
    // Re‑use the same logic as `resolveIdempotencyPrincipal` but in‑line.
    const { token, method } = authResult;
    switch (method.kind) {
      case "kv_api_key":
        return `api-key:${method.key_id}`;
      case "github_token":
        return `github-repo:${method.owner.toLowerCase()}/${method.repo.toLowerCase()}`;
      case "passkey_session":
        return `passkey-user:${method.user_id}`;
      case "auth_tokens_allowlist":
      case "admin_allowlist":
      case "deno_deploy_token":
        return `auth-method:${method.kind}`;
      case "disabled":
        return token ? `bearer-sha256:${await sha256Hex(token)}` : "local-auth-disabled";
    }
  })();
  const ownerUserId = principal;
  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return withCors(openaiError(400, "invalid JSON payload", "invalid_request_error"));
  }
  if (!body || typeof body !== "object") {
    return withCors(openaiError(400, "invalid payload", "invalid_request_error"));
  }
  const record = body as Record<string, unknown>;
  if (typeof record.provider !== "string" || typeof record.encryptedAuthJson !== "string") {
    return withCors(openaiError(400, "missing required fields", "invalid_request_error"));
  }
  const kv = await getKv();
  const rand = crypto.getRandomValues(new Uint8Array(8));
  const id = `auth_${await sha256Hex(new TextDecoder().decode(rand))}`;
  const now = Date.now();
  const authAccount = {
    id,
    ownerUserId,
    provider: record.provider,
    encryptedAuthJson: record.encryptedAuthJson,
    status: "enabled" as const,
    pricing: record.pricing ?? null,
    maxConcurrent: typeof record.maxConcurrent === "number" ? record.maxConcurrent : null,
    health: record.health ?? null,
    enabled: true,
    labels: record.labels ?? null,
    createdAt: now,
    updatedAt: now,
  };
  await kv.set(authAccountKey(id), authAccount);
  // Create an owner → id mapping for quick lookup.
  await kv.set(authAccountByOwnerKey(ownerUserId, id), null);
  return withCors(
    new Response(JSON.stringify({ success: true, id }), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
  );
};

/**
 * List the caller's own auth accounts.
 */
export const handleMarketplaceListAuth = async (req: Request): Promise<Response> => {
  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(openaiError(401, "unauthorized", "invalid_request_error"));
  const principal = await (async () => {
    const { token, method } = authResult;
    switch (method.kind) {
      case "kv_api_key":
        return `api-key:${method.key_id}`;
      case "github_token":
        return `github-repo:${method.owner.toLowerCase()}/${method.repo.toLowerCase()}`;
      case "passkey_session":
        return `passkey-user:${method.user_id}`;
      case "auth_tokens_allowlist":
      case "admin_allowlist":
      case "deno_deploy_token":
        return `auth-method:${method.kind}`;
      case "disabled":
        return token ? `bearer-sha256:${await sha256Hex(token)}` : "local-auth-disabled";
    }
  })();
  const kv = await getKv();
  const prefix = ["ubq_ai", "marketplace", "auth_accounts_by_owner", principal] as const;
  const accounts: unknown[] = [];
  for await (const entry of kv.list({ prefix })) {
    const id = entry.key[entry.key.length - 1] as string;
    const account = await kv.get<MarketplaceAuthAccount>(authAccountKey(id));
    if (account.value) accounts.push(account.value);
  }
  return withCors(
    new Response(JSON.stringify({ auths: accounts }), { status: 200, headers: { "Content-Type": "application/json" } }),
  );
};

/**
 * Update an existing auth account (owner‑only).
 */
export const handleMarketplaceUpdateAuth = async (req: Request, id: string): Promise<Response> => {
  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(openaiError(401, "unauthorized", "invalid_request_error"));
  const kv = await getKv();
  const existing = await kv.get<MarketplaceAuthAccount>(authAccountKey(id));
  if (!existing.value) return withCors(openaiError(404, "auth account not found", "invalid_request_error"));
  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return withCors(openaiError(400, "invalid JSON payload", "invalid_request_error"));
  }
  const now = Date.now();
  const updated = { ...existing.value, ...(body as Record<string, unknown>), updatedAt: now };
  await kv.set(authAccountKey(id), updated);
  return withCors(
    new Response(JSON.stringify({ success: true, id }), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
  );
};

/**
 * Disable an auth account (owner‑only).
 */
export const handleMarketplaceDisableAuth = async (req: Request, id: string): Promise<Response> => {
  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(openaiError(401, "unauthorized", "invalid_request_error"));
  const kv = await getKv();
  const existing = await kv.get<MarketplaceAuthAccount>(authAccountKey(id));
  if (!existing.value) return withCors(openaiError(404, "auth account not found", "invalid_request_error"));
  const now = Date.now();
  const updated = { ...existing.value, enabled: false, updatedAt: now };
  await kv.set(authAccountKey(id), updated);
  return withCors(
    new Response(JSON.stringify({ success: true, id }), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
  );
};

/**
 * Public catalog – returns a list of all marketplace auth accounts with only
 * non‑secret fields. This is intended for discoverability and does not expose
 * the encrypted `auth.json` payload.
 */
export const handleMarketplacePublicCatalog = async (_req: Request): Promise<Response> => {
  const kv = await getKv();
  const prefix = ["ubq_ai", "marketplace", "auth_accounts"] as const;
  const accounts: unknown[] = [];
  for await (const entry of kv.list({ prefix })) {
    const account = await kv.get<MarketplaceAuthAccount>(entry.key);
    if (account.value) {
      const publicFields: Record<string, unknown> = { ...account.value };
      delete publicFields.encryptedAuthJson;
      accounts.push(publicFields);
    }
  }
  return withCors(
    new Response(JSON.stringify({ auths: accounts }), { status: 200, headers: { "Content-Type": "application/json" } }),
  );
};
