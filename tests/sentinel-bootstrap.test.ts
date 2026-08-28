import assert from "node:assert/strict";
import {
  createRollbackActivation,
  initialSentinelBootstrapActivation,
  selectStableRollbackSha,
  type SentinelBootstrapActivationSnapshot,
  type SentinelBootstrapRollbackIntentSnapshot,
  type SentinelBootstrapStateStore,
} from "../scripts/sentinel/bootstrap/activation.ts";
import {
  reconcileSentinelBootstrap,
  type SentinelBootstrapRecoveryDispatch,
} from "../scripts/sentinel/bootstrap/controller.ts";
import { evaluateSentinelBootstrapHealth } from "../scripts/sentinel/bootstrap/health.ts";
import { parseSentinelBootstrapStateDocument } from "../scripts/sentinel/bootstrap/github-store.ts";
import {
  parseBootstrapReleaseRecord,
  type SentinelBootstrapActivationPointerV1,
  type SentinelBootstrapRollbackIntentV1,
  type SentinelFailureConstraintV1,
} from "../scripts/sentinel/bootstrap/contracts.ts";
import {
  assertImplementationSelection,
  assertNoBootstrapMutation,
  parseBootstrapEnvironment,
  SENTINEL_BOOTSTRAP_POLICY,
} from "../scripts/sentinel/bootstrap/policy.ts";
import { synchronizeObservedRelease } from "../scripts/sentinel/bootstrap/main.ts";

const readPermission = await Deno.permissions.query({ name: "read" });
const sourceInspectionUnavailable = readPermission.state !== "granted";

const stableSha = "1".repeat(40);
const candidateSha = "2".repeat(40);
const fingerprint = "a".repeat(64);
const release = () => ({
  schema_version: 1,
  stable_sha: stableSha,
  candidate_sha: candidateSha,
  acceptance_evidence: ["ci:stable"],
  activated_at: "2026-08-28T18:00:00.000Z",
  rollback_reason: null,
  generation: 1,
});

const signal = (
  overrides: Record<string, unknown> = {},
) => ({
  schema_version: 1,
  generation: 1,
  failure_class: "workflow_failure",
  failure_fingerprint: fingerprint,
  observed_at: "2026-08-28T18:10:00.000Z",
  evidence_refs: ["run:1"],
  ...overrides,
});

class FakeBootstrapStateStore implements SentinelBootstrapStateStore {
  pointer: ReturnType<typeof initialSentinelBootstrapActivation> | null = null;
  versionstamp: string | null = null;
  rollbackIntent: SentinelBootstrapRollbackIntentV1 | null = null;
  rollbackIntentVersionstamp: string | null = null;
  readonly constraints = new Map<string, unknown>();
  readonly activationWrites: Array<{ expected: string | null; activeSha: string; generation: number }> = [];

  readActivation(): Promise<SentinelBootstrapActivationSnapshot> {
    return Promise.resolve({ pointer: this.pointer, versionstamp: this.versionstamp });
  }

  compareAndSetActivation(
    expectedVersionstamp: string | null,
    next: ReturnType<typeof initialSentinelBootstrapActivation>,
  ): Promise<boolean> {
    if (expectedVersionstamp !== this.versionstamp) return Promise.resolve(false);
    this.activationWrites.push({
      expected: expectedVersionstamp,
      activeSha: next.active_sha,
      generation: next.generation,
    });
    this.pointer = next;
    this.versionstamp = String((Number(this.versionstamp) || 0) + 1);
    return Promise.resolve(true);
  }

  readRollbackIntent(): Promise<SentinelBootstrapRollbackIntentSnapshot> {
    return Promise.resolve({ intent: this.rollbackIntent, versionstamp: this.rollbackIntentVersionstamp });
  }

  commitRollback(
    expectedActivationVersionstamp: string | null,
    next: ReturnType<typeof initialSentinelBootstrapActivation>,
    intent: SentinelBootstrapRollbackIntentV1,
  ): Promise<boolean> {
    if (expectedActivationVersionstamp !== this.versionstamp || this.rollbackIntent !== null) {
      return Promise.resolve(false);
    }
    this.activationWrites.push({
      expected: expectedActivationVersionstamp,
      activeSha: next.active_sha,
      generation: next.generation,
    });
    this.pointer = next;
    this.versionstamp = String((Number(this.versionstamp) || 0) + 1);
    this.rollbackIntent = intent;
    this.rollbackIntentVersionstamp = "1";
    return Promise.resolve(true);
  }

  clearRollbackIntent(expectedVersionstamp: string): Promise<boolean> {
    if (expectedVersionstamp !== this.rollbackIntentVersionstamp || this.rollbackIntent === null) {
      return Promise.resolve(false);
    }
    this.rollbackIntent = null;
    this.rollbackIntentVersionstamp = null;
    return Promise.resolve(true);
  }

  putConstraintIfAbsent(constraint: SentinelFailureConstraintV1): Promise<boolean> {
    const key = constraint.failure_fingerprint;
    if (this.constraints.has(key)) return Promise.resolve(false);
    this.constraints.set(key, constraint);
    return Promise.resolve(true);
  }
}

const validBootstrapEnvironment = (): Record<string, string> => ({
  GITHUB_REPOSITORY: SENTINEL_BOOTSTRAP_POLICY.repository,
  GITHUB_REF: SENTINEL_BOOTSTRAP_POLICY.developmentRef,
  GITHUB_SHA: "3".repeat(40),
  GITHUB_RUN_ID: "42",
  GITHUB_WORKFLOW_REF:
    `${SENTINEL_BOOTSTRAP_POLICY.repository}/${SENTINEL_BOOTSTRAP_POLICY.bootstrapWorkflow}@${SENTINEL_BOOTSTRAP_POLICY.developmentRef}`,
});

Deno.test("bootstrap health ignores stale-generation observations", () => {
  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: 2,
    signals: [signal({ generation: 1 }), signal({ generation: 1 }), signal({ generation: 1 })],
  });
  assert.equal(health.rollback, false);
  assert.equal(health.status, "healthy");
});

Deno.test("two identical workflow failures do not roll back", () => {
  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: 1,
    signals: [signal({ observation_id: "run-1" }), signal({ observation_id: "run-2", evidence_refs: ["run:2"] })],
  });
  assert.equal(health.rollback, false);
  assert.equal(health.status, "transient");
  assert.equal(health.reason, "authoritative_failure_threshold_not_reached");
});

Deno.test("duplicate workflow deliveries without an observation ID do not reach the rollback threshold", () => {
  const duplicate = signal({ evidence_refs: ["run:1"] });
  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: 1,
    signals: [duplicate, duplicate, signal({ evidence_refs: ["run:2"] })],
  });
  assert.equal(health.rollback, false);
  assert.equal(health.status, "transient");
  assert.equal(health.reason, "authoritative_failure_threshold_not_reached");
});

Deno.test("three identical authoritative workflow failures mark the active generation unhealthy", () => {
  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: 1,
    signals: [
      signal({ observation_id: "run-1" }),
      signal({ observation_id: "run-2", evidence_refs: ["run:2"] }),
      signal({ observation_id: "run-3", evidence_refs: ["run:3"] }),
    ],
  });
  assert.equal(health.rollback, true);
  assert.equal(health.status, "unhealthy");
  assert.equal(health.failure_class, "workflow_failure");
  assert.equal(health.failure_fingerprint, fingerprint);
  assert.deepEqual(health.evidence_refs, ["run:1", "run:2", "run:3"]);
});

Deno.test("provider and Luna transient failures never become rollback decisions", () => {
  const health = evaluateSentinelBootstrapHealth({
    activeGeneration: 1,
    signals: [
      signal({ failure_class: "provider_timeout", observation_id: "timeout-1" }),
      signal({ failure_class: "provider_5xx", observation_id: "http-1" }),
      signal({ failure_class: "luna_capacity_failure", observation_id: "luna-1" }),
    ],
  });
  assert.equal(health.rollback, false);
  assert.equal(health.status, "transient");
});

Deno.test("activation rollback selects the stable release SHA and fences the unhealthy generation", () => {
  const active = initialSentinelBootstrapActivation(release(), "2026-08-28T18:00:00.000Z");
  assert.equal(selectStableRollbackSha(active, release()), stableSha);
  const rollback = createRollbackActivation(active, release(), "2026-08-28T18:20:00.000Z");
  assert.equal(rollback.active_sha, stableSha);
  assert.equal(rollback.generation, 2);
  assert.deepEqual(rollback.fenced_generations, [1]);
});

Deno.test("bootstrap release registry requires acceptance evidence", () => {
  assert.doesNotThrow(() => parseBootstrapReleaseRecord(release()));
  assert.throws(
    () => parseBootstrapReleaseRecord({ ...release(), acceptance_evidence: [] }),
    /no acceptance evidence/,
  );
});

Deno.test("bootstrap Git state document is bounded and starts without invented release identity", () => {
  const document = parseSentinelBootstrapStateDocument({
    schema_version: 1,
    release: null,
    signals: [],
    activation: null,
    rollback_intent: null,
    constraints: [],
  });
  assert.equal(document.release, null);
  assert.throws(
    () => parseSentinelBootstrapStateDocument({ ...document, signals: Array(65).fill(signal()) }),
    /invalid/,
  );
});

Deno.test("a newly deployed release supersedes and fences an unaccepted candidate", async () => {
  const currentRelease = release();
  const activation = initialSentinelBootstrapActivation(currentRelease, currentRelease.activated_at);
  let writtenRelease: ReturnType<typeof parseBootstrapReleaseRecord> | null = null;
  let writtenActivation: SentinelBootstrapActivationPointerV1 | null = null;
  const state = {
    readDocument: () => ({
      schema_version: 1 as const,
      release: currentRelease,
      signals: [],
      activation,
      rollback_intent: null,
      constraints: [],
    }),
    replaceRelease: (
      nextRelease: ReturnType<typeof parseBootstrapReleaseRecord>,
      nextActivation?: SentinelBootstrapActivationPointerV1,
    ) => {
      writtenRelease = nextRelease;
      writtenActivation = nextActivation ?? null;
      return Promise.resolve();
    },
  };
  const observedSha = "4".repeat(40);
  const next = await synchronizeObservedRelease(
    state as never,
    { sha: observedSha, revision: "revision-next" },
    "2026-08-28T18:30:00.000Z",
  );
  assert.equal(next.stable_sha, stableSha);
  assert.equal(next.candidate_sha, observedSha);
  assert.equal(next.generation, 2);
  assert.deepEqual(next.acceptance_evidence, [
    "health:revision-next",
    `bootstrap:superseded:${candidateSha}`,
  ]);
  assert.equal(writtenRelease, next);
  assert.ok(writtenActivation !== null);
  const persistedActivation = writtenActivation as SentinelBootstrapActivationPointerV1;
  assert.equal(persistedActivation.active_sha, observedSha);
  assert.equal(persistedActivation.generation, 2);
  assert.deepEqual(persistedActivation.fenced_generations, [1]);
  assert.equal(persistedActivation.reason, "managed_candidate_superseded");
});

Deno.test("a pending rollback intent blocks candidate supersession", async () => {
  const currentRelease = release();
  const activation = initialSentinelBootstrapActivation(currentRelease, currentRelease.activated_at);
  let writes = 0;
  const state = {
    readDocument: () => ({
      schema_version: 1 as const,
      release: currentRelease,
      signals: [],
      activation,
      rollback_intent: {} as SentinelBootstrapRollbackIntentV1,
      constraints: [],
    }),
    replaceRelease: () => {
      writes += 1;
      return Promise.resolve();
    },
  };
  await assert.rejects(
    () =>
      synchronizeObservedRelease(
        state as never,
        { sha: "4".repeat(40), revision: "revision-next" },
        "2026-08-28T18:30:00.000Z",
      ),
    /rollback effects remain pending/,
  );
  assert.equal(writes, 0);
});

Deno.test("controller performs one fenced rollback and deduplicates its constraint and recovery dispatch", async () => {
  const store = new FakeBootstrapStateStore();
  store.pointer = initialSentinelBootstrapActivation(release(), "2026-08-28T18:00:00.000Z");
  store.versionstamp = "1";
  const dispatches: SentinelBootstrapRecoveryDispatch[] = [];
  let published = 0;
  const dependencies = {
    store,
    now: () => "2026-08-28T18:20:00.000Z",
    publishConstraint: () => {
      published += 1;
      return Promise.resolve();
    },
    dispatchRecovery: (dispatch: SentinelBootstrapRecoveryDispatch) => {
      dispatches.push(dispatch);
      return Promise.resolve();
    },
  };
  const input = {
    release: release(),
    signals: [
      signal({ observation_id: "run-1" }),
      signal({ observation_id: "run-2", evidence_refs: ["run:2"] }),
      signal({ observation_id: "run-3", evidence_refs: ["run:3"] }),
    ],
  };
  const first = await reconcileSentinelBootstrap(input, dependencies);
  assert.equal(first.action, "rolled_back");
  assert.equal(first.active_sha, stableSha);
  assert.equal(first.generation, 2);
  assert.equal(first.fenced_generation, 1);
  assert.equal(first.constraint_published, true);
  assert.equal(first.recovery_dispatched, true);
  assert.equal(store.constraints.size, 1);
  assert.equal(published, 1);
  assert.equal(dispatches.length, 1);
  assert.equal(dispatches[0]!.previous_sha, candidateSha);
  assert.equal(dispatches[0]!.sha, stableSha);

  const second = await reconcileSentinelBootstrap(input, dependencies);
  assert.equal(second.action, "none");
  assert.equal(second.active_sha, stableSha);
  assert.equal(store.constraints.size, 1);
  assert.equal(published, 1);
  assert.equal(dispatches.length, 1);
});

Deno.test("rollback side effects resume from a durable intent after callback failure", async () => {
  const store = new FakeBootstrapStateStore();
  store.pointer = initialSentinelBootstrapActivation(release(), "2026-08-28T18:00:00.000Z");
  store.versionstamp = "1";
  const input = {
    release: release(),
    signals: [
      signal({ observation_id: "run-1" }),
      signal({ observation_id: "run-2" }),
      signal({ observation_id: "run-3" }),
    ],
  };
  await assert.rejects(
    () =>
      reconcileSentinelBootstrap(input, {
        store,
        now: () => "2026-08-28T18:20:00.000Z",
        publishConstraint: () => Promise.reject(new Error("injected publication failure")),
      }),
    /injected publication failure/,
  );
  assert.equal(store.pointer?.active_sha, stableSha);
  assert.notEqual(store.rollbackIntent, null);

  let published = 0;
  let dispatched = 0;
  const resumed = await reconcileSentinelBootstrap(input, {
    store,
    publishConstraint: () => {
      published += 1;
      return Promise.resolve();
    },
    dispatchRecovery: () => {
      dispatched += 1;
      return Promise.resolve();
    },
  });
  assert.equal(resumed.reason, "pending_rollback_effects_completed");
  assert.equal(published, 1);
  assert.equal(dispatched, 1);
  assert.equal(store.rollbackIntent, null);
});

Deno.test("controller validates an activation pointer returned by a state store", async () => {
  const store = new FakeBootstrapStateStore();
  store.pointer = { generation: 1 } as never;
  store.versionstamp = "1";
  await assert.rejects(
    () => reconcileSentinelBootstrap({ release: release(), signals: [] }, { store }),
    /activation pointer is invalid/,
  );
});

Deno.test("transient health leaves activation and side effects unchanged", async () => {
  const store = new FakeBootstrapStateStore();
  store.pointer = initialSentinelBootstrapActivation(release(), "2026-08-28T18:00:00.000Z");
  store.versionstamp = "1";
  const result = await reconcileSentinelBootstrap({
    release: release(),
    signals: [signal({ failure_class: "luna_capacity_failure", observation_id: "luna-1" })],
  }, { store });
  assert.equal(result.action, "none");
  assert.equal(store.pointer?.active_sha, candidateSha);
  assert.equal(store.activationWrites.length, 0);
  assert.equal(store.constraints.size, 0);
});

Deno.test("an already-stable generation cannot roll back to an unproven target", async () => {
  const stableRelease = { ...release(), candidate_sha: null };
  const store = new FakeBootstrapStateStore();
  store.pointer = initialSentinelBootstrapActivation(stableRelease, "2026-08-28T18:00:00.000Z");
  store.versionstamp = "1";
  const result = await reconcileSentinelBootstrap({
    release: stableRelease,
    signals: [
      signal({ observation_id: "run-1", failure_class: "canary_acceptance_failure" }),
    ],
  }, { store });
  assert.equal(result.action, "blocked");
  assert.equal(result.active_sha, stableSha);
  assert.equal(store.activationWrites.length, 0);
});

Deno.test("bootstrap policy rejects model substitution and bootstrap-path mutation", () => {
  assert.doesNotThrow(() => assertImplementationSelection("gpt-5.6-luna", "max"));
  assert.throws(() => assertImplementationSelection("other-model", "max"), /owner-controlled/);
  assert.throws(() => assertNoBootstrapMutation(["scripts/sentinel/bootstrap/controller.ts"]), /cannot modify/);
  assert.throws(() => assertNoBootstrapMutation(["scripts/sentinel/bootstrap"]), /cannot modify/);
  assert.throws(() => assertNoBootstrapMutation(["docs/sentinel-bootstrap-state.json"]), /cannot modify/);
  assert.doesNotThrow(() => assertNoBootstrapMutation(["src/ordinary-file.ts"]));
});

Deno.test("bootstrap environment requires the protected development identity", () => {
  const parsed = parseBootstrapEnvironment({ get: (name) => validBootstrapEnvironment()[name] });
  assert.equal(parsed.repository, SENTINEL_BOOTSTRAP_POLICY.repository);
  assert.equal(parsed.ref, SENTINEL_BOOTSTRAP_POLICY.developmentRef);
  assert.equal(parsed.runId, 42);
});

Deno.test("bootstrap environment rejects a non-development ref", () => {
  const values = validBootstrapEnvironment();
  values.GITHUB_REF = "refs/heads/feature";
  assert.throws(() => parseBootstrapEnvironment({ get: (name) => values[name] }), /development/);
});

Deno.test("bootstrap environment rejects a workflow loaded from another ref", () => {
  const values = validBootstrapEnvironment();
  values.GITHUB_WORKFLOW_REF =
    `${SENTINEL_BOOTSTRAP_POLICY.repository}/${SENTINEL_BOOTSTRAP_POLICY.bootstrapWorkflow}@refs/heads/feature`;
  assert.throws(() => parseBootstrapEnvironment({ get: (name) => values[name] }), /workflow ref/);
});

Deno.test({
  name: "bootstrap source and workflow expose no mutation or model-substitution path",
  ignore: sourceInspectionUnavailable,
  async fn() {
    const modulePaths = [
      "scripts/sentinel/bootstrap/activation.ts",
      "scripts/sentinel/bootstrap/contracts.ts",
      "scripts/sentinel/bootstrap/controller.ts",
      "scripts/sentinel/bootstrap/health.ts",
      "scripts/sentinel/bootstrap/github-store.ts",
      "scripts/sentinel/bootstrap/main.ts",
      "scripts/sentinel/bootstrap/policy.ts",
    ];
    for (const path of modulePaths) {
      const source = await Deno.readTextFile(path);
      assert.doesNotMatch(source, /Deno\.Command|git\s+(?:push|merge)|force-with-lease|deno\s+deploy/u);
      assert.doesNotMatch(source, /contents:\s*write|pull-requests:\s*write/u);
    }
    const workflow = await Deno.readTextFile(".github/workflows/provider-sentinel-bootstrap.yml");
    const evolvingWorkflow = await Deno.readTextFile(".github/workflows/provider-sentinel.yml");
    const entryPoint = await Deno.readTextFile("scripts/sentinel/bootstrap/main.ts");
    assert.match(workflow, /actions:\s+write/u);
    assert.match(workflow, /^\s+contents:\s+write$/mu);
    assert.match(workflow, /refs\/heads\/sentinel\/bootstrap-state|bootstrap-state/u);
    assert.match(entryPoint, /provider-sentinel\.yml\/dispatches/u);
    assert.match(evolvingWorkflow, /Honor protected bootstrap activation/u);
    assert.match(evolvingWorkflow, /git checkout --detach "\$active_sha"/u);
    assert.doesNotMatch(workflow, /pull-requests:\s+write|deno\s+deploy/u);
    const actions = workflow.split("\n").filter((line) => /\buses:\s/u.test(line));
    assert.ok(actions.length > 0);
    for (const action of actions) {
      assert.match(action, /@[0-9a-f]{40}(?:\s|$)/u);
    }
  },
});
