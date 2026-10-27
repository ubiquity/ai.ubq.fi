import { afterEach, beforeEach, test } from "bun:test";
import assert from "node:assert/strict";
import { authenticateAdmin } from "../src/auth/index.ts";
import { handlePassportRoute } from "../src/auth/passport.ts";
import { createPassportSession, getPassportSession, PassportSessionError, revokePassportSession } from "../src/auth/passport-sessions.ts";
import { setKvForTest } from "../src/kv.ts";
import { sha256Hex } from "../src/utils.ts";
import { cleanupHarness, kvStore, resetHarness, urlOf } from "./helpers/admin-auth-harness.ts";
import { createFetchMock } from "./helpers/fetch-mock.ts";

const ORIGIN = "https://ai.ubq.fi";
const COOKIE = "__Host-uos_ai_passport_session";
const binding = { clientId: "ai", audience: "ai", origin: ORIGIN, redirectUri: `${ORIGIN}/auth/callback` };
const grant = {
  token: `uos_app_session_${"a".repeat(43)}`,
  subject: "fixture-admin",
  ...binding,
  createdAt: "2000-01-01T00:00:00.000Z",
};
const originalFetch = globalThis.fetch;
let status = 200;
let isAdmin = true;
let scopes = ["admin"];
let reads = 0;
let bodyOverride: unknown;

const sessionBody = () => ({
  authenticated: true,
  session: { subject: grant.subject, ...binding, createdAt: grant.createdAt },
  user: { id: grant.subject, handle: "administrator", isAdmin, scopes },
});
const request = (token: string, method = "GET", path = "/admin/api-keys", extra: HeadersInit = {}) => {
  const headers = new Headers(extra);
  headers.set("Cookie", `${COOKIE}=${token}`);
  return new Request(`${ORIGIN}${path}`, { method, headers });
};
const unauthorized = (error: unknown) => error instanceof PassportSessionError && error.status === 401;
const unavailable = (error: unknown) => error instanceof PassportSessionError && error.status === 503;

beforeEach(() => {
  resetHarness();
  status = 200;
  isAdmin = true;
  scopes = ["admin"];
  reads = 0;
  bodyOverride = undefined;
  globalThis.fetch = createFetchMock(async (input, init) => {
    assert.equal(init?.method, "POST");
    assert.equal(init.redirect, "error");
    assert.deepEqual(JSON.parse(await new Response(init.body).text()), binding);
    assert.equal(new Headers(init.headers).get("Authorization"), `Bearer ${grant.token}`);
    const url = urlOf(input);
    if (url.endsWith("/revoke")) return new Response(null, { status: status === 200 ? 204 : status });
    assert.equal(url, "https://passport.ubq.fi/api/auth/sso/session");
    reads += 1;
    return Response.json(bodyOverride ?? sessionBody(), { status });
  });
});

afterEach(() => {
  globalThis.fetch = originalFetch;
  cleanupHarness();
});

test("permanent Passport sessions store a hashed local secret without an expiry or role snapshot", async () => {
  const token = await createPassportSession(grant, grant.subject);
  assert.match(token, /^uos_ai_passport_session_[A-Za-z0-9_-]{43}$/);
  const key = JSON.stringify(["uos_ai", "auth", "passport", "sessions", await sha256Hex(token)]);
  const record = kvStore.get(key);
  assert.deepEqual(record, { version: 1, subject: grant.subject, ...binding, createdAt: grant.createdAt, appSessionToken: grant.token });
  assert(!key.includes(token));
  const auth = await authenticateAdmin(request(token));
  assert(auth.ok);
  assert.equal(auth.method.kind, "passport_session");
  assert.equal(auth.method.user.exp, null);
  assert.equal(auth.method.user.jti, null);
  assert.equal(reads, 1);
});

test("every request reads current role and scopes, and a demotion immediately denies admin access", async () => {
  const token = await createPassportSession(grant, grant.subject);
  assert((await authenticateAdmin(request(token))).ok);
  isAdmin = false;
  scopes = [];
  assert.deepEqual(await getPassportSession(token), { subject: grant.subject, handle: "administrator", isAdmin: false, scopes: [] });
  const demoted = await authenticateAdmin(request(token));
  assert(!demoted.ok);
  assert.equal(demoted.response.status, 403);
  assert.equal(reads, 3);
  assert.equal(kvStore.size, 1);
});

test("persistent authority survives elapsed time beyond the original access lifespan", async () => {
  const token = await createPassportSession(grant, grant.subject);
  const now = Date.now;
  try {
    Date.now = () => now() + 366 * 24 * 60 * 60 * 1000;
    assert.equal((await getPassportSession(token)).subject, grant.subject);
  } finally {
    Date.now = now;
  }
  assert.equal(reads, 1);
});

test("unavailable introspection keeps the local session and recovers without another login", async () => {
  const token = await createPassportSession(grant, grant.subject);
  status = 503;
  await assert.rejects(getPassportSession(token), unavailable);
  assert.equal(kvStore.size, 1);
  globalThis.fetch = createFetchMock(() => {
    throw new Error("synthetic transport outage");
  });
  await assert.rejects(getPassportSession(token), unavailable);
  assert.equal(kvStore.size, 1);
  globalThis.fetch = createFetchMock(() => Promise.resolve(Response.json(sessionBody())));
  assert.equal((await getPassportSession(token)).subject, grant.subject);
});

test("authoritative revocation deletes local authority and clears the browser cookie", async () => {
  const token = await createPassportSession(grant, grant.subject);
  status = 401;
  const revoked = await authenticateAdmin(request(token));
  assert(!revoked.ok);
  assert.equal(revoked.response.status, 401);
  assert(revoked.response.headers.getSetCookie().some((value) => value.startsWith(`${COOKIE}=; Max-Age=0;`)));
  assert.equal(kvStore.size, 0);
  status = 200;
  await assert.rejects(getPassportSession(token), unauthorized);
  assert.equal(reads, 1);
});

test("creation rejects every wrong binding or subject and cannot replace a valid session on failure", async () => {
  const token = await createPassportSession(grant, grant.subject);
  for (const patch of [
    { clientId: "other" },
    { audience: "other" },
    { origin: "https://evil.example" },
    { redirectUri: `${ORIGIN}/other` },
    { subject: "other" },
    { token: "legacy.jwt.token" },
  ]) {
    await assert.rejects(createPassportSession({ ...grant, ...patch }, grant.subject, token), unavailable);
    assert.equal(kvStore.size, 1);
  }
  assert.equal((await getPassportSession(token)).subject, grant.subject);
});

test("introspection rejects substituted identities and bindings without cached-role fallback", async () => {
  const token = await createPassportSession(grant, grant.subject);
  for (const body of [
    { ...sessionBody(), session: { ...sessionBody().session, audience: "other" } },
    { ...sessionBody(), session: { ...sessionBody().session, origin: "https://evil.example" } },
    { ...sessionBody(), user: { ...sessionBody().user, id: "other" } },
    { ...sessionBody(), user: { ...sessionBody().user, isAdmin: "true" } },
  ]) {
    bodyOverride = body;
    await assert.rejects(getPassportSession(token), unavailable);
    assert.equal(kvStore.size, 1);
  }
});

test("local logout revokes first and truthfully reports unavailable upstream revocation", async () => {
  const token = await createPassportSession(grant, grant.subject);
  globalThis.fetch = createFetchMock(() => {
    assert.equal(kvStore.size, 0);
    return Promise.resolve(new Response(null, { status: 503 }));
  });
  const logout = await handlePassportRoute(request(token, "POST", "/auth/logout", { Origin: ORIGIN }), "/auth/logout");
  assert.equal(logout?.status, 204);
  assert.equal(logout.headers.get("X-UOS-Passport-Upstream-Revoke"), "unavailable");
  await assert.rejects(getPassportSession(token), unauthorized);
});

test("cross-origin logout never revokes a session, and local store failure cannot claim logout", async () => {
  const token = await createPassportSession(grant, grant.subject);
  const foreign = await handlePassportRoute(request(token, "POST", "/auth/logout", { Origin: "https://evil.example" }), "/auth/logout");
  assert.equal(foreign?.status, 403);
  assert.equal(kvStore.size, 1);
  setKvForTest(null);
  const unavailableLogout = await handlePassportRoute(request(token, "POST", "/auth/logout", { Origin: ORIGIN }), "/auth/logout");
  assert.equal(unavailableLogout?.status, 503);
  assert.equal(unavailableLogout.headers.getSetCookie().length, 0);
});

test("logout wins over an in-flight successful introspection without recreating authority", async () => {
  const token = await createPassportSession(grant, grant.subject);
  let release: (response: Response) => void = () => {};
  let started: () => void = () => {};
  const reached = new Promise<void>((resolve) => {
    started = resolve;
  });
  globalThis.fetch = createFetchMock((input) => {
    if (urlOf(input).endsWith("/revoke")) return Promise.resolve(new Response(null, { status: 204 }));
    started();
    return new Promise<Response>((resolve) => {
      release = resolve;
    });
  });
  const pending = getPassportSession(token);
  await reached;
  assert.equal(await revokePassportSession(token), "revoked");
  release(Response.json(sessionBody()));
  await assert.rejects(pending, unauthorized);
  assert.equal(kvStore.size, 0);
});

test("a successfully stored replacement atomically removes the previous local session", async () => {
  const previous = await createPassportSession(grant, grant.subject);
  const replacement = await createPassportSession(grant, grant.subject, previous);
  assert.notEqual(previous, replacement);
  assert.equal(kvStore.size, 1);
  await assert.rejects(getPassportSession(previous), unauthorized);
  assert.equal((await getPassportSession(replacement)).subject, grant.subject);
});

test("an authoritative revocation still removes authority when the rejected body cannot be cancelled", async () => {
  const token = await createPassportSession(grant, grant.subject);
  globalThis.fetch = createFetchMock(() =>
    Promise.resolve(
      new Response(
        new ReadableStream({
          cancel: () => Promise.reject(new Error("synthetic cancel failure")),
        }),
        { status: 401 }
      )
    )
  );
  await assert.rejects(getPassportSession(token), unauthorized);
  assert.equal(kvStore.size, 0);
});
