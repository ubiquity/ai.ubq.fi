/** Hash-bound one-shot native observer. No credential file or arbitrary RPC enters its stdin. */
import { lstatSync } from "node:fs";
import { adoptionRefused, boundedOperatorText, bytesSha256, canonicalOwnedDirectory, readBoundFile } from "./managed-native-operator-io.ts";

export type NativeOwnerObservation = Readonly<{
  v: 1;
  codex_home: string;
  peer_uid: number;
  peer_pid: number;
  account_kind: "chatgpt" | "masked";
  requires_openai_auth: boolean;
}>;
export type NativeObserverSeal = Readonly<{ path: string; sha256: string; home: string; uid: number; pid: number; timeoutMs?: number }>;
export const decodeNativeOwnerObservation = (value: unknown, seal: NativeObserverSeal): NativeOwnerObservation => {
  if (!value || typeof value !== "object" || Array.isArray(value)) adoptionRefused("observer_receipt");
  const row = value as Record<string, unknown>;
  const keys = ["v", "codex_home", "peer_uid", "peer_pid", "account_kind", "requires_openai_auth"];
  if (
    Object.keys(row).length !== keys.length ||
    !keys.every((k) => Object.hasOwn(row, k)) ||
    row.v !== 1 ||
    row.codex_home !== seal.home ||
    row.peer_uid !== seal.uid ||
    row.peer_pid !== seal.pid ||
    typeof row.requires_openai_auth !== "boolean"
  )
    adoptionRefused("observer_receipt");
  if (row.account_kind !== "chatgpt" && !(row.account_kind === "masked" && !row.requires_openai_auth)) adoptionRefused("observer_account_kind");
  return row as NativeOwnerObservation;
};

export class NativeOwnerObserver {
  readonly #seal: NativeObserverSeal;
  readonly #identity: string;
  private constructor(seal: NativeObserverSeal) {
    this.#seal = Object.freeze({ ...seal });
    this.#identity = this.#binary();
  }
  /** Called only by immutable launcher source literals, never decoded private input. */
  static fromLauncher(seal: NativeObserverSeal): NativeOwnerObserver {
    if (
      process.platform !== "linux" ||
      seal.uid !== process.getuid?.() ||
      !Number.isSafeInteger(seal.pid) ||
      seal.pid <= 0 ||
      !/^[a-f0-9]{64}$/.test(seal.sha256) ||
      (seal.timeoutMs !== undefined && (!Number.isSafeInteger(seal.timeoutMs) || seal.timeoutMs < 1 || seal.timeoutMs > 60_000))
    )
      adoptionRefused("observer_binding");
    canonicalOwnedDirectory(seal.home);
    return new NativeOwnerObserver(seal);
  }
  #binary(): string {
    const file = readBoundFile(this.#seal.path, 256 * 1024 * 1024, false);
    const mode = lstatSync(this.#seal.path).mode;
    if ((mode & 0o100) === 0 || (mode & 0o222) !== 0 || bytesSha256(file.bytes) !== this.#seal.sha256) adoptionRefused("observer_binary");
    return file.identity;
  }
  async observe(): Promise<NativeOwnerObservation> {
    if (this.#binary() !== this.#identity) adoptionRefused("observer_binary_changed");
    const child = Bun.spawn([this.#seal.path], { env: { PATH: "/usr/bin:/bin" }, stdin: "pipe", stdout: "pipe", stderr: "pipe" });
    const timer = setTimeout(() => {
      child.kill("SIGKILL");
    }, this.#seal.timeoutMs ?? 60_000);
    try {
      await child.stdin.write(JSON.stringify({ v: 1, codex_home: this.#seal.home, expected_uid: this.#seal.uid }));
      await child.stdin.end();
      const [output, errors, code] = await Promise.all([boundedOperatorText(child.stdout, 4096), boundedOperatorText(child.stderr, 1024), child.exited]);
      if (code !== 0 || errors || this.#binary() !== this.#identity) adoptionRefused("observer_failed");
      let raw: unknown;
      try {
        raw = JSON.parse(output);
      } catch {
        adoptionRefused("observer_receipt");
      }
      return decodeNativeOwnerObservation(raw, this.#seal);
    } catch {
      child.kill("SIGKILL");
      await child.exited;
      adoptionRefused("observer_failed");
    } finally {
      clearTimeout(timer);
    }
  }
}
