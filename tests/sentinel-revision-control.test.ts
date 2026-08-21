import assert from "node:assert/strict";
import type {
  DenoRevision,
  HealthIdentity,
  ProductionHealthAttestation,
  RollbackTarget,
} from "../scripts/sentinel/deploy.ts";
import {
  executeRevisionControl,
  parseRevisionControlEnvironment,
  type RevisionControlDenoClient,
  type RevisionControlInput,
  RevisionControlOutcomeError,
} from "../scripts/sentinel/revision-control.ts";

const DEVELOPMENT_SHA = "1".repeat(40);
const CURRENT_SHA = "2".repeat(40);
const TARGET_SHA = "3".repeat(40);
const CURRENT_REVISION = "current-revision";
const TARGET_REVISION = "target-revision";

const validEnvironment = (): Record<string, string> => ({
  SENTINEL_CORRELATION_ID: "sentinel:12345678",
  SENTINEL_TARGET_APP: "ai-ubq-fi",
  SENTINEL_TARGET_GIT_SHA: TARGET_SHA,
  SENTINEL_TARGET_REVISION: TARGET_REVISION,
  SENTINEL_EXPECTED_CURRENT_GIT_SHA: CURRENT_SHA,
  SENTINEL_EXPECTED_CURRENT_REVISION: CURRENT_REVISION,
  SENTINEL_EXPECTED_DEVELOPMENT_GIT_SHA: DEVELOPMENT_SHA,
  GITHUB_REPOSITORY: "ubiquity/ai.ubq.fi",
  GITHUB_RUN_ID: "42",
  GITHUB_SHA: DEVELOPMENT_SHA,
  GITHUB_REF: "refs/heads/development",
});

const validInput = (): RevisionControlInput => parseRevisionControlEnvironment(validEnvironment());

const githubRefFetch = (
  sha = DEVELOPMENT_SHA,
  observe?: (request: Request) => void,
): typeof fetch =>
  ((input, init) => {
    const request = new Request(input, init);
    observe?.(request);
    return Promise.resolve(
      Response.json({
        ref: "refs/heads/development",
        object: { type: "commit", sha },
      }),
    );
  }) as typeof fetch;

const revision = (id: string, status: DenoRevision["status"] = "routed"): DenoRevision => ({
  id,
  status,
  sourceStatus: status === "routed" ? "succeeded" : status,
  raw: { id, status },
});

const snapshot = (
  gitSha = CURRENT_SHA,
  revisionId = CURRENT_REVISION,
): RollbackTarget => ({
  gitSha,
  revisionId,
  healthUrls: ["https://ai-ubq-fi.ubiquity-dao.deno.net/health", "https://ai.ubq.fi/health"],
  snapshottedAt: "2026-08-21T10:00:00.000Z",
});

class FakeDenoClient implements RevisionControlDenoClient {
  readonly calls: string[] = [];
  snapshots: RollbackTarget[] = [snapshot(), snapshot()];
  targetStatus: DenoRevision["status"] = "routed";
  promoted = false;
  readonly promotedRevisions: string[] = [];
  productionHealthFailures = 0;
  throwAfterPromotingRevision: string | null = null;

  snapshotHealthyProduction(app: string, healthUrls: readonly string[]): Promise<RollbackTarget> {
    this.calls.push(`snapshot:${app}:${healthUrls.length}`);
    const value = this.snapshots.shift();
    if (!value) throw new Error("Missing fake snapshot");
    return Promise.resolve(value);
  }

  assertRevisionBelongsToApp(app: string, revisionId: string): Promise<DenoRevision> {
    this.calls.push(`belongs:${app}:${revisionId}`);
    return Promise.resolve(revision(revisionId, this.targetStatus));
  }

  getRevision(revisionId: string): Promise<DenoRevision> {
    this.calls.push(`get:${revisionId}`);
    return Promise.resolve(revision(revisionId, this.targetStatus));
  }

  verifyHealthIdentity(
    urls: readonly string[],
    gitSha: string,
    revisionId: string,
  ): Promise<readonly HealthIdentity[]> {
    this.calls.push(`health:${urls.join(",")}:${gitSha}:${revisionId}`);
    return Promise.resolve(urls.map((url) => ({ url, gitSha, revisionId })));
  }

  verifyProductionHealthIdentity(
    managedUrl: string,
    customUrl: string,
    gitSha: string,
    revisionId: string,
  ): Promise<ProductionHealthAttestation> {
    this.calls.push(`production-health:${gitSha}:${revisionId}`);
    if (this.productionHealthFailures > 0) {
      this.productionHealthFailures -= 1;
      throw new Error("Injected stable verification failure");
    }
    return Promise.resolve({
      managed: { url: managedUrl, gitSha, revisionId },
      custom: {
        kind: "cloudflare_challenge",
        url: customUrl,
        status: 403,
        ray: "test-ray",
      },
    });
  }

  promoteRevision(app: string, revisionId: string): Promise<void> {
    this.calls.push(`promote:${app}:${revisionId}`);
    this.promoted = true;
    this.promotedRevisions.push(revisionId);
    if (revisionId === this.throwAfterPromotingRevision) {
      throw new Error("Injected ambiguous promotion failure");
    }
    return Promise.resolve();
  }
}

Deno.test("revision-control inputs reject untrusted app, identifiers, and execution commit", () => {
  for (
    const [name, value] of [
      ["SENTINEL_TARGET_APP", "other-app"],
      ["SENTINEL_TARGET_GIT_SHA", "not-a-sha"],
      ["SENTINEL_TARGET_REVISION", "../revision"],
      ["SENTINEL_CORRELATION_ID", "bad value"],
      ["GITHUB_REPOSITORY", "attacker/repository"],
      ["GITHUB_SHA", "4".repeat(40)],
      ["GITHUB_REF", "refs/heads/other"],
    ]
  ) {
    const environment = validEnvironment();
    environment[name] = value;
    assert.throws(() => parseRevisionControlEnvironment(environment));
  }
});

Deno.test("revision control checks the exact development ref before any Deno operation", async () => {
  const deno = new FakeDenoClient();
  let request: Request | undefined;
  await assert.rejects(
    () =>
      executeRevisionControl(validInput(), {
        deno,
        githubToken: "test-token",
        githubFetch: githubRefFetch("4".repeat(40), (value) => request = value),
        githubApiBaseUrl: "https://github.test/",
      }),
    /Development advanced before promotion/,
  );
  assert.deepEqual(deno.calls, []);
  assert.equal(request?.url, "https://github.test/repos/ubiquity/ai.ubq.fi/git/ref/heads/development");
  assert.equal(request?.headers.get("Authorization"), "Bearer test-token");
  assert.equal(request?.headers.get("X-GitHub-Api-Version"), "2026-03-10");
});

Deno.test("revision control does not mutate when the initial stable identity mismatches", async () => {
  const deno = new FakeDenoClient();
  deno.snapshots = [snapshot("4".repeat(40), "external-revision")];
  await assert.rejects(
    () =>
      executeRevisionControl(validInput(), {
        deno,
        githubToken: "test-token",
        githubFetch: githubRefFetch(),
        githubApiBaseUrl: "https://github.test/",
      }),
    /identity changed before promotion/,
  );
  assert.equal(deno.promoted, false);
  assert.deepEqual(deno.calls, ["snapshot:ai-ubq-fi:2"]);
});

Deno.test("revision control does not mutate when target routing is not fresh", async () => {
  const deno = new FakeDenoClient();
  deno.targetStatus = "pending";
  await assert.rejects(
    () =>
      executeRevisionControl(validInput(), {
        deno,
        githubToken: "test-token",
        githubFetch: githubRefFetch(),
        githubApiBaseUrl: "https://github.test/",
      }),
    /Target revision target-revision is not routed/,
  );
  assert.equal(deno.promoted, false);
  assert.deepEqual(deno.calls, [
    "snapshot:ai-ubq-fi:2",
    "belongs:ai-ubq-fi:target-revision",
    "get:target-revision",
  ]);
});

Deno.test("revision control rechecks stable identity and stops drift before mutation", async () => {
  const deno = new FakeDenoClient();
  deno.snapshots = [snapshot(), snapshot("4".repeat(40), "external-revision")];
  await assert.rejects(
    () =>
      executeRevisionControl(validInput(), {
        deno,
        githubToken: "test-token",
        githubFetch: githubRefFetch(),
        githubApiBaseUrl: "https://github.test/",
      }),
    /identity changed before promotion/,
  );
  assert.equal(deno.promoted, false);
  assert.equal(deno.calls.filter((call) => call.startsWith("promote:")).length, 0);
  assert.equal(deno.calls.filter((call) => call.startsWith("snapshot:")).length, 2);
  assert.equal(deno.calls.filter((call) => call.startsWith("health:")).length, 1);
});

Deno.test("revision control rechecks development immediately before mutation", async () => {
  const deno = new FakeDenoClient();
  let reads = 0;
  const fetcher = ((_input: string | URL | Request, _init?: RequestInit) => {
    reads += 1;
    const sha = reads === 1 ? DEVELOPMENT_SHA : "4".repeat(40);
    return Promise.resolve(Response.json({
      ref: "refs/heads/development",
      object: { type: "commit", sha },
    }));
  }) as typeof fetch;
  await assert.rejects(
    () =>
      executeRevisionControl(validInput(), {
        deno,
        githubToken: "test-token",
        githubFetch: fetcher,
        githubApiBaseUrl: "https://github.test/",
      }),
    /advanced immediately before promotion/,
  );
  assert.equal(reads, 2);
  assert.deepEqual(deno.promotedRevisions, []);
});

Deno.test("revision control compensates a committed promotion when stable verification fails", async () => {
  const deno = new FakeDenoClient();
  deno.productionHealthFailures = 1;
  deno.snapshots = [
    snapshot(),
    snapshot(),
    snapshot(TARGET_SHA, TARGET_REVISION),
  ];
  let observed: unknown;
  try {
    await executeRevisionControl(validInput(), {
      deno,
      githubToken: "test-token",
      githubFetch: githubRefFetch(),
      githubApiBaseUrl: "https://github.test/",
      now: () => Date.parse("2026-08-21T10:31:00.000Z"),
    });
  } catch (error) {
    observed = error;
  }
  assert.ok(observed instanceof RevisionControlOutcomeError);
  assert.equal(observed.result.status, "compensated");
  assert.deepEqual(deno.promotedRevisions, [TARGET_REVISION, CURRENT_REVISION]);
  assert.equal(
    deno.calls.at(-1),
    `production-health:${CURRENT_SHA}:${CURRENT_REVISION}`,
  );
});

Deno.test("revision control inspects and compensates an ambiguous promotion error", async () => {
  const deno = new FakeDenoClient();
  deno.throwAfterPromotingRevision = TARGET_REVISION;
  deno.snapshots = [
    snapshot(),
    snapshot(),
    snapshot(TARGET_SHA, TARGET_REVISION),
  ];
  let observed: unknown;
  try {
    await executeRevisionControl(validInput(), {
      deno,
      githubToken: "test-token",
      githubFetch: githubRefFetch(),
      githubApiBaseUrl: "https://github.test/",
    });
  } catch (error) {
    observed = error;
  }
  assert.ok(observed instanceof RevisionControlOutcomeError);
  assert.equal(observed.result.status, "compensated");
  assert.deepEqual(deno.promotedRevisions, [TARGET_REVISION, CURRENT_REVISION]);
});

Deno.test("revision control promotes only after fresh checks and records strict production evidence", async () => {
  const deno = new FakeDenoClient();
  const result = await executeRevisionControl(validInput(), {
    deno,
    githubToken: "test-token",
    githubFetch: githubRefFetch(),
    githubApiBaseUrl: "https://github.test/",
    now: () => Date.parse("2026-08-21T10:30:00.000Z"),
  });

  assert.equal(deno.promoted, true);
  assert.deepEqual(deno.calls.map((call) => call.split(":")[0]), [
    "snapshot",
    "belongs",
    "get",
    "health",
    "snapshot",
    "belongs",
    "get",
    "promote",
    "production-health",
  ]);
  assert.deepEqual(result, {
    schema_version: 1,
    status: "promoted",
    correlation_id: "sentinel:12345678",
    github_run_id: 42,
    repository: "ubiquity/ai.ubq.fi",
    expected_development_git_sha: DEVELOPMENT_SHA,
    app: "ai-ubq-fi",
    previous: {
      git_sha: CURRENT_SHA,
      revision: CURRENT_REVISION,
      snapshotted_at: "2026-08-21T10:00:00.000Z",
    },
    target: {
      git_sha: TARGET_SHA,
      revision: TARGET_REVISION,
      immutable_health_url: "https://ai-ubq-fi-target-revision.ubiquity-dao.deno.net/health",
    },
    stable: {
      managed: {
        url: "https://ai-ubq-fi.ubiquity-dao.deno.net/health",
        git_sha: TARGET_SHA,
        revision: TARGET_REVISION,
      },
      custom: {
        kind: "cloudflare_challenge",
        url: "https://ai.ubq.fi/health",
        status: 403,
        ray: "test-ray",
      },
    },
    promoted_at: "2026-08-21T10:30:00.000Z",
  });
});

Deno.test("revision control verifies only the managed stable route for the preview app", async () => {
  const environment = validEnvironment();
  environment.SENTINEL_TARGET_APP = "p-ai-ubq-fi";
  const deno = new FakeDenoClient();

  const result = await executeRevisionControl(parseRevisionControlEnvironment(environment), {
    deno,
    githubToken: "test-token",
    githubFetch: githubRefFetch(),
    githubApiBaseUrl: "https://github.test/",
    now: () => Date.parse("2026-08-21T10:30:00.000Z"),
  });

  assert.equal(result.app, "p-ai-ubq-fi");
  assert.equal(result.stable.custom, null);
  assert.equal(
    result.stable.managed.url,
    "https://p-ai-ubq-fi.ubiquity-dao.deno.net/health",
  );
  assert.equal(deno.calls.some((call) => call.startsWith("production-health:")), false);
  assert.deepEqual(
    deno.calls.filter((call) => call.startsWith("snapshot:")),
    ["snapshot:p-ai-ubq-fi:1", "snapshot:p-ai-ubq-fi:1"],
  );
});
