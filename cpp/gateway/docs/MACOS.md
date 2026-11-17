# macOS portability and Raspberry Pi strategy

No Mac or Raspberry Pi was connected or executed during this task. The owner explicitly made the Mac unavailable, and the Pi operating system/glibc version is unknown. This document records a Linux source audit, two guarded source changes and a real Linux libc++ module gate. A full Darwin gateway build, test run, package or confinement receipt does not exist.

## Platform audit

| Surface                   | Source / tooling                                             | Current state                                                                                 | Remaining acceptance                                                                                   |
| ------------------------- | ------------------------------------------------------------ | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| Executable identity       | `src/rt/process.cpp`                                         | Darwin uses `_NSGetExecutablePath` then `realpath`; Linux retains `/proc/self/exe`            | Actual Apple SDK compilation and canonical/symlink path execution                                      |
| Signal numbers            | `src/rt/process.cpp`                                         | Darwin map uses native signal constants; Linux retains its existing names/numbers             | Bun-on-Mac oracle and real signal/subprocess cases                                                     |
| Randomness and timestamps | `src/rt/fs.cpp`                                              | Guarded Darwin `arc4random_buf` and `st_*timespec`; Linux retains `getrandom` / `st_*tim`     | Actual Darwin filesystem suite                                                                         |
| Errno values              | `src/rt/fs.cpp`                                              | Symbolic OS errno constants; Linux-only entries guarded; all 67 prior Linux numbers checked   | Actual Darwin Bun error-code/message/errno comparison; Darwin-only errors remain unaudited             |
| Child-exit events         | `src/rt/process_loop.cpp`                                    | Linux `pidfd_open`, `sys/syscall.h` and `pipe2` remain unguarded                              | A separately owned event-driven Darwin exit watcher and close-on-exec pipes; no timer polling fallback |
| Confinement               | `src/runtime/permissions_linux.cpp`                          | Linux ARM64 guard and unsupported-platform refusal preserved                                  | Native Darwin launcher and real Seatbelt positive/negative cases                                       |
| Decompression and SQLite  | `src/rt/sqlite.cpp`, `gzip.cpp`, `fetch_transport_codec.cpp` | Runtime owner moves native profiles to direct libraries; retained SDK uses ELF loader names   | Mac dylib discovery/version/closure and matching behavior                                              |
| Networking backend        | Asio transport/runtime sources                               | Bounded first-party search found no direct epoll API dependency                               | Actual kqueue-backed HTTP/TLS/Unix-socket/WebSocket/half-close/cancellation execution                  |
| C++ library               | Runtime sources and headers                                  | No `bits/`, `ext/`, `std::__` or `__gnu_cxx` dependency found in the audited production scope | Full libc++ gateway linkage with ABI-matching Boost and all tests                                      |
| Build supervision         | `tools/build.py`, `build_support.py`, `compiler-limit.py`    | Linux `/proc`, `prlimit`, `ionice` and Linux tool views                                       | Qualified Darwin resource supervision; no admitted full Darwin profile today                           |
| Test execution            | `tools/ctest-run.py`, `ctest_support.py`, `ctest-network.py` | Linux namespaces/pidfd/private-temp runner, intentionally fail-closed elsewhere               | Real Darwin test boundary; generated Links closures alone do not qualify execution                     |
| Link/package flags        | `cmake/Profile.cmake` and package integration                | Build owner owns platform dependency and ELF/mold guards                                      | Apple dependency targets, install names/rpaths, assets and package manifest                            |

Paths in this table are relative to `cpp/gateway`. Audit inputs and searches are retained in `/tmp/claude-1002/prod/portability_docs/audit-inputs-v7.json` and `portability-search-v7.json`. Later source changes require fresh affected-file inspection; the initial manifest is not a final whole-tree build identity.

## Preserve the TypeScript Darwin boundary

`ops/launch-service.ts` does not call Linux permissions on Darwin. It creates `macSandboxProfile`, then runs `/usr/bin/sandbox-exec` before the Bun application child, with a private SQLite temporary directory and signal forwarding. `ops/macos-sandbox.ts` uses a deny-default Seatbelt profile, exact/literal metadata grants, scoped read/write roots, explicit process execution and immutable release/runtime write denials. `scripts/serve-mac.ts` separately requires Darwin, verifies the release/native backend and marked persistent store, then starts the normal listener.

The C++ Linux permissions API must continue to reject unsupported platforms. A portable build must not replace Landlock/seccomp or Seatbelt with a JavaScript facade, successful stub or unrestricted launcher. Existing native credential custody, stopped-source conversion, latest-write/reverse-proof and production acceptance requirements remain separate and unchanged. A synthetic fixture or consistent copy grants no live state or activation authority.

## Verified Linux compiler evidence

The guarded `process.cpp` source passed strict `-O2 -g0` GCC 16 and Clang 23/libstdc++ hidden-stage compilation. `fs.cpp` passed GCC 16/libstdc++, Clang 23/libstdc++ and Clang 23/libc++ hidden-stage compilation; its 67 previous Linux errno values passed compile-time assertions. The files remain below 1,000 lines. The unchanged Linux process/filesystem runtime suites are pending from the test owner.

The actual Clang reports version 23.1.5, target `aarch64-unknown-linux-gnu`, commit `392dfff97d29b59d77e5cac6d4b6ef2585fe7117`. Installed libc++ headers/libraries were used, not a header-only fallback. The loop-free process control compiled, linked and passed five groups in a fresh Linux network namespace, with loopback success and outside-route denial (`ENETUNREACH`, 101). Its dynamic imports include `libc++.so.1`, `libc++abi.so.1` and `libunwind.so.1`. It deliberately does not link Boost or the full gateway and proves neither Darwin headers nor Darwin runtime behavior.

Exact wrapper argv, compiler/library/source hashes, status and logs are in `stage-compile-v8.json`, `libcxx-module-v8.json`, `libcxx-runtime-command-v8.json`, `libcxx-runtime-v8.json` and `fs-compile-v9.json` under `/tmp/claude-1002/prod/portability_docs`. The actual installed library directory is `/home/codex/toolchains/uos-cpp-2026.11/LLVM-23.1.5-Linux-ARM64/lib/aarch64-unknown-linux-gnu`. An initial mistaken tool-root/lib preflight started no compiler and remains recorded separately.

## Future Apple Silicon check sequence — unexecuted

Use an owner-prepared Mac workspace and offline dependency set. Apple Clang or owner-selected LLVM must support C++20/libc++; Boost and every other C++ library must use that same ABI. Record the real macOS/SDK/deployment target and all compiler/library identities before selecting a release target. No install, Mac connection or command below ran in this task.

```sh
uname -nsmr
xcrun --find clang++
xcrun --show-sdk-path
clang++ --version
cmake --version
ninja --version
mkdir -p .data/cpp-gateway-macos-portability
clang++ -std=c++20 -stdlib=libc++ -O2 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror -I cpp/gateway/include -c cpp/gateway/src/rt/process.cpp -o .data/cpp-gateway-macos-portability/process.o
clang++ -std=c++20 -stdlib=libc++ -O2 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror -I cpp/gateway/include -c cpp/gateway/src/rt/fs.cpp -o .data/cpp-gateway-macos-portability/fs.o
```

These are module compilation steps, not a full build recipe. There is no admitted Darwin CMake/driver profile to invoke: the current `native` driver is Linux-supervised and the Linux profile uses host libraries/mold. The build owner must first integrate platform dependency targets and a bounded Darwin supervisor; the runtime owner must resolve child-exit portability, safe native startup, dylib/assets closure and the actual Seatbelt launcher. Inventing a `macos-release` command would hide those blockers.

Once that interface exists, freeze its exact compiler/profile/source/dependency manifest; build all modules and three entrypoints; execute every original test through the real Darwin boundary; collect `otool -L` and package hashes; then run the same-binary synthetic HTTP/SSE/WebSocket/SIGTERM/persistent-reopen acceptance under that boundary. Verify unrelated file/write/network/process denials as well as positive operations. Never use the Linux namespace runner or a permissive fallback to manufacture a Mac pass. Production adoption is a separate owner action.

## Raspberry Pi decision

The retained SDK comparison gateway (`674e95f30ac8b8b852a6e3cafe1e011b29908dfdac0fc4875443b13dbbf1545c`) imports a maximum `GLIBC_2.34`, `GLIBCXX_3.4.30` and `CXXABI_1.3.13`, measured by `objdump -T`. Its ELF interpreter is `/lib/ld-linux-aarch64.so.1`; direct needed libraries are OpenSSL 3, libcrypto 3, libstdc++ 6, libgcc_s, libc and the loader. This is the binary's direct symbol floor, not a complete runtime-library-closure proof or a known Pi OS match. The receipt is `sdk-symbols-receipt.json` under the portability evidence directory.

The new primary binary's minimum GLIBC symbols, complete linked/loaded library closure, and idle/concurrent RSS versus Bun remain pending. Static libstdc++/libgcc can remove their dynamic library requirements but does not automatically lower GLIBC imports or the requirements of OpenSSL/SQLite/ICU/compression libraries. An Arch-host binary is not accepted for an older unknown glibc solely because it is AArch64.

Select a Pi strategy only after the actual Pi OS, libc, kernel confinement capabilities and memory budget are known and compared with the measured primary closure/RSS in [PERFORMANCE.md](PERFORMANCE.md). Prefer a separately qualified build against the chosen Pi baseline when the host closure is too new. Retaining the GCC 12 SDK is a possible explicit cross-target strategy, not a silent fallback: its coroutine workaround rules remain mandatory, and its older SQLite/IDNA behavior and every runtime library must be qualified separately. Keep the SDK physically intact; it supplies comparison evidence only after primary qualification retires its acceptance role. No Pi installation, execution or service activation is proved or authorized here.

## Scoped decisions

The owner toolchain amendment makes GCC 16/native libraries primary and keeps GCC 12 receipts as comparison. Source claims v8 and v9 permit only the guarded process/filesystem edits recorded above. Direct libraries remain runtime-owned; CMake/profile/driver files remain build-owned. Full Mac/Pi readiness stays open until actual matching-platform evidence exists. No edge/Wasm port is part of this task.
