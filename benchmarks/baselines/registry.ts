/**
 * Registry of the m03 baseline adapters (A, B, D).
 *
 * Every baseline flags `requiresExternalInference: true`, so the runner
 * refuses to execute them until an approved live-inference gate exists
 * (m03/m05 see benchmarks/README.md). The registry is a separate module on
 * purpose: `benchmarks/adapter.ts` is owned by m02 and keeps
 * `defaultAdapters()` hermetic; the orchestrator decides when to wire
 * `baselineAdapters()` into the shared runner registry.
 */

import { type BenchmarkAdapter } from "../adapter.ts";
import { adapterA } from "./adapter-a.ts";
import { adapterB } from "./adapter-b.ts";
import { adapterD } from "./adapter-d.ts";

export const BASELINE_CONFIG_IDS = ["A", "B", "D"] as const;
export type BaselineConfigId = (typeof BASELINE_CONFIG_IDS)[number];

/** A/B/D in stable config-id order. */
export function baselineAdapters(): BenchmarkAdapter[] {
  return [adapterA, adapterB, adapterD];
}

/**
 * Assertion helper for gate wiring: throws the same refusal the runner
 * raises, phrased for the baselines, when an external-inference adapter
 * escapes the gate. Used by focused tests and by the orchestrator's
 * integration tests.
 */
export function assertBaselinesRefusedByRunner(
  registry: BenchmarkAdapter[],
): void {
  for (const adapter of registry) {
    if (adapter.requiresExternalInference) {
      throw new Error(
        `refusing external-inference adapter ${adapter.configId} (${adapter.name}) outside the approved gate`,
      );
    }
  }
}
