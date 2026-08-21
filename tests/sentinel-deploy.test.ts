import assert from "node:assert/strict";
import {
  defaultRevisionBaseUrl,
  DenoDeployClient,
  normalizeRevisionStatus,
  type SentinelFetch,
} from "../scripts/sentinel/deploy.ts";
import { GitHubActionsClient, type GitHubFetch } from "../scripts/sentinel/github.ts";

const OLD_SHA = "1".repeat(40);
const NEW_SHA = "2".repeat(40);
const WRONG_SHA = "3".repeat(40);
const DENO_TOKEN = "deno-test-token";
const GITHUB_TOKEN = "github-test-token";

interface SeenRequest {
  readonly url: URL;
  readonly method: string;
  readonly headers: Headers;
  readonly body: BodyInit | null | undefined;
  readonly redirect: RequestRedirect | undefined;
  readonly signal: AbortSignal | null;
}

type RequestResponder = (request: SeenRequest) => Response | Promise<Response>;

const queuedFetch = (
  responders: RequestResponder[],
): { fetcher: SentinelFetch; seen: SeenRequest[]; assertDrained: () => void } => {
  const seen: SeenRequest[] = [];
  const fetcher: SentinelFetch = async (input, init = {}) => {
    const url = new URL(input instanceof Request ? input.url : input.toString());
    const request: SeenRequest = {
      url,
      method: init.method ?? (input instanceof Request ? input.method : "GET"),
      headers: new Headers(init.headers ?? (input instanceof Request ? input.headers : undefined)),
      body: init.body,
      redirect: init.redirect,
      signal: init.signal ?? null,
    };
    seen.push(request);
    const responder = responders.shift();
    if (!responder) throw new Error(`Unexpected request: ${request.method} ${url}`);
    return await responder(request);
  };
  return {
    fetcher,
    seen,
    assertDrained: () => assert.equal(responders.length, 0, "all fake API responses should be consumed"),
  };
};

const json = (value: unknown, status = 200, headers: HeadersInit = {}): Response =>
  new Response(JSON.stringify(value), {
    status,
    headers: { "Content-Type": "application/json", ...Object.fromEntries(new Headers(headers)) },
  });

const assertDenoApiRequest = (request: SeenRequest, pathname: string, method = "GET"): void => {
  assert.equal(request.url.origin, "https://deno.test");
  assert.equal(request.url.pathname, pathname);
  assert.equal(request.method, method);
  assert.equal(request.headers.get("Authorization"), `Bearer ${DENO_TOKEN}`);
  assert.equal(request.redirect, "manual");
};

const denoClient = (
  fetcher: SentinelFetch,
  options: {
    now?: () => number;
    sleep?: (ms: number) => Promise<void>;
    requestTimeoutMs?: number;
    createTimeoutSignal?: (ms: number) => AbortSignal;
  } = {},
) =>
  new DenoDeployClient({
    token: DENO_TOKEN,
    apiBaseUrl: "https://deno.test/v2/",
    fetcher,
    ...options,
  });

Deno.test("Deno revision status treats succeeded and routed as routed", () => {
  assert.equal(normalizeRevisionStatus("succeeded"), "routed");
  assert.equal(normalizeRevisionStatus("routed"), "routed");
  assert.equal(normalizeRevisionStatus("ready"), "unknown");
  assert.equal(normalizeRevisionStatus("building"), "pending");
  assert.equal(normalizeRevisionStatus("failed"), "failed");
  assert.equal(normalizeRevisionStatus("future-state"), "unknown");
});

Deno.test("Deno immutable revision base URL is independent of the stable application route", () => {
  assert.equal(
    defaultRevisionBaseUrl("p-ai-ubq-fi", "candidate-revision", "ubiquity-dao"),
    "https://p-ai-ubq-fi-candidate-revision.ubiquity-dao.deno.net",
  );
});

Deno.test("Deno revision listing follows same-endpoint Link pagination and retains every page", async () => {
  const fake = queuedFetch([
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/ai-ubq-fi/revisions");
      assert.equal(request.url.searchParams.get("limit"), "100");
      assert.equal(request.url.searchParams.get("cursor"), null);
      return json(
        [{ id: "first-revision", status: "succeeded" }],
        200,
        { Link: '</v2/apps/ai-ubq-fi/revisions?limit=100&cursor=next-page>; rel="next"' },
      );
    },
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/ai-ubq-fi/revisions");
      assert.equal(request.url.searchParams.get("limit"), "100");
      assert.equal(request.url.searchParams.get("cursor"), "next-page");
      return json({ items: [{ id: "second-revision", status: "building" }] });
    },
  ]);

  const revisions = await denoClient(fake.fetcher).listRevisions("ai-ubq-fi");
  assert.deepEqual(revisions.map((revision) => [revision.id, revision.status]), [
    ["first-revision", "routed"],
    ["second-revision", "pending"],
  ]);
  fake.assertDrained();
});

Deno.test("Deno requests install a bounded timeout signal and fail closed on timeout", async () => {
  const timeouts: number[] = [];
  const controller = new AbortController();
  controller.abort(new DOMException("deadline", "TimeoutError"));
  const fake = queuedFetch([
    (request) => {
      assert.equal(request.signal, controller.signal);
      throw controller.signal.reason;
    },
  ]);
  const client = denoClient(fake.fetcher, {
    requestTimeoutMs: 1_234,
    createTimeoutSignal: (milliseconds) => {
      timeouts.push(milliseconds);
      return controller.signal;
    },
  });

  await assert.rejects(() => client.getRevision("candidate-revision"), /timed out/);
  assert.deepEqual(timeouts, [1_234]);
  fake.assertDrained();
});

Deno.test("Deno candidate resolution accepts only a new routed revision with the exact health identity", async () => {
  const fake = queuedFetch([
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/p-ai-ubq-fi/revisions");
      return json({ revisions: [{ id: "old-revision", status: "succeeded" }, { id: "new-revision" }] });
    },
    (request) => {
      assertDenoApiRequest(request, "/v2/revisions/new-revision");
      return json({ revision: { id: "new-revision", status: "succeeded" } });
    },
    (request) => {
      assert.equal(request.url.href, "https://revision.test/new-revision/health");
      assert.equal(request.headers.get("Authorization"), null);
      return json(
        { release: { git_sha: NEW_SHA, deployment_id: "new-revision" } },
        200,
        { "x-uos-git-sha": NEW_SHA, "x-uos-deployment-id": "new-revision" },
      );
    },
  ]);
  const revision = await denoClient(fake.fetcher).resolveNewCandidateRevision({
    app: "p-ai-ubq-fi",
    previousRevisionIds: new Set(["old-revision"]),
    candidateGitSha: NEW_SHA,
    revisionHealthUrl: (_app, id) => `https://revision.test/${id}/health`,
  });

  assert.equal(revision?.id, "new-revision");
  assert.equal(revision?.status, "routed");
  fake.assertDrained();
});

Deno.test("Deno candidate wait polls with injected time and ignores a mismatched health identity", async () => {
  let now = 1_000;
  const fake = queuedFetch([
    () => json({ revisions: [{ id: "old-revision", status: "succeeded" }] }),
    () => json({ revisions: [{ id: "old-revision" }, { id: "wrong-revision" }] }),
    () => json({ id: "wrong-revision", status: "succeeded" }),
    () =>
      json({ release: { git_sha: WRONG_SHA, deployment_id: "wrong-revision" } }, 200, {
        "x-uos-git-sha": WRONG_SHA,
        "x-uos-deployment-id": "wrong-revision",
      }),
    () => json({ revisions: [{ id: "old-revision" }, { id: "new-revision" }] }),
    () => json({ id: "new-revision", status: "routed" }),
    () =>
      json({ release: { git_sha: NEW_SHA, deployment_id: "new-revision" } }, 200, {
        "x-uos-git-sha": NEW_SHA,
        "x-uos-deployment-id": "new-revision",
      }),
  ]);
  const client = denoClient(fake.fetcher, {
    now: () => now,
    sleep: (milliseconds) => {
      now += milliseconds;
      return Promise.resolve();
    },
  });
  const revision = await client.waitForNewCandidateRevision({
    app: "p-ai-ubq-fi",
    previousRevisionIds: new Set(["old-revision"]),
    candidateGitSha: NEW_SHA,
    timeoutMs: 3_000,
    pollIntervalMs: 1_000,
    revisionHealthUrl: (_app, id) => `https://revision.test/${id}/health`,
  });
  assert.equal(revision.id, "new-revision");
  assert.equal(now, 3_000);
  fake.assertDrained();
});

Deno.test("Deno promotion requires application membership, sends no body, and requires HTTP 204", async () => {
  const success = queuedFetch([
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/ai-ubq-fi/revisions");
      return json({ data: [{ id: "candidate-revision", status: "succeeded" }] });
    },
    (request) => {
      assertDenoApiRequest(request, "/v2/revisions/candidate-revision");
      return json({ revision: { id: "candidate-revision", status: "routed" } });
    },
    (request) => {
      assertDenoApiRequest(request, "/v2/revisions/candidate-revision/promote", "POST");
      assert.equal(request.body, undefined, "promotion must be bodyless");
      assert.equal(request.headers.has("Content-Type"), false);
      return new Response(null, { status: 204 });
    },
  ]);
  await denoClient(success.fetcher).promoteRevision("ai-ubq-fi", "candidate-revision");
  success.assertDrained();

  const notMember = queuedFetch([() => json({ revisions: [{ id: "different-revision" }] })]);
  await assert.rejects(
    () => denoClient(notMember.fetcher).promoteRevision("ai-ubq-fi", "candidate-revision"),
    /not a member/,
  );
  assert.equal(notMember.seen.length, 1, "membership failure must stop before POST");

  const wrongStatus = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () => json({ revision: { id: "candidate-revision", status: "building" } }),
  ]);
  await assert.rejects(
    () => denoClient(wrongStatus.fetcher).promoteRevision("ai-ubq-fi", "candidate-revision"),
    /not routed immediately before promotion/,
  );
  assert.equal(wrongStatus.seen.length, 2, "status failure must stop before POST");

  const wrongPromotionStatus = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision", status: "routed" }] }),
    () => json({ revision: { id: "candidate-revision", status: "routed" } }),
    () => json({ promoted: true }, 200),
  ]);
  await assert.rejects(
    () => denoClient(wrongPromotionStatus.fetcher).promoteRevision("ai-ubq-fi", "candidate-revision"),
    /expected 204/,
  );
});

Deno.test("Deno stable health rejects identity mismatch", async () => {
  let now = 0;
  const fake = queuedFetch([
    () =>
      json({ release: { git_sha: WRONG_SHA, deployment_id: "candidate-revision" } }, 200, {
        "x-uos-git-sha": WRONG_SHA,
        "x-uos-deployment-id": "candidate-revision",
      }),
  ]);
  await assert.rejects(
    () =>
      denoClient(fake.fetcher, {
        now: () => now,
        sleep: (milliseconds) => {
          now += milliseconds;
          return Promise.resolve();
        },
      }).verifyHealthIdentity(
        ["https://ai.ubq.fi/health"],
        NEW_SHA,
        "candidate-revision",
        { timeoutMs: 1, pollIntervalMs: 1 },
      ),
    /Health identity mismatch/,
  );
});

Deno.test("Deno health verification retries until an identity-bearing HTTP 200", async () => {
  let now = 0;
  const fake = queuedFetch([
    (request) => {
      assert.equal(request.url.href, "https://ai.ubq.fi/health");
      return json({ error: { code: "temporary" } }, 502);
    },
    (request) => {
      assert.equal(request.url.href, "https://ai.ubq.fi/health");
      return json(
        { release: { git_sha: NEW_SHA, deployment_id: "candidate-revision" } },
        200,
        { "x-uos-git-sha": NEW_SHA, "x-uos-deployment-id": "candidate-revision" },
      );
    },
  ]);
  const client = denoClient(fake.fetcher, {
    now: () => now,
    sleep: (milliseconds) => {
      now += milliseconds;
      return Promise.resolve();
    },
  });

  const identities = await client.verifyHealthIdentity(
    ["https://ai.ubq.fi/health"],
    NEW_SHA,
    "candidate-revision",
    { timeoutMs: 500, pollIntervalMs: 100 },
  );
  assert.deepEqual(identities, [{
    url: "https://ai.ubq.fi/health",
    gitSha: NEW_SHA,
    revisionId: "candidate-revision",
  }]);
  assert.equal(now, 100);
  fake.assertDrained();
});

Deno.test("Deno health rejects an identity-bearing HTTP 503", async () => {
  const fake = queuedFetch([
    () =>
      json({ release: { git_sha: NEW_SHA, deployment_id: "candidate-revision" } }, 503, {
        "x-uos-git-sha": NEW_SHA,
        "x-uos-deployment-id": "candidate-revision",
      }),
  ]);
  await assert.rejects(
    () => denoClient(fake.fetcher).readHealth("https://ai.ubq.fi/health"),
    /expected 200/,
  );
  fake.assertDrained();
});

Deno.test("Deno health requires release identity in both body and headers", async () => {
  const missingHeaders = queuedFetch([
    () => json({ release: { git_sha: NEW_SHA, deployment_id: "candidate-revision" } }),
  ]);
  await assert.rejects(
    () => denoClient(missingHeaders.fetcher).readHealth("https://managed.test/health"),
    /both its body and headers/,
  );
  missingHeaders.assertDrained();

  const missingBody = queuedFetch([
    () => json({}, 200, { "x-uos-git-sha": NEW_SHA, "x-uos-deployment-id": "candidate-revision" }),
  ]);
  await assert.rejects(
    () => denoClient(missingBody.fetcher).readHealth("https://managed.test/health"),
    /both its body and headers/,
  );
  missingBody.assertDrained();
});

Deno.test("production health accepts one identified Cloudflare challenge after exact managed identity", async () => {
  const fake = queuedFetch([
    () =>
      json({ release: { git_sha: NEW_SHA, deployment_id: "candidate-revision" } }, 200, {
        "x-uos-git-sha": NEW_SHA,
        "x-uos-deployment-id": "candidate-revision",
      }),
    () =>
      new Response("challenge", {
        status: 403,
        headers: { server: "cloudflare", "cf-mitigated": "challenge", "cf-ray": "test-ray" },
      }),
  ]);
  const attestation = await denoClient(fake.fetcher).verifyProductionHealthIdentity(
    "https://ai-ubq-fi.ubiquity-dao.deno.net/health",
    "https://ai.ubq.fi/health",
    NEW_SHA,
    "candidate-revision",
  );
  assert.deepEqual(attestation, {
    managed: {
      url: "https://ai-ubq-fi.ubiquity-dao.deno.net/health",
      gitSha: NEW_SHA,
      revisionId: "candidate-revision",
    },
    custom: {
      kind: "cloudflare_challenge",
      url: "https://ai.ubq.fi/health",
      status: 403,
      ray: "test-ray",
    },
  });
  assert.equal(fake.seen.length, 2, "an identified challenge must not be retried");
  fake.assertDrained();
});

Deno.test("production health fails immediately on custom-route HTTP 200 identity mismatch", async () => {
  const fake = queuedFetch([
    () =>
      json({ release: { git_sha: NEW_SHA, deployment_id: "candidate-revision" } }, 200, {
        "x-uos-git-sha": NEW_SHA,
        "x-uos-deployment-id": "candidate-revision",
      }),
    () =>
      json({ release: { git_sha: WRONG_SHA, deployment_id: "wrong-revision" } }, 200, {
        "x-uos-git-sha": WRONG_SHA,
        "x-uos-deployment-id": "wrong-revision",
      }),
  ]);
  await assert.rejects(
    () =>
      denoClient(fake.fetcher).verifyProductionHealthIdentity(
        "https://ai-ubq-fi.ubiquity-dao.deno.net/health",
        "https://ai.ubq.fi/health",
        NEW_SHA,
        "candidate-revision",
      ),
    /Health identity mismatch/,
  );
  assert.equal(fake.seen.length, 2, "an HTTP 200 identity mismatch must not be retried");
  fake.assertDrained();
});

Deno.test("Deno production snapshot preserves the exact previous healthy revision", async () => {
  const fake = queuedFetch([
    () =>
      json({ release: { git_sha: OLD_SHA, deployment_id: "previous-revision" } }, 200, {
        "x-uos-git-sha": OLD_SHA,
        "x-uos-deployment-id": "previous-revision",
      }),
    () =>
      json({ release: { git_sha: OLD_SHA, deployment_id: "previous-revision" } }, 200, {
        "x-uos-git-sha": OLD_SHA,
        "x-uos-deployment-id": "previous-revision",
      }),
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/ai-ubq-fi/revisions");
      return json({ revisions: [{ id: "previous-revision", status: "succeeded" }] });
    },
    (request) => {
      assertDenoApiRequest(request, "/v2/revisions/previous-revision");
      return json({ revision: { id: "previous-revision", status: "succeeded" } });
    },
  ]);
  const urls = ["https://ai-ubq-fi.ubiquity-dao.deno.net/health", "https://ai.ubq.fi/health"];
  const snapshot = await denoClient(fake.fetcher, { now: () => Date.parse("2026-08-21T12:00:00Z") })
    .snapshotHealthyProduction("ai-ubq-fi", urls);

  assert.deepEqual(snapshot, {
    gitSha: OLD_SHA,
    revisionId: "previous-revision",
    healthUrls: urls,
    snapshottedAt: "2026-08-21T12:00:00.000Z",
  });
  assert.equal(Object.isFrozen(snapshot), true);
  assert.equal(Object.isFrozen(snapshot.healthUrls), true);
  fake.assertDrained();
});

Deno.test("Deno production snapshot preserves the managed identity through a Cloudflare challenge", async () => {
  const fake = queuedFetch([
    () =>
      json({ release: { git_sha: OLD_SHA, deployment_id: "previous-revision" } }, 200, {
        "x-uos-git-sha": OLD_SHA,
        "x-uos-deployment-id": "previous-revision",
      }),
    () =>
      new Response("challenge", {
        status: 403,
        headers: { server: "cloudflare", "cf-mitigated": "challenge", "cf-ray": "snapshot-ray" },
      }),
    () => json({ revisions: [{ id: "previous-revision", status: "succeeded" }] }),
    () => json({ revision: { id: "previous-revision", status: "succeeded" } }),
  ]);
  const urls = ["https://ai-ubq-fi.ubiquity-dao.deno.net/health", "https://ai.ubq.fi/health"];
  const snapshot = await denoClient(fake.fetcher, { now: () => Date.parse("2026-08-21T12:00:00Z") })
    .snapshotHealthyProduction("ai-ubq-fi", urls);
  assert.equal(snapshot.gitSha, OLD_SHA);
  assert.equal(snapshot.revisionId, "previous-revision");
  assert.equal(fake.seen.length, 4, "the custom challenge must be observed only once");
  fake.assertDrained();
});

const workflowRun = (
  id: number,
  headSha: string,
  status: string,
  conclusion: string | null,
  createdAt = "2026-08-21T12:00:01Z",
  displayTitle = "Deno Deploy sentinel-test-correlation",
) => ({
  id,
  head_sha: headSha,
  status,
  conclusion,
  created_at: createdAt,
  display_title: displayTitle,
  html_url: `https://github.test/ubiquity/ai.ubq.fi/actions/runs/${id}`,
});

const githubClient = (
  fetcher: GitHubFetch,
  options: {
    now?: () => number;
    sleep?: (ms: number) => Promise<void>;
    requestTimeoutMs?: number;
    createTimeoutSignal?: (ms: number) => AbortSignal;
  } = {},
) =>
  new GitHubActionsClient({
    repository: "ubiquity/ai.ubq.fi",
    token: GITHUB_TOKEN,
    apiBaseUrl: "https://github.test/",
    fetcher,
    ...options,
  });

const assertGitHubApiRequest = (request: SeenRequest, pathname: string, method = "GET"): void => {
  assert.equal(request.url.origin, "https://github.test");
  assert.equal(request.url.pathname, pathname);
  assert.equal(request.method, method);
  assert.equal(request.headers.get("Authorization"), `Bearer ${GITHUB_TOKEN}`);
  assert.equal(request.headers.get("X-GitHub-Api-Version"), "2026-03-10");
};

Deno.test("GitHub current-run lookup exposes the stable workflow creation timestamp", async () => {
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/runs/39");
      return json(workflowRun(39, NEW_SHA, "in_progress", null, "2026-08-21T11:59:58Z"));
    },
  ]);
  const run = await githubClient(fake.fetcher).getWorkflowRun(39);
  assert.equal(run.createdAt, "2026-08-21T11:59:58Z");
  fake.assertDrained();
});

Deno.test("GitHub dispatch and polling follow only the exact candidate head SHA to a successful conclusion", async () => {
  let now = Date.parse("2026-08-21T12:00:00Z");
  const displayTitle = "Deno Deploy sentinel-test-correlation";
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(
        request,
        "/repos/ubiquity/ai.ubq.fi/actions/workflows/deno-deploy.yml/dispatches",
        "POST",
      );
      assert.deepEqual(JSON.parse(String(request.body)), {
        ref: "feature/triage-sentinel",
        inputs: { deploy_preview: true, sentinel_build_only: true },
        return_run_details: true,
      });
      return json({
        workflow_run_id: 41,
        run_url: "https://github.test/repos/ubiquity/ai.ubq.fi/actions/runs/41",
        html_url: "https://github.test/ubiquity/ai.ubq.fi/actions/runs/41",
      });
    },
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/runs/41");
      return json(workflowRun(41, NEW_SHA, "in_progress", null, undefined, displayTitle));
    },
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/runs/41");
      return json(workflowRun(41, NEW_SHA, "completed", "success", undefined, displayTitle));
    },
  ]);
  const client = githubClient(fake.fetcher, {
    now: () => now,
    sleep: (milliseconds) => {
      now += milliseconds;
      return Promise.resolve();
    },
  });
  const dispatch = await client.dispatchWorkflow(
    "deno-deploy.yml",
    "feature/triage-sentinel",
    { deploy_preview: true, sentinel_build_only: true },
  );
  const run = await client.waitForWorkflow({
    runId: dispatch.runId,
    headSha: NEW_SHA,
    displayTitle,
    timeoutMs: 2_000,
    pollIntervalMs: 1_000,
  });

  assert.equal(run.id, 41);
  assert.equal(run.headSha, NEW_SHA);
  assert.equal(run.conclusion, "success");
  fake.assertDrained();
});

Deno.test("GitHub workflow polling rejects a failed exact-SHA run", async () => {
  const now = Date.parse("2026-08-21T12:00:00Z");
  const fake = queuedFetch([
    () => json(workflowRun(51, NEW_SHA, "completed", "failure")),
  ]);
  await assert.rejects(
    () =>
      githubClient(fake.fetcher, { now: () => now }).waitForWorkflow({
        runId: 51,
        headSha: NEW_SHA,
        timeoutMs: 1_000,
      }),
    /completed with failure/,
  );
});

Deno.test("GitHub workflow polling reconciles transient API failures before returning", async () => {
  let now = Date.parse("2026-08-21T12:00:00Z");
  const fake = queuedFetch([
    () => {
      throw new TypeError("temporary GitHub network failure");
    },
    () => json(workflowRun(52, NEW_SHA, "in_progress", null)),
    () => json(workflowRun(52, NEW_SHA, "completed", "success")),
  ]);
  const run = await githubClient(fake.fetcher, {
    now: () => now,
    sleep: (milliseconds) => {
      now += milliseconds;
      return Promise.resolve();
    },
  }).waitForWorkflow({
    runId: 52,
    headSha: NEW_SHA,
    timeoutMs: 3_000,
    pollIntervalMs: 1_000,
  });

  assert.equal(run.id, 52);
  assert.equal(run.conclusion, "success");
  assert.equal(now, Date.parse("2026-08-21T12:00:02Z"));
  fake.assertDrained();
});

Deno.test("GitHub dispatch requires exact run details from the modern API", async () => {
  const fake = queuedFetch([
    () => new Response(null, { status: 204 }),
  ]);
  await assert.rejects(
    () => githubClient(fake.fetcher).dispatchWorkflow("deno-deploy.yml", "development"),
    /expected 200/,
  );
  fake.assertDrained();
});

Deno.test("GitHub API requests use a bounded timeout and fail closed", async () => {
  const timeouts: number[] = [];
  const controller = new AbortController();
  controller.abort(new DOMException("deadline", "TimeoutError"));
  const fake = queuedFetch([
    (request) => {
      assert.equal(request.signal, controller.signal);
      throw controller.signal.reason;
    },
  ]);
  await assert.rejects(
    () =>
      githubClient(fake.fetcher, {
        requestTimeoutMs: 1_234,
        createTimeoutSignal: (milliseconds) => {
          timeouts.push(milliseconds);
          return controller.signal;
        },
      }).listRunArtifacts(1),
    /timed out/,
  );
  assert.deepEqual(timeouts, [1_234]);
  fake.assertDrained();
});

Deno.test("GitHub artifact adapters return metadata and exact archive bytes", async () => {
  const archive = new Uint8Array([0x50, 0x4b, 0x03, 0x04]);
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/runs/61/artifacts");
      return json({
        artifacts: [{
          id: 71,
          name: "sentinel-replay-cases",
          size_in_bytes: archive.length,
          expired: false,
          created_at: "2026-08-21T12:00:00Z",
          expires_at: "2026-11-19T12:00:00Z",
        }],
      });
    },
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/artifacts/71/zip");
      assert.equal(request.redirect, "follow");
      return new Response(archive);
    },
  ]);
  const client = githubClient(fake.fetcher);
  const artifacts = await client.listRunArtifacts(61);
  assert.equal(artifacts.length, 1);
  assert.equal(artifacts[0].name, "sentinel-replay-cases");
  assert.deepEqual(await client.downloadArtifact(artifacts[0].id), archive);
  fake.assertDrained();
});

Deno.test("GitHub artifact downloads reject declared and streamed bytes above the caller limit", async () => {
  const fake = queuedFetch([
    () => new Response(new Uint8Array([1, 2, 3]), { headers: { "Content-Length": "3" } }),
    () =>
      new Response(
        new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array([1, 2]));
            controller.enqueue(new Uint8Array([3]));
            controller.close();
          },
        }),
      ),
  ]);
  const client = githubClient(fake.fetcher);
  await assert.rejects(() => client.downloadArtifact(1, 2), /byte limit/);
  await assert.rejects(() => client.downloadArtifact(2, 2), /byte limit/);
  fake.assertDrained();
});

Deno.test("GitHub repository artifacts paginate and filter expired or older encrypted replay bundles", async () => {
  const fence = Date.parse("2026-08-20T00:00:00Z");
  const firstPage = Array.from({ length: 100 }, (_, index) => ({
    id: 100 + index,
    name: index === 2 ? "different-artifact" : "sentinel-replay-bundle",
    size_in_bytes: 10,
    expired: index === 1,
    created_at: index < 3 ? "2026-08-21T00:00:00Z" : "2026-08-19T00:00:00Z",
    expires_at: "2026-11-19T00:00:00Z",
  }));
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/artifacts");
      assert.equal(request.url.searchParams.get("name"), "sentinel-replay-bundle");
      assert.equal(request.url.searchParams.get("per_page"), "100");
      assert.equal(request.url.searchParams.get("page"), "1");
      return json({ total_count: 101, artifacts: firstPage });
    },
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/artifacts");
      assert.equal(request.url.searchParams.get("name"), "sentinel-replay-bundle");
      assert.equal(request.url.searchParams.get("page"), "2");
      return json({
        total_count: 101,
        artifacts: [{
          id: 200,
          name: "sentinel-replay-bundle",
          size_in_bytes: 12,
          expired: false,
          created_at: "2026-08-21T01:00:00Z",
          expires_at: "2026-11-19T01:00:00Z",
        }],
      });
    },
  ]);

  const artifacts = await githubClient(fake.fetcher).listRepositoryArtifacts({
    name: "sentinel-replay-bundle",
    createdAfterMs: fence,
  });
  assert.deepEqual(artifacts.map((artifact) => artifact.id), [100, 200]);
  fake.assertDrained();
});
