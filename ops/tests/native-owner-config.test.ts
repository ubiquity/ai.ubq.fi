import { test } from "bun:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { nativeOwnerStdin, parseNativeOwnerCarrier, parseNativeOwnerConfig } from "../native-owner-config.ts";
import { verifyNativeOwnerLimits } from "../launch-native-owner.ts";

const key = Buffer.alloc(32, 19);
export const ownerConfig = () => ({
  v: 1,
  release: { git_sha: "a".repeat(40), git_tree: "b".repeat(40), manifest_sha256: "c".repeat(64) },
  control: {
    key_id: "fixture-key",
    owner_id: "fixture-control",
    owner_epoch: "fixture-epoch",
    key_fingerprint: createHash("sha256").update(key).digest("hex"),
    owner_revision: "actual-operator-revision",
  },
  worker: {
    origin: "https://fixture-worker.example",
    git_sha: "a".repeat(40),
    git_tree: "b".repeat(40),
    namespace_id: "d".repeat(32),
    object_id: "e".repeat(64),
    receipt_sha256: "f".repeat(64),
  },
  owners: [
    { owner_id: "fixture-one", codex_home: "/fixture/one" },
    { owner_id: "fixture-two", codex_home: "/fixture/two" },
  ],
});
export const ownerCarrier = () => ({
  origin: "https://origin.ubq.fi",
  identity: {
    key_id: "fixture-key",
    owner_id: "fixture-control",
    owner_epoch: "fixture-epoch",
    key_base64url: key.toString("base64url"),
    capabilities: ["supervisor_read", "owner_renewal", "access_publish"],
  },
  renewal_owners: ownerConfig().owners,
});

test("exact carrier binding emits strict native v2 and no carrier capability aliases", () => {
  const input = JSON.parse(nativeOwnerStdin("/fixture/root", parseNativeOwnerConfig(ownerConfig()), parseNativeOwnerCarrier(ownerCarrier())));
  assert.equal(input.v, 2);
  assert.equal(input.listen, "127.0.0.1:7990");
  assert.equal(input.identity.key_base64url, key.toString("base64url"));
  assert.deepEqual(
    Object.keys(input.identity).sort((a, b) => a.localeCompare(b)),
    ["key_base64url", "key_id", "owner_epoch", "owner_id"]
  );
  assert.deepEqual(input.sources, []);
  assert.equal(input.owner_renewal.owners.length, 2);
  assert.equal(input.owner_renewal.authority_origin, "https://fixture-worker.example/");
});
test("control identity epoch fingerprint and both owner mappings must match", () => {
  for (const field of ["key_id", "owner_id", "owner_epoch", "key_fingerprint"] as const) {
    const cfg = ownerConfig();
    cfg.control[field] = field === "key_fingerprint" ? "0".repeat(64) : "wrong";
    assert.throws(() => nativeOwnerStdin("/fixture/root", parseNativeOwnerConfig(cfg), parseNativeOwnerCarrier(ownerCarrier())));
  }
  const cfg = ownerConfig();
  cfg.owners[1].owner_id = "wrong";
  assert.throws(() => nativeOwnerStdin("/fixture/root", parseNativeOwnerConfig(cfg), parseNativeOwnerCarrier(ownerCarrier())));
});
test("closed config refuses missing receipts duplicate homes unknown fields and overbroad capability", () => {
  assert.throws(() => parseNativeOwnerConfig({ ...ownerConfig(), worker: {} }));
  assert.throws(() => parseNativeOwnerConfig({ ...ownerConfig(), extra: true }));
  const cfg = ownerConfig();
  cfg.owners[1].codex_home = cfg.owners[0].codex_home;
  assert.throws(() => parseNativeOwnerConfig(cfg));
  const carrier = ownerCarrier();
  carrier.identity.capabilities.push("live_sideband");
  assert.throws(() => parseNativeOwnerCarrier(carrier));
  const insecure = new URL(ownerCarrier().origin);
  insecure.protocol = "http:";
  assert.throws(() => parseNativeOwnerCarrier({ ...ownerCarrier(), origin: insecure.origin }));
});
test("actual kernel limits are required rather than unit-property claims", () => {
  const limits = { "memory.max": "268435456", "memory.high": "134217728", "memory.swap.max": "67108864", "pids.max": "96", "cpu.max": "50000 100000" };
  verifyNativeOwnerLimits(limits);
  for (const key of Object.keys(limits))
    assert.throws(() => {
      verifyNativeOwnerLimits({ ...limits, [key]: "max" });
    });
});
