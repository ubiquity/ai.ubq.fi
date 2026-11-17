# C++ gateway build

The accepted Linux ARM64 build passed 441/441 test binaries and 5,023/5,023 cases. These receipts bind the packaged gateway described below. Deployment targets are ARM64. Apple Silicon has a separate native receipt. Raspberry Pi qualification depends on its OS, loader, library closure and measured service memory. See [open stages](OPEN-STAGES.md).

## Build profiles

The native driver uses the owner-installed toolchain at `/home/codex/toolchains/uos-cpp-2026.11` when its `READY` marker is present. The installed versions are GCC 16.1.1, LLVM 23.1.5, CMake 4.5.0, Ninja 1.13.2, mold 3.0.0, ccache 4.14.3 and Boost 1.92.0. Native profiles use host OpenSSL 3.6.5, SQLite 3.54.0, zlib 1.3.2, ICU 78.3, Brotli 1.2.0 and Zstd 1.5.7. Boost.JSON and Boost.Container use host archives. The build driver treats the installed toolchain and the retained comparison SDK as read-only. See [native runtime and package](RUNTIME.md) for the dependency closure.

| Profile            | Compiler and standard library        | Intended coverage                                                                             |
| ------------------ | ------------------------------------ | --------------------------------------------------------------------------------------------- |
| `release`          | GCC 16.1.1 and libstdc++             | Primary `-O2 -g0` build, with strict warnings and the complete Linux ARM64 test registry      |
| `clang-release`    | LLVM 23.1.5 and libstdc++            | Independent strict optimized build and complete suite                                         |
| `asan-ubsan-clang` | LLVM 23.1.5 and libstdc++            | Whole suite with AddressSanitizer and UndefinedBehaviorSanitizer, with serial test execution  |
| `asan-ubsan-gcc`   | GCC 16.1.1 and libstdc++             | Optional independent sanitizer diagnostics                                                    |
| `tsan-threaded`    | LLVM 23.1.5 and libstdc++            | ThreadSanitizer coverage for the SQLite worker and Asio resolver                              |
| `fuzz-clang`       | LLVM 23.1.5 and libstdc++            | libFuzzer parser targets, with separate execution receipts                                    |
| `clang-libcxx`     | LLVM 23.1.5 and libc++               | Linux standard library compatibility, with ABI-matched dependencies required for full linkage |
| `sdk-release`      | Qualified GCC 12.2 SDK and libstdc++ | Historical comparison, with the GCC 12 coroutine rules                                        |

The native driver gives each profile a separate output tree, tool view, compiler database and profile manifest. Its ccache namespace includes the compiler content hash and profile. Named lanes use independent build graphs and share the cache only within that profile. Keep dependent C++ objects and libraries within one compiler and standard library ABI.

## Build and test entry points

Run from the worktree root after the suite releases its lock. The native `checkpoint` phase configures and then builds. The measured run selected the archive and three executable targets below and used the package cache bindings recorded in its configure receipt.

```sh
release_lock=/tmp/claude-1002/prod/release-build.lock
resource_log=/tmp/claude-1002/codex/resources.log

python3 cpp/gateway/tools/build.py native release checkpoint --keep-going --tests --jobs 2 \
  --target uos_gateway_modules --target uos_gateway \
  --target uos_kv_convert --target uos_kv_proof \
  --release-lock "$release_lock" --supervisor-log "$resource_log"
```

The default build produces `libuos_gateway.a` from `uos_gateway_modules` and links `uos_gateway`, `uos_kv_convert` and `uos_kv_proof`. The three executables link the application archive with whole-archive semantics to retain module registrars. CMake compiles source files into reusable object targets. The independent test targets link their resolved `Links` closures and fixture stand-ins. `--tests` generates the registry at configure time. CTest test binaries have to be built before using the driver's `test` phase.

The task's full primary suite uses its own `release-tests` lane, builds up to eight registered targets per wave, and runs their actual binaries one at a time. Configure the test lane through the same driver. The active test controller owns its release lock and evidence. Resume a settled run from a verified prior receipt rather than launching a parallel complete suite.

```sh
python3 cpp/gateway/tools/build.py native release configure --tests --lane tests \
  --supervisor-log "$resource_log"
```

That command generates `.data/cpp-gateway-native-release-tests/tests-registry.json` and `profile-owner.json`. The test wave controller selects the registry's `uos_test_*` targets through `build.py native release build --incremental --lane tests`. A standalone profile can use `tools/ctest-run.py suite` with its own registry, profile manifest, release lock and supervisor log. For an individual test or the SDK comparison, see [test commands and fixture isolation](TESTING.md). Select the comparison SDK by its exact qualified path and `sdk-release`. The old SDK `release` spelling is a transition alias for the same GCC 12 profile.

The CMake `package` target depends on all three executables. Configure the build cache with the exact native KV directory, frozen inputs file, provenance hash, permissions artifact directory and a fresh output directory. Finalize [RUNTIME.md](RUNTIME.md) before package creation because it becomes the manifest-bound README. The measured CMake package invocation follows.

```sh
python3 cpp/gateway/tools/build.py native release package --supervisor-log "$resource_log"
```

The reproducibility check invokes `tools/package.py` with the same sealed graph and artifact inputs and a second new output directory. The [runtime guide](RUNTIME.md) gives the direct packaging command and launch manifest. The [same-binary smoke guide](../tests/production_smoke/README.md) gives the isolated runtime command.

## Resource limits and resume

| Operation                      | Minimum Linux `MemAvailable` | Process address space                                                     |
| ------------------------------ | ---------------------------: | ------------------------------------------------------------------------- |
| Object-only link               |                      3.5 GiB | 3 GiB general cap. Qualified native mold inputs have a 1 GiB or 2 GiB cap |
| Compile or archive             |                        3 GiB | 2 GiB                                                                     |
| Driver and metadata operations |                        2 GiB | Bounded by the corresponding wrapper                                      |

The native driver requires more than 8 GiB free disk and healthy supervisor status. A live memory reading below 2 GiB is a hard stop. One full build or suite holds the release lock. The native and SDK wrappers share two compiler slots. An object-only link holds both, and a source-bearing compiler/link probe retains the 2 GiB address-space cap. Older link-specific memory admission thresholds were replaced by the 3.5 GiB link threshold. The caps for qualified mold links remain in force.

A target that can run once memory recovers waits 30 seconds between checks for up to 20 minutes. Exhausted admission remains pending infrastructure work. Preserve it and the earlier passing records at the wave boundary. Compiler and linker diagnostics, functional failures and a hard resource stop retain their separate outcomes. A prior pass can carry forward across an admission-only edit after its source, fixture, build flags, profile, configuration and original log are checked. The policy publication is recorded in `/tmp/claude-1002/prod/build/admission-v24/publication.json`. The independently read back transition preserved 232 targets and 2,845 cases with matched non-policy inputs. See `/tmp/claude-1002/prod/tests/resume-orchestrator-1/policy-v24-equivalence-terminal.json`.

Sanitizer runtimes use sampled physical memory and an owned process boundary. AddressSanitizer requires a shadow address range larger than the ordinary fixture virtual-memory limit. Run the sanitizer tests serially and record every explicit skip, failure or interruption.

## Accepted Linux ARM64 build

The GCC 16 primary profile uniformly disables Asio awaitable frame recycling. The actual default passkeys compile emitted a fatal allocation diagnostic, and the configured variant compiled successfully. Both native lifetime controls completed 1,000 paths with identical counts and no live frames at exit. Clang and sanitizer profiles keep their own recorded flags. The strict compiler fixes preserve existing values and evaluation order when GCC requires more explicit ownership of variants and coroutine results. The native frame control receipt is `.data/cpp-gateway-evidence/native-controls-v7/bound-terminal.json`.

The strict GCC 16 suite passed 441/441 binaries and 5,023/5,023 cases with zero failures or skips. The final runtime continuation executed 185 binaries and retained 256 earlier input-matched passes. A final content reconciliation verified all 441 receipts when CMake refreshed its generated files with identical bytes. The suite receipt SHA-256 is `a318d623185bf31bb3565ecc0fbfe76adde7396e9f8e20cb2615b70a0aa5672b`. [Suite result and input binding](/tmp/claude-1002/prod/tests/resume-v25/GCC16-SUITE-HANDOFF.json)

The current-at-measurement archive contains 369 objects. Configure took 13.899 s and the incremental build took 103.909 s, with a reported maximum process RSS of 869,840 KiB in the build phase. The bounded refresh including benchmark artifacts took 139.753 s. The graph reused its qualified cache. Across that refresh ccache recorded 253 additional direct hits and six additional misses. [Build commands, resource measurements and hashes](/tmp/claude-1002/prod/build/quiet-v25/run-1794945647148835996/terminal.json)

| Artifact           | Size in bytes | SHA-256                                                            |
| ------------------ | ------------: | ------------------------------------------------------------------ |
| `libuos_gateway.a` |   169,959,484 | `36c47a2153f86fd259f7ff9dfe74b631b67ce5411ed7fa906e24ea104891fde7` |
| `uos-gateway`      |    40,205,232 | `1b3f81123f7b6c81ee114a7dffb94df5d33c4ae89c05f7caff8dacc4cabbcf03` |
| `uos-kv-convert`   |    40,205,480 | `2b24e4d9de94178df31164765f54b8ab9366e176a25b5e72f43cc40bed81ea50` |
| `uos-kv-proof`     |    40,205,832 | `3329d9ba13a622df462a37010035e5a8829be8cd8820d023c7a2e6549050b34c` |

The real CMake package target and independent producer pass reproduced 59 files with identical bytes, modes and timestamps under umasks 077 and 022. The sorted manifest lists the other 58 files and has SHA-256 `636ccd35ad984fac8280b7599d885bd4bed6d3abd964bcb7ab0ca81d64ce05c0`. The packaged binaries passed 21/21 startup and offline CLI controls and converted four supported synthetic rows in each direction with separate key, value, revision and expiry readback. [Package receipt](/tmp/claude-1002/prod/runtime/final-package-v25/package-terminal.json) and [runtime controls](/tmp/claude-1002/prod/runtime/final-package-v25/runtime-terminal.json)

The smoke, RSS and quiet baseline timings use these exact package bytes. Subsequent Boost.JSON work has its own source-bound qualification. [Measured performance and library follow-up](PERFORMANCE.md) and [release repair ledger](RELEASE-REPAIRS.md)

## Selected static runtime libraries

`cmake/vendors.json` selects simdjson 5.2.0 for `src/rt/json.cpp` and simdutf 9.4.0 for `src/rt/base64.cpp`. The JSON source alone receives `UOS_GATEWAY_SIMDJSON` and the vendor system-header path. It tries the DOM parser with the existing default Boost storage and retains its exact-semantics fallback. The base64 source calls simdutf for inputs of at least three bytes. The vendor targets build separate static archives at the profile’s optimization and instrumentation settings. First-party code keeps its fatal warning set. The gateway and two CLIs link both archives after the whole-archive application group. The JSON and regex benchmarks link only simdjson. The build validates the supplied implementation, header and license hashes and records the archive digests in the consuming build receipts.

The selected GCC 16 incremental build took 46.147 seconds and reached 554,184 KiB reported maximum child RSS before the focused test phase. The measured build, tool-view and evidence footprint peaked at 550,948,864 allocated bytes. The gateway link took 1.007 seconds and grew by 575,616 bytes to 40,780,848 bytes compared with the separately accepted package above. The simdjson and simdutf archives occupy 239,588 and 616,550 bytes. Comparison of the native compilation databases found 846 unchanged commands, the one JSON command with its scoped options and two added vendor commands. Both benchmark targets and the focused JSON CTest built. The focused CTest passed 25/25 cases. The full suite and a new package and smoke receipt for this source remain separate checks. [Native build and link receipt](/tmp/claude-1002/prod/build/vendor-v32/selected-native-terminal.json)

The selected gateway keeps the same AArch64 interpreter, shared-library names and maximum directly required GLIBC version of 2.38. Its libstdc++ requirement adds `GLIBCXX_3.4.32`. Verify that symbol on any older ARM64 Linux target before using this native binary. The baseline package above and the earlier Apple Silicon and SDK receipts retain their original source and runtime identities.

## Build measurements

| Build measurement                        | Result                                                                              | Scope                                                                                                |
| ---------------------------------------- | ----------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| ccache across independent release graphs | Cache hit and identical object hash, with the absolute `__FILE__` intact            | One cross-lane control. Disabled, cold and warm timings across representative heavy sources are open |
| mold object-only gateway link            | Exit 0 with two threads and no fork. Sampled process-tree RSS reached about 266 MiB | Exact earlier native objects, with samples every 20 ms. An equivalent BFD comparison is open         |
| Heavy Boost precompiled header           | Measurement open                                                                    | Isolate build time and disk cost before adoption                                                     |
| Interprocedural optimization             | Off in the build profile                                                            | Separate qualification required before adoption                                                      |
| Release hardening                        | Measurement open                                                                    | Record effective flags, ELF evidence and workload cost                                               |

The cache and mold receipts are `.data/cpp-gateway-cache-lane-proof/terminal.json` and `.data/cpp-gateway-evidence/large-mold-v19/terminal.json`. In the retained GCC 12 SDK comparison, 325 module implementations plus 44 runtime translation units produced a 369-member archive. The largest successful compiler RSS was 1,227,092 KiB. The final link took 2.104 seconds and peaked at 331,944 KiB. The sum of unique production compile times was 4,262.539 seconds across two slots, and the final resumed gateway phase reused 166 objects over 1,351.582 seconds. These measurements describe the comparison build. See `.data/cpp-gateway-evidence/foundation-final/terminal.json`.

## Platform receipts

- Apple Silicon passed 369/369 production compilations, 441/441 test binaries and 5,029/5,029 cases on macOS 26.7.2 with Apple Clang 21.0.0 and libc++. The full run binds commit `c6cd29f9e9743e37c4eca1a0313194fd3279ba9e` and tree `8599a3a0bc292a66a664832f01ca1404d2192e3d`. The separate Darwin fallback loader run passed 4/4 binaries and 91/91 cases. [macOS build and evidence](MACOS.md)
- Linux ARM64 targeted ASan, UBSan and LeakSanitizer passed 25/25 binaries and 291/291 cases on Ubuntu 26.04, Clang 21 and Boost 1.90, at commit `e800a36b1`. The full local sanitizer and independent Clang release profiles remain open. [GitHub run 44131343367](https://github.com/ubiquity/ai.ubq.fi/actions/runs/44131343367)
- The measured Linux executables request `/lib/ld-linux-aarch64.so.1` and reference GLIBC symbols through 2.38. Their package binds 16 canonical Linux host libraries and the selected closure reaches GLIBC 2.43. A Raspberry Pi target needs a compatible 64-bit OS, loader, full dependency closure, Landlock ABI 8 or newer with filesystem threading support, seccomp and verified headroom under its own workload. Compare `uname -m` and `getconf GNU_LIBC_VERSION` on the target, then verify its dependency and confinement contracts from the package manifest. Keep the SDK as a comparison artifact. The x86_64 runs are portability evidence. [Measured ELF and package closure](/tmp/claude-1002/prod/runtime/final-package-v25/package-terminal.json) and [portable CI results](CI.md)

Bind every new receipt to the exact command, profile, consumed source and configuration, exit status, test totals, elapsed time, peak memory and artifact hashes. Reconcile affected evidence after any consumed input changes. See [open stages](OPEN-STAGES.md) for the order of release qualification.
