export type GitHubFetch = (
  input: string | URL | Request,
  init?: RequestInit,
) => Promise<Response>;

export interface GitHubWorkflowRun {
  readonly id: number;
  readonly headSha: string;
  readonly status: string;
  readonly conclusion: string | null;
  readonly htmlUrl: string | null;
  readonly createdAt: string;
  readonly displayTitle: string;
}

export interface GitHubArtifact {
  readonly id: number;
  readonly name: string;
  readonly sizeInBytes: number;
  readonly expired: boolean;
  readonly createdAt: string;
  readonly expiresAt: string | null;
}

export interface GitHubClientOptions {
  readonly repository: string;
  readonly token: string;
  readonly fetcher?: GitHubFetch;
  readonly apiBaseUrl?: string;
  readonly sleep?: (milliseconds: number) => Promise<void>;
  readonly now?: () => number;
  readonly requestTimeoutMs?: number;
  readonly createTimeoutSignal?: (milliseconds: number) => AbortSignal;
}

export interface WaitForWorkflowOptions {
  readonly runId: number;
  readonly headSha: string;
  readonly displayTitle?: string;
  readonly timeoutMs?: number;
  readonly pollIntervalMs?: number;
}

export interface GitHubWorkflowDispatch {
  readonly runId: number;
  readonly runUrl: string;
  readonly htmlUrl: string;
}

export interface ListRepositoryArtifactsOptions {
  readonly name?: string;
  readonly createdAfterMs?: number;
}

const DEFAULT_GITHUB_API_BASE_URL = "https://api.github.com/";
const DEFAULT_WORKFLOW_TIMEOUT_MS = 45 * 60 * 1000;
const DEFAULT_POLL_INTERVAL_MS = 10_000;
const DEFAULT_REQUEST_TIMEOUT_MS = 20_000;
// Ninety days of one evidence artifact and one optional replay bundle at the
// five-minute schedule needs up to 521 pages before deployment artifacts.
const MAX_ARTIFACT_METADATA_PAGES = 900;
const DEFAULT_ARTIFACT_DOWNLOAD_LIMIT_BYTES = 256 * 1024 * 1024;
const FULL_GIT_SHA = /^[0-9a-f]{40}$/;

const defaultSleep = (milliseconds: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

const asRecord = (value: unknown): Record<string, unknown> | null =>
  value !== null && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : null;

const nonEmptyString = (value: unknown): string | null =>
  typeof value === "string" && value.trim() !== "" ? value.trim() : null;

const integer = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null;

const parseWorkflowRun = (value: unknown): GitHubWorkflowRun => {
  const record = asRecord(value);
  if (!record) throw new Error("GitHub returned an invalid workflow run");
  const id = integer(record.id);
  const headSha = nonEmptyString(record.head_sha);
  const status = nonEmptyString(record.status);
  const createdAt = nonEmptyString(record.created_at);
  const displayTitle = nonEmptyString(record.display_title);
  if (!id || !headSha || !status || !createdAt || !displayTitle) {
    throw new Error("GitHub returned an incomplete workflow run");
  }
  return {
    id,
    headSha,
    status,
    conclusion: nonEmptyString(record.conclusion),
    htmlUrl: nonEmptyString(record.html_url),
    createdAt,
    displayTitle,
  };
};

const parseArtifact = (value: unknown): GitHubArtifact => {
  const record = asRecord(value);
  if (!record) throw new Error("GitHub returned an invalid artifact");
  const id = integer(record.id);
  const name = nonEmptyString(record.name);
  const sizeInBytes = typeof record.size_in_bytes === "number" && Number.isSafeInteger(record.size_in_bytes) &&
      record.size_in_bytes >= 0
    ? record.size_in_bytes
    : null;
  const createdAt = nonEmptyString(record.created_at);
  if (!id || !name || sizeInBytes === null || !createdAt) {
    throw new Error("GitHub returned an incomplete artifact");
  }
  return {
    id,
    name,
    sizeInBytes,
    expired: record.expired === true,
    createdAt,
    expiresAt: nonEmptyString(record.expires_at),
  };
};

export class GitHubActionsClient {
  readonly #repository: string;
  readonly #token: string;
  readonly #fetcher: GitHubFetch;
  readonly #apiBaseUrl: URL;
  readonly #sleep: (milliseconds: number) => Promise<void>;
  readonly #now: () => number;
  readonly #requestTimeoutMs: number;
  readonly #createTimeoutSignal: (milliseconds: number) => AbortSignal;

  constructor(options: GitHubClientOptions) {
    if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(options.repository)) {
      throw new Error("GitHub repository must use the owner/name form");
    }
    if (options.token.trim() === "") throw new Error("A GitHub token is required");
    this.#repository = options.repository;
    this.#token = options.token;
    this.#fetcher = options.fetcher ?? fetch;
    this.#apiBaseUrl = new URL(options.apiBaseUrl ?? DEFAULT_GITHUB_API_BASE_URL);
    this.#sleep = options.sleep ?? defaultSleep;
    this.#now = options.now ?? Date.now;
    this.#requestTimeoutMs = options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    if (!Number.isSafeInteger(this.#requestTimeoutMs) || this.#requestTimeoutMs <= 0) {
      throw new Error("GitHub request timeout must be a positive integer");
    }
    this.#createTimeoutSignal = options.createTimeoutSignal ?? AbortSignal.timeout;
  }

  #url(path: string): URL {
    return new URL(path.replace(/^\//, ""), this.#apiBaseUrl);
  }

  #headers(contentType = false): Headers {
    const headers = new Headers({
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${this.#token}`,
      "X-GitHub-Api-Version": "2026-03-10",
    });
    if (contentType) headers.set("Content-Type", "application/json");
    return headers;
  }

  async #request(
    input: string | URL | Request,
    init: RequestInit,
    operation: string,
  ): Promise<Response> {
    const signal = this.#createTimeoutSignal(this.#requestTimeoutMs);
    try {
      return await this.#fetcher(input, { ...init, signal });
    } catch {
      if (signal.aborted) throw new Error(`${operation} timed out`);
      throw new Error(`${operation} request failed`);
    }
  }

  async #json(response: Response, operation: string): Promise<unknown> {
    if (!response.ok) throw new Error(`${operation} failed with HTTP ${response.status}`);
    try {
      return await response.json();
    } catch {
      throw new Error(`${operation} returned invalid JSON`);
    }
  }

  async dispatchWorkflow(
    workflow: string,
    ref: string,
    inputs: Readonly<Record<string, string | boolean>> = {},
  ): Promise<GitHubWorkflowDispatch> {
    if (workflow.trim() === "" || ref.trim() === "") {
      throw new Error("Workflow and ref are required for dispatch");
    }
    const response = await this.#request(
      this.#url(
        `repos/${this.#repository}/actions/workflows/${encodeURIComponent(workflow)}/dispatches`,
      ),
      {
        method: "POST",
        headers: this.#headers(true),
        body: JSON.stringify({ ref, inputs, return_run_details: true }),
        redirect: "manual",
      },
      `Dispatch workflow ${workflow}`,
    );
    if (response.status !== 200) {
      throw new Error(`Dispatch workflow ${workflow} failed with HTTP ${response.status}; expected 200`);
    }
    const payload = asRecord(await this.#json(response, `Dispatch workflow ${workflow}`));
    const runId = integer(payload?.workflow_run_id);
    const runUrl = nonEmptyString(payload?.run_url);
    const htmlUrl = nonEmptyString(payload?.html_url);
    if (!runId || !runUrl || !htmlUrl) {
      throw new Error(`Dispatch workflow ${workflow} returned incomplete run details`);
    }
    const expectedRunUrl = this.#url(`repos/${this.#repository}/actions/runs/${runId}`);
    if (runUrl !== expectedRunUrl.href) {
      throw new Error(`Dispatch workflow ${workflow} returned a run URL outside the repository`);
    }
    let parsedHtmlUrl: URL;
    try {
      parsedHtmlUrl = new URL(htmlUrl);
    } catch {
      throw new Error(`Dispatch workflow ${workflow} returned an invalid HTML run URL`);
    }
    const expectedHtmlHost = this.#apiBaseUrl.hostname === "api.github.com"
      ? "github.com"
      : this.#apiBaseUrl.hostname.replace(/^api\./u, "");
    if (
      parsedHtmlUrl.protocol !== "https:" || parsedHtmlUrl.hostname !== expectedHtmlHost ||
      parsedHtmlUrl.username !== "" || parsedHtmlUrl.password !== "" || parsedHtmlUrl.search !== "" ||
      parsedHtmlUrl.hash !== "" || parsedHtmlUrl.pathname !== `/${this.#repository}/actions/runs/${runId}`
    ) {
      throw new Error(`Dispatch workflow ${workflow} returned an HTML run URL outside the repository`);
    }
    return { runId, runUrl, htmlUrl };
  }

  async getWorkflowRun(runId: number): Promise<GitHubWorkflowRun> {
    if (!Number.isSafeInteger(runId) || runId <= 0) throw new Error("Workflow run ID must be positive");
    const response = await this.#request(
      this.#url(`repos/${this.#repository}/actions/runs/${runId}`),
      {
        method: "GET",
        headers: this.#headers(),
        redirect: "manual",
      },
      `Get workflow run ${runId}`,
    );
    return parseWorkflowRun(await this.#json(response, `Get workflow run ${runId}`));
  }

  async waitForWorkflow(options: WaitForWorkflowOptions): Promise<GitHubWorkflowRun> {
    if (!Number.isSafeInteger(options.runId) || options.runId <= 0) {
      throw new Error("Workflow run ID must be positive");
    }
    if (!FULL_GIT_SHA.test(options.headSha)) {
      throw new Error("Workflow head SHA must be a lowercase, full Git commit SHA");
    }
    const timeoutMs = options.timeoutMs ?? DEFAULT_WORKFLOW_TIMEOUT_MS;
    const pollIntervalMs = options.pollIntervalMs ?? DEFAULT_POLL_INTERVAL_MS;
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) throw new Error("Workflow timeout must be positive");
    if (!Number.isFinite(pollIntervalMs) || pollIntervalMs <= 0) {
      throw new Error("Workflow poll interval must be positive");
    }
    if (options.displayTitle !== undefined && options.displayTitle.trim() === "") {
      throw new Error("Workflow display-title correlation must not be empty");
    }

    const deadline = this.#now() + timeoutMs;
    let lastPollingError: unknown = null;
    while (this.#now() <= deadline) {
      let run: GitHubWorkflowRun;
      try {
        run = await this.getWorkflowRun(options.runId);
        lastPollingError = null;
      } catch (error) {
        // A dispatch response gives Sentinel one exact run ID. A transient API,
        // network, or eventual-consistency failure must not release the caller
        // into rollback while that exact serialized run may still be active.
        lastPollingError = error;
        if (this.#now() >= deadline) break;
        await this.#sleep(Math.min(pollIntervalMs, Math.max(0, deadline - this.#now())));
        continue;
      }
      if (run.headSha !== options.headSha) {
        throw new Error(`Workflow run ${run.id} has the wrong head SHA`);
      }
      if (options.displayTitle !== undefined && run.displayTitle !== options.displayTitle) {
        throw new Error(`Workflow run ${run.id} has the wrong dispatch correlation`);
      }
      if (run.status === "completed") {
        if (run.conclusion !== "success") {
          throw new Error(`Workflow run ${run.id} completed with ${run.conclusion ?? "no conclusion"}`);
        }
        return run;
      }

      if (this.#now() >= deadline) break;
      await this.#sleep(Math.min(pollIntervalMs, Math.max(0, deadline - this.#now())));
    }
    throw new Error(
      `Timed out reconciling workflow run ${options.runId} at ${options.headSha}`,
      lastPollingError === null ? undefined : { cause: lastPollingError },
    );
  }

  async listRunArtifacts(runId: number): Promise<readonly GitHubArtifact[]> {
    if (!Number.isSafeInteger(runId) || runId <= 0) throw new Error("Workflow run ID must be positive");
    const artifacts: GitHubArtifact[] = [];
    for (let page = 1;; page += 1) {
      if (page > MAX_ARTIFACT_METADATA_PAGES) {
        throw new Error("GitHub workflow artifact metadata exceeded the Sentinel page limit");
      }
      const url = this.#url(`repos/${this.#repository}/actions/runs/${runId}/artifacts`);
      url.searchParams.set("per_page", "100");
      url.searchParams.set("page", String(page));
      const response = await this.#request(url, {
        method: "GET",
        headers: this.#headers(),
        redirect: "manual",
      }, `List artifacts for workflow run ${runId}`);
      const payload = asRecord(await this.#json(response, `List artifacts for workflow run ${runId}`));
      if (!payload || !Array.isArray(payload.artifacts)) {
        throw new Error(`List artifacts for workflow run ${runId} returned no artifacts array`);
      }
      const pageArtifacts = payload.artifacts.map(parseArtifact);
      artifacts.push(...pageArtifacts);
      if (pageArtifacts.length < 100) break;
    }
    return artifacts;
  }

  async listRepositoryArtifacts(
    options: ListRepositoryArtifactsOptions = {},
  ): Promise<readonly GitHubArtifact[]> {
    if (options.name !== undefined && options.name.length === 0) {
      throw new Error("Artifact name must not be empty");
    }
    if (
      options.createdAfterMs !== undefined &&
      (!Number.isFinite(options.createdAfterMs) || options.createdAfterMs < 0)
    ) {
      throw new Error("Artifact creation fence is invalid");
    }

    const artifacts: GitHubArtifact[] = [];
    for (let page = 1;; page += 1) {
      if (page > MAX_ARTIFACT_METADATA_PAGES) {
        throw new Error("GitHub repository artifact metadata exceeded the Sentinel page limit");
      }
      const url = this.#url(`repos/${this.#repository}/actions/artifacts`);
      url.searchParams.set("per_page", "100");
      url.searchParams.set("page", String(page));
      if (options.name !== undefined) url.searchParams.set("name", options.name);
      const response = await this.#request(url, {
        method: "GET",
        headers: this.#headers(),
        redirect: "manual",
      }, "List repository artifacts");
      const payload = asRecord(await this.#json(response, "List repository artifacts"));
      if (!payload || !Array.isArray(payload.artifacts)) {
        throw new Error("List repository artifacts returned no artifacts array");
      }
      const pageArtifacts = payload.artifacts.map(parseArtifact);
      artifacts.push(...pageArtifacts.filter((artifact) =>
        !artifact.expired &&
        (options.name === undefined || artifact.name === options.name) &&
        (options.createdAfterMs === undefined || Date.parse(artifact.createdAt) >= options.createdAfterMs)
      ));
      if (pageArtifacts.length < 100) break;
    }
    return artifacts;
  }

  async downloadArtifact(
    artifactId: number,
    maximumBytes = DEFAULT_ARTIFACT_DOWNLOAD_LIMIT_BYTES,
  ): Promise<Uint8Array> {
    if (!Number.isSafeInteger(artifactId) || artifactId <= 0) throw new Error("Artifact ID must be positive");
    if (!Number.isSafeInteger(maximumBytes) || maximumBytes <= 0) {
      throw new Error("Artifact download limit must be a positive integer");
    }
    const response = await this.#request(
      this.#url(`repos/${this.#repository}/actions/artifacts/${artifactId}/zip`),
      {
        method: "GET",
        headers: this.#headers(),
        redirect: "follow",
      },
      `Download artifact ${artifactId}`,
    );
    if (!response.ok) throw new Error(`Download artifact ${artifactId} failed with HTTP ${response.status}`);
    const declaredLength = response.headers.get("content-length");
    if (declaredLength !== null) {
      const parsedLength = Number(declaredLength);
      if (!Number.isSafeInteger(parsedLength) || parsedLength < 0 || parsedLength > maximumBytes) {
        throw new Error(`Download artifact ${artifactId} exceeded the Sentinel byte limit`);
      }
    }
    if (!response.body) return new Uint8Array();
    const chunks: Uint8Array[] = [];
    let total = 0;
    for await (const chunk of response.body) {
      total += chunk.byteLength;
      if (total > maximumBytes) {
        throw new Error(`Download artifact ${artifactId} exceeded the Sentinel byte limit`);
      }
      chunks.push(chunk);
    }
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return bytes;
  }
}
