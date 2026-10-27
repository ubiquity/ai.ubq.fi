import { runEarlyHeadWireCases } from "./early-head.ts";
import { decisionsCases } from "./decisions.ts";
import type { MatrixHarness } from "./contracts.ts";
import { codexProviderCases } from "./codex.ts";
import { directProviderCases } from "./direct.ts";
import { claudeProviderCases } from "./claude.ts";
import { openluxProviderCases, paidProviderCases, unknownPaidCases } from "./paid.ts";
import { cacheBodyRejection } from "./cache.ts";
import { precommitCases, precommitPaidCases } from "./precommit.ts";
import { streamSettlementCases } from "./stream-settlement.ts";
import { claudeReplayCases } from "./claude-replay.ts";
import { streamTerminationCases } from "./stream-termination.ts";
import { lithosRefusalCases } from "./lithos-refusal.ts";

export const STREAM_RUNTIME_CASE_NAMES = [
  "matrix_native_surplus_sse_usage_settles_exact_cost_once_without_delivery_claim",
  "matrix_claude_json_native_replay_restored_only_for_original_principal_cohort",
  "matrix_claude_stream_native_replay_restored_only_for_original_principal_cohort",
  "matrix_native_first_terminal_closes_and_cancels_hanging_upstream_once",
  "matrix_lithos_responses_native_429_is_in_band_without_paid_fallback",
  "matrix_lithos_chat_native_429_is_in_band_without_paid_fallback",
];

export async function runStreamRuntimeCases(context: MatrixHarness): Promise<void> {
  await context.withScenario("paid", streamSettlementCases);
  await context.withScenario("claude", claudeReplayCases);
  await context.withScenario("codex", streamTerminationCases);
  await context.withScenario("direct", lithosRefusalCases);
}

/** Definitions become evidence only after the actual compiled Rust entrypoint executes them. */
export async function runProviderMatrixCases(context: MatrixHarness): Promise<void> {
  await context.withScenario("precommit", precommitCases);
  await context.withScenario("precommit-paid", precommitPaidCases);
  await runStreamRuntimeCases(context);
  await runEarlyHeadWireCases(context);
  await context.withScenario("direct", async (context) => {
    await decisionsCases(context);
    await cacheBodyRejection(context);
    await directProviderCases(context);
  });
  await context.withScenario("codex", codexProviderCases);
  await context.withScenario("claude", claudeProviderCases);
  await context.withScenario("paid", paidProviderCases);
  await context.withScenario("paid", openluxProviderCases);
  await context.withScenario("paid", unknownPaidCases);
}

export type { MatrixContext, MatrixHarness, MatrixScenario } from "./contracts.ts";
