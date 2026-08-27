import assert from "node:assert/strict";
import {
  codexAuthMaintenanceArgs,
  codexAuthMaintenanceDue,
  maintainCodexAuthSlot,
  SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS,
  SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS,
} from "../scripts/sentinel/auth-maintenance.ts";
import type { CodexCommandRequest, CodexCommandResult } from "../scripts/sentinel/codex.ts";

const NOW = 1_800_000_000_000;

const base64Url = (value: string): string => btoa(value).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

const jwt = (expiresAtMs: number, label: string): string =>
  `${base64Url(JSON.stringify({ alg: "none" }))}.${
    base64Url(JSON.stringify({ exp: Math.floor(expiresAtMs / 1_000), label }))
  }.signature`;

const authJson = (
  label: string,
  account: string,
  expiresAtMs: number,
  refreshedAtMs: number,
): string =>
  JSON.stringify({
    auth_mode: "chatgpt",
    tokens: {
      id_token: jwt(expiresAtMs, `id-${label}`),
      access_token: jwt(expiresAtMs, `access-${label}`),
      refresh_token: `refresh-${label}`,
      account_id: account,
    },
    last_refresh: new Date(refreshedAtMs).toISOString(),
    preserved_unknown_field: { label },
  });

const result = (overrides: Partial<CodexCommandResult> = {}): CodexCommandResult => ({
  code: overrides.code ?? 0,
  stdout: overrides.stdout ?? "",
  stderr: overrides.stderr ?? "",
  outputExceeded: overrides.outputExceeded ?? false,
  timedOut: overrides.timedOut ?? false,
  stdoutBytes: overrides.stdoutBytes ?? 0,
  stderrBytes: overrides.stderrBytes ?? 0,
  durationMs: overrides.durationMs ?? 10,
});

Deno.test("auth maintenance due policy covers refresh age and access lifetime", () => {
  assert.equal(
    codexAuthMaintenanceDue({
      lastRefresh: new Date(NOW - SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS + 1).toISOString(),
      accessTokenExpiresAtMs: NOW + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS + 1,
    }, NOW),
    false,
  );
  assert.equal(
    codexAuthMaintenanceDue({
      lastRefresh: new Date(NOW - SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS).toISOString(),
      accessTokenExpiresAtMs: NOW + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS + 1,
    }, NOW),
    true,
  );
  assert.equal(
    codexAuthMaintenanceDue({
      lastRefresh: new Date(NOW).toISOString(),
      accessTokenExpiresAtMs: NOW + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS,
    }, NOW),
    true,
  );
});

Deno.test("auth maintenance CLI arguments isolate one fixed empty-workspace invocation", () => {
  const args = codexAuthMaintenanceArgs("/private/empty");
  assert.deepEqual(args.slice(0, 4), ["exec", "--ignore-rules", "--ephemeral", "--ignore-user-config"]);
  assert.ok(args.includes("--strict-config"));
  assert.ok(args.includes("--skip-git-repo-check"));
  assert.ok(args.includes("gpt-5.6-luna"));
  assert.ok(args.includes('model_reasoning_effort="low"'));
  assert.ok(args.includes('approval_policy="never"'));
  assert.deepEqual(args.slice(-3), ["--cd", "/private/empty", "-"]);
});

Deno.test("Codex-owned rotation preserves the complete rewritten document after a nonzero exit", async () => {
  const path = "/private/slot-1/auth.json";
  const before = authJson("before", "stable-account", NOW + 60_000, NOW - 7 * 24 * 60 * 60_000);
  const after = authJson("after", "stable-account", NOW + 10 * 24 * 60 * 60_000, NOW);
  const files = new Map([[path, before]]);
  let request: CodexCommandRequest | null = null;
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    now: () => NOW,
    readTextFile: (file) => Promise.resolve(files.get(file)!),
    runCommand: (candidate) => {
      request = candidate;
      files.set(path, after);
      return Promise.resolve(result({ code: 1, stderrBytes: 73 }));
    },
  });
  assert.ok(request);
  assert.equal((request as CodexCommandRequest).cwd, "/private/empty");
  assert.equal((request as CodexCommandRequest).env.CODEX_HOME, "/private/slot-1");
  assert.match((request as CodexCommandRequest).stdin, /^Reply with exactly OK/u);
  assert.equal(disposition.commandCode, 1);
  assert.equal(disposition.stateChanged, true);
  assert.equal(disposition.readyForMaintenanceWindow, true);
  assert.deepEqual(JSON.parse(files.get(path)!).preserved_unknown_field, { label: "after" });
});

Deno.test("auth maintenance skips a fresh ready file and rejects an account swap", async () => {
  const path = "/private/slot-1/auth.json";
  const fresh = authJson("fresh", "stable-account", NOW + 10 * 24 * 60 * 60_000, NOW);
  let calls = 0;
  const skipped = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    now: () => NOW,
    readTextFile: () => Promise.resolve(fresh),
    runCommand: () => {
      calls += 1;
      return Promise.resolve(result());
    },
  });
  assert.equal(skipped.invoked, false);
  assert.equal(calls, 0);

  const before = authJson("before", "stable-account", NOW + 60_000, NOW - 7 * 24 * 60 * 60_000);
  const swapped = authJson("after", "different-account", NOW + 10 * 24 * 60 * 60_000, NOW);
  const files = new Map([[path, before]]);
  await assert.rejects(
    () =>
      maintainCodexAuthSlot({
        slot: 1,
        slotDirectory: "/private/slot-1",
        workspace: "/private/empty",
      }, {
        now: () => NOW,
        readTextFile: (file) => Promise.resolve(files.get(file)!),
        runCommand: () => {
          files.set(path, swapped);
          return Promise.resolve(result());
        },
      }),
    /changed account identity/u,
  );
});
