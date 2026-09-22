// Run from the VPS repository root after the candidate's required CI has passed.
const canonicalRoot = "/home/codex/repos/ubiquity/ai.ubq.fi";

async function command(program: string, args: string[]): Promise<string> {
  const result = await new Deno.Command(program, { args, stdout: "piped", stderr: "piped" }).output();
  if (!result.success) throw new Error(`${program} failed: ${new TextDecoder().decode(result.stderr)}`);
  return new TextDecoder().decode(result.stdout).trim();
}

/**
 * Read-only preflight for the checkout that would be released: tracked-clean,
 * branch, full revision, and equality with the `origin/development`
 * remote-tracking ref. It runs once before `.data` is touched and again after
 * the exclusive deployment lock is acquired, because a queued deployment can
 * wait while the checkout changes underneath it. The returned SHA is the
 * candidate identity and is only trusted from the post-lock call.
 */
async function assertDeployableCheckout(): Promise<string> {
  if (await command("git", ["status", "--porcelain", "--untracked-files=no"])) {
    throw new Error("Commit or preserve tracked changes before deployment");
  }
  const branch = await command("git", ["branch", "--show-current"]);
  if (branch !== "development") throw new Error("Production deployment is allowed only from the development branch");
  const sha = await command("git", ["rev-parse", "HEAD"]);
  if (!/^[0-9a-f]{40}$/.test(sha)) throw new Error("A full Git revision is required");
  // Resolve the remote-tracking ref by its full name: a local branch named
  // `origin/development` would otherwise shadow it, and a missing ref must fail
  // closed instead of comparing HEAD against some other revision.
  const remoteDevelopmentSha = await command("git", ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/development"]).catch((error: unknown) => {
    throw new Error("The origin/development tracking ref is missing; run `git fetch origin development` before deployment", { cause: error });
  });
  if (remoteDevelopmentSha !== sha) throw new Error("Checkout must exactly match origin/development before deployment");
  return sha;
}

/**
 * The repository `ops/` directory is bind-mounted read-only into Caddy, which
 * runs as the unprivileged `caddy` user. If the proxy configuration is
 * unreadable to that user, Caddy's own `ExecStartPre` validation fails and a
 * reload silently keeps the previous configuration serving while the gateway
 * has already moved. Prove the composed proxy config is valid as Caddy before
 * the service is restarted, and fail closed otherwise; never repair the
 * checkout mode, which would hide the real permission fault.
 */
async function ensureCaddyIngressReady(): Promise<void> {
  const mode = (await Deno.stat("ops/Caddyfile")).mode ?? 0;
  // Caddy shares no group with this checkout, so only other-read exposes the
  // bind-mounted file to it.
  if ((mode & 0o004) === 0) {
    throw new Error("ops/Caddyfile is not readable by the caddy user; run `chmod 644 ops/Caddyfile` and redeploy");
  }
  // Validate inside Caddy's own unit namespace so the bind mount and the
  // unprivileged user are both exercised, exactly as the service start is.
  const mainPid = await command("sudo", ["-n", "systemctl", "show", "caddy", "-p", "MainPID", "--value"]);
  if (!/^\d+$/.test(mainPid) || mainPid === "0") throw new Error("Caddy is not running; start it before deploying");
  await command("sudo", [
    "-n",
    "nsenter",
    "-t",
    mainPid,
    "-m",
    "--",
    "sudo",
    "-u",
    "caddy",
    "caddy",
    "validate",
    "--config",
    "/etc/caddy/Caddyfile",
    "--adapter",
    "caddyfile",
  ]);
  console.log(JSON.stringify({ ingress_preflight: "caddy_config_valid", caddy_pid: mainPid }));
}

let lock: Deno.FsFile | undefined;
try {
  // The canonical-root boundary stays before any Git subprocess: running Git in
  // an arbitrary checkout can execute configured helpers, so an unvalidated
  // directory is rejected first. The read-only checkout preflight then runs
  // before `.data` exists, and again under the deployment lock before the
  // candidate SHA is used.
  const root = await Deno.realPath(".");
  if (root !== canonicalRoot) throw new Error("Run from the canonical VPS repository root");
  await assertDeployableCheckout();
  await Deno.mkdir(".data/releases", { recursive: true, mode: 0o700 });
  lock = await Deno.open(".data/deploy.lock", { create: true, write: true, mode: 0o600 });
  await lock.lock(true);
  // Capture the candidate only after the lock: a deployment that waited for it
  // must not release a revision from before the wait.
  const sha = await assertDeployableCheckout();
  const release = `.data/releases/${sha}`;
  try {
    await Deno.stat(release);
    throw new Error("This release already exists; use systemctl restart to restart the installed release");
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
  const staging = await Deno.makeTempDir({ dir: ".data/releases", prefix: ".staging-" });
  const archive = `${staging}.tar`;
  await command("git", ["archive", "--format=tar", `--output=${archive}`, sha]);
  await command("tar", ["-xf", archive, "-C", staging]);
  await Deno.writeTextFile(`${staging}/src/release.ts`, `// Generated for this immutable VPS release.\nexport const RELEASE_GIT_SHA = "${sha}";\n`);
  const archiveDigest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", await Deno.readFile(archive))))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
  await Deno.writeTextFile(`${staging}/.uos-release.json`, JSON.stringify({ git_sha: sha, source_archive_sha256: archiveDigest }) + "\n");
  await Deno.rename(staging, release);
  await Deno.remove(archive);
  const next = `.data/current-${crypto.randomUUID()}`;
  await Deno.symlink(`releases/${sha}`, next);
  await Deno.rename(next, ".data/current");
  await ensureCaddyIngressReady();
  // Repository-owned unit files are linked from `/etc/systemd/system`, so the
  // checkout update above changes their content, but systemd keeps the
  // previously loaded definition until `daemon-reload`. Reload before the
  // restart so changes to `ExecStart`, environment, or sandbox settings are
  // active for the run the health check validates.
  await command("sudo", ["-n", "systemctl", "daemon-reload"]);
  console.log(JSON.stringify({ systemd_daemon_reload: "before-restart", service: "ai-ubq-fi.service" }));
  await command("sudo", ["-n", "systemctl", "restart", "ai-ubq-fi.service"]);

  for (let attempt = 0; attempt < 30; attempt++) {
    try {
      const response = await fetch("http://127.0.0.1:7999/health", { signal: AbortSignal.timeout(2000) });
      const health = await response.json();
      if (
        response.status === 200 &&
        health.release?.git_sha === sha &&
        health.release?.deployment_id === `vps-${sha}` &&
        response.headers.get("x-uos-git-sha") === sha &&
        response.headers.get("x-uos-deployment-id") === `vps-${sha}`
      ) {
        console.log(JSON.stringify({ git_sha: sha, deployment_id: `vps-${sha}`, release, health_verified: true }));
        Deno.exit(0);
      }
    } catch {
      /* The listener may still be starting. */
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw new Error("The VPS did not serve the expected release; inspect journalctl -u ai-ubq-fi.service");
} finally {
  lock?.close();
}
