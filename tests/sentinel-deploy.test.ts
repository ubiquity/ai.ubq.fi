import assert from "node:assert/strict";
import {
  defaultRevisionBaseUrl,
  DenoDeployClient,
  normalizeRevisionStatus,
  type SentinelFetch,
} from "../scripts/sentinel/deploy.ts";
import {
  GitHubActionsClient,
  type GitHubFetch,
  MAX_ARTIFACT_METADATA_PAGES,
  MAX_ISSUE_METADATA_PAGES,
} from "../scripts/sentinel/github.ts";

const OLD_SHA = "1".repeat(40);
const NEW_SHA = "2".repeat(40);
const WRONG_SHA = "3".repeat(40);
const DENO_TOKEN = "deno-test-token";
const GITHUB_TOKEN = "github-test-token";

Deno.test("GitHub artifact pagination covers ninety days of five-minute Sentinel runs", () => {
  assert.ok(MAX_ARTIFACT_METADATA_PAGES >= 521);
  assert.ok(MAX_ARTIFACT_METADATA_PAGES <= 600);
});

Deno.test("GitHub issue pagination has a bounded metadata budget", () => {
  assert.equal(MAX_ISSUE_METADATA_PAGES, 10);
});

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

Deno.test("Deno revision listing re-homes the exact console pagination alias before authorizing it", async () => {
  const fake = queuedFetch([
    (request) => {
      assert.equal(request.url.origin, "https://api.deno.com");
      assert.equal(request.url.pathname, "/v2/apps/p-ai-ubq-fi/revisions");
      assert.equal(request.url.searchParams.get("cursor"), null);
      assert.equal(request.headers.get("Authorization"), `Bearer ${DENO_TOKEN}`);
      return json(
        [{ id: "first-revision", status: "succeeded" }],
        200,
        {
          Link: '<https://console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=next-page&limit=100>; rel="next"',
        },
      );
    },
    (request) => {
      assert.equal(request.url.origin, "https://api.deno.com");
      assert.equal(request.url.pathname, "/v2/apps/p-ai-ubq-fi/revisions");
      assert.equal(request.url.searchParams.get("cursor"), "next-page");
      assert.equal(request.url.searchParams.get("limit"), "100");
      assert.equal(request.headers.get("Authorization"), `Bearer ${DENO_TOKEN}`);
      return json([{ id: "second-revision", status: "building" }]);
    },
  ]);
  const client = new DenoDeployClient({
    token: DENO_TOKEN,
    apiBaseUrl: "https://api.deno.com/v2/",
    fetcher: fake.fetcher,
  });

  const revisions = await client.listRevisions("p-ai-ubq-fi");

  assert.deepEqual(revisions.map((revision) => revision.id), ["first-revision", "second-revision"]);
  assert.ok(fake.seen.every((request) => request.url.origin === "https://api.deno.com"));
  fake.assertDrained();
});

Deno.test("Deno revision listing preserves opaque cursors when the documented Link omits limit", async () => {
  const fake = queuedFetch([
    () =>
      json([{ id: "first-revision", status: "succeeded" }], 200, {
        Link: '</v2/apps/ai-ubq-fi/revisions?cursor=%20opaque%20>; rel="next"',
      }),
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/ai-ubq-fi/revisions");
      assert.equal(request.url.searchParams.get("cursor"), " opaque ");
      assert.equal(request.url.searchParams.get("limit"), "100");
      return json([{ id: "second-revision", status: "succeeded" }]);
    },
  ]);

  const revisions = await denoClient(fake.fetcher).listRevisions("ai-ubq-fi");

  assert.deepEqual(revisions.map((revision) => revision.id), ["first-revision", "second-revision"]);
  fake.assertDrained();
});

Deno.test("Deno revision listing rejects unsafe console pagination variants", async () => {
  const links = [
    "https://console.deno.com/api/v2/apps/other-app/revisions?cursor=next-page&limit=100",
    "https://console.deno.com.evil.example/api/v2/apps/p-ai-ubq-fi/revisions?cursor=next-page&limit=100",
    "https://user@console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=next-page&limit=100",
    "https://console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=one&cursor=two&limit=100",
    "https://console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=&limit=100",
    "https://console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=next-page&limit=99",
    "https://console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=next-page&limit=100&status=succeeded",
    "https://console.deno.com/api/v2/apps/p-ai-ubq-fi/revisions?cursor=next-page&limit=100#fragment",
  ];
  for (const link of links) {
    const fake = queuedFetch([
      () =>
        json([{ id: "first-revision", status: "succeeded" }], 200, {
          Link: `<${link}>; rel="next"`,
        }),
    ]);
    const client = new DenoDeployClient({
      token: DENO_TOKEN,
      apiBaseUrl: "https://api.deno.com/v2/",
      fetcher: fake.fetcher,
    });

    await assert.rejects(() => client.listRevisions("p-ai-ubq-fi"), /unsafe next-page link/);
    assert.equal(fake.seen.length, 1);
    fake.assertDrained();
  }
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

const githubIssue = (
  number: number,
  overrides: Record<string, unknown> = {},
): Record<string, unknown> => ({
  id: 1_000 + number,
  node_id: `I_kwDOIssue${number}`,
  number,
  state: "open",
  title: `Issue ${number}`,
  body: "Acceptance:\n- Fix it.\n\nFiles:\n- src/openai.ts\n",
  html_url: `https://github.com/ubiquity/ai.ubq.fi/issues/${number}`,
  user: { login: "octocat" },
  author_association: "MEMBER",
  labels: [{ name: "Priority: 2 (Medium)" }, { name: "Time: <2 Hours" }],
  assignees: [],
  locked: false,
  comments: 0,
  created_at: "2026-08-21T12:00:00Z",
  updated_at: "2026-08-21T12:01:00Z",
  ...overrides,
});

Deno.test("GitHub issue intake paginates open issues and excludes pull requests", async () => {
  const firstPage = Array.from(
    { length: 100 },
    (_, index) => githubIssue(index + 1, index === 50 ? { pull_request: { url: "https://github.test/pull/51" } } : {}),
  );
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/issues");
      assert.equal(request.url.searchParams.get("state"), "open");
      assert.equal(request.url.searchParams.get("sort"), "created");
      assert.equal(request.url.searchParams.get("direction"), "asc");
      assert.equal(request.url.searchParams.get("per_page"), "100");
      assert.equal(request.url.searchParams.get("page"), "1");
      return json(firstPage);
    },
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/issues");
      assert.equal(request.url.searchParams.get("page"), "2");
      return json([githubIssue(101, { body: null, labels: ["Priority: 2 (Medium)", "Time: <2 Hours"] })]);
    },
  ]);

  const issues = await githubClient(fake.fetcher).listOpenIssues();
  assert.equal(issues.length, 100);
  assert.equal(issues.some((issue) => issue.number === 51), false);
  assert.equal(issues.find((issue) => issue.number === 101)?.body, "");
  fake.assertDrained();
});

Deno.test("GitHub issue detail and relationships use exact repository resources", async () => {
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/issues/113");
      return json(githubIssue(113));
    },
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/issues/113/sub_issues");
      assert.equal(request.url.searchParams.get("per_page"), "1");
      assert.equal(request.url.searchParams.get("page"), "1");
      return json([]);
    },
    (request) => {
      assertGitHubApiRequest(request, "/graphql", "POST");
      assert.equal(request.headers.get("Content-Type"), "application/json");
      const body = JSON.parse(String(request.body));
      assert.deepEqual(body.variables, { owner: "ubiquity", name: "ai.ubq.fi", number: 113 });
      assert.match(body.query, /editor \{ login \}/u);
      assert.match(body.query, /lastEditedAt/u);
      assert.match(body.query, /RENAMED_TITLE_EVENT/u);
      assert.match(body.query, /parent \{ number \}/u);
      assert.match(body.query, /blockedBy\(first: 1\)/u);
      return json({
        data: {
          repository: {
            issue: {
              editor: { login: "body-writer" },
              lastEditedAt: "2026-08-21T12:00:30Z",
              timelineItems: {
                totalCount: 2,
                nodes: [{
                  createdAt: "2026-08-21T12:00:45Z",
                  actor: { login: "title-writer" },
                  currentTitle: "Issue 113",
                }],
              },
              parent: null,
              blockedBy: { totalCount: 0 },
              blocking: { totalCount: 0 },
            },
          },
        },
      });
    },
  ]);

  const client = githubClient(fake.fetcher);
  const issue = await client.getIssue(113);
  const relations = await client.getIssueRelations(113);
  assert.equal(issue.number, 113);
  assert.equal(issue.authorLogin, "octocat");
  assert.deepEqual(relations, {
    parentIssueNumber: null,
    subIssueCount: 0,
    blockedByCount: 0,
    blockingCount: 0,
    latestBodyEdit: { editorLogin: "body-writer", editedAt: "2026-08-21T12:00:30Z" },
    latestTitleEdit: {
      editorLogin: "title-writer",
      editedAt: "2026-08-21T12:00:45Z",
      title: "Issue 113",
    },
  });
  fake.assertDrained();
});

Deno.test("GitHub issue comment intake is bounded to the exact issue resource", async () => {
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/issues/142/comments");
      assert.equal(request.url.searchParams.get("per_page"), "100");
      assert.equal(request.url.searchParams.get("page"), "1");
      return json([{
        id: 54_323_347_30,
        user: { login: "ubiquity-os[bot]", type: "Bot" },
        body: "> [!WARNING]\n> You are not allowed to set labels.\n",
        created_at: "2026-08-26T23:33:29Z",
        updated_at: "2026-08-26T23:33:29Z",
      }, {
        id: 54_323_347_31,
        user: null,
        created_at: "2026-08-26T23:34:29Z",
        updated_at: "2026-08-26T23:34:29Z",
      }]);
    },
  ]);

  const comments = await githubClient(fake.fetcher).listIssueComments(142);
  assert.deepEqual(comments, [{
    id: 54_323_347_30,
    authorLogin: "ubiquity-os[bot]",
    authorType: "Bot",
    body: "> [!WARNING]\n> You are not allowed to set labels.\n",
    createdAt: "2026-08-26T23:33:29Z",
    updatedAt: "2026-08-26T23:33:29Z",
  }, {
    id: 54_323_347_31,
    authorLogin: null,
    authorType: null,
    body: null,
    createdAt: "2026-08-26T23:34:29Z",
    updatedAt: "2026-08-26T23:34:29Z",
  }]);
  fake.assertDrained();
});

Deno.test("GitHub repository permission lookup accepts only canonical calculated values", async () => {
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/collaborators/octocat/permission");
      return json({ permission: "admin" });
    },
    () => json({ permission: "write" }),
    () => json({ permission: "read" }),
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/collaborators/former-member/permission");
      return json({ message: "Not Found" }, 404);
    },
    () => json({ permission: "maintain" }),
  ]);
  const client = githubClient(fake.fetcher);
  assert.equal(await client.getRepositoryPermission("octocat"), "admin");
  assert.equal(await client.getRepositoryPermission("writer"), "write");
  assert.equal(await client.getRepositoryPermission("reader"), "read");
  assert.equal(await client.getRepositoryPermission("former-member"), "none");
  await assert.rejects(() => client.getRepositoryPermission("malformed"), /incomplete payload/);
  fake.assertDrained();
});

Deno.test("GitHub issue relationships retain a non-null parent issue number", async () => {
  const fake = queuedFetch([
    () => json([]),
    () =>
      json({
        data: {
          repository: {
            issue: {
              editor: null,
              lastEditedAt: null,
              timelineItems: { totalCount: 0, nodes: [] },
              parent: { number: 77 },
              blockedBy: { totalCount: 0 },
              blocking: { totalCount: 0 },
            },
          },
        },
      }),
  ]);
  assert.deepEqual(await githubClient(fake.fetcher).getIssueRelations(113), {
    parentIssueNumber: 77,
    subIssueCount: 0,
    blockedByCount: 0,
    blockingCount: 0,
    latestBodyEdit: null,
    latestTitleEdit: null,
  });
  fake.assertDrained();
});

Deno.test("GitHub issue intake rejects malformed API records and relationship payloads", async () => {
  const malformedIssue = queuedFetch([() => json([githubIssue(1, { updated_at: "invalid" })])]);
  await assert.rejects(
    () => githubClient(malformedIssue.fetcher).listOpenIssues(),
    /incomplete issue/,
  );

  const malformedRelations = queuedFetch([
    () => json([]),
    () =>
      json({
        data: {
          repository: {
            issue: {
              editor: null,
              lastEditedAt: null,
              timelineItems: { totalCount: 0, nodes: [] },
              parent: null,
              blockedBy: {},
              blocking: { totalCount: 0 },
            },
          },
        },
      }),
  ]);
  await assert.rejects(
    () => githubClient(malformedRelations.fetcher).getIssueRelations(1),
    /incomplete payload/,
  );

  const missingParent = queuedFetch([
    () => json([]),
    () =>
      json({
        data: {
          repository: {
            issue: {
              editor: null,
              lastEditedAt: null,
              timelineItems: { totalCount: 0, nodes: [] },
              blockedBy: { totalCount: 0 },
              blocking: { totalCount: 0 },
            },
          },
        },
      }),
  ]);
  await assert.rejects(
    () => githubClient(missingParent.fetcher).getIssueRelations(1),
    /incomplete payload/,
  );

  const partialGraphQl = queuedFetch([
    () => json([]),
    () =>
      json({
        data: {
          repository: {
            issue: {
              editor: null,
              lastEditedAt: null,
              timelineItems: { totalCount: 0, nodes: [] },
              parent: null,
              blockedBy: { totalCount: 0 },
              blocking: { totalCount: 0 },
            },
          },
        },
        errors: [{ message: "parent lookup failed" }],
      }),
  ]);
  await assert.rejects(
    () => githubClient(partialGraphQl.fetcher).getIssueRelations(1),
    /incomplete payload/,
  );

  const mismatchedBodyEdit = queuedFetch([
    () => json([]),
    () =>
      json({
        data: {
          repository: {
            issue: {
              editor: { login: "writer" },
              lastEditedAt: null,
              timelineItems: { totalCount: 0, nodes: [] },
              parent: null,
              blockedBy: { totalCount: 0 },
              blocking: { totalCount: 0 },
            },
          },
        },
      }),
  ]);
  await assert.rejects(
    () => githubClient(mismatchedBodyEdit.fetcher).getIssueRelations(1),
    /incomplete payload/,
  );

  const missingLatestTitleEdit = queuedFetch([
    () => json([]),
    () =>
      json({
        data: {
          repository: {
            issue: {
              editor: null,
              lastEditedAt: null,
              timelineItems: { totalCount: 1, nodes: [] },
              parent: null,
              blockedBy: { totalCount: 0 },
              blocking: { totalCount: 0 },
            },
          },
        },
      }),
  ]);
  await assert.rejects(
    () => githubClient(missingLatestTitleEdit.fetcher).getIssueRelations(1),
    /incomplete payload/,
  );
});

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

Deno.test("GitHub workflow polling tolerates a transient dispatch-title delay", async () => {
  let now = Date.parse("2026-08-21T12:00:00Z");
  const displayTitle = "Deno Deploy sentinel-test-correlation";
  const fake = queuedFetch([
    () => json(workflowRun(53, NEW_SHA, "in_progress", null, undefined, "Deno Deploy")),
    () => json(workflowRun(53, NEW_SHA, "completed", "success", undefined, displayTitle)),
  ]);
  const run = await githubClient(fake.fetcher, {
    now: () => now,
    sleep: (milliseconds) => {
      now += milliseconds;
      return Promise.resolve();
    },
  }).waitForWorkflow({
    runId: 53,
    headSha: NEW_SHA,
    displayTitle,
    timeoutMs: 2_000,
    pollIntervalMs: 1_000,
  });

  assert.equal(run.id, 53);
  assert.equal(run.displayTitle, displayTitle);
  assert.equal(now, Date.parse("2026-08-21T12:00:01Z"));
  fake.assertDrained();
});

Deno.test("GitHub workflow polling rejects a completed run with the wrong dispatch title", async () => {
  const now = Date.parse("2026-08-21T12:00:00Z");
  const fake = queuedFetch([
    () => json(workflowRun(54, NEW_SHA, "completed", "success", undefined, "Deno Deploy other-dispatch")),
  ]);
  await assert.rejects(
    () =>
      githubClient(fake.fetcher, { now: () => now }).waitForWorkflow({
        runId: 54,
        headSha: NEW_SHA,
        displayTitle: "Deno Deploy sentinel-test-correlation",
        timeoutMs: 1_000,
      }),
    /wrong dispatch correlation/,
  );
  fake.assertDrained();
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

Deno.test("GitHub matrix delivery pins the pull request head and merge method", async () => {
  const branch = "sentinel/candidate-123-1";
  const marker = "<!-- provider-sentinel-matrix:123:1 -->";
  const pull = {
    number: 91,
    html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/91",
    state: "open",
    merged: false,
    body: marker,
    head: { ref: branch, sha: NEW_SHA },
    base: { ref: "development" },
  };
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/pulls");
      assert.equal(request.url.searchParams.get("state"), "open");
      assert.equal(request.url.searchParams.get("base"), "development");
      assert.equal(request.url.searchParams.get("page"), "1");
      return json([]);
    },
    async (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/pulls", "POST");
      assert.deepEqual(JSON.parse(await new Response(request.body).text()), {
        title: "matrix repair",
        body: marker,
        head: branch,
        base: "development",
        draft: false,
        maintainer_can_modify: false,
      });
      return json(pull, 201);
    },
    async (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/pulls/91/merge", "PUT");
      assert.deepEqual(JSON.parse(await new Response(request.body).text()), {
        sha: NEW_SHA,
        merge_method: "merge",
        commit_title: "matrix repair",
      });
      return json({ merged: true, sha: WRONG_SHA, message: "Pull Request successfully merged" });
    },
  ]);
  const client = githubClient(fake.fetcher);
  assert.deepEqual(await client.listOpenPullRequests("development"), []);
  const created = await client.createPullRequest({
    title: "matrix repair",
    body: marker,
    head: branch,
    base: "development",
  });
  assert.equal(created.headSha, NEW_SHA);
  const merged = await client.mergePullRequest(created.number, NEW_SHA, "matrix repair");
  assert.deepEqual(merged, {
    merged: true,
    sha: WRONG_SHA,
    message: "Pull Request successfully merged",
  });
  fake.assertDrained();
});

Deno.test("GitHub pull-request listing is bounded, read-only, and inclusive of closed pull requests", async () => {
  const pull = (number: number, state: "open" | "closed", mergedAt: string | null) => ({
    number,
    html_url: `https://github.com/ubiquity/ai.ubq.fi/pull/${number}`,
    state,
    merged_at: mergedAt,
    body: "<!-- provider-sentinel-matrix:1:1 -->",
    head: { ref: `sentinel/candidate-${number}-${"1".repeat(8)}`, sha: NEW_SHA },
    base: { ref: "development" },
  });
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/pulls");
      assert.equal(request.url.searchParams.get("state"), "all");
      assert.equal(request.url.searchParams.get("per_page"), "100");
      assert.equal(request.url.searchParams.get("page"), "1");
      assert.equal(request.redirect, "manual");
      return json([pull(1, "open", null), pull(2, "closed", "2026-08-22T00:00:00Z")]);
    },
  ]);
  const listed = await githubClient(fake.fetcher).listPullRequests({ state: "all" });
  assert.deepEqual(
    listed.map((entry) => [entry.number, entry.state, entry.merged]),
    [[1, "open", false], [2, "closed", true]],
  );
  // Incomplete pull request data fails closed instead of being skipped, so the
  // review preselection can never silently under-count eligible work.
  const incomplete = queuedFetch([
    () => json([{ number: 1, state: "open", html_url: "https://github.com/x/y/pull/1" }]),
  ]);
  await assert.rejects(() => githubClient(incomplete.fetcher).listPullRequests(), /incomplete pull request/);
  fake.assertDrained();
  incomplete.assertDrained();
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

Deno.test("GitHub repository artifact listing can retain expired state for fail-closed restore", async () => {
  const fake = queuedFetch([
    (request) => {
      assertGitHubApiRequest(request, "/repos/ubiquity/ai.ubq.fi/actions/artifacts");
      return json({
        total_count: 2,
        artifacts: [{
          id: 301,
          name: "sentinel-codex-auth-state-v1-generation-r10-a1",
          size_in_bytes: 10,
          expired: true,
          created_at: "2026-08-21T00:00:00Z",
          expires_at: "2026-08-22T00:00:00Z",
        }, {
          id: 300,
          name: "sentinel-codex-auth-state-v1-generation-r9-a1",
          size_in_bytes: 10,
          expired: false,
          created_at: "2026-08-20T00:00:00Z",
          expires_at: "2026-11-18T00:00:00Z",
        }],
      });
    },
  ]);

  const artifacts = await githubClient(fake.fetcher).listRepositoryArtifacts({ includeExpired: true });
  assert.deepEqual(artifacts.map((artifact) => ({ id: artifact.id, expired: artifact.expired })), [
    { id: 301, expired: true },
    { id: 300, expired: false },
  ]);
  fake.assertDrained();
});

Deno.test("GitHub repository artifact listing rejects malformed expiration metadata", async () => {
  for (const expired of [undefined, null, "false", 0]) {
    const fake = queuedFetch([
      () =>
        json({
          total_count: 1,
          artifacts: [{
            id: 401,
            name: "sentinel-codex-auth-state-v1-generation-r11-a1",
            size_in_bytes: 10,
            ...(expired === undefined ? {} : { expired }),
            created_at: "2026-08-21T00:00:00Z",
            expires_at: "2026-11-19T00:00:00Z",
          }],
        }),
    ]);
    await assert.rejects(
      () => githubClient(fake.fetcher).listRepositoryArtifacts({ includeExpired: true }),
      /incomplete artifact/,
    );
    fake.assertDrained();
  }
});

Deno.test("Deno route ownership confirms when the active revision holds the managed route", async () => {
  const fake = queuedFetch([
    (request) => {
      assertDenoApiRequest(request, "/v2/apps/ai-ubq-fi/revisions");
      return json({ revisions: [{ id: "k3a2f3cpzra7", status: "succeeded" }] });
    },
    (request) => {
      assertDenoApiRequest(request, "/v2/revisions/k3a2f3cpzra7");
      return json({
        id: "k3a2f3cpzra7",
        status: "succeeded",
        timelines: [
          { name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"] },
          { name: "Preview", context: "Preview", hostnames: ["ai-ubq-fi-k3a2f3cpzra7.ubiquity-dao.deno.net"] },
        ],
      });
    },
  ]);
  const ownership = await denoClient(fake.fetcher, { now: () => Date.parse("2026-09-04T12:00:00Z") })
    .readProductionRouteOwnership("ai-ubq-fi", "k3a2f3cpzra7");

  assert.deepEqual(ownership, {
    app: "ai-ubq-fi",
    revisionId: "k3a2f3cpzra7",
    managedHostname: "ai-ubq-fi.ubiquity-dao.deno.net",
    ownsRoute: true,
    observedAt: "2026-09-04T12:00:00.000Z",
  });
  assert.equal(Object.isFrozen(ownership), true);
  assert.equal("gitSha" in ownership, false, "route ownership is not a Git SHA proof");
  assert.equal(fake.seen.length, 2, "route ownership must make only the membership and revision GETs");
  for (const request of fake.seen) {
    assert.equal(request.method, "GET", "route ownership must be read-only");
    assert.equal(request.redirect, "manual");
    assert.match(request.url.pathname, /^\/v2\/(?:apps|revisions)\//u);
    assert.equal(request.url.pathname.includes("health"), false, "route ownership never calls app health");
    assert.equal(request.url.pathname.includes("promote"), false, "route ownership never promotes");
  }
  fake.assertDrained();
});

Deno.test("Deno route ownership reports false for a shadowed revision with no production hostnames", async () => {
  const fake = queuedFetch([
    () => json({ revisions: [{ id: "hk95eyzdn3b2", status: "succeeded" }] }),
    () =>
      json({
        id: "hk95eyzdn3b2",
        status: "succeeded",
        timelines: [
          { name: "Production", context: "Production", hostnames: [] },
          { name: "Preview", context: "Preview", hostnames: ["ai-ubq-fi-hk95eyzdn3b2.ubiquity-dao.deno.net"] },
        ],
      }),
  ]);
  const ownership = await denoClient(fake.fetcher)
    .readProductionRouteOwnership("ai-ubq-fi", "hk95eyzdn3b2");
  assert.equal(ownership.managedHostname, "ai-ubq-fi.ubiquity-dao.deno.net");
  assert.equal(ownership.ownsRoute, false);
  fake.assertDrained();
});

Deno.test("Deno route ownership reports false when timelines omit Production", async () => {
  const emptyTimelines = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () => json({ id: "candidate-revision", status: "succeeded", timelines: [] }),
  ]);
  assert.equal(
    (await denoClient(emptyTimelines.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"))
      .ownsRoute,
    false,
  );
  emptyTimelines.assertDrained();

  const previewOnly = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () =>
      json({
        id: "candidate-revision",
        status: "succeeded",
        timelines: [{
          name: "Preview",
          context: "Preview",
          hostnames: ["ai-ubq-fi-candidate-revision.ubiquity-dao.deno.net"],
        }],
      }),
  ]);
  assert.equal(
    (await denoClient(previewOnly.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"))
      .ownsRoute,
    false,
  );
  previewOnly.assertDrained();
});

Deno.test("Deno route ownership derives the managed hostname from the configured organization", async () => {
  const fake = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () =>
      json({
        id: "candidate-revision",
        status: "succeeded",
        timelines: [{
          name: "Production",
          context: "Production",
          hostnames: ["ai-ubq-fi.acme-corp.deno.net"],
        }],
      }),
  ]);
  const client = new DenoDeployClient({
    token: DENO_TOKEN,
    apiBaseUrl: "https://deno.test/v2/",
    organization: "acme-corp",
    fetcher: fake.fetcher,
  });
  const ownership = await client.readProductionRouteOwnership("ai-ubq-fi", "candidate-revision");
  assert.equal(ownership.managedHostname, "ai-ubq-fi.acme-corp.deno.net");
  assert.equal(ownership.ownsRoute, true);
  fake.assertDrained();
});

Deno.test("Deno route ownership requires exact application membership before reading revision data", async () => {
  const fake = queuedFetch([() => json({ revisions: [{ id: "different-revision" }] })]);
  await assert.rejects(
    () => denoClient(fake.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
    /not a member/,
  );
  assert.equal(fake.seen.length, 1, "membership failure must stop before the revision GET");
  fake.assertDrained();
});

Deno.test("Deno route ownership rejects a revision payload for a different id", async () => {
  const fake = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () => json({ id: "different-revision", status: "succeeded", timelines: [] }),
  ]);
  await assert.rejects(
    () => denoClient(fake.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
    /wrong revision/,
  );
  fake.assertDrained();
});

Deno.test("Deno route ownership requires normalized routed control-plane status", async () => {
  for (const status of ["building", "failed", "ready"]) {
    const fake = queuedFetch([
      () => json({ revisions: [{ id: "candidate-revision" }] }),
      () => json({ id: "candidate-revision", status, timelines: [] }),
    ]);
    await assert.rejects(
      () => denoClient(fake.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
      /not routed on the Deno control plane/,
    );
    fake.assertDrained();
  }
});

Deno.test("Deno route ownership fails closed on malformed or missing control-plane timelines", async () => {
  const payloads: Record<string, unknown>[] = [
    { id: "candidate-revision", status: "succeeded" },
    { id: "candidate-revision", status: "succeeded", timelines: {} },
    { id: "candidate-revision", status: "succeeded", timelines: [null] },
    { id: "candidate-revision", status: "succeeded", timelines: [{ context: "Production", hostnames: [] }] },
    { id: "candidate-revision", status: "succeeded", timelines: [{ name: "", context: "Production", hostnames: [] }] },
    { id: "candidate-revision", status: "succeeded", timelines: [{ name: " ", context: "Production", hostnames: [] }] },
    { id: "candidate-revision", status: "succeeded", timelines: [{ name: "Production", hostnames: [] }] },
    { id: "candidate-revision", status: "succeeded", timelines: [{ name: "Production", context: "", hostnames: [] }] },
    { id: "candidate-revision", status: "succeeded", timelines: [{ name: "Production", context: "Production" }] },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: "ai-ubq-fi.ubiquity-dao.deno.net" }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: [""] }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: ["AI-UBQ-FI.ubiquity-dao.deno.net"] }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net."] }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net:443"] }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{
        name: "Production",
        context: "Production",
        hostnames: ["https://ai-ubq-fi.ubiquity-dao.deno.net"],
      }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net/health"] }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: ["*.ubiquity-dao.deno.net"] }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity_dao.deno.net"] }],
    },
    // Individual labels are bounded to 63 characters even when the whole
    // hostname is under 253 characters.
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{
        name: "Production",
        context: "Production",
        hostnames: [`${"a".repeat(64)}.ubiquity-dao.deno.net`],
      }],
    },
    {
      id: "candidate-revision",
      status: "succeeded",
      timelines: [{ name: "Production", context: "Production", hostnames: [`ai-ubq-fi.${"b".repeat(64)}.deno.net`] }],
    },
  ];
  for (const payload of payloads) {
    const fake = queuedFetch([
      () => json({ revisions: [{ id: "candidate-revision" }] }),
      () => json(payload),
    ]);
    await assert.rejects(
      () => denoClient(fake.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
      /Revision candidate-revision/,
    );
    fake.assertDrained();
  }
});

Deno.test("Deno route ownership rejects duplicate hostnames within one timeline", async () => {
  const fake = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () =>
      json({
        id: "candidate-revision",
        status: "succeeded",
        timelines: [{
          name: "Production",
          context: "Production",
          hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net", "ai-ubq-fi.ubiquity-dao.deno.net"],
        }],
      }),
  ]);
  await assert.rejects(
    () => denoClient(fake.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
    /duplicate hostnames/,
  );
  fake.assertDrained();
});

Deno.test("Deno route ownership rejects duplicate and partial Production timeline records", async () => {
  const cases: { timelines: unknown[]; error: RegExp }[] = [
    {
      timelines: [
        { name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"] },
        { name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"] },
      ],
      error: /multiple Production/,
    },
    {
      timelines: [{
        name: "Production",
        context: "Preview",
        hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"],
      }],
      error: /partial Production/,
    },
    {
      timelines: [{
        name: "Preview",
        context: "Production",
        hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"],
      }],
      error: /partial Production/,
    },
    // Padded names are not authoritative Production entries.
    {
      timelines: [{
        name: " Production ",
        context: "Production",
        hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"],
      }],
      error: /partial Production/,
    },
    {
      timelines: [{
        name: "Production",
        context: " Production ",
        hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"],
      }],
      error: /partial Production/,
    },
  ];
  for (const { timelines, error } of cases) {
    const fake = queuedFetch([
      () => json({ revisions: [{ id: "candidate-revision" }] }),
      () => json({ id: "candidate-revision", status: "succeeded", timelines }),
    ]);
    await assert.rejects(
      () => denoClient(fake.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
      error,
    );
    fake.assertDrained();
  }
});

Deno.test("Deno route ownership treats lookalike and immutable preview hostnames as non-owning", async () => {
  const lookalikeHostnames = [
    ["ai-ubq-fi-extra.ubiquity-dao.deno.net"],
    ["p-ai-ubq-fi.ubiquity-dao.deno.net"],
    ["ai-ubq-fi.k3a2f3cpzra7.ubiquity-dao.deno.net"],
    ["ai-ubq-fi-k3a2f3cpzra7.ubiquity-dao.deno.net"],
    ["ai-ubq-fi.ubiquity-dao.deno.net.evil.example"],
  ];
  for (const hostnames of lookalikeHostnames) {
    const fake = queuedFetch([
      () => json({ revisions: [{ id: "candidate-revision" }] }),
      () =>
        json({
          id: "candidate-revision",
          status: "succeeded",
          timelines: [{ name: "Production", context: "Production", hostnames }],
        }),
    ]);
    const ownership = await denoClient(fake.fetcher)
      .readProductionRouteOwnership("ai-ubq-fi", "candidate-revision");
    assert.equal(ownership.ownsRoute, false, `hostnames ${hostnames.join(",")} must not own the route`);
    fake.assertDrained();
  }
});

Deno.test("Deno route ownership rejects the managed hostname advertised outside Production", async () => {
  const previewOnly = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () =>
      json({
        id: "candidate-revision",
        status: "succeeded",
        timelines: [{
          name: "Preview",
          context: "Preview",
          hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"],
        }],
      }),
  ]);
  await assert.rejects(
    () => denoClient(previewOnly.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
    /outside Production/,
  );
  previewOnly.assertDrained();

  const contradictory = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () =>
      json({
        id: "candidate-revision",
        status: "succeeded",
        timelines: [
          { name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"] },
          { name: "Preview", context: "Preview", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"] },
        ],
      }),
  ]);
  await assert.rejects(
    () => denoClient(contradictory.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
    /outside Production/,
  );
  contradictory.assertDrained();

  const paddedBoth = queuedFetch([
    () => json({ revisions: [{ id: "candidate-revision" }] }),
    () =>
      json({
        id: "candidate-revision",
        status: "succeeded",
        timelines: [{
          name: " Production ",
          context: " Production ",
          hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"],
        }],
      }),
  ]);
  await assert.rejects(
    () => denoClient(paddedBoth.fetcher).readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
    /outside Production/,
  );
  paddedBoth.assertDrained();
});

Deno.test("Deno route ownership validates lowercase DNS label inputs before any request", async () => {
  const invalid: [string, string][] = [
    ["", "candidate-revision"],
    ["ai-ubq-fi", ""],
    ["AI-UBQ-FI", "candidate-revision"],
    ["ai-ubq-fi", "CANDIDATE-REVISION"],
    ["ai_ubq_fi", "candidate-revision"],
    ["ai-ubq-fi", "candidate_revision"],
    ["-ai-ubq-fi", "candidate-revision"],
    ["ai-ubq-fi", "-candidate-revision"],
    ["ai-ubq-fi-", "candidate-revision"],
    ["a".repeat(64), "candidate-revision"],
    ["ai-ubq-fi", "a".repeat(64)],
  ];
  for (const [app, revisionId] of invalid) {
    const fake = queuedFetch([]);
    await assert.rejects(
      () => denoClient(fake.fetcher).readProductionRouteOwnership(app, revisionId),
      /lowercase DNS label/,
    );
    assert.equal(fake.seen.length, 0, "invalid input must fail before any control-plane request");
    fake.assertDrained();
  }
});

Deno.test("Deno route ownership rejects an unrepresentable observation time", async () => {
  for (const now of [Number.NaN, Number.POSITIVE_INFINITY]) {
    const fake = queuedFetch([
      () => json({ revisions: [{ id: "candidate-revision" }] }),
      () =>
        json({
          id: "candidate-revision",
          status: "succeeded",
          timelines: [{ name: "Production", context: "Production", hostnames: ["ai-ubq-fi.ubiquity-dao.deno.net"] }],
        }),
    ]);
    await assert.rejects(
      () =>
        denoClient(fake.fetcher, { now: () => now })
          .readProductionRouteOwnership("ai-ubq-fi", "candidate-revision"),
      /not representable/,
    );
    fake.assertDrained();
  }
});
