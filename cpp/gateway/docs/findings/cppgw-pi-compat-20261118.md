# Portable ARM64 gateway package findings

## Work contract v1

Build a portable release from `b54357a92c0f2669335d83f04047c3f7de32f925` for 64-bit Raspberry Pi OS Bookworm and Trixie. Compile on Debian 12 ARM64 against glibc 2.36, statically link the C++ runtime and libraries that require newer versions, and qualify the remaining Debian shared dependencies. Preserve the real packaged entrypoint, storage codec, authentication, accounting and OS confinement. The owned paths are `packaging/portable/`, `docs/RASPBERRY-PI.md` and this report. Application sources, production deployment, provider traffic and live state are outside this task.

Acceptance requires a one-command build, symbol and loader checks on clean Debian 12 and 13 ARM64, health and authenticated small chat with a loopback mock, and memory and throughput receipts with two CPUs at 512 MiB and 256 MiB. The source checkout lacks `bench/request_path/` and its performance report. The tracked `tests/production_smoke/` harness provides an existing packaged gateway launcher and scripted upstream. Discovery still needs to resolve the host-library closure, artifact preparation and kernel requirements before publishing a package.

## Work contract v2

The same three ARM64 executables are built in Debian 12 and sealed offline on each target with that target's actual shared-library paths and hashes. Container runs keep the normal executable and OS enforcement. The proof uses the packaged `ops/launch-native.sh` and the existing same-binary smoke client, process observer and mock server with a scripted Codex Responses route. The chat request uses an account seeded through the isolated authenticated admin route. The process receives no provider environment variables and the mock has no outbound network route. The forked benchmark from the prompt is absent on the supplied branch.

| Surface                              | Bookworm                                                                                                       | Trixie                              |
| ------------------------------------ | -------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| C library and loader                 | glibc 2.36                                                                                                     | glibc 2.41                          |
| Shared packages                      | `libssl3`, `zlib1g`, `libbrotli1`, `libzstd1`, `ca-certificates`                                               | Same package list with `libssl3t64` |
| Linked statically in the executables | libstdc++, libgcc, Boost 1.92.0 JSON/Container, simdjson 5.2.0, simdutf 9.4.0, SQLite 3.54.0 and ICU 78.3 data | Same executable bytes               |
| Health and inference                 | `/health` before and after authenticated small Chat Completions. Check durable usage after SIGTERM and reopen. | Same fixture                        |
| Cgroup gates                         | 512 MiB and 256 MiB, no swap, two CPUs, default Docker seccomp and gateway Landlock ABI 8+                     | Same limits and kernel              |

The external assembly command is `bash cpp/gateway/packaging/portable/build.sh /var/tmp/uos-gateway-arm64.tar.gz`. The proof is `bash cpp/gateway/packaging/portable/verify.sh /var/tmp/uos-gateway-arm64.tar.gz /var/tmp/uos-gateway-pi-evidence`. The changed shell and Python scripts get ShellCheck and syntax checks. The docs get the locked Prettier check. The package build, loader resolution and runtime matrix are the acceptance gates. Target kernel fallback and the old release builder are outside the owned paths.

## Discovery and build checkpoint

- Ubuntu 24.04.5 ARM64 runner, 4 vCPUs, 15 GiB RAM, Docker 28.0.4, cgroup v2, Linux `7.0.0-1017-azure`. The native ARM64 Landlock syscall returned ABI 8 both on the host and in a non-root capped Docker container with the default seccomp profile. That container reported `memory.max=268435456`, `memory.swap.max=0` and `cpu.max=200000 100000`.
- The tracked Arch closure generator only searches flat library directories. The portable sealer reuses it with Debian's multiarch directory and verifies the canonical results against the actual loader. `runtime/startup.cpp` requires those local paths and hashes at each start. An OS library update requires sealing a new directory from the verified portable archive.
- Bookworm's SQLite 3.40.1 and ICU 72.1 are too old for the checked storage and Unicode fixtures. Trixie also ships older SQLite 3.46.1 and ICU 76.1. The package pins SQLite with `SQLITE_DQS=3` and links ICU's data archive. Debian's OpenSSL, zlib, Brotli and Zstd supply the shared APIs.
- Bookworm's Clang 21.1.5 reports `-Wdeprecated-declarations` in libstdc++ 12's `bits/stl_tempbuf.h` while instantiating standard `stable_sort` in `authority/admission_state.cpp`. A Clang warning mapping limited to that vendor header keeps the first-party warning gates. Boost's Clang-specific build driver picked `arm64-pc-linux`, which omitted Bookworm's libstdc++ include paths. The package builds the two Boost archives with Bookworm's GCC 12 and the C++ gateway with Clang 21.
- The existing CMake build also exposed `simdutf.h`'s sign-conversion warnings to first-party `-Werror` because `base64.cpp` uses a tree-relative include. The portable CMake hook marks that translation unit's `third_party/` headers as system headers, matching the existing sanitizer and fuzz drivers.
- The native KV build used the existing frozen Bun 1.4.0 and Rust 1.98.1 build recipe inside Bookworm. The confinement artifact was compiled from the existing Node-API source with Debian's headers. No artifact is compiled or fetched on application startup.

## Kernel compatibility

Linux 6.6 and 6.12 expose Landlock ABI 3 and 6 upstream. ABI 8 appeared in Linux 7.0. The C++ runtime requires ABI 8 and seccomp, then installs both thread-synchronized policies before it opens state or listens. An older ABI yields `Linux confinement requires Landlock ABI8+ and seccomp thread synchronization`. Denied ABI queries report a syscall error and failed thread synchronization exits with status 78. The Bookworm and Trixie containers share the runner's ABI 8 kernel. [Upstream Landlock 7.0](https://github.com/torvalds/linux/blob/v7.0/security/landlock/syscalls.c)

Bookworm's kernel headers omit four newer ARM64 syscall names guarded by `#ifdef` in the existing Node-API confinement source. The package passes the same numbers already used by `cpp/gateway/src/runtime/permissions_linux.cpp` for `fchmodat2`, `setxattrat`, `removexattrat` and `file_setattr`, so the verified native artifact also retains those denials. The C++ executable uses the linked enforcement implementation.

Supporting an unmodified 6.6 kernel would require a runtime change. In `entrypoints/gateway.cpp` and `runtime/startup.cpp`, move manifest and permission preflight into synchronous bootstrap before initializing the transport or event loop. In `src/runtime/permissions_linux.cpp`, add a separately tested ABI 3 path that installs the same filesystem rights with `landlock_restrict_self(..., 0)` only while the process is provably single-threaded. Later threads must inherit the policy. Keep seccomp TSYNC, distinguish inherited Landlock confinement in the enforcement receipt, and fail closed if early single-thread enforcement is unavailable. This task leaves the ABI 8 requirement in place.

## Build and loader receipts

The one-command build and the scripted proof both exited 0. The build profile records commit `f132ee38bd3e4abaa52cab8097f34f94b390df43` and tree `6266e1e0521043ec49ccb807c2c32b9eb90ead7f`. Ninja completed all 381 build actions, including 369 application and runtime translation units and all three executable links. The successful CMake phase took 89.1 seconds with previously cached objects. The native artifact build phase took 162.1 seconds. The archive contains 66 files and is 82,953,740 bytes. The stripped gateway is 67,058,144 bytes.

```sh
work=/var/tmp/cppgw-pi.EIcUVv
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$work/tool-home" TMPDIR="$work/tool-tmp" \
  timeout --kill-after=10s 40m bash cpp/gateway/packaging/portable/build.sh "$work/final/uos-gateway-arm64.tar.gz"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$work/tool-home" TMPDIR="$work/tool-tmp" \
  bash cpp/gateway/packaging/portable/verify.sh "$work/final/uos-gateway-arm64.tar.gz" "$work/final/evidence"
```

| Artifact                   | SHA-256                                                            |
| -------------------------- | ------------------------------------------------------------------ |
| `uos-gateway-arm64.tar.gz` | `4a65d2ddf3fdb5c3a5964b2f6bdca2437fb1ed8c3a5e955a447f0ac18b91e00d` |
| `bin/uos-gateway`          | `c5349ae9bc1e50859db1b525ffaecc0aa6ab71fe4faf88777d2d62386dffa630` |
| `bin/uos-kv-convert`       | `51323cdb196a1f637b35325bb99a48c4cf0b64aecb2d2c5092f79a40c83f771f` |
| `bin/uos-kv-proof`         | `314e440b55f2954d2dc6daeea48fa22e6522b29f10d986915380b9151bb7423a` |
| Bookworm sealed manifest   | `f81287c6ac8a0c4d0efb7a0d0a2d8732c5f69347729d3cd4d1b0fdb2e35506be` |
| Trixie sealed manifest     | `cdcb4b67222a768c1e9e6ab91a41ebc89fe7cf120ad494692674f7f22fbc19ce` |

Each image resolved nine canonical host objects, including the loader. The three executables imported symbols through `GLIBC_2.36`. They had zero `GLIBCXX`/`CXXABI` imports or RPATH/RUNPATH entries. Each distro's `ldd -v` and `ldd -r` resolved every executable's versions and relocations. Version checks also passed for both Node-API artifacts. The KV artifact imports `libgcc_s.so.1` from Debian's base `libgcc-s1`. The C++ gateway opens the linked SQLite store directly.

The observed OpenSSL, zlib, Brotli and Zstd versions were 3.0.22, 1.2.13, 1.0.9 and 1.5.4 on Bookworm, and 3.5.7, 1.3.1, 1.1.0 and 1.5.7 on Trixie. `elf.txt` in each evidence directory includes full package revisions and raw `readelf` and `ldd` output. Both images used the untouched Docker seccomp profile, all capabilities dropped, and `--network=none`. The gateway added its own seccomp filter and started with Landlock ABI 8. The existing checked-in release indicator reports `unknown` in `/health`. The build profile, sealed manifest and executable digest bind these local proofs to the exact source and binary.

## Small-chat memory and throughput

Each row used a fresh container with two CPUs and swap disabled. Health returned 200 before and after the workload and on reopen. Chat without auth returned 401. Each row checked 1,161 successful chats against the mock's 16-token completion and matched all 1,161 settled requests after SIGTERM and reopen. The mock processed one initial chat, eight requests held concurrently for 240 ms, and three batches of 384 with eight client workers. The throughput batches used no simulated provider delay. Rates include loopback transport, client and mock overhead, and the gateway's storage and accounting work.

RSS is sampled from the gateway's `/proc/<pid>/status` every 20 ms. The idle and held-request columns report sample medians. The timed-load RSS is the largest sample over the three batches. The cgroup peak includes the client and mock and accounts for shared file-backed pages differently from process RSS.

| Image | Cap (MiB) | Cold idle RSS (MiB) | Warm idle RSS (MiB) | Eight held RSS (MiB) | Timed peak RSS (MiB) | Cgroup peak (MiB) | Requests/s median (range) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Debian 12 | 512 | 22.45 | 31.89 | 33.95 | 37.12 | 39.16 | 353.1 (340.1–357.6) |
| Debian 12 | 256 | 22.44 | 31.88 | 33.94 | 37.32 | 36.34 | 343.7 (339.7–350.5) |
| Debian 13 | 512 | 23.78 | 33.16 | 35.29 | 38.60 | 39.58 | 358.6 (345.3–363.5) |
| Debian 13 | 256 | 23.78 | 33.16 | 35.30 | 38.45 | 36.71 | 344.8 (335.3–361.5) |

**256 MiB was the smallest tested healthy limit on both images.** All four runs had zero cgroup limit or OOM events. These measurements use the ARM64 runner's CPU and local storage. Pi hardware, its storage latency and sustained workloads remain unmeasured.

## Focused tests and delivery

The existing CMake `UOS_GATEWAY_FOCUS_TEST` target linked the unchanged tests using the prepared Bookworm object and library configuration. Both Debian images passed 8 of 8 binaries and 129 of 129 cases in their own network namespaces. Each test used a fresh working directory, empty home and temporary directory on a private Docker tmpfs. The proof selected the tests that exercise the changed dependency and package surface.

| Test                                  | Cases passed per image |
| ------------------------------------- | ---------------------: |
| `rt/crypto_test`                      |                     18 |
| `rt/fetch_transport_test`             |                     33 |
| `rt/gzip_test`                        |                     10 |
| `rt/kv_sqlite_test`                   |                     17 |
| `passkeys_webauthn_differential_test` |                      1 |
| `rt/sqlite_test`                      |                     29 |
| `rt/string_test`                      |                     19 |
| `rt/url_idna_test`                    |                      2 |

The focused build used `cmake -S /source/cpp/gateway -B /build -DUOS_GATEWAY_FOCUS_TEST=tests/<test>.cpp` followed by `cmake --build /build --parallel 2 --target uos_focus_<first-16-hex-of-test-path-sha256>`. It reused the successful Docker `cpp` stage with the original tests mounted read-only. Execution used each proof image with `docker run --network=none --cap-drop=ALL --security-opt=no-new-privileges:true --user 1001:1001 --tmpfs /tmp:rw,nosuid,nodev,size=512m,mode=1777` and `env -i`. The identical focused output on each image hashes to `5ba429b121ef25006e071ec11cd4d7253a9cf6183b08c4c93d638be32ffb35f8`. The baseline test registry enumerates 442 binaries. The portable profile's execution gate covered the eight above.

Build, ELF and memory evidence live under `/var/tmp/cppgw-pi.EIcUVv/final/` on the disposable runner. All changes remain in the owned packaging, guide and findings paths. Physical Pi qualification and the parallel jobs' combined tree remain open. GitHub rejected the runner's unauthenticated push attempt, so the task commits remain on the clean local `agent-job` branch for the enclosing job's authorized publication and integration.
