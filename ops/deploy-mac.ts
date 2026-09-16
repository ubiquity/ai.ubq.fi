// Run from the canonical Mac checkout after committing the candidate.
if (Deno.build.os !== "darwin") throw new Error("This deployment requires macOS");
const root = await Deno.realPath(".");
if (root !== "/Users/nv/repos/ubiquity/ai.ubq.fi") throw new Error("Run from the canonical Mac repository root");
await Deno.mkdir(".data/releases", { recursive: true, mode: 0o700 });
const lock = await Deno.open(".data/deploy.lock", { create: true, write: true, mode: 0o600 });
await lock.lock(true);
async function command(program: string, args: string[]): Promise<string> {
  const result = await new Deno.Command(program, { args, stdout: "piped", stderr: "piped" }).output();
  if (!result.success) throw new Error(`${program} failed: ${new TextDecoder().decode(result.stderr)}`);
  return new TextDecoder().decode(result.stdout).trim();
}
/**
 * Registers the scheduled Codex auth repair agent. The plist is symlinked from
 * the checkout so its command stays reviewable in Git, and `launchctl` needs an
 * explicit bootout because it caches the plist content at bootstrap time.
 */
async function installRepairAgent(domain: string, repositoryRoot: string): Promise<void> {
  const label = "com.ubiquity.ai.codex-auth-repair";
  const link = `/Users/nv/Library/LaunchAgents/${label}.plist`;
  const source = `${repositoryRoot}/ops/${label}.plist`;
  try {
    const target = await Deno.readLink(link);
    if (target !== source) throw new Error("An unrelated launch agent owns this path");
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
    await Deno.symlink(source, link);
  }
  const service = `${domain}/${label}`;
  const registered = await new Deno.Command("launchctl", { args: ["print", service], stdout: "null", stderr: "null" }).output();
  if (registered.success) await command("launchctl", ["bootout", service]);
  await command("launchctl", ["bootstrap", domain, link]);
}
try {
  if (await command("git", ["status", "--porcelain", "--untracked-files=no"])) {
    throw new Error("Preserve tracked changes before deployment");
  }
  const sha = await command("git", ["rev-parse", "HEAD"]);
  if (!/^[0-9a-f]{40}$/.test(sha)) throw new Error("A full Git revision is required");
  const release = `.data/releases/${sha}`;
  try {
    await Deno.stat(release);
    throw new Error("This release already exists; use launchctl to restart the current service");
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
  const staging = await Deno.makeTempDir({ dir: ".data/releases", prefix: ".staging-" });
  const archive = `${staging}.tar`;
  await command("git", ["archive", "--format=tar", `--output=${archive}`, sha]);
  await command("tar", ["-xf", archive, "-C", staging]);
  await Deno.writeTextFile(`${staging}/src/release.ts`, `export const RELEASE_GIT_SHA = "${sha}";\n`);
  await Deno.rename(staging, release);
  await Deno.remove(archive);
  const next = `.data/current-${crypto.randomUUID()}`;
  await Deno.symlink(`releases/${sha}`, next);
  await Deno.rename(next, ".data/current");
  const domain = `gui/${await command("id", ["-u"])}`;
  const service = `${domain}/com.ubiquity.ai.local`;
  const link = "/Users/nv/Library/LaunchAgents/com.ubiquity.ai.local.plist";
  try {
    const target = await Deno.readLink(link);
    if (target !== `${root}/ops/com.ubiquity.ai.local.plist`) {
      throw new Error("An unrelated launch agent owns this path");
    }
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
    await Deno.symlink(`${root}/ops/com.ubiquity.ai.local.plist`, link);
  }
  const registration = await new Deno.Command("launchctl", {
    args: ["print", service],
    stdout: "piped",
    stderr: "null",
  }).output();
  if (registration.success) {
    const oldPid = /\n[^\S\n]*pid = (\d+)/.exec(new TextDecoder().decode(registration.stdout))?.[1];
    await command("launchctl", ["bootout", service]);
    // bootout returns before a draining process exits. Wait before registering its replacement.
    if (oldPid) {
      const deadline = Date.now() + 185_000;
      while ((await new Deno.Command("ps", { args: ["-p", oldPid, "-o", "pid="], stdout: "null" }).output()).success) {
        if (Date.now() >= deadline) throw new Error(`The previous Mac service process ${oldPid} did not exit`);
        await new Promise((resolve) => setTimeout(resolve, 100));
      }
    }
  }
  await command("launchctl", ["bootstrap", domain, link]);
  let verified = false;
  for (let attempt = 0; attempt < 30; attempt++) {
    try {
      const response = await fetch("http://127.0.0.1:8000/health", { signal: AbortSignal.timeout(2000) });
      const body = await response.json();
      if (
        response.status === 200 &&
        body.release?.git_sha === sha &&
        body.release?.deployment_id === `mac-${sha}` &&
        response.headers.get("x-uos-git-sha") === sha &&
        response.headers.get("x-uos-deployment-id") === `mac-${sha}`
      ) {
        verified = true;
        break;
      }
    } catch {
      /* The listener is starting. */
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  if (!verified) throw new Error("The Mac service did not serve the expected release; inspect .data/mac.stderr.log");
  // The repair agent is a scheduled companion, so a registration problem must
  // not turn a verified service deployment into a failed one. The result is
  // reported, and the next deployment retries the registration.
  let repairAgent = "installed";
  try {
    await installRepairAgent(domain, root);
  } catch (error) {
    repairAgent = "failed";
    console.error(`[deploy-mac] Codex auth repair agent was not registered: ${error instanceof Error ? error.message : String(error)}`);
  }
  console.log(JSON.stringify({ git_sha: sha, deployment_id: `mac-${sha}`, health_verified: true, repair_agent: repairAgent }));
  Deno.exit(0);
} finally {
  lock.close();
}
