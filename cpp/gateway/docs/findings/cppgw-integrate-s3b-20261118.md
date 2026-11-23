# C++ gateway library integration, stage 3b

## Work contract v1

Start at the verified stage 2b tip `af7a0036`. Merge the three reviewed tips
from `origin` as ordinary two-parent commits, in the order below. The integration
owns `cpp/gateway/`. TypeScript, workflows, the pilot outside `cpp/gateway/`,
production services, credentials and live data are outside this stage.

| Surface           | Required behavior                                                                                                                                        |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Bundled resources | Use filesystem paths for trusted paths. Preserve untrusted URL and permission validation and the file response contract.                                 |
| Numbers           | Remove the reviewed unused helpers. Preserve active formatting, parsing and gateway accounting.                                                          |
| Regex             | Use the matching quickjs-ng source and keep the gateway's validation and stored-value contracts. Library diagnostics and Unicode set support may differ. |
| Depth and vendors | Preserve the JSON limit of 128 and the independent stored-graph limit of 2,000. Keep all registered libraries and the exact incoming vendor bytes.       |

Compare the fetched branch reports and actual deltas before resolving conflicts.
The conflict set and detailed fixture differences are open at discovery. Build
and run affected tests after each merge, then run all four
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4` selections and
`--config asan` for regex, KV, URL, static files, numbers and permissions.
Verify the Links registry and the vendored bytes. Every test uses an empty home,
fresh temporary storage and the portable driver's isolated mock network.
The stage ends with the ordered merge ancestry, passing gates, and a report of
the exact commands, counts, measurements and any unfinished work.

## Starting checkout

Ubuntu 24.04.5 on AArch64 with four CPUs. The checkout starts at the stage 2b
report commit with that stage's 448 release binaries and 5,086 cases.
[Stage 2b findings](cppgw-integrate-s2b-20261118.md)

| Order | Ref fetched from origin                                  | Incoming tip                               |
| ----- | -------------------------------------------------------- | ------------------------------------------ |
| 1     | `agent-jobs/cppgw-slim-file-urls-20261118-44298551168-1` | `caa5ee5c544ac2745533e6a97e1cc5e6f18a31e0` |
| 2     | `agent-jobs/cppgw-slim-numbers-20261118-44298517687-1`   | `dcf103473cbca9c185640c69b7ad0fb432187388` |
| 3     | `agent-jobs/cppgw-std-regex-c2-44298488608-1`            | `8ed010b9da77b0c87f8a3bf089e85934cc692651` |

Dependency and test evidence live outside the checkout at
`/var/tmp/cppgw-integrate-s3b.2sXMMKTy`. The runner uses Clang 21.1.8,
Boost 1.92.0, SQLite 3.54.0 with `SQLITE_DQS=3`, and ICU 78.3.

## Merge checkpoints

1. `84f5adbaadf6d0a0a0806fa1cde8135d1ebd1309` integrates the filesystem-path
   branch. Git conflicted only in `docs/DEVIATIONS-FROM-BUN.md`. The resolution
   retains the stage 2b sections and appends the trusted-resource section.
   The first focused build on `46a1492a` compiled all 369 production objects,
   passed ten binaries and 98 cases, and found one build failure. The new
   Linux permissions fixture called the removed `rt::path::basename` twice.
   The merge resolution uses `std::filesystem::path::filename()` for both
   checks and retains the assertions. The 11-binary rerun passed all 104 cases.
2. `029bd0d1ddd11426b1a416c07c2d4079a601df51` integrates the numeric cleanup.
   Git conflicted only in the deviations ledger. The resolution appends the
   numeric section. The automatic Number fixture merge carries forward the
   stage 2b BigInt rejections for `+`, `-` and `0x `. A source search found
   no calls to the deleted helpers after the merge. Its focused gate passed all 20
   binaries and 368 cases.
3. `51408a849928ae47a0a9b3fc1f6da02dbf80dcc9` integrates QuickJS-ng
   libregexp 0.18.0. The four textual conflicts and their resolutions are listed below.

The requested regex tip records the continuation sweep in
[`cppgw-std-regex-20261117.md`](cppgw-std-regex-20261117.md).
The named `cppgw-std-regex-c2.md` has no blob at that tip.
The two KV fixtures and their previous diagnostics are present in that sweep.

| Conflict                      | Resolution                                                                                                                                                                                  |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `CMakeLists.txt`              | Keep the stage 2b C frontend and C11 defaults. Supply the QuickJS system headers to the three regex objects through the registered archive, alongside the existing QCBOR consumer handling. |
| `cmake/vendors.json`          | Add the three QuickJS consumers to the registry alongside Ada, llhttp, QCBOR, simdjson and simdutf.                                                                                         |
| `tools/vendor_sources.py`     | Register both QuickJS C sources, its include directory, its pinned manifest, and its headers in the existing multi-source and input-digest model.                                           |
| `docs/DEVIATIONS-FROM-BUN.md` | Keep the prior ledger and append the regex syntax, Unicode, resource-limit and stored-value entries.                                                                                        |

The regex sources, public header, and upstream test overlay match the incoming tip.
The fuzz policy retains all 21 existing targets and adds the incoming `regex_library`
target and its seven seeds. The Links resolver merged without a conflict. Its regex
test input list now includes ten QuickJS files. All 38 SHA-256 entries across the
six vendor manifests pass. Git blob comparison matches the existing vendor files
to stage 2b and QuickJS to its incoming tip.

Clang 21 rejected the QuickJS C sources in strict C11 because POSIX declarations
were unavailable. Clang 21 and GCC 14 both accepted GNU C11. Only the QuickJS
C objects use that dialect. The portable build, CMake, standalone unit make rules,
request-path builder, Darwin builder, fuzz compiler, and sanitizer compiler carry
the setting through their existing C frontend. The portable profile compiles both
C objects in the selected instrumentation profile and links their static archive.

## Stored-value expectations

The original Bun captures stay in place. The C++ tests apply the engine diagnostics
to the nine named malformed inputs and require rejection with `SyntaxError`.
The typed-value round trip runs the full three-expression vector through the
ordinary and native decoders, checks the stored values, and compares the encoded
graph to the original JSON bytes.

| Fixture and input                         | Previous expectation                                                                 | Integrated expectation                                                                                 |
| ----------------------------------------- | ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| Typed graph `regexps`, `/a/v`             | `SyntaxError: Invalid regular expression: rt::Regex does not support the v flag`     | Accept source `a` and flags `v`. Both decoders agree and the original graph round-trips byte for byte. |
| Typed graph `regexp dup flags`, `gg`      | `Invalid flags supplied to RegExp constructor.`                                      | `invalid regular expression flags`                                                                     |
| Typed graph `regexp flags number`, `5`    | `Invalid flags supplied to RegExp constructor.`                                      | `invalid regular expression flags`                                                                     |
| Typed graph `regexp flags undefined atom` | `Invalid flags supplied to RegExp constructor.`                                      | `invalid regular expression flags`                                                                     |
| Typed graph `regexp flags empty set`      | `Invalid flags supplied to RegExp constructor.`                                      | `invalid regular expression flags`                                                                     |
| Typed graph `regexp flags bigint`, `7`    | `Invalid flags supplied to RegExp constructor.`                                      | `invalid regular expression flags`                                                                     |
| Typed graph `regexp bad pattern`, `(`     | `Invalid regular expression: missing )`                                              | `expecting ')'`                                                                                        |
| Profile JSON `/[/`                        | `SyntaxError: Invalid regular expression: missing terminating ] for character class` | `SyntaxError: unexpected end`                                                                          |
| Profile JSON `/a/zz`                      | `SyntaxError: Invalid flags supplied to RegExp constructor.`                         | `SyntaxError: invalid regular expression flags`                                                        |
| Profile JSON `noslash`                    | `SyntaxError: Invalid flags supplied to RegExp constructor.`                         | `SyntaxError: invalid regular expression flags`                                                        |

The JSON parser and serializer, stored-value projection, JSC reader and writer,
and typed-value codec match the stage 2b source blobs. The JSON cap remains
128 containers. The independent JSC and typed-graph cap remains 2,000.
The release and sanitizer selections include the JSON and stored-graph depth fixtures.

## First-party production source

Counts are tracked physical newlines, including comments and blank lines, in
C and C++ sources and headers under `src`, `include`, `runtime`, and `entrypoints`.
The count uses `git ls-tree -rz` and `git cat-file --batch` over each tree's Git blobs.
The 745-file denominator excludes tests, tools and vendors.

| Checkpoint             |     Lines | Change from prior checkpoint |
| ---------------------- | --------: | ---------------------------: |
| Stage 2b               |   186,033 |                              |
| Filesystem-path merge  |   185,996 |                          −37 |
| Numeric merge          |   185,877 |                         −119 |
| Regex merge            |   180,460 |                       −5,417 |
| **Stage 3b reduction** | **5,573** |         **3.0% of stage 2b** |

The six first-party regex files went from 6,318 to 901 lines.

## Runner and prerequisites

Linux `7.0.0-1017-azure`, Ubuntu 24.04.5, AArch64 and four CPUs. Clang and
Clang++ 21.1.8 came from the signed apt.llvm.org Noble 21 archive with key
fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. Ninja 1.13.2 and
mold 2.30.0 were used for portable builds. Native package versions were
OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. SQLite reports
`DQS=3` in `pragma compile_options`.

| Archive                          | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| `boost_1_92_0.tar.bz2`           | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

```bash
sudo apt-get update -o Acquire::Retries=2
sudo apt-get install -y clang-21 libclang-rt-21-dev llvm-21 libssl-dev \
  zlib1g-dev libicu-dev libbrotli-dev libzstd-dev pkg-config util-linux mold

# In the Boost source
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage

# In the SQLite source
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install

# In the out-of-tree ICU build directory
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install
```

## Verification commands

`run-gate.sh` runs each lane with `env -i`. The per-invocation home and temporary
directories are empty. The portable driver verifies loopback reachability and
outside-route denial, uses Linux network, mount and PID namespaces via `sudo -n`,
and runs each binary as UID 1001 with a private tmpfs and fresh fixture state.
Provider fixtures use local mocks. The staged ICU and SQLite library directories
stay visible inside the private `/tmp` through the recorded RPATH.

```bash
scratch=/var/tmp/cppgw-integrate-s3b.2sXMMKTy
bash "$scratch/run-gate.sh" merge-1 release --select file-urls
bash "$scratch/run-gate.sh" merge-1-final release --select file-urls
bash "$scratch/run-gate.sh" merge-2 release --select numbers
CPPGW_GATE_JOBS=2 bash "$scratch/run-gate.sh" merge-3 release --select regex-kv

# Release and sanitizer build trees have independent locks.
for shard in 1 2 3 4; do
  CPPGW_GATE_JOBS=2 bash "$scratch/run-gate.sh" "release-$shard" release --shard "$shard/4"
done
CPPGW_GATE_JOBS=2 bash "$scratch/run-gate.sh" asan asan --select asan
```

For each shard the wrapper runs `bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"`
with the following environment. For each focused gate it runs the same driver's
`main()` via `python3 -B "$scratch/select-ci.py" "$scratch/$selection.txt" --config "$config"`.
The selector replaces only the test path inventory with a validated subset of
the registry. The driver builds all 369 production objects. The first two
focused gates used four workers. The final lanes used two workers each while both were active. Release shard 4
used four workers after the sanitizer run completed. The sanitizer flags add
`-gline-tables-only -fno-sanitize-recover=all` to both languages.

```bash
session=$(mktemp -d "$scratch/gate.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/ci" PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
```

The focused selections cover file URL and IDNA, static assets, immutable SQLite,
startup, permissions and native KV for the first merge. The second selection
covers Number, JSON, KV, SQLite, Codex dispatch and every Links consumer of the
changed banked-reset harness, plus the changed Sentinel fixtures. The third
selection covers both storage codecs, JSON and JSC depth, regex, image validation,
and compaction. The 41-test sanitizer selection also includes the KV migration
and storage families and all of the requested surfaces. The exact path lists are
in `file-urls.txt`, `numbers.txt`, `regex-kv.txt`, and `asan.txt` in the scratch root
and in each JSON receipt.

```bash
PYTHONDONTWRITEBYTECODE=1 python3 cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/tools/links.py --hashes tests/rt/regex_test.cpp
python3 -B cpp/gateway/tools/vendor_sources.py cmake
for vendor in ada llhttp qcbor quickjs-libregexp simdjson simdutf; do
  (cd "cpp/gateway/third_party/$vendor" && sha256sum --check SHA256SUMS)
done
CC=clang-21 CXX=clang++-21 python3 -B cpp/gateway/cmake/ci/test_driver.py
git diff --check af7a0036 HEAD -- . ':(exclude)cpp/gateway/third_party'
```

Links resolved 448/448 binaries. The seven portable driver controls passed in
9.909 seconds. Changed documents and build JSON passed
`bun --no-env-file --no-install run format:check` with the Bun 1.4.0 runtime
and root and tools locks installed with `--frozen-lockfile --ignore-scripts`
in scratch.

```bash
UOS_GATEWAY_BUILD_EVIDENCE=stage-3b-runner UOS_GATEWAY_TOOL_ROOT="$scratch/native-tool" \
  cmake -S cpp/gateway -B "$scratch/cmake-final" -G Ninja -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 -DCMAKE_BUILD_TYPE=Release \
  -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_TESTS=ON
ninja -C "$scratch/cmake-final" -t commands uos_bench_regex
```

The scratch preview used the same CMake options and built the six vendor targets,
the three C++ QuickJS consumers, and both KV test objects. `ar t` reported
`libregexp.c.o` and `libunicode.c.o` in the QuickJS archive from CMake and from
`tools/unit.mk`. The legacy sanitizer and disposable fuzz compile probes each
built both C sources. The Darwin shell and embedded Python passed syntax checks.

## Build and test receipts

The last code commit is `51408a849928ae47a0a9b3fc1f6da02dbf80dcc9`,
Git tree `b9f6a2e6ccf2576b5655c6580981546344f55026`. The four
release shards and the sanitizer run bind source input SHA-256
`364b32a62c071736e67560aff226a28d2a6ff4d5f3a24fd57fd677eb19c73580` at start and finish.
Each completed all 369 production objects. The release shard selections are
disjoint and cover the full Links inventory.

| Gate                              | Passing binaries |   Passing cases | Compile s | Link/test s |  Total s | Peak process KiB |
| --------------------------------- | ---------------: | --------------: | --------: | ----------: | -------: | ---------------: |
| Filesystem path, initial cold run |            10/11 |           98/98 |   792.656 |       7.407 |  800.349 |          601,384 |
| Filesystem path, corrected        |            11/11 |         104/104 |     3.784 |       7.583 |   11.656 |          233,012 |
| Numbers                           |            20/20 |         368/368 |   821.871 |       9.371 |  831.538 |          601,208 |
| Regex and KV                      |            11/11 |         155/155 |   798.951 |       6.316 |  805.570 |          601,252 |
| Release 1/4                       |          112/112 |     1,319/1,319 |   653.809 |      26.088 |  680.255 |          686,872 |
| Release 2/4                       |          112/112 |     1,245/1,245 |   572.177 |      36.220 |  608.820 |          671,076 |
| Release 3/4                       |          112/112 |     1,358/1,358 |   596.841 |      52.763 |  649.968 |          488,440 |
| Release 4/4                       |          112/112 |     1,173/1,173 |   310.478 |      22.206 |  333.041 |          589,240 |
| ASan/UBSan                        |            41/41 |         667/667 |  2514.202 |     115.476 | 2629.992 |          967,744 |
| **Release total**                 |      **448/448** | **5,095/5,095** |           |             |          |                  |

The initial filesystem-path run exited 1 with a permissions fixture compile
error. Its ten executed binaries passed all 98 reported cases. The corrected
run and every later gate exited 0 with zero build failures, test failures or timeouts.

| Receipt under `$scratch/receipts` | SHA-256                                                            |
| --------------------------------- | ------------------------------------------------------------------ |
| `merge-1.json`                    | `01fda853eaa76b1ea4b5132161781a9c7914e03a60e719edec8331ccf028ef4f` |
| `merge-1-final.json`              | `bec9dd41fd68aa4049889bcd02f0c0d70fa7d4384fd3eca3a635efc43b4c912f` |
| `merge-2.json`                    | `eec6888765532e8fef3722765f7a555503e2d777583ea9009c73105821e37cfa` |
| `merge-3.json`                    | `b1301a65f4534a37afc72059d20de60483d4aa6abec227fd571e7562a1b8a381` |
| `release-1.json`                  | `b3f853981e2b44c2ab4445cd26296d715de3ac05eb66841ab11fff505a8939db` |
| `release-2.json`                  | `8f4d28685a5c0c781dd64be4a14dfbb2564731e97fdbcb233e3eb13c1808a0f2` |
| `release-3.json`                  | `d45d156851a458c115c5445bc9275639e01fbdf3e7cb29bcc67512e984fc4b75` |
| `release-4.json`                  | `3887ed0a8ed6f1bc8537996cb186ee850ab44f6e4ac8173986a3b2e997d860d1` |
| `asan.json`                       | `3756d5d58f3d00ba72dc5e4bde3ecbf9c498604e68971c91d30bc3db6a5b2bb0` |

| Sanitizer fixture                                  | Passing cases |
| -------------------------------------------------- | ------------: |
| `rt/regex_test.cpp`                                |         20/20 |
| `kv_conversion_typed_value_differential_test.cpp`  |         10/10 |
| `kv_conversion_profile_json_differential_test.cpp` |           5/5 |
| `rt/kv_jsc_deserialize_test.cpp`                   |           8/8 |
| `rt/kv_jsc_sparse_test.cpp`                        |           2/2 |
| `rt/json_test.cpp`                                 |         25/25 |
| `rt/url_test.cpp`                                  |           7/7 |
| `rt/url_idna_test.cpp`                             |           3/3 |
| `static_assets_test.cpp`                           |         38/38 |
| `static_differential_test.cpp`                     |           1/1 |
| `rt/number_test.cpp`                               |         21/21 |
| `permissions_linux_test.cpp`                       |           6/6 |

The focused regex gate passed 11/11 binaries and 155/155 cases, including
20/20 regex cases and both KV suites. The scan of all 41 sanitizer run logs
found zero AddressSanitizer, UndefinedBehaviorSanitizer or LeakSanitizer
diagnostics. The verifier checks per-binary status, case counts, source
identity, shard disjointness and full registry coverage.

```bash
python3 -B "$scratch/verify-receipts.py" merge-1-final merge-2 merge-3 \
  release-1 release-2 release-3 release-4 asan
```

## Open findings and handoff

- The older runtime guides still describe the retired numeric functions
  and the former engine's Unicode-set limitations.
  [Values guide](../rt/values.md) · [Regex guide](../rt/regex.md)
- Stage 2b recorded two shared gateway validation failures. `chat-invalid-role`
  and `responses-invalid-tool-type` returned HTTP 200 where independent
  conformance asserts HTTP 400. These remain open in the shared schema paths.
  [Stage 2b conformance findings](cppgw-integrate-s2b-20261118.md#paired-conformance-on-the-packaged-release)

The GitHub HTTPS push of the tested code exited 128 while requesting GitHub
write authentication. Refreshed `origin/development` is
`2ac5aae69b0f0bac77cf1c6f329995cf1187616c` and predates the C++ gateway.
Each of the three merge tips is outside that ancestry. Branch publication
and integration into `development` remain pending. The runner handoff is
the clean local `agent-job` branch and its bundle. The report commit adds
only this file to the tested tree.

```bash
env -u GIT_TRACE -u GIT_TRACE_CURL -u GIT_CURL_VERBOSE \
  GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false \
  git push --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-integrate-s3b-20261118
git fetch --no-tags origin '+refs/heads/development:refs/remotes/origin/development'
for tip in 84f5adba 029bd0d1 51408a84; do
  git merge-base --is-ancestor "$tip" origin/development
done
git bundle create "$scratch/cppgw-integrate-s3b-20261118.bundle" agent-job ^af7a0036
git bundle verify "$scratch/cppgw-integrate-s3b-20261118.bundle"
```

## Summary

All three merges are committed in order on `agent-job`. Linux ARM64 passed
**448/448 release binaries and 5,095/5,095 cases**, plus **41/41 ASan/UBSan
binaries and 667/667 cases**. Links resolved all 448 entries. First-party
production code fell from 186,033 to 180,460 lines, a reduction of 5,573 lines.
GitHub write authentication blocked publication. The local checkout is clean.
