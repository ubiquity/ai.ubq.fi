import {
  reconcileSentinelRecoveryRecord,
  type SentinelRecoveryDeliveryObservation,
  type SentinelRecoveryReconciliationAction,
  type SentinelRecoveryRemoteObservation,
} from "./issue-delivery-reconcile.ts";
import { sentinelRecoveryCandidateBranch } from "./issue-delivery.ts";
import {
  acquireSentinelRecoveryLease,
  nonTerminalSentinelRecoveryRecords,
  releaseSentinelRecoveryLease,
  sentinelRecoveryIdentityKey,
  upsertSentinelRecoveryRecord,
} from "./recovery-ledger.ts";
import {
  readGitHubSentinelRecoveryLedger,
  type SentinelRecoveryLedgerSnapshot,
  writeGitHubSentinelRecoveryLedger,
} from "./recovery-github-store.ts";
import type { SentinelRecoveryRecordV1 } from "./recovery.ts";
import { applySentinelRetryPolicyToRecovery, type SentinelFailureClass } from "./retry.ts";

const FULL_SHA = /^[0-9a-f]{40}$/u;
const MAX_RECORDS_PER_PASS = 32;
const LEASE_DURATION_MS = 10 * 60 * 1_000;

type JsonRecord = Record<string, unknown>;
const record = (value: unknown): JsonRecord | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as JsonRecord : null;

const request = async (
  input: Readonly<{ token: string; repository: string; path: string; fetcher: typeof fetch; allowNotFound?: boolean }>,
): Promise<Readonly<{ status: number; value: unknown }>> => {
  const response = await input.fetcher(`https://api.github.com/repos/${input.repository}${input.path}`, {
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${input.token}`,
      "X-GitHub-Api-Version": "2022-11-28",
    },
  });
  if (input.allowNotFound && response.status === 404) return { status: 404, value: null };
  const value = await response.json().catch(() => null);
  if (!response.ok) throw new Error(`GitHub recovery observation failed with HTTP ${response.status}`);
  return { status: response.status, value };
};

const refSha = (value: unknown): string => {
  const object = record(record(value)?.object);
  if (!object || typeof object.sha !== "string" || !FULL_SHA.test(object.sha)) {
    throw new Error("GitHub recovery ref response is invalid");
  }
  return object.sha;
};

const observeRemote = async (
  input: Readonly<{ token: string; repository: string; record: SentinelRecoveryRecordV1; fetcher: typeof fetch }>,
): Promise<SentinelRecoveryRemoteObservation | undefined> => {
  const branch = sentinelRecoveryCandidateBranch(input.record.identity);
  const branchRef = await request({
    ...input,
    path: `/git/ref/heads/${branch}`,
    allowNotFound: true,
  });
  // An encrypted artifact is still a live evidence source. Do not convert its
  // absent branch into a no-diff rejection before the artifact job runs.
  if (
    branchRef.status === 404 &&
    (input.record.artifact_ids.length > 0 || input.record.phase === "claimed" ||
      input.record.phase === "implementation_running" || input.record.phase === "workspace_dirty" ||
      input.record.phase === "checkpoint_publishing")
  ) return undefined;
  const candidateSha = branchRef.status === 404 ? null : refSha(branchRef.value);
  const developmentSha = refSha((await request({ ...input, path: "/git/ref/heads/development" })).value);
  const owner = input.repository.split("/", 1)[0]!;
  const pullsValue = (await request({
    ...input,
    path: `/pulls?state=all&base=development&head=${encodeURIComponent(`${owner}:${branch}`)}&per_page=100`,
  })).value;
  if (!Array.isArray(pullsValue)) throw new Error("GitHub recovery pull-request response is invalid");
  const deliveries: SentinelRecoveryDeliveryObservation[] = [];
  for (const value of pullsValue) {
    const pull = record(value);
    const head = record(pull?.head);
    const base = record(pull?.base);
    if (
      !pull || !head || !base || !Number.isSafeInteger(pull.number) || (pull.number as number) <= 0 ||
      head.ref !== branch || typeof head.sha !== "string" || !FULL_SHA.test(head.sha) || base.ref !== "development" ||
      (pull.state !== "open" && pull.state !== "closed") ||
      !(pull.merged_at === null || typeof pull.merged_at === "string")
    ) throw new Error("GitHub recovery pull-request identity is invalid");
    let contained = false;
    if (pull.merged_at !== null) {
      const comparison = record((await request({ ...input, path: `/compare/development...${head.sha}` })).value);
      contained = comparison?.status === "behind" || comparison?.status === "identical";
    }
    deliveries.push({
      pull_request_number: pull.number as number,
      head_branch: branch,
      head_sha: head.sha,
      base_branch: "development",
      head_contained_in_development: contained,
      state: pull.state,
      merged_at: pull.merged_at as string | null,
    });
  }
  return {
    candidate_branch: candidateSha === null ? null : branch,
    candidate_sha: candidateSha,
    development_sha: developmentSha,
    deliveries,
  };
};

export type SentinelRecoveryPassResult = Readonly<{
  identity_key: string;
  action: SentinelRecoveryReconciliationAction;
  state_version: number;
}>;

const retryFailureClass = (
  action: SentinelRecoveryReconciliationAction,
  record: SentinelRecoveryRecordV1,
): SentinelFailureClass | null => {
  if (action === "retry_checkpoint_push") return "git_publication_ambiguity";
  if (action === "await_evidence") return "runner_interruption";
  if (action === "resume_validation") return "validation_failure";
  const value = record.failure_class;
  return value === "capacity_quota" || value === "transient_transport" || value === "runner_interruption" ||
      value === "invalid_implementation_report" || value === "validation_failure" || value === "review_exhaustion" ||
      value === "git_publication_ambiguity" || value === "stale_source" || value === "unrecoverable_evidence"
    ? value
    : null;
};

export const runSentinelRecoveryPass = async (
  input: Readonly<{
    token: string;
    repository: string;
    owner: string;
    now?: string;
    fetcher?: typeof fetch;
  }>,
): Promise<readonly SentinelRecoveryPassResult[]> => {
  const fetcher = input.fetcher ?? fetch;
  const now = input.now ?? new Date().toISOString();
  if (!input.owner.trim() || !Number.isFinite(Date.parse(now))) {
    throw new Error("Sentinel recovery pass identity is invalid");
  }
  let snapshot: SentinelRecoveryLedgerSnapshot = await readGitHubSentinelRecoveryLedger({ ...input, fetcher });
  const results: SentinelRecoveryPassResult[] = [];
  for (const recoveryRecord of nonTerminalSentinelRecoveryRecords(snapshot.ledger).slice(0, MAX_RECORDS_PER_PASS)) {
    const key = sentinelRecoveryIdentityKey(recoveryRecord.identity);
    const currentLease = snapshot.ledger.leases.find((lease) => lease.identity_key === key);
    if (currentLease && currentLease.owner !== input.owner && Date.parse(currentLease.expires_at) > Date.parse(now)) {
      continue;
    }
    const token = `${input.owner}:${crypto.randomUUID()}`;
    const leased = acquireSentinelRecoveryLease(snapshot.ledger, {
      identity: recoveryRecord.identity,
      owner: input.owner,
      token,
      now,
      expires_at: new Date(Date.parse(now) + LEASE_DURATION_MS).toISOString(),
    });
    snapshot = await writeGitHubSentinelRecoveryLedger({
      ...input,
      fetcher,
      snapshot,
      ledger: leased,
      message: `chore(sentinel): lease recovery ${key}`,
    });
    const remote = await observeRemote({ ...input, fetcher, record: recoveryRecord });
    const reconciled = reconcileSentinelRecoveryRecord({
      record: recoveryRecord,
      ...(remote === undefined ? {} : { remote }),
      now,
      expected_state_version: recoveryRecord.state_version,
      expected_lease_token: recoveryRecord.lease_token,
    });
    let ledger = reconciled.changed
      ? upsertSentinelRecoveryRecord(snapshot.ledger, reconciled.after, recoveryRecord.state_version)
      : snapshot.ledger;
    let finalRecord = reconciled.after;
    const failureClass = retryFailureClass(reconciled.action, finalRecord);
    if (failureClass !== null && finalRecord.disposition === "active" && finalRecord.phase !== "retry_wait") {
      const history = ledger.retry_history.filter((attempt) => sentinelRecoveryIdentityKey(attempt.identity) === key)
        .slice(-8);
      const retry = await applySentinelRetryPolicyToRecovery({
        record: finalRecord,
        failure: {
          failure_class: failureClass,
          failure_fingerprint: finalRecord.failure_fingerprint,
          phase: finalRecord.phase,
          signature: finalRecord.reason ?? reconciled.action,
        },
        history,
        now,
      });
      if (retry.transitioned) {
        ledger = upsertSentinelRecoveryRecord(ledger, retry.after, finalRecord.state_version);
        finalRecord = retry.after;
      }
      ledger = {
        ...ledger,
        retry_history: [
          ...ledger.retry_history.filter((attempt) => sentinelRecoveryIdentityKey(attempt.identity) !== key),
          ...retry.history,
        ],
        retry_decisions: [
          ...ledger.retry_decisions.filter((entry) => entry.identity_key !== key),
          { identity_key: key, decision: retry.decision },
        ],
      };
    }
    ledger = releaseSentinelRecoveryLease(ledger, recoveryRecord.identity, token);
    snapshot = await writeGitHubSentinelRecoveryLedger({
      ...input,
      fetcher,
      snapshot,
      ledger,
      message: `chore(sentinel): reconcile recovery ${key}`,
    });
    results.push({ identity_key: key, action: reconciled.action, state_version: finalRecord.state_version });
  }
  return results;
};

if (import.meta.main) {
  const token = Deno.env.get("GITHUB_TOKEN")?.trim();
  const repository = Deno.env.get("GITHUB_REPOSITORY")?.trim();
  const owner = Deno.env.get("GITHUB_RUN_ID")?.trim();
  if (!token || !repository || !owner) throw new Error("Sentinel recovery workflow context is missing");
  console.log(JSON.stringify(await runSentinelRecoveryPass({ token, repository, owner })));
}
