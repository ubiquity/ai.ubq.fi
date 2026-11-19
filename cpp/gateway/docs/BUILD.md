# C++ gateway build

The selected Linux ARM64 release with simdjson 5.2.0 DOM and simdutf 9.4.0 Base64 encoding passed 441/441 freshly linked and run binaries and 5,024/5,024 cases. The same sealed package passed startup, paired loopback smoke and quiet measurements. Deployment targets are ARM64. See [open stages](OPEN-STAGES.md) for platform and cutover requirements.

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
  --release-lock "$release_lock" --supervisor-log "$resource_log"
```

The default build produces `libuos_gateway.a` from `uos_gateway_modules` and links `uos_gateway`. The executable links the application archive with whole-archive semantics to retain module registrars. CMake compiles source files into reusable object targets. The independent test targets link their resolved `Links` closures and fixture stand-ins. `--tests` generates the registry at configure time. CTest test binaries have to be built before using the driver's `test` phase.

The task's full primary suite uses its own `release-tests` lane, builds up to eight registered targets per wave, and runs their actual binaries one at a time. Configure the test lane through the same driver. The active test controller owns its release lock and evidence. Resume a settled run from a verified prior receipt rather than launching a parallel complete suite.

```sh
python3 cpp/gateway/tools/build.py native release configure --tests --lane tests \
  --supervisor-log "$resource_log"
```

That command generates `.data/cpp-gateway-native-release-tests/tests-registry.json` and `profile-owner.json`. The test wave controller selects the registry's `uos_test_*` targets through `build.py native release build --incremental --lane tests`. A standalone profile can use `tools/ctest-run.py suite` with its own registry, profile manifest, release lock and supervisor log. For an individual test or the SDK comparison, see [test commands and fixture isolation](TESTING.md). Select the comparison SDK by its exact qualified path and `sdk-release`. The old SDK `release` spelling is a transition alias for the same GCC 12 profile.

The CMake `package` target depends on `uos_gateway`. Close the qualified build, select a fresh `UOS_PACKAGE_OUTPUT` under the worktree's canonical `.data`, and run the bounded target. For a relocatable archive, use the Linux ARM64 Docker Buildx path. [Release packages](RELEASES.md) · [Runtime layout](RUNTIME.md)

```sh
python3 cpp/gateway/tools/build.py native release package --supervisor-log "$resource_log"
```

The reproducibility check invokes `tools/package.py` with the same sealed graph and artifact inputs and a second new output directory. The [runtime guide](RUNTIME.md) describes the package and launch manifest. The [same-binary smoke guide](../tests/production_smoke/README.md) gives the isolated runtime command.

## Resource limits and resume

| Operation                      | Minimum Linux `MemAvailable` | Process address space                                                     |
| ------------------------------ | ---------------------------: | ------------------------------------------------------------------------- |
| Object-only link               |                      3.5 GiB | 3 GiB general cap. Qualified native mold inputs have a 1 GiB or 2 GiB cap |
| Compile or archive             |                        3 GiB | 2 GiB                                                                     |
| Driver and metadata operations |                        2 GiB | Bounded by the corresponding wrapper                                      |

The native driver requires more than 8 GiB free disk and healthy supervisor status. A live memory reading below 2 GiB is a hard stop. One full build or suite holds the release lock. The native and SDK wrappers share two compiler slots. An object-only link holds both, and a source-bearing compiler/link probe retains the 2 GiB address-space cap. Older link-specific memory admission thresholds were replaced by the 3.5 GiB link threshold. The caps for qualified mold links remain in force.

A target that can run once memory recovers waits 30 seconds between checks for up to 20 minutes. Exhausted admission remains pending infrastructure work. Preserve it and the earlier passing records at the wave boundary. Compiler and linker diagnostics, functional failures and a hard resource stop retain their separate outcomes. A prior pass can carry forward across an admission-only edit after its source, fixture, build flags, profile, configuration and original log are checked. The policy publication is recorded in `/tmp/claude-1002/prod/build/admission-v24/publication.json`. The independently read back transition preserved 232 targets and 2,845 cases with matched non-policy inputs. See `/tmp/claude-1002/prod/tests/resume-orchestrator-1/policy-v24-equivalence-terminal.json`.

Sanitizer runtimes use sampled physical memory and an owned process boundary. AddressSanitizer requires a shadow address range larger than the ordinary fixture virtual-memory limit. Run the sanitizer tests serially and record every explicit skip, failure or interruption.

## Selected Linux ARM64 release

The GCC 16 primary profile uniformly disables Asio awaitable frame recycling. The actual default passkeys compile emitted a fatal allocation diagnostic, and the configured variant compiled successfully. Both native lifetime controls completed 1,000 paths with identical counts and no live frames at exit. Clang and sanitizer profiles keep their own recorded flags. The strict compiler fixes preserve existing values and evaluation order when GCC requires more explicit ownership of variants and coroutine results. The native frame control receipt is `.data/cpp-gateway-evidence/native-controls-v7/bound-terminal.json`.

GCC 16.1.1 at `-O2 -g0` with fatal warnings produced 441 fresh test binaries. All 5,024 cases passed with zero failures or skips, including the added JSON regression. Each test links both selected vendor archives. The suite SHA-256 is `d63e8d62445ad5defc8b527c629150387f093bb00d17114c5ecf01f629d27b0d`. [Selected full suite](../../../.data/cpp-gateway-evidence/full-native-gcc16-selected-vendors-1794950578188873969/suite/suite.json)

| Artifact                   | Size in bytes | SHA-256                                                            |
| -------------------------- | ------------: | ------------------------------------------------------------------ |
| `libuos_gateway.a`         |   169,976,650 | `636a44ce322096bc42c0666d1e321040aae3e928ae3ad4f4feb7cc189bd8ea51` |
| `uos-gateway`              |    40,780,848 | `58cd26cca3b8c40ac08c5ad09a3e4be87adfc8f0a0cd900e0b6fd42a18f5fa22` |
| `uos-kv-convert`           |    40,781,160 | `d58d711ba34878b8c997578999bac3e34df08b43d8823f6c06c7c279affd5c2a` |
| `uos-kv-proof`             |    40,781,512 | `0b2610ccc9515c682dd538a4182079e315ae67d10f407430d23be427aa269c56` |
| `libuos_vendor_simdjson.a` |       239,588 | `353d35f61d716715e63efdf16aab40b460afa84cf0796cf67412d822304c7524` |
| `libuos_vendor_simdutf.a`  |       616,550 | `ad401e0720174f9697a383db7727dd2dd42eaefc57f080382327e52b1cda3d6a` |

The CMake package target and independent producer pass reproduced 63 files with identical bytes, modes and timestamps under umasks 077 and 022. The manifest lists 62 files, including both vendor license and notice pairs. Its SHA-256 is `8c0600e80ccecbbc6cdcf8f471ffe340de64e6c7c2f8bb850ee8a50d7187cbd1`. The package passed 21/21 startup and offline CLI controls and a four-row acyclic conversion in both directions, with independent key, value, revision and expiry readback. [Package receipt](/tmp/claude-1002/prod/runtime/final-package-v32/package-terminal.json) and [runtime controls](/tmp/claude-1002/prod/runtime/final-package-v32/runtime-terminal.json)

Four interleaved smoke trials passed, two with the selected gateway and two with Bun. The quiet measurements also bind these package bytes. The preceding 5,023-case build is the earlier performance and ABI control. [Performance and comparison](PERFORMANCE.md) and [release repair ledger](RELEASE-REPAIRS.md)

## Selected static runtime libraries

The Assignment 2 registry selected simdjson 5.2.0 for `src/rt/json.cpp` and simdutf 9.4.0 for `src/rt/base64.cpp`. That JSON source received `UOS_GATEWAY_SIMDJSON` and the vendor system-header path. The sealed binary used DOM with default Boost storage and the historical alternate parser. Its Base64 encoder called simdutf for inputs of at least three bytes. The vendor targets build separate static archives at the profile’s optimization and instrumentation settings. First-party code keeps its fatal warning set. The gateway and two CLIs link both archives after the whole-archive application group. The JSON and regex benchmarks link only simdjson. The build validates the supplied implementation, header and license hashes and records the archive digests in the consuming build receipts.

The selected GCC 16 incremental build took 46.147 seconds and reached 554,184 KiB reported maximum child RSS before the focused test phase. The build, tool-view and evidence footprint peaked at 550,948,864 allocated bytes. The gateway link took 1.007 seconds and grew by 575,616 bytes to 40,780,848 bytes compared with the pre-library control. Native compilation databases retained 846 first-party commands, changed the JSON command’s scoped options and added two vendor commands. Both benchmark targets and the focused JSON CTest built. [Native build and link receipt](/tmp/claude-1002/prod/build/vendor-v32/selected-native-terminal.json)

All three selected executables use `/lib/ld-linux-aarch64.so.1` and directly require up to `GLIBC_2.38`, `CXXABI_1.3.15` and `GLIBCXX_3.4.32`. The pre-library executables required through `GLIBCXX_3.4.31`. The 16-library host closure includes GLIBC 2.43 and the package binds the exact DSO bytes. [ELF and package closure](/tmp/claude-1002/prod/runtime/final-package-v32/package-terminal.json)

## Assignment 3 library checkpoints

| Linux ARM64 checkpoint | Library and scope                                                                                                                                                    | Evidence                                                                                                                                                                                                                                                                                          |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| JSON (B1)              | simdjson 5.2.0 is the sole parser. Boost.JSON 1.92 serializer primitives write the existing value type.                                                              | 8/8 focused binaries, 71/71 cases, 643 parse vectors retained and 746 production lines removed. Frozen package manifest `c196ee22541cd11fcf67589a47ba7dc686c446e92bae0043b84e85da48a1cf4a`. [Focused results](/tmp/claude-1002/prod/assignment3/runtime/b1-20261117T230423Z/focused-results.json) |
| UTF (C2)               | simdutf 9.4.0 validation, ASCII scan, UTF-16 sizing and transcoding. `cmake/vendors.json` includes `src/rt/string.cpp` and `src/rt/text_codec.cpp` alongside Base64. | 4/4 focused binaries, 56/56 cases and 72 production lines removed. Frozen package manifest `8fced1a296cc3456b71db01b6ba544fdd93fa3f8e808c5dea32deb24db654edf`. [Focused results](/tmp/claude-1002/prod/assignment3/runtime/c2-20261117T230537Z/focused-results.json)                              |

The preserved Assignment 2 package is the complete release baseline. The later checkpoints cover their exact inputs. Record each incoming integration's source/tree, vendor provenance and archives, build profiles, affected tests, case counts and package manifest here once verified. The QCBOR 1.6.1 passkey lane and the combined boundary limits need their own receipts. A worker's x64 pool build supplies scoped evidence. The integrated ARM64 suite is the release gate. [Replacement plan](OSS-REPLACEMENT-PLAN.md) [Runtime limits](RUNTIME.md#input-limits)

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
- The supervisor’s Linux ARM64 sanitizer run passed 25/25 selected binaries under ASan, UBSan and LeakSanitizer at commit `5b4a66941`, consuming both vendor libraries. [GitHub run 44183482817](https://github.com/ubiquity/ai.ubq.fi/actions/runs/44183482817) The earlier pre-library run passed 25/25 selected binaries and 291/291 cases at `e800a36b1`. Independent Clang 23 and the local whole-suite sanitizer profile remain separate. [Earlier run 44131343367](https://github.com/ubiquity/ai.ubq.fi/actions/runs/44131343367)
- Raspberry Pi qualification needs a 64-bit ARM OS and compatible loader, the required GLIBC and libstdc++ symbols, the complete library closure, and measured headroom on the target workload. Seccomp thread synchronization is required. The default requires Landlock ABI 8 or newer, and operators can explicitly select reduced confinement on ABI 3–7. Check `uname -m` and `getconf GNU_LIBC_VERSION` on the target and inspect the target DSOs for the required versions. The x86_64 runs and GCC 12 SDK remain comparison evidence. [Raspberry Pi packaging](RASPBERRY-PI.md) [Linux confinement](RUNTIME.md#linux-confinement) [Portable CI results](CI.md)

Bind every new receipt to the exact command, profile, consumed source and configuration, exit status, test totals, elapsed time, peak memory and artifact hashes. Reconcile affected evidence after any consumed input changes. See [open stages](OPEN-STAGES.md) for the order of release qualification.
