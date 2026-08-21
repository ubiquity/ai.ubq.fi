export type CommandResult = Readonly<{
  code: number;
  stdout: Uint8Array<ArrayBuffer>;
  stderr: Uint8Array<ArrayBuffer>;
}>;

const TRUSTED_GIT_CONFIGURATION = Object.freeze([
  "core.hooksPath=/dev/null",
  "core.fsmonitor=false",
  "core.sshCommand=/bin/false",
  "credential.helper=",
  "http.proxy=",
  "protocol.ext.allow=never",
  "commit.gpgSign=false",
  "user.name=Provider Sentinel",
  "user.email=sentinel@users.noreply.github.com",
]);

export const trustedGitArguments = (args: readonly string[]): string[] => [
  ...TRUSTED_GIT_CONFIGURATION.flatMap((configuration) => ["-c", configuration]),
  ...args,
];

const readEnvironment = (name: string): string | undefined => {
  try {
    return Deno.env.get(name);
  } catch {
    return undefined;
  }
};

const commandHome = `${
  readEnvironment("RUNNER_TEMP") ?? readEnvironment("TMPDIR") ?? "/tmp"
}/uos-provider-sentinel-${Deno.pid}`;

const minimalCommandEnvironment = async (
  overrides: Readonly<Record<string, string>> = {},
): Promise<Record<string, string>> => {
  await Deno.mkdir(commandHome, { recursive: true, mode: 0o700 });
  return {
    HOME: commandHome,
    PATH: readEnvironment("PATH") ?? "/usr/local/bin:/usr/bin:/bin",
    CI: "true",
    LANG: "C.UTF-8",
    LC_ALL: "C.UTF-8",
    NO_COLOR: "1",
    TERM: "dumb",
    ...overrides,
  };
};

const concat = (left: Uint8Array, right: Uint8Array): Uint8Array<ArrayBuffer> => {
  const output = new Uint8Array(left.byteLength + right.byteLength);
  output.set(left);
  output.set(right, left.byteLength);
  return output;
};

const collectBounded = async (
  stream: ReadableStream<Uint8Array>,
  maximumBytes: number,
): Promise<Uint8Array<ArrayBuffer>> => {
  let output = new Uint8Array();
  for await (const chunk of stream) {
    if (output.byteLength + chunk.byteLength > maximumBytes) {
      throw new Error("A Sentinel subprocess exceeded its output limit");
    }
    output = concat(output, chunk);
  }
  return output;
};

export const runCommand = async (
  input: Readonly<{
    command: string;
    args: readonly string[];
    cwd: string;
    env?: Readonly<Record<string, string>>;
    stdin?: Uint8Array<ArrayBuffer>;
    maximumOutputBytes?: number;
  }>,
): Promise<CommandResult> => {
  const child = new Deno.Command(input.command, {
    args: [...input.args],
    cwd: input.cwd,
    env: await minimalCommandEnvironment(input.env),
    clearEnv: true,
    stdin: input.stdin ? "piped" : "null",
    stdout: "piped",
    stderr: "piped",
  }).spawn();
  if (input.stdin) {
    const writer = child.stdin.getWriter();
    await writer.write(input.stdin);
    await writer.close();
  }
  const maximum = input.maximumOutputBytes ?? 16 * 1024 * 1024;
  const [stdout, stderr, status] = await Promise.all([
    collectBounded(child.stdout, maximum),
    collectBounded(child.stderr, maximum),
    child.status,
  ]);
  return { code: status.code, stdout, stderr };
};

export const runChecked = async (input: Parameters<typeof runCommand>[0]): Promise<CommandResult> => {
  const result = await runCommand(input);
  if (result.code !== 0) {
    throw new Error(`${input.command} failed with exit code ${result.code}`);
  }
  return result;
};

export const runTrustedGit = (
  input: Omit<Parameters<typeof runCommand>[0], "command" | "args"> & Readonly<{ args: readonly string[] }>,
): Promise<CommandResult> => runChecked({ ...input, command: "git", args: trustedGitArguments(input.args) });

export const runTrustedGitUnchecked = (
  input: Omit<Parameters<typeof runCommand>[0], "command" | "args"> & Readonly<{ args: readonly string[] }>,
): Promise<CommandResult> => runCommand({ ...input, command: "git", args: trustedGitArguments(input.args) });

export const captureRawDenoLogs = async (
  input: Readonly<{
    cwd: string;
    token: string;
    organization: string;
    app: string;
    start: string;
    end: string;
    destination: string;
    executable?: string;
    timeoutMs?: number;
  }>,
): Promise<void> => {
  if (!input.token) throw new Error("DENO_DEPLOY_TOKEN is required to capture raw logs");
  const child = new Deno.Command(input.executable ?? "deno", {
    cwd: input.cwd,
    args: [
      "deploy",
      "logs",
      "--json",
      "--non-interactive",
      "--once",
      "--org",
      input.organization,
      "--app",
      input.app,
      "--start",
      input.start,
      "--end",
      input.end,
    ],
    env: await minimalCommandEnvironment({ DENO_DEPLOY_TOKEN: input.token }),
    clearEnv: true,
    signal: AbortSignal.timeout(input.timeoutMs ?? 10 * 60_000),
    stdout: "piped",
    stderr: "piped",
  }).spawn();
  const file = await Deno.open(input.destination, { create: true, truncate: true, write: true, mode: 0o600 });
  let stderr = new Uint8Array();
  try {
    const stderrPromise = collectBounded(child.stderr, 1024 * 1024);
    await child.stdout.pipeTo(file.writable);
    const status = await child.status;
    stderr = await stderrPromise;
    if (!status.success) throw new Error(`Deno raw log capture failed with exit code ${status.code}`);
  } finally {
    try {
      file.close();
    } catch {
      // pipeTo closes the file writable on success.
    }
    stderr.fill(0);
  }
};

const decode = (value: Uint8Array): string => new TextDecoder().decode(value).trim();

const changedFiles = async (cwd: string): Promise<string[]> => {
  const result = await runTrustedGit({
    args: ["diff", "--no-ext-diff", "--no-textconv", "--name-only", "--diff-filter=ACMR", "origin/development...HEAD"],
    cwd,
  });
  return decode(result.stdout).split("\n").map((value) => value.trim()).filter(Boolean);
};

const runValidationCommand = async (
  cwd: string,
  command: string,
  args: readonly string[],
  sandboxHome: string,
  denoDirectory: string,
  report: Array<Record<string, unknown>>,
): Promise<void> => {
  const started = Date.now();
  if (Deno.build.os !== "linux") {
    throw new Error("Candidate validation requires the Linux bubblewrap network sandbox");
  }
  const executable = command === "deno" ? Deno.execPath() : command;
  const sandboxArgs = [
    "--die-with-parent",
    "--new-session",
    "--unshare-net",
    "--unshare-pid",
    "--unshare-ipc",
    "--unshare-uts",
    "--ro-bind",
    "/",
    "/",
    "--dev",
    "/dev",
    "--proc",
    "/proc",
    "--tmpfs",
    "/tmp",
    "--bind",
    sandboxHome,
    sandboxHome,
    "--bind",
    denoDirectory,
    denoDirectory,
    "--chdir",
    cwd,
    "--",
    executable,
    ...args,
  ];
  const result = await runCommand({
    command: "bwrap",
    args: sandboxArgs,
    cwd,
    env: { HOME: sandboxHome, DENO_DIR: denoDirectory },
    maximumOutputBytes: 32 * 1024 * 1024,
  });
  report.push({ command: [command, ...args], exit_code: result.code, duration_ms: Date.now() - started });
  if (result.code !== 0) throw new Error(`${command} validation failed with exit code ${result.code}`);
};

export const runCandidateValidation = async (
  input: Readonly<{
    cwd: string;
    reportPath: string;
    privateDir: string;
    denoDirectory: string;
  }>,
): Promise<void> => {
  if (!input.denoDirectory.startsWith("/") || !input.privateDir.startsWith("/")) {
    throw new Error("Candidate validation paths must be absolute");
  }
  const files = await changedFiles(input.cwd);
  const report: Array<Record<string, unknown>> = [];
  const formattable = files.filter((file) => /\.(?:ts|tsx|js|jsx|json|jsonc|md|ya?ml)$/.test(file));
  const lintable = files.filter((file) => /\.(?:ts|tsx|js|jsx)$/.test(file));
  const sandboxHome = await Deno.makeTempDir({ dir: input.privateDir, prefix: "validation-home-" });
  await Deno.chmod(sandboxHome, 0o700);
  let failure: unknown = null;
  try {
    if (formattable.length) {
      await runValidationCommand(
        input.cwd,
        "deno",
        ["fmt", "--check", ...formattable],
        sandboxHome,
        input.denoDirectory,
        report,
      );
    }
    if (lintable.length) {
      await runValidationCommand(
        input.cwd,
        "deno",
        ["lint", ...lintable],
        sandboxHome,
        input.denoDirectory,
        report,
      );
    }
    await runValidationCommand(
      input.cwd,
      "deno",
      [
        "check",
        "--frozen",
        "--no-remote",
        "serve.ts",
        "scripts/setup-instance.ts",
        "scripts/sentinel/artifact-crypto.ts",
        "scripts/sentinel/encrypt-artifacts.ts",
        "scripts/sentinel/main.ts",
        "scripts/sentinel/revision-control.ts",
      ],
      sandboxHome,
      input.denoDirectory,
      report,
    );
    await runValidationCommand(
      input.cwd,
      "deno",
      [
        "test",
        "--cached-only",
        "--allow-read",
        "--allow-write",
        "--allow-run",
        "tests/sentinel-artifact-crypto.test.ts",
        "tests/sentinel-artifact-integration.test.ts",
      ],
      sandboxHome,
      input.denoDirectory,
      report,
    );
    await runValidationCommand(
      input.cwd,
      "deno",
      [
        "test",
        "--cached-only",
        "--allow-net=127.0.0.1",
        "--allow-env=CEREBRAS_API_KEY,VOYAGEAI_API_KEY,METERED_API_KEY,SURPLUS_API_KEY,GIT_REVISION,GITHUB_SHA,DENO_DEPLOYMENT_ID,DENO_DEPLOY_BUILD_ID",
        "--ignore=lib/codex",
        "--ignore=tests/failover-stress-http.test.ts",
        "--ignore=tests/usage-optimization-measurement.test.ts",
      ],
      sandboxHome,
      input.denoDirectory,
      report,
    );
    await runValidationCommand(
      input.cwd,
      "deno",
      [
        "test",
        "--cached-only",
        "--allow-net=127.0.0.1",
        "--allow-env=UOS_AI_TOKEN,DENO_DEPLOY_TOKEN",
        "tests/usage-optimization-measurement.test.ts",
      ],
      sandboxHome,
      input.denoDirectory,
      report,
    );
    await runValidationCommand(
      input.cwd,
      "deno",
      [
        "test",
        "--cached-only",
        "--allow-net=127.0.0.1",
        "--allow-env=GIT_REVISION,GITHUB_SHA,DENO_DEPLOYMENT_ID,DENO_DEPLOY_BUILD_ID",
        "tests/serve-delivery-http.test.ts",
        "tests/responses-stream.test.ts",
        "tests/responses-failover-stream.test.ts",
      ],
      sandboxHome,
      input.denoDirectory,
      report,
    );
  } catch (error) {
    failure = error;
  } finally {
    await Deno.writeTextFile(
      input.reportPath,
      `${JSON.stringify({ files, commands: report, passed: failure === null }, null, 2)}\n`,
      { mode: 0o600 },
    );
    await Deno.remove(sandboxHome, { recursive: true }).catch(() => undefined);
  }
  if (failure !== null) throw failure;
};

export const scanCandidateWithGitleaks = async (
  input: Readonly<{
    cwd: string;
    reportPath: string;
  }>,
): Promise<void> => {
  const checks: Array<Record<string, unknown>> = [];
  let failure: unknown = null;
  try {
    for (
      const args of [
        ["dir", ".", "--redact", "--no-banner", "--exit-code", "1"],
        ["git", ".", "--redact", "--no-banner", "--log-opts=origin/development..HEAD", "--exit-code", "1"],
        ["git", ".", "--redact", "--no-banner", "--log-opts=--all", "--exit-code", "1"],
      ]
    ) {
      const result = await runCommand({
        command: "gitleaks",
        args,
        cwd: input.cwd,
        maximumOutputBytes: 8 * 1024 * 1024,
      });
      checks.push({ args, exit_code: result.code });
      if (result.code !== 0) throw new Error("Gitleaks rejected the candidate or Git history");
    }
  } catch (error) {
    failure = error;
  } finally {
    await Deno.writeTextFile(
      input.reportPath,
      `${JSON.stringify({ checks, passed: failure === null }, null, 2)}\n`,
      { mode: 0o600 },
    );
  }
  if (failure !== null) throw failure;
};

const containsPattern = (haystack: Uint8Array, needle: Uint8Array): boolean => {
  if (needle.byteLength === 0 || needle.byteLength > haystack.byteLength) return false;
  outer:
  for (let offset = 0; offset <= haystack.byteLength - needle.byteLength; offset++) {
    for (let index = 0; index < needle.byteLength; index++) {
      if (haystack[offset + index] !== needle[index]) continue outer;
    }
    return true;
  }
  return false;
};

export const assertGitHistoryExcludesValues = async (
  input: Readonly<{
    cwd: string;
    sensitiveValues: readonly string[];
  }>,
): Promise<void> => {
  const patterns = input.sensitiveValues.filter((value) => value.length >= 8).map((value) =>
    new TextEncoder().encode(value)
  );
  if (patterns.length === 0) throw new Error("Secret scanning requires non-empty sensitive values");
  const child = new Deno.Command("git", {
    cwd: input.cwd,
    args: trustedGitArguments([
      "log",
      "-p",
      "--all",
      "--full-history",
      "--no-ext-diff",
      "--no-textconv",
      "--binary",
    ]),
    stdout: "piped",
    stderr: "null",
    env: await minimalCommandEnvironment(),
    clearEnv: true,
  }).spawn();
  const overlap = Math.max(...patterns.map((pattern) => pattern.byteLength)) - 1;
  let tail = new Uint8Array();
  let found = false;
  for await (const chunk of child.stdout) {
    const window = concat(tail, chunk);
    if (patterns.some((pattern) => containsPattern(window, pattern))) found = true;
    tail = window.slice(Math.max(0, window.byteLength - overlap));
  }
  const status = await child.status;
  patterns.forEach((pattern) => pattern.fill(0));
  tail.fill(0);
  if (!status.success) throw new Error("Git history secret scan could not inspect every revision");
  if (found) throw new Error("Credential material was found in reachable Git history");
};

export const hashProtectedFiles = async (
  cwd: string,
  paths: readonly string[],
): Promise<Readonly<Record<string, string>>> => {
  const hashes: Record<string, string> = {};
  for (const path of paths) {
    const result = await runTrustedGit({ args: ["hash-object", "--no-filters", path], cwd });
    hashes[path] = decode(result.stdout);
  }
  return hashes;
};

export const assertProtectedFilesUnchanged = async (
  cwd: string,
  expected: Readonly<Record<string, string>>,
): Promise<void> => {
  const actual = await hashProtectedFiles(cwd, Object.keys(expected));
  for (const [path, hash] of Object.entries(expected)) {
    if (actual[path] !== hash) throw new Error(`The implementation agent changed protected policy file ${path}`);
  }
};
