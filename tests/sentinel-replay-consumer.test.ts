/**
 * Acceptance tests for the fixed local Sentinel replay consumer.
 *
 * Every case invokes the real fixed command as a child process inside a
 * disposable snapshot: the source tree is copied into a temporary directory
 * under the checkout, the fixtures and trusted dispatch metadata are written
 * there, and the child runs with a fresh Deno cache, no configuration, no
 * remote access, and no application environment. The shared checkout source is
 * never mutated and no network or live API is contacted.
 */

import assert from "node:assert/strict";

const REPO_ROOT = Deno.cwd();
const SCRATCH_ROOT = await Deno.makeTempDir({ dir: REPO_ROOT, prefix: "m06-sentinel-replay-consumer-" });
const CACHE_ROOT = await Deno.makeTempDir({ dir: SCRATCH_ROOT, prefix: "deno-cache-" });
const SOURCE_ROOT = `${SCRATCH_ROOT}/source`;

const decoder = new TextDecoder();

globalThis.addEventListener("unload", () => {
  try {
    Deno.removeSync(SCRATCH_ROOT, { recursive: true });
  } catch {
    // Scratch cleanup is best effort; the snapshot stays outside shared source.
  }
});

const copyTree = async (source: string, destination: string): Promise<void> => {
  await Deno.mkdir(destination, { recursive: true });
  for await (const entry of Deno.readDir(source)) {
    const from = `${source}/${entry.name}`;
    const to = `${destination}/${entry.name}`;
    if (entry.isDirectory) await copyTree(from, to);
    else if (entry.isFile) await Deno.copyFile(from, to);
  }
};

await copyTree(`${REPO_ROOT}/src`, `${SOURCE_ROOT}/src`);
await Deno.mkdir(`${SOURCE_ROOT}/scripts`, { recursive: true });
await Deno.copyFile(`${REPO_ROOT}/scripts/replay.ts`, `${SOURCE_ROOT}/scripts/replay.ts`);
await copyTree(`${REPO_ROOT}/tests/helpers`, `${SOURCE_ROOT}/tests/helpers`);

const FIXED_ARGS = ["run", "--no-prompt", "--no-config", "--no-remote", "--allow-read=.", "scripts/replay.ts"];

type CaseFile = Readonly<{ path: string; content: string | Uint8Array }>;
type ChildResult = Readonly<{ code: number; stdout: string; stderr: string }>;

const materializeCase = async (files: readonly CaseFile[]): Promise<string> => {
  const dir = await Deno.makeTempDir({ dir: SCRATCH_ROOT, prefix: "case-" });
  await copyTree(SOURCE_ROOT, dir);
  for (const file of files) {
    const target = `${dir}/${file.path}`;
    await Deno.mkdir(target.slice(0, target.lastIndexOf("/")), { recursive: true });
    if (typeof file.content === "string") await Deno.writeTextFile(target, file.content);
    else await Deno.writeFile(target, file.content);
  }
  return dir;
};

const withCase = async <T>(files: readonly CaseFile[], fn: (dir: string) => Promise<T>): Promise<T> => {
  const dir = await materializeCase(files);
  try {
    return await fn(dir);
  } finally {
    await Deno.remove(dir, { recursive: true }).catch(() => {});
  }
};

/** Invoke exactly the fixed command with a fresh cache and no application environment. */
const runFixedConsumer = async (dir: string): Promise<ChildResult> => {
  const output = await new Deno.Command(Deno.execPath(), {
    args: FIXED_ARGS,
    cwd: dir,
    clearEnv: true,
    env: { DENO_DIR: CACHE_ROOT, DENO_NO_UPDATE_CHECK: "1" },
    stdout: "piped",
    stderr: "piped",
  }).output();
  return {
    code: output.code,
    stdout: decoder.decode(output.stdout),
    stderr: decoder.decode(output.stderr),
  };
};

const TEST_MARKER_PREFIX = "sentinel-replay-test:";
const CAUSAL_FAILURE_LINE = "sentinel-causal-failure:stream terminated unexpectedly\n";
const UNAVAILABLE_LINE = "sentinel-replay-unavailable: replay input is unavailable\n";
const CANARY = "sk-canary-do-not-log-0123456789abcdef";
const DEFAULT_IDS = ["alpha", "beta", "gamma"] as const;

const markers = (ids: readonly string[]): string => ids.map((id) => `${TEST_MARKER_PREFIX}${id}\n`).join("");

type FixtureProvider = "chatgpt_codex" | "surplus" | "metered" | "cerebras";
type FixtureTerminal = "pending" | "fetch_error" | "eof" | "read_error" | "cancelled";
type FixtureAttempt = Readonly<{
  provider: FixtureProvider;
  status: number | null;
  content_type: string | null;
  chunks_base64: readonly string[];
  terminal: FixtureTerminal;
}>;
type FixtureTrace = Readonly<{
  version: 1;
  attempts: readonly FixtureAttempt[];
  attempts_truncated: boolean;
  bytes_truncated: boolean;
  chunks_truncated: boolean;
}>;

const attempt = (
  provider: FixtureProvider,
  chunks: readonly string[],
  terminal: FixtureTerminal,
  overrides: Readonly<{ status?: number | null; content_type?: string | null }> = {}
): FixtureAttempt => ({
  provider,
  status: "status" in overrides ? overrides.status! : 200,
  content_type: "content_type" in overrides ? overrides.content_type! : "text/event-stream",
  chunks_base64: chunks.map((chunk) => btoa(chunk)),
  terminal,
});

const trace = (
  attempts: readonly FixtureAttempt[],
  flags: Partial<Pick<FixtureTrace, "attempts_truncated" | "bytes_truncated" | "chunks_truncated">> = {}
): FixtureTrace => ({
  version: 1,
  attempts,
  attempts_truncated: false,
  bytes_truncated: false,
  chunks_truncated: false,
  ...flags,
});

const sse = (value: Record<string, unknown>): string => `data: ${JSON.stringify(value)}\n\n`;
const created = (id: string): string => sse({ type: "response.created", response: { id, object: "response", status: "in_progress" } });
const inProgress = (id: string): string => sse({ type: "response.in_progress", response: { id, object: "response", status: "in_progress" } });
const delta = (id: string, text: string): string =>
  sse({
    type: "response.output_text.delta",
    response_id: id,
    item_id: "msg_fixture",
    output_index: 0,
    content_index: 0,
    delta: text,
  });
const itemDone = (id: string, text: string): string =>
  sse({
    type: "response.output_item.done",
    response_id: id,
    output_index: 0,
    item: {
      id: "msg_fixture",
      type: "message",
      status: "completed",
      role: "assistant",
      content: [{ type: "output_text", text, annotations: [] }],
    },
  });
const completed = (id: string, text: string, output: readonly Record<string, unknown>[] | null = null): string =>
  sse({
    type: "response.completed",
    response: {
      id,
      object: "response",
      status: "completed",
      output: output ?? [
        {
          id: "msg_fixture",
          type: "message",
          status: "completed",
          role: "assistant",
          content: [{ type: "output_text", text, annotations: [] }],
        },
      ],
    },
  });

const responsesBody = (stream: boolean, text = "hello"): Record<string, unknown> => ({
  model: "gpt-5.6-codex",
  stream,
  input: [{ type: "message", role: "user", content: [{ type: "input_text", text }] }],
});

const requestEnvelope = (body: Record<string, unknown>, extra: Record<string, unknown> = {}): string =>
  JSON.stringify({
    version: 1,
    endpoint: "/v1/responses",
    method: "POST",
    request_id: "fixture-request",
    ...extra,
    body: JSON.stringify(body),
  });

const metadata = (requestPath: string, upstreamPath: string, testIds: readonly string[]): string =>
  JSON.stringify({ version: "v1", requestPath, upstreamPath, testIds });

const replayCase = (options: {
  trace: FixtureTrace;
  stream: boolean;
  testIds?: readonly string[];
  requestBody?: Record<string, unknown>;
  extraEnvelope?: Record<string, unknown>;
}): readonly CaseFile[] => [
  {
    path: ".sentinel-replay-input.json",
    content: metadata("fixtures/request.json", "fixtures/upstream.json", options.testIds ?? DEFAULT_IDS),
  },
  {
    path: "fixtures/request.json",
    content: requestEnvelope(options.requestBody ?? responsesBody(options.stream), options.extraEnvelope),
  },
  { path: "fixtures/upstream.json", content: JSON.stringify(options.trace) },
];

const assertUnavailable = (result: ChildResult, label: string): void => {
  assert.equal(result.code, 2, `${label}: expected the static unavailable exit`);
  assert.equal(result.stdout, UNAVAILABLE_LINE, `${label}: exactly one fixed static unavailable line`);
  assert.equal(result.stderr, "", `${label}: stderr must stay empty`);
};

Deno.test("fixed consumer replays a complete recorded codex stream through the owned response stream", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "hello"), itemDone("resp_fixture", "hello"), completed("resp_fixture", "hello")];
  await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "cancelled")]), stream: true }), async (dir) => {
    const result = await runFixedConsumer(dir);
    assert.equal(result.code, 0, "a complete streamed replay must succeed");
    assert.equal(result.stdout, markers(DEFAULT_IDS), "markers only, in trusted order");
    assert.equal(result.stderr, "");
  });
});

Deno.test("fixed consumer replays a complete recorded codex stream through the buffered converter", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "hello"), itemDone("resp_fixture", "hello"), completed("resp_fixture", "hello")];
  await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "cancelled")]), stream: false }), async (dir) => {
    const result = await runFixedConsumer(dir);
    assert.equal(result.code, 0, "a complete buffered replay must succeed");
    assert.equal(result.stdout, markers(DEFAULT_IDS));
    assert.equal(result.stderr, "");
  });
});

Deno.test("fixed consumer preserves trusted marker order and emits no extra output", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "x"), completed("resp_fixture", "x")];
  const ids = ["zeta", "alpha", "middle", "A.b:C_d"];
  await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "cancelled")]), stream: true, testIds: ids }), async (dir) => {
    const result = await runFixedConsumer(dir);
    assert.equal(result.code, 0);
    assert.equal(result.stdout, markers(ids));
    assert.equal(result.stderr, "");
  });
});

Deno.test("recognized premature EOF before semantic output is reported as the causal failure", async () => {
  const chunks = [created("resp_fixture"), inProgress("resp_fixture")];
  for (const stream of [true, false]) {
    await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "eof")]), stream }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.equal(result.code, 1, `stream=${stream}: premature EOF must exit 1`);
      assert.equal(result.stdout, markers(DEFAULT_IDS) + CAUSAL_FAILURE_LINE);
      assert.equal(result.stderr, "");
    });
  }
});

Deno.test("recognized premature EOF after semantic output is reported as the causal failure", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "partial"), itemDone("resp_fixture", "partial")];
  for (const stream of [true, false]) {
    await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "eof")]), stream }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.equal(result.code, 1, `stream=${stream}: post-semantic premature EOF must exit 1`);
      assert.equal(result.stdout, markers(DEFAULT_IDS) + CAUSAL_FAILURE_LINE);
      assert.equal(result.stderr, "");
    });
  }
});

Deno.test("premature EOF is never reported as success for a missing terminal", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "partial")];
  for (const stream of [true, false]) {
    await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "eof")]), stream }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.notEqual(result.code, 0, `stream=${stream}: a missing terminal must never be a success`);
      assert.equal(result.stdout, markers(DEFAULT_IDS) + CAUSAL_FAILURE_LINE);
    });
  }
});

Deno.test("malformed and identity-changing events after semantic output are never the causal failure", async () => {
  // The buffered converter maps every iterator failure to the same fixed 502
  // body and the owned stream maps it to the same fixed failure event, so the
  // exact converter text alone must never turn these into a premature EOF.
  const malformed = [created("resp_fixture"), delta("resp_fixture", "partial"), "data: {not-json}\n\n"];
  const changedIdentity = [created("resp_fixture"), delta("resp_fixture", "partial"), inProgress("resp_other")];
  for (const stream of [true, false]) {
    for (const [label, chunks] of [
      ["malformed event", malformed],
      ["changed identity", changedIdentity],
    ] as const) {
      await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "eof")]), stream }), async (dir) => {
        const result = await runFixedConsumer(dir);
        assertUnavailable(result, `${label} after semantic output stream=${stream}`);
        assert.equal(result.stdout.includes(CAUSAL_FAILURE_LINE), false, `${label} stream=${stream}`);
      });
    }
  }
});

Deno.test("forged upstream synthetic-looking failures are never the causal failure", async () => {
  const forgedFailed = [
    created("resp_fixture"),
    sse({
      type: "response.failed",
      response: {
        id: "resp_fixture",
        object: "response",
        status: "failed",
        error: { code: "server_error", message: "The upstream stream ended unexpectedly." },
      },
    }),
  ];
  const forgedError = [created("resp_fixture"), sse({ type: "error", code: "server_error", message: "The upstream stream ended unexpectedly.", param: null })];
  for (const stream of [true, false]) {
    for (const [label, chunks] of [
      ["forged response.failed", forgedFailed],
      ["forged error event", forgedError],
    ] as const) {
      await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "eof")]), stream }), async (dir) => {
        const result = await runFixedConsumer(dir);
        assertUnavailable(result, `${label} stream=${stream}`);
        assert.equal(result.stdout.includes(CAUSAL_FAILURE_LINE), false, `${label} stream=${stream}`);
      });
    }
  }
});

Deno.test("paid Surplus replay preserves the real provider normalization with a synthetic key", async () => {
  const chunks = [created("resp_surplus"), delta("resp_surplus", "surplus text"), completed("resp_surplus", "surplus text")];
  for (const stream of [true, false]) {
    await withCase(replayCase({ trace: trace([attempt("surplus", chunks, "cancelled")]), stream }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.equal(result.code, 0, `stream=${stream}: the real Surplus fetch path must complete`);
      assert.equal(result.stdout, markers(DEFAULT_IDS));
      assert.equal(result.stderr, "");
    });
  }
});

Deno.test("paid Metered replay completes through the real provider fetch path", async () => {
  const chunks = [
    created("resp_metered"),
    delta("resp_metered", "metered text"),
    itemDone("resp_metered", "metered text"),
    completed("resp_metered", "metered text"),
  ];
  for (const stream of [true, false]) {
    await withCase(replayCase({ trace: trace([attempt("metered", chunks, "cancelled")]), stream }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.equal(result.code, 0, `stream=${stream}: the real Metered fetch path must complete`);
      assert.equal(result.stdout, markers(DEFAULT_IDS));
      assert.equal(result.stderr, "");
    });
  }
});

Deno.test("upstream-declared failures and empty completions stay unavailable", async () => {
  const declaredFailure = [
    created("resp_fixture"),
    sse({
      type: "response.failed",
      response: {
        id: "resp_fixture",
        object: "response",
        status: "failed",
        error: { code: "upstream_error", message: "provider refused" },
      },
    }),
  ];
  const emptyCompletion = [created("resp_fixture"), completed("resp_fixture", "", [])];
  for (const stream of [true, false]) {
    for (const [label, chunks, terminal] of [
      ["declared failure", declaredFailure, "cancelled"],
      ["empty completion", emptyCompletion, "cancelled"],
    ] as const) {
      await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, terminal)]), stream }), async (dir) => {
        const result = await runFixedConsumer(dir);
        assertUnavailable(result, `${label} stream=${stream}`);
        assert.equal(result.stdout.includes(TEST_MARKER_PREFIX), false, "no markers for unavailable input");
      });
    }
  }
});

Deno.test("completed responses without real semantic output are never successful", async () => {
  // The gateway rejects a committed response.completed with no semantic event
  // (prepareResponsesAttempt), and the actual converted output must also carry
  // completed status, response identity and semantic content reported by the
  // same responsesEventSemanticKind utility. None of these may be a success.
  const reasoningOnly = [created("resp_fixture"), completed("resp_fixture", "", [{ type: "reasoning", summary: [] }])];
  const malformedOutputItem = [created("resp_fixture"), completed("resp_fixture", "", [{}])];
  // The precommit content-part text is real semantic output, but the buffered
  // converter does not repair this malformed terminal output, so the actual
  // completed Response object still carries no usable semantic content.
  const malformedAfterSemanticText = [
    created("resp_fixture"),
    sse({
      type: "response.content_part.done",
      response_id: "resp_fixture",
      item_id: "msg_fixture",
      output_index: 0,
      content_index: 0,
      part: { type: "output_text", text: "hello", annotations: [] },
    }),
    completed("resp_fixture", "hello", [{}]),
  ];
  for (const stream of [true, false]) {
    for (const [label, chunks] of [
      ["reasoning-only completion", reasoningOnly],
      ["output:[{}] completion", malformedOutputItem],
      ["malformed completion after semantic text", malformedAfterSemanticText],
    ] as const) {
      await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "cancelled")]), stream }), async (dir) => {
        const result = await runFixedConsumer(dir);
        assertUnavailable(result, `${label} stream=${stream}`);
        assert.equal(result.stdout.includes(CAUSAL_FAILURE_LINE), false, `${label} stream=${stream}`);
      });
    }
  }
});

Deno.test("malformed recorded SSE is unavailable instead of the expected failure", async () => {
  const chunks = [created("resp_fixture"), "data: {not-json}\n\n"];
  for (const stream of [true, false]) {
    await withCase(replayCase({ trace: trace([attempt("chatgpt_codex", chunks, "cancelled")]), stream }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assertUnavailable(result, `malformed SSE stream=${stream}`);
    });
  }
});

Deno.test("truncated, empty, multi-attempt and unsupported attempts are unavailable", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "x"), completed("resp_fixture", "x")];
  const cases: readonly (readonly [string, FixtureTrace])[] = [
    ["empty trace", trace([])],
    ["attempts truncated", trace([attempt("chatgpt_codex", chunks, "cancelled")], { attempts_truncated: true })],
    ["bytes truncated", trace([attempt("chatgpt_codex", chunks, "cancelled")], { bytes_truncated: true })],
    ["chunks truncated", trace([attempt("chatgpt_codex", chunks, "cancelled")], { chunks_truncated: true })],
    ["multi-attempt fallback", trace([attempt("chatgpt_codex", chunks, "read_error"), attempt("surplus", chunks, "cancelled")])],
    ["cerebras provider", trace([attempt("cerebras", chunks, "cancelled")])],
    ["non-200 status", trace([attempt("chatgpt_codex", chunks, "cancelled", { status: 500 })])],
    ["non-SSE content type", trace([attempt("chatgpt_codex", chunks, "cancelled", { content_type: "application/json" })])],
    ["read_error terminal", trace([attempt("chatgpt_codex", chunks, "read_error")])],
    ["pending terminal", trace([attempt("chatgpt_codex", chunks, "pending")])],
  ];
  for (const [label, fixture] of cases) {
    await withCase(replayCase({ trace: fixture, stream: true }), async (dir) => {
      const result = await runFixedConsumer(dir);
      assertUnavailable(result, label);
    });
  }
});

Deno.test("unsupported request bodies and envelopes are unavailable", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "x"), completed("resp_fixture", "x")];
  const fixtures = trace([attempt("chatgpt_codex", chunks, "cancelled")]);
  const bodies: readonly (readonly [string, string])[] = [
    ["chat completions body", requestEnvelope({ model: "gpt-5.6-codex", messages: [{ role: "user", content: "hi" }] })],
    ["missing input", requestEnvelope({ model: "gpt-5.6-codex", stream: true })],
    ["non-boolean stream", requestEnvelope({ model: "gpt-5.6-codex", stream: "yes", input: "hi" })],
    ["non-object body", JSON.stringify({ version: 1, endpoint: "/v1/responses", body: '"text"' })],
    ["missing body", JSON.stringify({ version: 1, endpoint: "/v1/responses" })],
    ["truncated body json", JSON.stringify({ version: 1, endpoint: "/v1/responses", body: '{"model":"gpt-5.6-codex","input":' })],
  ];
  for (const [label, request] of bodies) {
    await withCase(
      [
        {
          path: ".sentinel-replay-input.json",
          content: metadata("fixtures/request.json", "fixtures/upstream.json", DEFAULT_IDS),
        },
        { path: "fixtures/request.json", content: request },
        { path: "fixtures/upstream.json", content: JSON.stringify(fixtures) },
      ],
      async (dir) => {
        const result = await runFixedConsumer(dir);
        assertUnavailable(result, label);
      }
    );
  }
});

Deno.test("malformed, oversized, escaped and duplicate dispatch metadata are unavailable", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "x"), completed("resp_fixture", "x")];
  const fixtures = JSON.stringify(trace([attempt("chatgpt_codex", chunks, "cancelled")]));
  const sixtyFiveIds = Array.from({ length: 65 }, (_value, index) => `id-${index}`);
  const metadataCases: readonly (readonly [string, string])[] = [
    ["invalid json", "{not-json"],
    [
      "wrong version",
      JSON.stringify({
        version: "v2",
        requestPath: "fixtures/request.json",
        upstreamPath: "fixtures/upstream.json",
        testIds: DEFAULT_IDS,
      }),
    ],
    ["missing test ids", JSON.stringify({ version: "v1", requestPath: "fixtures/request.json", upstreamPath: "fixtures/upstream.json" })],
    [
      "extra keys",
      JSON.stringify({
        version: "v1",
        requestPath: "fixtures/request.json",
        upstreamPath: "fixtures/upstream.json",
        testIds: DEFAULT_IDS,
        extra: true,
      }),
    ],
    ["absolute path", metadata("/etc/passwd", "fixtures/upstream.json", DEFAULT_IDS)],
    ["traversal path", metadata("../outside.json", "fixtures/upstream.json", DEFAULT_IDS)],
    ["empty segment", metadata("fixtures//request.json", "fixtures/upstream.json", DEFAULT_IDS)],
    ["dot segment", metadata("./fixtures/request.json", "fixtures/upstream.json", DEFAULT_IDS)],
    ["duplicate paths", metadata("fixtures/upstream.json", "fixtures/upstream.json", DEFAULT_IDS)],
    ["missing file", metadata("fixtures/missing.json", "fixtures/upstream.json", DEFAULT_IDS)],
    ["no test ids", metadata("fixtures/request.json", "fixtures/upstream.json", [])],
    ["duplicate test ids", metadata("fixtures/request.json", "fixtures/upstream.json", ["dup", "dup"])],
    ["invalid test id", metadata("fixtures/request.json", "fixtures/upstream.json", ["bad id"])],
    ["too many test ids", metadata("fixtures/request.json", "fixtures/upstream.json", sixtyFiveIds)],
    ["oversized metadata", metadata(`fixtures/${"a".repeat(17 * 1024)}.json`, "fixtures/upstream.json", DEFAULT_IDS)],
  ];
  for (const [label, dispatch] of metadataCases) {
    await withCase(
      [
        { path: ".sentinel-replay-input.json", content: dispatch },
        { path: "fixtures/request.json", content: requestEnvelope(responsesBody(false)) },
        { path: "fixtures/upstream.json", content: fixtures },
      ],
      async (dir) => {
        const result = await runFixedConsumer(dir);
        assertUnavailable(result, label);
      }
    );
  }

  // Fatal UTF-8 parsing must fail closed on every input file.
  const invalidBytes = new Uint8Array([0x7b, 0x22, 0xff, 0xfe, 0x22, 0x7d]);
  await withCase(
    [
      { path: ".sentinel-replay-input.json", content: invalidBytes },
      { path: "fixtures/request.json", content: requestEnvelope(responsesBody(false)) },
      { path: "fixtures/upstream.json", content: fixtures },
    ],
    async (dir) => {
      assertUnavailable(await runFixedConsumer(dir), "invalid utf8 metadata");
    }
  );
  await withCase(
    [
      {
        path: ".sentinel-replay-input.json",
        content: metadata("fixtures/request.json", "fixtures/upstream.json", DEFAULT_IDS),
      },
      { path: "fixtures/request.json", content: invalidBytes },
      { path: "fixtures/upstream.json", content: fixtures },
    ],
    async (dir) => {
      assertUnavailable(await runFixedConsumer(dir), "invalid utf8 request");
    }
  );
});

Deno.test("symlinked fixture files and symlinked ancestors are unavailable", async () => {
  const chunks = [created("resp_fixture"), delta("resp_fixture", "x"), completed("resp_fixture", "x")];
  const fixtures = JSON.stringify(trace([attempt("chatgpt_codex", chunks, "cancelled")]));
  const dispatch = metadata("fixtures/request.json", "fixtures/upstream.json", DEFAULT_IDS);
  const files: readonly CaseFile[] = [
    { path: ".sentinel-replay-input.json", content: dispatch },
    { path: "fixtures/real-request.json", content: requestEnvelope(responsesBody(false)) },
    { path: "fixtures/upstream.json", content: fixtures },
  ];

  await withCase(files, async (dir) => {
    await Deno.symlink("real-request.json", `${dir}/fixtures/request.json`);
    assertUnavailable(await runFixedConsumer(dir), "symlinked request file");
  });

  await withCase(
    [
      {
        path: ".sentinel-replay-input.json",
        content: metadata("linkdir/request.json", "fixtures/upstream.json", DEFAULT_IDS),
      },
      { path: "fixtures/request.json", content: requestEnvelope(responsesBody(false)) },
      { path: "fixtures/upstream.json", content: fixtures },
    ],
    async (dir) => {
      await Deno.symlink("fixtures", `${dir}/linkdir`);
      assertUnavailable(await runFixedConsumer(dir), "symlinked ancestor directory");
    }
  );
});

Deno.test("no request, upstream or secret-like canary bytes reach stdout or stderr", async () => {
  const canaryChunks = [created("resp_canary"), delta("resp_canary", CANARY), completed("resp_canary", CANARY)];
  const cleanTrace = trace([attempt("chatgpt_codex", canaryChunks, "cancelled")]);
  await withCase(
    [
      {
        path: ".sentinel-replay-input.json",
        content: metadata("fixtures/request.json", "fixtures/upstream.json", DEFAULT_IDS),
      },
      { path: "fixtures/request.json", content: requestEnvelope(responsesBody(true, CANARY), { note: CANARY }) },
      { path: "fixtures/upstream.json", content: JSON.stringify(cleanTrace) },
    ],
    async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.equal(result.code, 0);
      assert.equal(result.stdout, markers(DEFAULT_IDS));
      assert.equal(result.stdout.includes(CANARY), false, "stdout must never carry request or upstream content");
      assert.equal(result.stderr.includes(CANARY), false, "stderr must never carry request or upstream content");
    }
  );

  const eofTrace = trace([attempt("chatgpt_codex", [created("resp_canary"), delta("resp_canary", CANARY)], "eof")]);
  await withCase(
    [
      {
        path: ".sentinel-replay-input.json",
        content: metadata("fixtures/request.json", "fixtures/upstream.json", DEFAULT_IDS),
      },
      { path: "fixtures/request.json", content: requestEnvelope(responsesBody(true, CANARY)) },
      { path: "fixtures/upstream.json", content: JSON.stringify(eofTrace) },
    ],
    async (dir) => {
      const result = await runFixedConsumer(dir);
      assert.equal(result.code, 1);
      assert.equal(result.stdout, markers(DEFAULT_IDS) + CAUSAL_FAILURE_LINE);
      assert.equal(result.stdout.includes(CANARY), false);
      assert.equal(result.stderr.includes(CANARY), false);
    }
  );

  await withCase(
    [
      { path: ".sentinel-replay-input.json", content: `{"version":"v1","canary":"${CANARY}",` },
      { path: "fixtures/request.json", content: requestEnvelope(responsesBody(false)) },
      { path: "fixtures/upstream.json", content: JSON.stringify(cleanTrace) },
    ],
    async (dir) => {
      const result = await runFixedConsumer(dir);
      assertUnavailable(result, "malformed metadata canary");
      assert.equal(result.stdout.includes(CANARY), false);
    }
  );
});
