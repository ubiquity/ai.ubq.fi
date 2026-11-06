/** Ordered two-step protocol. Concrete prepared bindings, not stdin, select both accounts. */
import { ManagedAdoptionError, type ManagedAdoptionExpectation, type ManagedAdoptionReceipt } from "./managed-native-adoption.ts";

export type FullPoolAdoptionReceipt = Readonly<{
  v: 1;
  kind: "full_pool_native_adoption";
  status: "completed";
  primary: ManagedAdoptionReceipt;
  secondary: ManagedAdoptionReceipt;
}>;
export type FullPoolPartialReceipt = Readonly<{
  v: 1;
  kind: "full_pool_native_adoption";
  status: "recovery_required";
  phase: "primary" | "secondary" | "final_fences";
  primary: ManagedAdoptionReceipt | null;
  secondary: ManagedAdoptionReceipt | null;
  primary_accepted: boolean;
  code: string;
  stale_rollback_permitted: false;
}>;
export type FullPoolAdoptionSteps = Readonly<{
  primary: () => Promise<ManagedAdoptionReceipt>;
  secondary: (expected: ManagedAdoptionExpectation) => Promise<ManagedAdoptionReceipt>;
  verifyBoth: () => Promise<void>;
  recordPartial: (receipt: FullPoolPartialReceipt) => Promise<void>;
}>;
const accepted = (receipt: ManagedAdoptionReceipt): boolean =>
  ["committed", "reconciled"].includes(receipt.outcome) && receipt.source_versionstamp !== receipt.current_versionstamp;

export const executeFullPoolNativeAdoption = async (steps: FullPoolAdoptionSteps, secondaryNativeSha256: string): Promise<FullPoolAdoptionReceipt> => {
  let primary: ManagedAdoptionReceipt | null = null;
  let secondary: ManagedAdoptionReceipt | null = null;
  let phase: FullPoolPartialReceipt["phase"] = "primary";
  try {
    primary = await steps.primary();
    if (!accepted(primary)) throw new ManagedAdoptionError("primary_not_applied");
    phase = "secondary";
    // A is already strongly read back by its kernel. B must match that exact accepted state.
    secondary = await steps.secondary({
      poolVersionstamp: primary.current_versionstamp,
      poolSha256: primary.after_sha256,
      nativeSha256: secondaryNativeSha256,
    });
    if (
      !accepted(secondary) ||
      secondary.source_versionstamp !== primary.current_versionstamp ||
      secondary.before_sha256 !== primary.after_sha256 ||
      secondary.account_sha256 === primary.account_sha256
    )
      throw new ManagedAdoptionError("secondary_not_accepted");
    phase = "final_fences";
    await steps.verifyBoth();
    return { v: 1, kind: "full_pool_native_adoption", status: "completed", primary, secondary };
  } catch (error) {
    const code = error instanceof ManagedAdoptionError ? error.code : "operation_requires_reconciliation";
    await steps.recordPartial({
      v: 1,
      kind: "full_pool_native_adoption",
      status: "recovery_required",
      phase,
      primary,
      secondary,
      primary_accepted: primary !== null && accepted(primary),
      code,
      stale_rollback_permitted: false,
    });
    throw new ManagedAdoptionError("full_pool_partial_recovery_required");
  }
};
