import { parseEnv } from "node:util";
import { readFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { runSteeringProbe } from "./steering-health/probe.ts";

const repositoryEnv = async (): Promise<string> => {
  const checkout = resolve(import.meta.dirname, "..");
  const common = Bun.spawnSync(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], { cwd: checkout, stderr: "pipe" });
  const repository = common.exitCode === 0 ? dirname(common.stdout.toString().trim()) : checkout;
  return await readFile(join(repository, ".env"), "utf8");
};

export const readToken = (text: string): string => {
  const value = parseEnv(text).UOS_AI_TOKEN;
  const token = value
    ?.split(/[\n,]/)
    .map((entry) => entry.trim())
    .find((entry) => entry.length > 0);
  if (!token) throw new Error("Repository-root .env lacks a nonempty UOS_AI_TOKEN");
  return token;
};

export const steeringHealthMain = async (args: string[]): Promise<Record<string, unknown>> => {
  try {
    if (Bun.version !== "1.4.0") throw new Error("This command requires pinned Bun 1.4.0");
    if (args.length > 1 || args[0]?.startsWith("-")) throw new Error("Usage: bun --no-env-file run steering:health [endpoint]");
    const endpoint = new URL(args[0] ?? "http://127.0.0.1:7999");
    if (
      !["http:", "https:"].includes(endpoint.protocol) ||
      endpoint.username ||
      endpoint.password ||
      endpoint.search ||
      endpoint.hash ||
      endpoint.pathname !== "/"
    ) {
      throw new Error("Endpoint must be an HTTP(S) origin without credentials");
    }
    const executable = Bun.which("codex");
    if (!executable) throw new Error("Install Codex and make codex available on PATH");
    const token = readToken(await repositoryEnv());
    return await runSteeringProbe({ endpoint, token, executable });
  } catch (error) {
    return { status: "failed", error: error instanceof Error ? error.message : "Steering health failed" };
  }
};

if (import.meta.main) {
  const receipt = await steeringHealthMain(process.argv.slice(2));
  process.stdout.write(JSON.stringify(receipt) + "\n");
  process.exitCode = receipt.status === "passed" ? 0 : 1;
}
