// Positive receiver integration needs fresh rows minted by the actual Rust
// actor/action/claim/start constructors. Missing prepared tooling is a blocker,
// never permission to manufacture ActionRows or bypass the signed receiver.
import assert from "node:assert/strict";
import { createHmac, randomBytes } from "node:crypto";
import { awaitFrozen, beginDrain, cancelDrain } from "../state-transfer-control.ts";
import { prepareCancellation } from "./state-transfer-freeze-maintenance.ts";
import { sha256 } from "../state-transfer.ts";
import { ARTIFACT_SHA, ARTIFACT_TREE, jsonRecord, object, pending, tuple, type Fixture, type Json, type Permit } from "./state-transfer-freeze-support.ts";
import { invokePrepared, preparedTool } from "./state-transfer-freeze-prepared.ts";

const key = Buffer.alloc(32, 31);
const identity = {
  key_id: "freeze-key",
  owner_id: "freeze-owner",
  owner_epoch: "freeze-epoch",
  key_base64url: key.toString("base64url"),
  capabilities: ["supervisor_read", "owner_renewal", "access_publish"],
  codex_home: "/fixture/native-owner",
};

async function signed(fixture: Fixture, path: string, value: Json): Promise<Readonly<{ status: number; body: Json }>> {
  const body = JSON.stringify(value);
  const nonce = randomBytes(32).toString("base64url");
  const time = String(Date.now());
  const mac = createHmac("sha256", key)
    .update(`uos-native-http-v1\0POST\0${path}\0freeze-key\0${time}\0${nonce}\0${sha256(body)}`)
    .digest("base64url");
  const response = await fixture.public(path, {
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
  return { status: response.status, body: object(await response.json()) };
}

async function join(fixture: Fixture, selector: Json): Promise<Permit> {
  const id = randomBytes(32).toString("hex");
  const capability = randomBytes(32).toString("hex");
  const reply = await fixture.coordinate({ operation: "join_native", selector, id, capability_sha256: sha256(capability) });
  assert.equal(reply.operation, "acquired");
  return { id, capability, serving_epoch: Number(reply.serving_epoch) };
}

export async function positiveNativeReceiver(fixture: Fixture, raceCancellation = false): Promise<void> {
  const tool = await preparedTool(fixture, "native_fixture");
  await fixture.running();
  const cancellation = raceCancellation ? await prepareCancellation(fixture) : undefined;
  const now = Date.now();
  const prepared = await invokePrepared(fixture, tool, "native_constructor_fixture", {
    v: 1,
    now_ms: now,
    artifact_sha: ARTIFACT_SHA,
    artifact_tree: ARTIFACT_TREE,
    transport: identity,
    auth_values: {
      UOS_AI_TOKEN: "fixture-access",
      DENO_DEPLOY_TOKEN: "fixture-admin",
      DEEPSEEK_API_KEY: "fixture-provider-secret",
      LITHOSAI_API_KEY: "fixture-provider-secret",
    },
  });
  assert.equal(prepared.kind, "freeze_native_constructor_fixture");
  assert.equal(prepared.now_ms, now);
  assert.equal(prepared.artifact_sha, ARTIFACT_SHA);
  assert.equal(prepared.artifact_tree, ARTIFACT_TREE);
  assert.equal(prepared.constructors, "authenticate_client_actor/prepare_live_mapped_renewal/claim/start");
  assert(Array.isArray(prepared.rows) && prepared.rows.length > 0);
  const operation = object(prepared.operation);
  const publication = object(prepared.publication);
  for (const field of ["operation_id", "action_ref", "claim_ref", "publication_revision"]) assert(typeof operation[field] === "string");
  assert.equal(publication.operation_id, operation.operation_id);
  assert.equal(publication.action_ref, operation.action_ref);
  assert.equal(publication.publication_revision, operation.publication_revision);
  const root = await seedNativeRows(fixture, prepared.rows);
  const binding = {
    operation_id: operation.operation_id,
    action_ref: operation.action_ref,
    owner_id: identity.owner_id,
    owner_epoch: identity.owner_epoch,
    key_fingerprint: sha256(key),
  };
  const native = await fixture.acquire({ kind: "native_operation", binding }, root);
  const siblingBinding = { ...binding, operation_id: "unrelated-positive-operation", action_ref: "unrelated-positive-action" };
  const sibling = await fixture.acquire({ kind: "native_operation", binding: siblingBinding }, root);
  const heldJoin = await join(fixture, binding);
  const siblingJoin = await join(fixture, siblingBinding);
  const accounting = await fixture.root("positive-independent-required-settlement");
  await fixture.finish(accounting, "required_settlement");
  await fixture.finish(native, "native_effect_unknown");
  await fixture.finish(root);
  const current = fixture.current();
  await beginDrain(current.transport, current.plan);
  const frozen = pending(awaitFrozen(current.transport, current.plan));
  for (const stale of [
    { ...publication, source_revision: "wrong-source" },
    { ...publication, owner_revision: "wrong-owner" },
    { ...publication, previous_native_source_generation: "0".repeat(64) },
  ]) {
    const refused = await signed(fixture, "/uos/native/publication", stale);
    assert(refused.status >= 400, "A signed stale native fence was accepted");
    assert.equal((await fixture.status()).counts.native_operations, 2, "A refused publication retired an operation");
  }
  const published = await signed(fixture, "/uos/native/publication", publication);
  assert.equal(published.status, 200, JSON.stringify(published.body));
  assert.equal((await fixture.status()).counts.native_operations, 2, "Publication HTTP success alone retired the orphan");
  const raceKey = tuple("uos_ai", "freeze_fixture", "positive-native-race");
  const [race, terminal, cancelled] = await Promise.all([
    fixture.commit(heldJoin, raceKey, jsonRecord("positive-racing-write", { admitted: true })),
    signed(fixture, "/uos/native/renewal-authority", {
      v: 1,
      operation: "published",
      operation_id: operation.operation_id,
      claim_ref: operation.claim_ref,
      publication_revision: operation.publication_revision,
    }),
    cancellation ? cancelDrain(fixture.operatorTransport("cancel", cancellation.intent.token), cancellation.plan) : Promise.resolve(null),
  ]);
  assert.equal(terminal.status, 200, JSON.stringify(terminal.body));
  assert.equal(terminal.body.operation, "published");
  assert.equal(terminal.body.publication_revision, operation.publication_revision);
  const committed = race.status === 200;
  if (committed) assert.equal(object(await race.json()).committed, true);
  else await race.arrayBuffer();
  assert.equal((await fixture.exchange({ operation: "read", key: raceKey })).record !== null, committed);
  const late = await fixture.commit(heldJoin, raceKey, jsonRecord("forbidden-after-positive-terminal", {}));
  assert(late.status >= 400);
  await late.arrayBuffer();
  const remaining = await fixture.status();
  assert.equal(remaining.counts.native_operations, 1);
  assert.equal(remaining.counts.native_joins, 1);
  assert.equal(remaining.counts.blocked, 1);
  if (!cancellation) assert.equal(frozen.settled, false);
  await fixture.finish(siblingJoin);
  await fixture.finish(sibling);
  await fixture.finish(accounting);
  if (cancellation) await assert.rejects(frozen.promise, /drain_cancelled/u);
  const reference = cancellation ? await freezeAfterCancelledNative(fixture, cancellation) : await frozen.promise;
  fixture.facts.push({
    operation: "signed_native_receiver_terminal_retires_exact_orphan",
    positive_published_proof: true,
    stale_source_owner_generation_refused: true,
    http_publication_is_completion: false,
    late_write_refused: true,
    unrelated_blockers_retained: true,
    racing_commit_preceded: committed,
    frozen_ref: reference,
    raced_cancellation: raceCancellation,
    cancellation: cancelled?.cancellation ?? null,
  });
}

async function freezeAfterCancelledNative(fixture: Fixture, prepared: Awaited<ReturnType<typeof prepareCancellation>>) {
  const status = await fixture.status();
  assert.equal(status.gate.phase, "running", "Native completion after cancellation minted Frozen");
  const transport = fixture.operatorTransport("next", prepared.nextIntent.token);
  await beginDrain(transport, prepared.next);
  return awaitFrozen(transport, prepared.next);
}

async function seedNativeRows(fixture: Fixture, rows: unknown[]): Promise<Permit> {
  const root = await fixture.root("typed-native-fixture-origin");
  const seed = await fixture.rpc(
    {
      operation: "commit",
      checks: [],
      mutations: rows.map((entry) => {
        const row = object(entry);
        assert(typeof row.key === "string");
        return { operation: "put", key: row.key, record: object(row.record) };
      }),
    },
    { kind: "permit", value: root }
  );
  assert.equal(seed.status, 200, await seed.clone().text());
  assert.equal(object(await seed.json()).committed, true);
  return root;
}
