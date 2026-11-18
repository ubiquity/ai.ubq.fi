# Standard C++ gateway runtime helpers

## Work contract v1

Base commit `55afcc8b13b2ddd10747355745da19e5264f7b76`, tree
`5514adb01452ecaa20719fef73b6bf16c7e84b1a`. Replace five generic helpers in
the assigned order. Scope covers their assigned regions under `src/rt`,
component tests, fuzz targets and corpora, and the two documentation files.
Gateway wire formats, stored data, auth and validation decisions, public
runtime APIs, and delivery receipts are invariants. Deployment, provider
traffic, credentials, and unrelated source and test edits are outside the
work contract.

| Step        | Standard facility                     | Compatibility surface                                |
| ----------- | ------------------------------------- | ---------------------------------------------------- |
| Date        | `get_time`, `from_chars`, `chrono`    | ISO timestamps, offsets, UTC bytes, IMF-fixdate      |
| Headers     | Existing normalized HTTP field name   | Validation, Cookie/Set-Cookie, delivery and spelling |
| Environment | Boost.PropertyTree INI                | Flat explicit files, quotes, pre-existing variables  |
| Filesystem  | `filesystem::path`, shared UTF codecs | POSIX state paths, descriptors, text and byte reads  |
| Console     | Boost.JSON, `charconv`                | Operator events, structured values, captures         |

Each step requires fatal-warning release compilation, affected runtime and
application tests with and without ASan/UBSan, a two-minute parser smoke run,
and before/after latency, allocation and source-line counts. Every test links
the runtime, so all 441 binaries are eligible at each step. The named
component and application fixtures run first. The remaining coverage is
reported from actual runner results. Linux ARM64 is the execution platform
available here. Apple Clang/libc++ execution is outside this runner's scope.

The batch builds outside the checkout, with pinned Boost 1.92.0 and Bun 1.4.0.
Tests use a fresh Linux network/mount/PID namespace with loopback, tmpfs, empty
HOME and TMPDIR, and a credential-free allowlist. The stopping point for each
step is its verified commit and a report checkpoint.

## Progress

Date caller discovery complete. Implementation and toolchain preparation in
progress. Headers, environment, filesystem and console pending.

## Runner and commands

- Ubuntu 24.04 Linux AArch64. Release used GCC 14.2.0 `-O2 -g0`. Sanitizer
  used Clang 21.1.8 `-O1 -gline-tables-only -fsanitize=address,undefined
-fno-sanitize-recover=all -fno-omit-frame-pointer`. Both used the requested
  fatal warnings, feature definitions and include roots.
- Installed `clang-21 libclang-rt-21-dev` from the LLVM Noble 21 apt repository
  and `libbrotli-dev libzstd-dev libicu-dev libsqlite3-dev libssl-dev zlib1g-dev
mold` with apt. The preinstalled Bun reported `1.4.0` and was invoked with
  `env -i ... TZ=UTC bun --no-env-file -e '<date probes>'`.
- Built the official `boost-1.92.0-b2-nodocs.tar.xz` release in scratch after
  checking the GitHub-published digest
  `ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411`.
  Commands were `./bootstrap.sh --with-toolset=gcc --with-libraries=json,container`
  and `./b2 -j2 toolset=gcc variant=release link=static threading=multi
--with-json --with-container stage`.
- Built the documented SQLite 3.54.0 fixture dependency from
  `sqlite-autoconf-3540000.tar.gz`, SHA-256
  `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`,
  with `./configure --prefix="$scratch/sqlite/install" --disable-shared
--disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j2` and `make install`.
- A scratch driver reuses `cmake/ci/driver.py` for the Ninja object graph,
  vendor selection, exact `tools/links.py` closures, and per-test OS fixture
  isolation. Invocations are `python3 -B "$scratch/gate.py" PROFILE compile STEP`
  and `python3 -B "$scratch/gate.py" PROFILE run STEP`. Profiles are
  `release` and `asan`. The Clang build uses
  `--system-header-prefix=third_party/` on the existing quoted simdutf header
  in `src/rt/base64.cpp`. The preliminary compile exposed its vendor warnings
  without that option. First-party sources retain the fatal warnings.
- Microbenchmarks used GCC 14.2.0 `-std=c++20 -O2 -g0 -ffunction-sections
-fdata-sections -Wl,--gc-sections` with the parent and replacement objects
  linked separately. Times are the median of seven 100,000-operation batches
  after warmup. C++ allocations are counted in a separate batch via allocation
  operator interposition. C-library allocations are outside that count.
- Fuzz commands used `clang++-21 -O1 -gline-tables-only
-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all` with the port
  flags and a scratch corpus copy, followed by
  `sudo -n unshare --net --fork setpriv --reuid "$(id -u)" --regid "$(id -g)"
--clear-groups env -i PATH=/usr/bin:/bin HOME="$empty_home" TMPDIR="$fresh_tmp"
TZ=UTC ASAN_OPTIONS=detect_leaks=1 "$binary" -max_total_time=120 -max_len=16384
-timeout=3 -rss_limit_mb=768 -seed=17112026 -artifact_prefix="$out/" "$corpus"`.

## Date

Replaced the legacy tokenizer, grammar and field builder with fixed-width
`from_chars` for ISO, locale-fixed `get_time` for IMF-fixdate, and
`chrono` Gregorian calendars and durations. The adapter checks full input,
dates, times and offsets, applies TimeClip, and uses 400-year cycles for
expanded-year timestamps. UTC rendering and the existing Date setters
continue to use the original arithmetic. See
[date deviations](../DEVIATIONS-FROM-BUN.md#date-parsing) for the 784 classified
expectation updates. No previously invalid vector became valid.

- Production diff from the assigned base was **489 removed, 89 added**.
- `fuzz/date_parse.cpp` checked the string constructor and ISO round-trip.
  **40,567,369 executions in 121 seconds**, exit 0, no sanitizer report or
  crash artifact. Three starting inputs cover expanded year, ISO offset and IMF-fixdate.
- Named release and sanitizer fixtures are compiling.

| Input               | Before ns/op | After ns/op | Before alloc/op | After alloc/op |
| ------------------- | -----------: | ----------: | --------------: | -------------: |
| ISO persisted       |        490.5 |       115.4 |               1 |              0 |
| ISO provider offset |        586.1 |       129.5 |               1 |              0 |
| HTTP retry date     |        637.7 |      1622.9 |               1 |              2 |
| Malformed timestamp |        593.5 |        42.2 |               1 |              0 |

The HTTP path pays for the locale-fixed stream and canonical weekday validation.

## Continuation checkpoint

The continuation started from `8e53131d7bca965811c2b807d4f03c4c8d4f7f20`,
tree `97b5c6186b93804fad994e92895a2939946329f2`. This runner rebuilt Boost
and SQLite from the verified archives above. The scratch driver is at
`/tmp/uos-small-r1/gate.py` on this runner. It uses the checked-in
`cmake/ci/driver.py` and links the six system libraries plus the pinned
Boost and SQLite archives. The repository fixture boundary proved loopback
access and an unreachable external address in each test's fresh namespace.

`python3 -B /tmp/uos-small-r1/gate.py release compile date 3` and
`python3 -B /tmp/uos-small-r1/gate.py asan compile date 2` both exited 0.
Each compiled 159 first-party translation units and linked the two vendor
archives. The corresponding `run date 2` commands passed the six named
binaries and all 31 of their cases in each profile. The selection also
included `claude_code_usage_schema_test`, which passed two of three cases.
The aggregate per profile was 6/7 binaries, 33/34 cases, exit 1. No
sanitizer findings were reported.

The extra fixture expects a raw `resets_at` string of `"0"` to project into
a synthetic January 2000 deadline. The isolated probe
`bun --no-env-file -e 'console.log(Date.parse("0"))'` returned
`946684800000`. The standard parser rejects the string, yielding a null
deadline while preserving the raw provider value. This matches the
recorded legacy-date deviation. The application fixture
`tests/claude_code_usage_schema_vectors.inc` is outside the assigned scope.

## Headers

Deleted the WebKit name lookup. The append path uses its existing lowercase
key in errors and limits the visible field name to 64 token characters. The
caught validation error's value is dropped before the error leaves
`append`. The shared validator still builds its legacy text internally, and
`set` and invalid-name errors can still expose raw input. Those paths are
outside the assigned diagnostic region. Cookie and Set-Cookie handling and
HTTP wire spellings are covered by the existing fixtures.

- Production diff was **40 removed, 6 added**, a net reduction of 34 lines.
- For each of `release` and `asan`,
  `python3 -B /tmp/uos-small-r1/gate.py PROFILE quick headers 2 headers_test,fetch_transport_test,runtime_http_differential_test`
  exited 0 with **3/3 binaries, 43/43 cases** and stable source inputs.
  The heavy `serve_delivery_http_test` closure is compiling.
- `python3 -B /tmp/uos-small-r1/gate.py asan fuzz headers_library 1` ran
  **2,115,622 inputs in 121 seconds**, exit 0, with zero crash artifacts and
  no sanitizer finding.
- `python3 -B /tmp/uos-small-r1/bench.py headers headers` linked the actual
  old and new objects against the same runtime. Invalid-input timing includes
  exception handling. The rejected path allocates twice more because it maps
  the existing validator exception to a bounded diagnostic.

| Input                 | Before ns/op | After ns/op | Before alloc/op | After alloc/op |
| --------------------- | -----------: | ----------: | --------------: | -------------: |
| Content-Type          |        345.2 |       117.5 |               3 |              3 |
| Custom field          |        245.5 |       109.2 |               2 |              2 |
| Two Cookie fields     |        362.0 |       140.5 |               2 |              2 |
| Rejected Content-Type |      6,878.9 |     5,125.0 |               2 |              4 |

## Environment files

Replaced the Bun dialect parser with Boost.PropertyTree `read_ini` and
`std::quoted`. The adapter enforces a 256 KiB input limit, classic locale,
flat assignments, original key characters and safe line boundaries. It
maps all grammar failures to a generic `SyntaxError`. Existing values win
in `loadEnvFile`, which reads the complete result before applying it. The
application sources have no explicit environment-file caller. The old
dialect descriptions in `include/uos/rt/env.hpp` and
`docs/rt/platform.md` are outside the assigned paths.

- Production diff was **120 removed, 55 added**, a net reduction of 65
  lines. The additional adapter lines cover the flat-file and control-byte
  restrictions, inline comments, quotes and the allocation bound.
- `python3 -B /tmp/uos-small-r1/env-probe.py` classified the original 124
  inputs before expectations were edited. It identified 68 atomic rejects,
  9 changed quote values and 47 unchanged results. The tests also exercise
  empty INI sections, DEL, the size limit and pre-existing precedence.
- For each of `release` and `asan`,
  `python3 -B /tmp/uos-small-r1/gate.py PROFILE quick environment 2 env_test,runtime_native_startup_test,codex_auth_parse_differential_test`
  exited 0 with **3/3 binaries, 26/26 cases** and stable source inputs.
- `python3 -B /tmp/uos-small-r1/bench.py environment env` measured the
  cold explicit-parser path. PropertyTree allocates tree nodes and indexes
  during parsing. The benchmark inputs were synthetic assignments.

| Input               | Before ns/op | After ns/op | Before alloc/op | After alloc/op |
| ------------------- | -----------: | ----------: | --------------: | -------------: |
| Three assignments   |        329.8 |     1,220.3 |               4 |             42 |
| Quotes and comments |        359.8 |     1,671.1 |               3 |             42 |
| 128 assignments     |     43,545.5 |   121,544.3 |             136 |          2,185 |

`python3 -B /tmp/uos-small-r1/gate.py asan fuzz env_file 1` ran
**1,113,329 inputs in 121 seconds**, exit 0, with no finding. The local
link uses the cached ASan runtime objects for the parser translation
unit's existing `loadEnvFile` dependency; coverage instrumentation is on
the harness and parser unit.

The additional release run of `tests/rt/http_test.cpp` passed five of six
cases. Two assertions in the same test case still require the previous
`Response.redirect` wording and the malformed value bytes. The same
invalid redirects still throw `TypeError`. That diagnostic snapshot is
outside this job's test paths.

## Filesystem text and paths

The filesystem text overloads now use the existing runtime decoder with
`ignoreBOM = true` and the runtime encoder. The shared codec's eventual
backend change is a separate job. The file-operation fixtures preserve
BOM bytes, malformed subsequence handling, surrogate pairs, raw reads,
modes, descriptor behavior and symlinks.

The path functions delegate to `std::filesystem::path` for normalization,
relative paths and decomposition. The adapter keeps native POSIX bytes,
a single POSIX root, cwd lookup, empty results, the last absolute argument
in resolve and concatenate-then-normalize join. Resolved paths drop terminal
separators. Failed relative comparisons throw, protecting containment
checks that accept an empty result as equality.

- UTF changes **removed 82 lines and added 5**. Lexical-path changes
  **removed 233 and added 47**. The file lost 263 production lines.
- The original lexical test had 29 mismatches before its expectations
  were updated. The other nine cases passed, including the original
  UTF, descriptor, symlink, mode and errno assertions.
- `python3 -B /tmp/uos-small-r1/gate.py asan fuzz posix_path 1` ran
  **2,114,557 inputs in 121 seconds**, exit 0, with no finding. Resolve
  and relative used absolute fixture prefixes, so the campaign needed
  no cwd lookup or filesystem access.
- `python3 -B /tmp/uos-small-r1/bench.py filesystem fs` used synthetic
  path and file inputs. The text fixture had a BOM, 1,024 ASCII bytes and an
  emoji. File timings include system calls and are sensitive to runner load.

| Input                | Before ns/op | After ns/op | Before alloc/op | After alloc/op |
| -------------------- | -----------: | ----------: | --------------: | -------------: |
| Normalize POSIX      |        221.3 |       669.5 |               2 |              8 |
| Resolve backup path  |        538.7 |       826.5 |               8 |             12 |
| Relative containment |        514.5 |     1,456.2 |               6 |             24 |
| Read 1 KiB UTF-8     |      4,080.6 |     2,431.9 |               3 |              3 |
| Write 1 KiB UTF-8    |    309,618.4 |   144,149.4 |               2 |              2 |

The named `serve_delivery_http_test` passed **1/1 binary, 7/7 cases**
in each profile. The commands were
`python3 -B /tmp/uos-small-r1/gate.py PROFILE quick headers 1 serve_delivery_http_test`.
The full release and sanitizer object-graph builds are in progress.

The final path adapter joins the standard library's parent components
with a single separator. A libc++ 21 probe exposed that `generic_string()`
alone can retain repeated separators in a parent path. With parent
component joining, the isolated Linux libc++ probe passed **365/365
assertions** on path vectors and three date boundaries. It used
`clang++-21 -stdlib=libc++` with the same strict flags and linked
`fs.cpp`, `text_codec.cpp`, `string.cpp` and `date.cpp`. This was
Linux libc++ evidence. The final lexical diff **removed 232 and added
52** lines, giving a total filesystem reduction of 257 lines.

For each of `release` and `asan`,
`python3 -B /tmp/uos-small-r1/gate.py PROFILE quick filesystem 2 fs_test,runtime_native_startup_test,kv_conversion_cli_differential_test,kv_conversion_profile_json_differential_test`
exited 0 with **4/4 binaries and 46/46 cases** on the final adapter.
The second 120-second path campaign for that adapter is in progress.

## Console

Replaced the JSON encoder and multiline Bun inspector with Boost.JSON
serialization. Ordinary string output takes a direct append path.
C++ `to_chars` renders plain numbers and `from_chars` parses text in
numeric substitutions. A small token adapter preserves the existing
console call shapes. The value projection keeps properties and exception
causes, replaces invalid structured UTF-8, and bounds depth, array
length, projection allocation and final output. Capture, observers,
forwarding, stdout/stderr and the terminal-event construction stay at
their original boundaries. The old layout description in
`include/uos/rt/console.hpp` is outside the assigned paths.

- Production diff was **330 removed, 116 added**, a net reduction of
  214 lines. The 99 changed assertions cover 41 collection renderings,
  44 error renderings and 14 scalar or numeric substitution cases.
- `python3 -B /tmp/uos-small-r1/gate.py PROFILE compile all 2` compiled
  831 first-party translation units in each profile with no fatal warnings.
  `python3 -B /tmp/uos-small-r1/gate.py PROFILE run console 2` then exited 0
  with **16/16 binaries, 172/172 cases** in both profiles. The selection
  includes all Harmony-named tests, startup and OpenAI telemetry.
- `python3 -B /tmp/uos-small-r1/gate.py asan fuzz console_format 1` ran
  **843,101 inputs in 121 seconds**, exit 0, with no finding.
- The final `posix_path` rerun under the same sanitizer fuzz command passed
  **1,790,146 inputs in 121 seconds**, exit 0.
- Clang 21 with `-stdlib=libc++` compiled `console.cpp` with the same
  warnings as errors. This was a Linux compile check.
- `python3 -B /tmp/uos-small-r1/bench.py console console` measured
  constant calls with the same preconstructed arguments.

| Input                 | Before ns/op | After ns/op | Before alloc/op | After alloc/op |
| --------------------- | -----------: | ----------: | --------------: | -------------: |
| Raw message           |         68.5 |        27.5 |               1 |              1 |
| Preformatted terminal |        138.3 |        57.4 |               3 |              2 |
| Structured diagnostic |        662.2 |       721.1 |               8 |             14 |
| Format tokens         |        255.5 |       202.0 |               3 |              2 |
| Error                 |        102.6 |       295.6 |               3 |              6 |

## Combined build and regression evidence

All five production changes are present at
`bd8d7da191286f3ce4aac445f5e9ef12e41eed16`, tree
`493412442cde4f3b290aa1a36164ec178bfcc453`.

| Production source | Removed | Added | Net removed |
| ----------------- | ------: | ----: | ----------: |
| Date              |     489 |    89 |         400 |
| Headers           |      40 |     6 |          34 |
| Environment       |     120 |    55 |          65 |
| Filesystem        |     314 |    57 |         257 |
| Console           |     330 |   116 |         214 |
| Total             |   1,293 |   323 |         970 |

`python3 -B /tmp/uos-small-r1/link-entrypoints.py PROFILE` compiled and
linked the gateway, KV conversion and KV proof executables against the
complete production object graph, the existing startup translation unit,
and both vendor archives. All three linked in each profile. The local run
used disposable namespace fixtures. No packaged release or Mac runtime
was exercised.

`python3 -B /tmp/uos-small-r1/gate.py PROFILE run all 3` executed all
441 test binaries with stable inputs. Both ICU 74.2 runs returned exit 1
with **437/441 binaries and 5,022/5,027 cases**. There were no build
failures or timeouts. The release receipt SHA-256 is
`13c4a8c80543e7217312384881dde2acd88252766308c7a8cb4ba2baee072194`.
The sanitizer receipt SHA-256 is
`2499dcd48f0c3cc6ab0ae90d4c90f3da7b076b6b1880a5dc46706c43355c449e`.

| Failing fixture                         | Observed difference                                                                                                                                                                                                                                                                                     |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `claude_code_usage_schema_test`         | The string `"0"` leaves the normalized deadline null. Bun interpreted it as 2000-01-01. The raw provider value is preserved.                                                                                                                                                                            |
| `codex_external_auth_differential_test` | Three synthetic `last_refresh` strings are rejected in one case. The inputs are `"Nov 16 2026"`, `" 2026-11-16 "` and `"1"`. Canonical ISO and the official access-only fixture pass. The isolated Bun 1.4.0 probe returned 1794787200000, 1794787200000 and 978307200000 for the three legacy strings. |
| `rt/http_test`                          | Two malformed redirect asserts in one case expect the old `Location` spelling and the rejected input in the message. The same two values still throw `TypeError`.                                                                                                                                       |
| `rt/url_idna_test`                      | Two ICU 74.2 Unicode cases fail against the port's ICU 78.3 fixture, independent of the five runtime helpers.                                                                                                                                                                                           |

The first three fixtures and their vector files are outside the assigned
paths. Their expectations are unchanged in this branch.

### Pinned ICU sweep

ICU 78.3 was built from the official `icu4c-78.3-sources.tgz` archive in
scratch. The release-published SHA-256 checked by `sha256sum -c` was
`3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`.
Commands were `../icu/source/configure --prefix="$scratch/install"
--disable-samples --disable-tests --disable-extras`, `make -j4` and
`make install`. Only `src/rt/url.cpp` includes ICU headers. The scratch
driver rebuilt that object and linked the ICU 78.3 libraries into every
binary. The library location is outside the fixture's private `/tmp`
mount.

The first ICU 78.3 trial supplied the library directory in
`LD_LIBRARY_PATH`. Several fixtures correctly clear their child process
environment, which caused loader exit 127. Both trials returned exit 1
with 433/441 binaries and 5,002/5,010 reported cases. This also exposed
an out-of-scope test defect at `tests/local_admin_auth_test.cpp:324`:
after its child probe exits 127, the test dereferences a missing JSON
member and crashes. The sanitizer trial captured the null read. Binding
the ICU directory with linker RPATH resolved the loader failure, and the
auth fixture then passed 17/17 cases in both profiles.

The final command for each profile was
`UOS_ICU_ROOT="$(realpath ../tmp)/icu-78.3" python3 -B /tmp/uos-small-r1/gate.py PROFILE run all 3`.
The scratch linker added `-Wl,--disable-new-dtags,-rpath,<ICU lib directory>`
so the existing child environments could remain isolated.

| Profile             | Binaries passed | Cases passed | Exit | Wall time |
| ------------------- | --------------: | -----------: | ---: | --------: |
| GCC 14 release      |         438/441 |  5,024/5,027 |    1 |   148.1 s |
| Clang 21 sanitizers |         438/441 |  5,024/5,027 |    1 |   296.8 s |

Both IDNA cases passed in both final sweeps. There were zero build
failures, timeouts or sanitizer findings in those sweeps. The three
remaining cases are the Date and Headers snapshots listed above.
`python3 -B /tmp/uos-small-r1/compare-original.py` relinked the three
fixtures with the original Date and Headers objects and the rest of the
same release graph. That comparison passed all 13 cases.

The final release receipt SHA-256 is
`32ab67b69f78b2c4014de1edf980a8c0558c1a37fcd3d8644ce582bd36414e32`.
The final sanitizer receipt SHA-256 is
`172d0fef0116d0d3c03ff286a82f31f1d73b6cf89a2be5677427ca380954bc3f`.
Both record the same immutable source/test input digest
`4e51546d85fd38e07777c073b5ae1e04ee83e6e7e490fe4e10cf34aa88318f35`
at the source commit listed above.

The final Headers corpus rerun,
`UOS_CAMPAIGN_LABEL=final python3 -B /tmp/uos-small-r1/gate.py asan fuzz headers_library 1`,
completed **1,513,289 inputs in 121 seconds**, exit 0. Each of the five
registered harnesses has a clean two-minute smoke result.

### Checkout handoff

The component commits are on the assigned `agent-job` continuation.
The GitHub push preflight for the assigned
`pool-partial/cppgw-std-small-helpers-20261117-44194982655` ref returned
exit 128 because this checkout has no GitHub write credentials.
`gh auth status --hostname github.com` also returned exit 1. The task
history diverges from `origin/development`; integration of this C++ work
remains with the coordinating branch owner. The final commit after the
verified source tree contains only report and deviation formatting.
