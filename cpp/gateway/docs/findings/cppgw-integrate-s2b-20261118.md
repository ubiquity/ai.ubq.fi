# C++ gateway library integration, stage 2b

## Work contract v1

Start at the verified stage 1 tip `e90858809cf9cd733912be44077e97f949681c18`.
Merge the eight accepted branch tips from `origin` as two-parent commits in the
requested order. Reconcile overlapping code, vendor registrations and the
specified fixtures under `cpp/gateway/`. Preserve upstream vendor bytes.
The checkout handoff is the stage 2b branch for the maintainer's final integration.

| Surface                  | Required behavior                                                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| Slimming                 | Retain the supported console, environment, filesystem, Headers, multipart, ordered container and gzip contracts.                     |
| Fetch                    | Keep framing, limits, redirect and delivery behavior for supported HTTP requests. Reuse the stage 1 llhttp sources.                  |
| URL                      | Use Ada 4.0.0 for URL, IDNA and form operations. Retain origin, redirect and request validation at the gateway boundary.             |
| Passkeys                 | Require canonical CBOR and retain authentication and attestation checks with OpenSSL's typed decoders.                               |
| BigInt and stored values | Keep supported numeric and stored representations, including sparse arrays, shared references and cycles. Verify input depth bounds. |
| Conformance              | Compare the native and Bun gateways with isolated local mocks. Preserve wire behavior, statuses, accounting and validation.          |

The owner policy permits standard rejection of malformed inputs and library
diagnostic wording. Preserve validation coverage and all valid vectors. Keep
TypeScript, hosted workflows, other `cpp/` paths and production deployment
outside the integration. Current observations are the fetched accepted tips,
their recorded fixtures and the known-good stage 1 receipts. Textual conflicts,
compiler diagnostics and the merged suite results are open at discovery.

Prepare ARM64 dependencies outside the checkout. Verify changed build
registries and vendor bytes, then run the four
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4` selections,
the affected `--config asan` tests and the documented Bun 1.4.0 conformance
suite. Use empty homes, fresh temporary directories and local mock traffic.
The stop condition is a clean handoff with eight ordered merges, exact receipts,
the first-party line-count comparison and any observed limitations.

## Initial evidence

Ubuntu 24.04.5, AArch64, four CPUs. The starting checkout matched the specified
stage 1 tip and all eight requested refs were fetched. Bun 1.4.0 is present.
The runner prepared and verified the pinned dependency artifacts before execution.

## Work contract v2

The depth audit found `kJsonMaxDepth` set to 10,000 in the stage 1 tree.
Apply the requested 128-container limit to JSON parsing and recursive helpers.
Retain the stored-value codec's independent 2,000-container limit and enforce
it when a stored graph is serialized or traversed recursively from another
node. Graph references and cycles remain valid. The replay's measured differences
are the evidence for its fixture changes. This increment requires fresh release,
sanitizer and conformance receipts after the depth changes.

## Work contract v3

The release sweep found that the inherited Codex 429 replay expected body-based quota classification beyond the
required JSON depth. The capped parser rejects those five bodies as quota evidence while forwarding the complete
upstream response and independently evaluating Retry-After. The fixture update requires new release, sanitizer
and packaged conformance evidence on the final tree.

## Ordered merges

Each listed merge used `git merge --no-ff --no-commit origin/<branch>` followed by a two-parent commit. The second
parents are the fetched and reviewed tips. The first-parent order retains stage 1 through all eight merges.

| Order | Accepted branch                                          | Second parent                              | Merge                                      |
| ----: | -------------------------------------------------------- | ------------------------------------------ | ------------------------------------------ |
|     1 | `agent-jobs/cppgw-integrate-s1b-20261118-44270540815-1`  | `c8c483307429688f79288ac42754d9624edeacb6` | `906b65b685a376687bf85a7a69145cfd0af34a50` |
|     2 | `agent-jobs/cppgw-std-llhttp-client-fu-c1-44247790145-1` | `2485d89d326426ec6d5aff95475bf6073dc295ba` | `e4038afe097c4ba8a838b46864d6758f0acc670b` |
|     3 | `agent-jobs/cppgw-std-ada-c1-44247790145-1`              | `7bde4d44a00ed5322901a59e98f80a5f60bafa50` | `81f338c29e21401467167fea7af3947e5fc88063` |
|     4 | `agent-jobs/cppgw-std-qcbor-c1-44247790145-1`            | `196f96f2f9befd8ac7521713c6799fee6f3a85e8` | `581d6f222ffba34a402e445158375029e46c2179` |
|     5 | `agent-jobs/cppgw-std-asn1-c1-44247790145-1`             | `fe1b07c2e6b95a31e09ed36b760f6573eb028e6a` | `4031172c7e415ed7011c0e3d22b9d14494429ae8` |
|     6 | `agent-jobs/cppgw-std-bigint-c1-44247790145-1`           | `9b4cc9aa25ddaa8a229fdc480d949931fbcaceca` | `b876935cf76542fe150641318135ba999bf2546d` |
|     7 | `agent-jobs/cppgw-kv-jsc-graph-model-c1-44247790145-1`   | `1c2680ca73e8e8a959852e50e53d21558898b1ff` | `df3a9776c0ad08b9b64a463c3dd91fc92dacf879` |
|     8 | `agent-jobs/cppgw-conformance-e2e-c1-44247790145-1`      | `e2fe9cd97eb3659fbe1522053612dad0231330ae` | `634a93bd698162d2da31cf2f516bcdb0029b853e` |

| Order | Conflict and resolution                                                                                                                                                                                                                                                                                                                                                                                                           |
| ----: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
|     1 | `docs/DEVIATIONS-FROM-BUN.md` retains stage 1's ICU and request parsing sections and appends all stage 1b sections. The automatic HTTP fixture merge keeps stage 1b's wire-byte upload fixtures and stage 1's redirect diagnostics.                                                                                                                                                                                               |
|     2 | In `CMakeLists.txt` and `tools/vendor_sources.py`, retain stage 1's C11 and multi-source archive registration. Git's automatic `cmake/vendors.json` merge produced two llhttp keys. Combine the server, client, WebSocket and test/fuzz consumers into one llhttp entry. Append the fetch and WebSocket sections to the deviations ledger. Keep both the typed-byte fixture conversion and the accepted fetch framing assertions. |
|     3 | Merge Ada's source and digest into `tools/vendor_sources.py` and its consumer into `cmake/vendors.json`. Keep both the ICU and Ada targets in `fuzz/targets.json` and append the URL deviations.                                                                                                                                                                                                                                  |
|     4 | Merge the QCBOR source, header and digest declarations into the shared registries. In `CMakeLists.txt`, preserve C11 and use the bounded C compiler already supplied by the toolchain. The direct Clang tool-view fallback uses its matching C frontend. Append the CBOR deviations and pass the registered QCBOR include directory to the non-CMake builders.                                                                    |
|     5 | In `src/auth/passkeys.cpp`, combine the OpenSSL and QCBOR includes and keep the private ASN.1 namespace after the unchanged QCBOR codec. Use QCBOR's value access for hash selection, OpenSSL's DER scalar checks and digest verification, and OpenSSL's EC/RSA extraction with integer CBOR labels. Keep both passkey fuzz targets and both deviation sections.                                                                  |
|     6 | Append the BigInt input deviations. The test Links and fuzz additions merged automatically.                                                                                                                                                                                                                                                                                                                                       |
|   7–8 | Git reported no textual conflicts. The stored-value and conformance files retain the accepted branch content.                                                                                                                                                                                                                                                                                                                     |

A byte comparison matched the full QCBOR decode, encode and re-encode block against its incoming parent. The OpenSSL
DER preflight, ECDSA decoding, certificate and CRL policy stay on the OpenSSL path. The only interoperation changes in
certificate-to-COSE projection are the CBOR label and value types. The replay retains the original validation, store,
fetch and response assertions, subject to the explicit fixture changes below.

## Fixture decisions

- `22ccf2e0` updates the three incomplete BigInt vectors in `tests/rt/number_test.cpp`. A lone minus or plus sign and
  `"0x "` now require `SyntaxError`. The parser requires digits after a sign or radix prefix. All other input and
  arithmetic assertions stay in the same fixture.
- The first merged WebAuthn replay compiled 369 production sources and compared 155 rows. It found 25 differences.
  Sixteen involved ECDSA warnings. The response, headers, throws, store and fetch snapshots matched on those rows.
  Four malformed CBOR rows kept their rejection and needed the stricter diagnostic. Two unordered TPM statements
  and two malformed COSE keys changed from success to rejection. One correctly encoded UTF-8 extension changed from
  rejection to success after the codec stopped re-encoding the UTF-8 length as a UTF-16 length.
- `454458c1` keeps all 155 original inputs and records those observations. Fourteen ECDSA warnings now say
  `Invalid DER ECDSA signature`. Context-tagged and indefinite signatures add that warning at the earlier rejection
  point. All are still HTTP 400 with the same state and response. The original five unordered TPM inputs are still
  exercised as rejections. Five sorted companions preserve their signed bytes and original downstream TPM decisions.
  Two canonical COSE-text companions cover registration and stored-key authentication. The revised release replay
  passed all 162 vectors in its one harness case. The remaining CBOR and OpenSSL unit binaries keep their original
  acceptance and rejection cases.
- The fetch branch already changed `tests/rt/fetch_transport_test.cpp` to retain the malformed wire inputs while
  requiring a `FetchError` at the head or body boundary. TRACE consumes its valid framed response. Stage 1b's
  body fixtures still send the same bytes. The three application fixture differences recorded in the stage 1b
  report already had corrections in the stage 1 parent. [Stage 1b findings](cppgw-integrate-s1b-20261118.md)

- `f58d69d3` updates the 40 Codex 429 replay expectations affected by the JSON cap. Five synthetic bodies with
  5,000 to 9,999 nested arrays or objects still run under all eight original header sets. Independent computation
  of the expected hashes checks the raw bytes, length, HTTP 429 status, status text, headers and header-only
  deadline. It requires `usageLimitReached=false` and no body-derived quota proof. All other recorded cases,
  including the truncated body and already-rejected deeper bodies, retain their original expectations.

## Depth and stored graphs

The actual stage 1 JSON limit was 10,000 containers. Commit `315679f3` sets it to 128 in the shared header.
The simdjson and Boost fast conversions are separately limited to 32, and the iterative fallback checks the limit
before opening another container. The recursive comparison, coercion and copy fixtures exercise the current limit
and rejection at the next level. The existing `-O0` ASan failure in `matchesAt` was at the old 10,000-level fixture.

The JSC reader uses explicit frames and checks the sum of the open frames and the known height of a referenced
closed subtree. The shared JSC and typed-value limit remains 2,000. The writer now checks its depth independently
when serializing from a new root. Stored array/object projection checks the 128-container JSON bound. Value
comparison and diagnostics also bound their descent. The JSC fixture checks a 2,000-container round trip, the
rejection after adding a wrapper, and JSON projection at 128 and 129 containers. The typed-value fixture retains
the separately constructed 2,000-level inline array and its recursive copy. That diagnostic at `-O0` arose in
the programmatic fixture rather than in the frame-based reader.

## Production source lines

Physical newline counts from Git blobs, including comments, blanks and private headers. The command uses
`git ls-tree -r --name-only <revision> -- cpp/gateway/<path>` and
`git show <revision>:<file> | wc -l` for each tracked file.

| First-party path        |    Stage 1 |   Stage 2b |            Change |
| ----------------------- | ---------: | ---------: | ----------------: |
| `src/rt` (52 files)     |     29,246 |     27,171 |            −2,075 |
| `src/runtime` (8 files) |      3,004 |      3,004 |                 0 |
| `src/auth/passkeys.cpp` |      5,072 |      4,197 |              −875 |
| **Total**               | **37,322** | **34,372** | **−2,950 (7.9%)** |

## Build registration and runner

Ubuntu 24.04.5 on AArch64 with four CPUs. Clang 21.1.8, Ninja 1.13.2, mold 2.30.0, Boost 1.92.0, OpenSSL
3.0.13, ICU 78.3, SQLite 3.54.0 with `SQLITE_DQS=3`, zlib 1.3, Brotli 1.1.0, Zstd 1.5.5 and Bun 1.4.0.
The dependency scratch root is `/var/tmp/cppgw-integrate-s2b.PgYYDwMV`.

Clang came from the signed apt.llvm.org Noble 21 archive. Its key fingerprint was
`6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The install was
`sudo apt-get install -y clang-21 libclang-rt-21-dev llvm-21 libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev pkg-config mold cmake`.
SQLite and ICU were staged under `$scratch/sqlite` and `$scratch/icu78`. Clang linked the portable tests with RPATH
to those paths, which stay visible when the fixture namespace mounts a private tmpfs at `/tmp`.

| Verified archive                 | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| `boost_1_92_0.tar.bz2`           | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

```bash
# Boost 1.92.0 source, using the runner's bootstrap GCC 13.3
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage

# SQLite 3.54.0 source
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install

# ICU 78.3 out-of-tree build
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install
```

The 448-test Links registry resolved without skips. CMake configured the full test registry and built all five
vendor archives. Ada, simdjson and simdutf contribute one C++ object each. llhttp contributes `llhttp.c`, `api.c`
and `http.c`. QCBOR contributes `qcbor_decode.c`, `qcbor_encode.c`, `qcbor_err_to_str.c`, `UsefulBuf.c` and
`ieee754.c`. Both archives use the C11 frontend. The SDK unit make rules produced the same three new archive
member lists using a runner compiler shim. The request-path builder also compiled all eight vendored C inputs
before the redundant build was stopped in favor of the exact portable release objects. The portable fuzz build
compiled those same inputs with Clang 21 and sanitizer flags following the versioned C frontend fix in `27fe47c0`.

The first shard attempt on `27fe47c0` passed 111 binaries and 1,309 cases. Its single build failure showed that
the ASN.1 test includes `passkeys.cpp` directly and needs QCBOR even though its Links list omits the separate
implementation object. Commit `26c4ec79` registers the test as a direct QCBOR consumer. CMake compiled that object
and the portable shard rerun passed its nine cases.

```bash
python3 -B cpp/gateway/tools/vendor_sources.py cmake
python3 -B cpp/gateway/tools/links.py --check
UOS_GATEWAY_BUILD_EVIDENCE=stage-2b-runner UOS_GATEWAY_TOOL_ROOT="$scratch/native-cmake" \
  cmake -S cpp/gateway -B "$scratch/cmake" -G Ninja -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 -DCMAKE_BUILD_TYPE=Release \
    -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_TESTS=ON
UOS_GATEWAY_BUILD_EVIDENCE=stage-2b-runner UOS_GATEWAY_TOOL_ROOT="$scratch/native-cmake" \
  cmake --build "$scratch/cmake" --parallel 1 --target \
    uos_vendor_ada uos_vendor_llhttp uos_vendor_qcbor uos_vendor_simdjson uos_vendor_simdutf
CC=clang-21 CXX=clang++-21 python3 -B cpp/gateway/cmake/ci/test_driver.py
for vendor in ada llhttp qcbor simdjson simdutf; do
  (cd "cpp/gateway/third_party/$vendor" && sha256sum --check SHA256SUMS)
done
git diff --check e9085880..HEAD -- . ':(exclude)cpp/gateway/third_party'
```

The seven driver controls passed. The vendor manifests all passed, and `git diff --exit-code` matched Ada and
QCBOR against their respective incoming parents and llhttp against both stage 1 and its client parent. The
vendor files remain byte-identical, including QCBOR's upstream whitespace.

## ARM64 release and sanitizer commands

The release commands run through the stock portable driver. This is the wrapper environment for each shard.

```bash
scratch=/var/tmp/cppgw-integrate-s2b.PgYYDwMV
for shard in 1 2 3 4; do
  session=$(mktemp -d "$scratch/gate.XXXXXXXX")
  mkdir "$session"/{home,tmp}
  env -i PATH=/usr/local/bin:/usr/bin:/bin:/home/runner/.bun/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
    HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
    UOS_CI_BUILD_DIR="$scratch/ci" PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
    UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0" \
    UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
    bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
```

`bash "$scratch/asan-run.sh"` uses the same environment and libraries, adds
`-gline-tables-only -fno-sanitize-recover=all` to C and C++, and runs
`nice -n 4 python3 -B "$scratch/asan-selection.py"`. The selector feeds the driver its own `--config asan`
argument and the sorted set of 135 closures. The selection covers runtime tests, all passkey Links consumers,
standalone library tests, JSC and typed-value conversion, stored values, HTTP and WebSocket, the changed image and
live-request consumers, Codex 429 proof, console and environment-dependent startup, catalogs, and Sentinel replay/gzip.
The full selection is `$scratch/asan-selection.txt`. Both gates use the driver's namespace probe and give each
test loopback only, a fresh tmpfs, an empty home and a fresh temporary directory. The driver makes address,
undefined-behavior and leak diagnostics fatal.

The initial focused release replay populated the shared object cache. Intermediate compile attempts were interrupted
to bind their inputs to updated registry commits. Ninja reused completed objects. The final release and sanitizer
runs capture the selected source commit, tree, before/after input digests, compiler and archive hashes, binary
outcomes and phase durations in `$scratch/receipts`. These are incremental wall times.

### Final release receipts

Tested commit `f58d69d3ee3ef35eea975d6ecc4272bab5954246`, Git tree
`19e97aef4bd8ddb86d79c42f816d94e32397d5b9` and source input SHA-256
`887edea212bcb364fa0430b2e6c7fa0393df432485424e1bf796953a6fc41284`.
The four round-robin selections are disjoint and cover the full inventory. All 369 production objects were
available in every run. All shards exited 0 without test failures, build failures or timeouts.

| Shard     | Passing binaries |   Passing cases | Compile s | Link/test s | Total s |
| --------- | ---------------: | --------------: | --------: | ----------: | ------: |
| 1/4       |          112/112 |     1,318/1,318 |     0.008 |      15.794 |  16.141 |
| 2/4       |          112/112 |     1,240/1,240 |     0.014 |      25.586 |  26.085 |
| 3/4       |          112/112 |     1,357/1,357 |     0.014 |      41.365 |  41.821 |
| 4/4       |          112/112 |     1,171/1,171 |     6.036 |      25.109 |  31.649 |
| **Total** |      **448/448** | **5,086/5,086** |           |             |         |

| Receipt          | SHA-256                                                            |
| ---------------- | ------------------------------------------------------------------ |
| `release-1.json` | `8abf0abdf0011d7696f5db6c99fa20e8238c9621f4177b7b28ca10df0c4e1275` |
| `release-2.json` | `ba0822297555837e8d01519f26bb2f46bdd7661ff18e70909145725f41477704` |
| `release-3.json` | `4c73435a1eeb66ea0458df4374231c25abbadeaaf5aeb966aae871a2a9ab81f8` |
| `release-4.json` | `b71894b5dcae0461f5c84694b6045a2755da31c2c34f3c6df783c9bdad936716` |

The earlier shard 4 attempt on `26c4ec79` passed 111 binaries and 1,170 cases, with one failed harness case
containing the 40 over-depth Codex 429 expectation mismatches. The final shard passed all three cases in that
binary. Earlier failure logs and receipts remain in runner scratch.

### Final sanitizer receipt

The final `-O1` ASan/UBSan run passed **135/135 binaries and 1,554/1,554 cases**. It used the same commit, Git tree
and input digest as the final release runs and exited 0. The compile phase took 511.892 s, link and test took
119.773 s, and the total was 632.260 s. All 369 production objects were present. A scan of the 135 run logs found
zero ASan, UBSan or LeakSanitizer diagnostics. The SHA-256 of `receipts/asan.json` is
`c71231976a403599f54f747abb1b5d62b11e4a5ac99c060e68ca98e7714d09bd`.

The JSON test passed 25/25, the JSC reader 8/8, the sparse-array test 2/2 and the typed-value replay 10/10. The
BigInt Number fixture passed 21/21 and the Codex 429 replay passed 3/3. QCBOR passed 13/13 and OpenSSL ASN.1 passed
9/9. The WebAuthn replay passed its one harness case covering all 162 vectors.

## Paired conformance on the packaged release

Bun 1.4.0 was already installed. The scratch snapshot installed frozen dependencies with
`bun --no-env-file install --frozen-lockfile --ignore-scripts`. The pinned Rust 1.98.1 toolchain prepared the
genuine native KV and permissions artifacts through `bun --no-env-file --no-install scripts/build-permissions.ts`
and `bun --no-env-file --no-install scripts/prepare-native-runtime.ts` before launch.

The package reuses the 369 production objects and five vendor archives from the final portable release check.
The link helper verifies the release receipt and archive digests, compiles the startup unit and three entrypoints
with its recorded C++ flags, then links the three executables with mold and the pinned host libraries. The
host SQLite library was relinked from the verified build's `sqlite3.o` with `gcc-14 -shared -Wl,-soname,libsqlite3.so.0`,
then staged with ICU 78.3 under `/usr/lib` according to the conformance guide. The standard packager verified the
native artifacts and produced 65 files. This supplies the same package and isolated runner used by
`conformance/run.sh`, with the portable release objects as the native build. The benchmark compilation was
stopped after its vendor C inputs and the first application objects, and its partial output was excluded.

```bash
work="$scratch/conformance"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$work/prep-home" TMPDIR="$work/prep-tmp" LC_ALL=C.UTF-8 \
  nice -n 3 python3 -B "$scratch/package-release-objects.py"
bash "$scratch/run-paired.sh" --package-only
/usr/bin/time -o "$scratch/conformance-final.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash "$scratch/run-paired.sh"
```

The helper prepares the package through `bench/request_path/package.py` with the frozen KV binding. The final
invocation reuses that package and runs `conformance/run.py` through `bench/request_path/isolated.sh` and
`timeout -k 10s 180s`. It passes the final
commit and tree for the application and the suite. Both gateways use fresh state, empty homes, fresh temp
directories and the same loopback mock. Both outbound probes returned `ENETUNREACH`. The driver ran 162 cases
before restart and 24 after. The nine comparator controls also passed.

**184 of 186 paired cases passed.** Both gateways returned HTTP 200 where the independent assertions required 400
for `chat-invalid-role` and `responses-invalid-tool-type`. These are the two existing shared validation failures.
There were zero differential functional failures and zero cosmetic differences. The paired phase took 2.703 s,
and the outer command took 3.00 s with 139,416 KiB peak process RSS. The suite returned exit 1 for the two shared
assertions.

| Evidence                           | SHA-256                                                            |
| ---------------------------------- | ------------------------------------------------------------------ |
| Native executable                  | `097e2f6fb5b79eecb3b9d9f2620a92bc6e7a66d96073a330f929fb6014a01ba6` |
| Bun executable                     | `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9` |
| Package manifest                   | `68c6925faf912071c13610bce84b0d9dbccd3e6580827aa2ae964d6b7a9a3a17` |
| `run.AQ1abU92/output/results.json` | `24b64c243357be11373789090cdaae1d5bfbec3e9294a67ab6d6a5c54a875a8b` |
| `run.AQ1abU92/output/table.md`     | `2bea47a8a0cf89edca4f79f7da9b007578c6d65da0bc1e3028399fec1918de8e` |

## Open findings and handoff

- The two shared conformance assertions remain open in the gateway schema validation paths, which are outside the
  integration edits.
  [Incoming conformance findings](cppgw-conformance-e2e-20261118.md#functional-findings)
- The stored-value lane noted that JSON views can still materialize a sparse array when projecting to dense JSON.
  The JSC reader and typed-value interchange keep sparse entries. The inherited KV porting notes still describe
  cycle rejection and copied references. [Stored-value findings](cppgw-kv-jsc-graph-model-20261117b.md)
- The disposable fuzz policy retains each incoming harness. The production-host runner consumes prebuilt targets
  and its evidence inventory still omits per-target extra sources. The Darwin build path received syntax checks.
  The build and runtime qualification here is Linux ARM64.
- The scoped `bun --no-env-file --no-install run format:check`, shell and Python syntax checks and first-party
  `git diff --check` pass. The vendored QCBOR `UsefulBuf.h` retains its upstream blank line at EOF.
- The stage is on local branch `agent-jobs/cppgw-integrate-s2b-20261118` for the maintainer's final integration.
  `GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-integrate-s2b-20261118`
  exited 128 because GitHub write credentials were unavailable. The verified local handoff bundle is
  `$scratch/cppgw-integrate-s2b-20261118.bundle`. The refreshed development tip
  `2ac5aae69b0f0bac77cf1c6f329995cf1187616c` has no `cpp/gateway` tree.
- The final report commit adds only this evidence to the verified source tree.

Stage 2b contains all eight ordered merges. Linux ARM64 passed 448/448 release binaries and 5,086/5,086 cases,
plus 135/135 ASan/UBSan binaries and 1,554/1,554 cases. Native and Bun agreed across all 186 conformance cases,
with two shared validation failures. First-party production source fell from 37,322 to 34,372 lines, a 7.9% reduction.
