// Controlled, credential-free Passport service for the compiled Worker fixture.
// This module is never a deployment entrypoint or a production authentication host.
const binding = Object.freeze({ clientId: "ai", audience: "ai", origin: "https://ai.ubq.fi", redirectUri: "https://ai.ubq.fi/auth/callback" });
const encoder = new TextEncoder();
const gates = new WeakMap();
const modes = new Set(["normal", "demoted", "unavailable", "unauthorized", "reject_cancel", "malformed", "oversize", "substituted", "revoke_unavailable", "exchange_network", "exchange_missing", "exchange_http", "exchange_oversize", "exchange_malformed"]);
const base64url = (bytes) => btoa(String.fromCharCode(...bytes)).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
const randomToken = () => `uos_app_session_${base64url(crypto.getRandomValues(new Uint8Array(32)))}`;
const matchesBinding = (value) => value !== null && typeof value === "object" && Object.entries(binding).every(([key, expected]) => value[key] === expected);
const digest = async (value) => Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", encoder.encode(value))), (byte) => byte.toString(16).padStart(2, "0")).join("");
const read = (storage, name) => {
  const rows = storage.sql.exec("SELECT value FROM auth_passport_fixture WHERE name = ?", name).toArray();
  return rows.length ? JSON.parse(rows[0].value) : null;
};
const write = (storage, name, value) => storage.sql.exec("INSERT INTO auth_passport_fixture(name,value) VALUES (?,?) ON CONFLICT(name) DO UPDATE SET value=excluded.value", name, JSON.stringify(value));
const remove = (storage, name) => storage.sql.exec("DELETE FROM auth_passport_fixture WHERE name = ?", name);
const prepare = (storage) => storage.sql.exec("CREATE TABLE IF NOT EXISTS auth_passport_fixture (name TEXT PRIMARY KEY, value TEXT NOT NULL)");
async function signingIdentity(storage) {
  let keys = read(storage, "keys");
  if (!keys) {
    const pair = await crypto.subtle.generateKey({ name: "RSASSA-PKCS1-v1_5", hash: "SHA-256", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]) }, true, ["sign", "verify"]);
    keys = { public: await crypto.subtle.exportKey("jwk", pair.publicKey), private: await crypto.subtle.exportKey("jwk", pair.privateKey) };
    write(storage, "keys", keys);
  }
  return keys;
}
async function sign(storage, code, subject) {
  const keys = await signingIdentity(storage);
  const key = await crypto.subtle.importKey("jwk", keys.private, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["sign"]);
  const now = Math.floor(Date.now() / 1000);
  const claims = { iss: "https://passport.ubq.fi", sub: subject, handle: "fixture", isAdmin: code !== "user", scopes: ["ai"], aud: "ai", clientId: "ai", auth: "sso", iat: now, exp: now + 3600, jti: "fixture-jwt", ...(code === "agent" ? { agentTokenId: "fixture-agent" } : {}) };
  const header = base64url(encoder.encode(JSON.stringify({ alg: "RS256", typ: "JWT", kid: "auth-passport-fixture-v1" })));
  const payload = base64url(encoder.encode(JSON.stringify(claims)));
  const message = `${header}.${payload}`;
  return `${message}.${base64url(new Uint8Array(await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, encoder.encode(message))))}`;
}
function gate(storage) {
  let value = gates.get(storage);
  if (!value) { value = { hold: false, entered: 0, release: null, wait: Promise.resolve(), enteredWait: Promise.resolve(), enteredResolve: null, revokeHold: false, revokeWait: Promise.resolve(), revokeRelease: null, revokeEntered: Promise.resolve(), revokeResolve: null }; gates.set(storage, value); }
  return value;
}
/** Register only on the existing fixture-control DO, which owns these synthetic rows. */
export async function authPassportControl(request, storage) {
  if (new URL(request.url).pathname !== "/auth-passport/control") return null;
  prepare(storage);
  if (request.method === "GET") return Response.json({ mode: read(storage, "mode") ?? "normal", introspections: read(storage, "introspections") ?? 0, entered: gate(storage).entered, lastRevoke: read(storage, "last-revoke") });
  if (request.method !== "POST") return new Response(null, { status: 405 });
  const command = await request.json();
  if (command.operation === "mode" && modes.has(command.value)) write(storage, "mode", command.value);
  else if (command.operation === "created_at" && typeof command.value === "string") write(storage, "created-at", command.value);
  else if (command.operation === "created_at_reset") remove(storage, "created-at");
  else if (command.operation === "hold") {
    const pending = gate(storage); pending.hold = true; pending.entered = 0; pending.wait = new Promise((resolve) => { pending.release = resolve; }); pending.enteredWait = new Promise((resolve) => { pending.enteredResolve = resolve; });
  } else if (command.operation === "release") {
    const pending = gate(storage); pending.hold = false; pending.release?.(); pending.release = null;
  } else if (command.operation === "wait_entered") await gate(storage).enteredWait;
  else if (command.operation === "hold_revoke") {
    const pending = gate(storage); pending.revokeHold = true; pending.revokeWait = new Promise((resolve) => { pending.revokeRelease = resolve; }); pending.revokeEntered = new Promise((resolve) => { pending.revokeResolve = resolve; });
  } else if (command.operation === "wait_revoke") await gate(storage).revokeEntered;
  else if (command.operation === "release_revoke") { const pending = gate(storage); pending.revokeHold = false; pending.revokeRelease?.(); }
  else return new Response(null, { status: 400 });
  return new Response(null, { status: 204 });
}
/** Every request is dispatched by the fixture's exact passport.ubq.fi service binding. */
export async function authPassportUpstream(request, storage) {
  const url = new URL(request.url);
  if (url.hostname !== "passport.ubq.fi") return null;
  prepare(storage);
  if (request.method === "GET" && url.pathname === "/api/auth/jwks") {
    const keys = await signingIdentity(storage);
    return Response.json({ keys: [{ ...keys.public, kid: "auth-passport-fixture-v1", alg: "RS256", use: "sig" }] });
  }
  if (request.method !== "POST") return new Response(null, { status: 405 });
  const body = await request.json();
  if (!matchesBinding(body)) return new Response(null, { status: 400 });
  if (url.pathname === "/api/auth/sso/exchange") {
    const mode = read(storage, "mode") ?? "normal";
    if (mode === "exchange_network") throw new Error("Synthetic exchange transport failure");
    if (mode === "exchange_missing") return new Response(null);
    if (mode === "exchange_http") return new Response(null, { status: 503 });
    if (mode === "exchange_oversize") return new Response("x".repeat(65_537));
    if (mode === "exchange_malformed") return new Response("{");
    if (body.appSession !== true || !/^[A-Za-z0-9_-]{43}$/u.test(body.codeVerifier) || !["admin", "user", "agent", "utf16"].includes(body.code)) return new Response(null, { status: 400 });
    const subject = body.code === "utf16" ? "fixture-\ud800" : `fixture-${body.code}`;
    const grant = { ...binding, subject, createdAt: read(storage, "created-at") ?? new Date().toUTCString(), token: randomToken(), fixtureCode: body.code };
    write(storage, `grant:${await digest(grant.token)}`, grant);
    return Response.json({ accessToken: await sign(storage, body.code, subject), appSession: grant });
  }
  const token = request.headers.get("authorization")?.match(/^Bearer (uos_app_session_[A-Za-z0-9_-]{43})$/u)?.[1];
  const grant = token ? read(storage, `grant:${await digest(token)}`) : null;
  if (!grant) return new Response(null, { status: 401 });
  const mode = read(storage, "mode") ?? "normal";
  if (url.pathname === "/api/auth/sso/session/revoke") {
    const pending = gate(storage); pending.revokeResolve?.(); if (pending.revokeHold) await pending.revokeWait;
    write(storage, "last-revoke", { appBearer: true, status: mode === "revoke_unavailable" ? 503 : 204 });
    if (mode === "revoke_unavailable") return new Response(null, { status: 503 });
    remove(storage, `grant:${await digest(token)}`);
    return new Response(null, { status: 204 });
  }
  if (url.pathname !== "/api/auth/sso/session") return new Response(null, { status: 404 });
  write(storage, "introspections", (read(storage, "introspections") ?? 0) + 1);
  const pending = gate(storage); pending.entered += 1; pending.enteredResolve?.();
  if (pending.hold) await pending.wait;
  if (mode === "unavailable") return new Response(null, { status: 503 });
  if (mode === "unauthorized") return new Response(null, { status: 401 });
  if (mode === "reject_cancel") return new Response(new ReadableStream({ start(controller) { controller.enqueue(encoder.encode("rejected")); }, cancel() { return Promise.reject(new Error("synthetic rejected-body cancellation")); } }), { status: 401 });
  if (mode === "malformed") return new Response("{", { headers: { "content-type": "application/json" } });
  if (mode === "oversize") return new Response(JSON.stringify({ padding: "x".repeat(65_537) }), { headers: { "content-type": "application/json" } });
  return Response.json({ authenticated: true, session: { ...binding, subject: grant.subject, createdAt: grant.createdAt }, user: { id: mode === "substituted" ? "other" : grant.subject, handle: grant.fixtureCode === "utf16" ? "\udfff" : "current-fixture-handle", isAdmin: mode !== "demoted" && grant.fixtureCode !== "user", scopes: mode === "demoted" ? [] : ["ai", "current"] } });
}
