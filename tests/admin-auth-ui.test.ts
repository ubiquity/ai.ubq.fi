import { readFileSync } from "node:fs";
import { test } from "bun:test";
import assert from "node:assert/strict";
import { runInNewContext } from "node:vm";

const adminHtml = readFileSync(new URL("../static/admin.html", import.meta.url), "utf8");
const adminScript = readFileSync(new URL("../static/admin.js", import.meta.url), "utf8");

test("admin console exposes Passport as its only browser sign-in", () => {
  const gateStart = adminHtml.indexOf('id="admin-auth-gate"');
  const gate = adminHtml.slice(gateStart, adminHtml.indexOf('id="admin-loading-grid"', gateStart));
  assert.equal((gate.match(/<button\b/g) ?? []).length, 1, "the sign-in gate has one button");
  assert.match(gate, /id="admin-passport-login"/);
  assert.match(gate, /data-passport-signin/);
  assert.match(gate, /Sign in with Ubiquity/);
  assert.match(gate, /viewBox="0 0 96 96"/);
  assert.doesNotMatch(gate, /<circle cx="12" cy="8"/, "the generic user glyph is replaced by the Ubiquity mark");
  assert.doesNotMatch(gate, /admin-auth-gate-open|passkey|admin token/i);
  const compatStart = adminHtml.indexOf('id="admin-auth-compat"');
  const compat = adminHtml.slice(compatStart, adminHtml.indexOf("</div>", compatStart));
  assert.ok(compatStart > 0);
  assert.match(compat, /^id="admin-auth-compat" hidden aria-hidden="true"/, "non-Passport control nodes remain hidden plumbing only");
  assert.doesNotMatch(adminHtml, /<details data-passkey-advanced|<details data-token-fallback/);
});

test("opening the account panel never exposes a second Passport button", () => {
  const authGate = { hidden: false };
  const loadingGrid = { hidden: true };
  const loadingTitle = { textContent: "" };
  const body = { dataset: { authWidget: "collapsed" } };
  const fixture = {
    authGate,
    loadingGrid,
    loadingTitle,
    document: { body },
    adminAccessState: { checked: true, isAdmin: false },
    hasAdminCredential: () => false,
  };
  runInNewContext(
    adminScript.slice(adminScript.indexOf("const syncLoadingGate ="), adminScript.indexOf("const resetLoadingPrefetchStatuses =")) +
      "\nglobalThis.sync = syncLoadingGate;",
    fixture
  );
  const sync = (fixture as typeof fixture & { sync: () => void }).sync;
  sync();
  assert.equal(authGate.hidden, false, "the gate owns the single signed-out Passport action");
  body.dataset.authWidget = "expanded";
  sync();
  assert.equal(authGate.hidden, true, "opening the account panel removes the duplicate gate action");
  body.dataset.authWidget = "collapsed";
  sync();
  assert.equal(authGate.hidden, false);
});
