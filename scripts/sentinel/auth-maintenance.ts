import { type CodexCommandRequest, type CodexCommandRuntime, runCodexCommandWithRuntime } from "./codex.ts";
import { type CodexAuthDocument, type CodexAuthSlot, parseCodexAuthJsonB64ForMaintenance } from "./quota.ts";

export const SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS = 6 * 24 * 60 * 60 * 1_000;
export const SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS = 24 * 60 * 60 * 1_000;
export const SENTINEL_CODEX_AUTH_MAINTENANCE_TIMEOUT_MS = 5 * 60 * 1_000;
const MAINTENANCE_OUTPUT_LIMIT_BYTES = 1024 * 1024;
const AUTH_DOCUMENT_MAX_BYTES = 1024 * 1024;
const FIXED_MAINTENANCE_PROMPT =
  "Reply with exactly OK. Do not inspect files, run tools, browse, or perform any other task.";

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

export const codexAuthMaintenanceArgs = (workspace: string): readonly string[] => [
  "exec",
  "--ignore-rules",
  "--ephemeral",
  "--ignore-user-config",
  "--strict-config",
  "--skip-git-repo-check",
  "--json",
  "--color",
  "never",
  "-m",
  "gpt-5.6-luna",
  "-c",
  'model_reasoning_effort="low"',
  "-s",
  "read-only",
  "-c",
  'approval_policy="never"',
  "-c",
  'web_search="disabled"',
  "-c",
  "agents.enabled=false",
  "--cd",
  workspace,
  "-",
];

const directCodexRuntime: CodexCommandRuntime = {
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

export type CodexAuthMaintenanceDependencies = Readonly<{
  now?: () => number;
  runCommand?: (request: CodexCommandRequest) => ReturnType<typeof runCodexCommandWithRuntime>;
  readTextFile?: (path: string) => Promise<string>;
}>;

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
  const authPath = `${input.slotDirectory}/auth.json`;
  const beforeRaw = await readTextFile(authPath);
  if (new TextEncoder().encode(beforeRaw).byteLength > AUTH_DOCUMENT_MAX_BYTES) {
    throw new Error(`Sentinel Codex auth slot ${input.slot} document is too large`);
  }
  const before = parseRawAuth(beforeRaw, input.slot);
  const due = codexAuthMaintenanceDue(before, nowMs);
  if (!due) {
    return {
      slot: input.slot,
      due: false,
      invoked: false,
      commandCode: null,
      timedOut: false,
      outputExceeded: false,
      stdoutBytes: 0,
      stderrBytes: 0,
      stateChanged: false,
      readyForMaintenanceWindow:
        before.accessTokenExpiresAtMs > nowMs + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS,
    };
  }

  const request: CodexCommandRequest = {
    executable: input.executable ?? "codex",
    args: codexAuthMaintenanceArgs(input.workspace),
    cwd: input.workspace,
    repositoryRoot: input.workspace,
    workspaceWritable: false,
    env: {
      CODEX_HOME: input.slotDirectory,
      HOME: input.slotDirectory,
      PATH: maintenancePath(),
      CI: "true",
      LANG: "C.UTF-8",
      LC_ALL: "C.UTF-8",
      NO_COLOR: "1",
      TERM: "dumb",
    },
    clearEnv: true,
    stdin: FIXED_MAINTENANCE_PROMPT,
    outputLimitBytes: MAINTENANCE_OUTPUT_LIMIT_BYTES,
    timeoutMs: SENTINEL_CODEX_AUTH_MAINTENANCE_TIMEOUT_MS,
  };
  const command = await (dependencies.runCommand ??
    ((request: CodexCommandRequest) => runCodexCommandWithRuntime(request, directCodexRuntime)))(request);

  // Always re-read the complete file, including after a nonzero Codex exit.
  // Codex may have durably rotated the refresh token before a later inference
  // failure, and reconstructing only known fields would destroy that state.
  const afterRaw = await readTextFile(authPath);
  if (new TextEncoder().encode(afterRaw).byteLength > AUTH_DOCUMENT_MAX_BYTES) {
    throw new Error(`Sentinel Codex auth slot ${input.slot} document is too large after maintenance`);
  }
  const after = parseRawAuth(afterRaw, input.slot);
  if (after.tokens.account_id !== before.tokens.account_id) {
    throw new Error(`Sentinel Codex auth slot ${input.slot} changed account identity during maintenance`);
  }
  return {
    slot: input.slot,
    due: true,
    invoked: true,
    commandCode: command.code,
    timedOut: command.timedOut,
    outputExceeded: command.outputExceeded,
    stdoutBytes: command.stdoutBytes,
    stderrBytes: command.stderrBytes,
    stateChanged: afterRaw !== beforeRaw,
    readyForMaintenanceWindow: after.accessTokenExpiresAtMs > nowMs + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS,
  };
};

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required`);
  return value;
};

const runCli = async (): Promise<void> => {
  if (Deno.args.length !== 1 || Deno.args[0] !== "maintain") {
    throw new Error("Usage: auth-maintenance.ts maintain");
  }
  const runnerTemp = requiredEnvironment("RUNNER_TEMP");
  const stateDirectory = requiredEnvironment("SENTINEL_CODEX_AUTH_STATE_DIR");
  if (!runnerTemp.startsWith("/") || stateDirectory !== `${runnerTemp}/sentinel-codex-auth-state`) {
    throw new Error("Sentinel Codex auth maintenance received an invalid state path");
  }
  const workspace = `${runnerTemp}/sentinel-codex-auth-maintenance-workspace`;
  await Deno.mkdir(workspace, { recursive: true, mode: 0o700 });
  await Deno.chmod(workspace, 0o700);
  const dispositions: CodexAuthMaintenanceDisposition[] = [];
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
    dispositions.push(await maintainCodexAuthSlot({ slot, slotDirectory, workspace }));
  }
  if (dispositions.length === 0) throw new Error("No Sentinel Codex auth slots are configured");
  // This contains only categorical and numeric process metadata. Token values,
  // raw output, and auth documents are intentionally never printed.
  console.log(JSON.stringify({ schema_version: 1, slots: dispositions }));
};

if (import.meta.main) await runCli();
