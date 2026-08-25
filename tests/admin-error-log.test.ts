import assert from "node:assert/strict";
import {
  ADMIN_ERROR_BUCKET_MS,
  ADMIN_ERROR_LOG_PREFIX,
  listAdminFiveXxBuckets,
  recordAdminError,
} from "../src/admin_error_log.ts";

class SetOnlyKv {
  readonly writes: Array<{ key: Deno.KvKey; value: unknown; options?: { expireIn?: number } }> = [];

  set(key: Deno.KvKey, value: unknown, options?: { expireIn?: number }): Promise<Deno.KvCommitResult> {
    this.writes.push({ key, value, options });
    return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
  }
}

const terminal = (overrides: Partial<Parameters<typeof recordAdminError>[0]> = {}) => ({
  request_id: "request-1",
  route: "responses",
  status: 503,
  provider: "chatgpt_codex",
  model: "gpt-5.6-terra",
  reasoning: "max",
  stream: true,
  terminal_type: "http.error",
  failure_kind: "codex_admission_busy",
  delivery_outcome: "delivered" as const,
  created_at_ms: 1_777_000_000_000,
  latency_ms: 1_692,
  git_sha: "fixture-sha",
  deno_revision: "fixture-revision",
  ...overrides,
});

Deno.test("admin error ledger stores every HTTP failure with bounded diagnostics", async () => {
  const kv = new SetOnlyKv();
  await recordAdminError(terminal(), kv as unknown as Deno.Kv);

  assert.equal(kv.writes.length, 1);
  assert.deepEqual(kv.writes[0].key, [...ADMIN_ERROR_LOG_PREFIX, 1_777_000_000_000, "request-1"]);
  assert.deepEqual(kv.writes[0].value, { ...terminal(), version: 1 });
  assert.ok((kv.writes[0].options?.expireIn ?? 0) > 0);
});

Deno.test("admin error ledger excludes successful completed requests", async () => {
  const kv = new SetOnlyKv();
  await recordAdminError(
    terminal({ status: 200, terminal_type: "response.completed", failure_kind: null }),
    kv as unknown as Deno.Kv,
  );
  assert.equal(kv.writes.length, 0);
});

Deno.test("admin error analytics counts every inference 5xx in fifteen-minute buckets", async () => {
  const records = [
    terminal({ request_id: "one", status: 500, created_at_ms: ADMIN_ERROR_BUCKET_MS + 1 }),
    terminal({ request_id: "two", status: 503, created_at_ms: ADMIN_ERROR_BUCKET_MS + 2 }),
    terminal({ request_id: "three", status: 429, created_at_ms: ADMIN_ERROR_BUCKET_MS + 3 }),
    terminal({ request_id: "four", status: 599, created_at_ms: ADMIN_ERROR_BUCKET_MS * 2 + 1 }),
  ].map((value) => ({ value: { ...value, version: 1 } }));
  const kv = {
    list: () => ({
      async *[Symbol.asyncIterator]() {
        yield* records;
      },
    }),
  };

  assert.deepEqual(await listAdminFiveXxBuckets(kv as unknown as Deno.Kv), [
    { bucket_start_at_ms: ADMIN_ERROR_BUCKET_MS, count: 2 },
    { bucket_start_at_ms: ADMIN_ERROR_BUCKET_MS * 2, count: 1 },
  ]);
});
