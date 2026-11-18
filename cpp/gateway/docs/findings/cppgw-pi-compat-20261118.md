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
- The native KV build used the existing frozen Bun 1.4.0 and Rust 1.98.1 build recipe inside Bookworm. The confinement artifact was compiled from the existing Node-API source with Debian's headers. No artifact is compiled or fetched on application startup.
