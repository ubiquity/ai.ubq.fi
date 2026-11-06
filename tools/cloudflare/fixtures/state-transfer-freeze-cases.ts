import assert from "node:assert/strict";
import { createHash, createHmac, randomBytes } from "node:crypto";
import { activate, awaitFrozen, beginDrain, mintControlIntent } from "../state-transfer-control.ts";
import { canonical, sha256 } from "../state-transfer.ts";
import { Fixture, jsonRecord, object, pending, tuple, type Json, type Permit } from "./state-transfer-freeze-support.ts";

const auth = { authorization: "Bearer fixture-access", "content-type": "application/json" };
const write = async (fixture: Fixture, permit: Permit, name: string): Promise<void> => {
  const response = await fixture.commit(permit, tuple("uos_ai", "freeze_fixture", name), jsonRecord(`revision-${name}`, { name }));
  assert.equal(response.status, 200, await response.clone().text());
  assert.equal(object(await response.json()).committed, true);
};
const refused = async (response: Response): Promise<void> => {
  assert(response.status >= 400, `Expected refusal, received ${response.status}`);
  await response.arrayBuffer();
};

async function progressWhileWaiting(fixture: Fixture, permit: Permit): Promise<void> {
  await write(fixture, permit, `concurrent-${fixture.label}`);
  const independent = await fixture.rpc({ operation: "coordination", request: { operation: "status" } }, { kind: "operator_read" }, "independent-progress");
  assert.equal(independent.status, 200, "Independent production DO must progress during AwaitFrozen");
  assert.equal(object(object(object(await independent.json()).reply).status).gate !== undefined, true);
  await fixture.finish(permit);
  fixture.facts.push({ operation: "concurrent_commit_and_independent_do_progress" });
}

export async function heldRequest(fixture: Fixture, stream: boolean): Promise<void> {
  await fixture.running();
  const finisher = await fixture.root("concurrent-finisher");
  const response = pending(
    fixture.public("/v1/responses", {
      method: "POST",
      headers: auth,
      body: JSON.stringify({ model: "deepseek-flash", input: `fixture:freeze:${fixture.label}`, stream }),
    })
  );
  await fixture.event("held", fixture.label);
  const streamResponse = stream ? await response.promise : undefined;
  if (streamResponse) assert.equal(streamResponse.status, 200, "Held stream must return successful headers before provider release");
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const frozen = pending(awaitFrozen(current.transport, current.binding));
  await progressWhileWaiting(fixture, finisher);
  const held = await fixture.status();
  assert.equal(held.gate.phase, "draining");
  assert(held.counts.roots > 0, "Held production request must retain its durable writer root");
  assert.equal(frozen.settled, false, "AwaitFrozen completed before the controlled provider terminal");
  await refused(await fixture.public("/v1/models", { headers: auth }));
  const release = await fixture.control(`/release?name=${fixture.label}`);
  assert.equal(release.status, 204);
  const finalResponse = streamResponse ?? (await response.promise);
  assert.equal(finalResponse.status, 200);
  const body = await finalResponse.text();
  assert(body.includes(stream ? "response.completed" : "fixture answer"), "The real client did not receive its semantic terminal");
  const reference = await frozen.promise;
  assert.equal(canonical((await fixture.status()).counts), canonical({ roots: 0, native_operations: 0, native_joins: 0, import_frames: 0, blocked: 0 }));
  await refused(await fixture.commit(finisher, tuple("uos_ai", "freeze_fixture", "late"), jsonRecord("late", {})));
  assert(fixture.drain);
  await fixture.restart(fixture.drain.sha256);
  assert.equal(canonical(await awaitFrozen(fixture.current().transport, current.binding)), canonical(reference), "Lost acknowledgement/restart changed Frozen");
  await fixture.exportFrozen(reference);
  fixture.facts.push({
    operation: "held_request",
    mode: stream ? "stream" : "buffer",
    semantic_terminal: true,
    frozen_after_settlement: true,
    acknowledged_frozen_replayed_after_restart: true,
  });
}

async function reopen(fixture: Fixture): Promise<void> {
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const reference = await awaitFrozen(current.transport, current.binding);
  const intent = mintControlIntent({ purpose: "activate", data: reference });
  await fixture.restart(intent.sha256);
  await activate(fixture.transport(intent.token), reference);
  fixture.binding = { identity: reference.identity, serving_epoch: reference.serving_epoch + 1 };
  fixture.drain = mintControlIntent({ purpose: "drain", data: fixture.binding });
  await fixture.restart(fixture.drain.sha256);
}

export async function backgroundWriter(fixture: Fixture): Promise<void> {
  await fixture.running();
  const warm = await fixture.public("/v1/models?client_version=0.163.0", { headers: auth });
  assert.equal(warm.status, 200, await warm.clone().text());
  await warm.arrayBuffer();
  await reopen(fixture);
  const key = tuple("ubq_ai", "codex_catalog", "0.163.0");
  const metadata = object((await fixture.exchange({ operation: "read", key })).record);
  const value = object(metadata.value);
  const seed = await fixture.root("stale-catalog-seed");
  const seeded = await fixture.commit(seed, key, { ...metadata, revision: "freeze-stale-catalog", value: { ...value, fetched_at_ms: Date.now() - 600_000 } });
  assert.equal(seeded.status, 200, await seeded.clone().text());
  await seeded.arrayBuffer();
  await fixture.finish(seed);
  assert.equal((await fixture.control("/arm?name=background")).status, 204);
  const stale = await fixture.public("/v1/models?client_version=0.163.0", { headers: auth });
  assert.equal(stale.status, 200, await stale.clone().text());
  assert.equal(stale.headers.get("x-uos-cache"), "stale");
  await stale.arrayBuffer();
  await fixture.event("held", "background");
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const frozen = pending(awaitFrozen(current.transport, current.binding));
  const state = await fixture.status();
  assert(state.counts.roots > 0, "Response return abandoned its held background writer");
  assert.equal(frozen.settled, false);
  assert.equal((await fixture.control("/release?name=background")).status, 204);
  const reference = await frozen.promise;
  fixture.facts.push({ operation: "background_writer_outlives_response", frozen_ref: reference });
}

const nativeBinding = (suffix: string): Json => ({
  operation_id: `operation-${suffix}`,
  action_ref: `action-${suffix}`,
  owner_id: "freeze-owner",
  owner_epoch: "freeze-epoch",
  key_fingerprint: createHash("sha256").update(Buffer.alloc(32, 31)).digest("hex"),
});

async function join(fixture: Fixture, binding: Json): Promise<Permit> {
  const id = randomBytes(32).toString("hex");
  const capability = randomBytes(32).toString("hex");
  const acquired = await fixture.coordinate({ operation: "join_native", selector: binding, id, capability_sha256: sha256(capability) });
  assert.equal(acquired.operation, "acquired");
  return { id, capability, serving_epoch: Number(acquired.serving_epoch) };
}

export async function nativeOrdering(fixture: Fixture): Promise<void> {
  await fixture.running();
  const root = await fixture.root("native-origin");
  const a = nativeBinding("a");
  const b = nativeBinding("b");
  const operationA = await fixture.acquire({ kind: "native_operation", binding: a }, root);
  const operationB = await fixture.acquire({ kind: "native_operation", binding: b }, root);
  const accounting = await fixture.root("independent-required-settlement");
  await fixture.finish(accounting, "required_settlement");
  const first = await join(fixture, a);
  const second = await join(fixture, a);
  const sibling = await join(fixture, b);
  for (const mismatch of [
    { ...a, action_ref: "wrong" },
    { ...a, owner_epoch: "wrong" },
    { ...a, key_fingerprint: "0".repeat(64) },
  ]) {
    await refused(
      await fixture.rpc({
        operation: "coordination",
        request: { operation: "join_native", selector: mismatch, id: randomBytes(32).toString("hex"), capability_sha256: sha256("invalid-selector") },
      })
    );
  }
  await fixture.finish(operationA, "native_effect_unknown");
  await fixture.finish(root);
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const frozen = pending(awaitFrozen(current.transport, current.binding));
  const key = tuple("uos_ai", "freeze_fixture", "native-race");
  const [commit, publication] = await Promise.all([
    fixture.commit(second, key, jsonRecord("before-retirement", { write: "racing" })),
    fixture.coordinate({ operation: "native_published", join: first }),
  ]);
  assert.equal(publication.operation, "status");
  const committed = commit.status === 200;
  if (committed) assert.equal(object(await commit.json()).committed, true);
  else await refused(commit);
  const observed = (await fixture.exchange({ operation: "read", key })).record;
  assert.equal(observed !== null, committed, "Racing commit was partially applied or lost");
  await refused(await fixture.commit(second, key, jsonRecord("forbidden-after-retirement", {})));
  const stillPending = await fixture.status();
  assert.equal(stillPending.counts.native_operations, 1, "Publication retired an unrelated native operation");
  assert.equal(stillPending.counts.native_joins, 1, "Publication retired an unrelated native join");
  assert.equal(stillPending.counts.blocked, 1, "Publication cleared another root's required settlement blocker");
  assert.equal(frozen.settled, false);
  await write(fixture, sibling, "sibling-survives");
  await fixture.finish(sibling);
  await fixture.finish(operationB);
  await fixture.finish(accounting);
  const reference = await frozen.promise;
  fixture.facts.push({
    operation: "native_private_reducer_retirement",
    receiver_proof: false,
    racing_commit_preceded: committed,
    late_write_refused: true,
    sibling_retained: true,
    unrelated_required_settlement_retained: true,
    frozen_ref: reference,
  });
}

export async function orphanAndCancellation(fixture: Fixture): Promise<void> {
  await fixture.running();
  const root = await fixture.root("lost-root-ack");
  await write(fixture, root, "persist-before-lost-ack");
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const cancellation = new AbortController();
  const waiting = pending(
    awaitFrozen({ ...current.transport, fetch: (input, init) => fetch(input, { ...init, signal: cancellation.signal }) }, current.binding)
  );
  const before = await fixture.status();
  assert.equal(before.counts.roots, 1);
  assert.equal(waiting.settled, false);
  cancellation.abort();
  await assert.rejects(waiting.promise);
  assert(fixture.drain);
  await fixture.restart(fixture.drain.sha256);
  const after = await fixture.status();
  assert.equal(after.gate.phase, "draining", "Client cancellation or restart fabricated Frozen");
  assert.equal(after.counts.roots, 1, "Unconfirmed writer disappeared on restart");
  const resumed = pending(awaitFrozen(fixture.current().transport, current.binding));
  await write(fixture, root, "finish-after-restart");
  assert.equal(resumed.settled, false);
  const lostAck = await fixture.rpc({ operation: "coordination", request: { operation: "finish", permit: root, blocker: null } });
  assert.equal(lostAck.status, 200);
  await lostAck.body?.cancel();
  const reference = await resumed.promise;
  await fixture.finish(root);
  assert.equal(canonical(await awaitFrozen(fixture.current().transport, current.binding)), canonical(reference));
  fixture.facts.push({
    operation: "orphan_lost_ack_restart",
    cancellation_is_completion: false,
    explicit_finish_required: true,
    finish_body_acknowledgement_lost: true,
    frozen_ref: reference,
  });
}

export async function signedUnprovedCallback(fixture: Fixture): Promise<void> {
  await fixture.running();
  const root = await fixture.root("signed-callback-origin");
  const binding = nativeBinding("signed-unproved");
  const operation = await fixture.acquire({ kind: "native_operation", binding }, root);
  await fixture.finish(root);
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const path = "/uos/native/renewal-authority";
  const body = JSON.stringify({
    v: 1,
    operation: "published",
    operation_id: binding.operation_id,
    claim_ref: "missing-claim",
    publication_revision: "unproved",
  });
  const time = String(Date.now());
  const nonce = randomBytes(32).toString("base64url");
  const mac = createHmac("sha256", Buffer.alloc(32, 31))
    .update(`uos-native-http-v1\0POST\0${path}\0freeze-key\0${time}\0${nonce}\0${sha256(body)}`)
    .digest("base64url");
  const callback = await fixture.public(path, {
    method: "POST",
    body,
    headers: {
      "content-type": "application/json",
      "x-uos-native-version": "1",
      "x-uos-native-key-id": "freeze-key",
      "x-uos-native-time-ms": time,
      "x-uos-native-nonce": nonce,
      "x-uos-native-mac": mac,
    },
  });
  await refused(callback);
  const registry = object((await fixture.exchange({ operation: "read", key: tuple("uos_ai", "native_nonce_registry", "v1") })).record);
  assert.equal(typeof object(object(registry.value).nonces)[`freeze-key:${nonce}`], "number", "Signed callback never passed real authentication");
  const state = await fixture.status();
  assert.equal(state.gate.phase, "draining");
  assert.equal(state.counts.native_operations, 1, "A valid signature without receiver proof retired the operation");
  assert.equal(state.counts.native_joins, 0, "Refused callback leaked a join");
  await fixture.finish(operation, "native_effect_unknown");
  assert(fixture.drain);
  await fixture.restart(fixture.drain.sha256);
  const restarted = await fixture.status();
  assert.equal(restarted.counts.blocked, 1);
  assert.equal(restarted.gate.phase, "draining");
  fixture.facts.push({ operation: "signed_callback_without_terminal_proof_refused", positive_published_proof: false, blocker_survives_restart: true });
}

export async function eofWithoutTerminal(fixture: Fixture): Promise<void> {
  await fixture.running();
  const response = pending(
    fixture.public("/v1/responses", {
      method: "POST",
      headers: auth,
      body: JSON.stringify({ model: "deepseek-flash", input: "fixture:freeze:eof", stream: true }),
    })
  );
  await fixture.event("held", "eof");
  const current = fixture.current();
  await beginDrain(current.transport, current.binding);
  const frozen = pending(awaitFrozen(current.transport, current.binding));
  assert.equal((await fixture.control("/release?name=eof")).status, 204);
  const stream = await response.promise;
  assert(stream.body);
  const reader = stream.body.getReader();
  const chunks: Uint8Array[] = [];
  let readError: string | null = null;
  try {
    for (;;) {
      const chunk = await reader.read();
      if (chunk.done) break;
      chunks.push(chunk.value);
      assert(chunks.reduce((bytes, item) => bytes + item.length, 0) < 1_000_000, "Fixture response exceeded its bound");
    }
  } catch (error) {
    readError = String(error);
  } finally {
    reader.releaseLock();
  }
  const body = Buffer.concat(chunks).toString("utf8");
  assert(!body.includes("response.completed"), "Truncated provider stream emitted a semantic completed event");
  await fixture.event("eof_without_terminal", "eof");
  const state = await fixture.status();
  assert.equal(state.gate.phase, "draining", "EOF without semantic terminal fabricated Frozen");
  assert(state.counts.blocked > 0 || state.counts.roots > 0, "EOF discarded every unresolved writer");
  assert.equal(frozen.settled, false);
  fixture.facts.push({ operation: "eof_without_terminal_is_not_completion", counts: state.counts, read_error: readError });
}
