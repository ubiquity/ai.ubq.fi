import assert from "node:assert/strict";
import {
  type CodexCommandRequest,
  type CodexCommandRuntime,
  CodexInvocationError,
  runCodexCommandWithRuntime,
  runStructuredCodexAgent,
} from "../scripts/sentinel/codex.ts";
import { runImplementationStageWithContinuation } from "../scripts/sentinel/main.ts";
import type { CodexAuthSlotSecrets } from "../scripts/sentinel/quota.ts";
import { isImplementationReport } from "../scripts/sentinel/types.ts";

const requiredPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run" }),
]);

const nowMs = 1_800_000_000_000;

const base64Url = (value: string): string => btoa(value).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

const jwt = (expiresAtMs: number, label: string): string =>
  `${base64Url(JSON.stringify({ alg: "none", typ: "JWT" }))}.${
    base64Url(JSON.stringify({ exp: Math.floor(expiresAtMs / 1_000), label }))
  }.signature-${label}`;

const encodeUtf8Base64 = (value: string): string => {
  const bytes = new TextEncoder().encode(value);
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
};

const authSlot = (slot: 1 | 2): string =>
  encodeUtf8Base64(JSON.stringify({
    auth_mode: "chatgpt",
    tokens: {
      id_token: jwt(nowMs + 4 * 60 * 60_000, `id-slot-${slot}`),
      access_token: jwt(nowMs + 4 * 60 * 60_000, `slot-${slot}`),
      refresh_token: `refresh-token-slot-${slot}-never-log`,
      account_id: `account-slot-${slot}-identifier`,
    },
    last_refresh: "2026-08-21T00:00:00Z",
  }));

const authSlots: CodexAuthSlotSecrets = {
  slot1B64: authSlot(1),
  slot2B64: authSlot(2),
};

const usageResponse = (): Response =>
  new Response(
    JSON.stringify({
      rate_limit: {
        primary_window: { used_percent: 10, limit_window_seconds: 10_800, reset_at: 1_900_000_000 },
        secondary_window: { used_percent: 20, limit_window_seconds: 604_800, reset_at: 1_900_000_000 },
      },
    }),
    { status: 200, headers: { "Content-Type": "application/json" } },
  );

const directProcessRuntime: CodexCommandRuntime = {
  createTimeoutSignal: (timeoutMs) => AbortSignal.timeout(timeoutMs),
  spawn(request, signal) {
    return new Deno.Command(request.executable, {
      args: [...request.args],
      cwd: request.cwd,
      env: { ...request.env },
      clearEnv: request.clearEnv,
      signal,
      stdin: "piped",
      stdout: "piped",
      stderr: "piped",
    }).spawn();
  },
  now: () => performance.now(),
};

const fakeCodexScript = `#!/bin/sh
set -eu
last_message=
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--output-last-message" ]; then
    shift
    [ "$#" -gt 0 ] || exit 31
    last_message=$1
  fi
  shift
done
[ -n "$last_message" ] || exit 32
prompt=$(/bin/cat)
printf '%s\n' "$CODEX_HOME" >> homes.txt
case "$prompt" in
  *"NEVER_FINISH"*)
    printf 'partial\n' > candidate.txt
    exec /bin/sleep 60
    ;;
esac
if [ ! -f attempt.marker ]; then
  printf 'started\n' > attempt.marker
  printf 'partial\n' > candidate.txt
  exec /bin/sleep 60
fi
case "$prompt" in
  *"The first bounded implementation invocation timed out."*) ;;
  *) exit 41 ;;
esac
[ "$(/bin/cat candidate.txt)" = "partial" ] || exit 42
printf 'observed\n' > continuation.prompt
printf 'partial\ncompleted\n' > candidate.txt
printf '%s' '{"schema_version":1,"dispositions":[{"finding_id":"fixture","status":"implemented","summary":"Completed after timeout.","changed_files":["candidate.txt"],"validation":["sandbox continuation"]}],"replay_acceptances":[],"candidate_sha":null,"summary":"Sandbox repair completed."}' > "$last_message"
printf '%s\n' '{"type":"result"}'
`;

Deno.test({
  name: "implementation subprocess timeout preserves candidate edits for one bounded continuation",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-implementation-process-" });
    const checkout = `${root}/checkout`;
    const fakeCodex = `${root}/fake-codex`;
    const schemaPath = `${checkout}/implementation.schema.json`;
    const homes: string[] = [];
    const timeouts: number[] = [];
    let relayClosures = 0;
    let timeoutCallbacks = 0;
    const attempts: number[] = [];
    try {
      await Deno.mkdir(checkout);
      await Deno.writeTextFile(schemaPath, "{}\n");
      await Deno.writeTextFile(fakeCodex, fakeCodexScript, { mode: 0o700 });

      const result = await runImplementationStageWithContinuation({
        basePrompt: "REPAIR_FIXTURE",
        initialTimeoutMs: 500,
        continuationTimeoutMs: 2_000,
        invoke: ({ attempt, prompt, timeoutMs }) => {
          attempts.push(attempt);
          return runStructuredCodexAgent({
            role: "implementation",
            checkoutPath: checkout,
            prompt,
            outputSchemaPath: schemaPath,
            authSlots,
            codexExecutable: fakeCodex,
            expectedMaximumRuntimeMs: timeoutMs,
          }, {
            fetcher: () => Promise.resolve(usageResponse()),
            now: () => nowMs,
            createTimeoutSignal: () => new AbortController().signal,
            readEnvironment: (name) => name === "PATH" ? "/usr/bin:/bin" : undefined,
            authRelayFactory: () =>
              Promise.resolve({
                baseUrl: "http://127.0.0.1:9/sentinel-test/backend-api",
                close() {
                  relayClosures++;
                  return Promise.resolve();
                },
              }),
            commandRunner: (request: CodexCommandRequest) => {
              homes.push(request.env.CODEX_HOME);
              timeouts.push(request.timeoutMs);
              return runCodexCommandWithRuntime(request, directProcessRuntime);
            },
          });
        },
        onTimeout: (error) => {
          assert.equal(error.failure, "invocation_timeout");
          timeoutCallbacks++;
          return Promise.resolve();
        },
      });

      assert.deepEqual(attempts, [1, 2]);
      assert.equal(timeoutCallbacks, 1);
      assert.equal(relayClosures, 2);
      assert.equal(new Set(homes).size, 2);
      assert.deepEqual(timeouts, [500, 2_000]);
      assert.equal(await Deno.readTextFile(`${checkout}/candidate.txt`), "partial\ncompleted\n");
      assert.equal(await Deno.readTextFile(`${checkout}/continuation.prompt`), "observed\n");
      assert.notEqual(result.lastMessage, null);
      assert.equal(isImplementationReport(JSON.parse(result.lastMessage!)), true);
      for (const home of homes) await assert.rejects(() => Deno.stat(home), Deno.errors.NotFound);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "implementation subprocess reproduces two bounded timeouts without an hour-long run",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-implementation-double-timeout-" });
    const checkout = `${root}/checkout`;
    const fakeCodex = `${root}/fake-codex`;
    const schemaPath = `${checkout}/implementation.schema.json`;
    const homes: string[] = [];
    const timeouts: number[] = [];
    const attempts: number[] = [];
    let relayClosures = 0;
    let timeoutCallbacks = 0;
    const startedAt = performance.now();
    try {
      await Deno.mkdir(checkout);
      await Deno.writeTextFile(schemaPath, "{}\n");
      await Deno.writeTextFile(fakeCodex, fakeCodexScript, { mode: 0o700 });

      await assert.rejects(
        () =>
          runImplementationStageWithContinuation({
            basePrompt: "NEVER_FINISH",
            initialTimeoutMs: 500,
            continuationTimeoutMs: 500,
            invoke: ({ attempt, prompt, timeoutMs }) => {
              attempts.push(attempt);
              return runStructuredCodexAgent({
                role: "implementation",
                checkoutPath: checkout,
                prompt,
                outputSchemaPath: schemaPath,
                authSlots,
                codexExecutable: fakeCodex,
                expectedMaximumRuntimeMs: timeoutMs,
              }, {
                fetcher: () => Promise.resolve(usageResponse()),
                now: () => nowMs,
                createTimeoutSignal: () => new AbortController().signal,
                readEnvironment: (name) => name === "PATH" ? "/usr/bin:/bin" : undefined,
                authRelayFactory: () =>
                  Promise.resolve({
                    baseUrl: "http://127.0.0.1:9/sentinel-test/backend-api",
                    close() {
                      relayClosures++;
                      return Promise.resolve();
                    },
                  }),
                commandRunner: (request: CodexCommandRequest) => {
                  homes.push(request.env.CODEX_HOME);
                  timeouts.push(request.timeoutMs);
                  return runCodexCommandWithRuntime(request, directProcessRuntime);
                },
              });
            },
            onTimeout: (error) => {
              assert.equal(error.failure, "invocation_timeout");
              timeoutCallbacks++;
              return Promise.resolve();
            },
          }),
        (error) => error instanceof CodexInvocationError && error.failure === "invocation_timeout",
      );

      assert.deepEqual(attempts, [1, 2]);
      assert.deepEqual(timeouts, [500, 500]);
      assert.equal(timeoutCallbacks, 1);
      assert.equal(relayClosures, 2);
      assert.equal(new Set(homes).size, 2);
      assert.ok(performance.now() - startedAt < 3_000);
      assert.equal(await Deno.readTextFile(`${checkout}/candidate.txt`), "partial\n");
      for (const home of homes) await assert.rejects(() => Deno.stat(home), Deno.errors.NotFound);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});
