# Slim C++ gateway console

## Work contract v1

Base commit `6393434b110e2d1afe93e6c7005848219336d9b9`, tree
`89b42863ac7dd6a07610e2d15aa320efc42547af`. Replace the remaining
JavaScript-shaped inspection and substitution code from plan row 54 with
owned text, Boost.JSON fields and typed error diagnostics.

| Input or integration            | Required behavior                                                                       |
| ------------------------------- | --------------------------------------------------------------------------------------- |
| Text and pre-serialized records | Literal bytes, argument order and one sink newline                                      |
| Structured values               | Shared safe JSON serialization, with bounded diagnostics                                |
| Exceptions                      | Diagnostic name, message, cause and existing OS fields                                  |
| Log, info and debug             | Synchronous capture and stdout forwarding                                               |
| Warn and error                  | Synchronous capture and stderr forwarding                                               |
| Nested captures                 | Method filtering, original args, stable reentrant observation and exception propagation |

The owned paths are the console formatter and its argument declarations,
the console fixture, the appended deviation and this report. The observer
implementation and callers, including terminal and telemetry events,
keep their integration seams. HTTP, persisted data, redaction decisions,
accounting, provider selection, and other lanes are outside this change.
The checkout lacks the linked keep audit, so the caller inventory comes
from the checked-in source and the brief. The existing fuzz caller and
three recorded out-of-scope failures are part of the compatibility check.

Verify the named fixtures with `bash cpp/gateway/tools/ci-build.sh`, then
run the complete ARM64 release and ASan/UBSan registries because every test
links the console translation unit. Record source identity, commands,
compilation and runtime outcomes, any baseline failures, and physical line
counts. Builds use scratch space and the driver's real OS fixture boundary.
This unit ends with committed console changes and exact verification evidence.

## Callers and changes

`rg -n -C 1 '\bconsole::(log|info|warn|error|debug|Value)\b' cpp/gateway/src -g '*.cpp'`
found 126 calls in 44 translation units, including multiline arguments.
The call sites were read with their defining expressions and variable
prefixes. There are 61 warn, 40 error, 13 info and 12 log calls. The
standalone fixture also exercises debug.

| Shape                                   | Confirmed callers                                                                                                                                                                         |
| --------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Literal strings and pre-serialized JSON | Listener and shutdown messages, terminal log, telemetry, banked reset, KV CLI and Jev smoke reports. Lithos prefixes come from three literal event names.                                 |
| Boost.JSON diagnostics                  | Upstream failures use caller-projected `error_class`, `status` and `code`. Embeddings and Metered quota can pass an already selected thrown JSON value.                                   |
| Mixed fields                            | Passkeys use an explicit field list. Two admin cleanup paths carry an exception under an `error` field.                                                                                   |
| Exception or optional string            | Direct catches, `std::exception_ptr` and selected textual errors.                                                                                                                         |
| Capture reads                           | Jev parses the second string after checking the terminal prefix and request ID. Other callers inspect kinds, error name/message, or structured properties through the existing accessors. |

The argument storage now holds owned strings, native JSON or a typed
error snapshot. The collection builders adapt the passkey, admin and
existing fuzz call sites to native JSON. The renderer sends every
structured diagnostic through `rt::jsonStringify`, including the
error snapshot's name, message, code, cause and OS fields. The formatter
keeps depth, array length, projection and output limits. Typed JSON
capture copies detach from caller-supplied storage. The observer
implementation, capture stack and fd forwarding are byte-identical.

The fixture retains all 127 original format vectors, the separate
exception and output bounds, and the callback and fd checks. It now
asserts literal percent sequences, shared JSON number and key ordering,
lone surrogates, and ownership across the destruction of a caller's
JSON arena.

| File                         | Before | After | Net removed |
| ---------------------------- | -----: | ----: | ----------: |
| `src/rt/console.cpp`         |    386 |   279 |         107 |
| `include/uos/rt/console.hpp` |    167 |   122 |          45 |
| Production subtotal          |    553 |   401 |         152 |
| `tests/rt/console_test.cpp`  |    482 |   393 |          89 |
| Production and fixture       |  1,035 |   794 |         241 |

Counts use `wc -l` and `git diff --numstat 6393434b -- <paths>`.
The brief's 280-line lower estimate is 39 lines above the observed
production and fixture reduction. The baseline already used Boost.JSON
for output. Retained code covers the existing capture accessors and
collection builders, plus the structured diagnostic bounds.

## Runner and commands

GitHub runner job `44254409709` used Ubuntu 24.04 on AArch64, 4 CPUs and
15 GiB available to the host. All build output was outside the checkout.
The portable driver used new network, mount and PID namespaces via
passwordless sudo, then ran tests as uid 1001. Its per-binary control
verified loopback and outside errno 101, with private tmpfs, empty HOME
and a fresh TMPDIR.

- Installed `libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev
util-linux mold` with apt. Installed `clang-21 libclang-rt-21-dev` from
  the signed LLVM Noble 21 apt source.
- Downloaded official Boost 1.92.0
  `boost-1.92.0-b2-nodocs.tar.xz`, verified SHA-256
  `ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411`,
  then ran `./bootstrap.sh --with-toolset=gcc --with-libraries=json,container`
  and `./b2 -j2 toolset=gcc variant=release link=static threading=multi
--with-json --with-container stage`.
- Downloaded SQLite 3.54.0 `sqlite-autoconf-3540000.tar.gz`, verified SHA-256
  `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`,
  then ran `./configure --prefix="$scratch/sqlite/install" --disable-static
--disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j1` and
  `make install`.
- Downloaded ICU 78.3 `icu4c-78.3-sources.tgz`, verified SHA-256
  `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`,
  then ran `../icu/source/configure --prefix="$scratch/icu/install"
--disable-samples --disable-tests --disable-extras`, `make -j2` and
  `make install`.

The wrapper used `env -i` with a system path plus the pinned Bun 1.4.0
directory, a new empty HOME and TMPDIR, `LC_ALL=C` and `TZ=UTC`. The
portable driver supplies its own fresh per-binary fixture paths. The
build and test command was `bash cpp/gateway/tools/ci-build.sh --config
PROFILE [test]`, with these explicit build settings.

```bash
UOS_CI_BUILD_DIR="$scratch/build"
PKG_CONFIG_PATH="$scratch/icu/install/lib/pkgconfig:$scratch/sqlite/install/lib/pkgconfig"
UOS_CI_CXXFLAGS="-isystem $scratch/boost"
UOS_CI_LDFLAGS="-L$scratch/boost/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu/install/lib:$scratch/sqlite/install/lib"
# release uses CXX=g++-14 and UOS_CI_JOBS=4
# asan uses CXX=clang++-21 and UOS_CI_JOBS=3, plus the following compile flags
# --system-header-prefix=third_party/ -gline-tables-only -fno-sanitize-recover=all
```

Linker RPATH preserves the pinned shared libraries when fixture children
clear their environment. The Clang system-header option applies to the
first-party quoted includes of vendored headers. First-party warnings
remain fatal. Both compilers accepted `-fsyntax-only -std=c++20 -Wall
-Wextra -Wshadow -Wconversion -Werror -isystem "$scratch/boost"
-Icpp/gateway/include -Icpp/gateway` for the console implementation,
fixture and existing fuzz harness.

## Verification checkpoint

Source commit `ea44347d582eb5406440d208240238eff22cfc1f`, tree
`340c655c7a57b9ce03b7e050b73fce7857727227`. The focused driver calls
select the five requested fixtures. The complete release and ASan/UBSan
commands each select 441 binaries per `ci-build.sh --list`. The driver
links `src/rt/console.cpp` into every selected binary. The full commands
are expected to take tens of minutes on this runner. The compile log
shows object completions and the driver records each test result before
writing its final receipt. A failed gate is diagnosed from its own
compiler or fixture log.

A scratch GCC 14 build at `-O0 -ffunction-sections -fdata-sections
-Wl,--gc-sections` linked the console, filesystem, text codec, string,
number and JSON sources into the standalone console fixture. The
driver's namespace helper ran the binary and **11/11 cases passed**.
The original cold release warm-up passed the serve fixture's five cases
while the fixture inputs were being edited, so its receipt was invalidated.
The named release calls on the committed tree all exited 0. All five
fixtures also passed in the complete sanitizer sweep.

| Filter                                            | Binaries | Cases |
| ------------------------------------------------- | -------: | ----: |
| `tests/rt/console_test.cpp`                       |      1/1 | 11/11 |
| `tests/serve_test.cpp`                            |      1/1 |   5/5 |
| `tests/jev_compaction_smoke_test.cpp`             |      1/1 |   6/6 |
| `tests/serve_delivery_http_test.cpp`              |      1/1 |   7/7 |
| `tests/handler_terminal_admission_stage_test.cpp` |      1/1 |   4/4 |

`python3 -B "$scratch/jev-selector.py"` compiled a scratch copy of the
Jev fixture with one added test, reused the release link list, and ran
under the portable driver's namespace helper. The actual
`terminalRecords` selected the intended request ID from five captured
calls. The selected second argument matched its original JSON text
including percent sequences. All **7/7 cases passed**. Including the
whole fixture from a wrapper TU first triggered GCC's existing
`-Wsubobject-linkage` diagnostic. Compiling the copied fixture as the
primary TU with an absolute private implementation include resolved it
under the same fatal warnings.

The same six-source scratch fixture compiled with Clang 21.1.8,
`-O1 -gline-tables-only -fsanitize=address,undefined
-fno-sanitize-recover=all -fno-omit-frame-pointer`. All **11/11 cases
passed** with ASan leak detection enabled and UBSan set to halt on error.
Using `-fsanitize=fuzzer,address,undefined` and the unchanged
`fuzz/console_format.cpp` with the same runtime sources, the existing
corpus completed **294,605 executions in 121 seconds** with no finding.
The isolated campaign used `-max_total_time=120 -max_len=16384 -timeout=3
-rss_limit_mb=768 -seed=18112026 -artifact_prefix=/tmp/t/artifacts/
-print_final_stats=1 /tmp/t/corpus`. Peak reported RSS was 437 MiB.

Prettier 3.9.6 accepted the report and the appended deviations section with
`bun --no-env-file --no-install "$scratch/prettier/bin/prettier.cjs"
--config .prettierrc --check`. The existing Date table in the deviations
document has a formatting difference. The appendix check verified that
the original document is a byte-identical prefix.

## Complete release sweep

`bash cpp/gateway/tools/ci-build.sh --config release` selected all 441
binaries under GCC 14.2.0. All 369 production objects compiled. The
incremental object phase took 1,752.034 seconds, and linking and running
the suite took 91.956 seconds. The driver reported **438/441 binaries,
5,025/5,028 cases**, exit 1, with zero build failures or timeouts.
The stable source digest was
`fd69c612088e5ebbc32354b3301bb707596233ffcb1e3b6a32da0c19c8b6322a`.
The release receipt SHA-256 was
`5dbe7ac1b56ee3b2a3373d961cb2c5eb29fa24130ae2a82b3b26b70bb112f5b3`.

The three failures match the assigned baseline.

| Fixture                                 | Cases passed | Baseline difference                                                                                                      |
| --------------------------------------- | -----------: | ------------------------------------------------------------------------------------------------------------------------ |
| `claude_code_usage_schema_test`         |          2/3 | Legacy `resets_at` text of `"0"` is retained with a null normalized deadline.                                            |
| `codex_external_auth_differential_test` |          3/4 | External auth rejects three legacy `last_refresh` date forms.                                                            |
| `rt/http_test`                          |          5/6 | Malformed redirects throw the same type with a normalized field name and the rejected value omitted from the diagnostic. |

These are tracked in the [small-helpers report](cppgw-std-small-helpers-20261117.md#pinned-icu-sweep).

## Complete sanitizer sweep

`bash cpp/gateway/tools/ci-build.sh --config asan` selected all 441
binaries under Clang 21.1.8 with ASan, UBSan and leak detection. All 369
production objects compiled. The cold object phase took 3,989.702
seconds, and linking and running the suite took 209.622 seconds.
The driver reported **438/441 binaries, 5,025/5,028 cases**, exit 1.
The three failing cases were the same baseline cases listed above.
There were zero sanitizer findings, build failures or timeouts.
Both receipts recorded the same immutable source digest, commit and
tree. The sanitizer receipt SHA-256 was
`c598f0fd01ac572dc5554c9724c0f9845fd3c8ef5262136cb6cfc4c1d497cf1c`.

## Formatter timings

`bash "$scratch/benchmark-build.sh"` compiled the baseline and current
console implementations with GCC 14.2.0 at `-O2` against the same release
runtime objects. `python3 -B "$scratch/wait-verification.py"` ran alternating
baseline and candidate batches in the driver's network, mount and PID
boundary after both sweeps. The table gives the median of seven batches
of 150,000 `console::format` calls per input and build after one warm-up
pair. The inputs were synthetic, preconstructed arguments. The timing
covers formatting, excluding capture and fd writes.

| Input                          | Before ns/op | After ns/op | Output bytes |
| ------------------------------ | -----------: | ----------: | -----------: |
| Listener message               |        26.16 |       24.20 |           36 |
| Pre-serialized terminal record |        55.93 |       46.91 |          112 |
| Redacted diagnostic fields     |       509.91 |      601.38 |          104 |
| Filesystem error               |       899.00 |    1,331.10 |          200 |

## Documentation follow-up

The runtime platform reference still documents the retired
`Value::error(exception, properties)` factory and older console
formatting. It is outside this job's path ownership. The current
rendering contract is in the [deviations appendix](../DEVIATIONS-FROM-BUN.md#literal-console-arguments-and-shared-json-serialization).

## Publication

`GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-slim-console-20261118`
returned exit 128 because GitHub write authentication was unavailable.
`gh auth status --hostname github.com` returned exit 1. A read-only
fetch confirmed the task tip is outside `origin/development`. The
commits are on the assigned `agent-job` branch for coordinator pickup.

## Summary

- The console renders arguments as literal text or shared JSON. The Jev selector recovered its own terminal payload.
- Production fell from 553 to 401 lines. The production and fixture reduction was 241 lines, 39 below the target.
- The five requested fixtures passed all 33 cases in both profiles. Full ARM64 release and ASan/UBSan each passed 438 of 441 binaries and 5,025 of 5,028 cases. Both reproduced the three documented baseline failures, with no sanitizer findings.
- Formatter-only timings improved for literal text. Structured fields rose from 510 to 601 ns per call, and OS errors rose from 899 to 1,331 ns.
- Source commit `ea44347d`. GitHub write authentication blocked publication and integration.
