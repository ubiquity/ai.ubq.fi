# C++ gateway UTC calendar refactor

## Work contract v1

Use C++20 calendar and duration types for UTC extraction and normalized date arithmetic. Preserve the wide
TimeClip range, the ISO and HTTP wire formats, input truncation, and the current parser's provider decisions.
Remove fixture-only mutation after checking all callers. The owned paths are the calendar, formatting and Date
implementation around the existing parser in `src/rt/date.cpp`, their declarations in `include/uos/rt/date.hpp`,
the relevant `tests/rt/date_test.cpp` cases, the deviations appendix and this report. Clock and timers, provider
routing, persistence, other tests, build files, TypeScript and deployment are outside this scope.

| Surface                               | Required behavior                                                                                                 |
| ------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| ISO timestamps and UTC day boundaries | Preserve millisecond truncation, offsets, negative epoch values, leap dates and signed extended years.            |
| HTTP Date and Codex Retry-After       | Preserve the weekday and canonical formatting, exact parse round-trip, classification and applicable quota reset. |
| Filesystem and stored timestamps      | Retain the existing filesystem, migration and persisted-value comparisons.                                        |

Record the full Release baseline before source edits. Run the named date, filesystem, clock, Codex 429,
migration and delivery selections, plus every link closure in Release and ASan/UBSan. The CI driver links the
date object into every target. Each fixture runs inside the driver's Linux namespaces using local mocks, an
empty home, a fresh temporary directory and a credential-free environment. Downloads and build products go
under `/var/tmp/cppgw-slim-dates.bj9TzzhL`. The cold baseline and sanitizer builds each have a 90-minute
budget with four workers. Use their object logs, incremental `results.tsv` and final receipts for progress and
results. Preserve failed evidence and retry a recoverable prerequisite or resource failure with the same inputs.
The stop condition is matching baseline and candidate coverage, exact commit and tree receipts, and a clean Git
handoff or a documented external blocker.

## Starting point

The clean `agent-job` checkout matches `origin/agent-jobs/cppgw-integrate-f1-20261118-44343309770-1` at
`2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`, tree `d4f9c3beb10816358c85e92c1c231421fa35fa48`. The incoming
F1 report records 450 passing Release binaries and 5,104 passing cases. Its two shared validation failures
belong to the separate paired conformance suite. This runner is Ubuntu 24.04.5 on AArch64 with four CPUs and
15 GiB RAM. Local baseline results, the fixture-only API audit and publication access are pending.

First-party production counts use physical lines, including comments and blanks. `src/rt/date.cpp` starts at
353 lines and `include/uos/rt/date.hpp` starts at 71, for 424 lines across the owned production files.

## Caller audit

The UTC setters and `setTime`, `valueOf`, `getTimezoneOffset`, and the individual weekday and clock getters
occur only in the date fixture. The application reads the year, month and day for usage windows and model alias
dates. HTTP responses use `toUTCString`, the Codex 429 path compares it with the provider header, and the KV,
replay and log paths still use the ISO, JSON and display forms. The kernel's day-key parser admits only four
year digits and two month and day digits before calling `Date::UTC`.

## Runner setup

Clang 21.1.8 and its sanitizer runtime came from the signed apt.llvm.org Noble 21 archive. Its key fingerprint
is `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The runner installed the compiler and system libraries with
`sudo -n apt-get install -y -qq clang-21 libclang-rt-21-dev llvm-21 libssl-dev zlib1g-dev libbrotli-dev libzstd-dev libicu-dev mold`.
The driver used Ninja 1.13.2, mold 2.30.0, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5.

| Verified upstream archive        | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| `boost_1_92_0.tar.bz2`           | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

```bash
scratch=/var/tmp/cppgw-slim-dates.bj9TzzhL
(cd "$scratch/boost_1_92_0" && ./bootstrap.sh --with-libraries=json,container &&
  ./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage)
(cd "$scratch/icu-build" && ../icu/source/configure --prefix="$scratch/icu78" --disable-samples --disable-tests --disable-extras &&
  make -j2 && make install)
(cd "$scratch/sqlite-autoconf-3540000" && ./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3' &&
  make -j1 && make install)
```

The `ci-gate.sh` wrapper creates a new home and temp directory on every invocation and passes these settings
through `env -i`. The driver independently verifies SQLite 3.54.0, loopback access and an outside-route denial,
then starts each fixture with its own private tmpfs, empty home and credential-free environment.

```bash
scratch=/var/tmp/cppgw-slim-dates.bj9TzzhL
gate=$(mktemp -d "$scratch/gate.XXXXXXXX")
mkdir "$gate"/{home,tmp}
exec env -i PATH=/usr/local/bin:/usr/bin:/bin:/home/runner/.bun/bin:/usr/lib/llvm-21/bin \
  LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC HOME="$gate/home" TMPDIR="$gate/tmp" \
  CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$scratch/ci" \
  UOS_CI_SQLITE_PREFIX="$scratch/sqlite" PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" "$@"
```

## Local Release baseline

The driver ran on the report-only commit `4c74845ded3ba6f276d29312cf82ca9f4f2d0dee`, tree
`a2cfdf75709b9fe815ca935549c803b8c02a2003`. The whole checkout diff from the supplied starting commit
contained only this report. Driver input SHA-256 `97e66e6321b955b3a5e00ac9a6297b1520d045cd462186b977dc6d64c89790b6` also matches the
incoming F1 report. The baseline passed all 450 binaries and all 5,104 cases with
1758.108 s for the cold object build and 63.419 s for link and test.
The outer timing reported `wall_s=1822.64 peak_process_kib=696344 exit=0`.

```bash
bash "$scratch/ci-gate.sh" /usr/bin/time -o "$scratch/baseline-release.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  timeout -k 15s 90m bash cpp/gateway/tools/ci-build.sh --config release
```

Receipt `receipts/baseline-release.json` SHA-256 is
`93eb27c9266bc14ac815522715644da5a0eec9aa0e8985ca7b08313dcbe8cac7`.
