import {
  CODEX_TOKEN_EXPIRY_SAFETY_MS,
  type CodexAccountSelection,
  type CodexAuthDocument,
  type CodexAuthSlot,
  type CodexAuthSlotSecrets,
  type CodexUsageProbe,
  type CodexUsageProbeDependencies,
  selectCodexAccountForInvocation,
} from "./quota.ts";

export const CODEX_COMMAND_OUTPUT_LIMIT_BYTES = 8 * 1_024 * 1_024;
export const CODEX_EXPECTED_INVOCATION_MS = 45 * 60_000;
export const CODEX_MAX_PROMPT_BYTES = 8 * 1_024 * 1_024;
// The pinned CLI defaults to five reconnects. A silent stream then consumes a
// whole 20-minute Sentinel invocation before the outer continuation can run.
export const SENTINEL_RELAY_STREAM_MAX_RETRIES = 1;
export const SENTINEL_RELAY_STREAM_IDLE_TIMEOUT_MS = 5 * 60_000;
const CODEX_ROLLOUT_AGGREGATE_LIMIT_BYTES = 64 * 1_024 * 1_024;
const CODEX_ROLLOUT_FILE_LIMIT_BYTES = CODEX_ROLLOUT_AGGREGATE_LIMIT_BYTES;
const CODEX_ROLLOUT_MAX_FILES = 4;
const CODEX_ROLLOUT_MAX_ENTRIES = 128;

export type SentinelAgentRole = "triage" | "implementation" | "monitoring";

export type SentinelAgentPolicy = Readonly<{
  model: "gpt-5.6-sol" | "gpt-5.6-luna";
  reasoningEffort: "medium" | "max";
  sandbox: "read-only" | "workspace-write";
}>;

export const SENTINEL_AGENT_POLICIES: Readonly<Record<SentinelAgentRole, SentinelAgentPolicy>> = Object.freeze({
  triage: Object.freeze({ model: "gpt-5.6-sol", reasoningEffort: "medium", sandbox: "read-only" }),
  implementation: Object.freeze({ model: "gpt-5.6-luna", reasoningEffort: "max", sandbox: "workspace-write" }),
  monitoring: Object.freeze({ model: "gpt-5.6-sol", reasoningEffort: "medium", sandbox: "read-only" }),
});

/** Guard the owner-controlled implementation policy at every cell boundary. */
export const assertSentinelImplementationPolicy = (): void => {
  const policy = SENTINEL_AGENT_POLICIES.implementation;
  if (
    policy.model !== "gpt-5.6-luna" || policy.reasoningEffort !== "max" ||
    policy.sandbox !== "workspace-write"
  ) {
    throw new Error("Provider Sentinel implementation must use gpt-5.6-luna at max with workspace-write sandbox");
  }
};

export type CodexCommandRequest = Readonly<{
  executable: string;
  args: readonly string[];
  cwd: string;
  repositoryRoot: string;
  workspaceWritable: boolean;
  env: Readonly<Record<string, string>>;
  clearEnv: true;
  stdin: string;
  outputLimitBytes: number;
  timeoutMs: number;
}>;

export type CodexCommandResult = Readonly<{
  code: number;
  stdout: string;
  stderr: string;
  outputExceeded: boolean;
  timedOut: boolean;
  stdoutBytes: number;
  stderrBytes: number;
  durationMs: number;
}>;

export type CodexCommandRunner = (request: CodexCommandRequest) => Promise<CodexCommandResult>;

export type CodexCommandChild = Readonly<{
  status: Promise<Readonly<{ code: number }>>;
  stdout: ReadableStream<Uint8Array>;
  stderr: ReadableStream<Uint8Array>;
  stdin: WritableStream<Uint8Array>;
}>;

export type CodexCommandRuntime = Readonly<{
  createTimeoutSignal(timeoutMs: number): AbortSignal;
  spawn(request: CodexCommandRequest, signal: AbortSignal): CodexCommandChild;
  now(): number;
}>;

export type CodexFilesystem = Readonly<{
  makePrivateTempDir(prefix: string): Promise<string>;
  chmod(path: string, mode: number): Promise<void>;
  writePrivateTextFile(path: string, contents: string): Promise<void>;
  readTextFile(path: string): Promise<string>;
  readPrivateRolloutFiles(codexHome: string): Promise<readonly string[]>;
  removeTree(path: string): Promise<void>;
}>;

export type CodexInvocationDependencies =
  & CodexUsageProbeDependencies
  & Readonly<{
    commandRunner?: CodexCommandRunner;
    filesystem?: CodexFilesystem;
    readEnvironment?: (name: string) => string | undefined;
    authRelayFactory?: CodexAuthRelayFactory;
  }>;

export type CodexAuthRelay = Readonly<{
  baseUrl: string;
  authenticationRejected?(): boolean;
  streamIdleTimedOut?(): boolean;
  close(): Promise<void>;
}>;

export type CodexAuthRelayFactory = (auth: CodexAuthDocument) => Promise<CodexAuthRelay>;

export type StructuredCodexAgentOptions = Readonly<{
  role: SentinelAgentRole;
  checkoutPath: string;
  prompt: string;
  outputSchemaPath: string;
  authSlots: CodexAuthSlotSecrets;
  codexExecutable?: string;
  expectedMaximumRuntimeMs?: number;
  outputLimitBytes?: number;
}>;

export type NativeCodexReviewOptions = Readonly<{
  checkoutPath: string;
  authSlots: CodexAuthSlotSecrets;
  codexExecutable?: string;
  expectedMaximumRuntimeMs?: number;
  outputLimitBytes?: number;
}>;

export type CodexInvocationResult = Readonly<{
  slot: CodexAuthSlot;
  headroomPercent: number;
  probes: readonly [CodexUsageProbe, CodexUsageProbe];
  stdout: string;
  stderr: string;
  lastMessage: string | null;
  nativeReviewOutput: unknown | null;
}>;

export type CodexInvocationFailureCode =
  | "accounts_unavailable"
  | "invalid_options"
  | "command_failed"
  | "invocation_timeout"
  | "output_limit_exceeded"
  | "secret_in_output"
  | "auth_refresh_required"
  | "auth_mutated"
  | "last_message_missing"
  | "native_review_missing"
  | "runtime_failure";

export class CodexInvocationError extends Error {
  readonly failure: CodexInvocationFailureCode;
  readonly slot: CodexAuthSlot | null;
  readonly exitCode: number | null;
  readonly probes: readonly [CodexUsageProbe, CodexUsageProbe] | null;
  readonly stdoutBytes: number | null;
  readonly stderrBytes: number | null;
  readonly durationMs: number | null;
  readonly outputExceeded: boolean | null;
  readonly timedOut: boolean | null;

  constructor(
    failure: CodexInvocationFailureCode,
    options: Readonly<{
      slot?: CodexAuthSlot;
      exitCode?: number;
      probes?: readonly [CodexUsageProbe, CodexUsageProbe];
      commandResult?: CodexCommandResult;
    }> = {},
  ) {
    super(`Codex invocation stopped (${failure}).`);
    this.name = "CodexInvocationError";
    this.failure = failure;
    this.slot = options.slot ?? null;
    this.exitCode = options.commandResult?.code ?? options.exitCode ?? null;
    this.probes = options.probes ?? null;
    this.stdoutBytes = options.commandResult?.stdoutBytes ?? null;
    this.stderrBytes = options.commandResult?.stderrBytes ?? null;
    this.durationMs = options.commandResult?.durationMs ?? null;
    this.outputExceeded = options.commandResult?.outputExceeded ?? null;
    this.timedOut = options.commandResult?.timedOut ?? null;
  }
}

const defaultFilesystem: CodexFilesystem = {
  async makePrivateTempDir(prefix) {
    const path = await Deno.makeTempDir({ prefix });
    await Deno.chmod(path, 0o700);
    return path;
  },
  chmod: (path, mode) => Deno.chmod(path, mode),
  writePrivateTextFile: (path, contents) => Deno.writeTextFile(path, contents, { mode: 0o600 }),
  readTextFile: (path) => Deno.readTextFile(path),
  async readPrivateRolloutFiles(codexHome) {
    const sessionsRoot = `${codexHome}/sessions`;
    const files: string[] = [];
    let observedEntries = 0;
    let observedBytes = 0;
    const visit = async (directory: string, depth: number): Promise<void> => {
      if (depth > 4) throw new Error("Codex rollout directory depth exceeded its bound");
      let entries: Deno.DirEntry[];
      try {
        entries = [];
        for await (const entry of Deno.readDir(directory)) entries.push(entry);
      } catch (error) {
        if (error instanceof Deno.errors.NotFound && directory === sessionsRoot) return;
        throw error;
      }
      entries.sort((left, right) => left.name.localeCompare(right.name));
      for (const entry of entries) {
        observedEntries += 1;
        if (observedEntries > CODEX_ROLLOUT_MAX_ENTRIES) {
          throw new Error("Codex rollout directory exceeded its entry bound");
        }
        if (entry.isSymlink) throw new Error("Codex rollout directory contains a symbolic link");
        const path = `${directory}/${entry.name}`;
        if (entry.isDirectory) {
          await visit(path, depth + 1);
          continue;
        }
        if (!entry.isFile || !/^rollout-[A-Za-z0-9:.+_-]+\.jsonl$/u.test(entry.name)) continue;
        if (files.length >= CODEX_ROLLOUT_MAX_FILES) {
          throw new Error("Codex rollout file count exceeded its bound");
        }
        const stat = await Deno.stat(path);
        if (!stat.isFile || stat.size > CODEX_ROLLOUT_FILE_LIMIT_BYTES) {
          throw new Error("Codex rollout file exceeded its size bound");
        }
        observedBytes += stat.size;
        if (observedBytes > CODEX_ROLLOUT_AGGREGATE_LIMIT_BYTES) {
          throw new Error("Codex rollout files exceeded their aggregate size bound");
        }
        files.push(await Deno.readTextFile(path));
      }
    };
    await visit(sessionsRoot, 0);
    return files;
  },
  async removeTree(path) {
    try {
      await Deno.remove(path, { recursive: true });
    } catch (error) {
      if (!(error instanceof Deno.errors.NotFound)) throw error;
    }
  },
};

const readCommandStream = async (
  stream: ReadableStream<Uint8Array>,
  shared: { retained: number; exceeded: boolean },
  limit: number,
): Promise<Readonly<{ text: string; byteLength: number }>> => {
  const reader = stream.getReader();
  const retained: Uint8Array[] = [];
  let observedBytes = 0;
  try {
    while (true) {
      const next = await reader.read();
      if (next.done) break;
      observedBytes += next.value.byteLength;
      const available = Math.max(0, limit + 1 - shared.retained);
      if (available > 0) {
        const piece = next.value.byteLength <= available ? next.value : next.value.subarray(0, available);
        retained.push(piece);
        shared.retained += piece.byteLength;
      }
      if (next.value.byteLength > available || shared.retained > limit) shared.exceeded = true;
    }
  } finally {
    reader.releaseLock();
  }
  const byteLength = retained.reduce((sum, chunk) => sum + chunk.byteLength, 0);
  const combined = new Uint8Array(byteLength);
  let offset = 0;
  for (const chunk of retained) {
    combined.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return Object.freeze({ text: new TextDecoder().decode(combined), byteLength: observedBytes });
};

export const codexBubblewrapArguments = (request: CodexCommandRequest): string[] => {
  const codexHome = request.env.CODEX_HOME;
  if (
    !codexHome?.startsWith("/") || !request.cwd.startsWith("/") || !request.repositoryRoot.startsWith("/") ||
    (request.cwd !== request.repositoryRoot && !request.cwd.startsWith(`${request.repositoryRoot}/`))
  ) {
    throw new Error("Provider Sentinel Codex isolation requires absolute private paths");
  }
  const gitControlPath = `${request.cwd}/.git`;
  const mountParents = new Set<string>();
  for (const path of [request.repositoryRoot, codexHome]) {
    const segments = path.split("/").filter(Boolean);
    let parent = "";
    for (const segment of segments.slice(0, -1)) {
      parent += `/${segment}`;
      if (parent !== "/usr" && parent !== "/etc" && parent !== "/opt" && parent !== "/tmp") {
        mountParents.add(parent);
      }
    }
  }
  const workspaceMount = request.workspaceWritable
    ? ["--bind", request.cwd, request.cwd, "--ro-bind", gitControlPath, gitControlPath]
    : [];
  return [
    "--die-with-parent",
    "--new-session",
    "--unshare-pid",
    "--unshare-ipc",
    "--unshare-uts",
    "--ro-bind",
    "/usr",
    "/usr",
    "--ro-bind",
    "/etc",
    "/etc",
    "--ro-bind-try",
    "/opt",
    "/opt",
    "--symlink",
    "usr/bin",
    "/bin",
    "--symlink",
    "usr/sbin",
    "/sbin",
    "--symlink",
    "usr/lib",
    "/lib",
    "--symlink",
    "usr/lib64",
    "/lib64",
    "--dev",
    "/dev",
    "--proc",
    "/proc",
    "--tmpfs",
    "/tmp",
    ...[...mountParents].sort((left, right) => left.length - right.length).flatMap((path) => ["--dir", path]),
    "--ro-bind",
    request.repositoryRoot,
    request.repositoryRoot,
    ...workspaceMount,
    "--bind",
    codexHome,
    codexHome,
    "--chdir",
    request.cwd,
    "--",
    request.executable,
    ...request.args,
  ];
};

const defaultCodexCommandRuntime: CodexCommandRuntime = {
  createTimeoutSignal: (timeoutMs) => AbortSignal.timeout(timeoutMs),
  spawn(request, signal) {
    if (Deno.build.os !== "linux") {
      throw new Error("Provider Sentinel Codex isolation requires Linux bubblewrap");
    }
    return new Deno.Command("bwrap", {
      args: codexBubblewrapArguments(request),
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

/** Captures output privately. It never inherits stdout, stderr, stdin, or environment. */
export const runCodexCommandWithRuntime = async (
  request: CodexCommandRequest,
  runtime: CodexCommandRuntime,
): Promise<CodexCommandResult> => {
  const signal = runtime.createTimeoutSignal(request.timeoutMs);
  let timeoutObserved = signal.aborted;
  const observeTimeout = () => {
    timeoutObserved = true;
  };
  signal.addEventListener("abort", observeTimeout, { once: true });
  const startedAt = runtime.now();
  let child: CodexCommandChild;
  try {
    child = runtime.spawn(request, signal);
  } catch (error) {
    signal.removeEventListener("abort", observeTimeout);
    throw error;
  }
  const statusPromise = child.status;
  const outputState = { retained: 0, exceeded: false };
  const stdoutPromise = readCommandStream(child.stdout, outputState, request.outputLimitBytes);
  const stderrPromise = readCommandStream(child.stderr, outputState, request.outputLimitBytes);
  const writer = child.stdin.getWriter();
  const inputPromise = (async () => {
    try {
      await writer.write(new TextEncoder().encode(request.stdin));
    } finally {
      await writer.close();
    }
  })();
  try {
    const [status, stdout, stderr] = await Promise.all([statusPromise, stdoutPromise, stderrPromise, inputPromise])
      .then(([status, stdout, stderr]) => [status, stdout, stderr] as const);
    return {
      code: status.code,
      stdout: stdout.text,
      stderr: stderr.text,
      outputExceeded: outputState.exceeded,
      timedOut: timeoutObserved || signal.aborted,
      stdoutBytes: stdout.byteLength,
      stderrBytes: stderr.byteLength,
      durationMs: Math.max(0, Math.round(runtime.now() - startedAt)),
    };
  } catch (error) {
    const [status, stdout, stderr] = await Promise.allSettled([statusPromise, stdoutPromise, stderrPromise]);
    await Promise.allSettled([inputPromise]);
    if (timeoutObserved || signal.aborted) {
      return {
        code: status.status === "fulfilled" ? status.value.code : -1,
        stdout: stdout.status === "fulfilled" ? stdout.value.text : "",
        stderr: stderr.status === "fulfilled" ? stderr.value.text : "",
        outputExceeded: outputState.exceeded,
        timedOut: true,
        stdoutBytes: stdout.status === "fulfilled" ? stdout.value.byteLength : 0,
        stderrBytes: stderr.status === "fulfilled" ? stderr.value.byteLength : 0,
        durationMs: Math.max(0, Math.round(runtime.now() - startedAt)),
      };
    }
    throw error;
  } finally {
    signal.removeEventListener("abort", observeTimeout);
  }
};

export const runCodexCommand: CodexCommandRunner = (request) =>
  runCodexCommandWithRuntime(request, defaultCodexCommandRuntime);

const absolutePath = (value: string): string => {
  if (!value.startsWith("/") || value.includes("\0") || !value.trim()) {
    throw new CodexInvocationError("invalid_options");
  }
  return value;
};

const positiveInteger = (value: number, label: string): number => {
  if (!Number.isSafeInteger(value) || value <= 0) throw new TypeError(`${label} must be a positive integer.`);
  return value;
};

const selectedAuth = (
  selection: CodexAccountSelection,
): Extract<CodexAccountSelection, { kind: "selected" }> => {
  if (selection.kind === "unavailable") {
    throw new CodexInvocationError("accounts_unavailable", { probes: selection.probes });
  }
  return selection;
};

const minimalEnvironment = (
  codexHome: string,
  readEnvironment: ((name: string) => string | undefined) | undefined,
): Readonly<Record<string, string>> => {
  const read = readEnvironment ?? ((name: string) => Deno.env.get(name));
  const path = read("PATH");
  return Object.freeze({
    CODEX_HOME: codexHome,
    HOME: codexHome,
    PATH: path && path.startsWith("/") ? path : "/usr/local/bin:/usr/bin:/bin",
    CI: "true",
    LANG: "C.UTF-8",
    LC_ALL: "C.UTF-8",
    NO_COLOR: "1",
    TERM: "dumb",
  });
};

const secretValues = (auth: CodexAuthDocument): readonly string[] => {
  const values = [
    auth.rawJson,
    auth.encoded,
    auth.tokens.id_token,
    auth.tokens.access_token,
    auth.tokens.refresh_token,
  ];
  if (auth.tokens.account_id.length >= 8) values.push(auth.tokens.account_id);
  return values.filter((value, index, all) => value.length > 0 && all.indexOf(value) === index);
};

const base64Url = (value: string): string => btoa(value).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

export const syntheticCodexAuthJson = (
  auth: CodexAuthDocument,
  nowMs = Date.now(),
): string => {
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new TypeError("Synthetic auth clock is invalid");
  const expiresAtSeconds = Math.floor(auth.accessTokenExpiresAtMs / 1_000);
  const accessToken = `${base64Url(JSON.stringify({ alg: "none", typ: "JWT" }))}.${
    base64Url(JSON.stringify({
      exp: expiresAtSeconds,
      "https://api.openai.com/auth": {
        chatgpt_account_id: "sentinel-relay-account",
        chatgpt_plan_type: "sentinel",
      },
    }))
  }.sentinel`;
  return JSON.stringify({
    auth_mode: "chatgpt",
    tokens: {
      id_token: accessToken,
      access_token: accessToken,
      refresh_token: "sentinel-relay-refresh-token",
      account_id: "sentinel-relay-account",
    },
    // A stale timestamp makes Codex attempt to rotate the deliberately fake
    // refresh token. Real auth rotation is owned by the workflow's isolated
    // credential-maintenance lane; agent processes only receive this freshly
    // timestamped synthetic relay credential.
    last_refresh: new Date(nowMs).toISOString(),
  });
};

const RELAY_REQUEST_HEADER_DENYLIST = Object.freeze([
  "authorization",
  "chatgpt-account-id",
  "cookie",
  "host",
  "openai-organization",
  "openai-project",
  "proxy-authorization",
  "x-api-key",
]);

const relaySseEventActivity = (onEvent: () => void): (chunk: Uint8Array) => void => {
  const decoder = new TextDecoder();
  let atStreamStart = true;
  let previousWasCarriageReturn = false;
  let lineHasContent = false;
  let linePrefix = "";
  let eventHasData = false;
  const finishLine = () => {
    if (!lineHasContent) {
      if (eventHasData) onEvent();
      eventHasData = false;
    } else if (linePrefix === "data" || linePrefix === "data:") {
      eventHasData = true;
    }
    lineHasContent = false;
    linePrefix = "";
  };
  return (chunk) => {
    for (const character of decoder.decode(chunk, { stream: true })) {
      if (previousWasCarriageReturn) {
        previousWasCarriageReturn = false;
        if (character === "\n") continue;
      }
      if (character === "\r") {
        finishLine();
        previousWasCarriageReturn = true;
        continue;
      }
      if (character === "\n") {
        finishLine();
        continue;
      }
      if (atStreamStart && character === "\uFEFF") continue;
      atStreamStart = false;
      lineHasContent = true;
      if (linePrefix.length < 5) linePrefix += character;
    }
  };
};

const relayStreamWithIdleTimeout = (
  body: ReadableStream<Uint8Array>,
  requestSignal: AbortSignal,
  timeoutMs: number,
  onIdleTimeout: () => void,
): ReadableStream<Uint8Array> => {
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  let timeout: ReturnType<typeof setTimeout> | null = null;
  let closed = false;
  let idleTimedOut = false;
  let observeSseEvent: ((chunk: Uint8Array) => void) | null = null;
  const stopTimer = () => {
    if (timeout === null) return;
    clearTimeout(timeout);
    timeout = null;
  };
  const releaseReader = (activeReader: ReadableStreamDefaultReader<Uint8Array>) => {
    if (reader === activeReader) reader = null;
    try {
      activeReader.releaseLock();
    } catch {
      // The reader can already be released by a concurrent cancellation.
    }
  };
  const cancelReader = (reason: unknown) => {
    const activeReader = reader;
    if (activeReader === null) return;
    let cancellation: Promise<void>;
    try {
      cancellation = activeReader.cancel(reason);
    } catch {
      releaseReader(activeReader);
      return;
    }
    releaseReader(activeReader);
    void cancellation.catch(() => undefined).finally(() => releaseReader(activeReader));
  };
  const close = () => {
    closed = true;
    stopTimer();
    requestSignal.removeEventListener("abort", onRequestAbort);
  };
  const onRequestAbort = () => {
    if (closed) return;
    close();
    cancelReader(requestSignal.reason);
  };
  const resetTimer = () => {
    stopTimer();
    timeout = setTimeout(() => {
      timeout = null;
      if (closed || idleTimedOut) return;
      idleTimedOut = true;
      onIdleTimeout();
      cancelReader(new DOMException("Sentinel relay stream idle timeout", "TimeoutError"));
    }, timeoutMs);
  };
  return new ReadableStream<Uint8Array>({
    start() {
      reader = body.getReader();
      requestSignal.addEventListener("abort", onRequestAbort, { once: true });
      if (requestSignal.aborted) {
        onRequestAbort();
        return;
      }
      resetTimer();
      observeSseEvent = relaySseEventActivity(resetTimer);
    },
    async pull(controller) {
      if (closed) return;
      const activeReader = reader;
      if (activeReader === null) {
        if (idleTimedOut) {
          close();
          controller.close();
        }
        return;
      }
      try {
        const next = await activeReader.read();
        if (closed) {
          releaseReader(activeReader);
          return;
        }
        if (idleTimedOut || next.done) {
          close();
          releaseReader(activeReader);
          controller.close();
          return;
        }
        observeSseEvent?.(next.value);
        controller.enqueue(next.value);
      } catch (error) {
        if (closed) {
          releaseReader(activeReader);
          return;
        }
        close();
        releaseReader(activeReader);
        if (idleTimedOut) controller.close();
        else controller.error(error);
      }
    },
    cancel(reason) {
      if (closed) return;
      close();
      cancelReader(reason);
    },
  });
};

export const createCodexAuthRelayFactory = (
  upstreamFetch: (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>,
  options: Readonly<{ streamIdleTimeoutMs?: number }> = {},
): CodexAuthRelayFactory =>
async (auth) => {
  const relayPrefix = `/sentinel-${crypto.randomUUID()}/backend-api`;
  const streamIdleTimeoutMs = positiveInteger(
    options.streamIdleTimeoutMs ?? SENTINEL_RELAY_STREAM_IDLE_TIMEOUT_MS,
    "streamIdleTimeoutMs",
  );
  let authenticationRejected = false;
  let lastStreamSequence = 0;
  let lastStreamIdleTimedOut = false;
  const server = Deno.serve({ hostname: "127.0.0.1", port: 0, onListen: () => undefined }, async (request) => {
    const requestUrl = new URL(request.url);
    if (
      !requestUrl.pathname.startsWith(`${relayPrefix}/codex/`) ||
      (request.method !== "GET" && request.method !== "POST")
    ) {
      return new Response("Not found", { status: 404 });
    }
    const upstreamPath = requestUrl.pathname.slice(relayPrefix.length);
    const target = new URL(`https://chatgpt.com/backend-api${upstreamPath}${requestUrl.search}`);
    const headers = new Headers(request.headers);
    for (const name of RELAY_REQUEST_HEADER_DENYLIST) headers.delete(name);
    headers.set("Authorization", `Bearer ${auth.tokens.access_token}`);
    headers.set("ChatGPT-Account-ID", auth.tokens.account_id);
    headers.set("Cache-Control", "no-store");
    let upstream: Response;
    try {
      upstream = await upstreamFetch(target, {
        method: request.method,
        headers,
        body: request.method === "GET" ? undefined : request.body,
        redirect: "manual",
        signal: request.signal,
      });
    } catch {
      return new Response("Authentication relay upstream failure", { status: 502 });
    }
    if (upstream.status >= 300 && upstream.status < 400) {
      upstream.body?.cancel().catch(() => undefined);
      return new Response("Authentication relay rejected a redirect", { status: 502 });
    }
    if (upstream.status === 401) {
      authenticationRejected = true;
      upstream.body?.cancel().catch(() => undefined);
      return new Response("Sentinel credential refresh is required", {
        status: 401,
        headers: { "Cache-Control": "no-store" },
      });
    }
    const responseHeaders = new Headers(upstream.headers);
    responseHeaders.delete("set-cookie");
    responseHeaders.delete("location");
    responseHeaders.set("Cache-Control", "no-store");
    const tracksResponseStream = request.method === "POST" &&
      requestUrl.pathname.endsWith("/responses") &&
      upstream.status >= 200 &&
      upstream.status < 300 &&
      upstream.body !== null;
    const streamSequence = tracksResponseStream ? ++lastStreamSequence : null;
    if (streamSequence !== null) lastStreamIdleTimedOut = false;
    const body = streamSequence === null || upstream.body === null ? upstream.body : relayStreamWithIdleTimeout(
      upstream.body,
      request.signal,
      streamIdleTimeoutMs,
      () => {
        if (streamSequence === lastStreamSequence) lastStreamIdleTimedOut = true;
      },
    );
    return new Response(body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers: responseHeaders,
    });
  });
  const address = server.addr;
  if (address.transport !== "tcp" || address.hostname !== "127.0.0.1") {
    await server.shutdown();
    throw new Error("Codex authentication relay did not bind to loopback");
  }
  return Object.freeze({
    baseUrl: `http://127.0.0.1:${address.port}${relayPrefix}`,
    authenticationRejected: () => authenticationRejected,
    streamIdleTimedOut: () => lastStreamIdleTimedOut,
    async close() {
      await server.shutdown();
    },
  });
};

export const startCodexAuthRelay = createCodexAuthRelayFactory(fetch);

export const outputContainsCodexSecret = (auth: CodexAuthDocument, ...outputs: readonly string[]): boolean => {
  const combined = outputs.join("\n");
  return secretValues(auth).some((secret) => combined.includes(secret));
};

const sentinelRelayConfigArgs = (relayBaseUrl: string): readonly string[] => [
  "-c",
  'model_provider="sentinel_relay"',
  "-c",
  'model_providers.sentinel_relay.name="Sentinel relay"',
  "-c",
  `model_providers.sentinel_relay.base_url=${JSON.stringify(`${relayBaseUrl}/codex`)}`,
  "-c",
  'model_providers.sentinel_relay.wire_api="responses"',
  "-c",
  `model_providers.sentinel_relay.stream_max_retries=${SENTINEL_RELAY_STREAM_MAX_RETRIES}`,
  "-c",
  `model_providers.sentinel_relay.stream_idle_timeout_ms=${SENTINEL_RELAY_STREAM_IDLE_TIMEOUT_MS}`,
  "-c",
  "model_providers.sentinel_relay.requires_openai_auth=true",
  "-c",
  "model_providers.sentinel_relay.supports_websockets=false",
  "-c",
  "model_providers.sentinel_relay.supports_standalone_web_search=false",
  "-c",
  `chatgpt_base_url=${JSON.stringify(relayBaseUrl)}`,
  "-c",
  "features.apps=false",
];

const execConfigArgs = (
  checkoutPath: string,
  policy: SentinelAgentPolicy,
  outputSchemaPath: string,
  lastMessagePath: string,
  relayBaseUrl: string,
): readonly string[] => [
  "exec",
  "--ignore-rules",
  "--ephemeral",
  "--ignore-user-config",
  "--strict-config",
  "--json",
  "--output-last-message",
  lastMessagePath,
  "--output-schema",
  outputSchemaPath,
  "-m",
  policy.model,
  "-c",
  `model_reasoning_effort=\"${policy.reasoningEffort}\"`,
  "-s",
  policy.sandbox,
  "-c",
  'approval_policy="never"',
  "-c",
  'web_search="disabled"',
  "-c",
  "sandbox_workspace_write.network_access=false",
  "-c",
  'shell_environment_policy.inherit="none"',
  "-c",
  "agents.enabled=false",
  ...sentinelRelayConfigArgs(relayBaseUrl),
  "--cd",
  checkoutPath,
  "-",
];

const nativeReviewArgs = (checkoutPath: string, relayBaseUrl: string): readonly string[] => [
  "--cd",
  checkoutPath,
  "--sandbox",
  "read-only",
  "--ask-for-approval",
  "never",
  "-c",
  'web_search="disabled"',
  "-c",
  "sandbox_workspace_write.network_access=false",
  "-c",
  'shell_environment_policy.inherit="none"',
  "-c",
  "agents.enabled=false",
  ...sentinelRelayConfigArgs(relayBaseUrl),
  "review",
  "--strict-config",
  "--base",
  "origin/development",
];

const COMPATIBILITY_CHECK_RELAY = "http://127.0.0.1:9/sentinel-compatibility/backend-api";

export const codexExecCliCompatibilityArgs = (checkoutPath: string): readonly string[] => {
  const args = execConfigArgs(
    checkoutPath,
    SENTINEL_AGENT_POLICIES.triage,
    "/tmp/sentinel-output-schema.json",
    "/tmp/sentinel-last-message.json",
    COMPATIBILITY_CHECK_RELAY,
  );
  return [...args.slice(0, -1), "--help"];
};

export const codexReviewCliCompatibilityArgs = (checkoutPath: string): readonly string[] => [
  ...nativeReviewArgs(checkoutPath, COMPATIBILITY_CHECK_RELAY),
  "--help",
];

type PrivateInvocationOptions = Readonly<{
  checkoutPath: string;
  authSlots: CodexAuthSlotSecrets;
  codexExecutable: string;
  expectedMaximumRuntimeMs: number;
  outputLimitBytes: number;
  prompt: string;
  quotaModel: string | null;
  /**
   * Absolute source of the structured-output schema. The exact source text is
   * copied into the private Codex home before spawning because bubblewrap never
   * mounts an external runner temp directory.
   */
  outputSchemaSourcePath?: string;
  args: (
    lastMessagePath: string,
    relayBaseUrl: string,
    stagedOutputSchemaPath: string | null,
  ) => readonly string[];
  requireLastMessage: boolean;
  requireNativeReviewOutput: boolean;
  workspaceWritable: boolean;
}>;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

export const extractNativeReviewOutputFromRollouts = (rollouts: readonly string[]): unknown => {
  const outputs: unknown[] = [];
  for (const rollout of rollouts) {
    for (const line of rollout.split("\n")) {
      if (!line.trim()) continue;
      let record: unknown;
      try {
        record = JSON.parse(line);
      } catch {
        throw new Error("Codex rollout contained invalid JSONL");
      }
      if (!isRecord(record) || record.type !== "event_msg" || !isRecord(record.payload)) continue;
      if (
        record.payload.type === "item_completed" && isRecord(record.payload.item) &&
        record.payload.item.type === "ExitedReviewMode"
      ) {
        outputs.push(record.payload.item.review_output);
      }
    }
  }
  if (outputs.length !== 1 || outputs[0] === null || outputs[0] === undefined) {
    throw new Error("Codex rollout did not contain exactly one completed native review output");
  }
  return outputs[0];
};

const repositoryRootForCheckout = (checkoutPath: string): string => {
  const current = Deno.cwd();
  if (checkoutPath === current || checkoutPath.startsWith(`${current}/`)) return current;
  return checkoutPath;
};

const invokePrivately = async (
  options: PrivateInvocationOptions,
  dependencies: CodexInvocationDependencies,
): Promise<CodexInvocationResult> => {
  const selection = selectedAuth(
    await selectCodexAccountForInvocation({
      slots: options.authSlots,
      model: options.quotaModel,
      minimumValidityMs: options.expectedMaximumRuntimeMs + CODEX_TOKEN_EXPIRY_SAFETY_MS,
      fetcher: dependencies.fetcher,
      now: dependencies.now,
      usageUrl: dependencies.usageUrl,
      timeoutMs: dependencies.timeoutMs,
      maxResponseBytes: dependencies.maxResponseBytes,
      createTimeoutSignal: dependencies.createTimeoutSignal,
    }),
  );
  const filesystem = dependencies.filesystem ?? defaultFilesystem;
  const commandRunner = dependencies.commandRunner ?? runCodexCommand;
  const authRelayFactory = dependencies.authRelayFactory ?? startCodexAuthRelay;
  let codexHome: string | null = null;
  let authRelay: CodexAuthRelay | null = null;
  let result: CodexInvocationResult | null = null;
  let failure: CodexInvocationError | null = null;
  try {
    codexHome = await filesystem.makePrivateTempDir("uos-sentinel-codex-");
    await filesystem.chmod(codexHome, 0o700);
    let stagedOutputSchemaPath: string | null = null;
    if (options.outputSchemaSourcePath !== undefined) {
      let sourceSchema: string;
      try {
        sourceSchema = await filesystem.readTextFile(options.outputSchemaSourcePath);
      } catch {
        throw new CodexInvocationError("invalid_options", { slot: selection.slot, probes: selection.probes });
      }
      stagedOutputSchemaPath = `${codexHome}/output-schema.json`;
      await filesystem.writePrivateTextFile(stagedOutputSchemaPath, sourceSchema);
      await filesystem.chmod(stagedOutputSchemaPath, 0o600);
    }
    const authPath = `${codexHome}/auth.json`;
    const lastMessagePath = `${codexHome}/last-message.json`;
    const runtimeAuthJson = syntheticCodexAuthJson(
      selection.auth,
      dependencies.now?.() ?? Date.now(),
    );
    await filesystem.writePrivateTextFile(authPath, runtimeAuthJson);
    await filesystem.chmod(authPath, 0o600);
    authRelay = await authRelayFactory(selection.auth);

    const commandResult = await commandRunner({
      executable: options.codexExecutable,
      args: options.args(lastMessagePath, authRelay.baseUrl, stagedOutputSchemaPath),
      cwd: options.checkoutPath,
      repositoryRoot: repositoryRootForCheckout(options.checkoutPath),
      workspaceWritable: options.workspaceWritable,
      env: minimalEnvironment(codexHome, dependencies.readEnvironment),
      clearEnv: true,
      stdin: options.prompt,
      outputLimitBytes: options.outputLimitBytes,
      timeoutMs: options.expectedMaximumRuntimeMs,
    });
    const currentAuth = await filesystem.readTextFile(authPath).catch(() => null);
    if (authRelay.authenticationRejected?.()) {
      throw new CodexInvocationError("auth_refresh_required", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    if (currentAuth !== runtimeAuthJson) {
      throw new CodexInvocationError("auth_mutated", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    if (commandResult.timedOut || authRelay.streamIdleTimedOut?.()) {
      throw new CodexInvocationError("invocation_timeout", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    let lastMessage: string | null = null;
    if (options.requireLastMessage) {
      lastMessage = await filesystem.readTextFile(lastMessagePath).catch(() => null);
      if (lastMessage !== null) await filesystem.chmod(lastMessagePath, 0o600);
    }
    if (outputContainsCodexSecret(selection.auth, commandResult.stdout, commandResult.stderr, lastMessage ?? "")) {
      throw new CodexInvocationError("secret_in_output", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    if (commandResult.outputExceeded) {
      throw new CodexInvocationError("output_limit_exceeded", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    if (commandResult.code !== 0) {
      throw new CodexInvocationError("command_failed", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    if (options.requireLastMessage && lastMessage === null) {
      throw new CodexInvocationError("last_message_missing", {
        slot: selection.slot,
        probes: selection.probes,
        commandResult,
      });
    }
    let nativeReviewOutput: unknown | null = null;
    if (options.requireNativeReviewOutput) {
      try {
        nativeReviewOutput = extractNativeReviewOutputFromRollouts(
          await filesystem.readPrivateRolloutFiles(codexHome),
        );
      } catch {
        throw new CodexInvocationError("native_review_missing", {
          slot: selection.slot,
          probes: selection.probes,
          commandResult,
        });
      }
      if (outputContainsCodexSecret(selection.auth, JSON.stringify(nativeReviewOutput))) {
        throw new CodexInvocationError("secret_in_output", {
          slot: selection.slot,
          probes: selection.probes,
          commandResult,
        });
      }
    }
    result = {
      slot: selection.slot,
      headroomPercent: selection.headroomPercent,
      probes: selection.probes,
      stdout: commandResult.stdout,
      stderr: commandResult.stderr,
      lastMessage,
      nativeReviewOutput,
    };
  } catch (error) {
    failure = error instanceof CodexInvocationError
      ? error
      : new CodexInvocationError("runtime_failure", { slot: selection.slot, probes: selection.probes });
  }
  if (authRelay !== null) {
    try {
      await authRelay.close();
    } catch {
      failure = new CodexInvocationError("runtime_failure", { slot: selection.slot, probes: selection.probes });
    }
  }
  if (codexHome !== null) {
    try {
      await filesystem.removeTree(codexHome);
    } catch {
      failure = new CodexInvocationError("runtime_failure", { slot: selection.slot, probes: selection.probes });
    }
  }
  if (failure !== null) throw failure;
  if (result === null) {
    throw new CodexInvocationError("runtime_failure", { slot: selection.slot, probes: selection.probes });
  }
  return result;
};

export const runStructuredCodexAgent = async (
  options: StructuredCodexAgentOptions,
  dependencies: CodexInvocationDependencies = {},
): Promise<CodexInvocationResult> => {
  const checkoutPath = absolutePath(options.checkoutPath);
  const outputSchemaPath = absolutePath(options.outputSchemaPath);
  const expectedMaximumRuntimeMs = positiveInteger(
    options.expectedMaximumRuntimeMs ?? CODEX_EXPECTED_INVOCATION_MS,
    "expectedMaximumRuntimeMs",
  );
  const outputLimitBytes = positiveInteger(
    options.outputLimitBytes ?? CODEX_COMMAND_OUTPUT_LIMIT_BYTES,
    "outputLimitBytes",
  );
  if (new TextEncoder().encode(options.prompt).byteLength > CODEX_MAX_PROMPT_BYTES) {
    throw new CodexInvocationError("invalid_options");
  }
  const policy = SENTINEL_AGENT_POLICIES[options.role];
  if (!policy) throw new CodexInvocationError("invalid_options");
  return await invokePrivately({
    checkoutPath,
    authSlots: options.authSlots,
    codexExecutable: options.codexExecutable ?? "codex",
    expectedMaximumRuntimeMs,
    outputLimitBytes,
    prompt: options.prompt,
    quotaModel: policy.model,
    outputSchemaSourcePath: outputSchemaPath,
    args: (lastMessagePath, relayBaseUrl, stagedOutputSchemaPath) => {
      if (stagedOutputSchemaPath === null) {
        throw new Error("Provider Sentinel output schema was not staged into the private Codex home");
      }
      return execConfigArgs(checkoutPath, policy, stagedOutputSchemaPath, lastMessagePath, relayBaseUrl);
    },
    requireLastMessage: true,
    requireNativeReviewOutput: false,
    workspaceWritable: policy.sandbox === "workspace-write",
  }, dependencies);
};

export type StructuredCodexAgentContinuationOptions = Readonly<
  & StructuredCodexAgentOptions
  & {
    /** The first bounded attempt. A timeout gets one and only one continuation. */
    initialTimeoutMs?: number;
    /** The final bounded continuation after the first attempt times out. */
    continuationTimeoutMs?: number;
    /** Trusted checks to run after a timeout and before the continuation starts. */
    onTimeout?(error: CodexInvocationError): Promise<void> | void;
  }
>;

/**
 * Run one implementation invocation and, only after a timeout, one bounded
 * continuation in the same checkout. The caller owns candidate integrity
 * checks in `onTimeout`; this helper never resets or silently substitutes a
 * model. `runStructuredCodexAgent` still selects the role policy, so an
 * implementation call remains pinned to gpt-5.6-luna at max reasoning.
 */
export const runStructuredCodexAgentWithContinuation = async (
  options: StructuredCodexAgentContinuationOptions,
  dependencies: CodexInvocationDependencies = {},
): Promise<CodexInvocationResult> => {
  const initialTimeoutMs = positiveInteger(
    options.initialTimeoutMs ?? options.expectedMaximumRuntimeMs ?? CODEX_EXPECTED_INVOCATION_MS,
    "initialTimeoutMs",
  );
  const continuationTimeoutMs = positiveInteger(
    options.continuationTimeoutMs ?? initialTimeoutMs,
    "continuationTimeoutMs",
  );
  const invoke = (attempt: 1 | 2): Promise<CodexInvocationResult> =>
    runStructuredCodexAgent({
      ...options,
      prompt: `${options.prompt}\n\n${
        attempt === 1
          ? "Finish this bounded implementation-cell invocation and return the required JSON before the deadline. Prioritize the scoped repair over optional work."
          : "The first bounded implementation-cell invocation timed out. Continue from the existing cell changes. Inspect the current diff, do not redo completed work, and return the required JSON within this final bounded continuation."
      }`,
      expectedMaximumRuntimeMs: attempt === 1 ? initialTimeoutMs : continuationTimeoutMs,
    }, dependencies);
  try {
    return await invoke(1);
  } catch (error) {
    if (!(error instanceof CodexInvocationError) || error.failure !== "invocation_timeout") throw error;
    await options.onTimeout?.(error);
    return await invoke(2);
  }
};

export const runNativeCodexReview = async (
  options: NativeCodexReviewOptions,
  dependencies: CodexInvocationDependencies = {},
): Promise<CodexInvocationResult> => {
  const checkoutPath = absolutePath(options.checkoutPath);
  const expectedMaximumRuntimeMs = positiveInteger(
    options.expectedMaximumRuntimeMs ?? CODEX_EXPECTED_INVOCATION_MS,
    "expectedMaximumRuntimeMs",
  );
  const outputLimitBytes = positiveInteger(
    options.outputLimitBytes ?? CODEX_COMMAND_OUTPUT_LIMIT_BYTES,
    "outputLimitBytes",
  );
  return await invokePrivately({
    checkoutPath,
    authSlots: options.authSlots,
    codexExecutable: options.codexExecutable ?? "codex",
    expectedMaximumRuntimeMs,
    outputLimitBytes,
    prompt: "",
    quotaModel: null,
    args: (_lastMessagePath, relayBaseUrl) => nativeReviewArgs(checkoutPath, relayBaseUrl),
    requireLastMessage: false,
    requireNativeReviewOutput: true,
    workspaceWritable: false,
  }, dependencies);
};
