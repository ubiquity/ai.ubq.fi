import {
  executorEquals,
  isTerminalSentinelProviderPhase,
  parseSentinelProviderStateDocument,
  type SentinelProviderAttestationV1,
  type SentinelProviderStateDocumentV1,
} from "./provider-state.ts";
import {
  parseBootstrapReleaseRecord,
  parseSentinelBootstrapActivationPointer,
  parseSentinelBootstrapHealthSignal,
  parseSentinelBootstrapProgressDecision,
  parseSentinelBootstrapRollbackIntent,
  parseSentinelFailureConstraint,
  type SentinelBootstrapActivationPointerV1,
  type SentinelBootstrapHealthSignalV1,
  type SentinelBootstrapProgressDecisionV1,
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
export const SENTINEL_PROVIDER_STATE_PATH = "docs/sentinel-provider-state.json";

type JsonRecord = Record<string, unknown>;

export type SentinelBootstrapStateDocumentV1 = Readonly<{
  schema_version: 1;
  release: SentinelBootstrapReleaseRecordV1 | null;
  signals: readonly SentinelBootstrapHealthSignalV1[];
  activation: SentinelBootstrapActivationPointerV1 | null;
  rollback_intent: SentinelBootstrapRollbackIntentV1 | null;
  constraints: readonly SentinelFailureConstraintV1[];
  /**
   * Advisory bootstrap progress decision (m06). It never overrides the
   * activation/rollback identity, exact SHA/revision proof, or promotion
   * gates; older documents without the field parse as null.
   */
  progress: SentinelBootstrapProgressDecisionV1 | null;
}>;

type Snapshot = Readonly<{
  document: SentinelBootstrapStateDocumentV1;
  commit_sha: string;
  tree_sha: string;
  state_ref_exists: boolean;
}>;

type GitSnapshotBase = Readonly<{
  commit_sha: string;
  tree_sha: string;
  state_ref_exists: boolean;
}>;

export type SentinelProviderStateSnapshot = Readonly<{
  document: SentinelProviderStateDocumentV1 | null;
  commit_sha: string;
  tree_sha: string;
  state_ref_exists: boolean;
}>;

export type GitHubSentinelProviderState = Readonly<{
  readSnapshot(): SentinelProviderStateSnapshot;
  refresh(): Promise<SentinelProviderStateSnapshot>;
  compareAndSet(expectedCommitSha: string, nextDocument: unknown): Promise<boolean>;
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
  const progress = candidate.progress === undefined || candidate.progress === null
    ? null
    : parseSentinelBootstrapProgressDecision(candidate.progress);
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
    progress,
  });
};

const decodeBase64 = (value: string): string =>
  new TextDecoder("utf-8", { fatal: true }).decode(
    Uint8Array.from(atob(value.replaceAll("\n", "")), (character) => character.charCodeAt(0)),
  );

const withGitHubHeaders = (token: string, init: RequestInit): RequestInit => ({
  ...init,
  headers: {
    Accept: "application/vnd.github+json",
    Authorization: `Bearer ${token}`,
    "X-GitHub-Api-Version": "2022-11-28",
    ...(init.headers ?? {}),
  },
});

const request = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit,
  fetcher: typeof fetch,
  allowNotFound = false,
): Promise<Readonly<{ status: number; value: unknown }>> => {
  const response = await fetcher(`https://api.github.com/repos/${repository}${path}`, withGitHubHeaders(token, init));
  if (allowNotFound && response.status === 404) return { status: 404, value: null };
  const value = await response.json().catch(() => null);
  if (!response.ok) throw new Error(`GitHub Sentinel state request failed with HTTP ${response.status}`);
  return { status: response.status, value };
};

const refSha = (value: unknown): string => sha(record(record(value)?.object)?.sha, "Sentinel state ref SHA");
const treeSha = (value: unknown): string => sha(record(record(value)?.tree)?.sha, "Sentinel state tree SHA");

const readGitSnapshotBase = async (
  token: string,
  repository: string,
  fetcher: typeof fetch,
): Promise<GitSnapshotBase> => {
  const stateRef = await request(token, repository, "/git/ref/heads/sentinel/bootstrap-state", {}, fetcher, true);
  const stateRefExists = stateRef.status !== 404;
  const commitSha = stateRefExists
    ? refSha(stateRef.value)
    : refSha((await request(token, repository, "/git/ref/heads/development", {}, fetcher)).value);
  const commit = await request(token, repository, `/git/commits/${commitSha}`, {}, fetcher);
  return { commit_sha: commitSha, tree_sha: treeSha(commit.value), state_ref_exists: stateRefExists };
};

const readSnapshot = async (
  token: string,
  repository: string,
  fetcher: typeof fetch,
): Promise<Snapshot> => {
  const base = await readGitSnapshotBase(token, repository, fetcher);
  const contents = record(
    (await request(
      token,
      repository,
      `/contents/${SENTINEL_BOOTSTRAP_STATE_PATH}?ref=${base.commit_sha}`,
      {},
      fetcher,
    )).value,
  );
  if (!contents || contents.encoding !== "base64" || typeof contents.content !== "string") {
    throw new Error("GitHub bootstrap state file is invalid");
  }
  return {
    document: parseSentinelBootstrapStateDocument(JSON.parse(decodeBase64(contents.content))),
    ...base,
  };
};

const readProviderSnapshot = async (
  token: string,
  repository: string,
  fetcher: typeof fetch,
): Promise<SentinelProviderStateSnapshot> => {
  const base = await readGitSnapshotBase(token, repository, fetcher);
  const contents = await request(
    token,
    repository,
    `/contents/${SENTINEL_PROVIDER_STATE_PATH}?ref=${base.commit_sha}`,
    {},
    fetcher,
    true,
  );
  let document: SentinelProviderStateDocumentV1 | null = null;
  if (contents.status !== 404) {
    const encoded = record(contents.value);
    if (!encoded || encoded.encoding !== "base64" || typeof encoded.content !== "string") {
      throw new Error("GitHub provider state file is invalid");
    }
    let json: unknown;
    try {
      json = JSON.parse(decodeBase64(encoded.content));
    } catch {
      throw new Error("GitHub provider state file is not valid JSON");
    }
    document = parseSentinelProviderStateDocument(json);
  }
  return Object.freeze({ document, ...base });
};

const createStateCommit = async (
  token: string,
  repository: string,
  base: GitSnapshotBase,
  documentPath: string,
  document: unknown,
  message: string,
  fetcher: typeof fetch,
): Promise<Readonly<{ commit_sha: string; tree_sha: string }>> => {
  const blob = record(
    (await request(token, repository, "/git/blobs", {
      method: "POST",
      body: JSON.stringify({ content: `${JSON.stringify(document, null, 2)}\n`, encoding: "utf-8" }),
    }, fetcher)).value,
  );
  const blobSha = sha(blob?.sha, "Sentinel state blob SHA");
  const tree = record(
    (await request(token, repository, "/git/trees", {
      method: "POST",
      body: JSON.stringify({
        base_tree: base.tree_sha,
        tree: [{ path: documentPath, mode: "100644", type: "blob", sha: blobSha }],
      }),
    }, fetcher)).value,
  );
  const nextTreeSha = sha(tree?.sha, "Sentinel state updated tree SHA");
  const commit = record(
    (await request(token, repository, "/git/commits", {
      method: "POST",
      body: JSON.stringify({ message, tree: nextTreeSha, parents: [base.commit_sha] }),
    }, fetcher)).value,
  );
  const nextCommitSha = sha(commit?.sha, "Sentinel state commit SHA");
  return { commit_sha: nextCommitSha, tree_sha: nextTreeSha };
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
  const created = await createStateCommit(
    token,
    repository,
    snapshot,
    SENTINEL_BOOTSTRAP_STATE_PATH,
    document,
    message,
    fetcher,
  );
  await request(
    token,
    repository,
    snapshot.state_ref_exists ? "/git/refs/heads/sentinel/bootstrap-state" : "/git/refs",
    {
      method: snapshot.state_ref_exists ? "PATCH" : "POST",
      body: JSON.stringify(
        snapshot.state_ref_exists
          ? { sha: created.commit_sha, force: false }
          : { ref: SENTINEL_BOOTSTRAP_STATE_REF, sha: created.commit_sha },
      ),
    },
    fetcher,
  );
  return { document, commit_sha: created.commit_sha, tree_sha: created.tree_sha, state_ref_exists: true };
};

const attestationEquals = (left: SentinelProviderAttestationV1, right: SentinelProviderAttestationV1): boolean =>
  JSON.stringify(left) === JSON.stringify(right);

/**
 * CAS policy validation between the captured snapshot document and the next
 * document. Generation, application retention, protected transaction fields,
 * and the no-reclaim/no-implicit-initialization rules are all enforced here;
 * schema validity is enforced by the parser before this runs.
 */
const isValidProviderStateTransition = (
  previous: SentinelProviderStateDocumentV1 | null,
  next: SentinelProviderStateDocumentV1,
  capturedCommitSha: string,
): boolean => {
  if (previous === null) {
    return next.generation === 1 && next.applications.every((app) => app.transaction === null);
  }
  if (!Number.isSafeInteger(next.generation) || next.generation !== previous.generation + 1) return false;
  const previousApps = new Map(previous.applications.map((app) => [app.app, app]));
  for (const nextApp of next.applications) {
    const prevApp = previousApps.get(nextApp.app);
    if (prevApp === undefined) {
      // A brand-new application may never enter with a transaction.
      if (nextApp.transaction !== null) return false;
      continue;
    }
    const prevTransaction = prevApp.transaction;
    const nextTransaction = nextApp.transaction;
    const currentHealthy = prevApp.healthy;
    if (prevTransaction !== null && nextTransaction !== null && nextTransaction.id === prevTransaction.id) {
      if (isTerminalSentinelProviderPhase(prevTransaction.phase)) {
        // Terminal evidence is immutable: the transaction may only persist as
        // exactly equal normalized data with an unchanged healthy attestation.
        // Any terminal->nonterminal regression or completed-evidence rewrite is
        // rejected, and clearing a terminal transaction is handled below.
        if (
          currentHealthy !== null && nextApp.healthy !== null &&
          attestationEquals(currentHealthy, nextApp.healthy) &&
          JSON.stringify(nextTransaction) === JSON.stringify(prevTransaction)
        ) {
          continue;
        }
        return false;
      }
      // An unfinished transaction retains its fixed identity, previous/candidate,
      // executor/fence, created_at/retired_executor, and archive link.
      if (!attestationEquals(nextTransaction.previous, prevTransaction.previous)) return false;
      if (!attestationEquals(nextTransaction.candidate, prevTransaction.candidate)) return false;
      if (nextTransaction.expected_merged_sha !== prevTransaction.expected_merged_sha) return false;
      if (nextTransaction.previous_transaction_commit !== prevTransaction.previous_transaction_commit) return false;
      if (!executorEquals(nextTransaction.executor, prevTransaction.executor)) return false;
      if (nextTransaction.fence_generation !== prevTransaction.fence_generation) return false;
      if (nextTransaction.created_at !== prevTransaction.created_at) return false;
      if (JSON.stringify(nextTransaction.retired_executor) !== JSON.stringify(prevTransaction.retired_executor)) {
        return false;
      }
      continue;
    }
    if (prevTransaction === null) {
      if (nextTransaction === null) {
        if (
          currentHealthy !== null &&
          (nextApp.healthy === null || !attestationEquals(currentHealthy, nextApp.healthy))
        ) {
          return false;
        }
        continue;
      }
      // First transaction from an app with an initialized healthy attestation.
      if (
        nextTransaction.phase !== "prepared" ||
        currentHealthy === null ||
        nextTransaction.fence_generation !== next.generation ||
        nextTransaction.previous_transaction_commit !== null ||
        !attestationEquals(nextTransaction.previous, currentHealthy)
      ) return false;
      continue;
    }
    if (isTerminalSentinelProviderPhase(prevTransaction.phase)) {
      // A terminal transaction can never be cleared; a new transaction may
      // follow only as prepared, anchored at the exact captured commit, with
      // the previous attestation equal to the current healthy attestation.
      if (nextTransaction === null) return false;
      if (
        nextTransaction.phase !== "prepared" ||
        nextTransaction.fence_generation !== next.generation ||
        nextTransaction.previous_transaction_commit !== capturedCommitSha ||
        currentHealthy === null ||
        !attestationEquals(nextTransaction.previous, currentHealthy)
      ) return false;
      continue;
    }
    // A nonterminal transaction may never be removed or replaced.
    return false;
  }
  for (const prevApp of previous.applications) {
    if (!next.applications.some((app) => app.app === prevApp.app)) return false;
  }
  return true;
};

type RefPublication = "published" | "conflict";

const updateStateRef = async (
  token: string,
  repository: string,
  captured: GitSnapshotBase,
  nextCommitSha: string,
  fetcher: typeof fetch,
): Promise<RefPublication> => {
  const response = await fetcher(
    `https://api.github.com/repos/${repository}${
      captured.state_ref_exists ? "/git/refs/heads/sentinel/bootstrap-state" : "/git/refs"
    }`,
    withGitHubHeaders(token, {
      method: captured.state_ref_exists ? "PATCH" : "POST",
      body: JSON.stringify(
        captured.state_ref_exists
          ? { sha: nextCommitSha, force: false }
          : { ref: SENTINEL_BOOTSTRAP_STATE_REF, sha: nextCommitSha },
      ),
    }),
  );
  if (response.status === 409 || response.status === 422) return "conflict";
  if (!response.ok) throw new Error(`GitHub Sentinel state ref update failed with HTTP ${response.status}`);
  const value = await response.json().catch(() => null);
  if (refSha(value) !== nextCommitSha) {
    throw new Error("GitHub Sentinel state ref update response is ambiguous");
  }
  return "published";
};

export type GitHubSentinelBootstrapState = Readonly<{
  store: SentinelBootstrapStateStore;
  readDocument(): SentinelBootstrapStateDocumentV1;
  appendSignals(signals: readonly SentinelBootstrapHealthSignalV1[]): Promise<void>;
  replaceRelease(
    release: SentinelBootstrapReleaseRecordV1,
    activation?: SentinelBootstrapActivationPointerV1,
  ): Promise<void>;
  /** Persists the advisory progress decision; evidence is never authoritative. */
  replaceProgress(decision: SentinelBootstrapProgressDecisionV1): Promise<void>;
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
    async replaceProgress(decision): Promise<void> {
      const parsed = parseSentinelBootstrapProgressDecision(decision);
      if (JSON.stringify(snapshot.document.progress) === JSON.stringify(parsed)) return;
      await persist(
        { ...snapshot.document, progress: parsed },
        "chore(sentinel): record bootstrap progress evidence",
      );
    },
  };
};

/**
 * Fixed GitHub provider-state adapter. It reads by immutable commit, writes
 * ONLY docs/sentinel-provider-state.json into the existing tree with exactly
 * the captured parent, and publishes with force:false. It never reads or
 * parses the controller document, never calls health/deployment APIs, and
 * never force-updates any ref.
 */
export const createGitHubSentinelProviderState = async (
  input: Readonly<{ token: string; repository: string; fetcher?: typeof fetch }>,
): Promise<GitHubSentinelProviderState> => {
  if (!input.token || !SAFE_REPOSITORY.test(input.repository)) {
    throw new Error("GitHub provider state identity is invalid");
  }
  const fetcher = input.fetcher ?? fetch;
  let current = await readProviderSnapshot(input.token, input.repository, fetcher);
  const refresh = async (): Promise<SentinelProviderStateSnapshot> => {
    current = await readProviderSnapshot(input.token, input.repository, fetcher);
    return current;
  };
  return {
    readSnapshot: (): SentinelProviderStateSnapshot => current,
    async refresh(): Promise<SentinelProviderStateSnapshot> {
      return await refresh();
    },
    async compareAndSet(expectedCommitSha, nextDocument): Promise<boolean> {
      // Capture the complete immutable snapshot once: every validation,
      // expected-parent, base-tree, and ref decision below uses this exact
      // captured state, so concurrent calls on the same adapter can never
      // reuse a newly mutated outer snapshot to publish a duplicate
      // generation.
      const captured = current;
      // A stale local expected SHA rejects with false before any parsing or
      // request.
      if (expectedCommitSha !== captured.commit_sha) return false;
      const next = parseSentinelProviderStateDocument(nextDocument);
      if (!isValidProviderStateTransition(captured.document, next, captured.commit_sha)) return false;
      // Re-check the exact captured ref (and development SHA when the state
      // branch is absent) before creating any Git object.
      const remoteStateRef = await request(
        input.token,
        input.repository,
        "/git/ref/heads/sentinel/bootstrap-state",
        {},
        fetcher,
        true,
      );
      if (captured.state_ref_exists) {
        if (remoteStateRef.status === 404 || refSha(remoteStateRef.value) !== captured.commit_sha) return false;
      } else {
        if (remoteStateRef.status !== 404) return false;
        const development = await request(
          input.token,
          input.repository,
          "/git/ref/heads/development",
          {},
          fetcher,
        );
        if (refSha(development.value) !== captured.commit_sha) return false;
      }
      const created = await createStateCommit(
        input.token,
        input.repository,
        captured,
        SENTINEL_PROVIDER_STATE_PATH,
        next,
        `chore(sentinel): record provider state ${crypto.randomUUID()}`,
        fetcher,
      );
      const publication = await updateStateRef(
        input.token,
        input.repository,
        captured,
        created.commit_sha,
        fetcher,
      );
      if (publication === "conflict") {
        // Never blind-retry a stale write; reconcile through an exact refresh.
        await refresh().catch(() => undefined);
        return false;
      }
      current = Object.freeze({
        document: next,
        commit_sha: created.commit_sha,
        tree_sha: created.tree_sha,
        state_ref_exists: true,
      });
      return true;
    },
  };
};
