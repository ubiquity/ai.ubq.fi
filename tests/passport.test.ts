import { afterEach, beforeEach, test } from "bun:test";
import assert from "node:assert/strict";
import { createFetchMock } from "./helpers/fetch-mock.ts";
import { cleanupHarness, kvStub, resetHarness, urlOf } from "./helpers/admin-auth-harness.ts";
import { authenticateAdmin, authenticateClient } from "../src/auth/index.ts";
import { handlePassportRoute, resetPassportCacheForTest } from "../src/auth/passport.ts";
import handler from "../src/handler/index.ts";
import { resolveIdempotencyPrincipal } from "../src/handler/http.ts";
import { setKvForTest } from "../src/kv.ts";
import { base64UrlEncode } from "../src/utils.ts";

const ORIGIN = "https://ai.ubq.fi";
const ISSUER = "https://passport.ubq.fi";
const SESSION_COOKIE = "__Host-uos_ai_passport_session";
const STATE_COOKIE = "__Host-uos_ai_passport_state";
const VERIFIER_COOKIE = "__Host-uos_ai_passport_verifier";
const appGrant = {
  token: `uos_app_session_${"a".repeat(43)}`,
  subject: "fixture-admin",
  clientId: "ai",
  audience: "ai",
  origin: ORIGIN,
  redirectUri: `${ORIGIN}/auth/callback`,
  createdAt: "2026-01-01T00:00:00.000Z",
};
const keys = await crypto.subtle.generateKey(
  { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" },
  true,
  ["sign", "verify"]
);
const publicKey = await crypto.subtle.exportKey("jwk", keys.publicKey);
const originalFetch = globalThis.fetch;
let exchangedToken = "";
let exchanges: Record<string, unknown>[] = [];
let jwksStatus = 200;

const sign = async (overrides: Record<string, unknown> = {}, headerOverrides: Record<string, unknown> = {}): Promise<string> => {
  const now = Math.floor(Date.now() / 1000);
  const encode = (value: unknown): string => base64UrlEncode(new TextEncoder().encode(JSON.stringify(value)));
  const header = encode({ alg: "RS256", typ: "JWT", kid: "fixture-passport", ...headerOverrides });
  const body = encode({
    iss: ISSUER,
    sub: "fixture-admin",
    handle: "administrator",
    isAdmin: true,
    scopes: [],
    aud: "ai",
    clientId: "ai",
    auth: "sso",
    iat: now,
    exp: now + 3600,
    jti: "fixture-id",
    ...overrides,
  });
  const input = `${header}.${body}`;
  const signature = await crypto.subtle.sign("RSASSA-PKCS1-v1_5", keys.privateKey, new TextEncoder().encode(input));
  return `${input}.${base64UrlEncode(new Uint8Array(signature))}`;
};

const request = (path: string, token = "", init: RequestInit = {}): Request => {
  const headers = new Headers(init.headers);
  if (token) headers.set("Cookie", `${SESSION_COOKIE}=${token}`);
  return new Request(`${ORIGIN}${path}`, { ...init, headers });
};

beforeEach(() => {
  resetHarness();
  resetPassportCacheForTest();
  exchanges = [];
  jwksStatus = 200;
  globalThis.fetch = createFetchMock(async (input, init) => {
    const url = urlOf(input);
    if (url === `${ISSUER}/api/auth/jwks`)
      return Response.json({ keys: [{ ...publicKey, kid: "fixture-passport", alg: "RS256", use: "sig" }] }, { status: jwksStatus });
    if (url === `${ISSUER}/api/auth/sso/exchange`) {
      assert.equal(init?.method, "POST");
      assert.equal(init.redirect, "error");
      exchanges.push(JSON.parse(await new Response(init.body).text()) as Record<string, unknown>);
      return Response.json({ accessToken: exchangedToken, appSession: appGrant });
    }
    if (url === `${ISSUER}/api/auth/sso/session`)
      return Response.json({
        authenticated: true,
        session: {
          subject: appGrant.subject,
          clientId: "ai",
          audience: "ai",
          origin: ORIGIN,
          redirectUri: appGrant.redirectUri,
          createdAt: appGrant.createdAt,
        },
        user: { id: appGrant.subject, handle: "administrator", isAdmin: true, scopes: [] },
      });
    if (url === `${ISSUER}/api/auth/sso/session/revoke`) return new Response(null, { status: 204 });
    throw new Error(`Unexpected fetch: ${url}`);
  });
});

afterEach(() => {
  globalThis.fetch = originalFetch;
  resetPassportCacheForTest();
  cleanupHarness();
});

const begin = async (returnTo = "/admin") => {
  const response = await handler(request(`/auth/login?return_to=${encodeURIComponent(returnTo)}`));
  assert.equal(response.status, 302);
  const authorize = new URL(response.headers.get("Location") ?? "");
  assert.equal(authorize.origin, ISSUER);
  assert.equal(authorize.pathname, "/authorize");
  assert.equal(authorize.searchParams.get("client_id"), "ai");
  assert.equal(authorize.searchParams.get("audience"), "ai");
  assert.equal(authorize.searchParams.get("origin"), ORIGIN);
  assert.equal(authorize.searchParams.get("redirect_uri"), `${ORIGIN}/auth/callback`);
  const state = authorize.searchParams.get("state") ?? "";
  assert.match(state, /^[A-Za-z0-9_-]{43}\.(?:admin|chat)$/);
  assert.equal(authorize.searchParams.get("code_challenge_method"), "S256");
  const responseCookies = response.headers.getSetCookie();
  const stateCookie = responseCookies[0];
  const codeVerifier =
    responseCookies
      .find((value) => value.startsWith(`${VERIFIER_COOKIE}=`))
      ?.split(";")[0]
      .slice(VERIFIER_COOKIE.length + 1) ?? "";
  assert.match(codeVerifier, /^[A-Za-z0-9_-]{43}$/);
  assert.equal(
    authorize.searchParams.get("code_challenge"),
    base64UrlEncode(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(codeVerifier))))
  );
  assert(stateCookie.includes("HttpOnly; Secure; SameSite=Lax"));
  assert.equal(response.headers.get("Cache-Control"), "no-store");
  return { state, codeVerifier, stateCookie: responseCookies.map((value) => value.split(";")[0]).join("; ") };
};

test("Bun Passport button destination completes exchange, sets host cookies, returns to admin and authenticates protected reads", async () => {
  exchangedToken = await sign();
  const { state, stateCookie, codeVerifier } = await begin();
  const response = await handler(request(`/auth/callback?state=${state}&code=single-use-code`, "", { headers: { Cookie: stateCookie } }));
  assert.equal(response.status, 302);
  assert.equal(response.headers.get("Location"), "/admin");
  assert.deepEqual(exchanges, [
    { code: "single-use-code", clientId: "ai", audience: "ai", origin: ORIGIN, redirectUri: `${ORIGIN}/auth/callback`, codeVerifier, appSession: true },
  ]);
  const cookies = response.headers.getSetCookie();
  assert.equal(cookies.length, 3);
  const sessionCookie = cookies.find((value) => value.startsWith(`${SESSION_COOKIE}=uos_ai_passport_session_`));
  assert(sessionCookie);
  assert(sessionCookie.endsWith("HttpOnly; Secure; SameSite=Lax"));
  const sessionToken = sessionCookie.split(";")[0].slice(SESSION_COOKIE.length + 1);
  assert(cookies.some((value) => value.startsWith(`${STATE_COOKIE}=; Max-Age=0;`)));
  assert(cookies.some((value) => value.startsWith(`${VERIFIER_COOKIE}=; Max-Age=0;`)));
  const auth = await handler(request("/uos/auth", sessionToken));
  assert.equal(auth.status, 200);
  const payload = await auth.json();
  assert.equal(payload.auth.is_admin, true);
  assert.equal(payload.auth.principal, "passport-user:fixture-admin");
  assert.equal(payload.auth.method.kind, "passport_session");
  assert.equal(payload.auth.token.present, false);
  assert.equal(payload.auth.method.user.exp, null);
  assert.equal(payload.auth.method.user.jti, null);
  assert(auth.headers.getSetCookie().some((value) => value.startsWith(`${SESSION_COOKIE}=${sessionToken}; Max-Age=`)));
  assert.equal((await handler(request("/admin/api-keys", sessionToken))).status, 200);
  const client = await authenticateClient(request("/v1/models", sessionToken));
  assert(client.ok);
  assert.equal(await resolveIdempotencyPrincipal(client), "passport-user:fixture-admin");
});

test("Passport callback rejects absent, mismatched and duplicated state before code exchange", async () => {
  const { state, stateCookie } = await begin();
  for (const [query, value] of [
    [`state=${state}&code=code`, ""],
    [`state=${state}&code=code`, `${STATE_COOKIE}=${state}`],
    [`state=${state}&code=code`, `${STATE_COOKIE}=other`],
    [`state=${state}&state=${state}&code=code`, stateCookie],
    [`state=${state}&code=one&code=two`, stateCookie],
    [`state=${state}`, stateCookie],
  ]) {
    const response = await handler(request(`/auth/callback?${query}`, "", { headers: { Cookie: value } }));
    assert.equal(response.status, 401);
    assert(response.headers.getSetCookie().some((cookie) => cookie.startsWith(`${STATE_COOKIE}=; Max-Age=0;`)));
  }
  assert.equal(exchanges.length, 0);
});

test("failed Passport callback clears only transient cookies and never creates a session from an agent", async () => {
  exchangedToken = await sign({ agentTokenId: "agent-id" });
  const { state, stateCookie } = await begin();
  const response = await handler(
    request(`/auth/callback?state=${state}&code=code`, "", { headers: { Cookie: `${stateCookie}; ${SESSION_COOKIE}=existing-session` } })
  );
  assert.equal(response.status, 401);
  assert.deepEqual(
    response.headers.getSetCookie().map((value) => value.split("=")[0]),
    [STATE_COOKIE, VERIFIER_COOKIE]
  );
});

test("Passport callback only returns to fixed local destinations", async () => {
  exchangedToken = await sign();
  const { state, stateCookie } = await begin("https://evil.example/admin");
  const response = await handler(request(`/auth/callback?state=${state}&code=code`, "", { headers: { Cookie: stateCookie } }));
  assert.equal(response.headers.get("Location"), "/chat");
});

test("Passport admin rejects ordinary users, agent cookies, JWT bearers and cross-origin mutations", async () => {
  const nonAdmin = await authenticateAdmin(request("/admin/api-keys", await sign({ isAdmin: false })));
  assert(!nonAdmin.ok);
  assert.equal(nonAdmin.response.status, 403);
  const agent = await authenticateAdmin(request("/admin/api-keys", await sign({ agentTokenId: "agent-id" })));
  assert(!agent.ok);
  assert.equal(agent.response.status, 401);
  const admin = await sign();
  for (const token of ["", admin]) {
    const bearer = await authenticateAdmin(request("/admin/api-keys", token, { headers: { Authorization: `Bearer ${admin}` } }));
    assert(!bearer.ok);
    assert.equal(bearer.response.status, 401);
  }
  for (const headers of [new Headers({ Origin: "https://evil.example" }), new Headers({ "Sec-Fetch-Site": "cross-site" })]) {
    const foreign = await handler(request("/admin/api-keys", admin, { method: "POST", headers, body: "{}" }));
    assert.equal(foreign.status, 403);
  }
  assert((await authenticateAdmin(request("/admin/api-keys", admin, { method: "POST", headers: { Origin: ORIGIN, "Sec-Fetch-Site": "same-origin" } }))).ok);
});

test("Passport verifies signature, expiry, issuer, audience, client, algorithm and key source", async () => {
  const now = Math.floor(Date.now() / 1000);
  for (const claims of [{ iss: "https://evil.example" }, { aud: "other" }, { clientId: "other" }, { exp: now - 1 }, { iat: now + 120 }, { isAdmin: "true" }]) {
    const result = await authenticateAdmin(request("/admin/api-keys", await sign(claims)));
    assert(!result.ok);
    assert.equal(result.response.status, 401);
  }
  for (const header of [{ alg: "HS256" }, { jku: "https://evil.example/keys" }, { jwk: publicKey }]) {
    const result = await authenticateAdmin(request("/admin/api-keys", await sign({}, header)));
    assert(!result.ok);
    assert.equal(result.response.status, 401);
  }
  const token = await sign();
  const parts = token.split(".");
  parts[1] = base64UrlEncode(new TextEncoder().encode(JSON.stringify({ isAdmin: true })));
  const forged = await authenticateAdmin(request("/admin/api-keys", parts.join(".")));
  assert(!forged.ok);
  assert.equal(forged.response.status, 401);
  const signature = token.split(".");
  signature[2] = `${signature[2].startsWith("A") ? "B" : "A"}${signature[2].slice(1)}`;
  const badSignature = await authenticateAdmin(request("/admin/api-keys", signature.join(".")));
  assert(!badSignature.ok);
  assert.equal(badSignature.response.status, 401);
});

test("Passport unavailable keys fail closed, proxy origin stays fixed and logout clears only its cookie", async () => {
  jwksStatus = 503;
  const token = await sign();
  const unavailable = await authenticateAdmin(request("/admin/api-keys", token));
  assert(!unavailable.ok);
  assert.equal(unavailable.response.status, 502);
  const proxyUrl = new URL("/auth/login?return_to=%2Fadmin", ORIGIN);
  proxyUrl.protocol = "http:";
  const proxy = await handler(new Request(proxyUrl, { headers: { "X-Forwarded-Proto": "https", "X-Forwarded-Host": "evil.example" } }));
  assert.equal(proxy.status, 302);
  assert.equal(new URL(proxy.headers.get("Location") ?? "").searchParams.get("origin"), ORIGIN);
  const foreignHost = await handlePassportRoute(new Request("https://evil.example/auth/login"), "/auth/login");
  assert.equal(foreignHost?.status, 401);
  assert.equal((await handler(request("/auth/logout", token, { method: "POST", headers: { Origin: "https://evil.example" } }))).status, 403);
  const logout = await handler(request("/auth/logout", token, { method: "POST", headers: { Origin: ORIGIN } }));
  assert.equal(logout.status, 204);
  assert.deepEqual(logout.headers.getSetCookie(), [`${SESSION_COOKIE}=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax`]);
  setKvForTest(kvStub);
  assert.equal((await handler(request("/admin/api-keys"))).status, 401);
});
