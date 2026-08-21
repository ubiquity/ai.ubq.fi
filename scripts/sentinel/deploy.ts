export type SentinelFetch = (
  input: string | URL | Request,
  init?: RequestInit,
) => Promise<Response>;

export type RevisionRouteStatus = "routed" | "pending" | "failed" | "unknown";

export interface DenoRevision {
  readonly id: string;
  readonly status: RevisionRouteStatus;
  readonly sourceStatus: string;
  readonly raw: Readonly<Record<string, unknown>>;
}

export interface HealthIdentity {
  readonly url: string;
  readonly gitSha: string;
  readonly revisionId: string;
}

export interface RollbackTarget {
  readonly gitSha: string;
  readonly revisionId: string;
  readonly healthUrls: readonly string[];
  readonly snapshottedAt: string;
}

export interface DenoDeployClientOptions {
  readonly token: string;
  readonly fetcher?: SentinelFetch;
  readonly apiBaseUrl?: string;
  readonly organization?: string;
  readonly sleep?: (milliseconds: number) => Promise<void>;
  readonly now?: () => number;
  readonly requestTimeoutMs?: number;
  readonly createTimeoutSignal?: (milliseconds: number) => AbortSignal;
}

export interface CandidateRevisionOptions {
  readonly app: string;
  readonly previousRevisionIds: ReadonlySet<string>;
  readonly candidateGitSha: string;
  readonly timeoutMs?: number;
  readonly pollIntervalMs?: number;
  readonly revisionHealthUrl?: (app: string, revisionId: string) => string;
}

export interface HealthIdentityPollOptions {
  readonly timeoutMs?: number;
  readonly pollIntervalMs?: number;
}

const DEFAULT_DENO_API_BASE_URL = "https://api.deno.com/v2/";
const DEFAULT_DENO_ORGANIZATION = "ubiquity-dao";
const DEFAULT_CANDIDATE_TIMEOUT_MS = 20 * 60 * 1000;
const DEFAULT_POLL_INTERVAL_MS = 10_000;
const DEFAULT_REQUEST_TIMEOUT_MS = 20_000;
const DEFAULT_HEALTH_IDENTITY_TIMEOUT_MS = 30_000;
const DEFAULT_HEALTH_IDENTITY_POLL_INTERVAL_MS = 2_000;
const REVISION_PAGE_LIMIT = 100;
const MAX_REVISION_PAGES = 1_000;
const FULL_GIT_SHA = /^[0-9a-f]{40}$/;

const defaultSleep = (milliseconds: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

const asRecord = (value: unknown): Record<string, unknown> | null =>
  value !== null && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : null;

const nonEmptyString = (value: unknown): string | null =>
  typeof value === "string" && value.trim() !== "" ? value.trim() : null;

const requireGitSha = (gitSha: string, label: string): void => {
  if (!FULL_GIT_SHA.test(gitSha)) {
    throw new Error(`${label} must be a lowercase, full Git commit SHA`);
  }
};

const positiveMilliseconds = (value: number, label: string): number => {
  if (!Number.isSafeInteger(value) || value <= 0) throw new Error(`${label} must be a positive integer`);
  return value;
};

const isTimeoutError = (error: unknown, signal: AbortSignal): boolean =>
  signal.aborted ||
  (error instanceof DOMException && (error.name === "AbortError" || error.name === "TimeoutError")) ||
  (error instanceof Error && (error.name === "AbortError" || error.name === "TimeoutError"));

const responseJson = async (
  response: Response,
  operation: string,
  signal: AbortSignal,
  requireSuccess = true,
): Promise<unknown> => {
  if (requireSuccess && !response.ok) {
    throw new Error(`${operation} failed with HTTP ${response.status}`);
  }
  try {
    return await response.json();
  } catch (error) {
    if (isTimeoutError(error, signal)) throw new Error(`${operation} timed out`);
    throw new Error(`${operation} returned invalid JSON`);
  }
};

const extractCollection = (value: unknown, label: string): unknown[] => {
  if (Array.isArray(value)) return value;
  const record = asRecord(value);
  if (record) {
    for (const key of ["revisions", "data", "items"]) {
      if (Array.isArray(record[key])) return record[key];
    }
  }
  throw new Error(`${label} did not contain a revision list`);
};

const nextLinkTarget = (header: string | null): string | null => {
  if (header === null || header.trim() === "") return null;
  const targets: string[] = [];
  for (const part of header.split(/,(?=\s*<)/u)) {
    const match = part.match(/^\s*<([^>]+)>(.*)$/u);
    if (!match) continue;
    const relations = match[2]
      .split(";")
      .map((parameter) => parameter.trim().match(/^rel\s*=\s*"?([^";]+)"?$/iu)?.[1] ?? "")
      .flatMap((value) => value.toLowerCase().split(/\s+/u))
      .filter(Boolean);
    if (relations.includes("next")) targets.push(match[1]);
  }
  if (targets.length > 1) throw new Error("Deno revision list returned more than one next-page link");
  return targets[0] ?? null;
};

const revisionIdFrom = (record: Record<string, unknown>): string | null =>
  nonEmptyString(record.id) ?? nonEmptyString(record.revision_id) ?? nonEmptyString(record.uid);

const revisionStatusFrom = (record: Record<string, unknown>): string =>
  nonEmptyString(record.status) ?? nonEmptyString(record.state) ?? "unknown";

export const normalizeRevisionStatus = (status: string): RevisionRouteStatus => {
  switch (status.trim().toLowerCase()) {
    case "routed":
    case "succeeded":
      return "routed";
    case "failed":
    case "error":
    case "cancelled":
    case "canceled":
      return "failed";
    case "created":
    case "queued":
    case "pending":
    case "building":
    case "provisioning":
    case "deploying":
      return "pending";
    default:
      return "unknown";
  }
};

const parseRevision = (value: unknown, operation: string): DenoRevision => {
  const outer = asRecord(value);
  const record = asRecord(outer?.revision) ?? asRecord(outer?.data) ?? outer;
  if (!record) throw new Error(`${operation} returned an invalid revision`);
  const id = revisionIdFrom(record);
  if (!id) throw new Error(`${operation} returned a revision without an ID`);
  const sourceStatus = revisionStatusFrom(record);
  return {
    id,
    status: normalizeRevisionStatus(sourceStatus),
    sourceStatus,
    raw: record,
  };
};

export const defaultRevisionBaseUrl = (
  app: string,
  revisionId: string,
  organization = DEFAULT_DENO_ORGANIZATION,
): string => `https://${app}-${revisionId}.${organization}.deno.net`;

export const defaultRevisionHealthUrl = (
  app: string,
  revisionId: string,
  organization = DEFAULT_DENO_ORGANIZATION,
): string => `${defaultRevisionBaseUrl(app, revisionId, organization)}/health`;

export class DenoDeployClient {
  readonly #token: string;
  readonly #fetcher: SentinelFetch;
  readonly #apiBaseUrl: URL;
  readonly #organization: string;
  readonly #sleep: (milliseconds: number) => Promise<void>;
  readonly #now: () => number;
  readonly #requestTimeoutMs: number;
  readonly #createTimeoutSignal: (milliseconds: number) => AbortSignal;

  constructor(options: DenoDeployClientOptions) {
    if (options.token.trim() === "") throw new Error("A Deno deployment token is required");
    this.#token = options.token;
    this.#fetcher = options.fetcher ?? fetch;
    this.#apiBaseUrl = new URL(options.apiBaseUrl ?? DEFAULT_DENO_API_BASE_URL);
    this.#organization = options.organization ?? DEFAULT_DENO_ORGANIZATION;
    this.#sleep = options.sleep ?? defaultSleep;
    this.#now = options.now ?? Date.now;
    this.#requestTimeoutMs = positiveMilliseconds(
      options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      "Deno request timeout",
    );
    this.#createTimeoutSignal = options.createTimeoutSignal ?? AbortSignal.timeout;
  }

  #apiUrl(path: string): URL {
    return new URL(path.replace(/^\//, ""), this.#apiBaseUrl);
  }

  #authorizationHeaders(acceptJson = true): Headers {
    const headers = new Headers({ Authorization: `Bearer ${this.#token}` });
    if (acceptJson) headers.set("Accept", "application/json");
    return headers;
  }

  async #request(
    input: string | URL | Request,
    init: RequestInit,
    operation: string,
    timeoutMs = this.#requestTimeoutMs,
  ): Promise<Readonly<{ response: Response; signal: AbortSignal }>> {
    const signal = this.#createTimeoutSignal(positiveMilliseconds(timeoutMs, `${operation} timeout`));
    try {
      return { response: await this.#fetcher(input, { ...init, signal }), signal };
    } catch (error) {
      if (isTimeoutError(error, signal)) throw new Error(`${operation} timed out`);
      throw new Error(`${operation} request failed`);
    }
  }

  async listRevisions(app: string): Promise<readonly DenoRevision[]> {
    if (app.trim() === "") throw new Error("A Deno application name is required");
    const initialUrl = this.#apiUrl(`apps/${encodeURIComponent(app)}/revisions`);
    initialUrl.searchParams.set("limit", String(REVISION_PAGE_LIMIT));
    const expectedOrigin = initialUrl.origin;
    const expectedPath = initialUrl.pathname;
    const visited = new Set<string>();
    const revisions = new Map<string, DenoRevision>();
    let pageUrl: URL | null = initialUrl;
    for (let page = 0; page < MAX_REVISION_PAGES && pageUrl !== null; page++) {
      if (visited.has(pageUrl.href)) throw new Error(`List revisions for ${app} returned a pagination loop`);
      visited.add(pageUrl.href);
      const operation = `List revisions for ${app}`;
      const { response, signal } = await this.#request(pageUrl, {
        method: "GET",
        headers: this.#authorizationHeaders(),
        redirect: "manual",
      }, operation);
      const payload = await responseJson(response, operation, signal);
      for (const value of extractCollection(payload, operation)) {
        const revision = parseRevision(value, operation);
        revisions.set(revision.id, revision);
      }
      const target = nextLinkTarget(response.headers.get("Link"));
      if (target === null) return [...revisions.values()];
      const nextPage: URL = new URL(target, pageUrl);
      if (nextPage.origin !== expectedOrigin || nextPage.pathname !== expectedPath) {
        throw new Error(`List revisions for ${app} returned an unsafe next-page link`);
      }
      pageUrl = nextPage;
    }
    throw new Error(`List revisions for ${app} exceeded the pagination limit`);
  }

  async getRevision(revisionId: string): Promise<DenoRevision> {
    if (revisionId.trim() === "") throw new Error("A Deno revision ID is required");
    const operation = `Get revision ${revisionId}`;
    const { response, signal } = await this.#request(
      this.#apiUrl(`revisions/${encodeURIComponent(revisionId)}`),
      {
        method: "GET",
        headers: this.#authorizationHeaders(),
        redirect: "manual",
      },
      operation,
    );
    return parseRevision(
      await responseJson(response, operation, signal),
      operation,
    );
  }

  async assertRevisionBelongsToApp(app: string, revisionId: string): Promise<DenoRevision> {
    const revision = (await this.listRevisions(app)).find((candidate) => candidate.id === revisionId);
    if (!revision) {
      throw new Error(`Revision ${revisionId} is not a member of Deno application ${app}`);
    }
    return revision;
  }

  async #readHealth(url: string, timeoutMs: number): Promise<HealthIdentity> {
    const operation = `Read health at ${url}`;
    const { response, signal } = await this.#request(
      url,
      {
        method: "GET",
        headers: { Accept: "application/json" },
        redirect: "manual",
        cache: "no-store",
      },
      operation,
      timeoutMs,
    );
    if (response.status !== 200) {
      throw new Error(`${operation} failed with HTTP ${response.status}; expected 200`);
    }
    const payload = await responseJson(response, operation, signal, false);
    const release = asRecord(asRecord(payload)?.release);
    const bodyGitSha = nonEmptyString(release?.git_sha);
    const bodyRevisionId = nonEmptyString(release?.deployment_id);

    const headerGitSha = response.headers.get("x-uos-git-sha")?.trim();
    const headerRevisionId = response.headers.get("x-uos-deployment-id")?.trim();
    if (bodyGitSha && headerGitSha && headerGitSha !== bodyGitSha) {
      throw new Error(`Health at ${url} reported inconsistent Git identities`);
    }
    if (bodyRevisionId && headerRevisionId && headerRevisionId !== bodyRevisionId) {
      throw new Error(`Health at ${url} reported inconsistent revision identities`);
    }
    const gitSha = bodyGitSha ?? headerGitSha ?? null;
    const revisionId = bodyRevisionId ?? headerRevisionId ?? null;
    if (!gitSha || !revisionId) throw new Error(`Health at ${url} did not report a complete release identity`);
    return { url, gitSha, revisionId };
  }

  async readHealth(url: string): Promise<HealthIdentity> {
    return await this.#readHealth(url, this.#requestTimeoutMs);
  }

  async #pollHealthIdentities(
    urls: readonly string[],
    validate: (identities: readonly HealthIdentity[]) => void,
    options: HealthIdentityPollOptions,
  ): Promise<readonly HealthIdentity[]> {
    const timeoutMs = positiveMilliseconds(
      options.timeoutMs ?? DEFAULT_HEALTH_IDENTITY_TIMEOUT_MS,
      "Health identity timeout",
    );
    const pollIntervalMs = positiveMilliseconds(
      options.pollIntervalMs ?? DEFAULT_HEALTH_IDENTITY_POLL_INTERVAL_MS,
      "Health identity poll interval",
    );
    const deadline = this.#now() + timeoutMs;
    const maximumAttempts = Math.ceil(timeoutMs / pollIntervalMs) + 1;
    let lastError: unknown = new Error("Health identity did not converge");
    for (let attempt = 0; attempt < maximumAttempts; attempt++) {
      const remainingBeforeRequest = Math.max(1, deadline - this.#now());
      try {
        const identities = await Promise.all(
          urls.map((url) => this.#readHealth(url, Math.min(this.#requestTimeoutMs, remainingBeforeRequest))),
        );
        validate(identities);
        return identities;
      } catch (error) {
        lastError = error;
      }
      const remaining = deadline - this.#now();
      if (remaining <= 0 || attempt + 1 >= maximumAttempts) break;
      await this.#sleep(Math.min(pollIntervalMs, remaining));
      if (this.#now() >= deadline) break;
    }
    if (lastError instanceof Error) throw lastError;
    throw new Error("Health identity did not converge");
  }

  async verifyHealthIdentity(
    urls: readonly string[],
    expectedGitSha: string,
    expectedRevisionId: string,
    options: HealthIdentityPollOptions = {},
  ): Promise<readonly HealthIdentity[]> {
    if (urls.length === 0 || urls.length > 2) {
      throw new Error("Health verification requires one or two URLs");
    }
    requireGitSha(expectedGitSha, "Expected Git SHA");
    if (!expectedRevisionId.trim()) throw new Error("Expected revision ID is required");
    return await this.#pollHealthIdentities(urls, (identities) => {
      for (const identity of identities) {
        if (identity.gitSha !== expectedGitSha || identity.revisionId !== expectedRevisionId) {
          throw new Error(
            `Health identity mismatch at ${identity.url}: expected ${expectedGitSha}/${expectedRevisionId}`,
          );
        }
      }
    }, options);
  }

  async snapshotHealthyProduction(
    app: string,
    healthUrls: readonly string[],
    healthOptions: HealthIdentityPollOptions = {},
  ): Promise<RollbackTarget> {
    if (healthUrls.length === 0 || healthUrls.length > 2) {
      throw new Error("A production snapshot requires one or two health URLs");
    }
    const identities = await this.#pollHealthIdentities(healthUrls, (observed) => {
      const first = observed[0];
      requireGitSha(first.gitSha, "Production Git SHA");
      for (const identity of observed.slice(1)) {
        if (identity.gitSha !== first.gitSha || identity.revisionId !== first.revisionId) {
          throw new Error("Production health URLs do not agree on one rollback identity");
        }
      }
    }, healthOptions);
    const first = identities[0];
    await this.assertRevisionBelongsToApp(app, first.revisionId);
    const revision = await this.getRevision(first.revisionId);
    if (revision.id !== first.revisionId) {
      throw new Error(`Deno returned the wrong revision for ${first.revisionId}`);
    }
    if (revision.status !== "routed") {
      throw new Error(`Production rollback revision ${first.revisionId} is not routed`);
    }
    return Object.freeze({
      gitSha: first.gitSha,
      revisionId: first.revisionId,
      healthUrls: Object.freeze([...healthUrls]),
      snapshottedAt: new Date(this.#now()).toISOString(),
    });
  }

  async resolveNewCandidateRevision(
    options: Omit<CandidateRevisionOptions, "timeoutMs" | "pollIntervalMs">,
  ): Promise<DenoRevision | null> {
    requireGitSha(options.candidateGitSha, "Candidate Git SHA");
    const revisionHealthUrl = options.revisionHealthUrl ??
      ((app: string, revisionId: string) => defaultRevisionHealthUrl(app, revisionId, this.#organization));
    const listed = await this.listRevisions(options.app);
    const candidates = listed.filter((revision) => !options.previousRevisionIds.has(revision.id));

    for (const listedRevision of candidates) {
      const revision = await this.getRevision(listedRevision.id);
      if (revision.id !== listedRevision.id) {
        throw new Error(`Deno returned the wrong revision for ${listedRevision.id}`);
      }
      if (revision.status !== "routed") continue;
      let identity: HealthIdentity;
      try {
        identity = await this.readHealth(revisionHealthUrl(options.app, revision.id));
      } catch {
        continue;
      }
      if (identity.gitSha === options.candidateGitSha && identity.revisionId === revision.id) {
        return revision;
      }
    }
    return null;
  }

  async waitForNewCandidateRevision(options: CandidateRevisionOptions): Promise<DenoRevision> {
    const timeoutMs = options.timeoutMs ?? DEFAULT_CANDIDATE_TIMEOUT_MS;
    const pollIntervalMs = options.pollIntervalMs ?? DEFAULT_POLL_INTERVAL_MS;
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) throw new Error("Candidate timeout must be positive");
    if (!Number.isFinite(pollIntervalMs) || pollIntervalMs <= 0) {
      throw new Error("Candidate poll interval must be positive");
    }
    const deadline = this.#now() + timeoutMs;
    while (this.#now() <= deadline) {
      const revision = await this.resolveNewCandidateRevision(options);
      if (revision) return revision;
      if (this.#now() >= deadline) break;
      await this.#sleep(Math.min(pollIntervalMs, Math.max(0, deadline - this.#now())));
    }
    throw new Error(`No new routed revision for ${options.candidateGitSha} reached the expected health identity`);
  }

  async promoteRevision(app: string, revisionId: string): Promise<void> {
    await this.assertRevisionBelongsToApp(app, revisionId);
    const operation = `Promote revision ${revisionId}`;
    const { response } = await this.#request(
      this.#apiUrl(`revisions/${encodeURIComponent(revisionId)}/promote`),
      {
        method: "POST",
        headers: this.#authorizationHeaders(false),
        redirect: "manual",
      },
      operation,
    );
    if (response.status !== 204) {
      throw new Error(`Promote revision ${revisionId} failed with HTTP ${response.status}; expected 204`);
    }
  }
}
