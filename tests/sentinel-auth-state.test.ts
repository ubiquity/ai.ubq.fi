import assert from "node:assert/strict";
import {
  decryptSentinelArtifact,
  decryptSentinelAuthStateArtifact,
  encryptSentinelArtifact,
  encryptSentinelAuthStateArtifact,
} from "../scripts/sentinel/artifact-crypto.ts";
import {
  assertSentinelCodexAuthStateArtifactPrecedesWriter,
  deserializeSentinelCodexAuthState,
  parseSentinelCodexAuthStateArtifactName,
  parseSentinelCodexAuthStateDocument,
  parseSentinelCodexAuthStateGeneration,
  probePreparedSentinelCodexAuthState,
  probeSentinelCodexAuthDocuments,
  readPreparedSentinelCodexAuthState,
  restoreSentinelCodexAuthState,
  sealPreparedSentinelCodexAuthState,
  selectNewestSentinelCodexAuthStateArtifact,
  SENTINEL_CODEX_AUTH_PROBE_MINIMUM_VALIDITY_MS,
  sentinelCodexAuthAccountDigest,
  type SentinelCodexAuthStateArtifactMetadata,
  sentinelCodexAuthStateArtifactName,
  SentinelCodexAuthStateError,
  serializeSentinelCodexAuthState,
  writePreparedSentinelCodexAuthState,
} from "../scripts/sentinel/auth-state.ts";

const requiredFileSystemPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
]);
const fileSystemTestsUnavailable = requiredFileSystemPermissions.some(
  (permission) => permission.state !== "granted",
);

const base64Url = (value: string): string => btoa(value).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

const jwt = (expiresAtSeconds: number, label: string): string =>
  `${base64Url(JSON.stringify({ alg: "none", typ: "JWT" }))}.${
    base64Url(JSON.stringify({ exp: expiresAtSeconds, label }))
  }.signature-${label}`;

const authJson = (
  slot: 1 | 2,
  accountId = `account-${slot}`,
  expiresAtSeconds = 2_000_000_000,
): string =>
  JSON.stringify(
    {
      auth_mode: "chatgpt",
      tokens: {
        id_token: `id-token-${slot}`,
        access_token: jwt(expiresAtSeconds, `access-${slot}`),
        refresh_token: `refresh-token-${slot}`,
        account_id: accountId,
        future_token_field: `preserve-${slot}`,
      },
      last_refresh: "2026-08-25T12:34:56.789Z",
      future_top_level_field: { slot, preserve: true },
    },
    null,
    2,
  );

const encodeSeed = (raw: string): string => {
  const bytes = new TextEncoder().encode(raw);
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
};

const artifact = (
  id: number,
  generation: string,
  runId: number,
  runAttempt: number,
  overrides: Partial<SentinelCodexAuthStateArtifactMetadata> = {},
): SentinelCodexAuthStateArtifactMetadata => ({
  id,
  name: sentinelCodexAuthStateArtifactName({ generation, runId, runAttempt }),
  sizeInBytes: 512,
  expired: false,
  ...overrides,
});

const assertStateError = (code: SentinelCodexAuthStateError["code"]): (error: unknown) => boolean => (error) =>
  error instanceof SentinelCodexAuthStateError && error.code === code;

Deno.test("Sentinel auth-state generations and artifact identities are strict and round-trip", () => {
  const identity = { generation: "rotation-2026.08", runId: 123_456, runAttempt: 7 };
  const name = sentinelCodexAuthStateArtifactName(identity);
  assert.equal(name, "sentinel-codex-auth-state-v1-rotation-2026.08-r123456-a7");
  assert.deepEqual(parseSentinelCodexAuthStateArtifactName(name), identity);
  assert.equal(parseSentinelCodexAuthStateArtifactName("sentinel-evidence-v1-something"), null);
  assert.equal(parseSentinelCodexAuthStateGeneration("rotation-2026.08"), "rotation-2026.08");

  for (const invalid of ["", "-leading", "trailing-", "has space", "x".repeat(65)]) {
    assert.throws(() => parseSentinelCodexAuthStateGeneration(invalid), assertStateError("invalid_generation"));
  }
  for (
    const invalid of [
      "sentinel-codex-auth-state-v1-generation-r0-a1",
      "sentinel-codex-auth-state-v1-generation-r1-a0",
      "sentinel-codex-auth-state-v1--r1-a1",
      "sentinel-codex-auth-state-v1-generation-r1-a1-extra",
    ]
  ) {
    assert.throws(() => parseSentinelCodexAuthStateArtifactName(invalid), assertStateError("invalid_artifact_name"));
  }
});

Deno.test("Sentinel auth-state selection uses the exact greatest run and attempt identity", () => {
  const selected = selectNewestSentinelCodexAuthStateArtifact([
    artifact(10, "generation-one", 99, 9),
    artifact(11, "generation-one", 100, 1),
    artifact(12, "generation-one", 100, 3),
    artifact(13, "other-generation", 999, 9),
    { id: 14, name: "sentinel-evidence-v1-unrelated", sizeInBytes: 1, expired: false },
  ], "generation-one");
  assert.equal(selected?.artifact.id, 12);
  assert.deepEqual(selected?.identity, { generation: "generation-one", runId: 100, runAttempt: 3 });

  assert.throws(
    () =>
      selectNewestSentinelCodexAuthStateArtifact([
        artifact(20, "generation-one", 100, 3),
        artifact(21, "generation-one", 100, 3),
      ], "generation-one"),
    assertStateError("ambiguous_artifact_identity"),
  );
});

Deno.test("Sentinel auth-state ciphertext has a distinct purpose and preserves exact complete documents", async () => {
  const key = new Uint8Array(32).fill(41);
  const firstRaw = authJson(1);
  const secondRaw = authJson(2);
  const first = parseSentinelCodexAuthStateDocument(firstRaw, 1);
  const second = parseSentinelCodexAuthStateDocument(secondRaw, 2);
  const encrypted = await serializeSentinelCodexAuthState(
    {
      repository: "ubiquity/ai.ubq.fi",
      generation: "generation-one",
      runId: 200,
      runAttempt: 2,
      documents: [first, second],
    },
    key,
    new Uint8Array(12).fill(17),
  );
  try {
    assert.equal(new TextDecoder().decode(encrypted).includes("refresh-token"), false);
    const restored = await deserializeSentinelCodexAuthState(encrypted, key, {
      repository: "ubiquity/ai.ubq.fi",
      generation: "generation-one",
      runId: 200,
      runAttempt: 2,
    });
    assert.deepEqual(restored.documents.map((document) => document.rawJson), [firstRaw, secondRaw]);
    assert.equal(JSON.parse(restored.documents[0]!.rawJson).future_top_level_field.preserve, true);
    assert.equal(JSON.parse(restored.documents[1]!.rawJson).tokens.future_token_field, "preserve-2");

    await assert.rejects(() => decryptSentinelArtifact(encrypted, key));
    const tampered = encrypted.slice();
    tampered[tampered.byteLength - 2] ^= 1;
    await assert.rejects(() =>
      deserializeSentinelCodexAuthState(tampered, key, {
        repository: "ubiquity/ai.ubq.fi",
        generation: "generation-one",
        runId: 200,
        runAttempt: 2,
      })
    );
    tampered.fill(0);

    const firstAccountDigest = await sentinelCodexAuthAccountDigest(first.accountId);
    await assert.rejects(
      () =>
        serializeSentinelCodexAuthState({
          repository: "ubiquity/ai.ubq.fi",
          generation: "generation-one",
          runId: 201,
          runAttempt: 1,
          documents: [parseSentinelCodexAuthStateDocument(authJson(1, "replacement-account"), 1)],
          expectedAccountDigests: { 1: firstAccountDigest },
        }, key),
      assertStateError("account_identity_changed"),
    );
  } finally {
    encrypted.fill(0);
    key.fill(0);
  }
});

Deno.test("Sentinel evidence and auth-state ciphertext cannot be cross-decrypted", async () => {
  const key = new Uint8Array(32).fill(52);
  const files = [{ path: "manifest.json", bytes: new TextEncoder().encode("purpose fixture") }];
  const evidence = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(1));
  const authState = await encryptSentinelAuthStateArtifact(files, key, new Uint8Array(12).fill(2));
  try {
    await assert.rejects(() => decryptSentinelAuthStateArtifact(evidence, key));
    await assert.rejects(() => decryptSentinelArtifact(authState, key));
    await assert.rejects(() =>
      encryptSentinelAuthStateArtifact(
        [
          { path: "one", bytes: new Uint8Array() },
          { path: "two", bytes: new Uint8Array() },
          { path: "three", bytes: new Uint8Array() },
          { path: "four", bytes: new Uint8Array() },
        ],
        key,
      )
    );
  } finally {
    evidence.fill(0);
    authState.fill(0);
    files[0]!.bytes.fill(0);
    key.fill(0);
  }
});

Deno.test("Sentinel auth-state restore rejects an equal or newer artifact than its current writer", async () => {
  const key = new Uint8Array(32).fill(61);
  let downloads = 0;
  try {
    for (
      const [sourceRunId, sourceRunAttempt, writerRunId, writerRunAttempt] of [
        [500, 2, 500, 2],
        [500, 3, 500, 2],
        [501, 1, 500, 9],
      ] as const
    ) {
      await assert.rejects(
        () =>
          restoreSentinelCodexAuthState({
            artifacts: [artifact(300 + sourceRunAttempt, "generation-one", sourceRunId, sourceRunAttempt)],
            repository: "ubiquity/ai.ubq.fi",
            generation: "generation-one",
            currentWriterIdentity: { runId: writerRunId, runAttempt: writerRunAttempt },
            keyBytes: key,
            downloadEnvelope: () => {
              downloads++;
              return Promise.resolve(new Uint8Array());
            },
          }),
        assertStateError("artifact_not_older_than_writer"),
      );
    }
    assert.equal(downloads, 0);
    assert.doesNotThrow(() =>
      assertSentinelCodexAuthStateArtifactPrecedesWriter(
        { generation: "generation-one", runId: 499, runAttempt: 9 },
        { runId: 500, runAttempt: 1 },
      )
    );
    assert.doesNotThrow(() =>
      assertSentinelCodexAuthStateArtifactPrecedesWriter(
        { generation: "generation-one", runId: 500, runAttempt: 1 },
        { runId: 500, runAttempt: 2 },
      )
    );
  } finally {
    key.fill(0);
  }
});

Deno.test({
  name: "Sentinel auth-state seal repeats the source-before-writer fence",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const parent = await Deno.makeTempDir({ prefix: "sentinel-auth-state-seal-fence-test-" });
    const stateDirectory = `${parent}/state`;
    const key = new Uint8Array(32).fill(62);
    try {
      await writePreparedSentinelCodexAuthState(stateDirectory, {
        source: "artifact",
        sourceIdentity: { generation: "generation-one", runId: 500, runAttempt: 1 },
        repository: "ubiquity/ai.ubq.fi",
        generation: "generation-one",
        documents: [parseSentinelCodexAuthStateDocument(authJson(1), 1)],
        needsMaintenance: true,
      });
      await assert.rejects(
        () =>
          sealPreparedSentinelCodexAuthState(
            stateDirectory,
            { repository: "ubiquity/ai.ubq.fi", generation: "generation-one", runId: 500, runAttempt: 1 },
            key,
          ),
        assertStateError("artifact_not_older_than_writer"),
      );
      const sealed = await sealPreparedSentinelCodexAuthState(
        stateDirectory,
        { repository: "ubiquity/ai.ubq.fi", generation: "generation-one", runId: 500, runAttempt: 2 },
        key,
        new Uint8Array(12).fill(8),
      );
      assert.equal(sealed.artifactName, "sentinel-codex-auth-state-v1-generation-one-r500-a2");
      sealed.encrypted.fill(0);
    } finally {
      key.fill(0);
      await Deno.remove(parent, { recursive: true });
    }
  },
});

Deno.test({
  name: "prepared auth-state rejects a symlinked auth document before seal",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const parent = await Deno.makeTempDir({ prefix: "sentinel-auth-state-symlink-test-" });
    const stateDirectory = `${parent}/state`;
    const key = new Uint8Array(32).fill(64);
    try {
      await writePreparedSentinelCodexAuthState(stateDirectory, {
        source: "bootstrap",
        sourceIdentity: null,
        repository: "ubiquity/ai.ubq.fi",
        generation: "generation-one",
        documents: [parseSentinelCodexAuthStateDocument(authJson(1), 1)],
        needsMaintenance: true,
      });
      const authPath = `${stateDirectory}/slots/1/auth.json`;
      const targetPath = `${parent}/valid-auth.json`;
      await Deno.writeTextFile(targetPath, authJson(1), { createNew: true, mode: 0o600 });
      await Deno.remove(authPath);
      await Deno.symlink(targetPath, authPath);

      const prepared = await readPreparedSentinelCodexAuthState(stateDirectory);
      assert.equal(prepared.readiness[1], "invalid_document");
      await assert.rejects(
        () =>
          sealPreparedSentinelCodexAuthState(
            stateDirectory,
            { repository: "ubiquity/ai.ubq.fi", generation: "generation-one", runId: 600, runAttempt: 1 },
            key,
          ),
        assertStateError("prepared_state_invalid"),
      );
    } finally {
      key.fill(0);
      await Deno.remove(parent, { recursive: true });
    }
  },
});

Deno.test("Sentinel auth-state restore never falls back from a corrupt newest artifact", async () => {
  const key = new Uint8Array(32).fill(63);
  const older = artifact(301, "generation-one", 300, 1);
  const newest = artifact(302, "generation-one", 301, 1, { sizeInBytes: 3 });
  const olderEnvelope = await serializeSentinelCodexAuthState(
    {
      repository: "ubiquity/ai.ubq.fi",
      generation: "generation-one",
      runId: 300,
      runAttempt: 1,
      documents: [parseSentinelCodexAuthStateDocument(authJson(1), 1)],
    },
    key,
    new Uint8Array(12).fill(4),
  );
  const downloads: number[] = [];
  try {
    await assert.rejects(
      () =>
        restoreSentinelCodexAuthState({
          artifacts: [older, newest],
          repository: "ubiquity/ai.ubq.fi",
          generation: "generation-one",
          currentWriterIdentity: { runId: 302, runAttempt: 1 },
          keyBytes: key,
          seeds: { slot1B64: "not-valid-base64" },
          downloadEnvelope: (candidate) => {
            downloads.push(candidate.id);
            return Promise.resolve(candidate.id === newest.id ? new Uint8Array([1, 2, 3]) : olderEnvelope.slice());
          },
        }),
      assertStateError("artifact_invalid"),
    );
    assert.deepEqual(downloads, [newest.id]);
  } finally {
    olderEnvelope.fill(0);
    key.fill(0);
  }
});

Deno.test("Sentinel auth-state seeds are bootstrap-only and expired state does not use them", async () => {
  const key = new Uint8Array(32).fill(74);
  try {
    const bootstrapped = await restoreSentinelCodexAuthState({
      artifacts: [],
      repository: "ubiquity/ai.ubq.fi",
      generation: "generation-two",
      currentWriterIdentity: { runId: 401, runAttempt: 1 },
      keyBytes: key,
      seeds: { slot1B64: encodeSeed(authJson(1)) },
      downloadEnvelope: () => Promise.reject(new Error("must not download")),
    });
    assert.equal(bootstrapped.source, "bootstrap");
    assert.equal(bootstrapped.documents[0]!.rawJson, authJson(1));

    await assert.rejects(
      () =>
        restoreSentinelCodexAuthState({
          artifacts: [artifact(401, "generation-two", 400, 1, { expired: true })],
          repository: "ubiquity/ai.ubq.fi",
          generation: "generation-two",
          currentWriterIdentity: { runId: 401, runAttempt: 1 },
          keyBytes: key,
          seeds: { slot1B64: encodeSeed(authJson(1)) },
          downloadEnvelope: () => Promise.reject(new Error("must not download")),
        }),
      assertStateError("artifact_expired"),
    );
  } finally {
    key.fill(0);
  }
});

Deno.test("Sentinel auth-state probe enforces 50-minute validity and passively checks usage", async () => {
  const nowMs = 1_800_000_000_000;
  const expiring = parseSentinelCodexAuthStateDocument(
    authJson(1, "account-1", Math.floor((nowMs + SENTINEL_CODEX_AUTH_PROBE_MINIMUM_VALIDITY_MS) / 1_000)),
    1,
  );
  const healthy = parseSentinelCodexAuthStateDocument(
    authJson(2, "account-2", Math.floor((nowMs + 2 * 60 * 60_000) / 1_000)),
    2,
  );
  const calls: Array<Readonly<{ url: string; account: string | null; body: BodyInit | null | undefined }>> = [];
  const result = await probeSentinelCodexAuthDocuments([expiring, healthy], {
    now: () => nowMs,
    createTimeoutSignal: () => new AbortController().signal,
    probeRetry: { attempts: 1 },
    fetcher: (input, init) => {
      const headers = new Headers(init?.headers);
      calls.push({
        url: String(input),
        account: headers.get("ChatGPT-Account-ID"),
        body: init?.body,
      });
      return Promise.resolve(
        new Response(
          JSON.stringify({
            rate_limit: {
              primary_window: { used_percent: 25 },
              secondary_window: null,
            },
          }),
          { status: 200 },
        ),
      );
    },
  });

  assert.equal(SENTINEL_CODEX_AUTH_PROBE_MINIMUM_VALIDITY_MS, 50 * 60_000);
  assert.deepEqual(calls, [{
    url: "https://chatgpt.com/backend-api/wham/usage",
    account: "account-2",
    body: undefined,
  }]);
  assert.deepEqual(result, {
    usable: true,
    selectedSlot: 2,
    slots: [{
      slot: 1,
      code: "access_token_expiring",
      httpStatus: null,
      headroomPercent: null,
    }, {
      slot: 2,
      code: "available",
      httpStatus: 200,
      headroomPercent: 75,
    }],
  });
  assert.equal(JSON.stringify(result).includes("refresh-token"), false);
  assert.equal(JSON.stringify(result).includes("account-2"), false);
});

Deno.test({
  name: "prepared auth-state probe reports only safe 401 and quota failure categories",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const nowMs = 1_800_000_000_000;
    const parent = await Deno.makeTempDir({ prefix: "sentinel-auth-state-probe-test-" });
    const stateDirectory = `${parent}/state`;
    try {
      const documents = ([1, 2] as const).map((slot) =>
        parseSentinelCodexAuthStateDocument(
          authJson(slot, `account-${slot}`, Math.floor((nowMs + 2 * 60 * 60_000) / 1_000)),
          slot,
        )
      );
      await writePreparedSentinelCodexAuthState(stateDirectory, {
        source: "bootstrap",
        sourceIdentity: null,
        repository: "ubiquity/ai.ubq.fi",
        generation: "generation-probe",
        documents,
        needsMaintenance: true,
      });
      const calls: string[] = [];
      const result = await probePreparedSentinelCodexAuthState(stateDirectory, {
        now: () => nowMs,
        createTimeoutSignal: () => new AbortController().signal,
        probeRetry: { attempts: 1 },
        fetcher: (_input, init) => {
          const account = new Headers(init?.headers).get("ChatGPT-Account-ID");
          calls.push(account ?? "missing");
          if (account === "account-1") return Promise.resolve(new Response("unauthorized", { status: 401 }));
          return Promise.resolve(
            new Response(
              JSON.stringify({
                rate_limit: {
                  primary_window: { used_percent: 100 },
                  secondary_window: null,
                },
              }),
              { status: 200 },
            ),
          );
        },
      });

      assert.deepEqual(calls.sort(), ["account-1", "account-2"]);
      assert.deepEqual(result, {
        usable: false,
        selectedSlot: null,
        slots: [{ slot: 1, code: "http_error", httpStatus: 401, headroomPercent: null }, {
          slot: 2,
          code: "quota_exhausted",
          httpStatus: 200,
          headroomPercent: null,
        }],
      });
      const rendered = JSON.stringify(result);
      assert.equal(rendered.includes("refresh-token"), false);
      assert.equal(rendered.includes("account-"), false);
      assert.equal(rendered.includes("unauthorized"), false);
    } finally {
      await Deno.remove(parent, { recursive: true });
    }
  },
});

Deno.test({
  name: "prepared auth-state control rejects non-null bootstrap source identities",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const parent = await Deno.makeTempDir({ prefix: "sentinel-auth-state-control-test-" });
    const stateDirectory = `${parent}/state`;
    try {
      await Deno.mkdir(stateDirectory, { mode: 0o700 });
      await Deno.writeTextFile(
        `${stateDirectory}/auth-state-control-v1.json`,
        JSON.stringify({
          schema_version: 1,
          purpose: "sentinel_codex_auth_state_prepared",
          repository: "ubiquity/ai.ubq.fi",
          generation: "generation-control",
          source: "bootstrap",
          source_run_id: "invalid",
          source_run_attempt: null,
          slots: [{ slot: 1, account_id_sha256: "0".repeat(64) }],
        }),
        { createNew: true, mode: 0o600 },
      );
      await assert.rejects(
        () => readPreparedSentinelCodexAuthState(stateDirectory),
        assertStateError("prepared_state_invalid"),
      );
    } finally {
      await Deno.remove(parent, { recursive: true });
    }
  },
});
