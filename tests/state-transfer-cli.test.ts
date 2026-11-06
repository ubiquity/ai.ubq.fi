import assert from "node:assert/strict";
import { test } from "bun:test";
import { chmodSync, lstatSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  EXPORT_PATH,
  IMPORT_PATH,
  INTENT_HEADER,
  exportPages,
  importFrames,
  main,
  mintIntent,
  parseFrames,
  readVerificationInput,
  resumeIndex,
  sha256,
  signReceipt,
  verifyReceipt,
  type ImportStatus,
  type Transport,
} from "../tools/cloudflare/state-transfer.ts";

const CAPTURE = "c".repeat(64);
const digest = (index: number): string => sha256(`chunk-${index}`);
const framesText = (appends: number, owner = false): string => {
  const lines = [JSON.stringify({ stage: "begin", data: { manifest: { capture_id: CAPTURE }, records: appends, digest: digest(appends - 1) } })];
  for (let sequence = 0; sequence < appends; sequence++)
    lines.push(
      JSON.stringify({ stage: "append", data: { capture_id: CAPTURE, sequence, previous_digest: "x", records: [{ n: sequence }], digest: digest(sequence) } })
    );
  lines.push(JSON.stringify({ stage: "seal", data: { capture_id: CAPTURE } }));
  if (owner) lines.push(JSON.stringify({ stage: "bind_owner", data: { capture_id: CAPTURE } }));
  return `${lines.join("\n")}\n`;
};

/** A small model of the Worker routes with the Rust replay rules; it can lose replies. */
class FakeWorker {
  state: ImportStatus["state"] = "empty";
  sequence = 0;
  digest: string | null = null;
  rows: unknown[] = [];
  writes = 0;
  loseReplyAfterWrite = new Set<number>();
  constructor(
    readonly appends: number,
    readonly token: string
  ) {}
  status(): ImportStatus {
    const staged = this.state === "staging" || this.state === "sealed";
    return {
      state: this.state,
      capture_id: staged ? CAPTURE : null,
      next_sequence: this.state === "staging" ? this.sequence : 0,
      received_records: this.state === "sealed" ? this.appends : this.sequence,
      expected_records: staged ? this.appends : 0,
      digest: this.state === "staging" ? this.digest : null,
      activation_authorized: false,
    };
  }
  frame(body: Record<string, unknown>): { outcome: string } {
    const data = body.data as Record<string, unknown>;
    if (this.state === "sealed") return { outcome: "replayed" };
    if (body.stage === "begin") {
      if (this.state === "staging") return { outcome: "replayed" };
      this.state = "staging";
      return { outcome: "applied" };
    }
    if (body.stage === "append") {
      if (data.sequence === this.sequence - 1 && data.digest === this.digest) return { outcome: "replayed" };
      if (data.sequence !== this.sequence) throw new Error("409");
      this.sequence++;
      this.digest = data.digest as string;
      this.rows.push({ key: `row-${this.sequence}`, record: {} });
      return { outcome: "applied" };
    }
    if (body.stage === "seal") {
      if (this.sequence !== this.appends) throw new Error("409");
      this.state = "sealed";
      this.rows.push({ key: "manifest", record: { revision: `capture:${CAPTURE}`, value: { capture_id: CAPTURE } } });
      return { outcome: "applied" };
    }
    throw new Error("409");
  }
  fetch = (url: string, init: RequestInit): Promise<Response> => {
    try {
      return Promise.resolve(this.handle(url, init));
    } catch (error) {
      return Promise.reject(error instanceof Error ? error : new Error(String(error)));
    }
  };
  handle(url: string, init: RequestInit): Response {
    const headers = init.headers as Record<string, string>;
    if (headers.authorization !== "Bearer admin" || headers[INTENT_HEADER] !== this.token) return new Response("{}", { status: 403 });
    const path = new URL(url).pathname;
    if (path === IMPORT_PATH && init.method === "GET") return Response.json(this.status());
    if (path === IMPORT_PATH) {
      const body = JSON.parse(init.body as string) as Record<string, unknown>;
      let reply: { outcome: string };
      try {
        reply = this.frame(body);
      } catch {
        return new Response('{"error":"refused"}', { status: 409 });
      }
      this.writes++;
      if (this.loseReplyAfterWrite.delete(this.writes)) throw new TypeError("connection reset after commit");
      return Response.json({ v: 1, stage: body.stage, ...reply, status: this.status() });
    }
    if (path === EXPORT_PATH) {
      const request = JSON.parse(init.body as string) as { cursor: string | null; limit: number };
      const start = request.cursor === null ? 0 : Number(request.cursor);
      const page = this.rows.slice(start, start + 2);
      const body = JSON.stringify(page);
      const headers: Record<string, string> = { "x-uos-state-export-sha256": sha256(body), "x-uos-state-export-count": String(page.length) };
      if (start + 2 < this.rows.length) headers["x-uos-state-export-next-cursor"] = String(start + 2);
      return new Response(body, { headers });
    }
    return new Response("{}", { status: 404 });
  }
}

const transport = (worker: FakeWorker): Transport => ({
  origin: "https://candidate.example",
  adminToken: "admin",
  intent: worker.token,
  fetch: worker.fetch,
  attempts: 1,
});

test("frames are bound to their hash, inventory, single capture and stage order", () => {
  const text = framesText(3);
  const parsed = parseFrames(Buffer.from(text), sha256(text), 5);
  assert.equal(parsed.captureId, CAPTURE);
  assert.deepEqual(
    parsed.frames.map((frame) => frame.stage),
    ["begin", "append", "append", "append", "seal"]
  );
  assert.throws(() => parseFrames(Buffer.from(text), "0".repeat(64), 5), /hash differs/);
  assert.throws(() => parseFrames(Buffer.from(text), sha256(text), 6), /inventory differs/);
  const lines = text.trimEnd().split("\n");
  const swapped = `${[lines[0], lines[2], lines[1], ...lines.slice(3)].join("\n")}\n`;
  assert.throws(() => parseFrames(Buffer.from(swapped), sha256(swapped), 5), /order is invalid/);
  const other = text.replace(`"capture_id":"${CAPTURE}"}}`, `"capture_id":"${"d".repeat(64)}"}}`);
  assert.throws(() => parseFrames(Buffer.from(other), sha256(other), 5), /more than one capture/);
});

test("a lost reply after a committed chunk resumes from the Worker status without a second write", async () => {
  const token = mintIntent({ kind: "import", captureId: CAPTURE }).token;
  const worker = new FakeWorker(4, token);
  const text = framesText(4);
  const { frames } = parseFrames(Buffer.from(text), sha256(text), 6);
  worker.loseReplyAfterWrite.add(3);
  const result = await importFrames(transport(worker), frames, CAPTURE);
  assert.equal(result.status.state, "sealed");
  assert.equal(result.applied, 5);
  assert.equal(result.replayed, 0);
  assert.equal(worker.rows.length, 5);
  // Rerunning the whole import against the sealed target writes nothing.
  const again = await importFrames(transport(worker), frames, CAPTURE);
  assert.equal(again.applied + again.replayed, 0);
  assert.equal(again.resumed_at, frames.length);
});

test("resume refuses an occupied target, another capture or a diverged staging chain", () => {
  const text = framesText(2);
  const { frames } = parseFrames(Buffer.from(text), sha256(text), 4);
  const base: ImportStatus = {
    state: "staging",
    capture_id: CAPTURE,
    next_sequence: 1,
    received_records: 1,
    expected_records: 2,
    digest: digest(0),
    activation_authorized: false,
  };
  assert.equal(resumeIndex(base, frames, CAPTURE), 2);
  assert.equal(resumeIndex({ ...base, next_sequence: 2, digest: digest(1) }, frames, CAPTURE), 3);
  assert.throws(() => resumeIndex({ ...base, digest: digest(9) }, frames, CAPTURE), /digest differs/);
  assert.throws(() => resumeIndex({ ...base, capture_id: "e".repeat(64) }, frames, CAPTURE), /different capture/);
  assert.throws(() => resumeIndex({ ...base, state: "occupied", capture_id: null }, frames, CAPTURE), /new empty authority/);
  assert.equal(resumeIndex({ ...base, state: "empty", capture_id: null }, frames, CAPTURE), 0);
});

test("export pages verify every page hash and count, and receipts are intent-signed", async () => {
  const token = mintIntent({ kind: "export" }).token;
  const worker = new FakeWorker(0, token);
  worker.rows = [1, 2, 3, 4, 5].map((n) => ({ key: `k${n}` }));
  const result = await exportPages(transport(worker));
  assert.equal(result.records, 5);
  assert.equal(result.pages.length, 3);
  assert.equal(sha256(result.archive), result.sha256);
  const tampering = new FakeWorker(0, token);
  tampering.rows = worker.rows;
  const original = tampering.fetch;
  tampering.fetch = async (url, init) => {
    const response = await original(url, init);
    return new Response(`${await response.text()} `, { headers: response.headers });
  };
  await assert.rejects(exportPages(transport(tampering)), /hash or count differs/);
  const signed = signReceipt(token, { v: 1, records: 5 });
  assert.ok(verifyReceipt(token, signed));
  assert.ok(!verifyReceipt(token, { ...signed, records: 6 }));
  assert.ok(!verifyReceipt("other", signed));
});

test("the CLI verifies signed transfer evidence and rejects unbound or tampered receipts", async () => {
  const directory = mkdtempSync(join(tmpdir(), "uos-state-transfer-"));
  try {
    chmodSync(directory, 0o700);
    const intent = await main(["intent", "--scope", "import", "--capture-id", CAPTURE, "--out", join(directory, "import.intent")], {});
    const token = readFileSync(join(directory, "import.intent"), "utf8");
    assert.equal(intent.sha256, sha256(token));
    assert.equal(lstatSync(join(directory, "import.intent")).mode & 0o777, 0o600);
    const text = framesText(3, false);
    const framesPath = join(directory, "frames.private.ndjson");
    writeFileSync(framesPath, text, { mode: 0o600 });
    const stageReceipt = join(directory, "frames.receipt.json");
    const stage = {
      kind: "m20_stage_frames",
      frames: 5,
      frame_file_bytes: Buffer.byteLength(text),
      owner_frame: false,
      summary: { capture_id: CAPTURE, records: 3 },
    };
    writeFileSync(stageReceipt, JSON.stringify(stage), { mode: 0o600 });
    const worker = new FakeWorker(3, token);
    const environment = { UOS_STATE_TRANSFER_ADMIN_TOKEN: "admin", UOS_STATE_TRANSFER_INTENT: token };
    const common = ["--origin", "https://candidate.example"];
    const imported = await main(
      [
        "import",
        ...common,
        "--frames",
        framesPath,
        "--frames-sha256",
        sha256(text),
        "--frames-receipt",
        stageReceipt,
        "--receipt",
        join(directory, "import.json"),
      ],
      environment,
      worker.fetch
    );
    assert.ok(verifyReceipt(token, imported));
    assert.equal(imported.capture_id, CAPTURE);
    await assert.rejects(
      main(
        [
          "import",
          ...common,
          "--frames",
          framesPath,
          "--frames-sha256",
          sha256(text),
          "--frames-receipt",
          stageReceipt,
          "--receipt",
          join(directory, "import.json"),
        ],
        environment,
        worker.fetch
      ),
      /EEXIST/
    );
    await assert.rejects(
      main(["export", ...common, "--archive", join(directory, "a"), "--receipt", join(directory, "b")], environment, worker.fetch),
      /not an export intent/
    );
    const exportToken = mintIntent({ kind: "export" }).token;
    const exporter = Object.assign(worker, { token: exportToken });
    const exported = await main(
      ["export", ...common, "--archive", join(directory, "archive.ndjson"), "--receipt", join(directory, "export.json")],
      { UOS_STATE_TRANSFER_ADMIN_TOKEN: "admin", UOS_STATE_TRANSFER_INTENT: exportToken },
      exporter.fetch
    );
    assert.equal(exported.records, 4);
    assert.equal(exported.archive_sha256, sha256(readFileSync(join(directory, "archive.ndjson"))));
    const verifyArgs = [
      "verify",
      ...common,
      "--frames",
      framesPath,
      "--frames-sha256",
      sha256(text),
      "--archive",
      join(directory, "archive.ndjson"),
      "--frames-receipt",
      stageReceipt,
      "--import-receipt",
      join(directory, "import.json"),
      "--export-receipt",
      join(directory, "export.json"),
    ];
    const privateInput = JSON.stringify({ v: 1, import_intent: token, export_intent: exportToken });
    const verify = (input = privateInput) => main(verifyArgs, {}, worker.fetch, input);
    const verified = await verify();
    assert.equal(verified.matched, true);
    assert.equal(verified.stable_passes, 2);
    assert.equal(verified.origin, common[1]);
    const child = Bun.spawn([process.execPath, "--no-env-file", "--no-install", "tools/cloudflare/state-transfer.ts", ...verifyArgs], {
      env: { PATH: process.env.PATH ?? "/usr/bin:/bin" },
      stdin: "pipe",
      stdout: "pipe",
      stderr: "pipe",
    });
    await child.stdin.write(privateInput);
    await child.stdin.end();
    const [output, errors, code] = await Promise.all([new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited]);
    assert.equal(code, 0, errors);
    assert.equal((JSON.parse(output) as { matched: boolean }).matched, true);
    assert.ok(!output.includes(token) && !output.includes(exportToken));
    await assert.rejects(main(verifyArgs, environment), /private stdin JSON/);
    for (const input of ["", "{", " ".repeat(4097), JSON.stringify({ v: 1, import_intent: token, export_intent: exportToken, extra: true })])
      await assert.rejects(verify(input), /private stdin JSON/);
    await assert.rejects(verify(JSON.stringify({ v: 1, import_intent: exportToken, export_intent: token })), /swapped scopes/);
    await assert.rejects(
      verify(JSON.stringify({ v: 1, import_intent: mintIntent({ kind: "import", captureId: CAPTURE }).token, export_intent: exportToken })),
      /signature differs/
    );
    const resign = (receipt: Record<string, unknown>, intent: string, change: Record<string, unknown>) => {
      const body = { ...receipt };
      delete body.hmac_sha256;
      return signReceipt(intent, { ...body, ...change });
    };
    const writeReceipt = (name: string, value: unknown) => {
      writeFileSync(join(directory, name), JSON.stringify(value), { mode: 0o600 });
    };
    for (const [change, message] of [
      [{ origin: "https://other.example" }, /origin differs/],
      [{ capture_id: "d".repeat(64) }, /capture, frame hash/],
      [{ frames_sha256: "d".repeat(64) }, /capture, frame hash/],
      [{ frames: 4 }, /capture, frame hash/],
      [{ status: { ...(imported.status as Record<string, unknown>), state: "staging" } }, /sealed complete/],
    ] as const) {
      writeReceipt("import.json", resign(imported, token, change));
      await assert.rejects(verify(), message);
    }
    writeReceipt("import.json", { ...imported, frames: 4 });
    await assert.rejects(verify(), /signature differs/);
    writeReceipt("import.json", imported);
    for (const [change, message] of [
      [{ origin: "https://other.example" }, /origin differs/],
      [{ archive_sha256: "d".repeat(64) }, /archive hash or size/],
      [{ stable_passes: 1 }, /at least two stable passes/],
      [{ records: 5 }, /staged import implies/],
    ] as const) {
      writeReceipt("export.json", resign(exported, exportToken, change));
      await assert.rejects(verify(), message);
    }
    writeReceipt("export.json", { ...exported, stable_passes: 1 });
    await assert.rejects(verify(), /signature differs/);
    writeReceipt("export.json", exported);
    writeFileSync(framesPath, `${text} `);
    await assert.rejects(verify(), /hash differs/);
    writeFileSync(framesPath, text);
    const archivePath = join(directory, "archive.ndjson");
    const archive = readFileSync(archivePath);
    writeFileSync(archivePath, Buffer.concat([archive, Buffer.from(" ")]));
    await assert.rejects(verify(), /archive hash or size/);
    writeFileSync(archivePath, archive);
    const wrongCapture = archive.toString().replace(CAPTURE, "d".repeat(64));
    const pageLines = wrongCapture.trimEnd().split("\n");
    writeFileSync(archivePath, wrongCapture);
    writeReceipt(
      "export.json",
      resign(exported, exportToken, {
        archive_sha256: sha256(wrongCapture),
        archive_bytes: Buffer.byteLength(wrongCapture),
        pages: pageLines.map((page) => ({ sha256: sha256(page), count: (JSON.parse(page) as unknown[]).length })),
      })
    );
    await assert.rejects(verify(), /capture manifest differs/);
    await assert.rejects(main(["import", "--origin", "http://candidate.example"], environment), /exact https origin/);
    await assert.rejects(main(["export", ...common], {}), /must be set/);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("private verification stdin is bounded before parsing", async () => {
  const stream = (text: string) =>
    new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(Buffer.from(text));
        controller.close();
      },
    });
  assert.equal(await readVerificationInput(stream("fixture")), "fixture");
  await assert.rejects(readVerificationInput(stream(" ".repeat(4097))), /private stdin JSON/);
});
