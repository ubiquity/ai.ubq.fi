export const WORKER_ACCOUNT_ID = "5c0228bee348f4cf48829ffe21b61fe9";
export const WORKER_COMPATIBILITY_DATE = "2026-10-18";
export const WORKER_COMPATIBILITY_FLAGS = ["enable_request_signal", "request_signal_passthrough"];

const workers = {
  candidate: "uos-ai-rust-candidate",
  "rehearsal-20261106": "uos-ai-rust-rehearsal-20261106",
  "production-20261106": "uos-ai-rust-production-20261106",
} as const;

export type WorkerTarget = { mode: keyof typeof workers; worker: string };

export function workerTarget(mode = "candidate"): WorkerTarget {
  if (!Object.hasOwn(workers, mode)) throw new Error(`Unknown Rust Worker mode: ${mode}`);
  const selected = mode as keyof typeof workers;
  return { mode: selected, worker: workers[selected] };
}

export function workerTargetArguments(args: readonly string[]): WorkerTarget {
  if (args.length === 0) return workerTarget();
  if (args.length === 2 && args[0] === "--mode") return workerTarget(args[1]);
  if (args.length === 1 && args[0]?.startsWith("--mode=")) return workerTarget(args[0].slice(7));
  throw new Error("Only one standard cf --mode argument is permitted.");
}

export function workerDeploymentArguments(args: readonly string[]): { target: WorkerTarget; secretsFile?: string } {
  let mode: string | undefined;
  let secretsFile: string | undefined;
  for (let index = 0; index < args.length; index++) {
    const flag = args[index];
    if (flag !== "--mode" && flag !== "--secrets-file") throw new Error("Only standard cf --mode and --secrets-file arguments are permitted.");
    const value = args[++index];
    if (!value || value.startsWith("--")) throw new Error("A cf flag value is required.");
    if (flag === "--mode") {
      if (mode !== undefined) throw new Error("Duplicate cf mode.");
      mode = value;
    } else {
      if (secretsFile !== undefined) throw new Error("Duplicate cf secrets file.");
      secretsFile = value;
    }
  }
  return { target: workerTarget(mode), secretsFile };
}
