import { test } from "bun:test";
import assert from "node:assert/strict";
import { chmod, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { superviseNativeOwner } from "../launch-native-owner.ts";
import { runFixture, spawnFixture } from "./fixture-process.ts";

test("child receives only stdin and minimal environment and its exit is preserved", async () => {
  const root = await mkdtemp(join(tmpdir(), "native-child-"));
  const binary = join(root, "child");
  const input = join(root, "input");
  const environment = join(root, "environment");
  await writeFile(binary, '#!/bin/sh\n/usr/bin/cat > "' + input + '"\n/usr/bin/env > "' + environment + '"\nexit 7\n', { mode: 0o500 });
  try {
    assert.equal(await superviseNativeOwner(binary, root, "SYNTHETIC_PRIVATE_STDIN"), 7);
    assert.equal(await readFile(input, "utf8"), "SYNTHETIC_PRIVATE_STDIN");
    const env = await readFile(environment, "utf8");
    assert(env.includes("PATH=/usr/bin:/bin"));
    assert(!env.includes("UOS_NATIVE_CARRIER"));
    assert(!env.includes("API_KEY"));
    assert(!env.includes("SYNTHETIC_PRIVATE_STDIN"));
  } finally {
    await chmod(binary, 0o700);
    await rm(root, { recursive: true });
  }
});
test("owned signal is forwarded and child exit is propagated without a restart loop", async () => {
  const root = await mkdtemp(join(tmpdir(), "native-signal-"));
  const binary = join(root, "child");
  const runner = join(root, "runner.ts");
  await writeFile(
    binary,
    '#!/bin/sh\n/usr/bin/sleep 30 & pending=$!\ntrap \'kill "$pending" 2>/dev/null || :; wait "$pending" 2>/dev/null || :; exit 23\' TERM\nprintf "READY\\n"\nwait "$pending"\n',
    { mode: 0o500 }
  );
  const module = new URL("../launch-native-owner.ts", import.meta.url).href;
  await writeFile(
    runner,
    "import { superviseNativeOwner } from " +
      JSON.stringify(module) +
      ";\nprocess.exitCode=await superviseNativeOwner(" +
      JSON.stringify(binary) +
      "," +
      JSON.stringify(root) +
      ',"");\n'
  );
  const child = spawnFixture(process.execPath, { args: ["--no-env-file", "--no-install", runner], stdin: "piped" });
  try {
    await child.stdin.end();
    const reader = child.stdout.getReader();
    let ready = "";
    while (!ready.includes("READY")) {
      const value = await reader.read();
      assert(!value.done);
      ready += new TextDecoder().decode(value.value);
    }
    reader.releaseLock();
    child.kill("SIGTERM");
    assert.equal(await child.exited, 23);
  } finally {
    if (child.exitCode === null) {
      child.kill("SIGKILL");
      await child.exited;
    }
    await rm(root, { recursive: true });
  }
});
test("bootstrap refuses missing selector before any child or dotenv discovery", async () => {
  const root = await mkdtemp(join(tmpdir(), "native-bootstrap-"));
  const script = new URL("../launch-native-owner.sh", import.meta.url).pathname;
  try {
    const output = await runFixture("/bin/sh", { args: [script], cwd: root });
    assert.notEqual(output.code, 0);
    assert.equal(output.stdout.length, 0);
  } finally {
    await rm(root, { recursive: true });
  }
});
test("unit-specific same-name mask bounds only this service and preserves socket visibility", async () => {
  const service = await readFile(new URL("../uos-native-owner.service", import.meta.url), "utf8");
  const limits = await readFile(new URL("../uos-native-owner.service.d/99-unlimited-resources.conf", import.meta.url), "utf8");
  assert(service.includes("PrivateTmp=false"));
  assert(service.includes("Restart=on-failure"));
  assert(!service.includes("WatchdogSec"));
  assert(!service.includes("EnvironmentFile"));
  assert(service.includes("ProtectHome=read-only"));
  for (const value of ["MemoryMax=256M", "MemorySwapMax=64M", "TasksMax=96", "CPUQuota=50%"]) assert(limits.includes(value));
  const source = await readFile(new URL("../launch-native-owner.ts", import.meta.url), "utf8");
  assert(source.includes("loadEnvFile(environmentFile)"));
  assert(source.includes("await verifyCgroup()"));
  assert(!source.includes("setInterval("));
});
