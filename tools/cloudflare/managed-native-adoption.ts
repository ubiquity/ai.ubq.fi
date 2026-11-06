/** Private immutable-launcher composition. Direct CLI entry never opens storage or reads credentials. */
import { Database } from "bun:sqlite";
import { lstatSync, realpathSync, existsSync } from "node:fs";
import { join } from "node:path";
import { userInfo } from "node:os";
import { randomUUID } from "node:crypto";
import { type Kv, openKv, prepareNativeKvBackend } from "#kv";
import { CODEX_AUTH_POOL_KV_KEY, parseCodexAuthFromAuthJson } from "../../src/codex/auth.ts";
import {
  executeManagedNativeAdoption,
  executeOwnedPrimaryNativeAdoption,
  ManagedAdoptionError,
  type ManagedAdoptionPort,
  type ManagedAdoptionReceipt,
} from "../../src/codex/managed-native-adoption.ts";
import { executeFullPoolNativeAdoption, type FullPoolAdoptionReceipt } from "../../src/codex/full-pool-native-adoption.ts";
import { createNativeSnapshotReader, verifyNativeSchema } from "../../src/kv-conversion/native-schema.ts";
import { CutoverHost, boundedCutoverHolderCheck } from "../../ops/cutover-host.ts";
import { readCutoverExecStart, assertCutoverExecStart } from "../../ops/cutover-unit.ts";
import { acquireDeployLock } from "../../ops/deploy-lock.ts";
import { BUN_RUNTIMES, verifyRuntimeBinary } from "../../ops/bun-runtime.ts";
import { readNativeKvReleaseManifest, nativeKvReleaseBinding, verifyNativeKvReleasePayload } from "../../ops/native-kv-release.ts";
import { NativeOwnerObserver, type NativeObserverSeal, type NativeOwnerObservation } from "./managed-native-owner-observer.ts";
import {
  adoptionRefused,
  boundedOperatorText,
  bytesSha256,
  canonicalOwnedDirectory,
  decodeManagedAdoptionInput,
  readBoundFile,
  PrivateOutputAnchor,
  revalidateManagedAdoptionInput,
  type ManagedAdoptionInput,
} from "./managed-native-operator-io.ts";

export type ManagedAdoptionLauncherFacts = Readonly<{
  root: string;
  home: string;
  accountSha256: string;
  sourceSha: string;
  sourceManifestSha256: string;
  outputDirectory: string;
  observer: NativeObserverSeal;
}>;
export const assertManagedAdoptionUnit = (fields: Record<string, string>, root: string, username: string): void => {
  if (fields.WorkingDirectory !== root || fields.User !== username || fields.ActiveState !== "inactive" || fields.MainPID !== "0")
    adoptionRefused("source_unit_not_stopped");
};
export const assertManagedAdoptionHolders = (code: number, output: string, errors: string, opened: boolean, pid: number): void => {
  if (errors || ![0, 1].includes(code)) adoptionRefused("holder_inspection");
  const holders = output.trim() ? output.trim().split(/\s+/) : [];
  if (code === 0 && holders.length === 0) adoptionRefused("holder_inspection");
  if (holders.some((holder) => !/^\d+$/.test(holder) || !opened || Number(holder) !== pid) || (code === 1 && holders.length !== 0))
    adoptionRefused("source_holder_remains");
};
/** Only this fixed key is exposed to the transaction kernel; no arbitrary key/path is an input. */
export const managedAdoptionKvPort = (kv: Pick<Kv, "get" | "atomic">, hooks: Omit<ManagedAdoptionPort, "read" | "commit">): ManagedAdoptionPort => ({
  ...hooks,
  read: async () => {
    const row = await kv.get(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" });
    if (!row.versionstamp || row.value === null) adoptionRefused("source_missing");
    return { pool: row.value, versionstamp: row.versionstamp };
  },
  commit: async (versionstamp, pool) => (await kv.atomic().check({ key: CODEX_AUTH_POOL_KV_KEY, versionstamp }).set(CODEX_AUTH_POOL_KV_KEY, pool).commit()).ok,
});
const prepared = new WeakSet<PreparedManagedAdoption>();
type AdoptionRole = "owned-primary" | "unowned-secondary";
type DeploymentLease = Readonly<{ root: string; sourceSha: string; manifestSha256: string; members: readonly PreparedManagedAdoption[] }>;
const activeLeases = new WeakSet<DeploymentLease>();
const inside = (path: string, root: string): boolean => path === root || path.startsWith(root + "/");

export class PreparedManagedAdoption {
  readonly #facts: ManagedAdoptionLauncherFacts;
  readonly #observer: NativeOwnerObserver;
  readonly #host: CutoverHost;
  readonly #db: string;
  readonly #dbIdentity: string;
  readonly #output: PrivateOutputAnchor;
  readonly #role: AdoptionRole;
  #lease: DeploymentLease | null = null;
  #opened = false;
  #nativeIdentity: string | null = null;
  #nativeHash: string | null = null;
  #peer: NativeOwnerObservation | null = null;
  private constructor(facts: ManagedAdoptionLauncherFacts, observer: NativeOwnerObserver, role: AdoptionRole) {
    this.#facts = Object.freeze({ ...facts, observer: Object.freeze({ ...facts.observer }) });
    this.#observer = observer;
    this.#role = role;
    this.#host = new CutoverHost(facts.root);
    this.#db = join(facts.root, ".data/kv-bun.sqlite3");
    this.#dbIdentity = this.#databaseIdentity();
    this.#output = PrivateOutputAnchor.open(facts.root, facts.outputDirectory);
    prepared.add(this);
  }
  /** Source literals in the accepted immutable private launcher are the authority, never stdin. */
  static fromLauncher(facts: ManagedAdoptionLauncherFacts): PreparedManagedAdoption {
    return PreparedManagedAdoption.#fromLauncher(facts, "unowned-secondary");
  }
  static fromOwnedPrimaryLauncher(facts: ManagedAdoptionLauncherFacts): PreparedManagedAdoption {
    return PreparedManagedAdoption.#fromLauncher(facts, "owned-primary");
  }
  static #fromLauncher(facts: ManagedAdoptionLauncherFacts, role: AdoptionRole): PreparedManagedAdoption {
    let binding: PreparedManagedAdoption | undefined;
    try {
      if (
        process.platform !== "linux" ||
        Bun.version !== "1.4.0" ||
        !/^[a-f0-9]{40}$/.test(facts.sourceSha) ||
        !/^[a-f0-9]{64}$/.test(facts.sourceManifestSha256) ||
        !/^[a-f0-9]{64}$/.test(facts.accountSha256)
      )
        adoptionRefused("binding_identity");
      for (const path of [facts.root, join(facts.root, ".data"), facts.home, facts.outputDirectory]) canonicalOwnedDirectory(path);
      if (
        (lstatSync(facts.home).mode & 0o077) !== 0 ||
        (lstatSync(facts.outputDirectory).mode & 0o077) !== 0 ||
        inside(facts.outputDirectory, facts.home) ||
        facts.outputDirectory === join(facts.root, ".data") ||
        facts.observer.home !== facts.home
      )
        adoptionRefused("binding_scope");
      binding = new PreparedManagedAdoption(facts, NativeOwnerObserver.fromLauncher(facts.observer), role);
      binding.#source();
      return binding;
    } catch {
      if (binding) binding.#output.close();
      adoptionRefused("binding_invalid");
    }
  }
  #databaseIdentity(): string {
    if (realpathSync(this.#db) !== this.#db) adoptionRefused("database_identity");
    const stat = lstatSync(this.#db);
    if (!stat.isFile() || stat.nlink !== 1 || stat.uid !== process.getuid?.() || (stat.mode & 0o777) !== 0o600) adoptionRefused("database_identity");
    return [stat.dev, stat.ino, stat.uid, stat.mode, stat.nlink].join(":");
  }
  #source() {
    const release = join(this.#facts.root, ".data/releases", this.#facts.sourceSha);
    if (realpathSync(join(this.#facts.root, ".data/current")) !== release) adoptionRefused("source_release_changed");
    const bytes = readBoundFile(join(release, ".uos-release.json"), 1024 * 1024, false).bytes;
    if (bytesSha256(bytes) !== this.#facts.sourceManifestSha256) adoptionRefused("source_manifest_changed");
    const manifest = JSON.parse(bytes.toString()) as Record<string, unknown>;
    if (
      manifest.schema !== 3 ||
      manifest.git_sha !== this.#facts.sourceSha ||
      manifest.platform !== "linux" ||
      manifest.architecture !== "arm64" ||
      JSON.stringify(manifest.runtime) !== JSON.stringify(BUN_RUNTIMES.linux)
    )
      adoptionRefused("source_manifest_invalid");
    const launcher = join("ops", "launch-vps.sh");
    if (
      bytesSha256(readBoundFile(join(this.#facts.root, launcher), 128 * 1024, false).bytes) !==
      bytesSha256(readBoundFile(join(release, launcher), 128 * 1024, false).bytes)
    )
      adoptionRefused("source_launcher_changed");
    return { release, native: readNativeKvReleaseManifest(manifest.native_kv, "linux") };
  }
  async #unit(): Promise<void> {
    const text = await this.#host.run("sudo", [
      "-n",
      "systemctl",
      "show",
      "ai-ubq-fi.service",
      "-p",
      "WorkingDirectory",
      "-p",
      "User",
      "-p",
      "ActiveState",
      "-p",
      "MainPID",
      "-p",
      "FragmentPath",
    ]);
    const fields = Object.fromEntries(text.split("\n").map((line) => line.split("=")));
    assertManagedAdoptionUnit(fields, this.#facts.root, userInfo().username);
    if (!fields.FragmentPath || realpathSync(fields.FragmentPath) !== join(this.#facts.root, "ops/ai-ubq-fi.service")) adoptionRefused("source_unit_identity");
    assertCutoverExecStart(await readCutoverExecStart(this.#facts.root, this.#host.system.run), ["/bin/sh", join(this.#facts.root, "ops/launch-vps.sh")]);
  }
  async #guard(): Promise<void> {
    this.assertDeploymentLease();
    this.#output.assert();
    this.#source();
    await this.#unit();
    if (this.#databaseIdentity() !== this.#dbIdentity) adoptionRefused("database_changed");
    if (!this.#opened) await this.#host.assertStopped([this.#db]);
    else {
      const paths = ["", "-wal", "-shm"].map((s) => this.#db + s).filter(existsSync);
      const child = Bun.spawn(["/usr/bin/lsof", "-t", "--", ...paths], { env: {}, stdout: "pipe", stderr: "pipe" });
      const result = await boundedCutoverHolderCheck(child);
      assertManagedAdoptionHolders(result.code, result.output, result.errors, true, process.pid);
    }
    if (this.#peer) await this.#proveNative();
  }
  #native() {
    canonicalOwnedDirectory(this.#facts.home);
    const file = readBoundFile(join(this.#facts.home, "auth.json"), 1024 * 1024, true);
    const hash = bytesSha256(file.bytes);
    if (this.#nativeIdentity !== null && (file.identity !== this.#nativeIdentity || hash !== this.#nativeHash)) adoptionRefused("native_file_changed");
    this.#nativeIdentity = file.identity;
    this.#nativeHash = hash;
    return { raw: JSON.parse(file.bytes.toString()) as unknown, sha256: hash };
  }
  async #proveNative(): Promise<void> {
    const before = this.#native();
    const auth = parseCodexAuthFromAuthJson(before.raw);
    if (!auth || bytesSha256(Buffer.from(auth.account_id)) !== this.#facts.accountSha256) adoptionRefused("native_account_changed");
    this.#peer = await this.#observer.observe();
    if (this.#native().sha256 !== before.sha256) adoptionRefused("native_file_changed");
  }
  assertDeploymentLease(): void {
    const lease = this.#lease;
    if (
      !lease ||
      !activeLeases.has(lease) ||
      !lease.members.includes(this) ||
      lease.root !== this.#facts.root ||
      lease.sourceSha !== this.#facts.sourceSha ||
      lease.manifestSha256 !== this.#facts.sourceManifestSha256
    )
      adoptionRefused("deployment_lease_required");
  }
  dispose(): void {
    if (this.#lease && activeLeases.has(this.#lease)) adoptionRefused("active_deployment_lease");
    this.#output.close();
    prepared.delete(this);
  }
  static #fullPoolMembers(primary: PreparedManagedAdoption, secondary: PreparedManagedAdoption): readonly PreparedManagedAdoption[] {
    if (!prepared.has(primary) || !prepared.has(secondary)) adoptionRefused("immutable_launcher_required");
    if (
      primary === secondary ||
      primary.#role !== "owned-primary" ||
      secondary.#role !== "unowned-secondary" ||
      primary.#facts.root !== secondary.#facts.root ||
      primary.#facts.sourceSha !== secondary.#facts.sourceSha ||
      primary.#facts.sourceManifestSha256 !== secondary.#facts.sourceManifestSha256 ||
      primary.#dbIdentity !== secondary.#dbIdentity ||
      primary.#facts.home === secondary.#facts.home ||
      primary.#facts.accountSha256 === secondary.#facts.accountSha256
    )
      adoptionRefused("full_pool_binding_scope");
    return [primary, secondary];
  }
  static async #withLease<T>(members: readonly PreparedManagedAdoption[], callback: () => Promise<T>): Promise<T> {
    if (members.length < 1 || members.length > 2 || typeof callback !== "function" || members.some((member) => !prepared.has(member)))
      adoptionRefused("deployment_lease_scope");
    if (members.some((member) => member.#lease !== null)) adoptionRefused("nested_deployment_lease");
    for (const member of members) {
      member.#source();
      member.#output.assert();
    }
    const first = members[0];
    readBoundFile(join(first.#facts.root, ".data/deploy.lock"), 4096, true);
    const release = await acquireDeployLock(join(first.#facts.root, ".data/deploy.lock"));
    const lease = Object.freeze({
      root: first.#facts.root,
      sourceSha: first.#facts.sourceSha,
      manifestSha256: first.#facts.sourceManifestSha256,
      members: Object.freeze([...members]),
    });
    activeLeases.add(lease);
    for (const member of members) member.#lease = lease;
    const terminal = `window-terminal-${randomUUID()}.json`;
    try {
      const result = await callback();
      first.#output.write(terminal, { v: 1, kind: "managed_adoption_window", status: "callback_completed", source_sha: first.#facts.sourceSha });
      return result;
    } catch {
      first.#output.write(terminal, { v: 1, kind: "managed_adoption_window", status: "callback_failed_recovery_required", source_sha: first.#facts.sourceSha });
      return adoptionRefused("window_failed_recovery_required");
    } finally {
      activeLeases.delete(lease);
      for (const member of members) member.#lease = null;
      release();
      for (const member of members) member.dispose();
    }
  }
  withDeploymentLock<T>(callback: () => Promise<T>): Promise<T> {
    return PreparedManagedAdoption.#withLease([this], callback);
  }
  static async withFullPoolDeploymentLock<T>(primary: PreparedManagedAdoption, secondary: PreparedManagedAdoption, callback: () => Promise<T>): Promise<T> {
    return await PreparedManagedAdoption.#withLease(PreparedManagedAdoption.#fullPoolMembers(primary, secondary), callback);
  }
  static async executeFullPool(
    primary: PreparedManagedAdoption,
    secondary: PreparedManagedAdoption,
    primaryValue: ManagedAdoptionInput,
    secondaryValue: ManagedAdoptionInput
  ): Promise<FullPoolAdoptionReceipt> {
    const members = PreparedManagedAdoption.#fullPoolMembers(primary, secondary);
    for (const member of members) member.assertDeploymentLease();
    if (primary.#lease !== secondary.#lease) adoptionRefused("foreign_deployment_lease");
    const a = revalidateManagedAdoptionInput(primaryValue),
      b = revalidateManagedAdoptionInput(secondaryValue);
    if (primary.#facts.outputDirectory === secondary.#facts.outputDirectory && new Set([...Object.values(a.outputs), ...Object.values(b.outputs)]).size !== 6)
      adoptionRefused("full_pool_output_overlap");
    for (const [member, input] of [
      [primary, a],
      [secondary, b],
    ] as const)
      for (const path of Object.values(input.outputs)) if (member.#output.exists(path)) adoptionRefused("output_exists");
    const record = `full-pool-partial-${randomUUID()}.json`;
    return await executeFullPoolNativeAdoption(
      {
        primary: () => primary.execute(a),
        secondary: (expected) => secondary.execute({ ...b, expected }),
        verifyBoth: async () => {
          await primary.#guard();
          await secondary.#guard();
        },
        recordPartial: (receipt) => {
          primary.#output.write(record, receipt);
          return Promise.resolve();
        },
      },
      b.expected.nativeSha256
    );
  }
  async execute(value: ManagedAdoptionInput): Promise<ManagedAdoptionReceipt> {
    const input = revalidateManagedAdoptionInput(value);
    this.assertDeploymentLease();
    for (const path of Object.values(input.outputs)) if (this.#output.exists(path)) adoptionRefused("output_exists");
    let kv: Kv | undefined;
    let phase = "before_open";
    try {
      await this.#guard();
      const source = this.#source();
      await verifyRuntimeBinary(process.execPath, BUN_RUNTIMES.linux);
      await verifyNativeKvReleasePayload(source.release, "linux", source.native);
      const reader = createNativeSnapshotReader(new Database(this.#db, { readonly: true, strict: true, safeIntegers: true }));
      try {
        verifyNativeSchema(reader, "bun-jsc-v1");
      } finally {
        reader.close();
      }
      await this.#guard();
      prepareNativeKvBackend(nativeKvReleaseBinding(source.release, source.native));
      kv = await openKv(this.#db);
      this.#opened = true;
      phase = "opened";
      const port = managedAdoptionKvPort(kv, {
        assertOwned: () => this.#guard(),
        native: () => Promise.resolve(this.#native()),
        proveNative: () => this.#proveNative(),
        backup: (snapshot) => {
          this.#output.write(input.outputs.backup, { v: 1, key: CODEX_AUTH_POOL_KV_KEY, ...snapshot });
          phase = "backed_up";
          return Promise.resolve();
        },
        intent: (plan) => {
          this.#output.write(input.outputs.intent, {
            v: 1,
            kind: "managed_adoption_intent",
            expected: input.expected,
            before_sha256: plan.beforeSha256,
            after_sha256: plan.afterSha256,
            account_sha256: plan.accountSha256,
          });
          phase = "intent_written";
          return Promise.resolve();
        },
      });
      const commit = port.commit;
      const execute = this.#role === "owned-primary" ? executeOwnedPrimaryNativeAdoption : executeManagedNativeAdoption;
      const result = await execute(
        {
          ...port,
          commit: async (stamp, pool) => {
            phase = "cas_dispatched";
            return await commit(stamp, pool);
          },
        },
        { accountSha256: this.#facts.accountSha256, slot: this.#role === "owned-primary" ? 1 : 2, home: this.#facts.home },
        input.expected,
        Date.now()
      );
      this.#output.write(input.outputs.receipt, { ...result, peer: this.#peer, source_sha: this.#facts.sourceSha });
      return result;
    } catch (error) {
      const code = error instanceof ManagedAdoptionError ? error.code : "operator_failed";
      this.#output.write(input.outputs.receipt, {
        v: 1,
        kind: "managed_adoption_failure",
        phase,
        code,
        recovery_required: phase === "cas_dispatched",
      });
      return adoptionRefused(code);
    } finally {
      kv?.close();
      this.#opened = false;
    }
  }
}
export const runManagedNativeAdoptionOperator = async (
  binding: PreparedManagedAdoption | undefined,
  stream: ReadableStream<Uint8Array>
): Promise<ManagedAdoptionReceipt> => {
  if (!binding || !prepared.has(binding)) adoptionRefused("immutable_launcher_required");
  try {
    const input = decodeManagedAdoptionInput(await boundedOperatorText(stream, 4096));
    return await binding.execute(input);
  } catch (error) {
    adoptionRefused(error instanceof ManagedAdoptionError ? error.code : "operator_failed");
  }
};
if (import.meta.main) {
  console.error("Managed native adoption requires the accepted immutable private launcher");
  process.exitCode = 78;
}
