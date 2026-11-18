import { test } from "bun:test";
import assert from "node:assert/strict";
import { lstat, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { GEMINI_QUOTA_FILE, projectAntigravityStatusLine, type GeminiQuotaObservation } from "../src/admin/gemini-quota-snapshot.ts";

type Bucket = Extract<GeminiQuotaObservation, { status: "observed" }>["quota"][string];
type ResetCase = Readonly<{ name: string; previous: Bucket; current: Bucket; writes: boolean; age?: number }>;
const EXPORTER = fileURLToPath(new URL("../scripts/export-antigravity-quota.ts", import.meta.url));
const NOW = Date.parse("2026-11-18T12:00:00Z");
const RESET_AT = NOW + 7_200_000;
const absolute = (offset = 0): Bucket => ({ reset_time: new Date(RESET_AT + offset).toISOString() });
const countdown = (seconds = 7200): Bucket => ({ reset_in_seconds: seconds });
const cases: readonly ResetCase[] = [
  { name: "absolute to countdown", previous: absolute(), current: countdown(), writes: false },
  { name: "countdown to absolute", previous: countdown(7202), current: absolute(), writes: false },
  { name: "falling countdown", previous: countdown(7202), current: countdown(), writes: false },
  { name: "unchanged countdown with a later reset", previous: countdown(), current: countdown(), writes: true },
  { name: "absolute to countdown tolerance boundary", previous: absolute(), current: countdown(7201), writes: false },
  { name: "countdown to absolute tolerance boundary", previous: countdown(7202), current: absolute(1000), writes: false },
  { name: "absolute to changed countdown", previous: absolute(), current: countdown(7201.001), writes: true },
  { name: "countdown to changed absolute", previous: countdown(7202), current: absolute(1001), writes: true },
  { name: "missing reset", previous: absolute(), current: {}, writes: true },
  { name: "new reset", previous: {}, current: countdown(), writes: true },
  { name: "both resets missing", previous: {}, current: {}, writes: false },
  { name: "zero countdown", previous: absolute(-7_200_000), current: countdown(0), writes: false },
  { name: "absolute reset precedence", previous: absolute(), current: { ...absolute(), ...countdown(0) }, writes: false },
  { name: "changed remaining quota", previous: absolute(), current: { ...countdown(), remaining_fraction: 0.5 }, writes: true },
  { name: "changed disabled flag", previous: absolute(), current: { ...countdown(), disabled: true }, writes: true },
  { name: "inside throttle interval", previous: absolute(), current: countdown(), writes: false, age: 29_999 },
  { name: "expired throttle interval", previous: absolute(), current: countdown(), writes: true, age: 30_000 },
];

for (const { name, previous, current, writes, age = 2000 } of cases) {
  test(`Antigravity quota reset equivalence: ${name}`, async () => {
    const root = await mkdtemp(join(tmpdir(), "antigravity-quota-"));
    const directory = join(root, "quota");
    const clock = join(root, "clock.ts");
    try {
      await mkdir(directory, { mode: 0o700 });
      await writeFile(clock, `Date.now = () => ${NOW};\n`);
      const destination = join(directory, GEMINI_QUOTA_FILE);
      const snapshot = {
        version: 1,
        observed_at_ms: NOW - age,
        status: "observed",
        plan_tier: "Pro",
        quota: { weekly: { remaining_fraction: 0.25, ...previous } },
      };
      const original = JSON.stringify(snapshot) + "\n";
      await writeFile(destination, original, { mode: 0o600 });
      const inode = (await lstat(destination)).ino;
      const event = {
        product: "antigravity",
        email: "fixture@example.test",
        plan_tier: "Pro",
        quota: { weekly: { remaining_fraction: 0.25, ...current } },
      };
      const child = Bun.spawn([process.execPath, "--no-env-file", "--no-install", "--preload", clock, EXPORTER, directory], {
        stdin: new Blob([JSON.stringify(event)]),
        stdout: "pipe",
        stderr: "pipe",
      });
      const [exit, stdout, stderr] = await Promise.all([child.exited, new Response(child.stdout).text(), new Response(child.stderr).text()]);
      assert.deepEqual({ exit, stdout, stderr }, { exit: 0, stdout: "", stderr: "" });
      const result = await readFile(destination, "utf8");
      if (writes) {
        assert.deepEqual(JSON.parse(result), { version: 1, observed_at_ms: NOW, ...projectAntigravityStatusLine(event) });
        assert.notEqual((await lstat(destination)).ino, inode);
      } else {
        assert.equal(result, original);
        assert.equal((await lstat(destination)).ino, inode);
      }
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
}
