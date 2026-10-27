import { abortBeforeOutput, precommitCases, precommitPaidCases, PRECOMMIT_CASE_NAMES } from "./precommit.ts";
import { runStreamRuntimeCases, STREAM_RUNTIME_CASE_NAMES } from "./index.ts";
import { EARLY_HEAD_CASE_NAMES, runEarlyHeadWireCases } from "./early-head.ts";
import type { MatrixHarness } from "./contracts.ts";
import type { Selection } from "./conformance-receipt.ts";

/** The test-only selectors run the same named cases and preserve their exact order. */
export async function runSelectedRuntimeCases(harness: MatrixHarness, selector: string | undefined): Promise<Selection | null> {
  if (selector === "--early-head-wire-only") {
    await runEarlyHeadWireCases(harness);
    return { name: "early-head-wire", expectedCases: EARLY_HEAD_CASE_NAMES };
  }
  if (selector === "--presemantic-abort-only") {
    await harness.withScenario("precommit", abortBeforeOutput);
    return { name: "presemantic-abort", expectedCases: ["matrix_synthetic_presemantic_caller_abort_never_advances"] };
  }
  if (selector === "--precommit-only") {
    await harness.withScenario("precommit", precommitCases);
    await harness.withScenario("precommit-paid", precommitPaidCases);
    return { name: "synthetic-precommit", expectedCases: PRECOMMIT_CASE_NAMES };
  }
  if (selector === "--stream-runtime-only") {
    await runStreamRuntimeCases(harness);
    return { name: "stream-runtime", expectedCases: STREAM_RUNTIME_CASE_NAMES };
  }
  return null;
}
