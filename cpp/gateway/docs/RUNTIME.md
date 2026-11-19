# Native gateway runtime

The packaged service runs on Linux ARM64. The portable archive uses ARMv8-A and a glibc 2.36 baseline, and is sealed against the libraries on the installation host. The Arch Linux ARM native profile builds against its prepared host toolchain and binds that host's library closure during assembly. Apple Silicon builds support development tests and the packaged offline version check. [Release installation](RELEASES.md) · [Arch build profile](BUILD.md) · [Raspberry Pi](RASPBERRY-PI.md) · [macOS](MACOS.md)

## Build and package

The portable build uses Clang 21 on Debian 12. It statically links Boost.JSON and Boost.Container 1.92.0, SQLite 3.54.0 with `SQLITE_DQS=3`, ICU 78.3 with its data, libstdc++ and libgcc. The six vendored libraries include simdjson for On-Demand JSON parsing, simdutf for Base64 and Unicode encoding, llhttp for HTTP parsing, Ada for URLs, QCBOR for passkeys and QuickJS-ng libregexp for regular expressions. OpenSSL 3, zlib, Brotli, Zstd and the ARM64 GNU C runtime come from the host. The archive's `VERSION` records the exact build versions. [Library versions](../README.md#libraries-in-use) · [Portable build](../packaging/portable/Dockerfile) · [Vendor registry](../cmake/vendors.json)

Sealing validates the payload and binds the executable hashes, ELF interpreter, direct and transitive host-library paths and hashes, symbol versions and permitted system DSO runpaths. Executable search paths and missing host libraries fail verification. Seal a new release after a host library changes. The Arch profile uses its own host closure and may need newer ABI symbols than the portable archive. [Sealer](../packaging/portable/seal.py) · [Library closure tool](../tools/runtime-manifest.py)

### Arch host-bound build

Keep compiler work within two global slots and one full release build at a time. Link admission requires 3.5 GiB of `MemAvailable`, compile and archive admission require 3 GiB, and other build operations require 2 GiB. The live hard stop is below 2 GiB. Compiler wrappers keep the 3 GiB link and 2 GiB compile address-space caps. An admission deferral waits 30 seconds between attempts for up to 20 minutes per target. Build admission also requires more than 8 GiB of free disk.

During the final bounded CMake configure for `release`, set `UOS_PACKAGE_NATIVE_KV_DIR`, `UOS_PACKAGE_NATIVE_KV_INPUTS`, `UOS_PACKAGE_NATIVE_KV_PROVENANCE_SHA256`, `UOS_PACKAGE_PERMISSIONS_DIR` and `UOS_PACKAGE_OUTPUT` explicitly. Set the last value to a new directory under the worktree's canonical `.data`. After sealing the three primary executable hashes, check the generated `package` target's arguments and confirm that the prerequisites are up to date. Run the bounded target from the source checkout with the reviewed supervisor resource log under umask `077`.

```sh
/usr/bin/python3 cpp/gateway/tools/build.py native release package --supervisor-log "$supervisor_log"
```

The target depends on `uos_gateway`, `uos_kv_convert` and `uos_kv_proof` and runs the package producer with the configured inputs. For the second pass, use umask `022` and supply the same paths and provenance hash with a second new output directory.

```sh
/usr/bin/python3 cpp/gateway/tools/package.py \
  --build-dir "$build_dir" \
  --profile-manifest "$build_dir/profile-owner.json" \
  --native-kv "$native_kv_dir" \
  --native-kv-inputs "$native_kv_inputs" \
  --native-kv-provenance-sha256 "$native_kv_provenance_sha256" \
  --permissions "$permissions_dir" \
  --output "$second_output_dir"
```

The package producer verifies successful native KV provenance against the frozen `@deno/kv` 0.14.0 inputs at source commit `9fe65cc`, including the hashes, AArch64 ELF and Node-API registration. It also checks the AArch64 confinement artifact against its manifest. Missing or corrupt inputs refuse publication. If copying has started, the producer retains the partial directory for diagnosis.

The sealed packages contain `bin/{uos-gateway,uos-kv-convert,uos-kv-proof}`, immutable web assets, native KV and permissions artifacts, launch schemas, the pinned V8 helper and its input source, licenses, the launcher, a build profile, runtime and operator closures, and `manifest.sha256`. The portable build first assembles an unsealed payload, and its sealer writes the runtime and operator closures on the target host. In the Arch host-bound build, the producer also copies this document and the draft operator files. The manifest covers every other regular file in the sealed tree. Assembly fixes file modes and normalizes timestamps. [Portable payload](../packaging/portable/payload.py) · [Arch package producer](../tools/package.py)

## Input limits

| Boundary            | Effective limits                                                                                                                                                                                                                    |
| ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| HTTP request heads  | 16,408 bytes and 198 fields. Overflow returns HTTP 431.                                                                                                                                                                             |
| HTTP request bodies | 16 MiB of wire bytes by default. Passkeys, System One and live calls have 64 KiB, 256 KiB and 1 MiB route caps respectively.                                                                                                        |
| JSON                | 16 MiB of decoded UTF-8 and depth 128 by default.                                                                                                                                                                                   |
| Image edits         | 70,953,644 bytes of wire or decoded JSON and 64 MiB of multipart wire bytes. The decoded image limit is 50 MiB.                                                                                                                     |
| Fetch               | 1.5 MiB and 256 fields for heads, 16 MiB each for compressed and decoded bodies, and 8 MiB per decoded SSE line.                                                                                                                    |
| WebSocket           | 16 KiB for the client handshake head, 16 MiB and 4,096 data frames per message, and a 60-second idle deadline after upgrade. Live sideband, Codex supervisor and native auth use 4 MiB, 1 MiB and 64 KiB message caps respectively. |
| Regular expressions | 4 KiB pattern, 1 MiB subject and 10 ms compile deadline. Generic matching gets 250 ms. Fixed large-subject patterns get up to 1,800 ms and the fixed supervisor credential-key pattern gets 1,250 ms.                               |
| Passkey CBOR        | 1 MiB and depth 8 for the codec, within the outer 64 KiB route limit.                                                                                                                                                               |
| X.509               | 64 KiB per DER certificate and 256 KiB per attestation chain. Upstream TLS caps the peer certificate list at 64 KiB. ASN.1 has a separate 16 MiB, 100-level and 10,000-element budget.                                              |

See the library entry points, route exceptions and boundary tests in [Security limits](SECURITY-LIMITS.md) and the shared [limit constants](../include/uos/rt/limits.hpp).

## Launch and offline tools

The real gateway executable accepts exactly `--runtime-manifest <absolute-path>`. There is no default source-checkout or development-port fallback. The closed JSON schema at `runtime/launch-schema.json` requires `schema:"uos-gateway-launch-v1"`, canonical `packageRoot`, canonical private `stateRoot`, lowercase `packageManifestSha256`, lowercase `nativeKvProvenanceSha256`, `hostname:"127.0.0.1"` and integer `port`. Select a free port outside the reserved ports 7999 and 8000. The manifest is an owner-private regular file, not a symlink or hard link. Both stateRoot and its pre-existing `.data` directory must be canonical and owner-private; immutable package and mutable state roots must be disjoint.

Both packaged offline executables require the prefix `--runtime-manifest <absolute-path>` before their original arguments. Their separate closed schema is `uos-gateway-offline-v1` with only `schema`, `packageRoot`, `packageManifestSha256` and `nativeKvProvenanceSha256`; see `runtime/offline-schema.json`. Preflight verifies the immutable package, the actual selected CLI binary, host closure and genuine native backend, then prepares that backend and selects the immutable resource root. It opens no source/destination database, creates no state, preserves the caller's cwd and argv0, and removes only the two prefix tokens before the unchanged CLI argument parser. Relative data/proof paths keep their original meaning. The codec helper URL uses resourceRoot while existing unconfigured module tests retain cwd behavior.

The offline tools launch the independently hash-pinned Deno V8 helper under OS-enforced filesystem, network and process confinement. They require a stopped source and a new destination, with explicit receipts and independent verification. Production conversion requires separate authorization. [Offline schema](../runtime/offline-schema.json) · [Cutover and rollback](CUTOVER.md)

### Linux confinement

The synchronous bootstrap verifies the launch and package manifests, the executing binary, the host-library closure, and the native KV and permissions artifacts. It loads the configured TLS trust stores and installs Landlock and the existing seccomp filter before starting the HTTP transport, event loop, or SQLite worker. The package and the existing host runtime paths are readable. By default, only `stateRoot/.data` is writable. Seccomp is thread-synchronized and denies subprocess execution by default. The reviewed launcher and its environment control the resources available before enforcement.

The default policy requires Linux ARM64 with active Landlock ABI 8 or newer and uses Landlock thread synchronization. The operator can explicitly enable ABI 3–7 through the [environment switch below](#environment). Seccomp thread synchronization remains required. Kernel backports and configuration determine the installed Landlock ABI.

| Landlock ABI | First Linux release | Protections with the opt-in                                                                                        |
| -----------: | ------------------: | ------------------------------------------------------------------------------------------------------------------ |
|          0–2 |                   — | Startup refuses                                                                                                    |
|            3 |                 6.2 | Filesystem restrictions, including rename/link and truncation, plus seccomp TSYNC and Landlock inheritance         |
|            4 |                 6.7 | ABI 3 protections plus TCP bind and connect restrictions                                                           |
|            5 |                6.10 | ABI 4 protections plus device `ioctl` restrictions                                                                 |
|            6 |                6.12 | ABI 5 protections plus scoping for abstract Unix sockets and signals                                               |
|            7 |                6.15 | ABI 6 protections. The kernel also supports audit controls. The launcher uses the kernel's default audit settings. |
|            8 |                 7.0 | ABI 6 protections with Landlock TSYNC                                                                              |
|           9+ |                 7.1 | ABI 8 protections plus the existing restriction on pathname Unix socket lookup                                     |

On ABI 3–7, the bootstrap verifies in `/proc/self/task` that it is the sole thread. The later resolver and SQLite threads inherit its Landlock domain. On ABI 4 and newer, the opt-in TCP policy permits binding the validated listener port, including `0` for kernel assignment. Outbound TCP permits DNS on port 53 and the gateway's HTTPS and WSS upstreams on port 443. An isolated fixture instead gets its verified loopback mock port. Landlock checks port numbers, so redirects to other TCP ports are denied under this policy.

The startup log and `LinuxPermissionsReceipt` report the queried ABI, active controls, and missing controls. A Pi with ABI 3 reports the filesystem filter, thread inheritance, and seccomp TSYNC as active. It reports TCP port controls, device `ioctl` restrictions, Unix socket controls, signal scoping, and Landlock TSYNC as missing. [Linux Landlock documentation](https://docs.kernel.org/userspace-api/landlock.html)

After verification and confinement, the launcher changes its working directory to `stateRoot` and sets `SQLITE_TMPDIR` to its private `.data`. The linked SQLite KV backend opens `.data/kv-bun.sqlite3` and checks the `bun-jsc-v1` marker before use. Startup requires the existing private marked store in normal service mode. SIGTERM and SIGINT drain admitted requests and durable writes before optional telemetry and KV close. [Startup](../runtime/startup.cpp) · [Shutdown](../src/serve.cpp)

The optional manifest field `fixture` accepts `syntheticOrigin` with an assigned IPv4 loopback mock port. It requires a no-outbound fixture namespace, an automatically assigned gateway port, private synthetic state and an environment without deployment markers. The fixture routes DeepSeek, Codex and the live WebSocket sideband to the local script, and refuses external destinations and redirects. It uses an exclusive owner-private state marker bound to the manifest, state path and mock origin for subsequent reopens. [Fixture startup](../runtime/startup.cpp) · [Paired suite](CONFORMANCE.md)

## Environment

The service uses an explicit operator-owned `EnvironmentFile=` in its reviewed systemd unit. The launcher uses the supplied process environment and the binary performs no automatic `.env` discovery. Set `DENO_TIMELINE=production` to disable the legacy localhost auth bypass. Review credential custody and parse the selected environment through systemd before a cutover. The side-by-side rehearsal unit constructs a sparse environment with `env -i`. [Service draft](../ops/uos-gateway-native.service) · [Shadow unit](../ops/side-by-side/uos-gateway-side-by-side.service.in)

| Variable                                | Default                                                                      | Effect                                                                                                                                                                                                                  |
| --------------------------------------- | ---------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT` | Unset                                                                        | An exact `1` permits Landlock ABI 3–7 with the kernel's available rules. Seccomp TSYNC remains required.                                                                                                                |
| `UOS_GATEWAY_ENABLE_ANTIGRAVITY`        | Unset                                                                        | An exact `1` enables the Gemini CLI integration after validating its installation and state. It enables gateway-wide process creation and exec syscalls with the additional execution and child filesystem rules below. |
| `UOS_GATEWAY_OUT`                       | `<checkout>/.data/cpp-gateway`                                               | Overrides the direct unit build script's output directory.                                                                                                                                                              |
| `UOS_GATEWAY_ROOT`                      | Canonical `cpp/gateway` path, set by `tools/env.sh`                          | Supplies paths to the direct unit build script.                                                                                                                                                                         |
| `UOS_GATEWAY_BUILD_DIR`                 | Set by the selected native build wrapper                                     | Selects the binary directory in the CMake presets.                                                                                                                                                                      |
| `UOS_GATEWAY_ACTIVE_BUILD`              | Set by the build driver                                                      | Supplies the active build path for the test registry and profile-owner receipt.                                                                                                                                         |
| `UOS_GATEWAY_BUILD_PROFILE`             | Set by the native profile                                                    | Selects the compiler wrapper's release-link admission checks.                                                                                                                                                           |
| `UOS_GATEWAY_CCACHE`                    | Managed `ccache` path when the native profile uses its default cache setting | An empty value disables compiler caching in the wrapper.                                                                                                                                                                |
| `UOS_GATEWAY_TOOL_ROOT`                 | Set by the selected native toolchain or portable build                       | Supplies the pinned native dependency and tool prefix to CMake.                                                                                                                                                         |
| `UOS_GATEWAY_TOOL_VIEW`                 | Set by the selected native or SDK profile                                    | Supplies the directory of pinned command wrappers to the CMake toolchain files.                                                                                                                                         |
| `UOS_GATEWAY_BUILD_EVIDENCE`            | Set by the build or test driver                                              | Supplies the compiler receipt directory and the explicit CMake build marker.                                                                                                                                            |
| `UOS_GATEWAY_RESOURCE_LOG`              | Unset until a supervisor log is selected                                     | Supplies the resource log for compiler and test admission.                                                                                                                                                              |
| `UOS_GATEWAY_INSTRUCTION_SNAPSHOT`      | Unset until a supervised run pins a snapshot                                 | Binds a child build or test run to that instruction snapshot.                                                                                                                                                           |
| `UOS_GATEWAY_NOTES_SHA256`              | Unset until a supervised run pins its notes                                  | Supplies the digest checked during supervised admission.                                                                                                                                                                |
| `UOS_GATEWAY_RUN_ID`                    | Generated for each build or test run                                         | Correlates evidence files and the instruction snapshot.                                                                                                                                                                 |

The gateway also reads its existing process settings. Keep TLS verification enabled and review any extra CA paths in the selected `EnvironmentFile=`. The inherited Fetch setting `NODE_TLS_REJECT_UNAUTHORIZED=0` disables peer verification. [Configuration](../src/config.cpp) · [Fetch TLS policy](../src/rt/fetch_transport_net.cpp)

| Environment surface              | Names                                                                                                                                                                                                                                                                 |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime and identity             | `DENO_DEPLOY`, `DENO_DEPLOYMENT_ID`, `DENO_REGION`, `DENO_TIMELINE`, `DENO_DEPLOY_BUILD_ID`, `DENO_DEPLOY_APP_SLUG`, `DENO_DEPLOY_ORG_SLUG`                                                                                                                           |
| Gateway and native authority     | `UOS_AI_TOKEN`, `DENO_DEPLOY_TOKEN`, `CODEX_AUTH_JSON_B64`, `CODEX_HOME`, `HOME`, `UOS_AI_KERNEL_PUBLIC_KEY`                                                                                                                                                          |
| Provider credentials and scope   | `DEEPSEEK_API_KEY`, `SURPLUS_API_KEY`, `METERED_API_KEY`, `OPENLUX_API_KEY`, `OPENROUTER_API_KEY`, `CEREBRAS_API_KEY`, `LITHOSAI_API_KEY`, `VOYAGEAI_API_KEY`, `CLAUDE_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_WORKSPACE_ID`, `CLAUDE_CODE_ALLOWED_KEY_IDS` |
| Capture, TLS and process helpers | `SENTINEL_REPLAY_KEY`, `SSL_CERT_FILE`, `NODE_EXTRA_CA_CERTS`, `NODE_TLS_REJECT_UNAUTHORIZED`, `PATH`, `TMPDIR`, `TMP`, `TEMP`. Startup sets `SQLITE_TMPDIR`.                                                                                                         |

### Enable the Gemini subscription

1. Install the official Linux ARM64 Antigravity CLI 1.5.11 or a verified compatible version. Place the actual executable at `$HOME/.local/bin/agy` with mode `0700`, owned by the gateway OS user.
2. Launch `agy` interactively as that user and complete the CLI's sign-in. Run `agy models` and confirm it lists `gemini-3.8-flash-high`, `gemini-3.8-flash-medium` and `gemini-3.8-flash-low`. Keep `$HOME/.gemini/antigravity-cli` and any `$HOME/.gemini/config` and `$HOME/.config/antigravity` directories owner-private and separate from the gateway state. The gateway checks the metadata of the CLI's owner-private `antigravity-oauth-token` file. [Official CLI documentation](https://antigravity.google/docs/cli/)
3. Set `UOS_GATEWAY_ENABLE_ANTIGRAVITY=1` in the approved launch environment and restart. Gemini becomes available for text-only Responses and Chat Completions once the installation and CLI state pass validation. Admin Analytics shows the measured subscription buckets and exposes an explicit `/usage` refresh. The CLI owns credential renewal, account changes and credit settings.

The exact opt-in releases seccomp's process-creation and exec denials for the entire gateway process. Seccomp applies that exception to every gateway thread. Landlock allows direct execution of the canonical `agy` executable and its canonical dynamic loader. A second Landlock ruleset applies in the dedicated spawning thread and carries into the CLI. The gateway also gains read and write access to the CLI state. The child sees the individual runtime libraries and host files it needs, its private CLI state and optional configuration, and its own workspace. It receives four environment variables and standard I/O pipes, or `/dev/null` for unused stdin. Its only writable directories are its own CLI state and workspace.

The dynamic loader can open compatible ELF files anywhere the calling process can read. The opted-in gateway retains its ordinary readable library and writable data paths, so the loader can still reach a readable ELF there. The child's narrower ruleset removes that access to gateway state and the general library tree. Loadable content in the child's own readable and writable CLI state and workspace remains in scope. Keep the flag unset or remove it and restart to run with the original no-child-process policy.

## Cutover requirements

The graph reader and typed conversion preserve sparse entries, shared references and cycles within the stored-value bounds. The graph repair passed the original cyclic proof on its tested revision. Projection to JSON still expands holes and rejects cycles. The dry run on the final platform must read a stopped production copy and prove that Bun can reopen the latest native-written state. The original Deno `.data/kv.sqlite3` stays separate, and a Deno rollback after Bun writes requires reverse conversion with a verified private receipt. [Fixture and cross-reader evidence](rt/kv-durable.md) · [Sparse and cyclic proof](findings/cppgw-kv-jsc-graph-model-20261117b.md) · [Cutover checklist](CUTOVER.md)
