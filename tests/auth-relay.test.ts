import assert from "node:assert/strict";

import { isAiGatewayPreviewOrigin, parseAuthRelayAction, parseTrustedAuthRelayOrigin } from "../static/auth-relay.js";

Deno.test("auth relay identifies only ai gateway preview origins", () => {
  assert.equal(isAiGatewayPreviewOrigin("https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net"), true);
  assert.equal(isAiGatewayPreviewOrigin("https://ai-ubq-fi-ejc9p6zmdjxt.deno.dev"), true);
  assert.equal(isAiGatewayPreviewOrigin("https://ai.ubq.fi"), false);
  assert.equal(isAiGatewayPreviewOrigin("http://ai-ubq-fi-cv5fc93pzb5a.deno.dev"), false);
  assert.equal(isAiGatewayPreviewOrigin("https://example.com"), false);
});

Deno.test("auth relay accepts local development origins", () => {
  assert.equal(parseTrustedAuthRelayOrigin("http://localhost:8000"), "http://localhost:8000");
  assert.equal(parseTrustedAuthRelayOrigin("http://127.0.0.1:8000"), "http://127.0.0.1:8000");
  assert.equal(parseTrustedAuthRelayOrigin("http://[::1]:8000"), "http://[::1]:8000");
});

Deno.test("auth relay accepts ai gateway production and preview origins", () => {
  assert.equal(parseTrustedAuthRelayOrigin("https://ai.ubq.fi"), "https://ai.ubq.fi");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi.deno.dev"), "https://ai-ubq-fi.deno.dev");
  assert.equal(
    parseTrustedAuthRelayOrigin("https://ai-ubq-fi.ubiquity-dao.deno.net"),
    "https://ai-ubq-fi.ubiquity-dao.deno.net",
  );
  assert.equal(
    parseTrustedAuthRelayOrigin("https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net"),
    "https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net",
  );
  assert.equal(
    parseTrustedAuthRelayOrigin("https://ai-ubq-fi-ejc9p6zmdjxt.deno.dev"),
    "https://ai-ubq-fi-ejc9p6zmdjxt.deno.dev",
  );
});

Deno.test("auth relay rejects untrusted or malformed origins", () => {
  assert.equal(parseTrustedAuthRelayOrigin("https://example.com"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi-evil.deno.dev"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi-evil.ubiquity-dao.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi-cv5fc93pzb5a.evil.example"), "");
  assert.equal(parseTrustedAuthRelayOrigin("http://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai.ubq.fi/admin"), "");
  assert.equal(parseTrustedAuthRelayOrigin("javascript:alert(1)"), "");
});

Deno.test("auth relay only accepts the passkey login action", () => {
  assert.equal(parseAuthRelayAction("passkey-login"), "passkey-login");
  assert.equal(parseAuthRelayAction("token"), "");
  assert.equal(parseAuthRelayAction(null), "");
});
