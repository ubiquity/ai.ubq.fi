import { execFile, spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { chmod, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { EFFORT, LIMITS, MODEL, record, startProbeProxy, type RequestSample } from "./proxy.ts";

const INITIAL = "Do not use tools. Output integers starting at 1 separated by spaces until the output limit. This is an isolated steering test.";
type NativeEvent = Record<string, unknown>;
type ProbeOptions = { endpoint: URL; token: string; executable: string; deadlineMs?: number };

const waitFor = async <T>(read: () => T | undefined, signal: AbortSignal): Promise<T> => {
  for (;;) {
    signal.throwIfAborted();
    const value = read();
    if (value !== undefined) return value;
    await Bun.sleep(10);
  }
};

const nativeClient = (executable: string, directory: string, signal: AbortSignal) => {
  const child: ChildProcessWithoutNullStreams = spawn(executable, ["app-server", "--stdio"], {
    cwd: directory,
    detached: true,
    env: { PATH: process.env.PATH, HOME: process.env.HOME, CODEX_HOME: directory, TMPDIR: directory },
    stdio: ["pipe", "pipe", "pipe"],
  });
  const events: NativeEvent[] = [];
  const replies = new Map<number, NativeEvent>();
  const arrivals = new WeakMap<NativeEvent, number>();
  let sequence = 0;
  let exited = false;
  let exitCode: number | null = null;
  let exitSignal: NodeJS.Signals | null = null;
  let stderr = "";
  child.stdin.on("error", () => {
    /* RPC waiting observes actual process close after pipe teardown. */
  });
  child.stderr.on("data", (chunk: Buffer) => {
    stderr = (stderr + chunk.toString()).slice(-32_768);
  });
  child.on("error", () => {
    exited = true;
  });
  child.on("close", (code, kind) => {
    exited = true;
    exitCode = code;
    exitSignal = kind;
  });
  const lines = createInterface({ input: child.stdout });
  lines.on("line", (line) => {
    try {
      const value = record(JSON.parse(line) as unknown);
      const arrived = performance.now();
      arrivals.set(value, arrived);
      arrivals.set(record(value.result), arrived);
      if (typeof value.id === "number") replies.set(value.id, value);
      else if (events.length < 1000) events.push(value);
    } catch {
      /* Non-JSON native diagnostics belong on stderr. */
    }
  });
  return {
    events,
    receivedAt: (value: NativeEvent) => arrivals.get(value),
    rpc: async (method: string, params: unknown) => {
      const id = ++sequence;
      child.stdin.write(JSON.stringify({ id, method, params }) + "\n");
      const reply = await waitFor(() => {
        if (exited && !replies.has(id)) throw new Error("Native app-server exited before its RPC reply");
        return replies.get(id);
      }, signal);
      replies.delete(id);
      if (reply.error) throw new Error(`Native RPC ${method} failed: ${JSON.stringify(reply.error)}`);
      return record(reply.result);
    },
    initialized: () => child.stdin.write('{"method":"initialized"}\n'),
    stop: async () => {
      child.stdin.end();
      for (const kind of ["SIGTERM", "SIGKILL"] as const) {
        for (let index = 0; index < 100 && !exited; index++) await Bun.sleep(10);
        if (exited) break;
        if (child.pid) {
          try {
            process.kill(-child.pid, kind);
          } catch {
            /* Already exited. */
          }
        }
      }
      for (let index = 0; index < 100 && !exited; index++) await Bun.sleep(10);
      lines.close();
      return { settled: exited, stderr, exitCode, exitSignal };
    },
  };
};

const readJson = async (url: URL, token: string, signal: AbortSignal): Promise<{ body: Record<string, unknown>; headers: Headers }> => {
  const response = await fetch(url, { headers: { authorization: `Bearer ${token}` }, redirect: "manual", signal });
  if (!response.ok) throw new Error(`Preflight HTTP ${response.status}`);
  return { body: record((await response.json()) as unknown), headers: response.headers };
};

const preflight = async (options: ProbeOptions, signal: AbortSignal, directory: string) => {
  const health = await readJson(new URL("/health", options.endpoint), options.token, signal);
  const release = record(health.body.release);
  if (
    typeof release.git_sha !== "string" ||
    typeof release.deployment_id !== "string" ||
    !release.deployment_id ||
    !/^[a-f0-9]{40}$/.test(release.git_sha) ||
    release.git_sha !== health.headers.get("x-uos-git-sha") ||
    release.deployment_id !== health.headers.get("x-uos-deployment-id")
  ) {
    throw new Error("Health body/header release identity mismatch");
  }
  const version = await new Promise<string>((resolve, reject) => {
    execFile(options.executable, ["--version"], { env: { PATH: process.env.PATH, HOME: process.env.HOME, CODEX_HOME: directory }, signal }, (error, stdout) => {
      if (error) reject(new Error("Installed Codex version unavailable"));
      else resolve(stdout);
    });
  });
  const clientVersion = /codex-cli (\d{1,3}\.\d{1,3}\.\d{1,3})/.exec(version)?.[1];
  if (!clientVersion) throw new Error("Installed Codex version unavailable");
  const catalog = await readJson(new URL(`/v1/models?client_version=${clientVersion}`, options.endpoint), options.token, signal);
  const models = Array.isArray(catalog.body.models) ? catalog.body.models : [];
  const model = models.map(record).find((item) => item.slug === MODEL);
  const tiers = model?.supported_reasoning_levels;
  if (!Array.isArray(tiers) || !tiers.some((tier: unknown) => tier === EFFORT || record(tier).effort === EFFORT)) {
    throw new Error("Selected model/effort absent from current native catalog");
  }
  return { release, clientVersion, model: MODEL, effort: EFFORT };
};

export const validateSteering = (samples: RequestSample[], threadId: string, turnId: string, answer: string, marker: string): string | null => {
  if (samples.length !== LIMITS.requests) return "Expected exactly two forwarded inference requests";
  if (samples[1].markerCount !== 1 || !samples[1].ordered) return "Steer missing, duplicated or reordered";
  if (!samples.every((sample) => sample.identity.thread_id === threadId && sample.identity.turn_id === turnId && sample.identity.session_id === threadId))
    return "Native thread/turn identity changed";
  if (!samples[0].cancelled) return "Original stream cancellation was not observed";
  if (answer.trim() !== marker) return "Steer did not change the final answer";
  return null;
};

const closeProbe = async (
  native: ReturnType<typeof nativeClient> | undefined,
  proxy: Awaited<ReturnType<typeof startProbeProxy>> | undefined,
  nativeDirectory: string
) => {
  let stopped: { settled: boolean; stderr: string; exitCode: number | null; exitSignal: NodeJS.Signals | null } = {
    settled: !native,
    stderr: "",
    exitCode: null,
    exitSignal: null,
  };
  try {
    if (native) stopped = await native.stop();
  } catch {
    stopped.stderr = "Native process cleanup failed";
  }
  let proxyClosed = true;
  try {
    if (proxy) await proxy.stop();
  } catch {
    proxyClosed = false;
  }
  let nativeStateRemoved = false;
  if (stopped.settled) {
    try {
      await rm(nativeDirectory, { recursive: true, force: true });
      nativeStateRemoved = true;
    } catch {
      /* The failed cleanup remains explicit in the receipt. */
    }
  }
  return {
    summary: { childSettled: stopped.settled, proxyClosed, nativeStateRemoved, nativeExitCode: stopped.exitCode, nativeExitSignal: stopped.exitSignal },
    stderr: stopped.stderr,
  };
};

export const runSteeringProbe = async (options: ProbeOptions) => {
  const directory = await mkdtemp(join(tmpdir(), "uos-steering-health-"));
  await chmod(directory, 0o700);
  const nativeDirectory = join(directory, "native-state");
  await mkdir(nativeDirectory, { mode: 0o700 });
  const signal = AbortSignal.timeout(options.deadlineMs ?? LIMITS.deadlineMs);
  const marker = `STEER_ACK_${crypto.randomUUID()}`;
  const started = performance.now();
  let proxy: Awaited<ReturnType<typeof startProbeProxy>> | undefined;
  let native: ReturnType<typeof nativeClient> | undefined;
  let receipt: Record<string, unknown> = { status: "failed", endpoint: options.endpoint.origin, artifacts: directory, limits: LIMITS };
  let cleanup: Awaited<ReturnType<typeof closeProbe>>;
  try {
    const observed = await preflight(options, signal, nativeDirectory);
    receipt = { ...receipt, ...observed };
    proxy = await startProbeProxy({
      ...options,
      marker,
      initial: INITIAL,
      signal,
      expectedRelease: { gitSha: String(observed.release.git_sha), deploymentId: String(observed.release.deployment_id) },
    });
    await writeFile(
      join(nativeDirectory, "config.toml"),
      `model = "${MODEL}"\nmodel_provider = "steering_health"\nmodel_reasoning_effort = "${EFFORT}"\ncli_auth_credentials_store = "file"\n[model_providers.steering_health]\nname = "Steering health probe"\nbase_url = "${proxy.url}"\nwire_api = "responses"\nrequires_openai_auth = false\nsupports_websockets = false\nrequest_max_retries = 0\nstream_max_retries = 0\n`
    );
    native = nativeClient(options.executable, nativeDirectory, signal);
    await native.rpc("initialize", { clientInfo: { name: "uos_steering_health", version: "1" }, capabilities: { experimentalApi: true } });
    native.initialized();
    const thread = await native.rpc("thread/start", {
      model: MODEL,
      modelProvider: "steering_health",
      cwd: nativeDirectory,
      approvalPolicy: "never",
      sandbox: "read-only",
    });
    const threadId = String(record(thread.thread).id);
    if (thread.model !== MODEL || thread.reasoningEffort !== EFFORT) throw new Error("Native model/effort readback mismatch");
    const turn = await native.rpc("turn/start", { threadId, input: [{ type: "text", text: INITIAL }] });
    const turnId = String(record(turn.turn).id);
    await waitFor(() => proxy?.samples[0], signal);
    const steerAt = performance.now();
    const ack = await native.rpc("turn/steer", {
      threadId,
      expectedTurnId: turnId,
      input: [{ type: "text", text: `Replace the previous request. Do not use tools. Reply with exactly ${marker}` }],
    });
    const ackAt = native.receivedAt(ack);
    if (ack.turnId !== turnId) throw new Error("Steer acknowledged a different turn");
    const terminal = await waitFor(
      () => native?.events.find((event) => event.method === "turn/completed" && record(record(event.params).turn).id === turnId),
      signal
    );
    const completedAt = native.receivedAt(terminal);
    if (ackAt === undefined || completedAt === undefined) throw new Error("Native event arrival timestamp unavailable");
    if (record(record(terminal.params).turn).status !== "completed")
      throw new Error(`Native turn failed: ${JSON.stringify(record(record(terminal.params).turn).error)}`);
    const state = await native.rpc("thread/read", { threadId, includeTurns: true });
    const turns = record(state.thread).turns;
    const items = Array.isArray(turns) ? record(turns.at(-1)).items : [];
    const messages = Array.isArray(items) ? items.map(record).filter((item) => item.type === "agentMessage") : [];
    await waitFor(() => proxy?.samples[0]?.closed, signal);
    const answer = messages.at(-1)?.text;
    const failure = proxy.failure() ?? validateSteering(proxy.samples, threadId, turnId, typeof answer === "string" ? answer : "", marker);
    if (failure) throw new Error(failure);
    const difference = (value: number | undefined, base: number) => (value === undefined ? null : Math.round((value - base) * 1000) / 1000);
    receipt = {
      ...receipt,
      status: "passed",
      ...observed,
      threadId,
      turnId,
      marker,
      requests: proxy.samples,
      metrics: {
        ackMs: difference(ackAt, steerAt),
        replacementMs: difference(proxy.samples[1].started, steerAt),
        headerMs: proxy.samples.map((sample) => difference(sample.headers, sample.started)),
        originalCancellationMs: difference(proxy.samples[0].closed, steerAt),
        completionMs: difference(completedAt, steerAt),
        totalMs: difference(completedAt, started),
      },
    };
  } catch (error) {
    receipt.error = error instanceof Error ? error.message : "Steering probe failed";
  } finally {
    cleanup = await closeProbe(native, proxy, nativeDirectory);
  }
  if (native) {
    await writeFile(join(directory, "native-events.json"), JSON.stringify(native.events).replaceAll(options.token, "[redacted]"));
    await writeFile(join(directory, "native-stderr.txt"), cleanup.stderr.replaceAll(options.token, "[redacted]"));
  }
  receipt.requests = proxy?.samples ?? [];
  receipt.proxyFailure = proxy?.failure() ?? null;
  receipt.cleanup = cleanup.summary;
  if (
    !cleanup.summary.childSettled ||
    !cleanup.summary.proxyClosed ||
    !cleanup.summary.nativeStateRemoved ||
    (native && cleanup.summary.nativeExitCode !== 0)
  ) {
    receipt.status = "failed";
    receipt.error ??= "Native app-server did not exit cleanly";
  }
  const sanitized = JSON.stringify(receipt).replaceAll(options.token, "[redacted]");
  await writeFile(join(directory, "receipt.json"), sanitized);
  return record(JSON.parse(sanitized) as unknown);
};
