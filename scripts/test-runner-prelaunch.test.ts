import assert from "node:assert/strict";
import { test } from "bun:test";
import { realpathSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { groupsFor, testFileArgv, testFileSupportsPlatform } from "./test-runner.ts";
import { liveDestination } from "./test-network.ts";

const controller = realpathSync(fileURLToPath(new URL("../tests/private-bun-copy-confinement.test.ts", import.meta.url)));
const ordinary = realpathSync(fileURLToPath(new URL("../tests/private-bun-copy.test.ts", import.meta.url)));
const command = [process.execPath, "--no-env-file", "--no-install", "test", controller];
const wrapped = (argv: readonly string[]): string[] => ["OS_BOUNDARY", ...argv];

test("only the exact canonical Darwin private-copy controller gets the finite first-profile launch", () => {
  assert.deepEqual(testFileArgv(controller, command, {}, "darwin", wrapped), command);
  assert.deepEqual(testFileArgv(controller, command, {}, "linux", wrapped), wrapped(command));
  assert.deepEqual(testFileArgv(ordinary, command, {}, "darwin", wrapped), wrapped(command));
  for (const suffix of [".other", "-neighbor", "/nested"]) {
    assert.throws(() => testFileArgv(controller + suffix, command, {}, "darwin", wrapped), /ENOENT|ENOTDIR/);
  }
  assert.deepEqual(testFileArgv(controller.replace("/tests/", "/tests/../tests/"), command, {}, "darwin", wrapped), wrapped(command));
});

test("a live-policy request cannot opt the trusted controller out of its network boundary", () => {
  const policy = { live: liveDestination("http://127.0.0.1:12345") };
  assert.deepEqual(testFileArgv(controller, command, policy, "darwin", wrapped), wrapped(command));
});

test("the affected Darwin segment retains real private-copy execution without widening named files", () => {
  const path = "tests/private-bun-copy-confinement.test.ts";
  const selection = { paths: [path], documentationOnly: false, affectedTests: [path], runAll: false, reason: "selected" as const };
  assert.equal(testFileSupportsPlatform(path, "linux"), false);
  assert.equal(testFileSupportsPlatform(path, "darwin"), true);
  assert.deepEqual(groupsFor("darwin", [], "linux", selection), []);
  assert.deepEqual(
    groupsFor("darwin", [], "darwin", selection).flatMap((group) => group.files),
    [path]
  );
  assert.deepEqual(
    groupsFor("main", ["tests/gpg-dsh.test.ts"], "darwin", selection).flatMap((group) => group.files),
    ["tests/gpg-dsh.test.ts"]
  );
  assert.equal(
    groupsFor("main", [], "linux", selection).some((group) => group.files.includes(path)),
    false
  );
});
