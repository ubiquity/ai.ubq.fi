import type { KvKey } from "#kv";
import { getKv } from "../kv.ts";
import { base64UrlEncode, isRecord, sha256Hex } from "../utils.ts";

const ISSUER = "https://passport.ubq.fi";
const LOCAL_TOKEN = /^uos_ai_passport_session_[A-Za-z0-9_-]{43}$/;
const APP_TOKEN = /^uos_app_session_[A-Za-z0-9_-]{43}$/;
const BINDING = {
  clientId: "ai",
  audience: "ai",
  origin: "https://ai.ubq.fi",
  redirectUri: "https://ai.ubq.fi/auth/callback",
} as const;

/** Browser persistence is renewed on use; the authoritative store has no expiry. */
export const PASSPORT_COOKIE_MAX_AGE_SECONDS = 400 * 24 * 60 * 60;

type SessionRecord = Readonly<{
  version: 1;
  subject: string;
  clientId: string;
  audience: string;
  origin: string;
  redirectUri: string;
  createdAt: string;
  appSessionToken: string;
}>;

export type PassportSessionIdentity = Readonly<{ subject: string; handle: string; isAdmin: boolean; scopes: string[] }>;

export class PassportSessionError extends Error {
  constructor(readonly status: 401 | 503) {
    super(status === 401 ? "Unauthorized" : "Passport session verification is unavailable.");
  }
}

const boundedString = (value: unknown, limit: number): value is string => typeof value === "string" && value.length > 0 && value.length <= limit;
const matchesBinding = (value: Record<string, unknown>): boolean => Object.entries(BINDING).every(([name, expected]) => value[name] === expected);
const sessionKey = async (token: string): Promise<KvKey> => ["uos_ai", "auth", "passport", "sessions", await sha256Hex(token)];

export const isPersistentPassportSession = (token: string): boolean => LOCAL_TOKEN.test(token);

const validRecord = (value: unknown): value is SessionRecord =>
  isRecord(value) &&
  value.version === 1 &&
  boundedString(value.subject, 500) &&
  boundedString(value.createdAt, 100) &&
  Number.isFinite(Date.parse(value.createdAt)) &&
  typeof value.appSessionToken === "string" &&
  APP_TOKEN.test(value.appSessionToken) &&
  matchesBinding(value);

const readJson = async (response: Response): Promise<unknown> => {
  if (!response.body) throw new PassportSessionError(503);
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let bytes = 0;
  let body = "";
  try {
    for (;;) {
      const item = await reader.read();
      if (item.done) break;
      bytes += item.value.byteLength;
      if (bytes > 65_536) throw new PassportSessionError(503);
      body += decoder.decode(item.value, { stream: true });
    }
    return JSON.parse(body + decoder.decode()) as unknown;
  } finally {
    await reader.cancel().catch(() => {});
    reader.releaseLock();
  }
};

const sessionRequest = async (record: SessionRecord, revoke = false): Promise<Response> =>
  await fetch(`${ISSUER}/api/auth/sso/session${revoke ? "/revoke" : ""}`, {
    method: "POST",
    headers: { Authorization: `Bearer ${record.appSessionToken}`, "Content-Type": "application/json", Accept: "application/json" },
    body: JSON.stringify(BINDING),
    redirect: "error",
    signal: AbortSignal.timeout(10_000),
  });

export const createPassportSession = async (grant: unknown, subject: string, previousToken?: string): Promise<string> => {
  if (
    !isRecord(grant) ||
    !boundedString(subject, 500) ||
    grant.subject !== subject ||
    !boundedString(grant.createdAt, 100) ||
    !Number.isFinite(Date.parse(grant.createdAt)) ||
    typeof grant.token !== "string" ||
    !APP_TOKEN.test(grant.token) ||
    !matchesBinding(grant)
  )
    throw new PassportSessionError(503);
  const kv = await getKv();
  if (!kv) throw new PassportSessionError(503);
  const token = `uos_ai_passport_session_${base64UrlEncode(crypto.getRandomValues(new Uint8Array(32)))}`;
  const key = await sessionKey(token);
  const record: SessionRecord = { version: 1, subject, ...BINDING, createdAt: grant.createdAt, appSessionToken: grant.token };
  try {
    const previousKey = previousToken && isPersistentPassportSession(previousToken) ? await sessionKey(previousToken) : null;
    const previous = previousKey ? (await kv.get(previousKey)).value : null;
    const transaction = kv.atomic().check({ key, versionstamp: null }).set(key, record);
    if (previousKey) transaction.delete(previousKey);
    const commit = await transaction.commit();
    if (!commit.ok) throw new PassportSessionError(503);
    if (validRecord(previous)) {
      try {
        const response = await sessionRequest(previous, true);
        await response.body?.cancel();
      } catch {
        // The replaced local session is already irrevocably removed.
      }
    }
    return token;
  } catch {
    throw new PassportSessionError(503);
  }
};

const identityFromResponse = (body: unknown, record: SessionRecord): PassportSessionIdentity => {
  if (!isRecord(body) || body.authenticated !== true || !isRecord(body.session) || !isRecord(body.user)) throw new PassportSessionError(503);
  const { session, user } = body;
  if (
    !matchesBinding(session) ||
    session.subject !== record.subject ||
    session.createdAt !== record.createdAt ||
    user.id !== record.subject ||
    typeof user.handle !== "string" ||
    user.handle.length > 500 ||
    typeof user.isAdmin !== "boolean" ||
    !Array.isArray(user.scopes) ||
    user.scopes.length > 128 ||
    !user.scopes.every((scope: unknown) => typeof scope === "string" && scope.length <= 500)
  )
    throw new PassportSessionError(503);
  return { subject: record.subject, handle: user.handle, isAdmin: user.isAdmin, scopes: user.scopes as string[] };
};

export const getPassportSession = async (token: string): Promise<PassportSessionIdentity> => {
  if (!isPersistentPassportSession(token)) throw new PassportSessionError(401);
  const kv = await getKv();
  if (!kv) throw new PassportSessionError(503);
  try {
    const key = await sessionKey(token);
    const entry = await kv.get(key);
    if (!entry.value) throw new PassportSessionError(401);
    if (!validRecord(entry.value)) throw new PassportSessionError(503);
    const record = entry.value;
    const response = await sessionRequest(record);
    if (response.status === 401) {
      await response.body?.cancel().catch(() => {});
      await kv.delete(key);
      throw new PassportSessionError(401);
    }
    if (!response.ok) {
      await response.body?.cancel();
      throw new PassportSessionError(503);
    }
    const identity = identityFromResponse(await readJson(response), record);
    // Logout owns deletion. An in-flight introspection can never republish it.
    const current = await kv.get(key);
    if (!validRecord(current.value) || current.value.appSessionToken !== record.appSessionToken) throw new PassportSessionError(401);
    return identity;
  } catch (error) {
    if (error instanceof PassportSessionError) throw error;
    throw new PassportSessionError(503);
  }
};

/** Local revocation completes before the independent upstream best-effort request. */
export const revokePassportSession = async (token: string): Promise<"revoked" | "unavailable" | "not_applicable"> => {
  if (!isPersistentPassportSession(token)) return "not_applicable";
  const kv = await getKv();
  if (!kv) throw new PassportSessionError(503);
  let value: unknown;
  try {
    const key = await sessionKey(token);
    value = (await kv.get(key)).value;
    await kv.delete(key);
  } catch {
    throw new PassportSessionError(503);
  }
  if (!validRecord(value)) return "not_applicable";
  try {
    const response = await sessionRequest(value, true);
    await response.body?.cancel();
    return response.status === 204 ? "revoked" : "unavailable";
  } catch {
    return "unavailable";
  }
};
