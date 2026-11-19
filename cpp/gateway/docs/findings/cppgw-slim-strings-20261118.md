# C++ gateway string helpers

## Work contract v1

Use standard C++20 string-view operations for fixed ASCII protocol tokens and
the converted UTF-16 fallback. Retire the uncalled `codePointAt` and `trimStart`
APIs with equivalent fixture coverage. The work owns the row 32 string regions,
the Codex quota header-name checks, the Responses message-type prefix, their
fixtures, and an appendix in the deviations ledger. The UTF codec (including
`fromCodePoint`), ICU, percent escapes, regex, provider streaming, build tools,
TypeScript and workflows are outside this change. This task uses local mocks
and has no deployment or production state effects.

| Surface                  | Required behavior                                                       |
| ------------------------ | ----------------------------------------------------------------------- |
| Codex quota header names | Remove the same `x-` header families and preserve unrelated headers.    |
| Responses item type      | Preserve type classification and invalid-request field and status.      |
| Protocol ASCII tokens    | Match the same bytes next to arbitrary UTF-8, including malformed data. |
| User text and regex      | Preserve UTF-16 indices, coercion, lone surrogates and output bytes.    |
| Expansion and trimming   | Preserve bounds, whitespace, empty input and stable output.             |

Resolve caller and fixture coverage from this revision before changing the API.
Measure the retained indexing paths and request-path work on the ARM64 runner.
The acceptance surface includes the six named release fixtures, the full release
inventory, and the ASan+UBSan Links closure. Every test links `src/rt/string.cpp`,
so the sanitizer closure is also the full inventory. Stop after reconciling
both suites with the baseline, reporting source lines and timings, and committing
the result. External publication depends on the repository's GitHub access.

## Starting point

The regex integration is already present at `51408a849928ae47a0a9b3fc1f6da02dbf80dcc9`.
The starting branch resolves to `9acb016ae9900ffac255b7f3cec02c735f6ddeda`, tree
`72b5114c61fd335e63f43202708a419f01b11df0`. Its previous Linux ARM64 receipt
records 448/448 release binaries and 5,095/5,095 cases. The two standalone
packaged conformance failures recorded there are outside this test inventory.
[Stage 3b findings](cppgw-integrate-s3b-20261118.md)

The owned production files start at 1,485 physical lines. This runner is Ubuntu
24.04.5 on ARM64 with four Neoverse-N2 vCPUs. The local baseline build uses a
detached checkout of the starting commit and scratch dependencies and evidence
under `/var/tmp/cppgw-slim-strings.75hrNmNR`.

## Implementation

`14485690` switches the quota header-name and Responses message-type checks
to C++20 byte comparisons. The Responses fixture checks the 400 response,
error type, parameter and message for eight `input_` suffixes, including
supplementary characters, lone surrogates, truncated UTF-8 and embedded NUL.

`a4b3ca3c` removes `codePointAt`, `trimStart` and the `unitsOf` forwarding
helper. The existing Bun fixture vectors exercise positional code points
through `charAt`, `slice` and `codePoints`, and leading whitespace through
`codePoints` and `isJsWhitespace`. Shared predicates use native byte or
UTF-16 string-view methods. The string fixture covers ASCII tokens adjacent
to all 18 existing malformed inputs and retains the UTF-16 search results.

`2c0d6fec` uses a UTF-16 view throughout the substitution fallback and
processes `replaceAll` matches in order without collecting offsets in a
temporary vector. `8a34ff90` records the interface in the deviations ledger.
The UTF-8 index locator, UTF-16 result conversion, and slice fast paths
remain in use for non-ASCII text.

## Source size

Counts are tracked physical newlines, including blank lines and comments.
The gateway total covers the 745 C and C++ production files under `src`,
`include`, `runtime` and `entrypoints` in both Git trees.

| Production path             |    Before |     After | Removed |
| --------------------------- | --------: | --------: | ------: |
| `src/rt/string.cpp`         |     1,062 |     1,014 |      48 |
| `include/uos/rt/string.hpp` |       183 |       181 |       2 |
| `src/codex/quota.cpp`       |        74 |        74 |       0 |
| `src/responses_request.cpp` |       166 |       166 |       0 |
| **Owned production**        | **1,485** | **1,435** |  **50** |
| All gateway production      |   180,460 |   180,410 |      50 |

## Runner and commands

Clang 21.1.8 came from the signed apt.llvm.org Noble 21 archive with key
fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The runner uses
Ninja 1.13.2, mold 2.30.0, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and
Zstd 1.5.5. The staged libraries use the same official source archive hashes
as the starting integration.

| Source archive                   | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| `boost_1_92_0.tar.bz2`           | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

```bash
sudo -n apt-get update
sudo -n apt-get install -y libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev mold util-linux iproute2 ccache
sudo -n apt-get update -o Acquire::Retries=2 -o Acquire::https::Timeout=15
sudo -n apt-get install -y clang-21 libclang-rt-21-dev llvm-21

# In the Boost source directory
./bootstrap.sh --with-toolset=gcc --with-libraries=json,container
./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage

# In the SQLite source directory
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install

# In the out-of-tree ICU build directory
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-static
make -j2
make install
```

```bash
scratch=/var/tmp/cppgw-slim-strings.75hrNmNR
git worktree add --detach "$scratch/baseline" 9acb016ae9900ffac255b7f3cec02c735f6ddeda
bash "$scratch/run-gate.sh" baseline release "$scratch/baseline" 4
bash "$scratch/run-gate.sh" asan asan "$PWD" 4
bash "$scratch/after-baseline.sh" 8a34ff90d8b5e2dc0850954644de7e5f107d4428
python3 -B cpp/gateway/tools/links.py --check
git diff --check 9acb016a HEAD
```

The portable driver runs the full inventory per invocation. The candidate
release reuses the baseline's object cache in the detached worktree after
switching it to the candidate. The sanitizer invocation adds
`-gline-tables-only -fno-sanitize-recover=all` to the standard driver flags.
Each invocation starts with `env -i` and a new empty home and temporary
directory. The driver verifies loopback access and external-route denial
and gives each binary a private mount, PID and loopback network namespace.

The invocation inside `run-gate.sh` runs from the selected Git worktree.
`instrument` is empty in release and contains the two flags above in ASan.

```bash
session=$(mktemp -d "$scratch/gate.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
  UOS_CI_BUILD_DIR="$scratch/ci" PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$config"
```

## Verification

The driver compiled all 369 production translation units under both profiles.
Links resolved all 448 binaries. The candidate release and quiet sanitizer
rerun agree with the fresh baseline fixture by fixture, with one added case each
in the string and Responses conformance binaries. Release and the quiet rerun
had zero build failures, test failures or timeouts. Both sanitizer runs reported
zero ASan, UBSan and LeakSanitizer diagnostics.

| Gate                   | Passing binaries | Passing cases | Compile s | Link/test s |   Total s |
| ---------------------- | ---------------: | ------------: | --------: | ----------: | --------: |
| Baseline release       |          448/448 |   5,095/5,095 | 3,168.300 |     108.059 | 3,276.885 |
| Candidate release      |          448/448 |   5,097/5,097 | 2,656.524 |      66.633 | 2,724.224 |
| Initial ASan+UBSan     |          446/448 |   5,095/5,097 | 4,798.071 |     361.741 | 5,161.250 |
| Quiet ASan+UBSan rerun |          448/448 |   5,097/5,097 |     0.011 |     328.185 |   328.740 |

The candidate release and initial sanitizer gates tested commit
`8a34ff90d8b5e2dc0850954644de7e5f107d4428`, tree
`983e7fa2ad873d4f090e44c008dba5d8e3f23616`. The quiet sanitizer rerun tested
`b7e903c7ac2e713f80685e7137c59938fbee80b2`, tree
`6568b13b51b96d22c19d7b6db90b75960511b8e0`. The difference is confined to this
report. The candidate gates all consumed input SHA-256
`e21ee4fd36763cf5ef27656f8a5ed32de7e75a031ae793a9db41e0b0acc25b1e`. The fresh baseline consumed
`364b32a62c071736e67560aff226a28d2a6ff4d5f3a24fd57fd677eb19c73580`.

| Receipt under `$scratch/receipts` | SHA-256                                                            |
| --------------------------------- | ------------------------------------------------------------------ |
| `baseline/receipt.json`           | `830c88fe8a2c7800dddf87e18fff2644e8e74c6f31972542c251a017312cc048` |
| `release/receipt.json`            | `1d68d7b9b0c249b63fadef17767fed8abc978b5cbb42957bff3fea09dfff1ee2` |
| `asan/receipt.json`               | `7fa1a96b2f1a9f94fc01e1c10da47dd6588ff5eef4d5e49bab2996e9f97ca2de` |
| `asan-quiet/receipt.json`         | `1abb2541ad43c8b506f4a38ceb452468c2f48dca5c11acdb8ff7bcfbd2860530` |

| Fixture                                             | Baseline release | Candidate release | Quiet ASan+UBSan |
| --------------------------------------------------- | ---------------: | ----------------: | ---------------: |
| `tests/rt/string_test.cpp`                          |            19/19 |             20/20 |            20/20 |
| `tests/rt/text_codec_test.cpp`                      |              6/6 |               6/6 |              6/6 |
| `tests/rt/regex_test.cpp`                           |            20/20 |             20/20 |            20/20 |
| `tests/codex_quota_differential_test.cpp`           |              3/3 |               3/3 |              3/3 |
| `tests/responses_conformance_http_test.cpp`         |              8/8 |               9/9 |              9/9 |
| `tests/anthropic_chat_stream_differential_test.cpp` |              4/4 |               4/4 |              4/4 |

The strict Clang syntax gate passed for all three changed translation units
and both changed tests. The scratch old/new differential matched digest
`6057dda90c227365` across 120,000 cases covering searches, trimming,
replacements and slices. Both versions also passed prefix, suffix and
substring checks across all 65,536 two-byte inputs around `input_`.

Two timing-sensitive fixtures failed in the first sanitizer run while the
release rebuild was active.

- `codex_supervisor_secret_test.cpp` passed 40 of 41 cases. Its
  repeated fat-comma case passed redaction and missed the two-second assertion.
- `jev_compaction_test.cpp` passed 22 of 23 cases. Its floor-bound summary case
  reported `regular expression execution limit exceeded`.

Dedicated-CPU controls used each test's existing Links closure and ASan
objects, then relinked with the starting or candidate string implementation.
There were no other changed application objects in either closure. Both
string implementations passed all 41 redaction cases and all 23 compaction
cases on CPU 3 at nice −15. The full sanitizer rerun used two jobs and the same
cached objects after the release build and timing measurement finished.

```bash
python3 -B "$scratch/timing/control.py" "$PWD"
python3 -B "$scratch/timing/jev-control.py" "$PWD"
for variant in baseline candidate; do
  bash "$scratch/measure/run-isolated-sanitized.sh" "timing-$variant"
  bash "$scratch/measure/run-isolated-sanitized.sh" "jev-$variant"
done
```

```bash
bash "$scratch/run-gate.sh" asan-quiet asan "$PWD" 2
python3 -B "$scratch/reconcile.py"
bun --no-env-file --no-install run format:check \
  cpp/gateway/docs/DEVIATIONS-FROM-BUN.md cpp/gateway/docs/findings/cppgw-slim-strings-20261118.md
git diff --check 9acb016a HEAD
```

## ARM64 timings

The isolated string benchmark pinned the process to CPU 3 and alternated five runs per
revision. Each number is median nanoseconds per call, followed by the range.

| Operation                                    |              Before |               After |
| -------------------------------------------- | ------------------: | ------------------: |
| UTF-8 `indexOf`, 300,000 calls               | 1,287 [1,283–1,335] | 1,300 [1,297–1,341] |
| UTF-16 conversion plus `find`, 300,000 calls |       571 [568–591] |       573 [571–592] |
| UTF-8 `slice`, 300,000 calls                 |       740 [736–758] |       737 [735–738] |
| Short ASCII `trim`, 1,800,000 calls          |    24.2 [24.0–26.8] |    27.9 [27.9–28.0] |

The two indexing paths were measured on strings of 86–590 UTF-8 bytes
containing alternating two-byte and supplementary characters. The direct
UTF-16 reference assumes valid UTF-8 and a bounded integer position. The
retained UTF-8 search and slice had overlapping before-and-after ranges.
The simplified trim loop added about 3.8 ns per call on its short ASCII mix.
Clang 21 release objects confirm that the string object's `.text`
section shrank from 32,256 to 31,560 bytes. Across the three changed
translation units, `.text` fell from 68,864 to 68,640 bytes.

```bash
bash "$scratch/measure/run-isolated-priority.sh" before-strings
bash "$scratch/measure/run-isolated-priority.sh" after-strings
llvm-size-21 -A "$scratch/measure/before-string.o" "$scratch/measure/after-string.o"
```

Seven interleaved request-path pairs used the actual header filter and
Responses normalizer from the two release builds. After 4,096 warm-up calls per
operation, each run timed 400,000 quota filters and 350,000 normalizations per
type. The loopback conformance gateway received 960 timed authenticated
requests per run using a 1,031-byte `input_` type. Each run used 40 fresh
fixture batches with six warm-up requests and 24 timed requests per batch.
All 13,440 timed requests returned HTTP 400. Each batch verified the error
field, observed zero upstream calls and reopened its SQLite state after drain.
Processes ran pinned to CPU 3 at nice −15 with no concurrent build. Values
are medians with the full sample ranges.

| Operation                             |              Before |               After |
| ------------------------------------- | ------------------: | ------------------: |
| Quota filter, eight headers (µs/call) | 2.349 [2.342–2.353] | 1.592 [1.591–1.616] |
| Responses short ASCII type (µs/call)  | 1.423 [1.415–1.444] | 1.409 [1.407–1.412] |
| Responses long UTF-8 type (µs/call)   | 3.415 [3.406–3.430] | 1.409 [1.405–1.414] |
| Loopback Responses 400 (ms/request)   | 2.012 [1.990–2.060] | 2.001 [1.959–2.088] |

Quota filtering took 32% less time and long-type normalization took 59% less
time in this workload. The loopback latency ranges overlap.

```bash
python3 -B "$scratch/measure/link-request.py" before baseline
python3 -B "$scratch/measure/link-request.py" after release
bash "$scratch/measure/run-comparison.sh"
```

## Handoff

Both the GitHub HTTPS preflight and the push of `bba6a0fc` exited 128 because
write authentication was unavailable. The fetched `origin/development` tip is
`a3a96bc5fe96ba383936746fae23e64daa69562f` and predates the C++ gateway.
Publication and integration into `development` are pending. The verified
commits are on the clean local `agent-job` branch. The handoff bundle is
`/var/tmp/cppgw-slim-strings.75hrNmNR/cppgw-slim-strings-20261118.bundle`.

```bash
env -u GIT_TRACE -u GIT_TRACE_CURL -u GIT_CURL_VERBOSE \
  GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false \
  git push --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-slim-strings-20261118
git fetch --no-tags origin '+refs/heads/development:refs/remotes/origin/development'
git bundle create "$scratch/cppgw-slim-strings-20261118.bundle" agent-job ^9acb016a
git bundle verify "$scratch/cppgw-slim-strings-20261118.bundle"
```

## Existing guide and packaged validation

The runtime's general text guide still lists the retired APIs. The two
packaged-validation defects in the starting receipt remain outside this
lane. Both `chat-invalid-role` and `responses-invalid-tool-type` returned 200
in that separate packaged probe. [Starting findings](cppgw-integrate-s3b-20261118.md#open-findings-and-handoff)

## Summary

The C++20 string refactor removed **50 first-party production lines**,
from **180,460 to 180,410**. Linux ARM64 release and ASan+UBSan each passed
**448 binaries and 5,097 cases**. The sanitizer rerun cleared the two
timing-sensitive failures from the concurrent run.

The quota filter and long Responses type check took 32% and 59% less time
in the focused benchmark. End-to-end loopback latency was within measurement
variation. Short ASCII trim took about 4 ns longer per call. GitHub write
authentication blocked publication, so the verified commits remain on the
clean local `agent-job` branch.
