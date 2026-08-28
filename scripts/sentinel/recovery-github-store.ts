import {
  parseSentinelRecoveryLedger,
  renderSentinelRecoveryLedger,
  SENTINEL_RECOVERY_LEDGER_PATH,
  type SentinelRecoveryLedgerV1,
} from "./recovery-ledger.ts";

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
export const SENTINEL_RECOVERY_STATE_REF = "refs/heads/sentinel/recovery-state";

type JsonRecord = Record<string, unknown>;

export type SentinelRecoveryLedgerSnapshot = Readonly<{
  ledger: SentinelRecoveryLedgerV1;
  commit_sha: string;
  tree_sha: string;
  state_ref_exists: boolean;
}>;

const record = (value: unknown): JsonRecord | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as JsonRecord : null;

const requiredSha = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !FULL_SHA.test(value)) throw new Error(`${label} is invalid`);
  return value;
};

const decodeBase64 = (value: string): string => {
  const compact = value.replaceAll("\n", "");
  return new TextDecoder("utf-8", { fatal: true }).decode(
    Uint8Array.from(atob(compact), (character) => character.charCodeAt(0)),
  );
};

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
  if (!response.ok) {
    throw new Error(
      `GitHub recovery state ${init.method ?? "GET"} ${path} failed with HTTP ${response.status}`,
    );
  }
  return { status: response.status, value };
};

const refCommitSha = (value: unknown): string => {
  const object = record(record(value)?.object);
  return requiredSha(object?.sha, "Sentinel recovery state ref SHA");
};

const commitTreeSha = (value: unknown): string => {
  const tree = record(record(value)?.tree);
  return requiredSha(tree?.sha, "Sentinel recovery state tree SHA");
};

export const readGitHubSentinelRecoveryLedger = async (
  input: Readonly<{ token: string; repository: string; fetcher?: typeof fetch }>,
): Promise<SentinelRecoveryLedgerSnapshot> => {
  if (!input.token || !SAFE_REPOSITORY.test(input.repository)) {
    throw new Error("GitHub recovery state identity is invalid");
  }
  const fetcher = input.fetcher ?? fetch;
  const stateRef = await request(
    input.token,
    input.repository,
    "/git/ref/heads/sentinel/recovery-state",
    {},
    fetcher,
    true,
  );
  const stateRefExists = stateRef.status !== 404;
  const commitSha = stateRefExists ? refCommitSha(stateRef.value) : refCommitSha(
    (await request(input.token, input.repository, "/git/ref/heads/development", {}, fetcher)).value,
  );
  const commit = await request(input.token, input.repository, `/git/commits/${commitSha}`, {}, fetcher);
  const treeSha = commitTreeSha(commit.value);
  const contents = record(
    (await request(
      input.token,
      input.repository,
      `/contents/${SENTINEL_RECOVERY_LEDGER_PATH}?ref=${commitSha}`,
      {},
      fetcher,
    )).value,
  );
  if (!contents || contents.encoding !== "base64" || typeof contents.content !== "string") {
    throw new Error("GitHub recovery state file is invalid");
  }
  const ledger = parseSentinelRecoveryLedger(JSON.parse(decodeBase64(contents.content)));
  return { ledger, commit_sha: commitSha, tree_sha: treeSha, state_ref_exists: stateRefExists };
};

export const writeGitHubSentinelRecoveryLedger = async (
  input: Readonly<{
    token: string;
    repository: string;
    snapshot: SentinelRecoveryLedgerSnapshot;
    ledger: SentinelRecoveryLedgerV1;
    message: string;
    fetcher?: typeof fetch;
  }>,
): Promise<SentinelRecoveryLedgerSnapshot> => {
  if (!input.token || !SAFE_REPOSITORY.test(input.repository) || input.message.trim().length === 0) {
    throw new Error("GitHub recovery state write identity is invalid");
  }
  const fetcher = input.fetcher ?? fetch;
  const ledger = parseSentinelRecoveryLedger(input.ledger);
  const blobValue = record(
    (await request(input.token, input.repository, "/git/blobs", {
      method: "POST",
      body: JSON.stringify({ content: renderSentinelRecoveryLedger(ledger), encoding: "utf-8" }),
    }, fetcher)).value,
  );
  const blobSha = requiredSha(blobValue?.sha, "Sentinel recovery state blob SHA");
  const treeValue = record(
    (await request(input.token, input.repository, "/git/trees", {
      method: "POST",
      body: JSON.stringify({
        base_tree: input.snapshot.tree_sha,
        tree: [{ path: SENTINEL_RECOVERY_LEDGER_PATH, mode: "100644", type: "blob", sha: blobSha }],
      }),
    }, fetcher)).value,
  );
  const treeSha = requiredSha(treeValue?.sha, "Sentinel recovery state updated tree SHA");
  const commitValue = record(
    (await request(input.token, input.repository, "/git/commits", {
      method: "POST",
      body: JSON.stringify({ message: input.message, tree: treeSha, parents: [input.snapshot.commit_sha] }),
    }, fetcher)).value,
  );
  const commitSha = requiredSha(commitValue?.sha, "Sentinel recovery state commit SHA");
  const refPath = input.snapshot.state_ref_exists ? "/git/refs/heads/sentinel/recovery-state" : "/git/refs";
  await request(input.token, input.repository, refPath, {
    method: input.snapshot.state_ref_exists ? "PATCH" : "POST",
    body: JSON.stringify(
      input.snapshot.state_ref_exists
        ? { sha: commitSha, force: false }
        : { ref: SENTINEL_RECOVERY_STATE_REF, sha: commitSha },
    ),
  }, fetcher);
  return { ledger, commit_sha: commitSha, tree_sha: treeSha, state_ref_exists: true };
};
