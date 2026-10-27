import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { stringKey } from "./contracts.ts";

const sessionCookie = "__Host-uos_ai_passport_session";
const stateCookie = "__Host-uos_ai_passport_state";
const verifierCookie = "__Host-uos_ai_passport_verifier";
type Mode = "normal" | "demoted" | "unavailable" | "unauthorized" | "reject_cancel" | "malformed" | "oversize" | "substituted" | "revoke_unavailable";
type SourceRow = Readonly<{ revision: string; expires_at_ms: number | null; value: unknown }>;
/** The shared fixture runner must bind these to the compiled entrypoint, its
 * actual common AuthorityStore and the same persisted namespace across restart.
 * No method here writes auth rows or manufactures an authentication result. */
export type AuthPassportHarness = Readonly<{
  request(path: string, init?: RequestInit): Promise<Response>;
  mode(value: Mode): Promise<void>;
  readSource(key: string): Promise<SourceRow | null>;
  restart(): Promise<void>;
  holdIntrospection(): Promise<void>;
  waitForIntrospection(): Promise<void>;
  releaseIntrospection(): Promise<void>;
  holdRevoke(): Promise<void>;
  waitForRevoke(): Promise<void>;
  releaseRevoke(): Promise<void>;
  cases: string[];
}>;
function object(value: unknown): Record<string, unknown> {
  assert(value !== null && typeof value === "object" && !Array.isArray(value));
  return value as Record<string, unknown>;
}
const pair = (cookie: string): string => cookie.split(";", 1)[0] ?? "";
const key = (token: string): string => stringKey("uos_ai", "auth", "passport", "sessions", createHash("sha256").update(token).digest("hex"));
async function report(context: AuthPassportHarness, cookie: string): Promise<Response> {
  return context.request("/uos/auth", { headers: { cookie } });
}
async function login(context: AuthPassportHarness, code = "admin", previous?: string): Promise<{ cookie: string; token: string }> {
  const start = await context.request("/auth/login?return_to=%2Fadmin", { redirect: "manual" });
  assert.equal(start.status, 302);
  const url = new URL(start.headers.get("location") ?? "");
  assert.equal(url.origin, "https://passport.ubq.fi");
  for (const [name, value] of Object.entries({ client_id: "ai", audience: "ai", origin: "https://ai.ubq.fi", redirect_uri: "https://ai.ubq.fi/auth/callback", code_challenge_method: "S256" })) assert.equal(url.searchParams.get(name), value);
  const state = url.searchParams.get("state") ?? "";
  assert.match(state, /^[A-Za-z0-9_-]{43}\.admin$/u);
  const transient = start.headers.getSetCookie();
  assert.equal(transient.length, 2);
  const stateHeader = transient.find((value) => value.startsWith(`${stateCookie}=`));
  const verifierHeader = transient.find((value) => value.startsWith(`${verifierCookie}=`));
  assert(stateHeader && verifierHeader);
  assert.equal(pair(stateHeader), `${stateCookie}=${state}`);
  const verifier = pair(verifierHeader).slice(verifierCookie.length + 1);
  assert.match(verifier, /^[A-Za-z0-9_-]{43}$/u);
  assert.notEqual(verifier, state.slice(0, 43));
  assert.equal(url.searchParams.get("code_challenge"), createHash("sha256").update(verifier).digest("base64url"));
  for (const cookie of transient) for (const attribute of ["Max-Age=600", "Path=/", "HttpOnly", "Secure", "SameSite=Lax"]) assert(cookie.includes(attribute));
  const cookie = [...transient.map(pair), ...(previous ? [previous] : [])].join("; ");
  const callback = await context.request(`/auth/callback?state=${encodeURIComponent(state)}&code=${code}`, { headers: { cookie }, redirect: "manual" });
  if (code === "agent") {
    assert.equal(callback.status, 401);
    assert.equal(callback.headers.getSetCookie().length, 2);
    assert(!callback.headers.getSetCookie().some((value) => value.startsWith(`${sessionCookie}=`)));
    return { cookie: "", token: "" };
  }
  assert.equal(callback.status, 302);
  assert.equal(callback.headers.get("location"), "/admin");
  assert.equal(callback.headers.get("cache-control"), "no-store");
  assert.equal(callback.headers.get("referrer-policy"), "no-referrer");
  const cookies = callback.headers.getSetCookie();
  assert.equal(cookies.length, 3, "actual transport must append three separate Set-Cookie values");
  const local = cookies.find((value) => value.startsWith(`${sessionCookie}=`));
  assert(local && local.includes("Max-Age=34560000"));
  assert(cookies.some((value) => value.startsWith(`${stateCookie}=`) && value.includes("Max-Age=0")));
  assert(cookies.some((value) => value.startsWith(`${verifierCookie}=`) && value.includes("Max-Age=0")));
  const localPair = pair(local);
  const token = localPair.slice(sessionCookie.length + 1);
  assert.match(token, /^uos_ai_passport_session_[A-Za-z0-9_-]{43}$/u);
  return { cookie: localPair, token };
}
async function identity(response: Response): Promise<Record<string, unknown>> {
  assert.equal(response.status, 200);
  const auth = object(object(await response.json()).auth);
  assert.equal(auth.principal, "passport-user:fixture-admin");
  const method = object(auth.method);
  assert.equal(method.kind, "passport_session");
  const user = object(method.user);
  assert.equal(user.auth, "sso");
  for (const field of ["exp", "jti", "agent_token_id"]) assert.equal(user[field], null);
  assert.equal(object(auth.token).present, false);
  assert.equal(auth.is_super_admin, false);
  return user;
}

/** Definitions count as execution evidence only when the compiled Worker runner calls this. */
export async function runAuthPassportCases(context: AuthPassportHarness): Promise<void> {
  await context.mode("normal");
  const first = await login(context);
  const row = await context.readSource(key(first.token));
  assert(row && row.revision && row.expires_at_ms === null);
  const saved = object(row.value);
  assert.equal(saved.version, 1);
  assert.equal(saved.subject, "fixture-admin");
  assert.equal(Object.keys(saved).length, 8);
  assert.match(String(saved.appSessionToken), /^uos_app_session_[A-Za-z0-9_-]{43}$/u);
  for (const forbidden of ["handle", "isAdmin", "scopes", "exp", "expiresAt", "token"]) assert(!Object.hasOwn(saved, forbidden));
  let response = await report(context, first.cookie);
  const renewed = response.headers.getSetCookie();
  assert(renewed.some((value) => value.startsWith(`${sessionCookie}=${first.token};`) && value.includes("Max-Age=34560000")));
  assert.equal((await identity(response)).is_admin, true);
  context.cases.push("auth_passport_pkce_real_rs256_durable_common_authority_and_repeated_cookies");

  await context.restart();
  assert.equal((await identity(await report(context, first.cookie))).is_admin, true);
  assert.equal((await context.readSource(key(first.token)))?.revision, row.revision);
  context.cases.push("auth_passport_existing_authority_survives_worker_restart");

  await context.mode("demoted");
  assert.equal((await identity(await report(context, first.cookie))).is_admin, false);
  const denied = await context.request("/admin/api-keys", { headers: { cookie: first.cookie } });
  assert.equal(denied.status, 403);
  await denied.arrayBuffer();
  context.cases.push("auth_passport_central_role_demotion_is_immediate");
  for (const mode of ["unavailable", "malformed", "oversize", "substituted"] as const) {
    await context.mode(mode);
    response = await report(context, first.cookie);
    assert.equal(response.status, 503);
    assert(!response.headers.getSetCookie().some((value) => value.startsWith(`${sessionCookie}=`)));
    await response.arrayBuffer();
    assert.equal((await context.readSource(key(first.token)))?.revision, row.revision);
  }
  await context.mode("normal");
  await identity(await report(context, first.cookie));
  context.cases.push("auth_passport_503_preserves_authority_and_recovers_without_login");

  const foreign = await context.request("/auth/logout", { method: "POST", headers: { cookie: first.cookie, origin: "https://foreign.invalid", "sec-fetch-site": "cross-site" } });
  assert.equal(foreign.status, 403);
  await foreign.arrayBuffer();
  assert(await context.readSource(key(first.token)));
  await context.mode("reject_cancel");
  response = await report(context, first.cookie);
  assert.equal(response.status, 401);
  assert(response.headers.getSetCookie().some((value) => value.startsWith(`${sessionCookie}=`) && value.includes("Max-Age=0")));
  await response.arrayBuffer();
  assert.equal(await context.readSource(key(first.token)), null);
  context.cases.push("auth_passport_authoritative_401_deletes_even_with_rejected_body_cancellation");

  await context.mode("normal");
  const race = await login(context);
  await context.holdIntrospection();
  const pending = report(context, race.cookie);
  await context.waitForIntrospection();
  const logout = await context.request("/auth/logout", { method: "POST", headers: { cookie: race.cookie, origin: "https://ai.ubq.fi", "sec-fetch-site": "same-origin" } });
  assert.equal(logout.status, 204);
  assert.equal(await context.readSource(key(race.token)), null);
  await context.releaseIntrospection();
  const stale = await pending;
  assert.equal(stale.status, 401);
  await stale.arrayBuffer();
  assert.equal(await context.readSource(key(race.token)), null);
  context.cases.push("auth_passport_logout_wins_over_inflight_introspection_without_republication");

  const previous = await login(context);
  const replacement = await login(context, "admin", previous.cookie);
  assert.notEqual(previous.token, replacement.token);
  assert.equal(await context.readSource(key(previous.token)), null);
  assert(await context.readSource(key(replacement.token)));
  await context.mode("revoke_unavailable");
  await context.holdRevoke();
  const revokePending = context.request("/auth/logout", { method: "POST", headers: { cookie: replacement.cookie } });
  await context.waitForRevoke();
  assert.equal(await context.readSource(key(replacement.token)), null, "local authority must be gone while upstream revoke is still held");
  await context.releaseRevoke();
  const unavailable = await revokePending;
  assert.equal(unavailable.status, 204);
  assert.equal(unavailable.headers.get("x-uos-passport-upstream-revoke"), "unavailable");
  assert.equal(await context.readSource(key(replacement.token)), null);
  context.cases.push("auth_passport_atomic_replacement_and_local_first_best_effort_logout");
  await context.mode("normal");
  await login(context, "agent");
}
