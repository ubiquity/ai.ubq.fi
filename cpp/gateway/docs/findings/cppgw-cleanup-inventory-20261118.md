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

## Native addon audit

`native-minimum.sh` exited 0 after 228.19 seconds and reported peak process RSS of 547,872 KiB. The eight archived files were sufficient. Both outputs are AArch64 ELF shared objects. No `rust/`, installed npm package, root Bun manifest, root Bun lock or root TS application source was present. The exact commands after staging the pinned Rust host components were:

```bash
work="$scratch/native-minimum"
cd "$work/source"
env -i PATH=/usr/bin:/bin HOME="$work/home" TMPDIR="$work/tmp" LC_ALL=C.UTF-8 \
  CPPGW_NATIVE_WORK="$work" CPPGW_RUST_STAGE="$scratch/native-deps/rust" \
  /home/runner/.bun/bin/bun --no-env-file --no-install -e '
    import { buildNativeKv } from "./scripts/build-native-kv.ts";
    const work = Bun.env.CPPGW_NATIVE_WORK;
    const receipt = await buildNativeKv(work + "/artifacts/kv", {
      toolchainDirectory: Bun.env.CPPGW_RUST_STAGE + "/bin", workspaceDirectory: work + "/build",
    });
    console.log(JSON.stringify(receipt));
  ' > "$work/kv-receipt.json"
env -i PATH=/usr/bin:/bin HOME="$work/home" TMPDIR="$work/tmp" \
  cc -shared -fPIC -O2 -Wall -Wextra -Werror -march=armv8-a -DNAPI_VERSION=8 -I/usr/include/node \
    -D__NR_fchmodat2=452 -D__NR_setxattrat=463 -D__NR_removexattrat=466 -D__NR_file_setattr=469 \
    -o "$work/artifacts/permissions/linux-arm64.node" src/runtime/permissions-native.c
```

| Output                           | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| Host `native-kv.node`            | `56017beb299528f1e45c9d0eb71ddf5e22dee39d07495cd3ac2e2ed441a1774f` |
| Host `native-kv.provenance.json` | `2833e3319121f5237077be2b045ca7742a4fba9beb92039e853476557e8c824c` |
| Host `linux-arm64.node`          | `14a586d14df4b695260edda4ad37d0b2e0f5b1944c46ba5e1f5873f1ccb52af2` |

- **KV.** `scripts/build-native-kv.ts` uses Bun 1.4.0 builtins and Node standard modules. It downloads the Git source archive for `denoland/denokv@9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5`, verifies the archive and upstream lock/license, applies `tools/native-kv/{atomic-write-rollback,napi-workspace}.patch` at fuzz 0, and installs the verified frozen lock plus its derivation record. It then runs Rust 1.98.1 `cargo build --locked --release --jobs 1 --package deno-kv-napi --target aarch64-unknown-linux-gnu --manifest-path source/Cargo.toml --color never`. The recipe identifies npm SDK `@deno/kv@0.14.0` and N-API v4. `libdeno_kv_napi.so` comes from the fetched patched upstream workspace. Its dependencies are registry packages verified by Cargo against the frozen lock, including `napi`, `napi-sys`, `napi-derive` and the pinned SQLite stack. The workspace lock preserves 304 upstream entries and pins 316 registry checksums. The local inputs and patches supply provenance and correctness rules. The app's npm patch at `src/storage/patches/deno-kv-0.14.0.patch` is used by the oracle's package install, outside this native build.
- **Confinement.** `src/runtime/permissions-native.c` is the sole first-party input to the C compiler. The Docker build installs `libnode-dev` and uses `cc` with N-API v8 and the four syscall number defines above. It writes `manifest.json` with `path`, `architecture`, object and source hashes and Landlock ABI bounds 8–10. The legacy `scripts/build-permissions.ts` drives the same source for the TypeScript service using the pinned npm `node-api-headers@1.9.0` by default and an explicit header-directory override. The native Docker route calls `cc` directly.
- **Provenance and platform variations.** `native-receipt.py` retains the command and exit records, canonicalizes scratch paths, rebinds provenance to that log and copies license notices from upstream KV, the pinned Rust distribution and the verified registry. `scripts/build-native-kv-darwin.ts` drives the separate TypeScript Darwin build from `inputs-darwin-arm64.json` and the same six-file frozen set with the Darwin pin substituted. The C++ Darwin startup test reads that pin. The current macOS C++ package contains C++ development/version binaries and assets. Its workflow runs runtime primitives and `--version` without packaging either addon.
- **Runtime consumption.** `runtime/startup.cpp` requires both bound ELF files, provenance and permission manifests, and the V8 helper pair in the sealed package digest. `src/storage/native_addon.cpp` validates the KV binding and uses the linked `rt::SqliteKv` backend. `src/runtime/permissions_linux.cpp` validates the permission artifact and selects linked native Landlock/seccomp functions. Neither C++ implementation dynamically runs the `.node` code. The Bun oracle loads the actual N-API KV addon. Replacing these artifact requirements needs an explicit package/startup contract change.

`git ls-files rust/crates | cut -d/ -f3 | sort -u` and `rust/Cargo.toml` agree on **35 workspace members** at the pin, with **zero required by the C++ gateway**. The tree has 977 tracked entries and 311,482 text newlines. The full workspace is a separate Rust port and its support tooling:

| Scope                                      | Members                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Native Rust gateway boundary (2)           | `gateway-native`, `gateway-native-supervisor-read`                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| Worker entry point and fixture tooling (3) | `gateway-worker`, `gateway-worker-fixture`, `gateway-worker-fixture-setup`                                                                                                                                                                                                                                                                                                                                                                                                                               |
| Quality tools (2)                          | `gateway-quality`, `gateway-quality-syntax`                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| Test support (7)                           | `gateway-admin-test-support`, `gateway-claude-runtime-test-support`, `gateway-extras-runtime-test-support`, `gateway-migration-test-support`, `gateway-observability-test-support`, `gateway-publication-test-support`, `gateway-routing-runtime-test-support`                                                                                                                                                                                                                                           |
| Shared Rust gateway implementation (21)    | `gateway-core`, `gateway-compaction`, `gateway-schema`, `gateway-providers`, `gateway-authority`, `gateway-admission`, `gateway-catalog-runtime`, `gateway-capacity-recovery`, `gateway-auth`, `gateway-admin`, `gateway-admin-data`, `gateway-admin-runtime`, `gateway-migration`, `gateway-claude-runtime`, `gateway-observability`, `gateway-routing-runtime`, `gateway-extras-runtime`, `gateway-publication`, `gateway-publication-policy`, `gateway-publication-transport`, `gateway-live-runtime` |

`rust/vendor/webauthn-rs-core-0.6.1-dev` is a patched Rust-port dependency excluded from its workspace members. It is independent of the portable package's C++ vendors under `cpp/gateway/third_party/` and the KV addon's upstream Cargo build.

## Packaging and standalone control runs

`portable-closure.sh` archived `cpp/gateway`, `static`, `scripts/build-native-kv.ts`, `src/runtime/permissions-native.c`, `src/kv-conversion/{v8-helper.mjs,typed-value.ts}` and `tools/native-kv/{inputs.json,Cargo.lock,lock-derivation.json,LICENSE,atomic-write-rollback.patch,napi-workspace.patch}`. That is 48 external files. The scratch commit is `2c35bff35c1de8974ae5776d2efeb09e802ada78`, tree `96d222886e1e61bd2f2b2a477ba84e74f4add2c3`. The standard builder archives only those committed paths. The Docker native stage completed toolchain preparation and the CMake stage reached production object compilation.

```bash
cd "$scratch/portable/source"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$scratch/portable/home" TMPDIR="$scratch/portable/tmp" \
  DOCKER_CONFIG="$scratch/portable/docker" PORTABLE_JOBS=2 \
  bash cpp/gateway/packaging/portable/build.sh "$scratch/portable/uos-gateway-linux-arm64.tar.gz"
```

The wrapper is bounded to 60 minutes including networked dependency staging, network-disabled C++ compilation and archive production. `portable-build.log` is the Buildx progress handle. The Bookworm package will proceed to the existing Debian 12/13 verification and the pinned conformance check if the build succeeds.

I also extracted a `cpp/gateway`-only archive with no outside repository files. These commands ran in that archive with Clang 21, `env -i`, an empty home and external `TMPDIR`:

```bash
cmake -S cpp/gateway -B "$scratch/cpp-controls/cmake" -G Ninja -DUOS_GATEWAY_TESTS=ON
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/conformance/compare_test.py
```

CMake with `UOS_GATEWAY_BUILD_EVIDENCE=scratch-closure` configured 325 manifest source entries plus 44 runtime units, and generated CTest registrations for all 450 Link tests with zero skips. Configure took 17.9 seconds and generation 6.0 seconds. The Links check and all nine comparator controls passed. The CMake configure proof establishes source registration only. The suite and package runs provide the compile/link and test evidence.

The historical oracle checkout was created with `git worktree add --detach "$scratch/oracle" 2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`. After verifying both lockfiles exist, `bun --no-env-file install --frozen-lockfile --ignore-scripts --backend=copyfile` ran inside an empty environment and installed 89 packages in 2.77 seconds. The package remains the independent native input to the paired run.

The doc gate needed a separate **11-file** archive: `package.json`, `bun.lock`, `bunfig.toml`, `.prettierrc`, `.prettierignore`, `scripts/{format.sh,_bootstrap.sh,patch-bun-types.ts}`, `src/storage/patches/deno-kv-0.14.0.patch` and `tools/lint/{package.json,bun.lock}`. I installed the existing root and lint locks with `bun --no-env-file install --frozen-lockfile --ignore-scripts` in scratch, with `--backend=copyfile` for `tools/lint`. The locked Prettier is 3.9.6. `bun --no-env-file --no-install run format:check <changed Markdown paths>` passed on the report checkpoint. `scripts/format.sh` sources `_bootstrap.sh`, which checks both locks, the tools' ESLint/Prettier/Knip shims and patched Bun types plus Node types and `tsc`. The older workflow's docs-only gate instead invokes `scripts/test-scope.ts --check-docs` with the frozen lint install. The two C++ workflows have no root Bun formatter step.
