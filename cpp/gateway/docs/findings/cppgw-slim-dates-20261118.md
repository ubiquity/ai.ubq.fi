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
15 GiB RAM.

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
is `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The driver used Ninja 1.13.2, mold 2.30.0, OpenSSL 3.0.13, zlib
1.3, Brotli 1.1.0 and Zstd 1.5.5.

```bash
sudo -n apt-get install -y -qq libssl-dev zlib1g-dev libbrotli-dev libzstd-dev libicu-dev mold
sudo -n apt-get install -y -qq clang-21 libclang-rt-21-dev llvm-21
```

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

## Implementation

1. `177a16df9497260b473fa8e1567208d54e6038c2` uses `sys_days`, `year_month_day`, `weekday` and `hh_mm_ss` for UTC
   projection, and chrono durations for normalized numeric inputs. Wide days and milliseconds retain the full
   TimeClip range, and 400-year reduction keeps out-of-range years inside the standard calendar types. The
   first date fixture run passed 6/6 cases.
2. `118f9ed5cde38a0038f66843f2beb2e89c589cb5` reuses year, clock and width formatting for the ISO and HTTP date
   strings. New controls cover both chrono calendar bounds, overflow in month, day and time components,
   TimeClip endpoints, applied UTC offsets and weekday spelling. One control cancels a very large year and
   month and checks the mathematically normalized `0001-05-01T00:00:00.000Z`. Bun 1.4.0 produces
   `0000-09-01T00:00:00.000Z` for those extreme operands. The gateway day-key parser cannot supply them.
   The date fixture passed 7/7 cases.
3. `7e9a4bac9382dec76a4641dcf1df2e055077e691` removes the fixture-only methods and adds
   `Date::utcFields()` and `Date::fromUtc()`. All 520 former setter inputs now check immutable reconstruction.
   The 2,050 parser inputs, 205 epoch inputs and 221 UTC constructor inputs are retained. The
   `readDigits` through `Date::parse` block compares byte-for-byte with the base.

| First-party production path  |      Before |       After | Removed |
| ---------------------------- | ----------: | ----------: | ------: |
| `src/rt/date.cpp`            |         353 |         266 |      87 |
| `include/uos/rt/date.hpp`    |          71 |          60 |      11 |
| **Owned total**              |     **424** |     **326** |  **98** |
| **Gateway production total** | **185,211** | **185,113** |  **98** |

The gateway census counts tracked physical newlines under `src`, `include`, `runtime`, `entrypoints` and the
compiled macOS version launcher. Tests, documentation and vendors are outside the production count.

A scratch comparison compiled the base and each of the three patches with strict Clang 21 Release flags,
then ran all four through the driver fixture boundary. The 150,000 generated TimeClip values had matching
ISO, HTTP and display strings, and the 150,000 generated component sets had matching epoch milliseconds
or invalid results. Each ISO timestamp parsed back to its input milliseconds.

```bash
bash "$scratch/calendar-probe.sh"
bash "$scratch/run-release.sh" step1-date tests/rt/date_test.cpp
bash "$scratch/run-release.sh" step2-date tests/rt/date_test.cpp
python3 -B cpp/gateway/tools/links.py --check
git diff --check 2b23fb68..HEAD
```

## Candidate ARM64 verification

The driver receipts bind commit `7e9a4bac9382dec76a4641dcf1df2e055077e691`, tree
`f52dec859c479becf72a86e159eed999fdc38e5e`, and driver input SHA-256
`0c52ed5b68f29cc0140c52521a61802173b7c63703138cbc431e4ec377464087`. The portable driver links the date object into every test.

| Requested Release selection                        | Binaries passed | Cases passed |
| -------------------------------------------------- | --------------: | -----------: |
| `tests/rt/date_test.cpp`                           |               1 |            7 |
| `tests/rt/fs_test.cpp`                             |               1 |           13 |
| `tests/rt/clock_test.cpp`                          |               1 |           11 |
| `tests/native_codex_429_differential_test.cpp`     |               1 |            4 |
| `tests/native_codex429_conformance_test.cpp`       |               1 |            2 |
| `tests/cache_kv_migration_usage_coverage_test.cpp` |               1 |           43 |
| `tests/serve_delivery_http_test.cpp`               |               1 |            7 |
| **Total**                                          |           **7** |       **87** |

The native Codex fixture retained HTTP 429 and the stable `header_retry_after` deadline at
`1700000060000` ms for `Tue, 14 Nov 2023 22:14:20 GMT`. Changing the weekday to Wednesday rejected the
deadline. A conflicting later deadline took effect at `1700000120000` ms and cleared reset stability.
The filesystem, migration, serving and stored-value comparisons ran with their existing assertions.

The complete Release gate passed **450/450 binaries and 5,105/5,105 cases**, including checks for
health responses, HTTP framing and persisted timestamps. The date boundary test accounts for the one
additional case. The object phase took 314.243 s with cached objects available,
and link and test took 64.011 s. The outer timing was
`wall_s=379.08 peak_process_kib=696308 exit=0`. Receipt
`receipts/final-release.json` SHA-256 is `86e2752317f7bca86b1c54a5baa0d7032fa50befe4da99eb7e366615b0ffd550`.

```bash
for test in tests/rt/date_test.cpp tests/rt/fs_test.cpp tests/rt/clock_test.cpp \
  tests/native_codex_429_differential_test.cpp tests/native_codex429_conformance_test.cpp \
  tests/cache_kv_migration_usage_coverage_test.cpp tests/serve_delivery_http_test.cpp; do
  bash "$scratch/run-release.sh" "focused-$(basename "$test" .cpp)" "$test"
done
bash "$scratch/run-release.sh" final-release
```

The Release wrapper runs `bash "$scratch/ci-gate.sh" /usr/bin/time -o "$scratch/receipts/$label.time"
-f 'wall_s=%e peak_process_kib=%M exit=%x' timeout -k 15s 90m bash cpp/gateway/tools/ci-build.sh
--config release "$@"` and preserves the driver receipt and results table.

The complete AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer gate passed **450/450 binaries
and 5,105/5,105 cases** on the same candidate. Release and sanitizer receipts list the same targets and the same
per-target case counts. The sanitizer build compiled all 369 production objects in 2,875.712 s, and link and
test took 225.481 s. Each run log contains one successful network-isolation receipt with outside errno 101.
The sanitizer scans found zero diagnostics in the 450 logs. The outer timing was
`wall_s=3102.06 peak_process_kib=1107708 exit=0`. Receipt `receipts/final-asan.json` SHA-256 is
`bf798337cf6990d4ecdd5a5aa4150e27cb3bb421bb263da4e8c55ee76640eb71`.

```bash
bash "$scratch/ci-gate.sh" /usr/bin/time -o "$scratch/receipts/final-asan.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  timeout -k 15s 90m bash cpp/gateway/tools/ci-build.sh --config asan
```

## Findings outside the date edits

The parser declaration comment and `docs/rt/text.md` still describe the legacy Bun parser. The text guide also
lists the removed mutation and scalar getter APIs. The supported formats and immutable operations are recorded
in the deviations appendix. [Runtime deviations](../DEVIATIONS-FROM-BUN.md)

F1's separate paired conformance report records `chat-invalid-role` and `responses-invalid-tool-type` as open
shared validation defects. Native and Bun returned HTTP 200 and dispatched upstream in those cases. The
independent expectations require HTTP 400 and zero dispatch. [F1 paired conformance](cppgw-integrate-f1-20261118.md#paired-conformance)

## Handoff

The source and test tree is committed in three steps on `agent-job`. Later commits update this report. The
format gate used Bun 1.4.0 and both frozen dependency manifests in runner scratch. Links validated all 450
targets, and `git diff --check` passed. The branch diff covers five owned paths. Build output is in runner scratch.

```bash
(cd "$scratch/format" && bash "$scratch/ci-gate.sh" bun --no-env-file --no-install run format:check \
  cpp/gateway/docs/DEVIATIONS-FROM-BUN.md cpp/gateway/docs/findings/cppgw-slim-dates-20261118.md)
git diff --check 2b23fb68..HEAD
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false timeout -k 2s 25s git push --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-slim-dates-20261118
git merge-base --is-ancestor HEAD origin/development
git bundle create "$scratch/cppgw-slim-dates-20261118.bundle" agent-job ^2b23fb68
git bundle verify "$scratch/cppgw-slim-dates-20261118.bundle"
```

The push preflight and push exited 128 because GitHub write authentication is unavailable. The refreshed
`origin/development` at `a3a96bc5fe96ba383936746fae23e64daa69562f` lacks the supplied F1 base and the task tip.
The PR, merge and `development` checkout handoff remain pending. Runner scratch holds the verified Git bundle
and gate receipts.

**Summary.** C++20 `std::chrono` now handles UTC calendar operations. The refactor removes 98 first-party lines
(424 to 326) and replaces fixture-only setters with immutable operations. Extreme opposing year and month
inputs use mathematical normalization. Linux ARM64 Release and ASan/UBSan/LSan each passed 450 binaries and
5,105 cases. The Codex Retry-After fixture retained HTTP 429 and the `1700000060000` ms reset. GitHub write
authentication and the missing F1 ancestry on `development` block the handoff. The checkout is clean on `agent-job`.
