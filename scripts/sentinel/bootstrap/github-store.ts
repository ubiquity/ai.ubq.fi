import {
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelBootstrapHealthSignal,
  parseSentinelBootstrapRollbackIntent,
  parseSentinelFailureConstraint,
  type SentinelBootstrapActivationPointerV1,
  type SentinelBootstrapHealthSignalV1,
  type SentinelBootstrapReleaseRecordV1,
  type SentinelBootstrapRollbackIntentV1,
  type SentinelFailureConstraintV1,
} from "./contracts.ts";
import type {
  SentinelBootstrapActivationSnapshot,
  SentinelBootstrapRollbackIntentSnapshot,
  SentinelBootstrapStateStore,
} from "./activation.ts";

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
export const SENTINEL_BOOTSTRAP_STATE_PATH = "docs/sentinel-bootstrap-state.json";
export const SENTINEL_BOOTSTRAP_STATE_REF = "refs/heads/sentinel/bootstrap-state";

type JsonRecord = Record<string, unknown>;

export type SentinelBootstrapStateDocumentV1 = Readonly<{
  schema_version: 1;
  release: SentinelBootstrapReleaseRecordV1 | null;
  signals: readonly SentinelBootstrapHealthSignalV1[];
  activation: SentinelBootstrapActivationPointerV1 | null;
  rollback_intent: SentinelBootstrapRollbackIntentV1 | null;
  constraints: readonly SentinelFailureConstraintV1[];
}>;

type Snapshot = Readonly<{
  document: SentinelBootstrapStateDocumentV1;
  commit_sha: string;
  tree_sha: string;
  state_ref_exists: boolean;
}>;

const record = (value: unknown): JsonRecord | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as JsonRecord : null;

const sha = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !FULL_SHA.test(value)) throw new Error(`${label} is invalid`);
  return value;
};

export const parseSentinelBootstrapStateDocument = (value: unknown): SentinelBootstrapStateDocumentV1 => {
  const candidate = record(value);
  if (
    !candidate || candidate.schema_version !== 1 || !Array.isArray(candidate.signals) ||
    candidate.signals.length > 64 || !Array.isArray(candidate.constraints) || candidate.constraints.length > 64
  ) throw new Error("Sentinel bootstrap state document is invalid");
  const release = candidate.release === null ? null : parseBootstrapReleaseRecord(candidate.release);
  const signals = candidate.signals.map(parseSentinelBootstrapHealthSignal);
  const activation = candidate.activation === null
    ? null
    : parseSentinelBootstrapActivationPointer(candidate.activation);
  const rollbackIntent = candidate.rollback_intent === null
    ? null
    : parseSentinelBootstrapRollbackIntent(candidate.rollback_intent);
  const constraints = candidate.constraints.map(parseSentinelFailureConstraint);
  const constraintKeys = constraints.map((constraint) => constraint.failure_fingerprint);
  if (new Set(constraintKeys).size !== constraintKeys.length) {
    throw new Error("Sentinel bootstrap constraints contain duplicate fingerprints");
  }
  return Object.freeze({
    schema_version: 1,
    release,
    signals: Object.freeze(signals),
    activation,
    rollback_intent: rollbackIntent,
    constraints: Object.freeze(constraints),
  });
};

const decodeBase64 = (value: string): string =>
  new TextDecoder("utf-8", { fatal: true }).decode(
    Uint8Array.from(atob(value.replaceAll("\n", "")), (character) => character.charCodeAt(0)),
  );

const request = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit,
  fetcher: typeof fetch,
  allowNotFound = false,
): Promise<Readonly<{ status: number; value: unknown }>> => {
  const response = await fetcher(`https://api.github.com/repos/${repository}${path}`, {
    ...init,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": "2022-11-28",
      ...(init.headers ?? {}),
    },
  });
  if (allowNotFound && response.status === 404) return { status: 404, value: null };
  const value = await response.json().catch(() => null);
  if (!response.ok) throw new Error(`GitHub bootstrap state request failed with HTTP ${response.status}`);
  return { status: response.status, value };
};

const refSha = (value: unknown): string => sha(record(record(value)?.object)?.sha, "Bootstrap state ref SHA");
const treeSha = (value: unknown): string => sha(record(record(value)?.tree)?.sha, "Bootstrap state tree SHA");

const readSnapshot = async (
  token: string,
  repository: string,
  fetcher: typeof fetch,
): Promise<Snapshot> => {
  const stateRef = await request(token, repository, "/git/ref/heads/sentinel/bootstrap-state", {}, fetcher, true);
  const stateRefExists = stateRef.status !== 404;
  const commitSha = stateRefExists
    ? refSha(stateRef.value)
    : refSha((await request(token, repository, "/git/ref/heads/development", {}, fetcher)).value);
  const commit = await request(token, repository, `/git/commits/${commitSha}`, {}, fetcher);
  const baseTreeSha = treeSha(commit.value);
  const contents = record(
    (await request(
      token,
      repository,
      `/contents/${SENTINEL_BOOTSTRAP_STATE_PATH}?ref=${commitSha}`,
      {},
      fetcher,
    )).value,
  );
  if (!contents || contents.encoding !== "base64" || typeof contents.content !== "string") {
    throw new Error("GitHub bootstrap state file is invalid");
  }
  return {
    document: parseSentinelBootstrapStateDocument(JSON.parse(decodeBase64(contents.content))),
    commit_sha: commitSha,
    tree_sha: baseTreeSha,
    state_ref_exists: stateRefExists,
  };
};

const writeSnapshot = async (
  token: string,
  repository: string,
  snapshot: Snapshot,
  documentValue: unknown,
  message: string,
  fetcher: typeof fetch,
): Promise<Snapshot> => {
  const document = parseSentinelBootstrapStateDocument(documentValue);
  const blob = record(
    (await request(token, repository, "/git/blobs", {
      method: "POST",
      body: JSON.stringify({ content: `${JSON.stringify(document, null, 2)}\n`, encoding: "utf-8" }),
    }, fetcher)).value,
  );
  const blobSha = sha(blob?.sha, "Bootstrap state blob SHA");
  const tree = record(
    (await request(token, repository, "/git/trees", {
      method: "POST",
      body: JSON.stringify({
        base_tree: snapshot.tree_sha,
        tree: [{ path: SENTINEL_BOOTSTRAP_STATE_PATH, mode: "100644", type: "blob", sha: blobSha }],
      }),
    }, fetcher)).value,
  );
  const nextTreeSha = sha(tree?.sha, "Bootstrap state updated tree SHA");
  const commit = record(
    (await request(token, repository, "/git/commits", {
      method: "POST",
      body: JSON.stringify({ message, tree: nextTreeSha, parents: [snapshot.commit_sha] }),
    }, fetcher)).value,
  );
  const nextCommitSha = sha(commit?.sha, "Bootstrap state commit SHA");
  await request(
    token,
    repository,
    snapshot.state_ref_exists ? "/git/refs/heads/sentinel/bootstrap-state" : "/git/refs",
    {
      method: snapshot.state_ref_exists ? "PATCH" : "POST",
      body: JSON.stringify(
        snapshot.state_ref_exists
          ? { sha: nextCommitSha, force: false }
          : { ref: SENTINEL_BOOTSTRAP_STATE_REF, sha: nextCommitSha },
      ),
    },
    fetcher,
  );
  return { document, commit_sha: nextCommitSha, tree_sha: nextTreeSha, state_ref_exists: true };
};

export type GitHubSentinelBootstrapState = Readonly<{
  store: SentinelBootstrapStateStore;
  readDocument(): SentinelBootstrapStateDocumentV1;
  appendSignals(signals: readonly SentinelBootstrapHealthSignalV1[]): Promise<void>;
  replaceRelease(
    release: SentinelBootstrapReleaseRecordV1,
    activation?: SentinelBootstrapActivationPointerV1,
  ): Promise<void>;
}>;

export const createGitHubSentinelBootstrapState = async (
  input: Readonly<{ token: string; repository: string; fetcher?: typeof fetch }>,
): Promise<GitHubSentinelBootstrapState> => {
  if (!input.token || !SAFE_REPOSITORY.test(input.repository)) {
    throw new Error("GitHub bootstrap state identity is invalid");
  }
  const fetcher = input.fetcher ?? fetch;
  let snapshot = await readSnapshot(input.token, input.repository, fetcher);
  const persist = async (document: SentinelBootstrapStateDocumentV1, message: string): Promise<void> => {
    snapshot = await writeSnapshot(input.token, input.repository, snapshot, document, message, fetcher);
  };
  const store: SentinelBootstrapStateStore = {
    readActivation(): Promise<SentinelBootstrapActivationSnapshot> {
      return Promise.resolve({ pointer: snapshot.document.activation, versionstamp: snapshot.commit_sha });
    },
    async compareAndSetActivation(expectedVersionstamp, next): Promise<boolean> {
      if (
        expectedVersionstamp !== snapshot.commit_sha &&
        !(expectedVersionstamp === null && snapshot.document.activation === null)
      ) {
        return false;
      }
      await persist(
        { ...snapshot.document, activation: parseSentinelBootstrapActivationPointer(next) },
        "chore(sentinel): update bootstrap activation",
      );
      return true;
    },
    readRollbackIntent(): Promise<SentinelBootstrapRollbackIntentSnapshot> {
      return Promise.resolve({
        intent: snapshot.document.rollback_intent,
        versionstamp: snapshot.document.rollback_intent?.constraint.failure_fingerprint ?? null,
      });
    },
    async commitRollback(expectedVersionstamp, next, intent): Promise<boolean> {
      if (expectedVersionstamp !== snapshot.commit_sha || snapshot.document.rollback_intent !== null) return false;
      await persist({
        ...snapshot.document,
        activation: parseSentinelBootstrapActivationPointer(next),
        rollback_intent: parseSentinelBootstrapRollbackIntent(intent),
      }, "chore(sentinel): fence bootstrap generation");
      return true;
    },
    async clearRollbackIntent(expectedVersionstamp): Promise<boolean> {
      if (
        snapshot.document.rollback_intent === null ||
        expectedVersionstamp !== snapshot.document.rollback_intent.constraint.failure_fingerprint
      ) return false;
      await persist({ ...snapshot.document, rollback_intent: null }, "chore(sentinel): complete bootstrap rollback");
      return true;
    },
    async putConstraintIfAbsent(constraint): Promise<boolean> {
      const parsed = parseSentinelFailureConstraint(constraint);
      if (snapshot.document.constraints.some((entry) => entry.failure_fingerprint === parsed.failure_fingerprint)) {
        return false;
      }
      await persist(
        { ...snapshot.document, constraints: [...snapshot.document.constraints, parsed] },
        "chore(sentinel): record bootstrap constraint",
      );
      return true;
    },
  };
  return {
    store,
    readDocument: () => snapshot.document,
    async appendSignals(signals): Promise<void> {
      const parsed = signals.map(parseSentinelBootstrapHealthSignal);
      const byIdentity = new Map<string, SentinelBootstrapHealthSignalV1>();
      for (const signal of [...snapshot.document.signals, ...parsed]) {
        const key = signal.observation_id ?? JSON.stringify(signal);
        byIdentity.set(key, signal);
      }
      const retained = [...byIdentity.values()]
        .sort((left, right) => Date.parse(left.observed_at) - Date.parse(right.observed_at))
        .slice(-64);
      if (JSON.stringify(retained) === JSON.stringify(snapshot.document.signals)) return;
      await persist({ ...snapshot.document, signals: retained }, "chore(sentinel): record bootstrap health signals");
    },
    async replaceRelease(release, activation): Promise<void> {
      await persist({
        ...snapshot.document,
        release: parseBootstrapReleaseRecord(release),
        ...(activation === undefined ? {} : { activation: parseSentinelBootstrapActivationPointer(activation) }),
      }, "chore(sentinel): register bootstrap release");
    },
  };
};
