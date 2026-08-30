import assert from "node:assert/strict";
import {
  assertSentinelRecoveryTransition,
  parseSentinelRecoveryRecord,
  parseSentinelReleaseRecord,
  sentinelRecoveryCandidateBranch,
} from "../scripts/sentinel/recovery.ts";
import {
  acquireSentinelRecoveryLease,
  emptySentinelRecoveryLedger,
  nonTerminalSentinelRecoveryRecords,
  parseSentinelRecoveryLedger,
  renderSentinelRecoveryLedger,
  sentinelRecoveryIdentityKey,
  type SentinelRecoveryLedgerV1,
  upsertSentinelRecoveryRecord,
} from "../scripts/sentinel/recovery-ledger.ts";
import type { SentinelRecoveryRecordV1 } from "../scripts/sentinel/recovery.ts";
import {
  readGitHubSentinelRecoveryLedger,
  writeGitHubSentinelRecoveryLedger,
} from "../scripts/sentinel/recovery-github-store.ts";
import { runSentinelRecoveryPass } from "../scripts/sentinel/recovery-controller.ts";

const record = (overrides: Record<string, unknown> = {}) => ({
  schema_version: 1,
  identity: {
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    candidate_generation: 1,
  },
  run_id: "33197180235",
  attempt: 1,
  lease_token: "lease-1",
  base_sha: "b".repeat(40),
  phase: "workspace_dirty",
  disposition: "active",
  state_version: 2,
  created_at: "2026-08-28T18:00:00.000Z",
  updated_at: "2026-08-28T18:01:00.000Z",
  candidate_branch: null,
  candidate_sha: null,
  changed_files: ["scripts/sentinel/main.ts"],
  tree_sha: null,
  failure_class: null,
  failure_fingerprint: null,
  artifact_ids: [],
  artifact_digests: [],
  reason: null,
  next_action: "publish checkpoint",
  predecessor: null,
  ...overrides,
});

Deno.test("recovery records require a durable branch and SHA for checkpoint_durable", () => {
  assert.throws(() => parseSentinelRecoveryRecord(record({ phase: "checkpoint_durable" })), /requires a branch/);
  const durable = parseSentinelRecoveryRecord(record({
    phase: "checkpoint_durable",
    candidate_branch: "sentinel/candidate-33197180235-1",
    candidate_sha: "c".repeat(40),
  }));
  assert.equal(durable.disposition, "active");
});

Deno.test("recovery transition validates identity, lease, and monotonic state", () => {
  const previous = parseSentinelRecoveryRecord(record());
  const next = parseSentinelRecoveryRecord(record({ phase: "checkpoint_publishing", state_version: 3 }));
  assert.doesNotThrow(() => assertSentinelRecoveryTransition(previous, next));
  assert.throws(
    () =>
      assertSentinelRecoveryTransition(
        previous,
        parseSentinelRecoveryRecord(record({ phase: "delivered", disposition: "delivered", state_version: 3 })),
      ),
    /Invalid Sentinel recovery transition/,
  );
  assert.throws(
    () =>
      assertSentinelRecoveryTransition(
        previous,
        parseSentinelRecoveryRecord(record({ phase: "checkpoint_publishing", state_version: 4 })),
      ),
    /state version/,
  );
});

Deno.test("release records require immutable stable identity and evidence", () => {
  const release = parseSentinelReleaseRecord({
    schema_version: 1,
    stable_sha: "d".repeat(40),
    candidate_sha: null,
    acceptance_evidence: ["ci:123", "health:revision"],
    activated_at: "2026-08-28T18:00:00.000Z",
    rollback_reason: null,
    generation: 2,
  });
  assert.equal(release.generation, 2);
  assert.throws(() => parseSentinelReleaseRecord({ ...release, stable_sha: "invalid" }), /invalid/);
});

Deno.test("recovery ledger provides bounded create, CAS, lease, and non-terminal lookup", () => {
  const claimed = parseSentinelRecoveryRecord(record({
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
  }));
  const created = upsertSentinelRecoveryRecord(emptySentinelRecoveryLedger(), claimed, null);
  assert.deepEqual(nonTerminalSentinelRecoveryRecords(created), [claimed]);
  const leased = acquireSentinelRecoveryLease(created, {
    identity: claimed.identity,
    owner: "run-1",
    token: "lease-1",
    now: "2026-08-28T18:00:00.000Z",
    expires_at: "2026-08-28T18:10:00.000Z",
  });
  assert.throws(
    () =>
      acquireSentinelRecoveryLease(leased, {
        identity: claimed.identity,
        owner: "run-2",
        token: "lease-2",
        now: "2026-08-28T18:01:00.000Z",
        expires_at: "2026-08-28T18:11:00.000Z",
      }),
    /leased by another owner/u,
  );
  const running = parseSentinelRecoveryRecord(record({
    phase: "implementation_running",
    state_version: 2,
    candidate_branch: null,
    candidate_sha: null,
    updated_at: "2026-08-28T18:02:00.000Z",
  }));
  const updated = upsertSentinelRecoveryRecord(leased, running, 1);
  assert.equal(
    parseSentinelRecoveryLedger(JSON.parse(renderSentinelRecoveryLedger(updated))).records[0]?.state_version,
    2,
  );
  assert.throws(() => upsertSentinelRecoveryRecord(updated, running, 1), /compare-and-swap/u);
});

Deno.test("recovery ledger prunes the oldest terminal record before reaching its hard cap", () => {
  const terminal = Array.from({ length: 512 }, (_, index) =>
    parseSentinelRecoveryRecord(record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: String(index + 1),
        source_revision: "a".repeat(64),
        candidate_generation: 1,
      },
      phase: "rejected",
      disposition: "rejected",
      updated_at: new Date(Date.parse("2026-08-28T18:01:00.000Z") + index).toISOString(),
      reason: "rejected/no_candidate_diff",
      next_action: null,
    })));
  const full = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: terminal });
  const claimed = parseSentinelRecoveryRecord(record({
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: "513",
      source_revision: "a".repeat(64),
      candidate_generation: 1,
    },
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
  }));
  const pruned = upsertSentinelRecoveryRecord(full, claimed, null);
  assert.equal(pruned.records.length, 512);
  assert.equal(pruned.records.some((entry) => entry.identity.source_id === "1"), false);
  assert.equal(pruned.records.some((entry) => entry.identity.source_id === "513"), true);
});

Deno.test("GitHub recovery state uses a separate fast-forward-only ref", async () => {
  const base = "a".repeat(40);
  const baseTree = "b".repeat(40);
  const blob = "c".repeat(40);
  const nextTree = "d".repeat(40);
  const nextCommit = "e".repeat(40);
  const ledgerText = renderSentinelRecoveryLedger(emptySentinelRecoveryLedger());
  const encoded = btoa(ledgerText);
  const requests: Array<{ url: string; init: RequestInit }> = [];
  const responses = [
    new Response("not found", { status: 404 }),
    Response.json({ object: { sha: base } }),
    Response.json({ tree: { sha: baseTree } }),
    Response.json({ encoding: "base64", content: encoded }),
    Response.json({ sha: blob }),
    Response.json({ sha: nextTree }),
    Response.json({ sha: nextCommit }),
    Response.json({ ref: "refs/heads/sentinel/recovery-state", object: { sha: nextCommit } }),
  ];
  const fetcher: typeof fetch = (url, init = {}) => {
    requests.push({ url: String(url), init });
    const response = responses.shift();
    if (!response) throw new Error("Unexpected GitHub recovery-state request");
    return Promise.resolve(response);
  };
  const snapshot = await readGitHubSentinelRecoveryLedger({
    token: "token",
    repository: "ubiquity/ai.ubq.fi",
    fetcher,
  });
  assert.equal(snapshot.state_ref_exists, false);
  const written = await writeGitHubSentinelRecoveryLedger({
    token: "token",
    repository: "ubiquity/ai.ubq.fi",
    snapshot,
    ledger: snapshot.ledger,
    message: "chore(sentinel): claim recovery work",
    fetcher,
  });
  assert.equal(written.commit_sha, nextCommit);
  assert.equal(requests.at(-1)?.url.endsWith("/git/refs"), true);
  assert.deepEqual(JSON.parse(String(requests.at(-1)?.init.body)), {
    ref: "refs/heads/sentinel/recovery-state",
    sha: nextCommit,
  });
});

Deno.test("GitHub recovery state fetches the exact blob for an over-1-MiB contents response", async () => {
  const commit = "a".repeat(40);
  const tree = "b".repeat(40);
  const blobSha = "c".repeat(40);
  // Above 1 MiB the Contents API returns valid file metadata only
  // (`content: ""`, `encoding: "none"`) and keeps the payload in the Git Data
  // blob the validated SHA pins; this mirrors the live recovery branch.
  const ledger = parseSentinelRecoveryLedger({
    schema_version: 1,
    records: [parseSentinelRecoveryRecord(record({ reason: "r".repeat(1_100_000) }))],
    retry_history: [],
    retry_decisions: [],
    leases: [],
  });
  const ledgerText = renderSentinelRecoveryLedger(ledger);
  const size = new TextEncoder().encode(ledgerText).byteLength;
  assert.ok(size > 1_048_576);
  const responses = [
    Response.json({ ref: "refs/heads/sentinel/recovery-state", object: { sha: commit } }),
    Response.json({ sha: commit, tree: { sha: tree } }),
    Response.json({ type: "file", encoding: "none", content: "", size, sha: blobSha }),
    Response.json({ sha: blobSha, size, encoding: "base64", content: btoa(ledgerText) }),
  ];
  const requests: Array<{ url: string }> = [];
  const fetcher: typeof fetch = (url) => {
    requests.push({ url: String(url) });
    const response = responses.shift();
    if (!response) throw new Error("Unexpected large recovery-state request");
    return Promise.resolve(response);
  };
  const snapshot = await readGitHubSentinelRecoveryLedger({
    token: "token",
    repository: "ubiquity/ai.ubq.fi",
    fetcher,
  });
  assert.equal(snapshot.state_ref_exists, true);
  assert.equal(snapshot.commit_sha, commit);
  assert.equal(snapshot.tree_sha, tree);
  assert.deepEqual(snapshot.ledger, ledger);
  assert.equal(requests.length, 4);
  assert.equal(requests[2]?.url.endsWith(`/contents/docs/sentinel-recovery-records.json?ref=${commit}`), true);
  assert.equal(requests[3]?.url.endsWith(`/git/blobs/${blobSha}`), true);
});

Deno.test("GitHub recovery state fails closed on malformed large-file blob responses", async () => {
  const commit = "1".repeat(40);
  const tree = "2".repeat(40);
  const blobSha = "3".repeat(40);
  const encoded = btoa(renderSentinelRecoveryLedger(emptySentinelRecoveryLedger()));
  const contents = { type: "file", encoding: "none", content: "", size: 1_049_697, sha: blobSha };
  const readWith = (contentsValue: unknown, blob: unknown) => {
    const responses = [
      Response.json({ object: { sha: commit } }),
      Response.json({ tree: { sha: tree } }),
      Response.json(contentsValue),
      Response.json(blob),
    ];
    const fetcher: typeof fetch = () => {
      const response = responses.shift();
      if (!response) throw new Error("Unexpected malformed large-file response");
      return Promise.resolve(response);
    };
    return readGitHubSentinelRecoveryLedger({ token: "token", repository: "ubiquity/ai.ubq.fi", fetcher });
  };
  // A blob whose SHA does not match the validated file SHA is not the exact
  // blob the contents metadata identified and must never be accepted.
  await assert.rejects(
    readWith(contents, { sha: "4".repeat(40), encoding: "base64", content: encoded }),
    /file is invalid/u,
  );
  // The Git Data blob must carry base64 content; metadata-only or
  // non-base64 payloads are malformed and fail closed.
  await assert.rejects(readWith(contents, { sha: blobSha, encoding: "base64" }), /file is invalid/u);
  await assert.rejects(readWith(contents, { sha: blobSha, encoding: "none", content: "" }), /file is invalid/u);
  await assert.rejects(
    readWith(contents, { sha: "not-a-sha", encoding: "base64", content: encoded }),
    /blob SHA is invalid/u,
  );
  // Contents metadata without a valid full SHA cannot pin a blob at all.
  await assert.rejects(
    readWith({ ...contents, sha: "not-a-sha" }, { sha: blobSha, encoding: "base64", content: encoded }),
    /file SHA is invalid/u,
  );
});

Deno.test("an empty durable recovery pass performs no state write", async () => {
  const commit = "1".repeat(40);
  const tree = "2".repeat(40);
  const encoded = btoa(renderSentinelRecoveryLedger(emptySentinelRecoveryLedger()));
  const methods: string[] = [];
  const responses = [
    Response.json({ object: { sha: commit } }),
    Response.json({ tree: { sha: tree } }),
    Response.json({ encoding: "base64", content: encoded }),
  ];
  const fetcher: typeof fetch = (_url, init = {}) => {
    methods.push(init.method ?? "GET");
    const response = responses.shift();
    if (!response) throw new Error("Unexpected recovery pass request");
    return Promise.resolve(response);
  };
  assert.deepEqual(
    await runSentinelRecoveryPass({
      token: "token",
      repository: "ubiquity/ai.ubq.fi",
      owner: "run-1",
      now: "2026-08-28T18:00:00.000Z",
      fetcher,
    }),
    [],
  );
  assert.deepEqual(methods, ["GET", "GET", "GET"]);
});

type RecoveryPassDelivery = Readonly<{
  number: number;
  head_branch: string;
  head_sha: string;
  state: "open" | "closed";
  merged_at: string | null;
}>;

/**
 * Minimal stateful GitHub API double for the recovery pass: it answers the
 * ledger state ref, development ref, exact candidate refs, and pull-request
 * observations with one mutable ledger snapshot.
 */
const recoveryPassFetcher = (
  input: Readonly<{
    ledger: SentinelRecoveryLedgerV1;
    candidateShas: Readonly<Record<string, string>>;
    deliveries?: Readonly<Record<string, readonly RecoveryPassDelivery[]>>;
  }>,
): Readonly<{ fetcher: typeof fetch; ledger: () => SentinelRecoveryLedgerV1 }> => {
  const developmentSha = "1".repeat(40);
  let stateSha: string | null = null;
  const blobSha = "2".repeat(40);
  const treeSha = "3".repeat(40);
  let commitCounter = 1;
  let currentLedger = parseSentinelRecoveryLedger(input.ledger);
  const nextCommitSha = (): string => (++commitCounter).toString(16).padStart(40, "0");
  const fetcher: typeof fetch = (inputValue, init = {}) => {
    const url = new URL(String(inputValue));
    const path = url.pathname.replace(/^\/repos\/[^/]+\/[^/]+/u, "");
    if (path === "/git/ref/heads/sentinel/recovery-state") {
      return Promise.resolve(
        stateSha === null
          ? new Response("not found", { status: 404 })
          : Response.json({ ref: path, object: { sha: stateSha } }),
      );
    }
    if (path === "/git/ref/heads/development") {
      return Promise.resolve(Response.json({ ref: path, object: { sha: developmentSha } }));
    }
    const branch = path.match(/^\/git\/ref\/heads\/(sentinel\/candidate-.+)$/u)?.[1];
    if (branch) {
      const sha = input.candidateShas[branch];
      return Promise.resolve(
        sha === undefined ? new Response("not found", { status: 404 }) : Response.json({ ref: path, object: { sha } }),
      );
    }
    const commit = path.match(/^\/git\/commits\/([0-9a-f]{40})$/u);
    if (commit) return Promise.resolve(Response.json({ sha: commit[1], tree: { sha: treeSha } }));
    const contents = path.match(/^\/contents\/docs\/sentinel-recovery-records\.json$/u);
    if (contents) {
      return Promise.resolve(Response.json({
        encoding: "base64",
        content: btoa(renderSentinelRecoveryLedger(currentLedger)),
      }));
    }
    if (path === "/git/blobs") {
      const body = JSON.parse(String(init.body ?? "{}")) as { content?: unknown };
      if (typeof body.content === "string") {
        currentLedger = parseSentinelRecoveryLedger(JSON.parse(body.content));
      }
      return Promise.resolve(Response.json({ sha: blobSha }));
    }
    if (path === "/git/trees") {
      return Promise.resolve(Response.json({ sha: treeSha }));
    }
    if (path === "/git/commits") {
      return Promise.resolve(Response.json({ sha: nextCommitSha(), tree: { sha: treeSha } }));
    }
    if (path === "/git/refs") {
      stateSha = String((JSON.parse(String(init.body)) as { sha?: unknown }).sha ?? stateSha);
      return Promise.resolve(Response.json({ ref: path, object: { sha: stateSha } }));
    }
    if (path === "/git/refs/heads/sentinel/recovery-state") {
      stateSha = String((JSON.parse(String(init.body)) as { sha?: unknown }).sha ?? stateSha);
      return Promise.resolve(Response.json({ ref: path, object: { sha: stateSha } }));
    }
    const pulls = path.match(/^\/pulls$/u) && url.searchParams.get("base") === "development";
    if (pulls) {
      const head = url.searchParams.get("head")?.split(":", 2)[1] ?? null;
      return Promise.resolve(Response.json((input.deliveries?.[head ?? ""] ?? []).map((delivery) => ({
        number: delivery.number,
        state: delivery.state,
        merged_at: delivery.merged_at,
        head: { ref: delivery.head_branch, sha: delivery.head_sha },
        base: { ref: "development" },
      }))));
    }
    if (path.startsWith("/compare/development...")) {
      return Promise.resolve(Response.json({ status: "behind" }));
    }
    throw new Error(`Unexpected recovery pass request ${path}`);
  };
  return { fetcher, ledger: () => parseSentinelRecoveryLedger(currentLedger) };
};

const failureEvidenceRecord = (
  sourceKind: "triage" | "github_issue" = "triage",
): Readonly<{ record: SentinelRecoveryRecordV1; branchSha: string }> => {
  const sourceId = sourceKind === "triage" ? "triage-cell-1" : "208";
  const sourceRevision = "a".repeat(64);
  const candidateSha = "c".repeat(40);
  const parsed = parseSentinelRecoveryRecord({
    ...record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: sourceKind,
        source_id: sourceId,
        source_revision: sourceRevision,
        candidate_generation: 1,
      },
      lease_token: "lease-recovered",
      phase: "checkpoint_durable",
      state_version: 7,
      created_at: "2026-08-28T17:00:00.000Z",
      updated_at: "2026-08-28T17:05:00.000Z",
      candidate_branch: "sentinel/candidate-unused",
      candidate_sha: candidateSha,
      changed_files: ["candidate.txt"],
      tree_sha: "d".repeat(40),
      failure_class: "runner_interruption",
      failure_fingerprint: "8".repeat(64),
      reason: "encrypted candidate recovered into a quarantined checkpoint",
      next_action: "validate the checkpoint and request human review",
    }),
  });
  return { record: parsed, branchSha: candidateSha };
};

Deno.test({
  name: "a recovered durable checkpoint is reconciled to resume_validation without re-entering retry_wait",
  async fn() {
    const { record: child, branchSha } = failureEvidenceRecord();
    const deterministic = sentinelRecoveryCandidateBranch(child.identity);
    const durable: SentinelRecoveryRecordV1 = parseSentinelRecoveryRecord({
      ...child,
      candidate_branch: deterministic,
    });
    const key = sentinelRecoveryIdentityKey(durable.identity);
    const history = {
      schema_version: 1 as const,
      identity: durable.identity,
      attempt: 1,
      failure_class: "runner_interruption" as const,
      failure_fingerprint: "8".repeat(64),
      observed_at: "2026-08-28T17:00:00.000Z",
    };
    const decision = {
      disposition: "retry_wait" as const,
      should_retry: true,
      circuit_open: false,
      validation_repair_allowed: false,
      source_revision_changed: false,
      candidate_generation: 1,
      attempt_count: 1,
      identical_failure_count: 1,
      backoff_ms: 60_000,
      retry_at: "2026-08-28T17:01:00.000Z",
      failure_class: "runner_interruption" as const,
      failure_fingerprint: "8".repeat(64),
      reason: "The bounded Sentinel retry policy scheduled another attempt.",
      next_action: "Retry after 2026-08-28T17:01:00.000Z.",
    };
    const { fetcher, ledger } = recoveryPassFetcher({
      ledger: {
        schema_version: 1,
        records: [durable],
        retry_history: [history],
        retry_decisions: [{ identity_key: key, decision }],
        leases: [],
      },
      candidateShas: { [deterministic]: branchSha },
    });
    const results = await runSentinelRecoveryPass({
      token: "token",
      repository: "ubiquity/ai.ubq.fi",
      owner: "run-1",
      now: "2026-08-28T18:00:00.000Z",
      fetcher,
    });
    assert.deepEqual(results.map((result) => result.action), ["resume_validation"]);
    const after = ledger();
    const reconciled = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key)!;
    // The successfully reconstructed checkpoint stays durable and never
    // re-enters retry_wait, while the original classified failure evidence
    // remains preserved on the record and in the bounded retry metadata.
    assert.equal(reconciled.phase, "checkpoint_durable");
    assert.equal(reconciled.disposition, "active");
    assert.equal(reconciled.state_version, durable.state_version);
    assert.equal(reconciled.candidate_sha, branchSha);
    assert.equal(reconciled.failure_class, "runner_interruption");
    assert.equal(reconciled.failure_fingerprint, "8".repeat(64));
    assert.equal(after.retry_history.length, 1);
    assert.equal(after.retry_history[0]!.failure_class, "runner_interruption");
    assert.equal(after.retry_decisions.length, 1);
    assert.equal(after.retry_decisions[0]!.identity_key, key);
    assert.equal(after.retry_decisions[0]!.decision.disposition, "retry_wait");
  },
});

Deno.test({
  name: "a recovered checkpoint with a merged delivery progresses to review instead of retry_wait",
  async fn() {
    const { record, branchSha } = failureEvidenceRecord("github_issue");
    const deterministic = sentinelRecoveryCandidateBranch(record.identity);
    const durable: SentinelRecoveryRecordV1 = parseSentinelRecoveryRecord({
      ...record,
      candidate_branch: deterministic,
      candidate_sha: branchSha,
    });
    const key = sentinelRecoveryIdentityKey(durable.identity);
    const { fetcher, ledger } = recoveryPassFetcher({
      ledger: {
        schema_version: 1,
        records: [durable],
        retry_history: [],
        retry_decisions: [],
        leases: [],
      },
      candidateShas: { [deterministic]: branchSha },
      deliveries: {
        [deterministic]: [{
          number: 7,
          head_branch: deterministic,
          head_sha: branchSha,
          state: "closed",
          merged_at: "2026-08-28T17:30:00.000Z",
        }],
      },
    });
    const results = await runSentinelRecoveryPass({
      token: "token",
      repository: "ubiquity/ai.ubq.fi",
      owner: "run-1",
      now: "2026-08-28T18:00:00.000Z",
      fetcher,
    });
    assert.deepEqual(results.map((result) => result.action), ["resume_review"]);
    const after = ledger();
    const reconciled = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key)!;
    // The merged delivery moves the recovered checkpoint into review; the
    // preserved failure class must not demote it back to retry_wait.
    assert.equal(reconciled.phase, "review_pending");
    assert.equal(reconciled.disposition, "active");
    assert.equal(reconciled.failure_class, "runner_interruption");
    assert.equal(reconciled.failure_fingerprint, "8".repeat(64));
    assert.equal(after.retry_history.length, 0);
    assert.equal(after.retry_decisions.length, 0);
  },
});
