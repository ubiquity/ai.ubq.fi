# Native gateway cleanup inventory findings

## Work contract v1

Inventory the C++ gateway's live dependencies, checked-in replay inputs, optional reference tools, and owner-controlled operations. Produce a tested keep list and an executable removal plan. This job owns this report and `cpp/gateway/docs/CLEANUP-PLAN.md`. All source, tests, workflows, credentials, and production state are outside the edit scope. Provider-dependent checks use local mocks in an empty-home, fresh-temp, credential-free environment.

| Surface                                                                  | Evidence required                                                                                                          |
| ------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------- |
| Build, release shard, runtime, static files, KV, confinement and startup | A temporary checkout copied by an explicit allowlist, the requested release shard, and focused checks outside that shard   |
| Fuzzing and CI workflows                                                 | Entry points, transitive reads and relevant isolated probes                                                                |
| Portable package                                                         | Native source and dependency provenance, required repository inputs, and package/runtime checks                            |
| TypeScript comparison                                                    | Recorder and oracle call sites, historical pin and recovery commands                                                       |
| Repository handoff                                                       | Every tracked root entry and workflow classified, measured before/after counts, ordered cleanup steps and acceptance gates |

Open questions at kickoff include exact fixture boundaries, whether any package probe reads the Rust workspace, and which features require the host namespace and native addons. Stop after the closure and named tests are verified and the plan is sufficiently specific to execute, or record the exact failing command and host constraint if a gate cannot be run.

## Baseline

- The checkout starts on local branch `agent-job`, equal to `origin/agent-jobs/cppgw-integrate-f1-20261118-44343309770-1` at `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` (tree `d4f9c3beb10816358c85e92c1c231421fa35fa48`). `git status --porcelain=v1` was empty. This is the source snapshot for the inventory, measurements and oracle worktree.
- Host observation via `uname -m`, `cat /etc/os-release`, `nproc`, `free -h`, `gcc-14 --version` and `bun --version` returned Ubuntu 24.04.5 ARM64, four CPUs, 15 GiB RAM, GCC 14.2.0 and Bun 1.4.0. The default Clang is 18.1.3.
- Initial discovery used `git ls-tree --name-only HEAD`, `git ls-files .github/workflows .githooks`, `rg --files -g AGENTS.md -g CI.md --hidden` and `sed -n '1,240p' cpp/gateway/docs/CI.md`.

## Isolation and toolchain preparation

Runner scratch is `/var/tmp/cppgw-cleanup-inventory.VWTXiGqi`. The qualified release driver needs its source, ICU and SQLite outside `/tmp`, which the test mount namespace replaces. `sudo -n unshare --net -- true` succeeded. Docker 28.0.4 is available. The host had GCC 14.2 and lacked the Brotli development package and Clang 21. I used the Clang 21 toolchain to match the prior Linux ARM64 qualification. The following setup ran outside the checkout.

```bash
sudo -n apt-get -o Acquire::Retries=2 update -qq
sudo -n apt-get -o Acquire::Retries=2 install -y -qq --no-install-recommends \
  pkg-config libssl-dev zlib1g-dev libbrotli-dev libzstd-dev libicu-dev mold util-linux gpg strace libnode-dev patch
curl -fsSL --retry 3 --max-time 60 https://apt.llvm.org/llvm-snapshot.gpg.key -o llvm.asc
# Checked the single primary fingerprint with gpg --batch --with-colons --show-keys.
# Expected 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421.
sudo -n install -m644 llvm.asc /usr/share/keyrings/cppgw-cleanup-llvm.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/cppgw-cleanup-llvm.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' > llvm.list
sudo -n install -m644 llvm.list /etc/apt/sources.list.d/cppgw-cleanup-llvm.list
sudo -n apt-get -o Acquire::Retries=2 -o Acquire::https::Timeout=30 update -qq
sudo -n apt-get -o Acquire::Retries=2 install -y -qq --no-install-recommends clang-21 libclang-rt-21-dev llvm-21
```

Clang reports 21.1.8. I used `curl -fsSL --retry 3 --max-time 360 <url> -o <archive>` and `sha256sum --check` to prepare these three upstream archives. All three match the pins in `cpp/gateway/packaging/portable/dependencies.sh`.

| Inputs                                                                                     | SHA-256                                                            | Extract, configure, build and install wall time |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------ | ----------------------------------------------: |
| `https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2`                     | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |                                         95.24 s |
| `https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz`                               | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |                                         67.83 s |
| `https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz` | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |                                        155.99 s |

```bash
scratch=/var/tmp/cppgw-cleanup-inventory.VWTXiGqi
# From the extracted Boost source
./bootstrap.sh --with-libraries=json,container --with-toolset=gcc
./b2 --ignore-site-config --user-config=/dev/null -j2 toolset=gcc \
  --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 cxxflags=-march=armv8-a stage
# From the extracted SQLite source
CC=gcc-14 CFLAGS='-O2 -DSQLITE_DQS=3' ./configure --prefix="$scratch/sqlite" --disable-static --disable-readline
make -j1 && make install
# From icu/source
CC=gcc-14 CXX=g++-14 CFLAGS='-O2 -march=armv8-a' CXXFLAGS='-O2 -march=armv8-a' \
  ./configure --prefix="$scratch/icu78" --enable-shared --disable-static --disable-tests --disable-samples --disable-extras
make -j2 && make install
```

## First release and native addon probes

`copy-closure.sh` generated the allowlist below from the pinned baseline with `git ls-tree -r --name-only`, sorted it with `LC_ALL=C sort -u`, archived it with `git archive "$baseline" -- "${paths[@]}" | tar -x -C "$scratch/closure"`, and compared the resulting `git ls-files` against the allowlist. The synthetic scratch commit is `4272fd984e847f9a66bea5258a4a5c0ec0e0266c` (tree `0ce2eb0808b0223cbed9439a0492f43f311d4773`). The 2,349-line `closure.paths` SHA-256 is `833e10aa896e91d418708b22ee8d5dbb1093cd05d54965c1234c21e48c846573`.

```text
cpp/                                           # all tracked files at the source snapshot
.github/workflows/cpp-gateway-ci.yml
.github/workflows/cpp-gateway-release.yml
README.md
favicon.png
favicon-32.png
static/                                        # all 38 tracked files
src/**/*.ts                                    # all 316 tracked .ts files below src/
scripts/openai-spec-pin.json
tests/fixtures/sentinel-historical-framing.json
tools/native-kv/inputs.json
```

The release shard started with `timeout -k 15s 2400s bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4`. The wrapper enters `closure` with `env -i`, fresh `HOME` and `TMPDIR`, `PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin`, `LANG=C.UTF-8`, `LC_ALL=C.UTF-8`, `TZ=UTC`, `CC=clang-21`, `CXX=clang++-21`, `UOS_CI_JOBS=4`, `UOS_CI_BUILD_DIR=$scratch/ci`, `UOS_CI_SQLITE_PREFIX=$scratch/sqlite`, `PKG_CONFIG_PATH=$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig`, `UOS_CI_CXXFLAGS=-isystem $scratch/boost_1_92_0` and `UOS_CI_LDFLAGS=-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib`. It reached Ninja compilation. The 40-minute bound allows one cold object build and link/test phase. `shard-1.log` and the driver's incremental results expose progress. A failed test will be reduced to its specific missing input while reusing the C++ object cache. Final counts are pending.

The addon probe stages only `scripts/build-native-kv.ts`, `src/runtime/permissions-native.c`, and `tools/native-kv/{inputs.json,Cargo.lock,lock-derivation.json,LICENSE,atomic-write-rollback.patch,napi-workspace.patch}` from the baseline. It runs the portable builder's Bun import and C compile with a clean environment. Rust 1.98.1 was downloaded from the exact URL and verified with the archive SHA-256 in `native.sh`, `0b514a8cc1cbcd939bff0f151661fe58b6ea5c7a7f645a5098c69e32e8c1e0a2`. `native-minimum.sh` records the commands and writes only to scratch. The Cargo recipe builds pinned `deno-kv-napi` from the `denoland/denokv` Git archive and its crates.io lock. The confinement addon is N-API C compiled with `cc` and system `libnode-dev` headers. The 15-minute bound covers the locked registry download and single Cargo build worker. Final artifact results are pending.

## Initial dependency observations

- `openapi_runtime_contract_test.cpp` recursively opens every root `src/` `.ts` and scans it for `/uos/` literal routes, in addition to checking actual compiled native registration tables. Running it against a directory reduced to the V8 helper files could silently lose literal coverage. `admin_models_catalog_test.cpp` reads `src/models/catalog.ts`, `src/admin/codex.ts` and `src/handler/index.ts`.
- `static_assets_test.cpp` opens `README.md` and pins operator wording. It also requires the published auth uploader references in `static/docs/llms-agents.md`. `openai_spec_drift_test.cpp` opens `scripts/openai-spec-pin.json`. `sentinel_upstream_providers_test.cpp` opens `tests/fixtures/sentinel-historical-framing.json`. The startup test uses the Linux KV input manifest and selects `inputs-darwin-arm64.json` when compiled for Darwin.
- `runtime/paths.hpp` selects the manifest-verified immutable package resource root, or the current directory of a module test. `src/static.cpp` uses `src/static.ts` as a synthetic URL base and serves the 38 `static/` files and two root favicons. The Linux host and portable packagers and the macOS packager currently copy `static/` alone. Missing packaged favicons are a pre-existing defect to reproduce separately.
- `cpp/gateway/packaging/portable/build.sh` archives committed source paths, then the Dockerfile copies only the two V8 codec files from root `src/kv-conversion/`. `v8-helper.mjs` imports `typed-value.ts` and is an offline Deno helper. Package manifests require both bytes even for normal serving, and conversion invokes them. The package builders also verify the real KV and permissions artifact files and manifests.
- The paired conformance script currently diffs the whole candidate tree against the oracle revision with a narrow conformance-only exception, then builds both sides from the same archived source. A detached pinned TypeScript checkout needs a separate candidate build/package input. Existing comparison assertions and restart phases can be reused.
