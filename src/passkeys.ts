import {
  type AuthenticationResponseJSON,
  generateAuthenticationOptions,
  type RegistrationResponseJSON,
  verifyAuthenticationResponse,
  verifyRegistrationResponse,
} from "@simplewebauthn/server";
import { getBearerToken, json, openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { base64UrlDecode, base64UrlEncode, getString, isRecord, sha256Hex } from "./utils.ts";

export type PasskeyUserRecord = {
  id: string;
  handle: string;
  is_admin: boolean;
  credential_ids: string[];
  created_at_ms: number;
  updated_at_ms: number;
};

export type PasskeyCredentialRecord = {
  credential_id: string;
  user_id: string;
  public_key: string;
  sign_count: number;
  transports: string[];
  created_at_ms: number;
};

export type PasskeySessionRecord = {
  token: string;
  user_id: string;
  created_at_ms: number;
  expires_at_ms: number;
};

type PasskeyChallengeRecord = {
  challenge: string;
  type: "registration" | "authentication";
  origin: string;
  rp_id: string;
  user_id?: string;
  handle?: string;
  is_admin?: boolean;
  created_at_ms: number;
  expires_at_ms: number;
};

export type PasskeySession = {
  token: string;
  user: PasskeyUserRecord;
  session: PasskeySessionRecord;
};

export const PASSKEY_CHALLENGE_TTL_MS = 5 * 60 * 1000;
export const PASSKEY_SESSION_TTL_MS = 7 * 24 * 60 * 60 * 1000;

const AUTH_PREFIX = ["uos_ai", "auth"] as const;
const PASSKEY_CANONICAL_ORIGIN = "https://ai.ubq.fi";
const RP_NAME = "UbiquityOS AI Gateway";

export const passkeyUserKey = (userId: string): Deno.KvKey => [...AUTH_PREFIX, "users", userId];
export const passkeyHandleKey = (handle: string): Deno.KvKey => [...AUTH_PREFIX, "handles", handle];
export const passkeyCredentialKey = (credentialId: string): Deno.KvKey => [
  ...AUTH_PREFIX,
  "credentials",
  credentialId,
];
export const passkeyChallengeKey = (challenge: string): Deno.KvKey => [...AUTH_PREFIX, "challenges", challenge];
export const passkeySessionKey = (token: string): Deno.KvKey => [...AUTH_PREFIX, "sessions", token];

const nowMs = () => Date.now();

export const normalizePasskeyHandle = (value: unknown): string => {
  const text = getString(value)?.trim().toLowerCase() ?? "";
  return text
    .replace(/[^a-z0-9._-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .replace(/-{2,}/g, "-")
    .slice(0, 96);
};

export const buildPasskeyHandle = async (seed: string): Promise<string> => {
  const trimmed = seed.trim();
  if (!trimmed) return "";
  const fingerprint = (await sha256Hex(trimmed)).slice(0, 16);
  return `uos-passkey-${fingerprint}`;
};

export const isPasskeyUserAdmin = (user: Pick<PasskeyUserRecord, "is_admin"> | null | undefined): boolean =>
  user?.is_admin === true;

const serializePasskeyUser = (user: PasskeyUserRecord): Record<string, unknown> => ({
  id: user.id,
  handle: user.handle,
  is_admin: isPasskeyUserAdmin(user),
  credential_count: user.credential_ids.length,
  created_at_ms: user.created_at_ms,
  updated_at_ms: user.updated_at_ms,
});

const parseClientChallenge = (encoded: string): string => {
  const decoded = base64UrlDecode(encoded);
  const jsonText = new TextDecoder().decode(decoded);
  const parsed = JSON.parse(jsonText) as { challenge?: unknown };
  return getString(parsed.challenge)?.trim() ?? "";
};

const firstHeaderValue = (value: string | null): string | null => {
  const first = value?.split(",")[0]?.trim() ?? "";
  return first || null;
};

const parseOrigin = (value: string | null): string | null => {
  const trimmed = value?.trim() ?? "";
  if (!trimmed || trimmed === "null") return null;
  try {
    const origin = new URL(trimmed).origin;
    return origin === "null" ? null : origin;
  } catch {
    return null;
  }
};

const parseOriginFromHost = (host: string | null, protocol: string): string | null => {
  if (!host) return null;
  try {
    return new URL(`${protocol}://${host}`).origin;
  } catch {
    return null;
  }
};

const isLoopbackHost = (hostname: string): boolean =>
  hostname === "localhost" || hostname === "127.0.0.1" || hostname === "[::1]" || hostname === "::1";

const isTrustedPasskeyOrigin = (origin: string): boolean => {
  try {
    const url = new URL(origin);
    if (url.origin === PASSKEY_CANONICAL_ORIGIN) return true;
    return (url.protocol === "http:" || url.protocol === "https:") && isLoopbackHost(url.hostname);
  } catch {
    return false;
  }
};

const firstTrustedPasskeyOrigin = (origins: Array<string | null>): string => {
  for (const origin of origins) {
    if (origin && isTrustedPasskeyOrigin(origin)) return origin;
  }
  return PASSKEY_CANONICAL_ORIGIN;
};

export const getPasskeyRequestMeta = (req: Request, clientOrigin?: unknown): { origin: string; rpId: string } => {
  const url = new URL(req.url);
  const forwardedProtocol = firstHeaderValue(req.headers.get("x-forwarded-proto"));
  const protocol = forwardedProtocol || url.protocol.replace(/:$/, "");
  const forwardedHost = firstHeaderValue(req.headers.get("x-forwarded-host"));
  const host = firstHeaderValue(req.headers.get("host"));
  const origin = firstTrustedPasskeyOrigin([
    parseOrigin(getString(clientOrigin) ?? null),
    parseOrigin(req.headers.get("origin")),
    parseOrigin(req.headers.get("referer")),
    parseOriginFromHost(forwardedHost, protocol),
    parseOriginFromHost(host, protocol),
    url.origin,
  ]);
  const originUrl = new URL(origin);
  return { origin: originUrl.origin, rpId: originUrl.hostname };
};

const getKvOrError = async (): Promise<Deno.Kv | Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(503, "Passkey auth requires Deno KV", "server_error");
  return kv;
};

const getUserByHandle = async (kv: Deno.Kv, handle: string): Promise<PasskeyUserRecord | null> => {
  const userIdEntry = await kv.get<string>(passkeyHandleKey(handle));
  if (!userIdEntry.value) return null;
  const userEntry = await kv.get<PasskeyUserRecord>(passkeyUserKey(userIdEntry.value));
  return userEntry.value ?? null;
};

export const hasPasskeyUsers = async (): Promise<boolean> => {
  const kv = await kvPromise;
  if (!kv) return false;
  for await (const _entry of kv.list({ prefix: [...AUTH_PREFIX, "users"] }, { limit: 1 })) {
    return true;
  }
  return false;
};

const getCredential = async (kv: Deno.Kv, credentialId: string): Promise<PasskeyCredentialRecord | null> => {
  const entry = await kv.get<PasskeyCredentialRecord>(passkeyCredentialKey(credentialId));
  return entry.value ?? null;
};

const saveChallenge = async (
  kv: Deno.Kv,
  input: Omit<PasskeyChallengeRecord, "created_at_ms" | "expires_at_ms">,
): Promise<PasskeyChallengeRecord> => {
  const createdAtMs = nowMs();
  const record: PasskeyChallengeRecord = {
    ...input,
    created_at_ms: createdAtMs,
    expires_at_ms: createdAtMs + PASSKEY_CHALLENGE_TTL_MS,
  };
  await kv.set(passkeyChallengeKey(record.challenge), record, { expireIn: PASSKEY_CHALLENGE_TTL_MS });
  return record;
};

const consumeChallenge = async (kv: Deno.Kv, challenge: string): Promise<PasskeyChallengeRecord | null> => {
  const key = passkeyChallengeKey(challenge);
  const entry = await kv.get<PasskeyChallengeRecord>(key);
  if (!entry.value) return null;
  if (entry.value.expires_at_ms <= nowMs()) {
    await kv.atomic().check(entry).delete(key).commit();
    return null;
  }
  const commit = await kv.atomic().check(entry).delete(key).commit();
  if (!commit.ok) return null;
  return entry.value;
};

const createSession = async (kv: Deno.Kv, userId: string): Promise<PasskeySessionRecord> => {
  const createdAtMs = nowMs();
  const token = `uos_ai_session_${crypto.randomUUID()}`;
  const record: PasskeySessionRecord = {
    token,
    user_id: userId,
    created_at_ms: createdAtMs,
    expires_at_ms: createdAtMs + PASSKEY_SESSION_TTL_MS,
  };
  await kv.set(passkeySessionKey(token), record, { expireIn: PASSKEY_SESSION_TTL_MS });
  return record;
};

type SaveVerifiedPasskeyRegistrationInput = {
  userId: string;
  handle: string;
  isAdmin: boolean;
  credentialId: string;
  publicKey: string;
  signCount: number;
  transports: string[];
};

export const saveVerifiedPasskeyRegistration = async (
  kv: Deno.Kv,
  input: SaveVerifiedPasskeyRegistrationInput,
): Promise<{ ok: true; user: PasskeyUserRecord } | { ok: false; response: Response }> => {
  const handleEntry = await kv.get<string>(passkeyHandleKey(input.handle));
  if (handleEntry.value && handleEntry.value !== input.userId) {
    return {
      ok: false,
      response: openaiError(409, "A passkey account already exists for this username", "invalid_request_error"),
    };
  }

  const createdAtMs = nowMs();
  const credentialRecord: PasskeyCredentialRecord = {
    credential_id: input.credentialId,
    user_id: input.userId,
    public_key: input.publicKey,
    sign_count: input.signCount,
    transports: input.transports,
    created_at_ms: createdAtMs,
  };

  const existingUserEntry = await kv.get<PasskeyUserRecord>(passkeyUserKey(input.userId));
  const userRecord: PasskeyUserRecord = existingUserEntry.value
    ? {
      ...existingUserEntry.value,
      handle: input.handle,
      is_admin: isPasskeyUserAdmin(existingUserEntry.value),
      credential_ids: Array.from(new Set([...existingUserEntry.value.credential_ids, input.credentialId])),
      updated_at_ms: createdAtMs,
    }
    : {
      id: input.userId,
      handle: input.handle,
      is_admin: input.isAdmin,
      credential_ids: [input.credentialId],
      created_at_ms: createdAtMs,
      updated_at_ms: createdAtMs,
    };

  let atomic = kv.atomic()
    .check(existingUserEntry)
    .check(handleEntry)
    .set(passkeyUserKey(userRecord.id), userRecord)
    .set(passkeyHandleKey(input.handle), userRecord.id)
    .set(passkeyCredentialKey(input.credentialId), credentialRecord);
  if (existingUserEntry.value && existingUserEntry.value.handle !== input.handle) {
    atomic = atomic.delete(passkeyHandleKey(existingUserEntry.value.handle));
  }
  const commit = await atomic.commit();
  if (!commit.ok) {
    return {
      ok: false,
      response: openaiError(409, "Passkey account was modified concurrently; retry", "invalid_request_error"),
    };
  }

  return { ok: true, user: userRecord };
};

export const getPasskeySession = async (token: string): Promise<PasskeySession | null> => {
  const kv = await kvPromise;
  if (!kv) return null;
  const sessionEntry = await kv.get<PasskeySessionRecord>(passkeySessionKey(token));
  if (!sessionEntry.value) return null;
  if (sessionEntry.value.expires_at_ms <= nowMs()) {
    await kv.atomic().check(sessionEntry).delete(passkeySessionKey(token)).commit();
    return null;
  }
  const userEntry = await kv.get<PasskeyUserRecord>(passkeyUserKey(sessionEntry.value.user_id));
  if (!userEntry.value) return null;
  return { token, user: userEntry.value, session: sessionEntry.value };
};

export const getPasskeySessionFromRequest = async (req: Request): Promise<PasskeySession | null> => {
  const token = getBearerToken(req);
  return token ? await getPasskeySession(token) : null;
};

export const handlePasskeyRegisterStart = async (
  req: Request,
  options: { defaultIsAdmin?: boolean } = {},
): Promise<Response> => {
  const kvOrError = await getKvOrError();
  if (kvOrError instanceof Response) return kvOrError;
  const kv = kvOrError;

  let raw: unknown = null;
  try {
    raw = await req.json();
  } catch {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }
  if (!isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const token = getBearerToken(req) ?? "";
  const existingSession = token ? await getPasskeySession(token) : null;
  const requestedHandle = normalizePasskeyHandle(raw.handle);
  const tokenHandle = token ? await buildPasskeyHandle(token) : "";
  const requestedUser = requestedHandle ? await getUserByHandle(kv, requestedHandle) : null;
  const tokenUser = !requestedHandle && tokenHandle ? await getUserByHandle(kv, tokenHandle) : null;
  const existingUser = existingSession?.user ?? requestedUser ?? tokenUser;
  const userId = existingUser?.id ?? crypto.randomUUID();
  const handle = existingSession?.user.handle || requestedHandle || tokenHandle || await buildPasskeyHandle(userId);
  if (!handle) return openaiError(400, "username is required", "invalid_request_error");
  const isAdmin = existingUser ? isPasskeyUserAdmin(existingUser) : options.defaultIsAdmin === true;
  const { origin, rpId } = getPasskeyRequestMeta(req, raw.client_origin);

  const challengeBytes = crypto.getRandomValues(new Uint8Array(32));
  const challenge = base64UrlEncode(challengeBytes);
  await saveChallenge(kv, {
    challenge,
    type: "registration",
    origin,
    rp_id: rpId,
    user_id: userId,
    handle,
    is_admin: isAdmin,
  });

  const userIdBytes = new TextEncoder().encode(userId);
  return json(
    200,
    {
      publicKey: {
        rp: { id: rpId, name: RP_NAME },
        user: {
          id: base64UrlEncode(userIdBytes),
          name: handle,
          displayName: handle,
        },
        challenge,
        pubKeyCredParams: [
          { type: "public-key", alg: -7 },
          { type: "public-key", alg: -257 },
        ],
        timeout: PASSKEY_CHALLENGE_TTL_MS,
        attestation: "none",
        authenticatorSelection: {
          residentKey: "required",
          userVerification: isAdmin ? "required" : "preferred",
        },
        excludeCredentials: existingUser?.credential_ids.map((id) => ({ id, type: "public-key" })) ?? [],
      },
      handle,
    },
    { "Cache-Control": "no-store" },
  );
};

export const handlePasskeyRegisterFinish = async (req: Request): Promise<Response> => {
  const kvOrError = await getKvOrError();
  if (kvOrError instanceof Response) return kvOrError;
  const kv = kvOrError;

  let raw: unknown = null;
  try {
    raw = await req.json();
  } catch {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }
  if (!isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const response = raw.response as RegistrationResponseJSON | undefined;
  const clientDataJson = response?.response?.clientDataJSON;
  if (!clientDataJson) return openaiError(400, "Invalid passkey response", "invalid_request_error");

  let challenge = "";
  try {
    challenge = parseClientChallenge(clientDataJson);
  } catch {
    return openaiError(400, "Invalid passkey response", "invalid_request_error");
  }
  if (!challenge) return openaiError(400, "Invalid passkey response", "invalid_request_error");

  const challengeRecord = await consumeChallenge(kv, challenge);
  if (!challengeRecord || challengeRecord.type !== "registration" || !challengeRecord.user_id) {
    return openaiError(400, "Invalid passkey challenge", "invalid_request_error");
  }

  try {
    const verification = await verifyRegistrationResponse({
      response,
      expectedChallenge: challengeRecord.challenge,
      expectedOrigin: challengeRecord.origin,
      expectedRPID: challengeRecord.rp_id,
      requireUserVerification: challengeRecord.is_admin === true,
    });
    if (!verification.verified || !verification.registrationInfo) {
      return openaiError(400, "Invalid passkey attestation", "invalid_request_error");
    }

    const registrationInfo = verification.registrationInfo;
    const credentialId = registrationInfo.credential.id;
    const publicKey = base64UrlEncode(new Uint8Array(registrationInfo.credential.publicKey));
    const handle = normalizePasskeyHandle(challengeRecord.handle);
    if (!handle) return openaiError(400, "username is required", "invalid_request_error");

    const saved = await saveVerifiedPasskeyRegistration(kv, {
      userId: challengeRecord.user_id,
      handle,
      isAdmin: challengeRecord.is_admin === true,
      credentialId,
      publicKey,
      signCount: registrationInfo.credential.counter,
      transports: registrationInfo.credential.transports ?? [],
    });
    if (!saved.ok) return saved.response;

    const session = await createSession(kv, saved.user.id);
    return json(
      200,
      {
        token: session.token,
        user_id: saved.user.id,
        handle: saved.user.handle,
        expires_at_ms: session.expires_at_ms,
      },
      { "Cache-Control": "no-store" },
    );
  } catch {
    return openaiError(400, "Invalid passkey attestation", "invalid_request_error");
  }
};

export const handlePasskeyLoginStart = async (req: Request): Promise<Response> => {
  const kvOrError = await getKvOrError();
  if (kvOrError instanceof Response) return kvOrError;
  const kv = kvOrError;

  let raw: unknown = {};
  const contentType = req.headers.get("content-type") ?? "";
  if (contentType.includes("application/json")) {
    try {
      raw = await req.json();
    } catch {
      return openaiError(400, "Invalid JSON body", "invalid_request_error");
    }
  }
  if (!isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const handle = normalizePasskeyHandle(raw.handle);

  let allowCredentials: Array<{ id: string; type: "public-key" }> | undefined;
  let userVerification: "preferred" | "required" = "preferred";
  if (handle) {
    const user = await getUserByHandle(kv, handle);
    if (!user) return openaiError(404, "Passkey account not found", "not_found");
    if (!user.credential_ids.length) return openaiError(404, "No passkeys registered", "not_found");
    allowCredentials = user.credential_ids.map((id) => ({ id, type: "public-key" }));
    if (isPasskeyUserAdmin(user)) userVerification = "required";
  }

  const { origin, rpId } = getPasskeyRequestMeta(req, raw.client_origin);
  const publicKey = await generateAuthenticationOptions({
    rpID: rpId,
    timeout: PASSKEY_CHALLENGE_TTL_MS,
    allowCredentials,
    userVerification,
  });

  await saveChallenge(kv, { challenge: publicKey.challenge, type: "authentication", origin, rp_id: rpId });
  return json(200, { publicKey }, { "Cache-Control": "no-store" });
};

export const handlePasskeyLoginFinish = async (req: Request): Promise<Response> => {
  const kvOrError = await getKvOrError();
  if (kvOrError instanceof Response) return kvOrError;
  const kv = kvOrError;

  let raw: unknown = null;
  try {
    raw = await req.json();
  } catch {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }
  if (!isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const response = raw.response as AuthenticationResponseJSON | undefined;
  const clientDataJson = response?.response?.clientDataJSON;
  if (!response?.id || !clientDataJson) return openaiError(400, "Invalid passkey response", "invalid_request_error");

  let challenge = "";
  try {
    challenge = parseClientChallenge(clientDataJson);
  } catch {
    return openaiError(400, "Invalid passkey response", "invalid_request_error");
  }

  const challengeRecord = await consumeChallenge(kv, challenge);
  if (!challengeRecord || challengeRecord.type !== "authentication") {
    return openaiError(400, "Invalid passkey challenge", "invalid_request_error");
  }

  const credential = await getCredential(kv, response.id);
  if (!credential) return openaiError(400, "Unknown passkey", "invalid_request_error");
  const userEntry = await kv.get<PasskeyUserRecord>(passkeyUserKey(credential.user_id));
  if (!userEntry.value) return openaiError(401, "Unauthorized", "invalid_api_key");

  try {
    const verification = await verifyAuthenticationResponse({
      response,
      expectedChallenge: challengeRecord.challenge,
      expectedOrigin: challengeRecord.origin,
      expectedRPID: challengeRecord.rp_id,
      requireUserVerification: isPasskeyUserAdmin(userEntry.value),
      credential: {
        id: credential.credential_id,
        publicKey: base64UrlDecode(credential.public_key),
        counter: credential.sign_count,
      },
    });
    if (!verification.verified) {
      return openaiError(400, "Invalid passkey assertion", "invalid_request_error");
    }

    await kv.set(passkeyCredentialKey(credential.credential_id), {
      ...credential,
      sign_count: verification.authenticationInfo.newCounter,
    });

    const session = await createSession(kv, credential.user_id);
    return json(
      200,
      {
        token: session.token,
        user_id: userEntry.value.id,
        handle: userEntry.value.handle,
        expires_at_ms: session.expires_at_ms,
      },
      { "Cache-Control": "no-store" },
    );
  } catch {
    return openaiError(400, "Invalid passkey assertion", "invalid_request_error");
  }
};

export const handlePasskeySession = async (req: Request): Promise<Response> => {
  const session = await getPasskeySessionFromRequest(req);
  if (!session) return openaiError(401, "Unauthorized", "invalid_api_key");
  return json(
    200,
    {
      user: {
        id: session.user.id,
        handle: session.user.handle,
        is_admin: isPasskeyUserAdmin(session.user),
      },
      session: {
        created_at_ms: session.session.created_at_ms,
        expires_at_ms: session.session.expires_at_ms,
      },
    },
    { "Cache-Control": "no-store" },
  );
};

export const handlePasskeyLogout = async (req: Request): Promise<Response> => {
  const token = getBearerToken(req);
  if (token) {
    const kv = await kvPromise;
    if (kv) await kv.delete(passkeySessionKey(token));
  }
  return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
};

export const handlePasskeyUsersList = async (): Promise<Response> => {
  const kvOrError = await getKvOrError();
  if (kvOrError instanceof Response) return kvOrError;
  const users: PasskeyUserRecord[] = [];
  for await (const entry of kvOrError.list<PasskeyUserRecord>({ prefix: [...AUTH_PREFIX, "users"] })) {
    if (entry.value) users.push(entry.value);
  }
  users.sort((a, b) => b.updated_at_ms - a.updated_at_ms);
  return json(
    200,
    {
      object: "list",
      data: users.map(serializePasskeyUser),
    },
    { "Cache-Control": "no-store" },
  );
};

export const handlePasskeyUsersUpdate = async (req: Request): Promise<Response> => {
  const kvOrError = await getKvOrError();
  if (kvOrError instanceof Response) return kvOrError;
  const kv = kvOrError;

  let raw: unknown = null;
  try {
    raw = await req.json();
  } catch {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }
  if (!isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const id = getString(raw.id)?.trim() ?? "";
  if (!id) return openaiError(400, "id is required", "invalid_request_error");
  if (typeof raw.is_admin !== "boolean") {
    return openaiError(400, "is_admin must be a boolean", "invalid_request_error");
  }

  const key = passkeyUserKey(id);
  const entry = await kv.get<PasskeyUserRecord>(key);
  if (!entry.value) return openaiError(404, "Passkey user not found", "not_found");

  const next: PasskeyUserRecord = {
    ...entry.value,
    is_admin: raw.is_admin,
    updated_at_ms: nowMs(),
  };
  const commit = await kv.atomic().check(entry).set(key, next).commit();
  if (!commit.ok) {
    return openaiError(409, "Passkey user was modified concurrently; retry", "invalid_request_error");
  }

  return json(200, { user: serializePasskeyUser(next) }, { "Cache-Control": "no-store" });
};
