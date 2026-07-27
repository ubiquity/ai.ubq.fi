import assert from "node:assert/strict";
import {
  analyzeStage0CacheTelemetryLines,
  STAGE0_AGGREGATE_MIN_COMPLETED,
  STAGE0_COHORT_MIN_COMPLETED,
  STAGE0_MIN_REPORTED_COVERAGE,
  Stage0CacheTelemetryGateError,
} from "../scripts/stage0-cache-telemetry-gate.ts";

const TERMINAL_MARKER = "[ai.ubq.fi] request_terminal";

let nextRequestId = 0;

const terminalLine = (overrides: Record<string, unknown> = {}): string =>
  `INFO ${TERMINAL_MARKER} ${
    JSON.stringify({
      request_id: `req-input-only-${nextRequestId++}`,
      route: "responses",
      status: 200,
      provider: "chatgpt_codex",
      model: "gpt-cache-fixture",
      cached_input_tokens: 0,
      cache_write_input_tokens: 0,
      usage_observed: true,
      usage_telemetry_status: "reported",
      stream_terminal_type: "response.completed",
      git_sha: "0123456789abcdef",
      deno_revision: "deploy-a",
      router_revision: null,
      ...overrides,
    })
  }`;

Deno.test("Stage 0 cache telemetry analyzer groups completed inference and preserves null cache values", () => {
  const report = analyzeStage0CacheTelemetryLines([
    "an unrelated log line",
    terminalLine({ cached_input_tokens: 0, cache_write_input_tokens: 0 }),
    terminalLine({ cached_input_tokens: null, cache_write_input_tokens: 20, usage_telemetry_status: "partial" }),
    // Cache reads and writes can overlap. The analyzer reports them separately
    // and intentionally never calculates an uncached remainder.
    terminalLine({ input_tokens: 100, cached_input_tokens: 80, cache_write_input_tokens: 80 }),
    terminalLine({
      route: "chat.completions",
      status: 201,
      provider: "yunwu",
      model: "gpt-chat-fixture",
      cached_input_tokens: 7,
      cache_write_input_tokens: null,
    }),
    terminalLine({ route: "embeddings", model: null, stream_terminal_type: "response.completed" }),
    terminalLine({ stream_terminal_type: "cancelled", model: null, status: 499 }),
  ]);

  assert.equal(report.terminal_events, 6);
  assert.deepEqual(report.status_totals, { "200": 4, "201": 1, "499": 1 });
  assert.equal(report.completed_inference, 4);
  assert.deepEqual(report.completed_status_totals, { "200": 3, "201": 1 });
  assert.deepEqual(report.usage_telemetry_status_totals, { partial: 1, reported: 3 });
  assert.deepEqual(report.cache_read_input_tokens, {
    sum_tokens: 87,
    observed_events: 3,
    null_events: 1,
    zero_events: 1,
    positive_events: 2,
  });
  assert.deepEqual(report.cache_write_input_tokens, {
    sum_tokens: 100,
    observed_events: 3,
    null_events: 1,
    zero_events: 1,
    positive_events: 2,
  });
  assert.deepEqual(report.reported_over_completed, { reported: 3, completed: 4, ratio: 0.75 });
  assert.equal(Object.hasOwn(report as object, "uncached_input_tokens"), false);
  assert.doesNotMatch(JSON.stringify(report), /"input_tokens":/);
  assert.equal(report.cohorts.length, 2);
  assert.equal(report.cohorts[0]?.completed_inference, 3);
  assert.equal(report.cohorts[1]?.route, "chat.completions");
  assert.equal(report.cohorts[1]?.completed_inference, 1);
  assert.equal(report.cohorts[0]?.completed_1k_gate.passed, false);
  assert.equal(report.cohorts[0]?.reported_coverage_99_5_gate.passed, false);
  assert.equal(report.cohorts[1]?.reported_coverage_99_5_gate.passed, true);
  assert.equal(report.gates.reported_coverage_99_5.all_observed_cohorts_passed, false);
});

Deno.test("Stage 0 cache telemetry analyzer applies aggregate, observed-cohort, and coverage thresholds", () => {
  const aggregateLines = Array.from(
    { length: STAGE0_AGGREGATE_MIN_COMPLETED },
    () => terminalLine({ model: "gpt-large-cohort" }),
  );
  const aggregateReport = analyzeStage0CacheTelemetryLines(aggregateLines);

  assert.equal(aggregateReport.gates.aggregate_completed_10k.passed, true);
  assert.equal(aggregateReport.gates.observed_cohort_completed_1k.all_observed_cohorts_passed, true);
  assert.equal(aggregateReport.gates.reported_coverage_99_5.observed_all_completed_passed, true);
  assert.equal(aggregateReport.gates.reported_coverage_99_5.all_observed_cohorts_passed, true);
  assert.equal(aggregateReport.gates.reported_coverage_99_5.minimum_ratio, STAGE0_MIN_REPORTED_COVERAGE);
  assert.equal(aggregateReport.gates.stage0_eligibility.status, "not_evaluated");
  assert.equal(aggregateReport.cohorts[0]?.completed_1k_gate.minimum_completed, STAGE0_COHORT_MIN_COMPLETED);

  const coverageReport = analyzeStage0CacheTelemetryLines([
    ...Array.from({ length: 199 }, () => terminalLine()),
    terminalLine({ cached_input_tokens: null, usage_telemetry_status: "partial" }),
  ]);
  assert.equal(coverageReport.reported_over_completed.ratio, 0.995);
  assert.equal(coverageReport.gates.reported_coverage_99_5.observed_all_completed_passed, true);

  const belowCoverageReport = analyzeStage0CacheTelemetryLines([
    ...Array.from({ length: 198 }, () => terminalLine()),
    terminalLine({ cached_input_tokens: null, usage_telemetry_status: "partial" }),
    terminalLine({ cached_input_tokens: null, usage_telemetry_status: "partial" }),
  ]);
  assert.equal(belowCoverageReport.gates.reported_coverage_99_5.observed_all_completed_passed, false);
});

Deno.test("Stage 0 cache telemetry analyzer fails closed without echoing request-level data", () => {
  const secret = "must-not-appear-in-errors-or-output";
  const malformed = `${TERMINAL_MARKER} {"request_id":"${secret}"`;
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([malformed]),
    (error: unknown) => {
      assert.ok(error instanceof Stage0CacheTelemetryGateError);
      assert.match(error.message, /malformed JSON/);
      assert.doesNotMatch(error.message, new RegExp(secret));
      return true;
    },
  );

  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine(), terminalLine({ deno_revision: "deploy-b" })]),
    /release identity differs/,
  );
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine({ git_sha: "unknown" })]),
    /missing release identity/,
  );
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine({ provider: null })]),
    /invalid provider field/,
  );
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine({ model: null })]),
    /invalid model field/,
  );
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine({ route: "response_typo" })]),
    /unsupported route field/,
  );
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine({ stream_terminal_type: "response.finished" })]),
    /unsupported stream_terminal_type field/,
  );
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([terminalLine({ usage_observed: false })]),
    /inconsistent usage_observed/,
  );

  const duplicateRequestId = `req-${secret}`;
  assert.throws(
    () =>
      analyzeStage0CacheTelemetryLines([
        terminalLine({ request_id: duplicateRequestId }),
        terminalLine({ request_id: duplicateRequestId }),
      ]),
    (error: unknown) => {
      assert.ok(error instanceof Stage0CacheTelemetryGateError);
      assert.match(error.message, /duplicate request_terminal event/);
      assert.doesNotMatch(error.message, new RegExp(secret));
      return true;
    },
  );

  const report = analyzeStage0CacheTelemetryLines([
    terminalLine({
      client_id: secret,
      key_id: secret,
      prompt: secret,
      prompt_cache_key: secret,
      request_id: secret,
    }),
  ]);
  assert.doesNotMatch(JSON.stringify(report), new RegExp(secret));
});

Deno.test("Stage 0 cache telemetry analyzer accepts strict JSON body log envelopes without exposing envelope data", () => {
  const secret = "envelope-secret";
  const report = analyzeStage0CacheTelemetryLines([
    JSON.stringify({
      body: terminalLine({ request_id: "req-envelope", client_id: secret, prompt_cache_key: secret }),
      request_id: secret,
      client_id: secret,
    }),
  ]);

  assert.equal(report.completed_inference, 1);
  assert.doesNotMatch(JSON.stringify(report), new RegExp(secret));
});

Deno.test("Stage 0 cache telemetry analyzer rejects an empty input stream", () => {
  assert.throws(
    () => analyzeStage0CacheTelemetryLines([]),
    /no request_terminal events/,
  );
});
