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

export type CustomHealthAttestation =
  | Readonly<{ kind: "identity"; identity: HealthIdentity }>
  | Readonly<{ kind: "cloudflare_challenge"; url: string; status: 403; ray: string }>;

export type ProductionHealthAttestation = Readonly<{
  managed: HealthIdentity;
  custom: CustomHealthAttestation;
}>;

export interface ProductionRouteOwnership {
  readonly app: string;
  readonly revisionId: string;
  readonly managedHostname: string;
  readonly ownsRoute: boolean;
  readonly observedAt: string;
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
const MAX_REVISION_CURSOR_LENGTH = 4_096;
const FULL_GIT_SHA = /^[0-9a-f]{40}$/;
const DNS_LABEL = /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/u;
const DNS_HOSTNAME = /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*$/u;
const DNS_LABEL_MAX_LENGTH = 63;
const DNS_HOSTNAME_MAX_LENGTH = 253;

class HealthIdentityMismatchError extends Error {
  constructor(url: string, expectedGitSha: string, expectedRevisionId: string) {
    super(`Health identity mismatch at ${url}: expected ${expectedGitSha}/${expectedRevisionId}`);
    this.name = "HealthIdentityMismatchError";
  }
}

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

const requireDnsLabel = (value: unknown, label: string): string => {
  if (typeof value !== "string" || value.length > DNS_LABEL_MAX_LENGTH || !DNS_LABEL.test(value)) {
    throw new Error(`${label} must be a nonempty lowercase DNS label`);
  }
  return value;
};

const isDnsHostname = (value: string): boolean => {
  if (value.length > DNS_HOSTNAME_MAX_LENGTH || !DNS_HOSTNAME.test(value)) return false;
  return value.split(".").every((label) => label.length <= DNS_LABEL_MAX_LENGTH);
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

const nextRevisionPageUrl = (target: string, currentPageUrl: URL, initialUrl: URL): URL => {
  const candidate = new URL(target, currentPageUrl);
  const sameEndpoint = candidate.origin === initialUrl.origin && candidate.pathname === initialUrl.pathname;
  const isDefaultDenoApi = initialUrl.origin === new URL(DEFAULT_DENO_API_BASE_URL).origin;
  const knownConsoleAlias = isDefaultDenoApi &&
    candidate.origin === "https://console.deno.com" &&
    candidate.pathname === `/api${initialUrl.pathname}`;
  const cursors = candidate.searchParams.getAll("cursor");
  const limits = candidate.searchParams.getAll("limit");
  const hasOnlyExpectedParameters = [...candidate.searchParams.keys()].every((key) =>
    key === "cursor" || key === "limit"
  );
  const cursor = cursors[0] ?? "";
  if (
    (!sameEndpoint && !knownConsoleAlias) || candidate.username !== "" || candidate.password !== "" ||
    candidate.hash !== "" || !hasOnlyExpectedParameters || cursors.length !== 1 || cursor === "" ||
    cursor.length > MAX_REVISION_CURSOR_LENGTH || limits.length > 1 ||
    (limits.length === 1 && limits[0] !== String(REVISION_PAGE_LIMIT))
  ) {
    throw new Error("Deno revision list returned an unsafe next-page link");
  }
  const nextPage = new URL(initialUrl);
  nextPage.searchParams.set("cursor", cursor);
  return nextPage;
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

interface RouteTimelineEntry {
  readonly name: string;
  readonly context: string;
  readonly hostnames: readonly string[];
}

const parseRouteTimelineEntry = (value: unknown, revisionId: string): RouteTimelineEntry => {
  const entry = asRecord(value);
  if (!entry) throw new Error(`Revision ${revisionId} has a malformed timeline entry`);
  // Exact control-plane strings: a padded " Production " is not an
  // authoritative Production entry and must stay distinguishable.
  const name = entry.name;
  const context = entry.context;
  if (
    typeof name !== "string" || name.trim() === "" ||
    typeof context !== "string" || context.trim() === ""
  ) {
    throw new Error(`Revision ${revisionId} has an incomplete timeline entry`);
  }
  if (!Array.isArray(entry.hostnames)) {
    throw new Error(`Revision ${revisionId} timeline ${name} has no hostnames array`);
  }
  const hostnames: string[] = [];
  for (const hostname of entry.hostnames) {
    if (typeof hostname !== "string" || !isDnsHostname(hostname)) {
      throw new Error(`Revision ${revisionId} timeline ${name} has an invalid hostname`);
    }
    hostnames.push(hostname);
  }
  if (hostnames.length !== new Set(hostnames).size) {
    throw new Error(`Revision ${revisionId} timeline ${name} has duplicate hostnames`);
  }
  return Object.freeze({ name, context, hostnames: Object.freeze([...hostnames]) });
};

const ownsManagedRoute = (
  raw: Readonly<Record<string, unknown>>,
  revisionId: string,
  managedHostname: string,
): boolean => {
  const timelines = raw.timelines;
  if (!Array.isArray(timelines)) {
    throw new Error(`Revision ${revisionId} has malformed control-plane timelines`);
  }
  let production: RouteTimelineEntry | null = null;
  for (const value of timelines) {
    const entry = parseRouteTimelineEntry(value, revisionId);
    const productionName = entry.name === "Production";
    const productionContext = entry.context === "Production";
    if (productionName !== productionContext) {
      throw new Error(`Revision ${revisionId} has a partial Production timeline entry`);
    }
    if (productionName && productionContext) {
      if (production !== null) {
        throw new Error(`Revision ${revisionId} has multiple Production timeline entries`);
      }
      production = entry;
    } else if (entry.hostnames.includes(managedHostname)) {
      throw new Error(
        `Revision ${revisionId} advertises the managed route ${managedHostname} outside Production`,
      );
    }
  }
  return production?.hostnames.includes(managedHostname) ?? false;
};

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
      let nextPage: URL;
      try {
        nextPage = nextRevisionPageUrl(target, pageUrl, initialUrl);
      } catch {
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

  async #requestHealth(
    url: string,
    timeoutMs: number,
  ): Promise<Readonly<{ response: Response; signal: AbortSignal; operation: string }>> {
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
    return { response, signal, operation };
  }

  async #parseHealthIdentity(
    url: string,
    response: Response,
    signal: AbortSignal,
    operation: string,
  ): Promise<HealthIdentity> {
    if (response.status !== 200) {
      throw new Error(`${operation} failed with HTTP ${response.status}; expected 200`);
    }
    const payload = await responseJson(response, operation, signal, false);
    const release = asRecord(asRecord(payload)?.release);
    const bodyGitSha = nonEmptyString(release?.git_sha);
    const bodyRevisionId = nonEmptyString(release?.deployment_id);

    const headerGitSha = response.headers.get("x-uos-git-sha")?.trim();
    const headerRevisionId = response.headers.get("x-uos-deployment-id")?.trim();
    if (!bodyGitSha || !bodyRevisionId || !headerGitSha || !headerRevisionId) {
      throw new Error(`Health at ${url} did not report release identity in both its body and headers`);
    }
    if (headerGitSha !== bodyGitSha) {
      throw new Error(`Health at ${url} reported inconsistent Git identities`);
    }
    if (headerRevisionId !== bodyRevisionId) {
      throw new Error(`Health at ${url} reported inconsistent revision identities`);
    }
    return { url, gitSha: bodyGitSha, revisionId: bodyRevisionId };
  }

  async #readHealth(url: string, timeoutMs: number): Promise<HealthIdentity> {
    const { response, signal, operation } = await this.#requestHealth(url, timeoutMs);
    return await this.#parseHealthIdentity(url, response, signal, operation);
  }

  async #readCustomHealth(url: string, timeoutMs: number): Promise<CustomHealthAttestation> {
    const { response, signal, operation } = await this.#requestHealth(url, timeoutMs);
    if (response.status === 200) {
      return { kind: "identity", identity: await this.#parseHealthIdentity(url, response, signal, operation) };
    }
    const server = response.headers.get("server")?.trim().toLowerCase();
    const mitigation = response.headers.get("cf-mitigated")?.trim().toLowerCase();
    const ray = response.headers.get("cf-ray")?.trim() ?? "";
    if (response.status === 403 && server === "cloudflare" && mitigation === "challenge" && ray !== "") {
      await response.body?.cancel().catch(() => undefined);
      return { kind: "cloudflare_challenge", url, status: 403, ray };
    }
    await response.body?.cancel().catch(() => undefined);
    throw new Error(`${operation} failed with HTTP ${response.status}; expected 200`);
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
          throw new HealthIdentityMismatchError(identity.url, expectedGitSha, expectedRevisionId);
        }
      }
    }, options);
  }

  async #pollCustomHealthIdentity(
    url: string,
    expectedGitSha: string,
    expectedRevisionId: string,
    options: HealthIdentityPollOptions,
  ): Promise<CustomHealthAttestation> {
    const timeoutMs = positiveMilliseconds(
      options.timeoutMs ?? DEFAULT_HEALTH_IDENTITY_TIMEOUT_MS,
      "Custom health identity timeout",
    );
    const pollIntervalMs = positiveMilliseconds(
      options.pollIntervalMs ?? DEFAULT_HEALTH_IDENTITY_POLL_INTERVAL_MS,
      "Custom health identity poll interval",
    );
    const deadline = this.#now() + timeoutMs;
    const maximumAttempts = Math.ceil(timeoutMs / pollIntervalMs) + 1;
    let lastError: unknown = new Error("Custom health identity did not converge");
    for (let attempt = 0; attempt < maximumAttempts; attempt++) {
      const remainingBeforeRequest = Math.max(1, deadline - this.#now());
      try {
        const attestation = await this.#readCustomHealth(
          url,
          Math.min(this.#requestTimeoutMs, remainingBeforeRequest),
        );
        if (attestation.kind === "cloudflare_challenge") return attestation;
        if (
          attestation.identity.gitSha !== expectedGitSha ||
          attestation.identity.revisionId !== expectedRevisionId
        ) {
          throw new HealthIdentityMismatchError(url, expectedGitSha, expectedRevisionId);
        }
        return attestation;
      } catch (error) {
        if (error instanceof HealthIdentityMismatchError) throw error;
        lastError = error;
      }
      const remaining = deadline - this.#now();
      if (remaining <= 0 || attempt + 1 >= maximumAttempts) break;
      await this.#sleep(Math.min(pollIntervalMs, remaining));
      if (this.#now() >= deadline) break;
    }
    if (lastError instanceof Error) throw lastError;
    throw new Error("Custom health identity did not converge");
  }

  async verifyProductionHealthIdentity(
    managedUrl: string,
    customUrl: string,
    expectedGitSha: string,
    expectedRevisionId: string,
    options: HealthIdentityPollOptions = {},
  ): Promise<ProductionHealthAttestation> {
    const managed = (await this.verifyHealthIdentity(
      [managedUrl],
      expectedGitSha,
      expectedRevisionId,
      options,
    ))[0]!;
    const custom = await this.#pollCustomHealthIdentity(
      customUrl,
      expectedGitSha,
      expectedRevisionId,
      options,
    );
    return { managed, custom };
  }

  async snapshotHealthyProduction(
    app: string,
    healthUrls: readonly string[],
    healthOptions: HealthIdentityPollOptions = {},
  ): Promise<RollbackTarget> {
    if (healthUrls.length === 0 || healthUrls.length > 2) {
      throw new Error("A production snapshot requires one or two health URLs");
    }
    const managedUrls = healthUrls.length === 2 ? [healthUrls[0]!] : healthUrls;
    const identities = await this.#pollHealthIdentities(managedUrls, (observed) => {
      const first = observed[0];
      requireGitSha(first.gitSha, "Production Git SHA");
      for (const identity of observed.slice(1)) {
        if (identity.gitSha !== first.gitSha || identity.revisionId !== first.revisionId) {
          throw new Error("Production health URLs do not agree on one rollback identity");
        }
      }
    }, healthOptions);
    const first = identities[0];
    if (healthUrls.length === 2) {
      await this.#pollCustomHealthIdentity(
        healthUrls[1]!,
        first.gitSha,
        first.revisionId,
        healthOptions,
      );
    }
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
    const revision = await this.getRevision(revisionId);
    if (revision.id !== revisionId) {
      throw new Error(`Deno returned the wrong revision for ${revisionId}`);
    }
    if (revision.status !== "routed") {
      throw new Error(`Revision ${revisionId} is not routed immediately before promotion`);
    }
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

  /**
   * Reads read-only control-plane route ownership evidence for one exact
   * revision: whether that revision currently holds the app's stable managed
   * hostname in its Production timeline. Routing evidence only — never a
   * healthy attestation and never a Git SHA proof.
   */
  async readProductionRouteOwnership(
    app: string,
    revisionId: string,
  ): Promise<ProductionRouteOwnership> {
    const routeApp = requireDnsLabel(app, "Deno application name");
    const routeRevisionId = requireDnsLabel(revisionId, "Deno revision ID");
    const managedHostname = `${routeApp}.${this.#organization}.deno.net`;
    await this.assertRevisionBelongsToApp(routeApp, routeRevisionId);
    const revision = await this.getRevision(routeRevisionId);
    if (revision.id !== routeRevisionId) {
      throw new Error(`Deno returned the wrong revision for ${routeRevisionId}`);
    }
    if (revision.status !== "routed") {
      throw new Error(`Revision ${routeRevisionId} is not routed on the Deno control plane`);
    }
    const ownsRoute = ownsManagedRoute(revision.raw, routeRevisionId, managedHostname);
    const observedAtMs = this.#now();
    const observedAt = new Date(observedAtMs);
    if (!Number.isFinite(observedAtMs) || Number.isNaN(observedAt.getTime())) {
      throw new Error("Route ownership observation time is not representable");
    }
    return Object.freeze({
      app: routeApp,
      revisionId: routeRevisionId,
      managedHostname,
      ownsRoute,
      observedAt: observedAt.toISOString(),
    });
  }
}
