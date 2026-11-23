# C++ gateway date, string and Apple Silicon follow-ups

## Work contract v1

This follow-up targets `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1` at
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. Bring in the three reviewed branches in the requested
order using ordinary merge commits. Retain every deviation entry from both sides and commit each
merge and fix separately.

| Surface                | Required behavior                                                                                                                  |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| UTC date helpers       | Use the C++20 calendar and preserve the gateway's UTC, validated input and date-range contracts.                                   |
| Fixed protocol strings | Use byte comparisons for the Codex quota and Responses tokens. Keep Unicode behavior for callers that need it.                     |
| Apple Silicon          | Build and link every registered vendor, including QuickJS-ng `libregexp`, and retain the four Darwin stored-value fixture updates. |
| Provider traffic       | Use local mocks only. Run with an empty home, a fresh temporary directory and a clean environment.                                 |

The owned paths are the three branches' changes under `cpp/gateway/`, the two macOS build entry
points and this report. The workflow, TypeScript, the rest of `cpp/`, live providers and deployments
are out of scope. Build products and upstream dependencies belong in scratch outside the checkout.

The acceptance gates are `python3 cpp/gateway/tools/links.py --check`, the date, string, Codex quota,
Responses request, clock, filesystem and stored-value tests in `release` and `asan`, and
`bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4`. Check shell and Python syntax for
the macOS build changes on Linux. The Linux runner cannot execute either macOS build. GitHub write
access is unverified. Stop after the local gates and follow-up handoff, with the remaining evidence
recorded here.

## Merge and build changes

| Follow-up                           | Fetched tip                                | Merge commit                               |
| ----------------------------------- | ------------------------------------------ | ------------------------------------------ |
| C++20 UTC calendar                  | `a9ba3f3dd255d487117b319bd907b87924d2d5af` | `30c02865b06b912adf5c9db3183c55fc40dd7f2f` |
| Byte comparisons and string cleanup | `76bf1b523f95f2c690fb21ce08d738b8e9686c39` | `24d1ad4709028b3f1ba4466fa7fd0121148a3ac6` |
| Apple Silicon build and fixtures    | `e8b8bbc32920a4d9d345485d9c6569ddad8d41dc` | `96ae7d3a4db2026280d887f443d2d48d2bed8277` |

The only conflict was in `docs/DEVIATIONS-FROM-BUN.md` during the string merge. The resolution kept the
existing integration guidance, the UTC calendar section and the protocol string section. The date and
macOS merges needed no manual resolution. The date and string production sources and headers dropped
148 lines in total compared with the integration base.

The merged `tools/macos-build.sh` archives all six registry entries and applies the registry's GNU C11
flags to QuickJS-ng's `libregexp.c` and `libunicode.c`. The production and runtime test closures both
select `libuos_vendor_quickjs-libregexp.a`. Commit `8f693261eadd24150fbc98bb81a6ee9e3544099f` makes the
packager require the complete archive set from the clean-source receipt, then links every registered
archive and records its version and license. A comparison of the Darwin fixtures confirmed that all
50 verification and 28 bootstrap inputs are intact. Four malformed-JSON cases still require
`SyntaxError` and now expect the integrated parser's `Invalid JSON` message.

## Linux toolchain

The runner used Ubuntu 24.04.5 ARM64, kernel `7.0.0-1017-azure`, four CPUs and 15 GiB RAM. Clang 21.1.8
came from the signed apt.llvm.org `llvm-toolchain-noble-21` repository. OpenSSL 3.0.13, zlib 1.3,
Brotli 1.1.0, Zstd 1.5.5 and mold 2.30.0 came from apt. Boost, SQLite and the fixture's ICU 78.3
were prepared outside the checkout at `scratch=/var/tmp/cppgw-followups-1-20261118.HlJk57Xn`.

| Verified upstream archive | SHA-256                                                            | Build and install wall time |
| ------------------------- | ------------------------------------------------------------------ | --------------------------: |
| Boost 1.92.0              | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |                     61.36 s |
| ICU4C 78.3                | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |                    160.52 s |
| SQLite 3.54.0             | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |                     62.21 s |

```bash
scratch=$(mktemp -d /var/tmp/cppgw-followups-1-20261118.XXXXXXXX)
mkdir "$scratch/downloads" "$scratch/receipts"
(
  cd "$scratch/downloads"
  curl --fail --silent --show-error --location --max-time 25 -o llvm-snapshot.gpg.key \
    https://apt.llvm.org/llvm-snapshot.gpg.key
  gpg --batch --yes --dearmor --output llvm-snapshot.gpg llvm-snapshot.gpg.key
  test "$(gpg --batch --show-keys --with-colons llvm-snapshot.gpg | awk -F: '$1=="fpr"{print $10; exit}')" \
    = 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
  sudo -n install -m 0644 llvm-snapshot.gpg /usr/share/keyrings/cppgw-llvm-snapshot.gpg
  printf '%s\n' 'deb [signed-by=/usr/share/keyrings/cppgw-llvm-snapshot.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' \
    | sudo -n tee /etc/apt/sources.list.d/cppgw-llvm-21.list >/dev/null
)
sudo -n apt-get update -qq
sudo -n apt-get install -y -qq clang-21 libssl-dev zlib1g-dev libicu-dev libbrotli-dev \
  libzstd-dev util-linux iproute2 pkg-config ninja-build mold
(
  cd "$scratch/downloads"
  curl -fLSs --retry 2 -o boost_1_92_0.tar.bz2 https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2
  curl -fLSs --retry 2 -o icu.tgz https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz
  curl -fLSs --retry 2 -o sqlite.tar.gz https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz
  sha256sum -c <<'SHA256'
5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c  boost_1_92_0.tar.bz2
3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0  icu.tgz
134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed  sqlite.tar.gz
SHA256
  tar -xjf boost_1_92_0.tar.bz2 -C ..
  tar -xzf icu.tgz -C ..
  tar -xzf sqlite.tar.gz -C ..
)
printf '%s\n' 'using gcc : 14 : g++-14 ;' > "$scratch/boost-user-config.jam"
(cd "$scratch/boost_1_92_0" && ./bootstrap.sh --with-libraries=json,container &&
  ./b2 --user-config="$scratch/boost-user-config.jam" -j2 --with-json --with-container \
    variant=release link=static threading=multi toolset=gcc-14 stage)
mkdir "$scratch/icu-build"
(cd "$scratch/icu-build" && CC=gcc-14 CXX=g++-14 ../icu/source/configure --prefix="$scratch/icu78" \
  --disable-tests --disable-samples --disable-extras --disable-static && make -j2 && make install)
(cd "$scratch/sqlite-autoconf-3540000" && ./configure --prefix="$scratch/sqlite" \
  --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3' && make -j1 && make install)
```

## Verification commands

The runner used fresh, empty HOME and TMPDIR for every invocation. The driver's sudo fallback proved
loopback-only access in fresh Linux network, mount and PID namespaces and ran the fixtures as UID
1001 without provider variables. The SQLite probe loaded 3.54.0 inside that boundary. The fixtures used local mocks.

```bash
session=$(mktemp -d "$scratch/links.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$session/home" TMPDIR="$session/tmp" \
  python3 cpp/gateway/tools/links.py --check

gate() {
  local config=$1 session instrument=''
  shift
  session=$(mktemp -d "$scratch/gate.XXXXXXXX")
  mkdir "$session/home" "$session/tmp"
  if [[ $config == asan ]]; then instrument='-gline-tables-only -fno-sanitize-recover=all'; fi
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
    HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
    UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
    PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
    UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
    UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
    bash cpp/gateway/tools/ci-build.sh --config "$config" "$@"
}

gate release --shard 1/4
focused=(tests/rt/date_test.cpp tests/rt/string_test.cpp tests/codex_quota_differential_test.cpp
         tests/responses_conformance_http_test.cpp tests/rt/clock_test.cpp tests/rt/fs_test.cpp
         tests/storage_value_compatibility_test.cpp tests/native_addon_differential_test.cpp)
for config in release asan; do
  for test in "${focused[@]}"; do
    if [[ $config == release && $test == tests/codex_quota_differential_test.cpp ]]; then continue; fi
    gate "$config" "$test"
  done
done
```

`bash -n cpp/gateway/tools/macos-build.sh` and Python `compile()` checks of its embedded Python and
`cpp/gateway/packaging/macos/build.py` passed. The static registry check verified both QuickJS-ng C sources,
GNU C11 selection, the test and production link closures and the vendored license. Scoped
`bun --no-env-file run format:check` passed for all six changed Markdown files using frozen lockfiles and
Bun 1.4.0 in scratch. `git diff --check` passed for the complete follow-up.

## Linux ARM64 results

The driver receipts identify commit `05a435c392808be4f1e3f1c5d73f52b5d3b581bb`, tree
`13d6beca9b4f7c82ba1fd6bf08c7767b50a9c58b`, and unchanged Linux test input SHA-256
`253175d7e36c429573ff62a45eff87265e64eb4100089eb90ca9e17196b6036b`. Only this report changed
subsequently. Both build profiles compiled all 369 production objects and built all six vendor archives.

| Gate                             | Binaries passed | Cases passed | Compile (s) | Link and test (s) | Driver wall (s) |
| -------------------------------- | --------------: | -----------: | ----------: | ----------------: | --------------: |
| Release shard 1/4                |         113/113 |  1,333/1,333 |     956.880 |            32.839 |         990.291 |
| Additional focused release tests |             7/7 |        69/69 |      46.928 |             3.151 |          53.294 |
| Focused ASan, UBSan and LSan     |             8/8 |        72/72 |     972.729 |             6.374 |         982.884 |

Release covered 120 unique binaries and 1,402 cases. Codex quota ran once in release shard 1/4 and is
included in the focused counts below. All 450 test paths resolved in `links.py --check`. The eight
sanitizer logs each recorded the namespace proof and contained zero sanitizer diagnostics.

| Focused test                                 |   Release |      ASan |
| -------------------------------------------- | --------: | --------: |
| `tests/rt/date_test.cpp`                     |       7/7 |       7/7 |
| `tests/rt/string_test.cpp`                   |     21/21 |     21/21 |
| `tests/codex_quota_differential_test.cpp`    |       3/3 |       3/3 |
| `tests/responses_conformance_http_test.cpp`  |       9/9 |       9/9 |
| `tests/rt/clock_test.cpp`                    |     11/11 |     11/11 |
| `tests/rt/fs_test.cpp`                       |     13/13 |     13/13 |
| `tests/storage_value_compatibility_test.cpp` |       4/4 |       4/4 |
| `tests/native_addon_differential_test.cpp`   |       4/4 |       4/4 |
| **Total**                                    | **72/72** | **72/72** |

The 16 JSON receipts, command output and receipt hashes are under `$scratch/receipts`. This command
checked the exact commit and tree, selection, immutable inputs, vendor archive counts, exit codes and
sanitizer logs against the driver output.

```bash
python3 -B "$scratch/verify-receipts.py" "$scratch/receipts" 05a435c392808be4f1e3f1c5d73f52b5d3b581bb
```

## Handoff

The follow-up targets the integrated branch. [PR #1383](https://github.com/ubiquity/ai.ubq.fi/pull/1383)

The Apple Silicon build, Darwin fixture execution and release packaging remain unexecuted for this
combined tree. The remaining release shards, full sanitizer shards and fuzz campaigns are left to the
automatic branch workflow. No defects outside the owned paths surfaced in the local checks.

The direct push and its preflight both exited 128 because GitHub required credentials. The clean
`agent-job` branch and its verified bundle in scratch preserve the follow-up for an authenticated
push. The pull request and hosted receipts are pending publication.

```bash
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-followups-1-20261118
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-followups-1-20261118
git bundle create "$scratch/cppgw-followups-1-20261118.bundle" agent-job ^aae39d0ea73e7b5678dc92c437f7da6fb32f0c06
git bundle verify "$scratch/cppgw-followups-1-20261118.bundle"
```

## Summary

Merged all three follow-ups and wired QuickJS-ng into the Apple Silicon package. On Linux ARM64,
release shard 1/4 passed 113 binaries and 1,333 cases, and all eight focused tests passed in both
release and sanitizer builds. Apple Silicon execution remains pending. GitHub write access blocked
publication, so the follow-up is committed locally.
