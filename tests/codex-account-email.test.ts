import assert from "node:assert/strict";

import { getCodexAccountEmail } from "../src/codex.ts";

const encodeBase64Url = (value: unknown): string =>
  btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const tokenWithPayload = (payload: unknown): string =>
  `${encodeBase64Url({ alg: "none" })}.${encodeBase64Url(payload)}.signature`;

Deno.test("Codex account email is read from the provider profile claim", () => {
  const token = tokenWithPayload({
    "https://api.openai.com/profile": { email: "first@example.com" },
  });
  assert.equal(getCodexAccountEmail(token), "first@example.com");
});

Deno.test("Codex account email falls back to the top-level claim and rejects invalid tokens", () => {
  assert.equal(getCodexAccountEmail(tokenWithPayload({ email: "second@example.com" })), "second@example.com");
  assert.equal(getCodexAccountEmail("not-a-jwt"), null);
  assert.equal(getCodexAccountEmail(tokenWithPayload({ email: "not-an-email" })), null);
});
