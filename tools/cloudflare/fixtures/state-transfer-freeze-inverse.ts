import assert from "node:assert/strict";
import { mkdir, readFile } from "node:fs/promises";
import { join } from "node:path";
import { awaitFrozen, beginDrain } from "../state-transfer-control.ts";
import { canonical } from "../state-transfer.ts";
import { ARTIFACT_SHA, ARTIFACT_TREE, ROOT, bytesHash, object, tuple, type Fixture, type Json } from "./state-transfer-freeze-support.ts";
import { invokePrepared, preparedBunSupport, preparedTool } from "./state-transfer-freeze-prepared.ts";

export async function frozenInverse(fixture: Fixture): Promise<void> {
  const tool = await preparedTool(fixture, "reverse");
  await fixture.running();
  const root = await fixture.root("latest-counter-writer");
  const counter = tuple("default", "native-counter");
  const response = await fixture.commit(root, counter, {
    schema_version: 1,
    value_encoding: "profile-v1",
    revision: "worker-write-1",
    value: { type: "kv_u64", value: "7" },
    expires_at_ms: null,
  });
  assert.equal(response.status, 200, await response.clone().text());
  assert.equal(object(await response.json()).committed, true);
  await fixture.finish(root);
  const current = fixture.current();
  await beginDrain(current.transport, current.plan);
  const reference = await awaitFrozen(current.transport, current.plan);
  const archive = await fixture.exportFrozen(reference);
  await fixture.stop();
  const receipt = await invokePrepared(fixture, tool, "native_reverse", {
    v: 1,
    now_ms: 2400,
    archive,
    registrations: [],
    artifacts: {
      rust_git_sha: ARTIFACT_SHA,
      rust_git_tree: ARTIFACT_TREE,
      wasm_sha256: await bytesHash(fixture.inputs.workerWasm),
      native_sha256: tool.sha256,
      native_nonce_capture_sha256: null,
    },
    output_directory: join(fixture.directory, "native-reverse"),
  });
  assert.equal(receipt.kind, "state_transfer_reverse");
  assert.equal(receipt.activation_authorized, false);
  assert.equal(receipt.archive_sha256, archive.sha256);
  assert.equal(receipt.archive_records, archive.records);
  const source = object(receipt.source);
  const custody = object(receipt.custody);
  assert(typeof source.path === "string" && typeof custody.path === "string");
  assert.equal(await bytesHash(source.path), source.sha256);
  assert.equal(await bytesHash(custody.path), custody.sha256);
  const captured = object(JSON.parse(await readFile(source.path, "utf8")));
  const original = object(JSON.parse(await readFile(join(ROOT, "rust/crates/gateway-migration/tests/fixtures/native-source.json"), "utf8")));
  assert.equal(captured.source_git_sha, original.source_git_sha, "Native source identity was rewritten to the Worker identity");
  assert(Array.isArray(captured.entries) && Array.isArray(original.entries));
  const rows = (entries: unknown[]): Map<string, Json> =>
    new Map(
      entries.map((entry) => {
        const row = object(entry);
        assert(Array.isArray(row.raw_key));
        const raw = row.raw_key.map((byte: unknown) => {
          assert(typeof byte === "number" && Number.isInteger(byte) && byte >= 0 && byte <= 255, "Native raw key contains a non-byte");
          return byte;
        });
        return [Buffer.from(raw).toString("hex"), row];
      })
    );
  const actual = rows(captured.entries);
  const baseline = rows(original.entries);
  assert.equal(actual.size, baseline.size, "NoCustody inverse changed the committed fixture inventory");
  const key = counter.slice("tuple-v1:".length);
  for (const [raw, row] of baseline) {
    const value = actual.get(raw);
    assert(value, `Inverse lost raw key ${raw}`);
    if (raw === key) {
      assert.equal(canonical(value.value), canonical({ encoding: "counter-le64", value: [7, 0, 0, 0, 0, 0, 0, 0] }));
      assert.equal(value.expiration_ms, -1);
      assert.notEqual(canonical(value.revision), canonical(row.revision), "Post-import write reused the old native revision");
    } else assert.equal(canonical(value), canonical(row), `Inverse changed revision, expiry or typed graph for ${raw}`);
  }
  fixture.facts.push({
    operation: "actual_native_inverse",
    clock_ms: 2400,
    entries: actual.size,
    latest_counter: 7,
    raw_keys_revisions_expiry_typed_values_verified: true,
    restore_and_recapture_verified: false,
    receipt,
  });
  const support = await preparedBunSupport(fixture);
  const output = join(fixture.directory, "new-bun-restore");
  await mkdir(output, { mode: 0o700 });
  const reverseReceipt = join(fixture.directory, "native_reverse.stdout.private.json");
  const restored = await invokePrepared(fixture, support, "bun_inverse_restore", {
    v: 1,
    source_root: support.sourceRoot,
    reverse_receipt: { path: reverseReceipt, sha256: await bytesHash(reverseReceipt) },
    output_directory: output,
  });
  assert.equal(restored.kind, "frozen_inverse_new_bun_recapture");
  assert.equal(restored.status, "matched");
  assert.equal(restored.rows, actual.size);
  // The unchanged Bun codec helper pins its own clean production-code source,
  // independently from the newly built Rust lifecycle artifact above.
  assert.equal(restored.artifact_source_sha, "bf9c949686d14a193564be507d4048c4b3709244");
  assert.equal(restored.artifact_source_tree, "31f3fc58424ceaaecfbe0d32796ad3271e99b3a8");
  assert.equal(restored.native_backend_loaded, false);
  assert.equal(restored.live_state, false);
  fixture.facts.push({ operation: "new_bun_restore_and_independent_recapture", verified: true, receipt: restored });
}
