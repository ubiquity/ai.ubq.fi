// Run from the VPS repository root after the candidate's required CI has passed.
const root = await Deno.realPath(".");
if (root !== "/home/codex/repos/ubiquity/ai.ubq.fi") throw new Error("Run from the canonical VPS repository root");
await Deno.mkdir(".data/releases", { recursive: true, mode: 0o700 });
const lock = await Deno.open(".data/deploy.lock", { create: true, write: true, mode: 0o600 });
await lock.lock(true);

async function command(program: string, args: string[]): Promise<string> {
  const result = await new Deno.Command(program, { args, stdout: "piped", stderr: "piped" }).output();
  if (!result.success) throw new Error(`${program} failed: ${new TextDecoder().decode(result.stderr)}`);
  return new TextDecoder().decode(result.stdout).trim();
}

try {
  if (await command("git", ["status", "--porcelain", "--untracked-files=no"])) {
    throw new Error("Commit or preserve tracked changes before deployment");
  }
  const sha = await command("git", ["rev-parse", "HEAD"]);
  if (!/^[0-9a-f]{40}$/.test(sha)) throw new Error("A full Git revision is required");
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
  await Deno.writeTextFile(
    `${staging}/src/release.ts`,
    `// Generated for this immutable VPS release.\nexport const RELEASE_GIT_SHA = "${sha}";\n`,
  );
  const archiveDigest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", await Deno.readFile(archive))))
    .map((byte) => byte.toString(16).padStart(2, "0")).join("");
  await Deno.writeTextFile(
    `${staging}/.uos-release.json`,
    JSON.stringify({ git_sha: sha, source_archive_sha256: archiveDigest }) + "\n",
  );
  await Deno.rename(staging, release);
  await Deno.remove(archive);
  const next = `.data/current-${crypto.randomUUID()}`;
  await Deno.symlink(`releases/${sha}`, next);
  await Deno.rename(next, ".data/current");
  await command("sudo", ["-n", "systemctl", "restart", "ai-ubq-fi.service"]);

  for (let attempt = 0; attempt < 30; attempt++) {
    try {
      const response = await fetch("http://127.0.0.1:8001/health", { signal: AbortSignal.timeout(2000) });
      const health = await response.json();
      if (
        response.status === 200 && health.release?.git_sha === sha &&
        health.release?.deployment_id === `vps-${sha}` &&
        response.headers.get("x-uos-git-sha") === sha &&
        response.headers.get("x-uos-deployment-id") === `vps-${sha}`
      ) {
        console.log(JSON.stringify({ git_sha: sha, deployment_id: `vps-${sha}`, release, health_verified: true }));
        Deno.exit(0);
      }
    } catch { /* The listener may still be starting. */ }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw new Error("The VPS did not serve the expected release; inspect journalctl -u ai-ubq-fi.service");
} finally {
  lock.close();
}
