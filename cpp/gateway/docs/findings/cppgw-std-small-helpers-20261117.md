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

| Step        | Standard facility                       | Compatibility surface                              |
| ----------- | --------------------------------------- | -------------------------------------------------- |
| Date        | `get_time`, `from_chars`, `chrono`     | ISO timestamps, offsets, UTC bytes, IMF-fixdate     |
| Headers     | Existing normalized HTTP field name     | Validation, Cookie/Set-Cookie, delivery and spelling |
| Environment | Boost.PropertyTree INI                  | Flat explicit files, quotes, pre-existing variables |
| Filesystem  | `filesystem::path`, shared UTF codecs   | POSIX state paths, descriptors, text and byte reads  |
| Console     | Boost.JSON, `charconv`                   | Operator events, structured values, captures        |

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
