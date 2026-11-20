# Native runtime and package

The C++20 gateway serves on Linux ARM64. The portable archive targets ARMv8-A with glibc 2.36 or newer.
The Apple Silicon archive supplies the offline version check and native development build. [Release packages](RELEASES.md)

## Build and package

Follow [CI.md](CI.md) to prepare the dependencies and run scoped release tests in Linux namespaces. To build an
installable archive from a committed Linux ARM64 checkout with Docker Buildx, use a new path outside the checkout.

```sh
bash cpp/gateway/packaging/portable/build.sh /var/tmp/uos-gateway-linux-arm64.tar.gz
bash cpp/gateway/packaging/portable/verify.sh /var/tmp/uos-gateway-linux-arm64.tar.gz /var/tmp/uos-gateway-proof
```

The archive contains the C++ executable, browser assets, both favicons, licenses, the launcher and package metadata.
On the target host, `seal.py` verifies the payload and binds the canonical loader and shared libraries to the release.
The local Arch CMake `package` target binds the building host instead. It takes a qualified release build and a new
`UOS_PACKAGE_OUTPUT` below the checkout's canonical `.data`. [Native build profile](BUILD.md) · [Install and seal](RELEASES.md)

The sealed directory contains `bin/uos-gateway`, `static/`, the favicons, `licenses/`, `ops/launch-native.sh`,
`runtime/launch-schema.json`, `package-info.json`, `build-profile.json`, `runtime-closure.json`,
`operator-closure.json` and `manifest.sha256`. The manifest covers every other regular file in relative path order.
The package metadata binds the build profile. The runtime closure records the interpreter, library paths and hashes,
and ELF version requirements. Seal into a new directory after a shared-library change. [Vendor registry](../cmake/vendors.json)

## Input limits

The shared constants in [limits.hpp](../include/uos/rt/limits.hpp) record the target defaults. The enforced limits below apply at the parser, library or transport boundary. Keep smaller existing limits on the same operation and record a larger allowance only for a proven application flow. Map failures to the boundary's established status, close code or exception. Source wiring and executed acceptance are recorded separately below.

| Boundary                  | Target default                                | Enforcement and qualification                                                                                                                                                                                                                                                                      |
| ------------------------- | --------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| HTTP header block         | 32 KiB                                        | The server refuses heads above 16,408 bytes or 198 fields with 431. The WebSocket client handshake head caps bytes at 16 KiB. Fetch uses a 1.5 MiB buffered-head guard.                                                                                                                            |
| HTTP request body         | 16 MiB of wire bytes                          | The application JSON reader and Decisions measure declared and received wire bytes. Decisions maps oversize to HTTP 413. Passkey, System One and live-call routes keep their lower caps of 64 KiB, 256 KiB and 1 MiB respectively.                                                                 |
| WebSocket message         | 16 MiB                                        | `Receiver::haveLength` checks cumulative frame lengths before reading payload bytes. The Beast adapter uses 100 MiB by default, with 4 MiB for live sideband, 1 MiB for the Codex supervisor and 64 KiB for native auth. [Stage 2a results](findings/cppgw-integrate-s2a-20261118.md)              |
| WebSocket idle            | 60 s                                          | Stage 2a exercises the existing handshake and close timers. The generic post-upgrade idle deadline still needs implementation and qualification.                                                                                                                                                   |
| Regex                     | 4 KiB pattern, 1 MiB generic subject, subject-sized work budget | QuickJS-ng libregexp uses a bounded count of interrupt callbacks, with larger subject allowances for fixed application patterns. [Regex limits](SECURITY-LIMITS.md#regular-expressions) |
| JSON document and nesting | 16 MiB of decoded UTF-8, depth 128            | simdjson applies both limits. A raw parse rejection raises `SyntaxError`. Request adapters preserve each route's error mapping. The image-edit exception follows below.                                                                                                                            |
| Passkey CBOR              | 1 MiB, depth 8                                | QCBOR and its adapter enforce both bounds and strict CTAP2 canonical form. The outer passkey request also has a 64 KiB cap.                                                                                                                                                                        |
| X.509 inputs              | 64 KiB per certificate, 256 KiB per chain     | The OpenSSL adapter validates DER structure and uses the existing 16 MiB input, 100-level nesting and 10,000-element limits. The tighter certificate and chain defaults still need qualification.                                                                                                  |

**Documented exceptions.** Image edits keep a wire and decoded JSON allowance of 70,953,644 bytes (about 67.7 MiB) for a 50 MiB image plus Base64 and metadata, and a 64 MiB wire limit for multipart. The route supplies the larger JSON allowance in C++ and compares the original wire size and decoded text separately. Overflow after UTF-8 decoding maps to invalid JSON through the existing body adapter. Sentinel capture keeps its independent 32 MiB storage cap even when the image request is admissible.

The WebSocket limit fixture sets a 1,024-byte message budget and verifies zero delivery and the route-defined 1011 close when a continuation header declares too many bytes. The fixture passed under Release and ASan on Linux ARM64. [Stage 2a results](findings/cppgw-integrate-s2a-20261118.md#test-decisions)

After each integration, update its row with the actual source entry point, effective limits and exception, boundary-test outcome and exact binary or tree. The final package includes the resulting table. [v40 implementation scope](/tmp/claude-1002/prod/assignment3/runtime/limits-v40/runtime-table.md)

## Launch and state

The executable serves with `--runtime-manifest <absolute-path>`. The launch manifest selects
`uos-gateway-launch-v1`, the canonical `packageRoot` and private `stateRoot`, the lowercase
`packageManifestSha256`, `hostname` equal to `127.0.0.1`, and an integer `port` other than 7999 or 8000.
Use disjoint package and state roots. The launch file must be owner-private, regular and singly linked. The
state root and its existing `.data` directory must be canonical and owner-private. [Launch schema](../runtime/launch-schema.json)

The launcher also accepts `<absolute-manifest-path> --import-codex-auth-from <absolute-state-directory>` in follower mode. This one-shot command verifies the same package, applies Linux confinement, and reads the source database under a read-only grant. It uses the Codex usage endpoint to validate incoming access tokens before updating the stopped follower's auth pool. [Side-by-side procedure](CUTOVER.md#running-side-by-side-on-one-host)

### Linux confinement

The synchronous bootstrap verifies the launch and package manifests, the executing binary, and the host-library closure. It loads the configured TLS trust stores and installs Landlock and the existing seccomp filter before starting the HTTP transport, event loop, or SQLite worker. The package and the existing host runtime paths are readable. By default, only `stateRoot/.data` is writable. Seccomp is thread-synchronized and denies subprocess execution by default. The reviewed launcher and its environment control the resources available before enforcement.

The default policy requires Linux ARM64 with Landlock ABI 8 or newer and applies the existing filesystem policy using Landlock thread synchronization. On a host with ABI 3–7, the operator can explicitly enable the reduced policy. Landlock must be enabled in the kernel, and seccomp with thread synchronization is required.

```sh
UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1 /absolute/path/to/package/ops/launch-native.sh /absolute/path/to/launch.json
```

For a service launch, put the assignment in the approved `EnvironmentFile=`. For a fixture unit that constructs its environment with `env -i`, supply the assignment explicitly. The launcher reads the exact value `1` from its process environment during bootstrap. It queries the ABI from Landlock itself, so kernel backports and kernel configuration determine the available controls.

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

After enforcement, the gateway changes cwd to `stateRoot`, sets `SQLITE_TMPDIR` to the private `.data`, and
serves immutable assets from `packageRoot`. The linked C++ SQLite store opens the existing
`.data/kv-bun.sqlite3` after startup checks privacy and the store checks the `bun-jsc-v1` marker. The legacy
`.data/kv.sqlite3` stays in its original custody. SIGTERM or SIGINT drains requests and durable writes, then
closes telemetry and KV. [Durable KV](rt/kv-durable.md)

The only optional manifest field is `fixture:{syntheticOrigin:"http://127.0.0.1:<assigned-port>"}`. It requires a non-deploy runtime, gateway port 0, a real no-outbound network namespace, private task state and a loopback scripted upstream. `DENO_TIMELINE=production` is permitted inside these fences so the fixture exercises ordinary authentication instead of the source's legacy local-hostname bypass; deployment markers remain forbidden. The fixed DeepSeek completion URL maps to `/chat/completions`, Codex maps to `/codex`, and live WebSocket sideband maps to `/live`. The real transport rejects unmapped initial destinations and forces the existing `redirect:"error"` mode for every fixture fetch, so a scripted-upstream redirect cannot introduce a second destination. Normal transport redirect behavior is unchanged. Provider request schemas and authentication are unchanged. Routing ownership lasts through the full serve lifetime. An initial fixture requires an empty `.data`; after all immutable and OS checks it exclusively writes a mode-0600 binding marker. Reopen requires that exact owner-private marker binding schema, package-manifest hash, stateRoot and syntheticOrigin. Markerless nonempty state is refused. This provides no authority to read production state.

## Environment

The gateway reads the process environment supplied by the approved launcher. For systemd, use an explicit
owner-controlled `EnvironmentFile=`. The binary does not discover dotenv files. The following switches include
the process flags and the environment read by the native build and test tooling.

| Switch | Default | Effect |
| --- | --- | --- |
| `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT` | Unset | Exact value `1` opts in to the available Landlock policy starting at ABI 3. Startup still requires seccomp TSYNC. |
| `UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER` | Unset | Exact value `1` serves Codex with the copied KV auth pool, refuses online credential refresh and pool writes, and disables the Codex app-server connection and Antigravity. `GET /health` reports follower status and time to the earliest known Codex access-token expiry. |
| `UOS_GATEWAY_ENABLE_ANTIGRAVITY` | Unset | Exact value `1` opts in to the owner-installed Antigravity CLI integration and its child-process profile when the CLI passes validation. |
| `UOS_GATEWAY_ACTIVE_BUILD` | Set by `build.py` (compiler wrapper falls back to its working directory) | Locates the active profile owner and generated test registry. |
| `UOS_GATEWAY_BUILD_DIR` | Set by the native build owner | Selects the CMake presets' build directory. |
| `UOS_GATEWAY_BUILD_EVIDENCE` | Required and set by the build owner | Locates native build receipts and instruction snapshots. CMake refuses direct configuration without it. |
| `UOS_GATEWAY_BUILD_PROFILE` | Set by the native build owner | Identifies the selected profile for compiler and link admission checks. |
| `UOS_GATEWAY_CCACHE` | Empty when the owner disables caching | Selects the ccache executable for the compiler wrapper. |
| `UOS_GATEWAY_INSTRUCTION_SNAPSHOT` | Unset | Carries the reviewed instruction snapshot to bounded build or test subprocesses. |
| `UOS_GATEWAY_NOTES_SHA256` | Unset (required with an inherited reviewed snapshot) | Binds the instruction snapshot to the reviewed hash. |
| `UOS_GATEWAY_OUT` | `.data/cpp-gateway` under the repository | Selects output for the legacy single-test shell driver. |
| `UOS_GATEWAY_RESOURCE_LOG` | Unset | Selects the supervisor resource log for build and test admission. |
| `UOS_GATEWAY_RUN_ID` | Set by the build or test owner | Binds instructions and resource receipts to an invocation. |
| `UOS_GATEWAY_TOOL_ROOT` | Set by the native build owner | Selects the qualified compiler and dependency root for the native CMake profile. |
| `UOS_GATEWAY_TOOL_VIEW` | Set by the build owner | Selects the CMake profile's task-owned tool wrappers. |

| Environment surface              | Existing names                                                                                                                                                                                                                                                        |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime and identity             | `DENO_DEPLOY`, `DENO_DEPLOYMENT_ID`, `DENO_REGION`, `DENO_TIMELINE`, `DENO_DEPLOY_BUILD_ID`, `DENO_DEPLOY_APP_SLUG`, `DENO_DEPLOY_ORG_SLUG`                                                                                                                           |
| Gateway and native authority     | `UOS_AI_TOKEN`, `DENO_DEPLOY_TOKEN`, `CODEX_AUTH_JSON_B64`, `CODEX_HOME`, `HOME`, `UOS_AI_KERNEL_PUBLIC_KEY`                                                                                                                                                          |
| Provider credentials and scope   | `DEEPSEEK_API_KEY`, `SURPLUS_API_KEY`, `METERED_API_KEY`, `OPENLUX_API_KEY`, `OPENROUTER_API_KEY`, `CEREBRAS_API_KEY`, `LITHOSAI_API_KEY`, `VOYAGEAI_API_KEY`, `CLAUDE_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_WORKSPACE_ID`, `CLAUDE_CODE_ALLOWED_KEY_IDS` |
| Capture, TLS and process helpers | `SENTINEL_REPLAY_KEY`, `SSL_CERT_FILE`, `NODE_EXTRA_CA_CERTS`, `NODE_TLS_REJECT_UNAUTHORIZED`, `PATH`, `TMPDIR`, `TMP`, `TEMP`; startup supplies the existing SQLite-library variable `SQLITE_TMPDIR`                                                                 |

OpenSSL also reads its standard trust-store environment. The native TLS adapter retains its existing environment
names for client compatibility. Provider test environments use fresh home and temporary directories, synthetic
credentials and loopback mocks. [Test isolation](CI.md)

### Enable the Gemini subscription

1. Install the official Linux ARM64 Antigravity CLI 1.5.11 or a verified compatible version. Place the actual executable at `$HOME/.local/bin/agy` with mode `0700`, owned by the gateway OS user.
2. Launch `agy` interactively as that user and complete the CLI's sign-in. Run `agy models` and confirm it lists `gemini-3.8-flash-high`, `gemini-3.8-flash-medium` and `gemini-3.8-flash-low`. Keep `$HOME/.gemini/antigravity-cli` and any `$HOME/.gemini/config` and `$HOME/.config/antigravity` directories owner-private and separate from the gateway state. The gateway checks the metadata of the CLI's owner-private `antigravity-oauth-token` file. [Official CLI documentation](https://antigravity.google/docs/cli/)
3. Set `UOS_GATEWAY_ENABLE_ANTIGRAVITY=1` in the approved launch environment and restart. Gemini becomes available for text-only Responses and Chat Completions once the installation and CLI state pass validation. Admin Analytics shows the measured subscription buckets and exposes an explicit `/usage` refresh. The CLI owns credential renewal, account changes and credit settings.

The exact opt-in releases seccomp's process-creation and exec denials for the entire gateway process. Seccomp applies that exception to every gateway thread. Landlock allows direct execution of the canonical `agy` executable and its canonical dynamic loader. A second Landlock ruleset applies in the dedicated spawning thread and carries into the CLI. The gateway also gains read and write access to the CLI state. The child sees the individual runtime libraries and host files it needs, its private CLI state and optional configuration, and its own workspace. It receives four environment variables and standard I/O pipes, or `/dev/null` for unused stdin. Its only writable directories are its own CLI state and workspace.

The dynamic loader can open compatible ELF files anywhere the calling process can read. The opted-in gateway retains its ordinary readable library and writable data paths, so the loader can still reach a readable ELF there. The child's narrower ruleset removes that access to gateway state and the general library tree. Loadable content in the child's own readable and writable CLI state and workspace remains in scope. Keep the flag unset or remove it and restart to run with the original no-child-process policy.

Follower mode takes precedence over the Gemini opt-in and keeps process execution denied. The startup log identifies the switch and explains why Antigravity is disabled.

## Cutover requirements

Verify the target host, release identity and a consistent stopped database copy. Rehearse a rollback that opens
the latest writes. The historical conversion and round-trip tooling lives at [STATE A](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891).
Follow the owner's [cutover checklist](CUTOVER.md) and preserve the required credential, ingress and Sentinel-reader approvals.
