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

## Verification in progress

The strict Clang syntax gate passed for the three changed translation units
and both changed tests. Links resolved all 448 binaries. The scratch
old/new differential matched digest `6057dda90c227365` across 120,000 cases
covering searches, trimming, replacements and slices. Both versions also
passed prefix, suffix and substring checks across all 65,536 two-byte inputs
around `input_`. The full baseline release, candidate release and sanitizer
receipts are pending.

The runtime's general text guide still lists the retired APIs. The two
packaged-validation defects in the starting receipt remain outside this
lane. Both `chat-invalid-role` and `responses-invalid-tool-type` returned 200
in that separate packaged probe. [Starting findings](cppgw-integrate-s3b-20261118.md#open-findings-and-handoff)
