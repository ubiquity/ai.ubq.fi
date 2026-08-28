import {
  reconcileSentinelBootstrap,
  type SentinelBootstrapControllerDependencies,
  type SentinelBootstrapReconcileInput,
  type SentinelBootstrapReconcileOutcome,
} from "./controller.ts";
import { parseBootstrapEnvironment } from "./policy.ts";

/**
 * The workflow invokes this entry point from the protected development ref.
 * State and observation wiring is supplied by the orchestrator through the
 * typed controller function; the process entry point itself has no write
 * access to the repository or deployment systems.
 */
export const runProtectedBootstrap = async (
  input: SentinelBootstrapReconcileInput,
  dependencies: SentinelBootstrapControllerDependencies,
): Promise<SentinelBootstrapReconcileOutcome> => {
  const environment = parseBootstrapEnvironment();
  return await reconcileSentinelBootstrap({
    ...input,
    repository: environment.repository,
    ref: environment.ref,
  }, dependencies);
};

if (import.meta.main) {
  const environment = parseBootstrapEnvironment();
  // Keep the protected entry point useful as a cheap, side-effect-free
  // workflow preflight until the orchestrator supplies durable state wiring.
  console.log(JSON.stringify({
    schema_version: 1,
    status: "protected_bootstrap_preflight_passed",
    repository: environment.repository,
    ref: environment.ref,
    sha: environment.sha,
    run_id: environment.runId,
  }));
}
