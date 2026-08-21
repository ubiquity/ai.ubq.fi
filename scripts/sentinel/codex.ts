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
  timedOut?: boolean;
}>;

export type CodexCommandRunner = (request: CodexCommandRequest) => Promise<CodexCommandResult>;

export type CodexFilesystem = Readonly<{
  makePrivateTempDir(prefix: string): Promise<string>;
  chmod(path: string, mode: number): Promise<void>;
  writePrivateTextFile(path: string, contents: string): Promise<void>;
  readTextFile(path: string): Promise<string>;
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
}>;

export type CodexInvocationFailureCode =
  | "accounts_unavailable"
  | "invalid_options"
  | "command_failed"
  | "invocation_timeout"
  | "output_limit_exceeded"
  | "secret_in_output"
  | "auth_mutated"
  | "last_message_missing"
  | "runtime_failure";

export class CodexInvocationError extends Error {
  readonly failure: CodexInvocationFailureCode;
  readonly slot: CodexAuthSlot | null;
  readonly exitCode: number | null;
  readonly probes: readonly [CodexUsageProbe, CodexUsageProbe] | null;

  constructor(
    failure: CodexInvocationFailureCode,
    options: Readonly<{
      slot?: CodexAuthSlot;
      exitCode?: number;
      probes?: readonly [CodexUsageProbe, CodexUsageProbe];
    }> = {},
  ) {
    super(`Codex invocation stopped (${failure}).`);
    this.name = "CodexInvocationError";
    this.failure = failure;
    this.slot = options.slot ?? null;
    this.exitCode = options.exitCode ?? null;
    this.probes = options.probes ?? null;
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
): Promise<string> => {
  const reader = stream.getReader();
  const retained: Uint8Array[] = [];
  try {
    while (true) {
      const next = await reader.read();
      if (next.done) break;
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
  return new TextDecoder().decode(combined);
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

/** Captures output privately. It never inherits stdout, stderr, stdin, or environment. */
export const runCodexCommand: CodexCommandRunner = async (request) => {
  if (Deno.build.os !== "linux") {
    throw new Error("Provider Sentinel Codex isolation requires Linux bubblewrap");
  }
  const signal = AbortSignal.timeout(request.timeoutMs);
  const command = new Deno.Command("bwrap", {
    args: codexBubblewrapArguments(request),
    cwd: request.cwd,
    env: { ...request.env },
    clearEnv: request.clearEnv,
    signal,
    stdin: "piped",
    stdout: "piped",
    stderr: "piped",
  });
  const child = command.spawn();
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
    const [status, stdout, stderr] = await Promise.all([child.status, stdoutPromise, stderrPromise, inputPromise])
      .then(([status, stdout, stderr]) => [status, stdout, stderr] as const);
    return { code: status.code, stdout, stderr, outputExceeded: outputState.exceeded, timedOut: false };
  } catch (error) {
    await Promise.allSettled([stdoutPromise, stderrPromise, inputPromise]);
    if (signal.aborted) {
      return { code: -1, stdout: "", stderr: "", outputExceeded: outputState.exceeded, timedOut: true };
    }
    throw error;
  }
};

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

export const syntheticCodexAuthJson = (auth: CodexAuthDocument): string => {
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
    last_refresh: auth.lastRefresh,
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

export const createCodexAuthRelayFactory = (
  upstreamFetch: (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>,
): CodexAuthRelayFactory =>
async (auth) => {
  const relayPrefix = `/sentinel-${crypto.randomUUID()}/backend-api`;
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
    const responseHeaders = new Headers(upstream.headers);
    responseHeaders.delete("set-cookie");
    responseHeaders.delete("location");
    responseHeaders.set("Cache-Control", "no-store");
    return new Response(upstream.body, {
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
  args: (lastMessagePath: string, relayBaseUrl: string) => readonly string[];
  requireLastMessage: boolean;
  workspaceWritable: boolean;
}>;

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
    const authPath = `${codexHome}/auth.json`;
    const lastMessagePath = `${codexHome}/last-message.json`;
    const runtimeAuthJson = syntheticCodexAuthJson(selection.auth);
    await filesystem.writePrivateTextFile(authPath, runtimeAuthJson);
    await filesystem.chmod(authPath, 0o600);
    authRelay = await authRelayFactory(selection.auth);

    const commandResult = await commandRunner({
      executable: options.codexExecutable,
      args: options.args(lastMessagePath, authRelay.baseUrl),
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
    if (currentAuth !== runtimeAuthJson) {
      throw new CodexInvocationError("auth_mutated", {
        slot: selection.slot,
        exitCode: commandResult.code,
        probes: selection.probes,
      });
    }
    if (commandResult.timedOut === true) {
      throw new CodexInvocationError("invocation_timeout", {
        slot: selection.slot,
        probes: selection.probes,
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
        exitCode: commandResult.code,
        probes: selection.probes,
      });
    }
    if (commandResult.outputExceeded) {
      throw new CodexInvocationError("output_limit_exceeded", {
        slot: selection.slot,
        exitCode: commandResult.code,
        probes: selection.probes,
      });
    }
    if (commandResult.code !== 0) {
      throw new CodexInvocationError("command_failed", {
        slot: selection.slot,
        exitCode: commandResult.code,
        probes: selection.probes,
      });
    }
    if (options.requireLastMessage && lastMessage === null) {
      throw new CodexInvocationError("last_message_missing", {
        slot: selection.slot,
        exitCode: commandResult.code,
        probes: selection.probes,
      });
    }
    result = {
      slot: selection.slot,
      headroomPercent: selection.headroomPercent,
      probes: selection.probes,
      stdout: commandResult.stdout,
      stderr: commandResult.stderr,
      lastMessage,
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
    args: (lastMessagePath, relayBaseUrl) =>
      execConfigArgs(checkoutPath, policy, outputSchemaPath, lastMessagePath, relayBaseUrl),
    requireLastMessage: true,
    workspaceWritable: policy.sandbox === "workspace-write",
  }, dependencies);
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
    workspaceWritable: false,
  }, dependencies);
};
