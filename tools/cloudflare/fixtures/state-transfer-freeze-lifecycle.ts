import assert from "node:assert/strict";
import { Database } from "bun:sqlite";
import { cp, open, readdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { awaitFrozen, beginDrain, cancelDrain } from "../state-transfer-control.ts";
import { canonical, INTENT_HEADER, mintIntent } from "../state-transfer.ts";
import { ADMIN, bytesHash, object, type Fixture, type Json } from "./state-transfer-freeze-support.ts";
import { prepareCancellation } from "./state-transfer-freeze-maintenance.ts";
import { preparedWriter } from "./state-transfer-freeze-prepared.ts";

const counts = (roots: number, blocked: number): Json => ({ roots, native_operations: 0, native_joins: 0, import_frames: 0, blocked });

async function stoppedDropLease(fixture: Fixture, writer: Json, label: string): Promise<Json> {
  const state = join(fixture.directory, "state");
  const copy = join(fixture.directory, `stopped-sqlite-cohort-${label}`);
  await cp(state, copy, { recursive: true, errorOnExist: true, force: false });
  const files = await readdir(copy, { recursive: true, withFileTypes: true });
  const cohort: Json[] = [];
  let matches = 0;
  let observed: Json | undefined;
  for (const file of files) {
    if (!file.isFile()) continue;
    const path = join(file.parentPath, file.name);
    cohort.push({ path, sha256: await bytesHash(path) });
    const handle = await open(path, "r");
    const header = Buffer.alloc(16);
    try {
      await handle.read(header, 0, 16, 0);
    } finally {
      await handle.close();
    }
    if (header.toString("binary") !== "SQLite format 3\0") continue;
    const database = new Database(path, { readonly: true, create: false, strict: true });
    try {
      const table = database.query("SELECT name FROM sqlite_master WHERE type='table' AND name='uos_drain_lease'").get();
      if (!table) continue;
      const rows = database.query("SELECT payload FROM uos_drain_lease WHERE id=?").all(String(writer.writer_id));
      for (const raw of rows) {
        const row = object(raw);
        assert(typeof row.payload === "string");
        const lease = object(JSON.parse(row.payload));
        assert.equal(lease.id, writer.writer_id);
        assert.equal(lease.serving_epoch, writer.serving_epoch);
        assert.equal(lease.capability_sha256, writer.capability_sha256);
        assert.equal(lease.blocker, "task_abandoned");
        assert.equal(lease.terminal, false);
        observed = lease;
        matches++;
      }
    } finally {
      database.close();
    }
  }
  assert.equal(matches, 1, "Exactly one stopped production lease must confirm the real Rust abandonment");
  await writeFile(join(fixture.directory, `stopped-sqlite-cohort-${label}.json`), JSON.stringify(cohort, null, 2) + "\n", { flag: "wx", mode: 0o600 });
  assert(observed);
  fixture.facts.push({ operation: "stopped_actual_drop_lease", label, writer_id: writer.writer_id, blocker: "task_abandoned", matches, cohort });
  return observed;
}

export async function actualWriter(fixture: Fixture, abandon: boolean): Promise<void> {
  fixture.writer = await preparedWriter(fixture);
  await fixture.running();
  const cancellation = abandon ? await prepareCancellation(fixture) : undefined;
  const response = await fetch(`${fixture.origin}/writers/${abandon ? "drop" : "complete"}`, { method: "POST", signal: AbortSignal.timeout(20_000) });
  assert.equal(response.status, 200, await response.clone().text());
  assert.equal(response.headers.get("x-uos-fixture-retained-promises"), "1");
  const writer = object(await response.json());
  assert.equal(writer.operation, "actual_rust_writer");
  assert.equal(writer.mode, abandon ? "drop" : "complete");
  const expected = counts(abandon ? 1 : 0, abandon ? 1 : 0);
  const state = await fixture.status();
  assert.equal(state.gate.phase, "running");
  assert.equal(canonical(state.counts), canonical(expected), "Joined real retain promise did not establish expected durable state");
  const current = fixture.current();
  await beginDrain(current.transport, current.plan);
  if (!abandon) {
    const frozen = await awaitFrozen(current.transport, current.plan);
    assert(fixture.drain);
    await fixture.restart(fixture.drain.sha256);
    assert.equal(canonical(await awaitFrozen(fixture.current().transport, current.plan)), canonical(frozen));
    fixture.facts.push({ operation: "actual_writer_complete", retention_joined: true, frozen_ref: frozen });
    return;
  }
  const draining = await fixture.status();
  assert.equal(draining.gate.phase, "draining");
  assert.equal(canonical(draining.counts), canonical(expected));
  const ordinary = await fixture.public("/v1/models", { headers: { authorization: "Bearer fixture-access" } });
  assert.equal(ordinary.status, 503);
  await ordinary.arrayBuffer();
  await fixture.stop();
  const unknown = await stoppedDropLease(fixture, writer, "before-cancel");
  assert(fixture.drain);
  await fixture.restart(fixture.drain.sha256);
  const restarted = await fixture.status();
  assert.equal(restarted.gate.phase, "draining");
  assert.equal(canonical(restarted.counts), canonical(expected));
  fixture.facts.push({ operation: "actual_writer_drop", retention_joined: true, restart_persisted: true, counts: restarted.counts });
  assert(cancellation);
  const resumed = await cancelDrain(fixture.operatorTransport("cancel", cancellation.intent.token), cancellation.plan);
  assert.equal(canonical(object(resumed.status).counts), canonical(expected));
  const replay = await cancelDrain(fixture.operatorTransport("cancel", cancellation.intent.token), cancellation.plan);
  assert.equal(replay.replayed, true);
  assert.equal(replay.cancellation.next_drain_revision, resumed.cancellation.next_drain_revision);
  await fixture.stop();
  const retained = await stoppedDropLease(fixture, writer, "after-cancel");
  assert.equal(canonical(retained), canonical(unknown), "Cancellation changed the actual abandoned lease row");
  await fixture.restart(fixture.drain.sha256);
  await assert.rejects(beginDrain(fixture.transport(fixture.drain.token), cancellation.current.plan));
  await assert.rejects(awaitFrozen(fixture.transport(fixture.drain.token), cancellation.current.plan), /drain_cancelled/u);
  const persistedReplay = await cancelDrain(fixture.operatorTransport("cancel", cancellation.intent.token), cancellation.plan);
  assert.equal(persistedReplay.replayed, true);
  assert.equal(persistedReplay.cancellation.next_drain_revision, resumed.cancellation.next_drain_revision);
  const admitted = await fixture.public("/v5-admission-probe", { headers: { authorization: "Bearer fixture-access" } });
  assert.equal(admitted.status, 404);
  await admitted.arrayBuffer();
  await beginDrain(fixture.operatorTransport("next", cancellation.nextIntent.token), cancellation.next);
  fixture.facts.push({
    operation: "cancel_actual_drop_same_authority",
    same_epoch: true,
    exact_lease_preserved: true,
    replay_no_second_increment: true,
    old_drain_rejected: true,
  });

  const status = object((await fixture.coordinate({ operation: "status" })).status);
  const historical = object(object(status.gate).frozen);
  const exportIntent = mintIntent({ kind: "export" });
  await fixture.restart(exportIntent.sha256);
  const exported = await fixture.public("/admin/state-transfer/export", {
    method: "POST",
    headers: {
      authorization: `Bearer ${ADMIN}`,
      [INTENT_HEADER]: exportIntent.token,
      "content-type": "application/json",
    },
    body: JSON.stringify({ v: 2, freeze_id: historical.freeze_id, freeze_revision: historical.freeze_revision, cursor: null, limit: 3 }),
  });
  assert.equal(exported.status, 409, "Unresolved actual Drop must refuse historical-Frozen export");
  await exported.arrayBuffer();
}

export async function actualProtocolTimeout(fixture: Fixture): Promise<void> {
  await fixture.running();
  const root = await fixture.root("actual-five-minute-timeout");
  const current = fixture.current();
  await beginDrain(current.transport, current.plan);
  const started = performance.now();
  const response = await fetch(`${fixture.origin}/admin/state-transfer/control`, {
    method: "POST",
    signal: AbortSignal.timeout(345_000),
    headers: { authorization: `Bearer ${ADMIN}`, [INTENT_HEADER]: current.transport.intent, "content-type": "application/json" },
    body: JSON.stringify({ v: 2, request: { operation: "await_frozen", plan: current.plan } }),
  });
  const body = object(await response.json());
  const elapsed = performance.now() - started;
  assert.equal(response.status, 504, JSON.stringify(body));
  assert(elapsed >= 300_000 && elapsed < 345_000, "Only the real server deadline establishes protocol timeout");
  assert.equal(body.operation, "drain_timeout");
  const timedOut = object(body.status);
  const gate = object(timedOut.gate);
  assert.equal(gate.phase, "draining");
  assert.equal(canonical(gate.identity), canonical(current.binding.identity));
  assert.equal(gate.serving_epoch, root.serving_epoch);
  assert(fixture.drain);
  assert.equal(gate.drain_digest, fixture.drain.sha256);
  const historical = gate.frozen === null ? null : object(gate.frozen);
  assert(historical?.freeze_id !== fixture.drain.sha256, "Timeout minted a current Frozen acknowledgement");
  assert.equal(canonical(timedOut.counts), canonical(counts(1, 0)));
  assert.equal(canonical((await fixture.status()).counts), canonical(counts(1, 0)));
  await fixture.restart(fixture.drain.sha256);
  const restarted = await fixture.status();
  assert.equal(restarted.gate.phase, "draining");
  assert.equal(canonical(restarted.counts), canonical(counts(1, 0)));
  const cancellation = await prepareCancellation(fixture);
  const resumed = await cancelDrain(fixture.operatorTransport("cancel", cancellation.intent.token), cancellation.plan);
  assert.equal(canonical(object(resumed.status).counts), canonical(counts(1, 0)));
  assert.equal(object(object(resumed.status).gate).serving_epoch, root.serving_epoch);
  await assert.rejects(beginDrain(fixture.transport(fixture.drain.token), cancellation.current.plan));
  fixture.facts.push({ operation: "cancel_after_actual_timeout_same_authority", cancellation: resumed.cancellation, root_unresolved: true });
  fixture.facts.push({
    operation: "actual_five_minute_protocol_timeout",
    status: response.status,
    elapsed_ms: elapsed,
    body,
    root_unresolved: true,
    restarted_counts: restarted.counts,
  });
}
