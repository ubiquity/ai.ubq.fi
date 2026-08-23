import assert from "node:assert/strict";

import {
  isTrustedAuthRelayClientOrigin,
  parseAuthRelayAction,
  parseTrustedAuthRelayOrigin,
} from "../static/auth-relay.js";

Deno.test("auth relay identifies approved Deno relay client origins", () => {
  for (
    const origin of [
      "https://ai-ubq-fi.ubiquity-dao.deno.net",
      "https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net",
      "https://telegram-daily-exporter.0x4007.deno.net",
      "https://telegram-daily-exporter-4d2p9cx7m1ab.0x4007.deno.net",
      "https://agent-worker.ubiquity-os.deno.net",
      "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net",
      "https://ai-ubq-fi.deno.dev",
      "https://ai-ubq-fi-ejc9p6zmdjxt.deno.dev",
    ]
  ) {
    assert.equal(isTrustedAuthRelayClientOrigin(origin), true, origin);
  }
  assert.equal(isTrustedAuthRelayClientOrigin("https://ai.ubq.fi"), false);
  assert.equal(isTrustedAuthRelayClientOrigin("http://ai-ubq-fi-cv5fc93pzb5a.deno.dev"), false);
  assert.equal(isTrustedAuthRelayClientOrigin("https://example.com"), false);
});

Deno.test("auth relay rejects local development origins", () => {
  for (const hostname of ["localhost", "127.0.0.1", "[::1]"]) {
    assert.equal(parseTrustedAuthRelayOrigin(`http://${hostname}:8000`), "");
    assert.equal(parseTrustedAuthRelayOrigin(`https://${hostname}`), "");
  }
});

Deno.test("auth relay accepts canonical and owned Deno organization origins", () => {
  assert.equal(parseTrustedAuthRelayOrigin("https://ai.ubq.fi"), "https://ai.ubq.fi");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi.deno.dev"), "https://ai-ubq-fi.deno.dev");
  for (
    const origin of [
      "https://ai-ubq-fi.ubiquity-dao.deno.net",
      "https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net",
      "https://telegram-daily-exporter.0x4007.deno.net",
      "https://telegram-daily-exporter-4d2p9cx7m1ab.0x4007.deno.net",
      "https://agent-worker.ubiquity-os.deno.net",
      "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net",
      "https://ai-ubq-fi-ejc9p6zmdjxt.deno.dev",
    ]
  ) {
    assert.equal(parseTrustedAuthRelayOrigin(origin), origin);
  }
});

Deno.test("auth relay rejects untrusted or malformed origins", () => {
  assert.equal(parseTrustedAuthRelayOrigin("https://example.com"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://app.other-org.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://untrusted-app.0x4007.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://untrusted-app.ubiquity-os.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://checkout-ubq-fi.ubiquity-dao.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://app.0x4007.deno.net.evil.example"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://nested.app.0x4007.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://unowned-app.deno.dev"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi-evil.deno.dev"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai-ubq-fi-cv5fc93pzb5a.evil.example"), "");
  assert.equal(parseTrustedAuthRelayOrigin("http://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://app.0x4007.deno.net:8443"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://app.0x4007.deno.net/admin"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai.ubq.fi:8443"), "");
  assert.equal(parseTrustedAuthRelayOrigin("https://ai.ubq.fi/admin"), "");
  assert.equal(parseTrustedAuthRelayOrigin("javascript:alert(1)"), "");
});

Deno.test("auth relay only accepts the passkey login action", () => {
  assert.equal(parseAuthRelayAction("passkey-login"), "passkey-login");
  assert.equal(parseAuthRelayAction("token"), "");
  assert.equal(parseAuthRelayAction(null), "");
});
