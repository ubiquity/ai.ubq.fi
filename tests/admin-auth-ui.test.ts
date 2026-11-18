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

class QuotaElement {
  dataset: Record<string, string> = {};
  hidden = false;
  textContent = "";
  children: QuotaElement[] = [];
  attributes = new Map<string, string>();
  style = { setProperty: () => {} };
  setAttribute(name: string, value: string) {
    this.attributes.set(name, value);
  }
  text(): string {
    return [this.textContent, ...this.children.map((child) => child.text())].join(" ").trim();
  }
  append(...children: QuotaElement[]) {
    this.children.push(...children);
  }
  appendChild(child: QuotaElement) {
    this.append(child);
  }
  replaceChildren() {
    this.children = [];
  }
}

test("the Analytics renderer distinguishes fractions, absolute amounts, unknowns, resets and stale observations", () => {
  const source = adminScript;
  const panel = {
    document: { createElement: () => new QuotaElement() },
    formatDate: (ms: number) => new Date(ms).toISOString(),
    quotaPercentFormatter: new Intl.NumberFormat("en-US"),
    setBadge: (badge: QuotaElement, state: string, text: string) => {
      badge.dataset.state = state;
      badge.textContent = text;
    },
    geminiQuotaCard: new QuotaElement(),
    geminiQuotaBadge: new QuotaElement(),
    geminiQuotaDetails: new QuotaElement(),
    geminiQuotaBuckets: new QuotaElement(),
    geminiQuotaStatus: new QuotaElement(),
  };
  const render = runInNewContext(
    source.slice(source.indexOf("const appendProviderFact"), source.indexOf("let geminiQuotaRequest")) + "\nrenderGeminiQuota",
    panel
  ) as (value: unknown) => void;
  render({
    status: "observed",
    observed_at_ms: Date.UTC(2026, 10, 18, 7),
    age_ms: 360_000,
    plan_tier: null,
    quota: { weekly: { remaining_fraction: 0.25, reset_in_seconds: 60 }, balance: { remaining_amount: 0 }, unknown: { disabled: true } },
  });
  const [weekly, balance, unknown] = panel.geminiQuotaBuckets.children;
  assert.equal(weekly.children[1].children[1].attributes.get("aria-valuenow"), "25");
  assert.match(weekly.text(), /25% remaining.*Reset 2026-11-18T07:01:00\.000Z · passed/);
  assert.match(balance.text(), /0 remaining.*Reset Not reported.*Unit Not reported/);
  assert.equal(balance.children[1].children.length, 1);
  assert.match(unknown.text(), /Not reported.*State Disabled/);
  assert.match(panel.geminiQuotaDetails.textContent, /Display only · Plan not reported · Observed 6 minutes ago/);
  assert.equal(panel.geminiQuotaBadge.textContent, "Stale observation");

  render({ status: "missing" });
  assert.equal(panel.geminiQuotaCard.hidden, true);
  assert.equal(panel.geminiQuotaBuckets.children.length, 0);
  assert.match(panel.geminiQuotaStatus.textContent, /No local Antigravity CLI snapshot/);
  render({ status: "signed_out", age_ms: 0 });
  assert.match(panel.geminiQuotaStatus.textContent, /no signed-in account/);
});
