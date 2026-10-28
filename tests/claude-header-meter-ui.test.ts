import { test } from "bun:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";

const AT = 1_800_000_000_000;
const script = readFileSync(new URL("../static/admin.js", import.meta.url), "utf8");
const chart = runInNewContext(
  script.slice(script.indexOf("const CLAUDE_CAPACITY_CHART_SERIES ="), script.indexOf("const claudeHistoryAccountId =")) +
    "\n({series:claudeCapacityChartSeries,points:claudeCapacityChartPoints})",
  { CAPACITY_CHART_BUCKET_MS: 900_000, clampCapacityChartPercent: (value: number) => Math.min(100, Math.max(0, value)) }
) as {
  series: (value: unknown, account: string) => { window: string; label: string }[];
  points: (value: unknown, account: string, id: string, interval: unknown) => ({ remainingPercent: number; observedAt: number } | null)[];
};
const headerPoint = (offset: number, patch: Record<string, unknown> = {}) => ({
  source: "response_headers",
  account_id: "a",
  account_cohort_id: "cohort",
  credential_kind: "oauth",
  workspace_id: null,
  state: "available",
  sampled_at_ms: AT + offset,
  source_observed_at_ms: AT + offset,
  bucket_start_at_ms: AT,
  windows: { five_hour: { used_percent: 20, observed_at_ms: AT + offset, reset_at_ms: AT + 100 } },
  ...patch,
});
const interval = { startAtMs: AT, resetAtMs: AT + 100, durationMs: 100 };

test("shipped Claude chart excludes direct usage and uses explicit or legacy header evidence", () => {
  const oldDirect = { ...headerPoint(0), source: undefined, usage_snapshot: { meters: [{ id: "direct", used_percent: 99 }] } };
  const explicit = headerPoint(1, { usage_snapshot: oldDirect.usage_snapshot });
  const legacy = headerPoint(2, { source: undefined });
  const otherAccount = headerPoint(3, { account_id: "other" });
  const view = { points: [oldDirect, explicit, legacy, otherAccount], stale_after_ms: 1000 };
  const series = chart.series({ claude_history: view }, "a");
  assert.equal(series.length, 1);
  assert.equal(series[0]?.window, "five_hour");
  const points = chart.points(view, "a", "five_hour", interval).filter((point) => point !== null);
  assert.equal(points.length, 2);
  assert.equal(points[0]?.remainingPercent, 80);
  assert.equal(points[0]?.observedAt, AT + 1);
  assert.equal(points[1]?.observedAt, AT + 2);
  assert.equal(chart.series({ claude_history: { points: [oldDirect] } }, "a").length, 0);
});

test("shipped Claude header chart preserves missing, reset, cohort and staleness gaps", () => {
  const first = headerPoint(0);
  const missing = headerPoint(2, { windows: {} });
  const replaced = headerPoint(4, { account_cohort_id: "replacement" });
  const view = { points: [first, headerPoint(1), missing, headerPoint(3), replaced], stale_after_ms: 1000 };
  const points = chart.points(view, "a", "five_hour", interval);
  assert.equal(points.filter(Boolean).length, 4);
  assert.equal(points.filter((point) => point === null).length, 4);
  for (const patch of [
    { windows: { five_hour: { used_percent: 20, observed_at_ms: AT + 1, reset_at_ms: null } } },
    { credential_kind: "api_key" },
    { workspace_id: "replacement" },
  ]) {
    const changed = chart.points({ points: [first, headerPoint(1, patch)] }, "a", "five_hour", interval);
    assert.equal(changed.filter((point) => point === null).length, 2);
  }
  const stale = chart.points({ points: [first, headerPoint(2)], stale_after_ms: 1 }, "a", "five_hour", interval);
  assert.equal(stale.filter((point) => point === null).length, 2);
});

class MeterElement {
  dataset: Record<string, string> = {};
  attributes: Partial<Record<string, string>> = {};
  textContent = "";
  children: MeterElement[] = [];
  style = { setProperty() {} };
  constructor(readonly tag: string) {}
  setAttribute(name: string, value: string) {
    this.attributes[name] = value;
  }
  append(...children: MeterElement[]) {
    this.children.push(...children);
  }
  appendChild(child: MeterElement) {
    this.children.push(child);
  }
}
const elementText = (element: MeterElement): string => [element.textContent, ...element.children.map(elementText)].join(" ");
const descendants = (element: MeterElement): MeterElement[] => [element, ...element.children.flatMap(descendants)];
const historyContext = {
  CAPACITY_CHART_BUCKET_MS: 900_000,
  clampCapacityChartPercent: (value: number) => Math.min(100, Math.max(0, value)),
  latestClaudeSubscription: null as { active_account_id: string; accounts: { account_id: string; account_cohort_id: string }[] } | null,
  formatCapacityTimestamp: (value: number) => String(value),
  quotaPercentFormatter: new Intl.NumberFormat("en-US"),
  capacityChartPath: () => "",
  capacityChartSvgElement: (tag: string, attributes: Record<string, string | number> = {}) => {
    const element = new MeterElement(tag);
    for (const [name, value] of Object.entries(attributes)) element.setAttribute(name, String(value));
    return element;
  },
};
const history = runInNewContext(
  script.slice(script.indexOf("const CLAUDE_CAPACITY_CHART_SERIES ="), script.indexOf("const capacityChartFailureIsDowntime =")) +
    "\n({account:claudeHistoryAccountId,render:renderClaudeHistoryPaths,summary:claudeHistorySummary})",
  historyContext
) as {
  account: (snapshot: unknown) => string;
  render: (element: MeterElement, snapshot: unknown, interval: unknown, plot: unknown) => void;
  summary: (snapshot: unknown, now: number) => string;
};
const historyAccount = (id: string, cohort = "cohort") => ({ account_id: id, account_cohort_id: cohort });
const renderHistory = (snapshot: unknown) => {
  const svg = new MeterElement("svg");
  history.render(svg, snapshot, interval, { left: 0, top: 0, width: 100, height: 100 });
  return elementText(svg);
};

test("shipped Claude history follows each snapshot's active account without a selector or pinned manual choice", () => {
  const a = historyAccount("a");
  const b = historyAccount("b");
  historyContext.latestClaudeSubscription = { active_account_id: "b", accounts: [a, b] };
  const snapshot = {
    claude: a,
    claude_accounts: [a, b],
    claude_history: { scan: "ok", points: [headerPoint(0), headerPoint(1, { account_id: "b" })] },
  };
  const before = structuredClone(snapshot);
  assert.equal(history.account(snapshot), "a", "current capacity metadata wins over independently loaded subscription status");
  assert.match(renderHistory(snapshot), / · a · cohort/);
  assert.doesNotMatch(renderHistory(snapshot), / · b · cohort/);
  const switched = { ...snapshot, claude: b };
  assert.equal(history.account(switched), "b");
  assert.match(renderHistory(switched), / · b · cohort/);
  assert.doesNotMatch(renderHistory(switched), / · a · cohort/);
  assert.deepEqual(snapshot, before, "view rendering preserves all retained account history");
  assert.doesNotMatch(script, /Claude history account|capacityClaudeFilter|selectedClaudeHistoryAccountId|renderClaudeHistorySelector/);
});

test("shipped Claude history refuses stale or mismatched active metadata instead of borrowing retained history", () => {
  const a = historyAccount("a");
  const b = historyAccount("b");
  historyContext.latestClaudeSubscription = { active_account_id: "b", accounts: [a, b] };
  const retained = { scan: "ok", points: [headerPoint(0), headerPoint(1, { account_id: "b" })] };
  for (const snapshot of [
    { claude: null, claude_accounts: [a, b] },
    { claude: a, claude_accounts: [] },
    { claude: a, claude_accounts: [b] },
    { claude: historyAccount("a", "replacement"), claude_accounts: [a] },
  ]) {
    const current = { ...snapshot, claude_history: retained };
    assert.equal(history.account(current), "");
    assert.equal(renderHistory(current), "");
    assert.match(history.summary(current, AT), /waiting for actual upstream quota observations/);
  }
  assert.equal(history.account({ claude_accounts: [b] }), "b", "legacy snapshots may use matching verified subscription metadata");
  assert.equal(history.account({ claude_accounts: [historyAccount("b", "replacement")] }), "");
  historyContext.latestClaudeSubscription = { active_account_id: "retired", accounts: [a, b] };
  assert.equal(history.account({ claude_history: retained }), "");
  historyContext.latestClaudeSubscription = null;
  assert.equal(history.account({ claude_history: retained }), "");
});

const render = runInNewContext(
  script.slice(script.indexOf("const appendProviderFact ="), script.indexOf("const capacityAdditionalLimitLabel =")) +
    script.slice(script.indexOf("const claudeCapacityWindowForDisplay ="), script.indexOf("const claudeCapacitySourcesForDisplay =")) +
    "\nrenderClaudeCapacitySource",
  {
    document: { createElement: (tag: string) => new MeterElement(tag) },
    latestClaudeSubscription: null,
    latestProviderHealth: null,
    quotaPercentFormatter: new Intl.NumberFormat("en-US"),
    formatDate: (value: number) => String(value),
    formatWindowShort: (value: number) => String(value),
    setBadge: (element: MeterElement, state: string, label: string) => {
      element.dataset.state = state;
      element.textContent = label;
    },
    appendCapacitySourceMeta: () => {},
  }
) as (value: unknown) => MeterElement;

test("shipped Claude renderer uses header values and clocks despite old direct snapshots or errors", () => {
  const source = {
    source: "claude_code",
    state: "available",
    account_id: "a",
    source_observed_at_ms: AT,
    windows: { five_hour: { used_percent: 20, observed_at_ms: AT - 10, reset_at_ms: AT + 100 } },
    usage: { state: "scope_missing", message: "rejected-direct-read" },
    usage_stale: true,
    usage_snapshot: { meters: [{ label: "untrusted-direct-meter", used_percent: 99 }], observed_at_ms: AT + 1 },
  };
  const result = render(source);
  const text = elementText(result);
  assert.match(text, /Last observed/);
  assert.match(text, /80% remaining/);
  assert.match(text, new RegExp(`Observed ${AT - 10}`));
  assert.doesNotMatch(text, /Live|OAuth|scope|direct|Refresh usage/);
  assert.equal(descendants(result).filter((element) => element.tag === "button").length, 0);
  const bars = descendants(result).filter((element) => element.attributes.role === "progressbar");
  assert.equal(bars[0]?.attributes["aria-valuenow"], "80");
  assert.equal(bars[1]?.attributes["aria-valuenow"], undefined);
  assert.equal(elementText(render({ ...source, state: "stale" })).replace("Last observed · stale", "Last observed"), text);
});

test("shipped Claude renderer awaits headers without manufacturing subscription capacity for API keys", () => {
  const result = render({
    source: "claude_code",
    state: "available",
    credential_kind: "api_key",
    source_observed_at_ms: null,
    windows: { five_hour: null, seven_day: null },
    standard: [{ family: "requests", limit: 10, remaining: 8 }],
  });
  const text = elementText(result);
  assert.match(text, /Awaiting response headers/);
  assert.match(text, /Not reported/);
  assert.doesNotMatch(text, /remaining|Live|OAuth|Refresh/);
  assert.equal(descendants(result).filter((element) => element.attributes["aria-valuenow"] !== undefined).length, 0);
  assert.doesNotMatch(script, /claudeUsageReadLabel|refreshClaudeUsage|\/admin\/providers\/claude-code\/quota\/refresh|Reads Anthropic's OAuth usage endpoint/);
});
