import { afterEach, test } from "bun:test";
import assert from "node:assert/strict";
import { handlePassportRoute } from "../src/auth/passport.ts";
import { createFetchMock } from "./helpers/fetch-mock.ts";

const ORIGIN = "https://ai.ubq.fi";
const STATE_COOKIE = "__Host-uos_ai_passport_state";
const originalFetch = globalThis.fetch;

afterEach(() => {
  globalThis.fetch = originalFetch;
});

const callback = async (): Promise<Response> => {
  const login = await handlePassportRoute(new Request(`${ORIGIN}/auth/login`), "/auth/login");
  assert(login);
  const state = new URL(login.headers.get("Location") ?? "").searchParams.get("state");
  assert(state);
  const stateCookie = login.headers.getSetCookie()[0].split(";")[0];
  const response = await handlePassportRoute(
    new Request(`${ORIGIN}/auth/callback?state=${state}&code=fixture-code`, { headers: { Cookie: stateCookie } }),
    "/auth/callback"
  );
  assert(response);
  return response;
};

const assertFailure = async (response: Response, status: number, code: string): Promise<void> => {
  assert.deepEqual(response.headers.getSetCookie(), [`${STATE_COOKIE}=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax`]);
  assert.equal(response.headers.get("Cache-Control"), "no-store");
  assert.equal(response.status, status);
  assert.equal((await response.json()).error.code, code);
};

for (const [name, failure] of [
  ["network failures", new TypeError("fixture exchange connection failed")],
  ["timeouts", new DOMException("fixture exchange timed out", "TimeoutError")],
] as const) {
  test(`Bun Passport callback reports exchange ${name} as unavailable and clears state`, async () => {
    let exchanges = 0;
    globalThis.fetch = createFetchMock(async (input, init) => {
      assert.equal(input, "https://passport.ubq.fi/api/auth/sso/exchange");
      assert.equal(init?.method, "POST");
      assert(init?.signal instanceof AbortSignal);
      exchanges += 1;
      throw failure;
    });
    const response = await callback();
    assert.equal(exchanges, 1);
    await assertFailure(response, 502, "passport_unavailable");
  });
}

test("Bun Passport callback still rejects invalid exchanged credentials with 401 and clears state", async () => {
  let exchanges = 0;
  globalThis.fetch = createFetchMock(async (input) => {
    assert.equal(input, "https://passport.ubq.fi/api/auth/sso/exchange");
    exchanges += 1;
    return Response.json({ accessToken: "invalid-token" });
  });
  const response = await callback();
  assert.equal(exchanges, 1);
  await assertFailure(response, 401, "invalid_api_key");
});
