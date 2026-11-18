# C++ gateway runtime slimming integration, stage 1b

## Work contract v1

Integrate the four accepted slimming branches into one stage 1b branch. Start at Headers/multipart tip
`2e4b3dce72bcb955060ba46547cf0d75b6b1c691` and preserve the three incoming tips as second parents of ordinary merge
commits. The four lanes share base `6393434b110e2d1afe93e6c7005848219336d9b9`.

| Lane                          | Required behavior                                                                                               |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------- |
| Headers and inbound multipart | Keep header validation and wire projection, ordered upload parts, endpoint bounds, and replay and scrub timing. |
| Console                       | Keep captures, severity streams, terminal records, and bounded diagnostic output.                               |
| Environment and filesystem    | Read process variables and explicit configuration inputs. Keep path and file behavior used by gateway callers.  |
| Ordered containers and gzip   | Keep observable ordering and gzip handling for replay, including size and decode checks.                        |

Changes are limited to merges, necessary integration edits under `cpp/gateway/`, and this report. Other agents own
parallel port work, including repairs to the three known application fixtures. Production activation, credential
operations, provider spend, workflows, TypeScript changes, sibling C++ code and vendored libraries are outside scope.
Qualification uses an empty test HOME, fresh TMPDIR, a credential-free environment and the driver's real loopback-only
Linux boundary.

Run the four `bash cpp/gateway/tools/ci-build.sh --config release --shard i/4` gates and focused
`--config asan` tests for the four lanes. Count physical production lines from Git under `src/rt` and `src/runtime`.
Trace native startup, offline CLIs, test setup and the production TypeScript launcher to establish the `.env` contract.
The merge dry runs and first compiler receipts will establish actual conflicts and runner compatibility. Prior runs
took tens of minutes per profile. Watch compile logs and incremental results in runner scratch and retry recoverable
prerequisite failures. The stop condition is a three-merge history, a verified commit and tree, four release receipts
with exactly the three baseline failures, focused sanitizer coverage, and this completed report.

## Merges

Fetched the three named `refs/heads/agent-jobs/cppgw-slim-*-20261118-44254409709-1` refs to their matching
`refs/remotes/origin/` refs. Each was merged using `git merge --no-ff --no-commit <ref>`, followed by a merge commit.

| Incoming branch              | Incoming tip                               | Merge commit                               |
| ---------------------------- | ------------------------------------------ | ------------------------------------------ |
| `cppgw-slim-console`         | `f79a45cddb36310413f2278a67a8e2b09bc67c42` | `7f88ae4c6ec7c458c962a2e2563b3ece2a202692` |
| `cppgw-slim-env-fs`          | `c305bff2f5082e60192e0baf8f989585a15bc988` | `e53d9edfaad658cd84b8e6fbcbc761aa6753857e` |
| `cppgw-slim-containers-gzip` | `27ca975ba8c14bcab2cbe0483d7ed645a1b81158` | `a8d916be8a43e3b75e060a89688dbf4bc4f8ef99` |

In all three merges, `docs/DEVIATIONS-FROM-BUN.md` was the sole conflict. Both versions started with the same base
blob. Each resolution kept the existing text byte-for-byte and appended the incoming branch's complete addition.
Source, test, link-registry and fuzz-registry files merged cleanly.
[Headers/multipart](cppgw-slim-headers-multipart-20261118.md) · [Console](cppgw-slim-console-20261118.md) ·
[Environment/filesystem](cppgw-slim-env-fs-20261118.md) · [Containers/gzip](cppgw-slim-containers-gzip-20261118.md)

Follow-up `87e5289f0102af2c8a2fca3051e0450f5049c8fb` corrected the fuzz registry's descriptive `env_file.entry` to
`uos::rt::env::ScopedEnvironment`, which the merged harness calls. The harness, corpus, bounds and build settings are
those of the accepted environment branch. All four supplied tips are ancestors of the integrated head, and 36 other
changed branch paths match the accepted tips byte-for-byte.

## Configuration and dotenv

**Native startup, offline CLIs and isolated tests run without implicit `.env` loading.** For a native service cutover,
the operator must supply the required variables in the service environment before the executable starts, including the
production timeline on the VPS. The draft unit already names an explicit systemd
`EnvironmentFile=/srv/uos-native/operator.env`. [Native service unit](../../ops/uos-gateway-native.service)

A symbol search at the shared base found `loadEnvFile` and `parseEnvFile` in the runtime implementation and fixture
paths, with no call from startup or an offline CLI. The retirement removes that fixture-only API surface.

| Surface                          | Configuration source                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Native gateway                   | `entrypoints/gateway.cpp` and `runtime/startup.cpp` require and validate an explicit `--runtime-manifest` for package, state and listener settings. Startup sets `SQLITE_TMPDIR` to the private state `.data`. `src/config.cpp` reads deployment markers, timeline, authentication and identity from the first-use inherited OS snapshot in `src/rt/env.cpp`. `src/runtime_config.cpp` reads model/catalog state from KV. Native Codex home and provider settings use the environment or their existing explicit inputs. |
| Native offline CLIs              | `entrypoints/kv-convert.cpp` and `entrypoints/kv-proof.cpp` take the separate offline manifest and explicit application arguments for the data paths and pinned codec identity. `src/kv_conversion/legacy_codec.cpp` gives the helper a scoped `DENO_DIR`, and proof subprocesses get an empty environment. There is no dotenv lookup in these paths.                                                                                                                                                                    |
| Native tests                     | `cmake/ci/driver.py` and `isolate.py` launch each binary with a small allowlist and fresh HOME/TMPDIR. Configuration tests pass explicit subprocess environments, scoped runtime overrides, injected readers or fixture KV. `tests/rt/env_test.cpp` writes synthetic cwd and home `.env` files to verify that the snapshot ignores both. The separate packaged smoke harness also passes a synthetic environment and explicit manifest.                                                                                  |
| TypeScript service               | `serve.ts` consumes the configuration and parsed arguments. `src/config.ts` reads `process.env`, and `src/runtime-config.ts` consumes KV. `ops/launch-vps.sh` and `ops/launch-mac.sh` start Bun with `--no-env-file --no-install`. `ops/launch-service.ts` calls `loadEnvFile(join(root, ".env"))` explicitly before importing `scripts/serve-vps.ts` or starting the Mac child with its environment. **Production Bun depends on the explicit root-file load. Automatic Bun discovery is disabled.**                    |
| TypeScript local tasks and tests | The `dev` and `dev:local` package scripts explicitly source `.env` if present and then disable Bun discovery. The scoped test runner starts Bun with `--no-env-file --no-install`. The retained legacy VPS rollback command names `.env` explicitly via Deno `--env-file=.env`.                                                                                                                                                                                                                                          |

### Audit commands

```sh
rg -n 'env' cpp/gateway/runtime cpp/gateway/entrypoints cpp/gateway/src/config* cpp/gateway/src/rt/env.cpp
rg -n 'env|manifest|loadConfig' serve.ts src/config.ts cpp/gateway/src/runtime_config.cpp
git grep -n -E 'dotenv|loadEnvFile|parseEnvFile|--env-file|"\.env"' -- \
  cpp/gateway/runtime cpp/gateway/entrypoints cpp/gateway/src cpp/gateway/tests cpp/gateway/ops
rg -n 'EnvironmentFile|--no-env-file|loadEnvFile' cpp/gateway/ops ops/launch-{vps,mac}.sh \
  ops/launch-service.ts scripts/test-runner.ts package.json
```

The platform reference still claims the C++ entrypoint calls `rt::env::loadEnvFile` and describes an older console
factory. Those statements predate the merged changes. [Platform reference](../rt/platform.md)

## Production line counts

Counted newline bytes (`wc -l` semantics) in each `git ls-tree -r` blob under `cpp/gateway/src/rt` and
`cpp/gateway/src/runtime`, using `git cat-file blob <object>`. This includes tracked implementation and private headers
in those directories. Public headers under `include/`, tests, and vendored code are outside the requested measure.

| Revision                                                                 | `src/rt` (52 files) | `src/runtime` (8 files) |  Total |
| ------------------------------------------------------------------------ | ------------------: | ----------------------: | -----: |
| Shared branch point `6393434b`                                           |              30,635 |                   2,966 | 33,601 |
| Starting tip's parent `92494f96` and report-only starting tip `2e4b3dce` |              30,342 |                   2,966 | 33,308 |
| Integrated code `87e5289f`                                               |              30,128 |                   2,966 | 33,094 |

The integrated tree removed **507 lines** from the shared base and **214 lines** from the starting tip's parent.

## Runner and commands

Ubuntu 24.04 ARM64, 4 CPUs and 15 GiB RAM. Release used GCC 14.2.0. ASan/UBSan used Clang 21.1.8, with leak
detection and fatal sanitizer errors. Ninja 1.13.2 and mold 2.30.0 drove compilation and linkage. The linked C
libraries were OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, Zstd 1.5.5, SQLite 3.54.0 and ICU 78.3. An isolated loader
probe called `sqlite3_libversion()`, `sqlite3_compileoption_used("DQS=3")` and `u_getVersion()`. It confirmed both
pinned shared libraries and the SQLite double-quoted-string build option inside the same namespace boundary.

Downloaded and SHA-256 verified the official archives outside the checkout.

| Archive                          | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| `boost-1.92.0-b2-nodocs.tar.xz`  | `ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

The build commands run in their respective scratch directories.

```sh
scratch=/tmp/cppgw-integrate-s1b.AQcH1W2c
libs=/var/tmp/cppgw-integrate-s1b-libs.eDG4i0Kz
sudo -n apt-get install -y g++-14 libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux mold
sudo -n apt-get -o Dir::Etc::sourcelist="$scratch/llvm21.list" -o Dir::Etc::sourceparts=- \
  -o Acquire::Retries=2 -o Acquire::https::Timeout=30 update
sudo -n apt-get -o Dir::Etc::sourcelist="$scratch/llvm21.list" -o Dir::Etc::sourceparts=- \
  install -y clang-21 libclang-rt-21-dev

# Boost source
./bootstrap.sh --with-toolset=gcc --with-libraries=json,container
printf '%s\n' 'using gcc : 14 : /usr/bin/g++-14 ;' > "$scratch/boost-gcc14.jam"
./b2 --user-config="$scratch/boost-gcc14.jam" -j4 toolset=gcc-14 variant=release link=static threading=multi \
  --with-json --with-container stage

# SQLite source
./configure --prefix="$scratch/sqlite/install" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install

# ICU out-of-tree build
../icu/source/configure --prefix="$scratch/icu-build/install" --disable-samples --disable-tests --disable-extras
make -j2
make install

# Stage the shared objects where the test namespaces can resolve them.
mkdir "$libs/sqlite" "$libs/icu"
cp -a "$scratch/sqlite/install/lib"/libsqlite3.so* "$libs/sqlite/"
cp -a "$scratch/icu-build/install/lib/"libicu{uc,i18n,data}.so* "$libs/icu/"
```

The LLVM source used `https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main` and the signed LLVM key with
fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The RPATH below keeps the pinned libraries visible after
the driver replaces `/tmp`. The Clang system-header option applies the driver's warning policy to first-party code
while allowing the vendored simdutf include spelled with its tree path.

The scratch wrapper `gate` uses the following gate command and settings. Each invocation creates its own empty HOME
and TMPDIR. Each binary then runs as uid 1001 inside private network, mount and PID namespaces with a fresh tmpfs.
The namespace helper checks working loopback and denial of an outside route before executing a test.

```sh
# release: compiler=g++-14, jobs=4, compile_flags="-isystem $scratch/boost"
# asan: compiler=clang++-21, jobs=2, add the following to compile_flags:
# --system-header-prefix=third_party/ -gline-tables-only -fno-sanitize-recover=all
env -i PATH=/usr/local/bin:/usr/bin:/bin:/home/runner/.bun/bin LC_ALL=C LANG=C TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CXX="$compiler" UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/build" \
  PKG_CONFIG_PATH="$scratch/icu-build/install/lib/pkgconfig:$scratch/sqlite/install/lib/pkgconfig" \
  UOS_CI_CXXFLAGS="$compile_flags" \
  UOS_CI_LDFLAGS="-L$scratch/boost/stage/lib -Wl,--disable-new-dtags,-rpath,$libs/icu:$libs/sqlite" \
  bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"
```

The release shards run serially in a shared release object tree. The sanitizer profile has its own object tree and
runs the selected targets individually. The `capture` wrapper records the exit status, console log and driver receipt
for each invocation in `$scratch/evidence` before continuing to the next one.

```bash
for shard in 1 2 3 4; do
  bash "$scratch/capture" "release-$shard" release --shard "$shard/4" || :
done

tests=(
  tests/rt/{console,env,fs,process,text_codec,ordered,gzip,headers,http,fetch_transport,sqlite,url_idna}_test.cpp
  tests/{runtime_native_startup,config_differential,runtime_config_differential,native_addon_differential,permissions_linux}_test.cpp
  tests/{jev_compaction_smoke,serve_delivery_http,images,provider_images_branch_coverage,live_calls_http,runtime_http_sse_headers}_test.cpp
  tests/{catalog_store_differential,codex_catalog_reasoning_order,openrouter_models}_test.cpp
  tests/{sentinel_replay_envelope_coverage,sentinel_replay_differential,sentinel_replay_limits}_test.cpp
)
for test in "${tests[@]}"; do
  label=${test#tests/}
  label=${label//\//-}
  bash "$scratch/capture" "asan-${label%.cpp}" asan "$test" || :
done
```

## Qualification

The testable commit is `87e5289f0102af2c8a2fca3051e0450f5049c8fb`, tree
`672300b7c2149a096df03446b115412171fcbbbe`. `python3 -B cpp/gateway/tools/links.py --check` resolved all 441
binaries. The release driver's round-robin `--list --shard i/4` partitions them into 111, 110, 110 and 110 without
overlap. The 29 sanitizer filters each select one existing target. The source input SHA-256 is
`315e72713814381653e4f7e5c248d9649a6272bf51e767f3163df71460eec8c4`.

### ARM64 release

| Release shard | Binaries passed |    Cases passed | Object seconds | Link/test seconds | Elapsed seconds | Exit |
| ------------- | --------------: | --------------: | -------------: | ----------------: | --------------: | ---: |
| 1/4           |         111/111 |     1,292/1,292 |      1,309.623 |            22.382 |       1,332.343 |    0 |
| 2/4           |         109/110 |     1,028/1,029 |        327.982 |            13.565 |         341.886 |    1 |
| 3/4           |         110/110 |     1,375/1,375 |        347.958 |            23.584 |         371.871 |    0 |
| 4/4           |         108/110 |     1,337/1,339 |        355.369 |            38.992 |         394.690 |    1 |
| **Total**     |     **438/441** | **5,032/5,035** |  **2,340.932** |        **98.523** |   **2,440.790** |      |

Shard 1 compiled 369/369 production sources. Subsequent shards reused those objects. Every binary emitted exactly one
fixture-boundary receipt with working loopback and outside-route denial. Build failures and timeouts were both zero.
The archive and test inputs matched across the four receipts. The environment fixture passed 7/7, SQLite passed
29/29, and ICU IDNA passed 2/2.

| Shard | Known failing fixture                                   | Observed difference                                                                                                                                                                                  |
| ----- | ------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2/4   | `tests/codex_external_auth_differential_test.cpp` (3/4) | One case rejects the legacy `last_refresh` forms `"Nov 16 2026"`, `" 2026-11-16 "` and `"1"`.                                                                                                        |
| 4/4   | `tests/claude_code_usage_schema_test.cpp` (2/3)         | A legacy `resets_at` value of `"0"` leaves the normalized deadline null. The Bun fixture expects `946684800000`.                                                                                     |
| 4/4   | `tests/rt/http_test.cpp` (5/6)                          | One generated case has two redirect mismatches. Both invalid values throw `TypeError` with normalized `location` and omit the rejected value. The fixture expects `Location` and the rejected value. |

These three cases account for every failure. Their repair belongs to the separate test-suite integration.

| Release shard | Receipt SHA-256                                                    |
| ------------- | ------------------------------------------------------------------ |
| 1/4           | `9080364205c18b4e0a00179dfaab3f375f2b62adb6bf664a570983da543137d8` |
| 2/4           | `33bc8d362b4cafe9d7d59f79f963a981bafbbd10836f6fa176f9ac88a969e2ca` |
| 3/4           | `3c72e1a0bce286cc713d76b65af63bc1570d7dfc0a64046775ecdba462378899` |
| 4/4           | `ed68e68bb4e6b0e6289e41bbe4ff1ef58fb80df490f2b1161d0e8f5422bb0ea8` |

### Focused ASan/UBSan

| Selection                              | Binaries passed | Cases passed | Link/test seconds |
| -------------------------------------- | --------------: | -----------: | ----------------: |
| Runtime fixtures                       |           11/12 |      144/145 |            54.949 |
| Startup, configuration and confinement |             5/5 |        38/38 |             2.688 |
| Console, HTTP delivery and uploads     |             6/6 |        70/70 |             9.615 |
| Catalog and replay                     |             6/6 |        49/49 |             7.011 |
| **Total**                              |       **28/29** |  **301/302** |        **74.263** |

The cold object phase compiled 369/369 production sources in 2,469.882 seconds. The 29 driver invocations took
2,887.624 seconds in total and each recorded the same commit, tree and source digest as release. The only nonzero exit
was the known HTTP case, with the same two redirect diagnostics. Every binary emitted one fixture-boundary receipt.
The run logs contained zero ASan, UBSan or LeakSanitizer findings. Build failures and timeouts were both zero.

The receipt hash list at `$scratch/evidence/asan-receipts.sha256` has SHA-256
`5db7c86ba5e3b96ef622c1ecd78a43580cfdfb5f26b8f95b72c8c0f3f0cb9e70`. The release and sanitizer objects, receipts and
test logs reside in runner scratch.

## Checkout and handoff

`git diff --check 6393434b` passed. Prettier 3.9.6 via Bun 1.4.0 with `--no-env-file --no-install` passed the five
findings reports, the corrected fuzz registry and the merged deviation appendices. The original deviation text has
pre-existing formatting differences, and a comparison of the original and merged formatted text verified that the
additions introduce none. The final commit adds the report results. Its test inputs match the qualified code commit.

`GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-integrate-s1b-20261118`
exited 128 because the runner lacks GitHub write authentication. `git fetch --no-tags origin
+refs/heads/development:refs/remotes/origin/development` returned `2ac5aae69b0f0bac77cf1c6f329995cf1187616c`.
The stage tip is outside that branch's history. Publication and the repository's `development` handoff remain pending.

## Summary

- Integrated all four branches in three merge commits and updated the stale dotenv fuzz entry.
- ARM64 release passed **438/441 binaries and 5,032/5,035 cases**, with exactly the three known failures. ASan/UBSan passed **28/29 binaries and 301/302 cases**, with the known HTTP failure and zero sanitizer findings.
- Native launch uses the service environment. Production Bun explicitly loads the repository-root `.env` with automatic discovery disabled.
- Runtime production lines fell from **33,601 to 33,094**, down 507 from the common base and 214 from the starting tip's parent.
- GitHub write authentication blocked publication and the `development` handoff. The integrated commits are on the clean local `agent-job` branch.
