import { type CodexAuthDocument, type CodexAuthSlot, parseCodexAuthJsonB64ForMaintenance } from "./quota.ts";

export const SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS = 6 * 24 * 60 * 60 * 1_000;
export const SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS = 24 * 60 * 60 * 1_000;
export const SENTINEL_CODEX_AUTH_READINESS_MIN_VALIDITY_MS = 50 * 60_000;
export const SENTINEL_CODEX_AUTH_MAINTENANCE_TIMEOUT_MS = 5 * 60 * 1_000;
const MAINTENANCE_OUTPUT_LIMIT_BYTES = 1024 * 1024;
const AUTH_DOCUMENT_MAX_BYTES = 1024 * 1024;
const INITIALIZE_REQUEST_ID = 0;
const ACCOUNT_READ_REQUEST_ID = 1;
const TEXT_ENCODER = new TextEncoder();

const maintenancePath = (): string => {
  try {
    const configured = Deno.env.get("PATH");
    if (configured?.startsWith("/")) return configured;
  } catch {
    // Unit tests and other restricted runtimes may intentionally deny env
    // access. Codex still receives a deterministic, minimal executable path.
  }
  return "/usr/local/bin:/usr/bin:/bin";
};

export type CodexAuthMaintenanceDisposition = Readonly<{
  slot: CodexAuthSlot;
  due: boolean;
  invoked: boolean;
  rpcSucceeded: boolean;
  managedAccountAvailable: boolean;
  duplicateAccountSkipped: boolean;
  commandCode: number | null;
  timedOut: boolean;
  outputExceeded: boolean;
  stdoutBytes: number;
  stderrBytes: number;
  stateChanged: boolean;
  readyForMaintenanceWindow: boolean;
}>;

const encodeStandardBase64 = (value: string): string => {
  const bytes = new TextEncoder().encode(value);
  try {
    let binary = "";
    for (let offset = 0; offset < bytes.byteLength; offset += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    }
    return btoa(binary);
  } finally {
    bytes.fill(0);
  }
};

const parseRawAuth = (rawJson: string, slot: CodexAuthSlot): CodexAuthDocument =>
  parseCodexAuthJsonB64ForMaintenance(encodeStandardBase64(rawJson), slot);

export const codexAuthMaintenanceDue = (
  auth: Pick<CodexAuthDocument, "lastRefresh" | "accessTokenExpiresAtMs">,
  nowMs: number,
): boolean => {
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new TypeError("Auth maintenance clock is invalid");
  const lastRefreshMs = Date.parse(auth.lastRefresh);
  if (!Number.isSafeInteger(lastRefreshMs) || lastRefreshMs > nowMs + 5 * 60 * 1_000) return true;
  return lastRefreshMs <= nowMs - SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS ||
    auth.accessTokenExpiresAtMs <= nowMs + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS;
};

export const codexAuthMaintenanceArgs = (): readonly string[] => [
  "app-server",
  "--listen",
  "stdio://",
  "--strict-config",
  "-c",
  'cli_auth_credentials_store="file"',
];

export type CodexAuthAppServerRequest = Readonly<{
  executable: string;
  args: readonly string[];
  cwd: string;
  env: Readonly<Record<string, string>>;
  clearEnv: true;
  outputLimitBytes: number;
  timeoutMs: number;
}>;

export type CodexAuthAppServerResult = Readonly<{
  code: number;
  rpcSucceeded: boolean;
  managedAccountAvailable: boolean;
  outputExceeded: boolean;
  timedOut: boolean;
  stdoutBytes: number;
  stderrBytes: number;
  durationMs: number;
}>;

export type CodexAuthAppServerChild = Readonly<{
  status: Promise<Readonly<{ code: number }>>;
  stdout: ReadableStream<Uint8Array>;
  stderr: ReadableStream<Uint8Array>;
  stdin: WritableStream<Uint8Array>;
  kill(signal?: Deno.Signal): void;
}>;

export type CodexAuthAppServerRuntime = Readonly<{
  spawn(request: CodexAuthAppServerRequest): CodexAuthAppServerChild;
  now(): number;
  setTimer(callback: () => void, delayMs: number): ReturnType<typeof setTimeout>;
  clearTimer(timer: ReturnType<typeof setTimeout>): void;
}>;

const directAppServerRuntime: CodexAuthAppServerRuntime = {
  spawn(request) {
    return new Deno.Command(request.executable, {
      args: [...request.args],
      cwd: request.cwd,
      env: { ...request.env },
      clearEnv: request.clearEnv,
      stdin: "piped",
      stdout: "piped",
      stderr: "piped",
    }).spawn();
  },
  now: () => performance.now(),
  setTimer: (callback, delayMs) => setTimeout(callback, delayMs),
  clearTimer: (timer) => clearTimeout(timer),
};

class AppServerProtocolStopped extends Error {}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const createBoundedJsonlReader = (
  stream: ReadableStream<Uint8Array>,
  observe: (byteLength: number) => void,
) => {
  const reader = stream.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let buffer = "";
  let finished = false;
  return {
    async next(): Promise<string | null> {
      while (true) {
        const newline = buffer.indexOf("\n");
        if (newline >= 0) {
          const line = buffer.slice(0, newline);
          buffer = buffer.slice(newline + 1);
          return line;
        }
        if (finished) {
          if (!buffer) return null;
          const line = buffer;
          buffer = "";
          return line;
        }
        const chunk = await reader.read();
        if (chunk.done) {
          buffer += decoder.decode();
          finished = true;
          continue;
        }
        observe(chunk.value.byteLength);
        buffer += decoder.decode(chunk.value, { stream: true });
      }
    },
    release(): void {
      reader.releaseLock();
    },
  };
};

const waitForAppServerResponse = async (
  reader: ReturnType<typeof createBoundedJsonlReader>,
  id: number,
  validateResult: (result: unknown) => boolean,
): Promise<boolean> => {
  while (true) {
    const line = await reader.next();
    if (line === null) throw new AppServerProtocolStopped();
    if (!line) continue;
    let value: unknown;
    try {
      value = JSON.parse(line);
    } catch {
      throw new AppServerProtocolStopped();
    }
    if (!isRecord(value)) throw new AppServerProtocolStopped();
    if (!("id" in value)) {
      if (typeof value.method !== "string") throw new AppServerProtocolStopped();
      continue;
    }
    if (value.id !== id) throw new AppServerProtocolStopped();
    return !("error" in value) && "result" in value && validateResult(value.result);
  }
};

const isInitializeResult = (value: unknown, codexHome: string): boolean =>
  isRecord(value) && value.codexHome === codexHome &&
  typeof value.userAgent === "string" &&
  typeof value.platformFamily === "string" &&
  typeof value.platformOs === "string";

const inspectGetAccountResponse = (value: unknown): Readonly<{ managedAccountAvailable: boolean }> | null => {
  if (!isRecord(value) || typeof value.requiresOpenaiAuth !== "boolean" || !("account" in value)) return null;
  if (value.account === null) return { managedAccountAvailable: false };
  if (!isRecord(value.account)) return null;
  if (value.account.type === "apiKey") return { managedAccountAvailable: false };
  if (value.account.type === "amazonBedrock") {
    return typeof value.account.usesCodexManagedCredentials === "boolean" ? { managedAccountAvailable: false } : null;
  }
  if (
    value.account.type !== "chatgpt" ||
    !(typeof value.account.email === "string" || value.account.email === null) ||
    typeof value.account.planType !== "string"
  ) return null;
  return { managedAccountAvailable: value.requiresOpenaiAuth };
};

const validateTrailingAppServerLine = (line: string): void => {
  if (!line) return;
  let value: unknown;
  try {
    value = JSON.parse(line);
  } catch {
    throw new AppServerProtocolStopped();
  }
  if (!isRecord(value) || "id" in value || typeof value.method !== "string") {
    throw new AppServerProtocolStopped();
  }
};

export const runCodexAuthAppServerWithRuntime = async (
  request: CodexAuthAppServerRequest,
  runtime: CodexAuthAppServerRuntime = directAppServerRuntime,
): Promise<CodexAuthAppServerResult> => {
  if (!Number.isSafeInteger(request.outputLimitBytes) || request.outputLimitBytes <= 0) {
    throw new TypeError("Codex auth app-server output limit is invalid");
  }
  if (!Number.isSafeInteger(request.timeoutMs) || request.timeoutMs <= 0) {
    throw new TypeError("Codex auth app-server timeout is invalid");
  }
  const startedAt = runtime.now();
  const child = runtime.spawn(request);
  let stdoutBytes = 0;
  let stderrBytes = 0;
  let observedBytes = 0;
  let outputExceeded = false;
  let timedOut = false;
  let stopped = false;
  const stop = (reason: "output" | "timeout"): void => {
    if (stopped) return;
    stopped = true;
    if (reason === "output") outputExceeded = true;
    if (reason === "timeout") timedOut = true;
    try {
      child.kill("SIGKILL");
    } catch {
      // The child may already have exited after writing its response.
    }
  };
  const observe = (target: "stdout" | "stderr", byteLength: number): void => {
    if (target === "stdout") stdoutBytes += byteLength;
    else stderrBytes += byteLength;
    observedBytes += byteLength;
    if (observedBytes > request.outputLimitBytes) {
      stop("output");
      throw new AppServerProtocolStopped();
    }
  };
  const timer = runtime.setTimer(() => stop("timeout"), request.timeoutMs);
  const stdout = createBoundedJsonlReader(child.stdout, (byteLength) => observe("stdout", byteLength));
  const stderr = child.stderr.getReader();
  const stderrDrain = (async (): Promise<void> => {
    try {
      while (true) {
        const chunk = await stderr.read();
        if (chunk.done) return;
        observe("stderr", chunk.value.byteLength);
      }
    } finally {
      stderr.releaseLock();
    }
  })().then(
    () => ({ ok: true } as const),
    // Attach the rejection handler immediately: output overflow may reject
    // this concurrent drain while the main protocol task is still blocked.
    (error: unknown) => ({ ok: false, error } as const),
  );
  const writer = child.stdin.getWriter();
  const send = (message: unknown): Promise<void> => writer.write(TEXT_ENCODER.encode(`${JSON.stringify(message)}\n`));
  let rpcSucceeded = false;
  let managedAccountAvailable = false;
  let protocolFailure = false;
  try {
    try {
      await send({
        method: "initialize",
        id: INITIALIZE_REQUEST_ID,
        params: {
          clientInfo: {
            name: "provider_sentinel_auth_maintenance",
            title: "Provider Sentinel Auth Maintenance",
            version: "1.0.0",
          },
        },
      });
      const initialized = await waitForAppServerResponse(
        stdout,
        INITIALIZE_REQUEST_ID,
        (result) => isInitializeResult(result, request.env.CODEX_HOME),
      );
      if (!initialized) throw new AppServerProtocolStopped();
      await send({ method: "initialized" });
      await send({
        method: "account/read",
        id: ACCOUNT_READ_REQUEST_ID,
        params: { refreshToken: true },
      });
      rpcSucceeded = await waitForAppServerResponse(
        stdout,
        ACCOUNT_READ_REQUEST_ID,
        (result) => {
          const inspected = inspectGetAccountResponse(result);
          if (inspected === null) return false;
          managedAccountAvailable = inspected.managedAccountAvailable;
          return true;
        },
      );
    } catch {
      protocolFailure = true;
    } finally {
      try {
        await writer.close();
      } catch {
        protocolFailure = true;
      }
      writer.releaseLock();
    }
    try {
      while (true) {
        const line = await stdout.next();
        if (line === null) break;
        // Drain bounded private protocol output without retaining or logging it.
        validateTrailingAppServerLine(line);
      }
    } catch {
      protocolFailure = true;
    } finally {
      stdout.release();
    }
    const status = await child.status.catch((error) => {
      if (timedOut || outputExceeded) return { code: 1 };
      throw error;
    });
    const stderrOutcome = await stderrDrain;
    if (!stderrOutcome.ok && !(timedOut || outputExceeded)) throw stderrOutcome.error;
    return {
      code: status.code,
      rpcSucceeded: rpcSucceeded && !protocolFailure,
      managedAccountAvailable: rpcSucceeded && !protocolFailure && managedAccountAvailable,
      outputExceeded,
      timedOut,
      stdoutBytes,
      stderrBytes,
      durationMs: Math.max(0, runtime.now() - startedAt),
    };
  } finally {
    runtime.clearTimer(timer);
  }
};

export type CodexAuthMaintenanceDependencies = Readonly<{
  now?: () => number;
  runAppServer?: (request: CodexAuthAppServerRequest) => Promise<CodexAuthAppServerResult>;
  readTextFile?: (path: string) => Promise<string>;
  stageAuth?: (slotDirectory: string, rawJson: string) => Promise<CodexAuthMaintenanceStage>;
  promoteAuth?: (stagedAuthPath: string, durableAuthPath: string, expectedDurableRaw: string) => Promise<void>;
  discardStage?: (stage: CodexAuthMaintenanceStage) => Promise<void>;
}>;

export type CodexAuthMaintenanceStage = Readonly<{
  directory: string;
  authPath: string;
}>;

const stageCodexAuth = async (slotDirectory: string, rawJson: string): Promise<CodexAuthMaintenanceStage> => {
  const directory = await Deno.makeTempDir({ dir: slotDirectory, prefix: ".sentinel-auth-maintenance-" });
  try {
    await Deno.chmod(directory, 0o700);
    const authPath = `${directory}/auth.json`;
    await Deno.writeTextFile(authPath, rawJson, { createNew: true, mode: 0o600 });
    return { directory, authPath };
  } catch (error) {
    await Deno.remove(directory, { recursive: true }).catch(() => undefined);
    throw error;
  }
};

const promoteCodexAuth = async (
  stagedAuthPath: string,
  durableAuthPath: string,
  expectedDurableRaw: string,
): Promise<void> => {
  const info = await Deno.lstat(stagedAuthPath);
  if (!info.isFile || info.isSymlink || info.size <= 0 || info.size > AUTH_DOCUMENT_MAX_BYTES) {
    throw new Error("Staged Sentinel Codex auth file is invalid");
  }
  if (await Deno.readTextFile(durableAuthPath) !== expectedDurableRaw) {
    throw new Error("Sentinel Codex auth changed during maintenance");
  }
  await Deno.chmod(stagedAuthPath, 0o600);
  await Deno.rename(stagedAuthPath, durableAuthPath);
};

const discardCodexAuthStage = async (stage: CodexAuthMaintenanceStage): Promise<void> => {
  try {
    const info = await Deno.lstat(stage.authPath);
    if (info.isFile && !info.isSymlink) await Deno.writeTextFile(stage.authPath, "");
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
  try {
    await Deno.remove(stage.directory, { recursive: true });
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
};

export const maintainCodexAuthSlot = async (
  input: Readonly<{
    slot: CodexAuthSlot;
    slotDirectory: string;
    workspace: string;
    executable?: string;
  }>,
  dependencies: CodexAuthMaintenanceDependencies = {},
): Promise<CodexAuthMaintenanceDisposition> => {
  const nowMs = Math.trunc((dependencies.now ?? Date.now)());
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new TypeError("Auth maintenance clock is invalid");
  const readTextFile = dependencies.readTextFile ?? ((path: string) => Deno.readTextFile(path));
  const runAppServer = dependencies.runAppServer ?? runCodexAuthAppServerWithRuntime;
  const stageAuth = dependencies.stageAuth ?? stageCodexAuth;
  const promoteAuth = dependencies.promoteAuth ?? promoteCodexAuth;
  const discardStage = dependencies.discardStage ?? discardCodexAuthStage;
  const authPath = `${input.slotDirectory}/auth.json`;
  const beforeRaw = await readTextFile(authPath);
  if (TEXT_ENCODER.encode(beforeRaw).byteLength > AUTH_DOCUMENT_MAX_BYTES) {
    throw new Error(`Sentinel Codex auth slot ${input.slot} document is too large`);
  }
  const before = parseRawAuth(beforeRaw, input.slot);
  const due = codexAuthMaintenanceDue(before, nowMs);
  if (!due) {
    return {
      slot: input.slot,
      due: false,
      invoked: false,
      rpcSucceeded: false,
      managedAccountAvailable: false,
      duplicateAccountSkipped: false,
      commandCode: null,
      timedOut: false,
      outputExceeded: false,
      stdoutBytes: 0,
      stderrBytes: 0,
      stateChanged: false,
      readyForMaintenanceWindow: before.accessTokenExpiresAtMs > nowMs + SENTINEL_CODEX_AUTH_READINESS_MIN_VALIDITY_MS,
    };
  }

  const stage = await stageAuth(input.slotDirectory, beforeRaw);
  try {
    const request: CodexAuthAppServerRequest = {
      executable: input.executable ?? "codex",
      args: codexAuthMaintenanceArgs(),
      cwd: input.workspace,
      env: {
        CODEX_HOME: stage.directory,
        HOME: stage.directory,
        PATH: maintenancePath(),
        CI: "true",
        LANG: "C.UTF-8",
        LC_ALL: "C.UTF-8",
        NO_COLOR: "1",
        TERM: "dumb",
      },
      clearEnv: true,
      outputLimitBytes: MAINTENANCE_OUTPUT_LIMIT_BYTES,
      timeoutMs: SENTINEL_CODEX_AUTH_MAINTENANCE_TIMEOUT_MS,
    };
    let command: CodexAuthAppServerResult | null = null;
    let runnerFailure: Readonly<{ error: unknown }> | null = null;
    try {
      command = await runAppServer(request);
    } catch (error) {
      runnerFailure = { error };
    }

    // Codex operates only on the staged copy. Preserve its complete rewrite
    // after an actual token transition even if the process or RPC later
    // failed. Never promote formatting or last_refresh-only mutations.
    const afterRaw = await readTextFile(stage.authPath);
    if (TEXT_ENCODER.encode(afterRaw).byteLength > AUTH_DOCUMENT_MAX_BYTES) {
      throw new Error(`Sentinel Codex auth slot ${input.slot} document is too large after maintenance`);
    }
    const after = parseRawAuth(afterRaw, input.slot);
    if (after.tokens.account_id !== before.tokens.account_id) {
      throw new Error(`Sentinel Codex auth slot ${input.slot} changed account identity during maintenance`);
    }
    const refreshed = after.tokens.id_token !== before.tokens.id_token ||
      after.tokens.access_token !== before.tokens.access_token ||
      after.tokens.refresh_token !== before.tokens.refresh_token;
    if (refreshed) await promoteAuth(stage.authPath, authPath, beforeRaw);
    if (runnerFailure !== null) throw runnerFailure.error;
    if (command === null) throw new Error("Sentinel Codex auth app-server returned no result");
    const durable = refreshed ? after : before;
    return {
      slot: input.slot,
      due: true,
      invoked: true,
      rpcSucceeded: command.rpcSucceeded,
      managedAccountAvailable: command.managedAccountAvailable,
      duplicateAccountSkipped: false,
      commandCode: command.code,
      timedOut: command.timedOut,
      outputExceeded: command.outputExceeded,
      stdoutBytes: command.stdoutBytes,
      stderrBytes: command.stderrBytes,
      stateChanged: refreshed && afterRaw !== beforeRaw,
      readyForMaintenanceWindow: durable.accessTokenExpiresAtMs > nowMs + SENTINEL_CODEX_AUTH_READINESS_MIN_VALIDITY_MS,
    };
  } finally {
    await discardStage(stage);
  }
};

export type CodexAuthMaintenanceSlotInput = Readonly<{
  slot: CodexAuthSlot;
  slotDirectory: string;
  workspace: string;
  executable?: string;
}>;

const preferAuthMaintenanceCandidate = (candidate: CodexAuthDocument, current: CodexAuthDocument): boolean => {
  const candidateRefreshMs = Date.parse(candidate.lastRefresh);
  const currentRefreshMs = Date.parse(current.lastRefresh);
  if (candidateRefreshMs !== currentRefreshMs) return candidateRefreshMs > currentRefreshMs;
  if (candidate.accessTokenExpiresAtMs !== current.accessTokenExpiresAtMs) {
    return candidate.accessTokenExpiresAtMs > current.accessTokenExpiresAtMs;
  }
  return candidate.slot < current.slot;
};

export const maintainCodexAuthSlots = async (
  inputs: readonly CodexAuthMaintenanceSlotInput[],
  dependencies: CodexAuthMaintenanceDependencies = {},
): Promise<readonly CodexAuthMaintenanceDisposition[]> => {
  const nowMs = Math.trunc((dependencies.now ?? Date.now)());
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new TypeError("Auth maintenance clock is invalid");
  const readTextFile = dependencies.readTextFile ?? ((path: string) => Deno.readTextFile(path));
  const snapshots = await Promise.all(inputs.map(async (input) => {
    const raw = await readTextFile(`${input.slotDirectory}/auth.json`);
    if (TEXT_ENCODER.encode(raw).byteLength > AUTH_DOCUMENT_MAX_BYTES) {
      throw new Error(`Sentinel Codex auth slot ${input.slot} document is too large`);
    }
    return { input, auth: parseRawAuth(raw, input.slot) };
  }));
  const canonicalByAccount = new Map<string, CodexAuthDocument>();
  const accountCounts = new Map<string, number>();
  for (const { auth } of snapshots) {
    const accountId = auth.tokens.account_id;
    accountCounts.set(accountId, (accountCounts.get(accountId) ?? 0) + 1);
    const current = canonicalByAccount.get(accountId);
    if (current === undefined || preferAuthMaintenanceCandidate(auth, current)) {
      canonicalByAccount.set(accountId, auth);
    }
  }
  const stableDependencies: CodexAuthMaintenanceDependencies = { ...dependencies, now: () => nowMs };
  const dispositions: CodexAuthMaintenanceDisposition[] = [];
  for (const { input, auth } of snapshots) {
    const accountId = auth.tokens.account_id;
    const duplicate = (accountCounts.get(accountId) ?? 0) > 1;
    if (duplicate && canonicalByAccount.get(accountId)?.slot !== input.slot) {
      dispositions.push({
        slot: input.slot,
        due: codexAuthMaintenanceDue(auth, nowMs),
        invoked: false,
        rpcSucceeded: false,
        managedAccountAvailable: false,
        duplicateAccountSkipped: true,
        commandCode: null,
        timedOut: false,
        outputExceeded: false,
        stdoutBytes: 0,
        stderrBytes: 0,
        stateChanged: false,
        readyForMaintenanceWindow: auth.accessTokenExpiresAtMs > nowMs + SENTINEL_CODEX_AUTH_READINESS_MIN_VALIDITY_MS,
      });
      continue;
    }
    dispositions.push(await maintainCodexAuthSlot(input, stableDependencies));
  }
  return dispositions;
};

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required`);
  return value;
};

const requiredExecutableEnvironment = (name: string): string => {
  const value = Deno.env.get(name);
  if (!value || value !== value.trim() || !value.startsWith("/") || /[\0\r\n]/u.test(value)) {
    throw new Error(`${name} must be an absolute executable path`);
  }
  return value;
};

const runCli = async (): Promise<void> => {
  if (Deno.args.length !== 1 || Deno.args[0] !== "maintain") {
    throw new Error("Usage: auth-maintenance.ts maintain");
  }
  const runnerTemp = requiredEnvironment("RUNNER_TEMP");
  const stateDirectory = requiredEnvironment("SENTINEL_CODEX_AUTH_STATE_DIR");
  const executable = requiredExecutableEnvironment("SENTINEL_CODEX_AUTH_EXECUTABLE");
  if (!runnerTemp.startsWith("/") || stateDirectory !== `${runnerTemp}/sentinel-codex-auth-state`) {
    throw new Error("Sentinel Codex auth maintenance received an invalid state path");
  }
  const workspace = `${runnerTemp}/sentinel-codex-auth-maintenance-workspace`;
  await Deno.mkdir(workspace, { recursive: true, mode: 0o700 });
  await Deno.chmod(workspace, 0o700);
  const slots: CodexAuthMaintenanceSlotInput[] = [];
  for (const slot of [1, 2] as const) {
    const slotDirectory = `${stateDirectory}/slots/${slot}`;
    let info: Deno.FileInfo;
    try {
      info = await Deno.lstat(`${slotDirectory}/auth.json`);
    } catch (error) {
      if (error instanceof Deno.errors.NotFound) continue;
      throw error;
    }
    if (!info.isFile || info.isSymlink || info.size <= 0 || info.size > AUTH_DOCUMENT_MAX_BYTES) {
      throw new Error(`Sentinel Codex auth slot ${slot} file is invalid`);
    }
    slots.push({ slot, slotDirectory, workspace, executable });
  }
  if (slots.length === 0) throw new Error("No Sentinel Codex auth slots are configured");
  const dispositions = await maintainCodexAuthSlots(slots);
  // This contains only categorical and numeric process metadata. Token values,
  // raw output, and auth documents are intentionally never printed.
  console.log(JSON.stringify({ schema_version: 1, slots: dispositions }));
};

if (import.meta.main) await runCli();
