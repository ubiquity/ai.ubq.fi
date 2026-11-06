import { createHash } from "node:crypto";
import { isAbsolute, join } from "node:path";

type Row = Record<string, unknown>;
export type NativeOwnerHome = Readonly<{ owner_id: string; codex_home: string }>;
export type NativeOwnerConfig = Readonly<{
  v: 1;
  release: { git_sha: string; git_tree: string; manifest_sha256: string };
  control: { key_id: string; owner_id: string; owner_epoch: string; key_fingerprint: string; owner_revision: string };
  worker: { origin: string; git_sha: string; git_tree: string; namespace_id: string; object_id: string; receipt_sha256: string };
  owners: readonly NativeOwnerHome[];
}>;
export type NativeOwnerCarrier = Readonly<{
  origin: string;
  identity: { key_id: string; owner_id: string; owner_epoch: string; key_base64url: string; capabilities: readonly string[] };
  renewal_owners: readonly NativeOwnerHome[];
}>;

const fail = (): never => {
  throw new Error("Native owner configuration or binding is invalid");
};
export const nativeOwnerRecord = (value: unknown, keys: readonly string[]): Row => {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return fail();
  const row = value as Row;
  if (
    Object.keys(row)
      .sort((a, b) => a.localeCompare(b))
      .join(",") !== [...keys].sort((a, b) => a.localeCompare(b)).join(",")
  )
    return fail();
  return row;
};
const text = (value: unknown): string => (typeof value === "string" && value.length > 0 && value.length <= 4096 ? value : fail());
const hex = (value: unknown, length: number): string => {
  const result = text(value);
  return new RegExp("^[0-9a-f]{" + String(length) + "}$").test(result) ? result : fail();
};
const identifier = (value: unknown, key = false): string => {
  const result = text(value);
  const pattern = key ? /^[A-Za-z0-9_-]{1,64}$/ : /^[A-Za-z0-9_.-]{1,128}$/;
  return pattern.test(result) ? result : fail();
};
const origin = (value: unknown): string => {
  const result = text(value);
  const url = new URL(result);
  if (url.protocol !== "https:" || url.username || url.password || url.port || url.pathname !== "/" || url.search || url.hash) return fail();
  return url.origin;
};
const homes = (value: unknown): NativeOwnerHome[] => {
  if (!Array.isArray(value) || value.length !== 2) return fail();
  const result = value.map((item) => {
    const row = nativeOwnerRecord(item, ["owner_id", "codex_home"]);
    const home = text(row.codex_home);
    if (!isAbsolute(home) || home.includes("\0")) return fail();
    return { owner_id: identifier(row.owner_id), codex_home: home };
  });
  if (new Set(result.map((r) => r.owner_id)).size !== 2 || new Set(result.map((r) => r.codex_home)).size !== 2) return fail();
  return result;
};
const source = (value: unknown): { git_sha: string; git_tree: string; manifest_sha256: string } => {
  const row = nativeOwnerRecord(value, ["git_sha", "git_tree", "manifest_sha256"]);
  return { git_sha: hex(row.git_sha, 40), git_tree: hex(row.git_tree, 40), manifest_sha256: hex(row.manifest_sha256, 64) };
};
export const parseNativeOwnerConfig = (value: unknown): NativeOwnerConfig => {
  const row = nativeOwnerRecord(value, ["v", "release", "control", "worker", "owners"]);
  if (row.v !== 1) return fail();
  const control = nativeOwnerRecord(row.control, ["key_id", "owner_id", "owner_epoch", "key_fingerprint", "owner_revision"]);
  const worker = nativeOwnerRecord(row.worker, ["origin", "git_sha", "git_tree", "namespace_id", "object_id", "receipt_sha256"]);
  return {
    v: 1,
    release: source(row.release),
    control: {
      key_id: identifier(control.key_id, true),
      owner_id: identifier(control.owner_id),
      owner_epoch: identifier(control.owner_epoch),
      key_fingerprint: hex(control.key_fingerprint, 64),
      owner_revision: text(control.owner_revision),
    },
    worker: {
      origin: origin(worker.origin),
      git_sha: hex(worker.git_sha, 40),
      git_tree: hex(worker.git_tree, 40),
      namespace_id: hex(worker.namespace_id, 32),
      object_id: hex(worker.object_id, 64),
      receipt_sha256: hex(worker.receipt_sha256, 64),
    },
    owners: homes(row.owners),
  };
};
export const parseNativeOwnerCarrier = (value: unknown): NativeOwnerCarrier => {
  const row = nativeOwnerRecord(value, ["origin", "identity", "renewal_owners"]);
  const identity = nativeOwnerRecord(row.identity, ["key_id", "owner_id", "owner_epoch", "key_base64url", "capabilities"]);
  if (origin(row.origin) !== "https://origin.ubq.fi") return fail();
  const key = text(identity.key_base64url);
  if (!/^[A-Za-z0-9_-]{43}$/.test(key) || Buffer.from(key, "base64url").length !== 32 || Buffer.from(key, "base64url").toString("base64url") !== key)
    return fail();
  const capabilities = identity.capabilities;
  const expected = ["access_publish", "owner_renewal", "supervisor_read"];
  if (!Array.isArray(capabilities) || !capabilities.every((item: unknown): item is string => typeof item === "string")) return fail();
  if ([...capabilities].sort((a, b) => a.localeCompare(b)).join(",") !== expected.join(",")) return fail();
  return {
    origin: "https://origin.ubq.fi",
    identity: {
      key_id: identifier(identity.key_id, true),
      owner_id: identifier(identity.owner_id),
      owner_epoch: identifier(identity.owner_epoch),
      key_base64url: key,
      capabilities: expected,
    },
    renewal_owners: homes(row.renewal_owners),
  };
};
export const nativeOwnerStdin = (root: string, config: NativeOwnerConfig, carrier: NativeOwnerCarrier): string => {
  const fingerprint = createHash("sha256").update(Buffer.from(carrier.identity.key_base64url, "base64url")).digest("hex");
  if (
    fingerprint !== config.control.key_fingerprint ||
    carrier.identity.key_id !== config.control.key_id ||
    carrier.identity.owner_id !== config.control.owner_id ||
    carrier.identity.owner_epoch !== config.control.owner_epoch ||
    JSON.stringify([...config.owners].sort((a, b) => a.owner_id.localeCompare(b.owner_id))) !==
      JSON.stringify([...carrier.renewal_owners].sort((a, b) => a.owner_id.localeCompare(b.owner_id)))
  )
    return fail();
  return JSON.stringify({
    v: 2,
    listen: "127.0.0.1:7990",
    identity: {
      key_id: carrier.identity.key_id,
      owner_id: carrier.identity.owner_id,
      owner_epoch: carrier.identity.owner_epoch,
      key_base64url: carrier.identity.key_base64url,
    },
    nonce_database: join(root, ".data/native-owner/replay/nonces.sqlite3"),
    sources: [],
    owner_renewal: { authority_origin: config.worker.origin + "/", owners: config.owners },
  });
};
