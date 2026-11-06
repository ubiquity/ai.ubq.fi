/** Purpose-bound controls for the private controller; no CLI entrypoint or ambient configuration. */
import { randomBytes } from "node:crypto";
import { canonical, INTENT_HEADER, sha256, type Transport } from "./state-transfer.ts";

export type Identity = Readonly<{ object_id: string; source_sha: string; source_tree: string }>;
export type Binding = Readonly<{ identity: Identity; serving_epoch: number }>;
export type DrainPlan = Readonly<{ binding: Binding; drain_revision: number }>;
export type CancelDrainPlan = Readonly<DrainPlan & { drain_digest: string }>;
export type DrainCancellation = Readonly<{ plan: CancelDrainPlan; next_drain_revision: number; cancel_digest: string; cancellation_revision: string }>;

export type Owner = Readonly<{ owner_id: string; owner_epoch: string; key_fingerprint: string; owner_revision: string }>;
export type FinalizePlan = Readonly<{
  binding: Binding;
  capture_id: string;
  staged_records: number;
  final_chain_digest: string;
  manifest_revision: string;
  expected_owner: Readonly<{ kind: "absent" } | { kind: "bound"; binding: Owner }>;
}>;
export type FrozenRef = Readonly<{
  identity: Identity;
  serving_epoch: number;
  freeze_id: string;
  freeze_revision: string;
  capture_id: string;
  finalization_digest: string;
  roots: 0;
  native_operations: 0;
  native_joins: 0;
  import_frames: 0;
  blocked: 0;
}>;
export type Authorization = Readonly<
  | { purpose: "finalize_import"; data: FinalizePlan }
  | { purpose: "drain"; data: DrainPlan }
  | { purpose: "cancel_drain"; data: CancelDrainPlan }
  | { purpose: "activate"; data: FrozenRef }
>;

export const authorizationDigest = (authorization: Authorization): string => sha256(`uos-state-transfer-authorization-v2\0${canonical(authorization)}`);

export const mintControlIntent = (authorization: Authorization): Readonly<{ token: string; sha256: string }> => {
  const token = `uos-state-transfer-v2.${authorization.purpose}.${authorizationDigest(authorization)}.${randomBytes(32).toString("hex")}`;
  return { token, sha256: sha256(token) };
};

const control = async (transport: Transport, request: object): Promise<unknown> => {
  const body = JSON.stringify({ v: 2, request });
  if (Buffer.byteLength(body) > 65_536) throw new Error("Control request exceeds its bound");
  const response = await transport.fetch(`${transport.origin}/admin/state-transfer/control`, {
    method: "POST",
    headers: { authorization: `Bearer ${transport.adminToken}`, [INTENT_HEADER]: transport.intent, "content-type": "application/json" },
    body,
    redirect: "error",
  });
  if (response.status !== 200 && response.status !== 409) throw new Error(`Control did not establish its requested result (HTTP ${response.status})`);
  const reader = response.body?.getReader();
  if (!reader) throw new Error("Control reply body is absent");
  const chunks: Uint8Array[] = [];
  let bytes = 0;
  try {
    for (;;) {
      const item = await reader.read();
      if (item.done) break;
      bytes += item.value.byteLength;
      if (bytes > 65_536) throw new Error("Control reply exceeds its bound");
      chunks.push(item.value);
    }
    const reply: unknown = JSON.parse(Buffer.concat(chunks).toString("utf8"));
    if (response.status === 409 && !tagged(reply, "drain_cancelled") && !tagged(reply, "drain_stale"))
      throw new Error("Control refused the operation (HTTP409)");
    return reply;
  } finally {
    await reader.cancel();
    reader.releaseLock();
  }
};

const tagged = (reply: unknown, operation: string): boolean =>
  typeof reply === "object" && reply !== null && "operation" in reply && reply.operation === operation;

const record = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value);
const hexField = (value: Record<string, unknown>, name: string, size: number): string => {
  const field = value[name];
  if (typeof field !== "string" || !new RegExp(`^[a-f0-9]{${size}}$`).test(field)) throw new Error(`Control ${name} is malformed`);
  return field;
};
const frozenReference = (reply: unknown): FrozenRef => {
  if (!tagged(reply, "frozen") || !record(reply) || !record(reply.reference)) throw new Error("Control did not establish Frozen");
  const value = reply.reference;
  if (!record(value.identity) || !Number.isSafeInteger(value.serving_epoch) || Number(value.serving_epoch) < 0) throw new Error("Frozen identity is malformed");
  for (const key of ["roots", "native_operations", "native_joins", "import_frames", "blocked"])
    if (value[key] !== 0) throw new Error("Frozen has outstanding writers");
  return {
    identity: {
      object_id: hexField(value.identity, "object_id", 64),
      source_sha: hexField(value.identity, "source_sha", 40),
      source_tree: hexField(value.identity, "source_tree", 40),
    },
    serving_epoch: Number(value.serving_epoch),
    freeze_id: hexField(value, "freeze_id", 64),
    freeze_revision: hexField(value, "freeze_revision", 64),
    capture_id: hexField(value, "capture_id", 64),
    finalization_digest: hexField(value, "finalization_digest", 64),
    roots: 0,
    native_operations: 0,
    native_joins: 0,
    import_frames: 0,
    blocked: 0,
  };
};
const checkBinding = (reference: FrozenRef, binding: Binding, transport: Transport): void => {
  if (
    canonical(reference.identity) !== canonical(binding.identity) ||
    reference.serving_epoch !== binding.serving_epoch ||
    reference.freeze_id !== sha256(transport.intent)
  )
    throw new Error("Frozen reply belongs to another source, object, epoch or operation");
};

export const readStatus = (transport: Transport): Promise<unknown> => control(transport, { operation: "status" });

export const finalizeImport = async (transport: Transport, plan: FinalizePlan): Promise<FrozenRef> => {
  const reply = await control(transport, { operation: "finalize_import", plan });
  const reference = frozenReference(reply);
  checkBinding(reference, plan.binding, transport);
  if (reference.capture_id !== plan.capture_id || reference.finalization_digest !== sha256(transport.intent)) throw new Error("Frozen names another import");
  return reference;
};

export const beginDrain = (transport: Transport, plan: DrainPlan): Promise<unknown> => control(transport, { operation: "drain", plan });

export const awaitFrozen = async (transport: Transport, plan: DrainPlan): Promise<FrozenRef> => {
  const reply = await control(transport, { operation: "await_frozen", plan });
  if (tagged(reply, "drain_cancelled") || tagged(reply, "drain_stale"))
    throw new Error(`Drain did not freeze: ${String(record(reply) ? reply.operation : "invalid")}`);
  const reference = frozenReference(reply);
  checkBinding(reference, plan.binding, transport);
  return reference;
};

export const activate = async (transport: Transport, reference: FrozenRef): Promise<unknown> => {
  const reply = await control(transport, { operation: "activate", reference });
  if (
    !tagged(reply, "activated") ||
    !record(reply) ||
    !record(reply.status) ||
    !record(reply.status.gate) ||
    reply.status.gate.phase !== "running" ||
    reply.serving_epoch !== reference.serving_epoch + 1
  )
    throw new Error("Activation did not establish its recorded running epoch");
  return reply;
};

export const cancellationRevision = (value: Omit<DrainCancellation, "cancellation_revision">): string =>
  sha256(
    JSON.stringify([
      "uos-drain-cancellation-v1",
      value.plan.binding.identity.object_id,
      value.plan.binding.identity.source_sha,
      value.plan.binding.identity.source_tree,
      value.plan.binding.serving_epoch,
      value.plan.drain_revision,
      value.next_drain_revision,
      value.plan.drain_digest,
      value.cancel_digest,
    ])
  );

export const cancelDrain = async (
  transport: Transport,
  plan: CancelDrainPlan
): Promise<Readonly<{ cancellation: DrainCancellation; replayed: boolean; status: unknown }>> => {
  const reply = await control(transport, { operation: "cancel_drain", plan });
  if (
    !record(reply) ||
    !tagged(reply, "cancelled") ||
    !record(reply.cancellation) ||
    typeof reply.replayed !== "boolean" ||
    !record(reply.status) ||
    !record(reply.status.gate)
  )
    throw new Error("Control did not establish cancellation");
  const raw = reply.cancellation;
  if (
    canonical(raw.plan) !== canonical(plan) ||
    raw.cancel_digest !== sha256(transport.intent) ||
    !Number.isSafeInteger(raw.next_drain_revision) ||
    raw.next_drain_revision !== plan.drain_revision + 1 ||
    raw.next_drain_revision < 0
  )
    throw new Error("Cancellation names another drain or revision");
  const cancellation: DrainCancellation = {
    plan,
    next_drain_revision: raw.next_drain_revision,
    cancel_digest: hexField(raw, "cancel_digest", 64),
    cancellation_revision: hexField(raw, "cancellation_revision", 64),
  };
  if (cancellation.cancellation_revision !== cancellationRevision(cancellation)) throw new Error("Cancellation receipt digest differs");
  const gate = reply.status.gate;
  if (
    gate.phase !== "running" ||
    canonical(gate.identity) !== canonical(plan.binding.identity) ||
    gate.serving_epoch !== plan.binding.serving_epoch ||
    gate.drain_revision !== cancellation.next_drain_revision ||
    gate.drain_digest !== null ||
    canonical(gate.last_cancellation) !== canonical(cancellation)
  )
    throw new Error("Cancellation did not establish the same Running authority");
  return { cancellation, replayed: reply.replayed, status: reply.status };
};
