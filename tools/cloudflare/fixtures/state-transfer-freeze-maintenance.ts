import assert from "node:assert/strict";
import { activate, awaitFrozen, beginDrain, cancelDrain, mintControlIntent, type CancelDrainPlan } from "../state-transfer-control.ts";
import { canonical } from "../state-transfer.ts";
import { Fixture, object, pending, tuple } from "./state-transfer-freeze-support.ts";
import { reopen } from "./state-transfer-freeze-cases.ts";

export async function prepareCancellation(fixture: Fixture) {
  const before = fixture.current();
  assert(fixture.drain);
  const plan: CancelDrainPlan = { ...before.plan, drain_digest: fixture.drain.sha256 };
  const intent = mintControlIntent({ purpose: "cancel_drain", data: plan });
  const next = { binding: before.binding, drain_revision: before.plan.drain_revision + 1 };
  const nextIntent = mintControlIntent({ purpose: "drain", data: next });
  fixture.operators = { ...fixture.operators, cancel: intent.sha256, next: nextIntent.sha256 };
  await fixture.restart(fixture.drain.sha256);
  return { current: fixture.current(), plan, intent, next, nextIntent };
}

export async function cancelHeldRequest(fixture: Fixture, stream: boolean): Promise<void> {
  await fixture.running();
  const prepared = await prepareCancellation(fixture);
  const response = pending(
    fixture.public("/v1/responses", {
      method: "POST",
      headers: { authorization: "Bearer fixture-access", "content-type": "application/json" },
      body: JSON.stringify({ model: "deepseek-flash", input: `fixture:freeze:${fixture.label}`, stream }),
    })
  );
  await fixture.event("held", fixture.label);
  const head = stream ? await response.promise : undefined;
  if (head) assert.equal(head.status, 200);
  await beginDrain(prepared.current.transport, prepared.current.plan);
  const waiter = pending(awaitFrozen(prepared.current.transport, prepared.current.plan));
  const before = await fixture.status();
  const cancelled = await cancelDrain(fixture.operatorTransport("cancel", prepared.intent.token), prepared.plan);
  const resumed = object(object(cancelled.status).gate);
  assert.equal(canonical(resumed.identity), canonical(prepared.current.binding.identity));
  assert.equal(resumed.serving_epoch, before.gate.serving_epoch);
  assert.equal(canonical(object(cancelled.status).counts), canonical(before.counts));
  await assert.rejects(waiter.promise, /drain_cancelled/u);
  assert.equal(response.settled, stream, "Cancellation completed an unresolved buffered request");
  await assert.rejects(beginDrain(prepared.current.transport, prepared.current.plan));
  const ordinary = await fixture.public("/v5-admission-probe", { headers: { authorization: "Bearer fixture-access" } });
  assert.equal(ordinary.status, 404, "Running admission must reach the actual router after cancellation");
  await ordinary.arrayBuffer();
  const nextTransport = fixture.operatorTransport("next", prepared.nextIntent.token);
  await beginDrain(nextTransport, prepared.next);
  const frozen = pending(awaitFrozen(nextTransport, prepared.next));
  await assert.rejects(cancelDrain(fixture.operatorTransport("cancel", prepared.intent.token), prepared.plan));
  assert.equal((await fixture.status()).gate.phase, "draining");
  assert.equal(frozen.settled, false);
  assert.equal((await fixture.control(`/release?name=${fixture.label}`)).status, 204);
  const result = head ?? (await response.promise);
  assert.equal(result.status, 200);
  assert((await result.text()).includes(stream ? "response.completed" : "fixture answer"));
  const reference = await frozen.promise;
  await fixture.exportFrozen(reference);
  fixture.facts.push({
    operation: "cancel_held_request_same_authority",
    stream,
    serving_epoch_preserved: true,
    counts_preserved: true,
    old_drain_rejected: true,
    old_wait_cancelled: true,
    stale_cancel_rejected: true,
    frozen_ref: reference,
  });
}

export async function cancelHeldBackground(fixture: Fixture): Promise<void> {
  await fixture.running();
  const warm = await fixture.public("/v1/models?client_version=0.163.0", { headers: { authorization: "Bearer fixture-access" } });
  assert.equal(warm.status, 200);
  await warm.arrayBuffer();
  await reopen(fixture);
  const prepared = await prepareCancellation(fixture);
  const key = tuple("ubq_ai", "codex_catalog", "0.163.0");
  const metadata = object((await fixture.exchange({ operation: "read", key })).record);
  const seed = await fixture.root("cancel-stale-catalog");
  const saved = await fixture.commit(seed, key, {
    ...metadata,
    revision: "cancel-stale",
    value: { ...object(metadata.value), fetched_at_ms: Date.now() - 600_000 },
  });
  assert.equal(saved.status, 200);
  await saved.arrayBuffer();
  await fixture.finish(seed);
  assert.equal((await fixture.control("/arm?name=background")).status, 204);
  const stale = await fixture.public("/v1/models?client_version=0.163.0", { headers: { authorization: "Bearer fixture-access" } });
  assert.equal(stale.status, 200);
  assert.equal(stale.headers.get("x-uos-cache"), "stale");
  await stale.arrayBuffer();
  await fixture.event("held", "background");
  await beginDrain(prepared.current.transport, prepared.current.plan);
  const waiter = pending(awaitFrozen(prepared.current.transport, prepared.current.plan));
  const before = await fixture.status();
  assert(before.counts.roots > 0);
  const cancelled = await cancelDrain(fixture.operatorTransport("cancel", prepared.intent.token), prepared.plan);
  assert.equal(canonical(object(cancelled.status).counts), canonical(before.counts));
  await assert.rejects(waiter.promise, /drain_cancelled/u);
  const nextTransport = fixture.operatorTransport("next", prepared.nextIntent.token);
  await beginDrain(nextTransport, prepared.next);
  const frozen = pending(awaitFrozen(nextTransport, prepared.next));
  assert.equal((await fixture.control("/release?name=background")).status, 204);
  const reference = await frozen.promise;
  fixture.facts.push({ operation: "cancel_held_background_same_authority", cancellation: cancelled.cancellation, frozen_ref: reference });
}

export async function repeatedMaintenance(fixture: Fixture): Promise<void> {
  await fixture.running();
  const cycles = [];
  for (let cycle = 0; cycle < 3; cycle++) {
    const current = fixture.current();
    assert(fixture.drain);
    const oldDrain = fixture.drain;
    await beginDrain(current.transport, current.plan);
    const frozen = await awaitFrozen(current.transport, current.plan);
    const activation = mintControlIntent({ purpose: "activate", data: frozen });
    await fixture.restart(activation.sha256);
    await activate(fixture.transport(activation.token), frozen);
    fixture.binding = { identity: frozen.identity, serving_epoch: frozen.serving_epoch + 1 };
    fixture.drainRevision = 0;
    fixture.drain = mintControlIntent({ purpose: "drain", data: { binding: fixture.binding, drain_revision: 0 } });
    fixture.operators = { ...fixture.operators, prior: oldDrain.sha256, activate: activation.sha256 };
    await fixture.restart(fixture.drain.sha256);
    await assert.rejects(beginDrain(fixture.operatorTransport("prior", oldDrain.token), current.plan));
    const resumed = await fixture.status();
    assert.equal(resumed.gate.phase, "running");
    assert.equal(resumed.gate.serving_epoch, frozen.serving_epoch + 1);
    const blocker = await fixture.root("old-activation-cannot-thaw-new-drain");
    const next = fixture.current();
    await beginDrain(next.transport, next.plan);
    await assert.rejects(activate(fixture.operatorTransport("activate", activation.token), frozen));
    await fixture.finish(blocker);
    const second = await awaitFrozen(next.transport, next.plan);
    const nextActivation = mintControlIntent({ purpose: "activate", data: second });
    await fixture.restart(nextActivation.sha256);
    await activate(fixture.transport(nextActivation.token), second);
    fixture.binding = { identity: second.identity, serving_epoch: second.serving_epoch + 1 };
    fixture.drainRevision = 0;
    fixture.drain = mintControlIntent({ purpose: "drain", data: { binding: fixture.binding, drain_revision: 0 } });
    await fixture.restart(fixture.drain.sha256);
    cycles.push({ first: frozen, second });
  }
  fixture.facts.push({ operation: "repeatable_genuine_maintenance", cycles, old_activation_fenced: true, old_epoch_drain_fenced: true });
}

export async function cancellationFinishRace(fixture: Fixture): Promise<void> {
  await fixture.running();
  const prepared = await prepareCancellation(fixture);
  const root = await fixture.root("actual-cancel-finish-race");
  await beginDrain(prepared.current.transport, prepared.current.plan);
  const oldWait = pending(awaitFrozen(prepared.current.transport, prepared.current.plan));
  const [cancellation, finish] = await Promise.allSettled([
    cancelDrain(fixture.operatorTransport("cancel", prepared.intent.token), prepared.plan),
    fixture.finish(root),
  ]);
  assert.equal(finish.status, "fulfilled", "The actual owning finisher must settle durably");
  const state = await fixture.status();
  assert.equal(canonical(state.counts), canonical({ roots: 0, native_operations: 0, native_joins: 0, import_frames: 0, blocked: 0 }));
  assert.equal(state.gate.serving_epoch, prepared.current.binding.serving_epoch);
  if (state.gate.phase === "frozen") {
    assert.equal(cancellation.status, "rejected", "Cancellation thawed a winning actual Frozen transition");
    const reference = await oldWait.promise;
    await fixture.exportFrozen(reference);
    fixture.facts.push({ operation: "cancel_races_actual_finish", winner: "finish", frozen_ref: reference });
    return;
  }
  assert.equal(state.gate.phase, "running");
  assert.equal(cancellation.status, "fulfilled");
  await assert.rejects(oldWait.promise, /drain_cancelled/u);
  const next = fixture.operatorTransport("next", prepared.nextIntent.token);
  await beginDrain(next, prepared.next);
  const reference = await awaitFrozen(next, prepared.next);
  await fixture.exportFrozen(reference);
  fixture.facts.push({
    operation: "cancel_races_actual_finish",
    winner: "cancel",
    later_finish_did_not_freeze: true,
    fresh_drain_required: true,
    frozen_ref: reference,
  });
}
