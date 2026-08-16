export type RemovedProviderCircuitProbe = Readonly<Record<never, never>>;
export const REMOVED_PROVIDER_CIRCUIT_KEY = ["uos_ai", "removed_provider", "circuit", "v1"] as const;
export const parseRemovedProviderCircuitState = (_value: unknown): null => null;
export const selectRemovedProviderCircuitRoute = (): Promise<{
  route: "codex";
  probe: null;
  transition: "none";
}> => Promise.resolve({ route: "codex", probe: null, transition: "none" });
export const claimRemovedProviderEarlyRecoveryProbe = (): Promise<null> => Promise.resolve(null);
export const closeRemovedProviderCircuit = (_probe: RemovedProviderCircuitProbe | null): Promise<"none"> =>
  Promise.resolve("none");
export const recordRemovedProviderEligibleFailure = (
  _probe: RemovedProviderCircuitProbe | null,
): Promise<"none"> => Promise.resolve("none");
export const releaseRemovedProviderCircuitProbe = (_probe: RemovedProviderCircuitProbe | null): Promise<"none"> =>
  Promise.resolve("none");
export const renewRemovedProviderCircuitProbe = (_probe: RemovedProviderCircuitProbe): Promise<void> =>
  Promise.resolve();
export const getRemovedProviderCircuitView = (): Promise<Record<string, never>> => Promise.resolve({});
