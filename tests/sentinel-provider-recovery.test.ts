// Bounded Provider Sentinel recovery-operation tests.
//
// Exercises the exported recoverProviderTransaction operation with the real
// DenoDeployClient through a synthetic transport and an injected in-memory
// exact-parent state implementing ProviderRecoveryState. No file system, no
// subprocess, no network, no real credentials: the Deno client token and the
// GitHub authority token are synthetic and every retained event is asserted to
// never contain them. The fixture supports the exact success scenarios and the
// fail-closed fault table below; it never reimplements the production storage
// transition validator.

import assert from "node:assert/strict";

import {
  defaultRevisionHealthUrl,
  DenoDeployClient,
  type SentinelFetch,
} from "../scripts/sentinel/bootstrap/deploy.ts";
import {
  type ProviderRecoveryDependencies,
  type ProviderRecoveryResult,
  type ProviderRecoveryState,
  type ProviderRecoveryStateSnapshot,
  recoverProviderTransaction,
} from "../scripts/sentinel/bootstrap/provider-recovery.ts";
import {
  parseSentinelProviderStateDocument,
  type SentinelProviderAttestationV1,
  type SentinelProviderExecutorV1,
  type SentinelProviderStateDocumentV1,
  type SentinelProviderTransactionV1,
} from "../scripts/sentinel/bootstrap/provider-state.ts";

const APP = "ai-ubq-fi" as const;
const ELSEWHERE_APP = "p-ai-ubq-fi" as const;
const DENO_TOKEN = "synthetic-deno-token";
const GITHUB_TOKEN = "synthetic-github-token";
const FULL_GIT_SHA = /^[0-9a-f]{40}$/u;
const SHA256_REFERENCE = /^sha256:[0-9a-f]{64}$/u;
const ISO_UTC = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/u;

const PRIOR: SentinelProviderAttestationV1 = {
  git_sha: "a".repeat(40),
  revision_id: "prior-revision",
  configuration_digest: "d".repeat(64),
  validator_sha: "c".repeat(40),
  corpus_digest: "e".repeat(64),
  verified_at: "2026-09-05T00:00:00.000Z",
  identity_ref: "artifact:prior-identity",
  inference_ref: "artifact:prior-inference",
};

const CANDIDATE: SentinelProviderAttestationV1 = {
  ...PRIOR,
  git_sha: "b".repeat(40),
  revision_id: "candidate-revision",
};

const EXECUTOR: SentinelProviderExecutorV1 = {
  repository: "ubiquity/ai.ubq.fi",
  workflow_path: ".github/workflows/sentinel-revision-control.yml",
  run_id: 42,
  run_attempt: 2,
};

const TRANSACTION: SentinelProviderTransactionV1 = {
  id: "fixture-release",
  fence_generation: 2,
  phase: "rollback_pending",
  previous: PRIOR,
  candidate: CANDIDATE,
  expected_merged_sha: CANDIDATE.git_sha,
  executor: EXECUTOR,
  retired_executor: null,
  previous_transaction_commit: null,
  created_at: "2026-09-05T00:01:00.000Z",
  promotion_intent_at: "2026-09-05T00:02:00.000Z",
  promotion_result: {
    kind: "acknowledged",
    http_status: 204,
    observed_at: "2026-09-05T00:03:00.000Z",
    evidence_ref: "artifact:promotion-204",
  },
  observation_deadline_at: "2026-09-05T00:32:00.000Z",
  observation: {
    last_observed_at: "2026-09-05T00:06:00.000Z",
    samples: 3,
    consecutive_liveness_failures: 3,
    consecutive_inference_failures: 3,
    invariant_id: null,
    consecutive_invariant_failures: 0,
  },
  route: {
    revision_id: CANDIDATE.revision_id,
    observed_at: "2026-09-05T00:06:00.000Z",
    evidence_ref: "artifact:candidate-route",
  },
  decision: "rollback",
  reason: "candidate_failed",
  rollback_intent_at: "2026-09-05T00:07:00.000Z",
  rollback_result: null,
  restoration: null,
};

const AUTHORITY_RESPONSE = {
  id: 42,
  run_attempt: 2,
  repository: { full_name: EXECUTOR.repository },
  path: EXECUTOR.workflow_path,
  status: "in_progress",
  conclusion: null,
  created_at: "2026-09-05T00:00:00Z",
  run_started_at: "2026-09-05T00:00:01Z",
  updated_at: "2026-09-05T00:07:59Z",
  html_url: "https://github.com/ubiquity/ai.ubq.fi/actions/runs/42/attempts/2",
};

const PRIOR_HEALTH_URL = defaultRevisionHealthUrl(APP, PRIOR.revision_id);
const MANAGED_HEALTH_URL = `https://${APP}.ubiquity-dao.deno.net/health`;
const CUSTOM_HEALTH_URL = "https://ai.ubq.fi/health";
const REVISION_BASE = "https://api.deno.com/v2";
const REVISION_LIST_URL = `${REVISION_BASE}/apps/${APP}/revisions?limit=100`;
const PROMOTE_URL = (revisionId: string): string => `${REVISION_BASE}/revisions/${revisionId}/promote`;

/** Fields the operation must never mutate while completing a rollback. */
const INVARIANT_FIELDS = [
  "id",
  "fence_generation",
  "previous",
  "candidate",
  "expected_merged_sha",
  "executor",
  "retired_executor",
  "previous_transaction_commit",
  "created_at",
  "promotion_intent_at",
  "promotion_result",
  "observation_deadline_at",
  "observation",
  "decision",
  "reason",
  "rollback_intent_at",
] as const;

const initialDocument = (): SentinelProviderStateDocumentV1 =>
  parseSentinelProviderStateDocument({
    schema_version: 1,
    generation: 2,
    applications: [
      { app: APP, healthy: PRIOR, transaction: TRANSACTION },
      { app: ELSEWHERE_APP, healthy: null, transaction: null },
    ],
  });

const sha256hex = async (value: string): Promise<string> => {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

/**
 * In-memory exact-parent state. readSnapshot/refresh return deep-cloned
 * snapshots, compareAndSet rejects a stale or invalid next document BEFORE
 * publishing, validates the document with the strict parser, and advances the
 * commit deterministically only on success. Two CAS injection modes: the first
 * rollback_pending_verification CAS (the acknowledgement CAS) publishes the
 * document and then throws, exactly like a lost confirmation after a durable
 * write; routeCasRejected returns false on the very first CAS before touching
 * the document, commit or published array, exactly like an external rejection
 * of the initial publication.
 */
class MemoryProviderRecoveryState implements ProviderRecoveryState {
  #document: SentinelProviderStateDocumentV1;
  #commit: string = "1".repeat(40);
  #published: SentinelProviderStateDocumentV1[] = [];
  #ackCasPublishThenThrow: boolean;
  #routeCasRejected: boolean;
  #casAttempts = 0;

  constructor(
    initial: SentinelProviderStateDocumentV1,
    options: Readonly<{
      ackCasPublishThenThrow?: boolean;
      routeCasRejected?: boolean;
    }> = {},
  ) {
    this.#document = parseSentinelProviderStateDocument(initial);
    this.#ackCasPublishThenThrow = options.ackCasPublishThenThrow === true;
    this.#routeCasRejected = options.routeCasRejected === true;
  }

  /** Number of compareAndSet attempts, including stale rejects and the rejected first CAS. */
  casAttempts(): number {
    return this.#casAttempts;
  }

  readSnapshot(): ProviderRecoveryStateSnapshot {
    return Object.freeze({
      document: structuredClone(this.#document),
      commit_sha: this.#commit,
      tree_sha: this.#commit,
      state_ref_exists: true,
    });
  }

  refresh(): Promise<ProviderRecoveryStateSnapshot> {
    return Promise.resolve(this.readSnapshot());
  }

  publishedDocuments(): readonly SentinelProviderStateDocumentV1[] {
    return this.#published;
  }

  compareAndSet(expectedCommitSha: string, nextDocument: unknown): Promise<boolean> {
    this.#casAttempts++;
    if (this.#routeCasRejected) {
      this.#routeCasRejected = false;
      return Promise.resolve(false);
    }
    if (expectedCommitSha !== this.#commit) return Promise.resolve(false);
    let parsed: SentinelProviderStateDocumentV1;
    try {
      parsed = parseSentinelProviderStateDocument(nextDocument);
    } catch {
      return Promise.resolve(false);
    }
    this.#document = parsed;
    this.#commit = (BigInt(`0x${this.#commit}`) + 1n).toString(16).padStart(40, "0");
    this.#published.push(structuredClone(parsed));
    const transaction = parsed.applications.find((entry) => entry.app === APP)?.transaction;
    if (
      this.#ackCasPublishThenThrow && transaction?.phase === "rollback_pending_verification" &&
      transaction.rollback_result?.kind === "acknowledged"
    ) {
      this.#ackCasPublishThenThrow = false;
      throw new TypeError("Synthetic ACK CAS publication confirm lost");
    }
    return Promise.resolve(true);
  }
}

interface TransportRow {
  readonly method: string;
  readonly url: string;
  revision: string | null;
  owns: boolean | null;
}

interface Scenario {
  readonly state: MemoryProviderRecoveryState;
  readonly transport: readonly TransportRow[];
  readonly retained: readonly Record<string, unknown>[];
  readonly retainedRefs: readonly string[];
  readonly posts: () => number;
  readonly guardRuns: () => number;
  readonly faultHits: () => number;
  readonly expectedCommitSha: () => string;
  invoke: (freshClockMs?: number) => Promise<ProviderRecoveryResult>;
}

/** Optional one-shot fault injections for the fail-closed negative table. */
type RecoveryFault =
  | "stale_initial"
  | "state_drift"
  | "later_route"
  | "expired_control"
  | "regressed_clock"
  | "terminal_executor"
  | "missing_control"
  | "route_cas_rejected"
  | "completion_route_changed";

interface ScenarioOptions {
  readonly postThrowsOnce?: boolean;
  readonly inferFailOnce?: boolean;
  readonly ackCasPublishThenThrow?: boolean;
  readonly routeCasRejected?: boolean;
  readonly fault?: RecoveryFault;
}

const createScenario = (options: ScenarioOptions = {}): Scenario => {
  const state = new MemoryProviderRecoveryState(initialDocument(), {
    ackCasPublishThenThrow: options.ackCasPublishThenThrow === true,
    routeCasRejected: options.routeCasRejected === true,
  });
  const transport: TransportRow[] = [];
  const retained: Record<string, unknown>[] = [];
  const retainedRefs: string[] = [];
  let routeOwner = CANDIDATE.revision_id;
  let posts = 0;
  let guardRuns = 0;
  let lockDepth = 0;
  let inferFailures = options.inferFailOnce === true ? 1 : 0;
  let clockMs = Date.parse("2026-09-05T00:08:00.000Z");
  let inPromotion = false;
  let promotionFaultApplied = false;
  let faultHits = 0;
  let expectedCommitShaUsed = "";

  const now = (): number => clockMs++;
  const iso = (): string => new Date(now()).toISOString();

  const identityResponse = (): Response =>
    new Response(JSON.stringify({ release: { git_sha: PRIOR.git_sha, deployment_id: PRIOR.revision_id } }), {
      headers: {
        "content-type": "application/json",
        "x-uos-git-sha": PRIOR.git_sha,
        "x-uos-deployment-id": PRIOR.revision_id,
      },
    });

  // Synthetic Deno transport: exactly the real client's read set and the one
  // prior-revision promotion POST. Candidate revision health, stable health
  // before the promotion POST, and any other URL/method are hard failures.
  const denoFetch: SentinelFetch = async (input, init) => {
    const url = new URL(input instanceof Request ? input.url : String(input));
    const method = init?.method ?? "GET";
    const row: TransportRow = { method, url: url.href, revision: null, owns: null };
    transport.push(row);
    if (url.origin === "https://api.deno.com") {
      assert.ok(method === "GET" || method === "POST", `unexpected Deno API method ${method}`);
      assert.equal(lockDepth, 1, "Deno API traffic must stay inside withPromotionLock");
      if (method === "POST") {
        assert.equal(url.pathname, `/v2/revisions/${PRIOR.revision_id}/promote`);
        assert.equal(url.href, PROMOTE_URL(PRIOR.revision_id));
        const durable = (await state.refresh()).document!;
        const durableTransaction = durable.applications[0]!.transaction!;
        assert.equal(durableTransaction.phase, "rollback_pending");
        assert.equal(durableTransaction.rollback_intent_at, TRANSACTION.rollback_intent_at);
        posts++;
        routeOwner = PRIOR.revision_id;
        if (options.postThrowsOnce === true && posts === 1) {
          throw new TypeError("Synthetic POST response lost");
        }
        return new Response(null, { status: 204 });
      }
      if (url.pathname === "/v2/apps/ai-ubq-fi/revisions") {
        assert.equal(method, "GET");
        assert.equal(url.href, REVISION_LIST_URL);
        assert.equal([...url.searchParams.keys()].length, 1);
        assert.equal(url.searchParams.get("limit"), "100");
        return Response.json([
          { id: PRIOR.revision_id, status: "succeeded" },
          { id: CANDIDATE.revision_id, status: "succeeded" },
        ]);
      }
      const match = /^\/v2\/revisions\/([a-z0-9-]+)$/u.exec(url.pathname);
      assert.ok(match !== null, `unexpected Deno API path ${url.pathname}`);
      const revisionId = match[1]!;
      assert.ok(
        revisionId === PRIOR.revision_id || revisionId === CANDIDATE.revision_id,
        `unexpected Deno revision target ${revisionId}`,
      );
      assert.equal(method, "GET");
      assert.equal(
        url.href,
        `${REVISION_BASE}/revisions/${revisionId}`,
        "revision reads must be exact hrefs without extra query",
      );
      // One-shot in-promotion fault injection: fired on the first exact
      // revision read inside the real client.promoteRevision call, i.e. after
      // the initial ownership+route CAS and before the real client's
      // beforePromote guard. The response is then formed from the faulted
      // state; never inject earlier or replace the real Deno client.
      if (inPromotion && !promotionFaultApplied) {
        promotionFaultApplied = true;
        switch (options.fault) {
          case "state_drift": {
            const snapshot = await state.refresh();
            const source = structuredClone(snapshot.document!);
            const drifted: SentinelProviderStateDocumentV1 = {
              ...source,
              generation: source.generation + 1,
            };
            const published = await state.compareAndSet(snapshot.commit_sha, drifted);
            assert.ok(published, "synthetic state drift CAS must publish at the current parent");
            faultHits++;
            break;
          }
          case "later_route":
            routeOwner = "unrelated-revision";
            faultHits++;
            break;
          case "expired_control":
            clockMs += 31000;
            faultHits++;
            break;
          case "regressed_clock":
            clockMs -= 1000;
            faultHits++;
            break;
          default:
            break;
        }
      }
      row.revision = revisionId;
      row.owns = routeOwner === revisionId;
      return Response.json({
        id: revisionId,
        status: "succeeded",
        timelines: [{
          name: "Production",
          context: "Production",
          hostnames: row.owns ? [`${APP}.ubiquity-dao.deno.net`] : [],
        }],
      });
    }
    if (url.hostname === `ai-ubq-fi-${PRIOR.revision_id}.ubiquity-dao.deno.net`) {
      assert.equal(method, "GET");
      assert.equal(url.href, PRIOR_HEALTH_URL, "prior health must use the exact prior revision health URL");
      assert.equal(lockDepth, 0, "prior health check must run outside withPromotionLock");
      return identityResponse();
    }
    if (url.hostname === `ai-ubq-fi-${CANDIDATE.revision_id}.ubiquity-dao.deno.net`) {
      throw new TypeError("candidate revision health must never be requested");
    }
    if (url.hostname === `${APP}.ubiquity-dao.deno.net` || url.hostname === "ai.ubq.fi") {
      assert.equal(method, "GET");
      assert.ok(
        url.href === MANAGED_HEALTH_URL || url.href === CUSTOM_HEALTH_URL,
        `unexpected stable health URL ${url.href}`,
      );
      assert.ok(posts > 0, "stable health must never be requested before the promotion POST");
      assert.equal(lockDepth, 0, "restoration check must run outside withPromotionLock");
      return identityResponse();
    }
    throw new TypeError(`unexpected Deno transport host ${url.hostname}`);
  };

  // Exact active GitHub authority endpoint; the response is the original
  // payload preserved verbatim in retained events.
  const githubFetch: SentinelFetch = (input, init) => {
    const url = new URL(input instanceof Request ? input.url : String(input));
    assert.equal(url.href, "https://api.github.com/repos/ubiquity/ai.ubq.fi/actions/runs/42/attempts/2");
    assert.equal(init?.method, "GET");
    if (options.fault === "terminal_executor") {
      faultHits++;
      return Promise.resolve(
        Response.json({ ...AUTHORITY_RESPONSE, status: "completed", conclusion: "failure" }),
      );
    }
    return Promise.resolve(Response.json(AUTHORITY_RESPONSE));
  };

  const retainEvidence = async (event: Readonly<Record<string, unknown>>): Promise<string> => {
    retained.push(structuredClone(event));
    const reference = `sha256:${await sha256hex(JSON.stringify(event))}`;
    retainedRefs.push(reference);
    return reference;
  };

  const buildDependencies = (): ProviderRecoveryDependencies => {
    const client = new DenoDeployClient({
      token: DENO_TOKEN,
      fetcher: denoFetch,
      now,
      sleep: () => Promise.reject(new Error("Unexpected health polling")),
      createTimeoutSignal: () => new AbortController().signal,
    });
    return {
      state,
      deno: {
        verifyHealthIdentity: client.verifyHealthIdentity.bind(client),
        readProductionRouteOwnership: client.readProductionRouteOwnership.bind(client),
        promoteRevision: async (app, revisionId, guard) => {
          inPromotion = true;
          try {
            await client.promoteRevision(app, revisionId, async () => {
              await guard?.();
              guardRuns++;
            });
          } finally {
            inPromotion = false;
          }
        },
      },
      githubToken: GITHUB_TOKEN,
      githubFetch,
      now,
      withPromotionLock: async <T>(action: () => Promise<T>): Promise<T> => {
        assert.equal(lockDepth, 0, "withPromotionLock must never nest");
        lockDepth++;
        try {
          return await action();
        } finally {
          lockDepth--;
        }
      },
      verifyPreviousControl: (app, record) => {
        assert.equal(app, APP);
        assert.deepEqual(record, PRIOR);
        assert.equal(lockDepth, 0, "previous control verification must run outside withPromotionLock");
        if (options.fault === "missing_control") {
          faultHits++;
          return Promise.resolve(null);
        }
        return Promise.resolve({
          ...PRIOR,
          verified_at: iso(),
          identity_ref: "artifact:fresh-control-identity",
          inference_ref: "artifact:fresh-control-inference",
        });
      },
      verifyRestoration: async (app, record) => {
        assert.equal(app, APP);
        assert.deepEqual(record, PRIOR);
        assert.equal(lockDepth, 0, "restoration verification must run outside withPromotionLock");
        await client.verifyProductionHealthIdentity(
          MANAGED_HEALTH_URL,
          CUSTOM_HEALTH_URL,
          PRIOR.git_sha,
          PRIOR.revision_id,
        );
        if (inferFailures > 0) {
          inferFailures--;
          return null;
        }
        if (options.fault === "completion_route_changed") {
          routeOwner = "unrelated-revision";
          faultHits++;
        }
        return {
          ...PRIOR,
          verified_at: iso(),
          identity_ref: "artifact:restored-identity",
          inference_ref: "artifact:restored-inference",
        };
      },
      retainEvidence,
    };
  };

  return {
    state,
    transport,
    retained,
    retainedRefs,
    posts: () => posts,
    guardRuns: () => guardRuns,
    faultHits: () => faultHits,
    expectedCommitSha: () => expectedCommitShaUsed,
    invoke: async (freshClockMs?: number): Promise<ProviderRecoveryResult> => {
      if (freshClockMs !== undefined) clockMs = freshClockMs;
      const snapshot = await state.refresh();
      const expectedCommitSha = options.fault === "stale_initial" ? "9".repeat(40) : snapshot.commit_sha;
      expectedCommitShaUsed = expectedCommitSha;
      return await recoverProviderTransaction(
        { app: APP, transactionId: TRANSACTION.id, expectedCommitSha, executor: EXECUTOR },
        buildDependencies(),
      );
    },
  };
};

const appStateOf = (document: SentinelProviderStateDocumentV1) => document.applications[0]!;

const transactionOf = (document: SentinelProviderStateDocumentV1) => appStateOf(document).transaction!;

const assertRetainedEvidence = (scenario: Scenario): void => {
  assert.ok(scenario.retained.length >= 1, "expected at least one retained evidence event");
  for (const event of scenario.retained) {
    assert.equal(event.app, APP);
    assert.equal(event.transaction_id, TRANSACTION.id);
    assert.equal(event.fence_generation, TRANSACTION.fence_generation);
    assert.deepEqual(event.executor, EXECUTOR);
    assert.match(String(event.expected_commit_sha), FULL_GIT_SHA);
    const previousControl = event.previous_control as SentinelProviderAttestationV1;
    assert.equal(previousControl.git_sha, PRIOR.git_sha);
    assert.equal(previousControl.revision_id, PRIOR.revision_id);
    const attempt = (event.authority as {
      attempt: {
        run_id: number;
        run_attempt: number;
        status: string;
        conclusion: string | null;
        response: Record<string, unknown>;
      };
    }).attempt;
    assert.equal(attempt.run_id, EXECUTOR.run_id);
    assert.equal(attempt.run_attempt, EXECUTOR.run_attempt);
    assert.equal(attempt.status, "in_progress");
    assert.equal(attempt.conclusion, null);
    assert.equal(attempt.response.id, EXECUTOR.run_id);
    assert.equal(attempt.response.run_attempt, EXECUTOR.run_attempt);
    assert.equal(attempt.response.path, EXECUTOR.workflow_path);
    assert.equal(attempt.response.status, "in_progress");
    assert.equal(attempt.response.conclusion, null);
    assert.deepEqual(
      attempt.response,
      AUTHORITY_RESPONSE,
      "the original GitHub authority response is retained verbatim",
    );
    const serialized = JSON.stringify(event);
    assert.ok(!serialized.includes(DENO_TOKEN), "retained event must not contain the Deno token");
    assert.ok(!serialized.includes(GITHUB_TOKEN), "retained event must not contain the GitHub token");
  }
};

/** Route reads must report exactly the current owner: candidate before each POST, prior after. */
const assertRouteOwnershipWindows = (scenario: Scenario): void => {
  let promoted = false;
  for (const row of scenario.transport) {
    if (row.method === "POST") {
      promoted = true;
      continue;
    }
    if (row.revision === null) continue;
    const expectedOwner: string = promoted ? PRIOR.revision_id : CANDIDATE.revision_id;
    assert.equal(row.owns, row.revision === expectedOwner, `unexpected route ownership at ${row.url}`);
  }
};

const assertStableHealthAfterPromotion = (scenario: Scenario): void => {
  const promoteIndex = scenario.transport.findIndex((row) => row.method === "POST");
  assert.ok(promoteIndex >= 0, "expected at least one promotion POST");
  for (const [index, row] of scenario.transport.entries()) {
    if (row.url === MANAGED_HEALTH_URL || row.url === CUSTOM_HEALTH_URL) {
      assert.ok(
        index > promoteIndex,
        `${row.url} (stable health) must only be requested after the promotion POST`,
      );
    }
  }
};

const assertFinishedRollback = async (
  scenario: Scenario,
  options: Readonly<{ expectedGeneration: number; expectedPosts: number; expectedGuardRuns: number }>,
): Promise<SentinelProviderTransactionV1> => {
  const snapshot = await scenario.state.refresh();
  const document = snapshot.document!;
  const appState = appStateOf(document);
  const transaction = transactionOf(document);
  assert.equal(document.generation, options.expectedGeneration);
  assert.equal(appState.app, APP);
  assert.equal(transaction.phase, "rolled_back");
  assert.equal(transaction.decision, "rollback");
  assert.equal(transaction.reason, "candidate_failed");
  assert.deepEqual(appState.healthy, transaction.restoration);
  assert.equal(transaction.restoration!.git_sha, PRIOR.git_sha);
  assert.equal(transaction.restoration!.revision_id, PRIOR.revision_id);
  assert.equal(transaction.restoration!.configuration_digest, PRIOR.configuration_digest);
  assert.equal(transaction.restoration!.validator_sha, PRIOR.validator_sha);
  assert.equal(transaction.restoration!.corpus_digest, PRIOR.corpus_digest);
  assert.equal(transaction.restoration!.identity_ref, "artifact:restored-identity");
  assert.equal(transaction.restoration!.inference_ref, "artifact:restored-inference");
  assert.match(transaction.restoration!.verified_at, ISO_UTC);
  assert.ok(Date.parse(transaction.restoration!.verified_at) >= Date.parse(transaction.rollback_result!.observed_at));
  assert.equal(transaction.rollback_result!.kind, "acknowledged");
  assert.equal(transaction.rollback_result!.http_status, 204);
  assert.match(transaction.rollback_result!.evidence_ref, SHA256_REFERENCE);
  assert.equal(transaction.route!.revision_id, PRIOR.revision_id);
  assert.match(transaction.route!.evidence_ref, SHA256_REFERENCE);
  assert.ok(Date.parse(transaction.route!.observed_at) >= Date.parse(transaction.rollback_result!.observed_at));
  for (const field of INVARIANT_FIELDS) {
    assert.deepEqual(
      transaction[field],
      (TRANSACTION as unknown as Record<string, unknown>)[field],
      `transaction history field ${field} must be preserved`,
    );
  }
  assert.deepEqual(document.applications[1], { app: ELSEWHERE_APP, healthy: null, transaction: null });
  assert.equal(scenario.posts(), options.expectedPosts);
  assert.equal(scenario.guardRuns(), options.expectedGuardRuns);
  return transaction;
};

Deno.test("provider recovery rolls back an already authorized rollback_pending transaction", async () => {
  const scenario = createScenario();
  const first = await scenario.invoke();

  assert.equal(first.status, "rolled_back");
  assert.equal(first.reason, "rollback_completed");
  assert.match(first.state_commit_sha, FULL_GIT_SHA);

  await assertFinishedRollback(scenario, { expectedGeneration: 5, expectedPosts: 1, expectedGuardRuns: 1 });

  const finalTransaction = transactionOf(
    (await scenario.state.refresh()).document!,
  );
  assert.equal(first.state_commit_sha, (await scenario.state.refresh()).commit_sha);

  // Transport: exact API set only, one prior POST, candidate health never read.
  const postRows = scenario.transport.filter((row) => row.method === "POST");
  assert.equal(postRows.length, 1);
  assert.equal(postRows[0]?.url, PROMOTE_URL(PRIOR.revision_id));
  assert.equal(
    scenario.transport.filter((row) => row.url === PRIOR_HEALTH_URL).length,
    1,
    "prior immutable health is read exactly once before promotion",
  );
  assert.equal(
    scenario.transport.filter((row) => row.url === MANAGED_HEALTH_URL).length,
    1,
    "managed stable health is read exactly once after promotion",
  );
  assert.equal(
    scenario.transport.filter((row) => row.url === CUSTOM_HEALTH_URL).length,
    1,
    "custom stable health is read exactly once after promotion",
  );
  assert.equal(
    scenario.transport.some((row) => row.url.includes(`ai-ubq-fi-${CANDIDATE.revision_id}.ubiquity-dao.deno.net`)),
    false,
    "candidate revision health must never be requested",
  );
  assertRouteOwnershipWindows(scenario);
  assertStableHealthAfterPromotion(scenario);

  // Durable route/restoration evidence comes from the retained events.
  assert.match(finalTransaction.route!.evidence_ref, SHA256_REFERENCE);
  assert.match(finalTransaction.rollback_result!.evidence_ref, SHA256_REFERENCE);

  const publishedPhases = scenario.state.publishedDocuments().map((document) => transactionOf(document).phase);
  assert.deepEqual(publishedPhases, ["rollback_pending", "rollback_pending_verification", "rolled_back"]);

  assertRetainedEvidence(scenario);
});

Deno.test("inference failure yields restoration_unverified and resume completes with no new POST", async () => {
  const scenario = createScenario({ inferFailOnce: true });
  const first = await scenario.invoke();

  assert.equal(first.status, "pending");
  assert.equal(first.reason, "restoration_unverified");
  assert.equal(scenario.posts(), 1);

  const afterFirst = await scenario.state.refresh();
  const afterFirstTransaction = transactionOf(afterFirst.document!);
  assert.equal(afterFirstTransaction.phase, "rollback_pending_verification");
  assert.equal(afterFirstTransaction.rollback_result!.kind, "acknowledged");
  assert.equal(afterFirstTransaction.rollback_result!.http_status, 204);
  assert.equal(afterFirstTransaction.restoration, null);
  assert.match(afterFirstTransaction.rollback_result!.evidence_ref, SHA256_REFERENCE);
  const acknowledgedResult = afterFirstTransaction.rollback_result!;

  // Fresh invocation clock, passing restoration: complete without another POST.
  const second = await scenario.invoke(Date.parse("2026-09-05T00:10:00.000Z"));
  assert.equal(second.status, "rolled_back");
  assert.equal(second.reason, "rollback_completed");
  const finalTransaction = await assertFinishedRollback(scenario, {
    expectedGeneration: 5,
    expectedPosts: 1,
    expectedGuardRuns: 1,
  });

  // The original acknowledged promotion result is preserved byte for byte.
  assert.deepEqual(finalTransaction.rollback_result, acknowledgedResult);
  assert.equal(
    scenario.transport.filter((row) => row.url === PRIOR_HEALTH_URL).length,
    1,
    "the resume must not re-run the prior health check",
  );
  assert.equal(
    scenario.transport.filter((row) => row.url === PROMOTE_URL(PRIOR.revision_id)).length,
    1,
    "the resume must not POST a second promotion",
  );
  assertRouteOwnershipWindows(scenario);
  assertStableHealthAfterPromotion(scenario);
  assertRetainedEvidence(scenario);
});

Deno.test("a lost first POST persists an ambiguous result and resume uses one new guarded POST", async () => {
  const scenario = createScenario({ postThrowsOnce: true });
  const first = await scenario.invoke();

  assert.equal(first.status, "pending");
  assert.equal(first.reason, "promotion_ambiguous");
  assert.equal(scenario.posts(), 1);
  assert.equal(scenario.guardRuns(), 1);

  const afterFirst = await scenario.state.refresh();
  const afterFirstTransaction = transactionOf(afterFirst.document!);
  assert.equal(afterFirstTransaction.phase, "rollback_pending");
  assert.equal(afterFirstTransaction.rollback_intent_at, TRANSACTION.rollback_intent_at);
  assert.equal(afterFirstTransaction.rollback_result!.kind, "ambiguous");
  assert.equal(afterFirstTransaction.rollback_result!.http_status, null);
  assert.match(afterFirstTransaction.rollback_result!.evidence_ref, SHA256_REFERENCE);
  assert.equal(afterFirstTransaction.route!.revision_id, CANDIDATE.revision_id);

  const second = await scenario.invoke();
  assert.equal(second.status, "rolled_back");
  assert.equal(second.reason, "rollback_completed");
  const finalTransaction = await assertFinishedRollback(scenario, {
    expectedGeneration: 7,
    expectedPosts: 2,
    expectedGuardRuns: 2,
  });

  // Exactly one new guarded POST obtained the real 204; the acknowledgement
  // comes from that POST, never from a route read.
  assert.equal(
    scenario.transport.filter((row) => row.method === "POST" && row.url === PROMOTE_URL(PRIOR.revision_id)).length,
    2,
  );
  const acknowledgedEventIndex = scenario.retained.findIndex(
    (event) => event.event === "provider_recovery_rollback_acknowledged",
  );
  assert.ok(acknowledgedEventIndex >= 0, "expected a retained acknowledged event");
  assert.equal(scenario.retained[acknowledgedEventIndex]!.http_status, 204);
  assert.equal(finalTransaction.rollback_result!.evidence_ref, scenario.retainedRefs[acknowledgedEventIndex]);
  assert.equal(
    scenario.retained.filter((event) => event.event === "provider_recovery_rollback_ambiguous").length,
    1,
    "the ambiguous outcome is retained exactly once",
  );
  const ambiguousEventIndex = scenario.retained.findIndex(
    (event) => event.event === "provider_recovery_rollback_ambiguous",
  );
  assert.equal(scenario.retained[ambiguousEventIndex]!.http_status, null);
  assertRouteOwnershipWindows(scenario);
  assertStableHealthAfterPromotion(scenario);
  assertRetainedEvidence(scenario);
});

Deno.test("an ACK CAS that publishes then throws is distinguished from a prepublication failure", async () => {
  const scenario = createScenario({ ackCasPublishThenThrow: true });
  const first = await scenario.invoke();

  assert.equal(first.status, "pending");
  assert.equal(first.reason, "state_publication_unresolved");
  assert.equal(scenario.posts(), 1);

  // The acknowledgement document WAS published durably even though the caller
  // observed an unresolved publication: exact publication, not a lost write.
  const afterFirst = await scenario.state.refresh();
  const afterFirstTransaction = transactionOf(afterFirst.document!);
  assert.equal(afterFirstTransaction.phase, "rollback_pending_verification");
  assert.equal(afterFirstTransaction.rollback_result!.kind, "acknowledged");
  assert.equal(afterFirstTransaction.rollback_result!.http_status, 204);
  assert.match(afterFirstTransaction.rollback_result!.evidence_ref, SHA256_REFERENCE);
  const publishedPhases = scenario.state.publishedDocuments().map((document) => transactionOf(document).phase);
  assert.deepEqual(publishedPhases, ["rollback_pending", "rollback_pending_verification"]);
  const acknowledgedResult = afterFirstTransaction.rollback_result!;

  const second = await scenario.invoke();
  assert.equal(second.status, "rolled_back");
  assert.equal(second.reason, "rollback_completed");
  const finalTransaction = await assertFinishedRollback(scenario, {
    expectedGeneration: 5,
    expectedPosts: 1,
    expectedGuardRuns: 1,
  });

  // No new POST to obtain another acknowledgement; the original one survives.
  assert.deepEqual(finalTransaction.rollback_result, acknowledgedResult);
  assert.equal(
    scenario.transport.filter((row) => row.url === PROMOTE_URL(PRIOR.revision_id)).length,
    1,
    "the resume must not POST a second promotion",
  );
  assertRouteOwnershipWindows(scenario);
  assertStableHealthAfterPromotion(scenario);
  assertRetainedEvidence(scenario);
});

/**
 * Fail-closed table: every injected fault must keep the operation pending with
 * the exact contracted reason and POST count, and leave the durable
 * transaction in the contracted phase with the sibling app, history
 * invariants, healthy predecessor and null restoration untouched. The
 * final-route row must preserve its acknowledged 204 and never mark the
 * transaction rolled_back. Successful-route assertions do not apply here.
 */
const NEGATIVE_RECOVERY_CASES: readonly {
  readonly fault: RecoveryFault;
  readonly reason: ProviderRecoveryResult["reason"];
  readonly posts: number;
  readonly durablePhase: SentinelProviderTransactionV1["phase"];
}[] = [
  { fault: "stale_initial", reason: "state_conflict", posts: 0, durablePhase: "rollback_pending" },
  { fault: "state_drift", reason: "state_conflict", posts: 0, durablePhase: "rollback_pending" },
  { fault: "later_route", reason: "ownership_unresolved", posts: 0, durablePhase: "rollback_pending" },
  { fault: "expired_control", reason: "control_unverified", posts: 0, durablePhase: "rollback_pending" },
  { fault: "regressed_clock", reason: "promotion_guard_blocked", posts: 0, durablePhase: "rollback_pending" },
  { fault: "terminal_executor", reason: "promotion_guard_blocked", posts: 0, durablePhase: "rollback_pending" },
  { fault: "missing_control", reason: "control_unverified", posts: 0, durablePhase: "rollback_pending" },
  { fault: "route_cas_rejected", reason: "state_publication_unresolved", posts: 0, durablePhase: "rollback_pending" },
  {
    fault: "completion_route_changed",
    reason: "ownership_unresolved",
    posts: 1,
    durablePhase: "rollback_pending_verification",
  },
];

Deno.test("recovery fails closed with the exact reason for every injected fault", async () => {
  for (const row of NEGATIVE_RECOVERY_CASES) {
    const scenario = createScenario({
      fault: row.fault,
      routeCasRejected: row.fault === "route_cas_rejected",
    });
    const result = await scenario.invoke();

    assert.equal(result.status, "pending", `${row.fault}: must stay pending`);
    assert.equal(result.reason, row.reason, `${row.fault}: exact reason`);
    assert.equal(scenario.posts(), row.posts, `${row.fault}: promotion POST count`);

    const snapshot = await scenario.state.refresh();
    const document = snapshot.document!;
    const transaction = transactionOf(document);
    assert.equal(transaction.phase, row.durablePhase, `${row.fault}: durable phase`);
    assert.equal(transaction.restoration, null, `${row.fault}: restoration must stay null`);
    assert.deepEqual(appStateOf(document).healthy, PRIOR, `${row.fault}: healthy must stay the prior attestation`);
    assert.deepEqual(
      document.applications[1],
      { app: ELSEWHERE_APP, healthy: null, transaction: null },
      `${row.fault}: sibling app must stay unchanged`,
    );
    for (const field of INVARIANT_FIELDS) {
      assert.deepEqual(
        transaction[field],
        (TRANSACTION as unknown as Record<string, unknown>)[field],
        `${row.fault}: history field ${field} must be preserved`,
      );
    }

    if (row.fault === "stale_initial") {
      assert.equal(
        scenario.expectedCommitSha(),
        "9".repeat(40),
        "stale_initial: invoke must use the stale expected commit",
      );
      assert.equal(scenario.state.publishedDocuments().length, 0, "stale_initial: rejection must not publish");
      assert.equal(snapshot.commit_sha, "1".repeat(40), "stale_initial: the commit must be unchanged");
    } else if (row.fault === "route_cas_rejected") {
      assert.equal(
        scenario.expectedCommitSha(),
        "1".repeat(40),
        "route_cas_rejected: invoke must use the fresh expected commit",
      );
      assert.ok(scenario.state.casAttempts() >= 1, "route_cas_rejected: the first CAS must have been attempted");
      assert.equal(scenario.state.publishedDocuments().length, 0, "route_cas_rejected: rejection must not publish");
      assert.equal(snapshot.commit_sha, "1".repeat(40), "route_cas_rejected: the commit must be unchanged");
    } else {
      assert.ok(scenario.faultHits() >= 1, `${row.fault}: the injected fault must have been reached`);
    }

    if (row.fault === "state_drift") {
      assert.equal(document.generation, 4, `${row.fault}: drift must persist as generation+1`);
      assert.equal(scenario.state.publishedDocuments().length, 2, `${row.fault}: drift must publish after the CAS`);
    }
    if (row.fault === "completion_route_changed") {
      assert.equal(transaction.rollback_result!.kind, "acknowledged");
      assert.equal(transaction.rollback_result!.http_status, 204);
      assert.match(transaction.rollback_result!.evidence_ref, SHA256_REFERENCE);
      assert.notEqual(transaction.phase, "rolled_back", "the final-route row must never mark rolled_back");
    }
  }
});
