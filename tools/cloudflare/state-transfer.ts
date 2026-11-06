/** Operator side of the authenticated Worker state transfer (DECISIONS 2026-11-05).
 * It streams already-staged frames to one Worker and pages its export back. It reads
 * only the admin token and one-time intent from this command's own environment, never
 * a dotenv file, and it never opens a database, refreshes a credential or deploys.
 */
import { createHash, createHmac, randomBytes } from "node:crypto";
import { closeSync, constants, fsyncSync, lstatSync, openSync, readFileSync, writeSync } from "node:fs";

export const INTENT_HEADER = "x-uos-state-transfer-intent";
export const IMPORT_PATH = "/admin/state-transfer/import";
export const EXPORT_PATH = "/admin/state-transfer/export";
const FRAME_MAX_BYTES = 32 * 1024 * 1024;

type ObjectValue = Record<string, unknown>;
type Fetch = (url: string, init: RequestInit) => Promise<Response>;
export type Transport = Readonly<{ origin: string; adminToken: string; intent: string; fetch: Fetch; attempts?: number }>;
export type Frame = Readonly<{ stage: "begin" | "append" | "seal" | "bind_owner"; sequence?: number; digest?: string; raw: string }>;
export type ImportStatus = Readonly<{
  state: "empty" | "staging" | "sealed" | "occupied";
  capture_id: string | null;
  next_sequence: number;
  received_records: number;
  expected_records: number;
  digest: string | null;
  activation_authorized: boolean;
}>;

const isObject = (value: unknown): value is ObjectValue => typeof value === "object" && value !== null && !Array.isArray(value);
const hex = (value: unknown, length: number): value is string => typeof value === "string" && new RegExp(`^[a-f0-9]{${length}}$`).test(value);
export const sha256 = (bytes: string | Uint8Array): string => createHash("sha256").update(bytes).digest("hex");

// Locale-independent UTF-16 code-unit order, so a receipt signs identically everywhere.
const byCodeUnit = (left: string, right: string): number => Number(left > right) - Number(left < right);
const canonical = (value: unknown): string => {
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (isObject(value))
    return `{${Object.keys(value)
      .sort(byCodeUnit)
      .map((key) => `${JSON.stringify(key)}:${canonical(value[key])}`)
      .join(",")}}`;
  return JSON.stringify(value);
};
/** HMAC-SHA256 keyed by the one-time intent: only its holder can produce or check it. */
export const signReceipt = <T extends ObjectValue>(intent: string, receipt: T): T & { hmac_sha256: string } => ({
  ...receipt,
  hmac_sha256: createHmac("sha256", intent).update(canonical(receipt)).digest("hex"),
});
export const verifyReceipt = (intent: string, receipt: ObjectValue): boolean => {
  const { hmac_sha256: mac, ...body } = receipt;
  return typeof mac === "string" && signReceipt(intent, body).hmac_sha256 === mac;
};

export const mintIntent = (scope: Readonly<{ kind: "import"; captureId: string } | { kind: "export" }>): Readonly<{ token: string; sha256: string }> => {
  const nonce = randomBytes(32).toString("hex");
  if (scope.kind === "import" && !hex(scope.captureId, 64)) throw new Error("Import intent requires a 64-hex capture id");
  const token = scope.kind === "import" ? `uos-state-transfer-v1.import.${scope.captureId}.${nonce}` : `uos-state-transfer-v1.export.${nonce}`;
  return { token, sha256: sha256(token) };
};

const privateFile = (path: string): Buffer => {
  const stat = lstatSync(path);
  if (!stat.isFile() || (stat.mode & 0o077) !== 0) throw new Error(`${path} must be an owner-only regular file`);
  return readFileSync(path);
};
export const writeNewPrivate = (path: string, bytes: string | Uint8Array): void => {
  const fd = openSync(path, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
  try {
    const data = typeof bytes === "string" ? Buffer.from(bytes) : bytes;
    let offset = 0;
    while (offset < data.length) offset += writeSync(fd, data, offset, data.length - offset);
    fsyncSync(fd);
  } finally {
    closeSync(fd);
  }
};

const frameCapture = (stage: string, data: ObjectValue): string => {
  const id = stage === "begin" && isObject(data.manifest) ? data.manifest.capture_id : data.capture_id;
  if (!hex(id, 64)) throw new Error("Stage frame has no capture identity");
  return id;
};
const nextFrame = (last: Frame["stage"] | undefined, appends: number, stage: string, data: ObjectValue, raw: string): Frame => {
  if (stage === "begin" && last === undefined) return { stage: "begin", raw };
  if (stage === "append" && (last === "begin" || last === "append") && data.sequence === appends && hex(data.digest, 64))
    return { stage: "append", sequence: appends, digest: data.digest, raw };
  if (stage === "seal" && last === "append") return { stage: "seal", raw };
  if (stage === "bind_owner" && last === "seal") return { stage: "bind_owner", raw };
  throw new Error("Stage frame order is invalid");
};

/** Frames are the exact native-stage-frames output: begin, ordered appends, seal, optional owner. */
export const parseFrames = (bytes: Uint8Array, expectedSha256: string, expectedFrames: number): Readonly<{ captureId: string; frames: Frame[] }> => {
  if (sha256(bytes) !== expectedSha256) throw new Error("Stage frames hash differs from the operator receipt");
  const text = Buffer.from(bytes).toString("utf8");
  if (!text.endsWith("\n")) throw new Error("Stage frames file is truncated");
  const frames: Frame[] = [];
  let captureId: string | undefined;
  let appends = 0;
  for (const raw of text.slice(0, -1).split("\n")) {
    if (Buffer.byteLength(raw) > FRAME_MAX_BYTES) throw new Error("Stage frame exceeds its bound");
    const value: unknown = JSON.parse(raw);
    if (!isObject(value) || !isObject(value.data) || typeof value.stage !== "string") throw new Error("Stage frame is malformed");
    const id = frameCapture(value.stage, value.data);
    if (captureId !== undefined && id !== captureId) throw new Error("Stage frames name more than one capture");
    captureId = id;
    const frame = nextFrame(frames.at(-1)?.stage, appends, value.stage, value.data, raw);
    if (frame.stage === "append") appends++;
    frames.push(frame);
  }
  if (frames.length !== expectedFrames || !frames.some((frame) => frame.stage === "seal") || captureId === undefined)
    throw new Error("Stage frame inventory differs from the operator receipt");
  return { captureId, frames };
};

const transient = (status: number): boolean => status === 429 || status >= 500;
const request = async (transport: Transport, method: "GET" | "POST", path: string, body?: string): Promise<Response> => {
  const attempts = transport.attempts ?? 4;
  for (let attempt = 1; ; attempt++) {
    try {
      const response = await transport.fetch(`${transport.origin}${path}`, {
        method,
        headers: { authorization: `Bearer ${transport.adminToken}`, [INTENT_HEADER]: transport.intent, "content-type": "application/json" },
        body,
        signal: AbortSignal.timeout(120_000),
      });
      if (response.ok) return response;
      const detail = await response.text();
      if (!transient(response.status) || attempt >= attempts) throw new RefusedError(response.status, detail.slice(0, 2000));
    } catch (error) {
      if (error instanceof RefusedError || attempt >= attempts) throw error;
    }
    await Bun.sleep(250 * attempt);
  }
};
export class RefusedError extends Error {
  constructor(
    readonly status: number,
    readonly detail: string
  ) {
    super(`Worker refused with HTTP ${status}: ${detail}`);
  }
}

const parseStatus = (value: unknown): ImportStatus => {
  if (!isObject(value) || value.activation_authorized !== false || typeof value.state !== "string") throw new Error("Worker import status is malformed");
  return value as ImportStatus;
};
export const importStatus = async (transport: Transport): Promise<ImportStatus> => parseStatus(await (await request(transport, "GET", IMPORT_PATH)).json());

/** Where to continue: the reported next chunk, after checking the chain digest the Worker holds. */
export const resumeIndex = (status: ImportStatus, frames: readonly Frame[], captureId: string): number => {
  if (status.state === "occupied") throw new Error("Target holds rows without a staged capture; import requires a new empty authority");
  if (status.state === "empty") return 0;
  if (status.capture_id !== captureId) throw new Error("Target holds a different capture");
  const seal = frames.findIndex((frame) => frame.stage === "seal");
  if (status.state === "sealed") return seal + 1;
  const next = frames.findIndex((frame) => frame.stage === "append" && frame.sequence === status.next_sequence);
  const index = next === -1 ? seal : next;
  const previous = frames[index - 1];
  if (previous.stage === "append" && previous.digest !== (status.digest ?? undefined)) throw new Error("Target staging digest differs from these frames");
  return index;
};

const replyOutcome = (reply: unknown, frame: Frame): "applied" | "replayed" => {
  if (!isObject(reply) || reply.stage !== frame.stage || (reply.outcome !== "applied" && reply.outcome !== "replayed"))
    throw new Error("Worker import reply is malformed");
  const status = parseStatus(reply.status);
  if (frame.stage === "append" && status.state === "staging" && status.next_sequence !== (frame.sequence ?? -1) + 1)
    throw new Error("Worker did not advance to the next chunk");
  return reply.outcome;
};

export type ImportResult = Readonly<{ applied: number; replayed: number; resumed_at: number; status: ImportStatus }>;
export const importFrames = async (transport: Transport, frames: readonly Frame[], captureId: string): Promise<ImportResult> => {
  let index = resumeIndex(await importStatus(transport), frames, captureId);
  const resumedAt = index;
  let applied = 0;
  let replayed = 0;
  let recoveries = 0;
  while (index < frames.length) {
    const frame = frames[index];
    let reply: unknown;
    try {
      reply = await (await request(transport, "POST", IMPORT_PATH, frame.raw)).json();
    } catch (error) {
      // An unknown delivery outcome is resolved only by the Worker's own status.
      if (error instanceof RefusedError || ++recoveries > 3) throw error;
      index = resumeIndex(await importStatus(transport), frames, captureId);
      continue;
    }
    if (replyOutcome(reply, frame) === "applied") applied++;
    else replayed++;
    index++;
  }
  const status = await importStatus(transport);
  if (status.state !== "sealed" || status.capture_id !== captureId || status.received_records !== status.expected_records)
    throw new Error("Worker target is not sealed with the complete capture");
  return { applied, replayed, resumed_at: resumedAt, status };
};

export type ExportResult = Readonly<{ archive: Uint8Array; records: number; pages: { sha256: string; count: number }[]; sha256: string }>;
export const exportPages = async (transport: Transport, limit = 256): Promise<ExportResult> => {
  const chunks: Uint8Array[] = [];
  const pages: { sha256: string; count: number }[] = [];
  let cursor: string | null = null;
  let records = 0;
  do {
    const response = await request(transport, "POST", EXPORT_PATH, JSON.stringify({ v: 1, cursor, limit }));
    const body = new Uint8Array(await response.arrayBuffer());
    const digest = response.headers.get("x-uos-state-export-sha256");
    const count = Number(response.headers.get("x-uos-state-export-count"));
    const rows: unknown = JSON.parse(Buffer.from(body).toString("utf8"));
    if (digest !== sha256(body) || !Array.isArray(rows) || rows.length !== count) throw new Error("Worker export page hash or count differs");
    pages.push({ sha256: digest, count });
    records += count;
    chunks.push(body, new Uint8Array([10]));
    cursor = response.headers.get("x-uos-state-export-next-cursor");
    if (pages.length > 100_000) throw new Error("Worker export did not terminate");
  } while (cursor !== null);
  const archive = Buffer.concat(chunks);
  return { archive, records, pages, sha256: sha256(archive) };
};

const flags = (args: readonly string[]): Map<string, string> => {
  const result = new Map<string, string>();
  if (args.length % 2 !== 0) throw new Error("Expected unique --flag value pairs");
  for (let index = 0; index < args.length; index += 2) {
    const key = args[index];
    const value = args[index + 1];
    if (!key.startsWith("--") || value.startsWith("--") || result.has(key.slice(2))) throw new Error("Expected unique --flag value pairs");
    result.set(key.slice(2), value);
  }
  return result;
};
const required = (values: Map<string, string>, name: string): string => {
  const value = values.get(name);
  if (!value) throw new Error(`--${name} is required`);
  return value;
};
const json = (path: string): ObjectValue => {
  const value: unknown = JSON.parse(privateFile(path).toString("utf8"));
  if (!isObject(value)) throw new Error(`${path} is not a JSON object`);
  return value;
};
const exactOrigin = (origin: string): string => {
  const url = new URL(origin);
  const loopback = url.hostname === "127.0.0.1" || url.hostname === "localhost";
  if (url.origin !== origin || (url.protocol !== "https:" && !(url.protocol === "http:" && loopback)))
    throw new Error("--origin must be an exact https origin");
  return origin;
};
export const transportFromEnvironment = (origin: string, environment: Record<string, string | undefined>, fetcher: Fetch = fetch): Transport => {
  exactOrigin(origin);
  const adminToken = environment.UOS_STATE_TRANSFER_ADMIN_TOKEN;
  const intent = environment.UOS_STATE_TRANSFER_INTENT;
  if (!adminToken || !intent) throw new Error("UOS_STATE_TRANSFER_ADMIN_TOKEN and UOS_STATE_TRANSFER_INTENT must be set for this one command");
  return { origin, adminToken, intent, fetch: fetcher };
};

type Environment = Record<string, string | undefined>;
const now = (): string => new Date().toISOString();

const intentCommand = (values: Map<string, string>): ObjectValue => {
  const scope = required(values, "scope");
  if (scope !== "export" && scope !== "import") throw new Error("--scope must be import or export");
  const minted = scope === "export" ? mintIntent({ kind: "export" }) : mintIntent({ kind: "import", captureId: required(values, "capture-id") });
  writeNewPrivate(required(values, "out"), minted.token);
  return { v: 1, kind: "state_transfer_intent", scope, setting: "UOS_STATE_TRANSFER_INTENT_SHA256", sha256: minted.sha256 };
};

const importCommand = async (values: Map<string, string>, environment: Environment, fetcher: Fetch): Promise<ObjectValue> => {
  const transport = transportFromEnvironment(required(values, "origin"), environment, fetcher);
  const stage = json(required(values, "frames-receipt"));
  const summary = stage.summary;
  if (stage.kind !== "m20_stage_frames" || !isObject(summary) || typeof stage.frames !== "number") throw new Error("Stage frames receipt is invalid");
  const bytes = privateFile(required(values, "frames"));
  if (bytes.length !== stage.frame_file_bytes) throw new Error("Stage frames size differs from the operator receipt");
  const { captureId, frames } = parseFrames(bytes, required(values, "frames-sha256"), stage.frames);
  if (summary.capture_id !== captureId || !transport.intent.startsWith(`uos-state-transfer-v1.import.${captureId}.`))
    throw new Error("Intent, frames and receipt name different captures");
  const startedAt = now();
  const result = await importFrames(transport, frames, captureId);
  const receipt = signReceipt(transport.intent, {
    v: 1,
    kind: "state_transfer_import",
    origin: transport.origin,
    capture_id: captureId,
    frames_sha256: sha256(bytes),
    frames: frames.length,
    expected_records: summary.records,
    ...result,
    started_at: startedAt,
    finished_at: now(),
    activation_authorized: false,
  });
  writeNewPrivate(required(values, "receipt"), `${JSON.stringify(receipt, null, 2)}\n`);
  return receipt;
};

const exportCommand = async (values: Map<string, string>, environment: Environment, fetcher: Fetch): Promise<ObjectValue> => {
  const transport = transportFromEnvironment(required(values, "origin"), environment, fetcher);
  if (!transport.intent.startsWith("uos-state-transfer-v1.export.")) throw new Error("UOS_STATE_TRANSFER_INTENT is not an export intent");
  const passes = Number(values.get("passes") ?? "2");
  if (!Number.isSafeInteger(passes) || passes < 1 || passes > 5) throw new Error("--passes must be 1..5");
  const startedAt = now();
  const first = await exportPages(transport);
  // Paged reads are not one snapshot: equal repeated passes prove the target was quiescent.
  for (let pass = 1; pass < passes; pass++)
    if ((await exportPages(transport)).sha256 !== first.sha256) throw new Error("Worker state changed between export passes; quiesce the target");
  writeNewPrivate(required(values, "archive"), first.archive);
  const receipt = signReceipt(transport.intent, {
    v: 1,
    kind: "state_transfer_export",
    origin: transport.origin,
    archive_sha256: first.sha256,
    archive_bytes: first.archive.length,
    records: first.records,
    pages: first.pages,
    stable_passes: passes,
    started_at: startedAt,
    finished_at: now(),
    activation_authorized: false,
  });
  writeNewPrivate(required(values, "receipt"), `${JSON.stringify(receipt, null, 2)}\n`);
  return receipt;
};

const VERIFY_INPUT_MAX_BYTES = 4096;
const VERIFY_INPUT_SCHEMA = 'verify requires private stdin JSON {"v":1,"import_intent":"<import intent>","export_intent":"<export intent>"}';
type VerificationIntents = Readonly<{ v: 1; import_intent: string; export_intent: string }>;
const verificationIntents = (text: string | undefined): VerificationIntents => {
  if (!text || Buffer.byteLength(text) > VERIFY_INPUT_MAX_BYTES) throw new Error(VERIFY_INPUT_SCHEMA);
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    throw new Error(VERIFY_INPUT_SCHEMA);
  }
  if (
    !isObject(value) ||
    Object.keys(value).length !== 3 ||
    value.v !== 1 ||
    typeof value.import_intent !== "string" ||
    typeof value.export_intent !== "string"
  )
    throw new Error(VERIFY_INPUT_SCHEMA);
  if (
    !/^uos-state-transfer-v1\.import\.[a-f0-9]{64}\.[a-f0-9]{64}$/.test(value.import_intent) ||
    !/^uos-state-transfer-v1\.export\.[a-f0-9]{64}$/.test(value.export_intent)
  )
    throw new Error("Verification intents have invalid or swapped scopes");
  return value as VerificationIntents;
};

/** Private verification input is bounded before JSON parsing and never read from an environment token. */
export const readVerificationInput = async (stream: ReadableStream<Uint8Array>): Promise<string> => {
  const reader = stream.getReader();
  const chunks: Uint8Array[] = [];
  let bytes = 0;
  try {
    for (;;) {
      const next = await reader.read();
      if (next.done) break;
      bytes += next.value.length;
      if (bytes > VERIFY_INPUT_MAX_BYTES) throw new Error(VERIFY_INPUT_SCHEMA);
      chunks.push(next.value);
    }
    return Buffer.concat(chunks).toString("utf8");
  } finally {
    await reader.cancel();
    reader.releaseLock();
  }
};

const archiveManifest = (row: unknown, manifest: unknown, captureId: string): boolean => {
  if (!isObject(row) || typeof row.key !== "string" || !isObject(row.record)) throw new Error("Export archive row is malformed");
  return row.record.revision === `capture:${captureId}` && canonical(row.record.value) === canonical(manifest);
};
const verifyArchive = (archive: Uint8Array, receipt: ObjectValue, manifest: unknown, captureId: string): void => {
  if (receipt.archive_sha256 !== sha256(archive) || receipt.archive_bytes !== archive.length) throw new Error("Export archive hash or size differs");
  const text = Buffer.from(archive).toString("utf8");
  if (!text.endsWith("\n") || !Array.isArray(receipt.pages)) throw new Error("Export archive or page inventory is malformed");
  const pages = text.slice(0, -1).split("\n");
  if (pages.length !== receipt.pages.length) throw new Error("Export archive page inventory differs");
  let records = 0;
  let manifests = 0;
  for (const [index, page] of pages.entries()) {
    const fact: unknown = receipt.pages[index];
    const rows: unknown = JSON.parse(page);
    if (!isObject(fact) || fact.sha256 !== sha256(page) || !Array.isArray(rows) || fact.count !== rows.length)
      throw new Error("Export archive page hash or count differs");
    records += rows.length;
    for (const row of rows) manifests += Number(archiveManifest(row, manifest, captureId));
  }
  if (records !== receipt.records || manifests !== 1) throw new Error("Export archive count or capture manifest differs");
};

const verifyCommand = (values: Map<string, string>, privateInput: string | undefined): ObjectValue => {
  const intents = verificationIntents(privateInput);
  const stage = json(required(values, "frames-receipt"));
  const imported = json(required(values, "import-receipt"));
  const exported = json(required(values, "export-receipt"));
  if (!verifyReceipt(intents.import_intent, imported) || !verifyReceipt(intents.export_intent, exported)) throw new Error("Receipt signature differs");
  const origin = exactOrigin(required(values, "origin"));
  if (imported.origin !== origin || exported.origin !== origin) throw new Error("Receipt origin differs from the expected target");
  const summary = isObject(stage.summary) ? stage.summary : {};
  if (stage.kind !== "m20_stage_frames" || !Number.isSafeInteger(stage.frames) || !Number.isSafeInteger(summary.records) || Number(summary.records) < 0)
    throw new Error("Stage frames receipt is invalid");
  const bytes = privateFile(required(values, "frames"));
  const parsed = parseFrames(bytes, required(values, "frames-sha256"), Number(stage.frames));
  const begin = JSON.parse(parsed.frames[0].raw) as { data: { manifest: unknown; records: number } };
  if (
    stage.frame_file_bytes !== bytes.length ||
    summary.capture_id !== parsed.captureId ||
    summary.records !== begin.data.records ||
    stage.owner_frame !== (parsed.frames.at(-1)?.stage === "bind_owner")
  )
    throw new Error("Stage receipt differs from the actual frames");
  if (!intents.import_intent.startsWith(`uos-state-transfer-v1.import.${parsed.captureId}.`)) throw new Error("Import intent names another capture");
  if (
    imported.v !== 1 ||
    imported.kind !== "state_transfer_import" ||
    exported.v !== 1 ||
    exported.kind !== "state_transfer_export" ||
    imported.activation_authorized !== false ||
    exported.activation_authorized !== false
  )
    throw new Error("Receipt kinds or activation evidence are invalid");
  if (
    imported.capture_id !== parsed.captureId ||
    imported.frames_sha256 !== sha256(bytes) ||
    imported.frames !== parsed.frames.length ||
    imported.expected_records !== summary.records
  )
    throw new Error("Import receipt capture, frame hash or inventory differs");
  const status = isObject(imported.status) ? imported.status : {};
  if (
    status.state !== "sealed" ||
    status.capture_id !== parsed.captureId ||
    status.received_records !== summary.records ||
    status.expected_records !== summary.records ||
    status.activation_authorized !== false
  )
    throw new Error("Import receipt does not prove a sealed complete capture");
  if (!Number.isSafeInteger(exported.stable_passes) || Number(exported.stable_passes) < 2 || Number(exported.stable_passes) > 5)
    throw new Error("Export receipt requires at least two stable passes");
  // Immediately after import: every staged record, plus the sealed manifest, plus an owner row if bound.
  const expected = summary.records + 1 + (stage.owner_frame ? 1 : 0);
  if (exported.records !== expected) throw new Error(`Export has ${String(exported.records)} records; the staged import implies ${expected}`);
  verifyArchive(privateFile(required(values, "archive")), exported, begin.data.manifest, parsed.captureId);
  return {
    v: 1,
    kind: "state_transfer_verify",
    origin,
    capture_id: parsed.captureId,
    records: expected,
    frames_sha256: sha256(bytes),
    archive_sha256: exported.archive_sha256,
    stable_passes: exported.stable_passes,
    matched: true,
  };
};

export const main = async (args: readonly string[], environment: Environment, fetcher: Fetch = fetch, privateInput?: string): Promise<ObjectValue> => {
  const [command, ...rest] = args;
  const values = flags(rest);
  if (command === "intent") return intentCommand(values);
  if (command === "import") return await importCommand(values, environment, fetcher);
  if (command === "export") return await exportCommand(values, environment, fetcher);
  if (command === "verify") return verifyCommand(values, privateInput);
  throw new Error("usage: state-transfer.ts intent|import|export|verify --flag value ...");
};

if (import.meta.main) {
  const args = Bun.argv.slice(2);
  const input = args[0] === "verify" && !process.stdin.isTTY ? readVerificationInput(Bun.stdin.stream()) : Promise.resolve(undefined);
  input
    .then((privateInput) => main(args, process.env, fetch, privateInput))
    .then(
      (receipt) => {
        console.log(JSON.stringify(receipt, null, 2));
      },
      (error: unknown) => {
        console.error(error instanceof Error ? error.message : "State transfer failed");
        process.exitCode = 1;
      }
    );
}
