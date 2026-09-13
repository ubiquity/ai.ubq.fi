export type RemovedProviderCircuitProbe = Readonly<Record<string, never>>;
export const selectRemovedProviderCircuitRoute = (): Promise<{
  route: "codex";
  probe: null;
  transition: "none";
}> => Promise.resolve({ route: "codex", probe: null, transition: "none" });
export const claimRemovedProviderEarlyRecoveryProbe = (): Promise<null> => Promise.resolve(null);
export const closeRemovedProviderCircuit = (_probe: RemovedProviderCircuitProbe | null): Promise<"none"> => Promise.resolve("none");
export const recordRemovedProviderEligibleFailure = (_probe: RemovedProviderCircuitProbe | null): Promise<"none"> => Promise.resolve("none");
export const releaseRemovedProviderCircuitProbe = (_probe: RemovedProviderCircuitProbe | null): Promise<"none"> => Promise.resolve("none");
export const renewRemovedProviderCircuitProbe = (_probe: RemovedProviderCircuitProbe): Promise<void> => Promise.resolve();
