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

The release shard ran with `timeout -k 15s 2400s bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4`. The wrapper enters `closure` with `env -i`, fresh `HOME` and `TMPDIR`, `PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin`, `LANG=C.UTF-8`, `LC_ALL=C.UTF-8`, `TZ=UTC`, `CC=clang-21`, `CXX=clang++-21`, `UOS_CI_JOBS=4`, `UOS_CI_BUILD_DIR=$scratch/ci`, `UOS_CI_SQLITE_PREFIX=$scratch/sqlite`, `PKG_CONFIG_PATH=$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig`, `UOS_CI_CXXFLAGS=-isystem $scratch/boost_1_92_0` and `UOS_CI_LDFLAGS=-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib`. The 40-minute bound covered one cold object build and link/test phase. `shard-1.log` and the driver's incremental results provided progress. Focused checks and missing-input controls reused the C++ object cache.

The addon probe staged only `scripts/build-native-kv.ts`, `src/runtime/permissions-native.c`, and `tools/native-kv/{inputs.json,Cargo.lock,lock-derivation.json,LICENSE,atomic-write-rollback.patch,napi-workspace.patch}` from the baseline. It ran the portable builder's Bun import and C compile with a clean environment. Rust 1.98.1 was downloaded from the exact URL and verified with the archive SHA-256 in `native.sh`, `0b514a8cc1cbcd939bff0f151661fe58b6ea5c7a7f645a5098c69e32e8c1e0a2`. `native-minimum.sh` records the commands and writes only to scratch. The Cargo recipe builds pinned `deno-kv-napi` from the `denoland/denokv` Git archive and its crates.io lock. The confinement addon is N-API C compiled with `cc` and system `libnode-dev` headers. The 15-minute bound covered the locked registry download and single Cargo build worker.

## Initial dependency observations

- `openapi_runtime_contract_test.cpp` recursively opens every root `src/` `.ts` and scans it for `/uos/` literal routes, in addition to checking actual compiled native registration tables. Running it against a directory reduced to the V8 helper files could silently lose literal coverage. `admin_models_catalog_test.cpp` reads `src/models/catalog.ts`, `src/admin/codex.ts` and `src/handler/index.ts`.
- `static_assets_test.cpp` opens `README.md` and pins operator wording. It also requires the published auth uploader references in `static/docs/llms-agents.md`. `openai_spec_drift_test.cpp` opens `scripts/openai-spec-pin.json`. `sentinel_upstream_providers_test.cpp` opens `tests/fixtures/sentinel-historical-framing.json`. The startup test uses the Linux KV input manifest and selects `inputs-darwin-arm64.json` when compiled for Darwin.
- `runtime/paths.hpp` selects the manifest-verified immutable package resource root, or the current directory of a module test. `src/static.cpp` uses `src/static.ts` as a synthetic URL base and serves the 38 `static/` files and two root favicons. The Linux host and portable packagers and the macOS packager currently copy `static/` alone. Missing packaged favicons are a pre-existing defect to reproduce separately.
- `cpp/gateway/packaging/portable/build.sh` archives committed source paths, then the Dockerfile copies only the two V8 codec files from root `src/kv-conversion/`. `v8-helper.mjs` imports `typed-value.ts` and is an offline Deno helper. Package manifests require both bytes even for normal serving, and conversion invokes them. The package builders also verify the real KV and permissions artifact files and manifests.
- The paired conformance script currently diffs the whole candidate tree against the oracle revision with a narrow conformance-only exception, then builds both sides from the same archived source. A detached pinned TypeScript checkout needs a separate candidate build/package input. Existing comparison assertions and restart phases can be reused.

## Repository inputs by entry point

The 2,349-path release-test copy contains 1,987 tracked entries under `cpp/`, the two native workflow files and 360 other files. The exact filename selection is `git ls-tree -r --name-only "$baseline" -- cpp static .github/workflows/cpp-gateway-ci.yml .github/workflows/cpp-gateway-release.yml`, `git ls-tree -r --name-only "$baseline" -- src | rg '\.ts$'`, and the six individually listed test paths above, combined with `LC_ALL=C sort -u`. Fresh Git metadata supplies `HEAD` and tree identity. Git does not need the `.gitmodules` file or the Codex submodule for the native gateway gates.

| Entry point                                                                                                                                                         | Tracked input outside `cpp/` at the pin                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `CMakeLists.txt`, `cmake/{Profile,Tests,Vendors,ExtraTargets}.cmake`, `tools/{cmake-sources,generate-tests,vendor_sources,links}.py`, `bench/request_path/build.py` | CMake generation and C/C++ compilation use `cpp/gateway/{PORT-MANIFEST.json,src,include,runtime,entrypoints,third_party,tests,cmake}`. The manifest has historical `.ts` keys and actual C++ paths. The generator reads the C++ paths. Linux `cmake/Package.cmake` defaults to `tools/native-kv/inputs.json` for the separate package target.                                                                                                                                                                                                                                                                                                                                                                                   |
| `tools/ci-build.sh`, `cmake/ci/{driver,isolate}.py`, `tools/{ctest-run,ctest-network}.py`, `tools/{unit,run-tests,macos-build}.sh`                                  | The test programs read the archived test inputs above. `native_addon_differential_test.cpp` and `runtime_native_startup_test.cpp` also select `tools/native-kv/inputs-darwin-arm64.json` on macOS. Runtime tests get generated scratch copies and symlinks to fixtures. Test builds use checked-in C++ `*_vectors.inc`, inline fixture data and `tests/LINKS.txt` without invoking Bun. The `tools/` SDK and Arch profile wrappers have additional external toolchain and generated `.data/cpp-gateway-*`/instruction receipt paths. These paths are host/build state.                                                                                                                                                          |
| `fuzz/{ci,run,seed}.py` and CMake fuzz target registration                                                                                                          | Source paths, 24 target definitions, 306 seed recipes and 274 checked-in corpus files are under `cpp/gateway/`. `seed.py` extracts recorded C++ regex, KV and passkey fixtures and creates synthetic seeds. The CI runner compiles and runs the named target in fresh output. The host runner uses generated `.data/cpp-gateway-*-fuzz-*` build/evidence state. No repository runtime tree is required.                                                                                                                                                                                                                                                                                                                         |
| `.github/workflows/cpp-gateway-ci.yml`                                                                                                                              | Uses the Release/ASan shard and fuzz rows above. The workflow reads `cpp/gateway/fuzz/targets.json`, provisions host packages plus pinned SQLite and uploads receipts. It has no frozen Bun install, Rust workspace, root formatter or deploy step.                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `.github/workflows/cpp-gateway-release.yml` and `packaging/portable/{build.sh,Dockerfile,native.sh,native-receipt.py,payload.py}`                                   | Linux package external paths are the 48-file subset recorded below. `build.sh` archives the entire `tools/native-kv/` and `src/kv-conversion/` directories when available. Docker copies the Linux frozen inputs, build driver, C source, V8 pair and `static/`. The two explanatory KV Markdown files, Darwin pin and seven other `src/kv-conversion/*.ts` files accompany the full archive context only.                                                                                                                                                                                                                                                                                                                      |
| `tools/package.py`, `tools/runtime-manifest.py`, `cmake/Package.cmake` and `bench/request_path/package.py`                                                          | Host package assembly reads `static/**`, `src/kv-conversion/{v8-helper.mjs,typed-value.ts}`, explicit native KV and permissions artifact directories and manifests, and host ELF paths. The CMake default pin is `tools/native-kv/inputs.json`. Runtime library search and exact build-profile identity are host prerequisites. `tools/package.py` uses a new private output under `.data/`.                                                                                                                                                                                                                                                                                                                                    |
| Apple Silicon job and `packaging/macos/build.py`                                                                                                                    | The packager checks Git identity, enumerates `static/` with `git ls-files` and copies all 38 files. It depends on the host Xcode/SDK, Homebrew prerequisites, and C++ packaging notices. The workflow runs version and runtime primitive checks. The Darwin KV test input belongs to the separate application-test path, while the TypeScript Darwin addon builder belongs to the historical checkout.                                                                                                                                                                                                                                                                                                                          |
| `runtime/{paths.hpp,startup.cpp}`, `src/static.cpp`, `src/storage/native_addon.cpp`, `src/runtime/native_kv_bootstrap.cpp`                                          | Static reads resolve from package root or source-test cwd using the synthetic `src/static.ts` URL. Serving needs `static/**` and both PNGs. The package root provides the sealed executables, `manifest.sha256`, `package.json`, `runtime-closure.json`, launch/offline schemas, V8 pair, ELF dependencies and artifact bindings described below. A development binding can resolve the tracked KV pin under `tools/native-kv`, private `.data/native-kv.binding.json` and its selected `.data/native-kv-*` directory. Production-shaped startup instead uses the installed explicit artifact binding. Its launch manifest, marked `.data/kv-bun.sqlite3` and other authorized writable state are external to the release tree. |
| `src/kv_conversion/legacy_codec.cpp` and `entrypoints/{kv-convert,kv-proof}.cpp`                                                                                    | The offline tools use the same sealed resource root, packaged V8 pair and caller-supplied pinned Deno executable, stopped database/snapshot and explicit destination/receipt. `src/kv-conversion/legacy-codec.ts` is a synthetic URL base for the C++ runtime, like `src/static.ts`.                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `conformance/run.sh`, the Bun half of `bench/request_path`, historical generators and docs-only checks                                                              | These are separate reference or tooling inputs. The historical worktree, formatter input archive and removal plan below describe their complete scopes.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |

The Linux test copy plus the nine paths below is the 2,358-path union for native testing, packaging and runtime across the two documented platforms. Its sorted path-list SHA-256 is `e9b8627728987d71d5cdc8e24c9017604b954d9fdb422b25d1b33662fa72381a`. Byte reads were established in the narrower release-test and Linux package copies. The extra Darwin path is established by the platform-conditioned test loaders. The source worktree and formatter archive remain separate so the native proof cannot see their legacy application inputs.

```text
scripts/build-native-kv.ts
src/runtime/permissions-native.c
src/kv-conversion/v8-helper.mjs
tools/native-kv/Cargo.lock
tools/native-kv/LICENSE
tools/native-kv/atomic-write-rollback.patch
tools/native-kv/inputs-darwin-arm64.json
tools/native-kv/lock-derivation.json
tools/native-kv/napi-workspace.patch
```

`file-trace.wwALOku1` confirms the source scan. From a fresh home and temp directory I ran `timeout -k 5s 45s sudo -n strace -u "$(id -un)" -f -qq -s 512 -e trace=open,openat,openat2 -o "$trace/$filter.log" -- bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release "$filter"` for `openapi_runtime_contract` and `static_differential`. The driver still established its real network, mount and PID namespace boundaries. The first passed 11/11 and successfully opened all **316** tracked root TS sources. Its sorted open list SHA-256 is `2a15ec700af5483ac3d4ce77e775207676c2075c968a652503f9c0115ca9c100`. The second passed 1/1 and opened 19 of the 40 static resources. The broader serving probe below exercised every static file and the three favicon routes.

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

`portable-closure.sh` archived `cpp/gateway`, `static`, `scripts/build-native-kv.ts`, `src/runtime/permissions-native.c`, `src/kv-conversion/{v8-helper.mjs,typed-value.ts}` and `tools/native-kv/{inputs.json,Cargo.lock,lock-derivation.json,LICENSE,atomic-write-rollback.patch,napi-workspace.patch}`. That is 48 external files. The scratch commit is `2c35bff35c1de8974ae5776d2efeb09e802ada78`, tree `96d222886e1e61bd2f2b2a477ba84e74f4add2c3`. The standard builder archives those paths and all tracked entries under its `tools/native-kv` selector. The reduced archive supplies the six entries actually read by the Linux build.

```bash
cd "$scratch/portable/source"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$scratch/portable/home" TMPDIR="$scratch/portable/tmp" \
  DOCKER_CONFIG="$scratch/portable/docker" PORTABLE_JOBS=2 \
  bash cpp/gateway/packaging/portable/build.sh "$scratch/portable/uos-gateway-linux-arm64.tar.gz"
```

The 60-minute bound covered networked dependency staging, network-disabled C++ compilation and archive production. `portable-build.log` records the Buildx phases. The Bookworm archive fed the Debian 12/13 proof and the separate pinned conformance run.

I also extracted a `cpp/gateway`-only archive with no outside repository files. These commands ran in that archive with Clang 21, `env -i`, an empty home and external `TMPDIR`:

```bash
cmake -S cpp/gateway -B "$scratch/cpp-controls/cmake" -G Ninja -DUOS_GATEWAY_TESTS=ON
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/conformance/compare_test.py
```

CMake with `UOS_GATEWAY_BUILD_EVIDENCE=scratch-closure` configured 325 manifest source entries plus 44 runtime units, and generated CTest registrations for all 450 Link tests with zero skips. Configure took 17.9 seconds and generation 6.0 seconds. The Links check and all nine comparator controls passed. The CMake configure proof establishes source registration only. The suite and package runs provide the compile/link and test evidence.

The historical oracle checkout was created with `git worktree add --detach "$scratch/oracle" 2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`. After verifying both lockfiles exist, `bun --no-env-file install --frozen-lockfile --ignore-scripts --backend=copyfile` ran inside an empty environment and installed 89 packages in 2.77 seconds. The package remains the independent native input to the paired run.

The doc gate needed a separate **11-file** archive: `package.json`, `bun.lock`, `bunfig.toml`, `.prettierrc`, `.prettierignore`, `scripts/{format.sh,_bootstrap.sh,patch-bun-types.ts}`, `src/storage/patches/deno-kv-0.14.0.patch` and `tools/lint/{package.json,bun.lock}`. I installed the existing root and lint locks with `bun --no-env-file install --frozen-lockfile --ignore-scripts` in scratch, with `--backend=copyfile` for `tools/lint`. The locked Prettier is 3.9.6. `bun --no-env-file --no-install run format:check <changed Markdown paths>` passed on the report checkpoint. `scripts/format.sh` sources `_bootstrap.sh`, which checks both locks, the tools' ESLint/Prettier/Knip shims and patched Bun types plus Node types and `tsc`. The older workflow's docs-only gate instead invokes `scripts/test-scope.ts --check-docs` with the frozen lint install. The two C++ workflows have no root Bun formatter step.

## Release shard and package checkpoint

The cold, unmodified `bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4` in the 2,349-path archive passed **113/113 binaries and 1,332/1,332 cases**, with 369/369 production objects, no fixture failures and exit 0. The object phase took 1,867.610 s, link/test 39.476 s and the timed command 1,907.80 s. The driver proved the network, mount and PID namespace boundary via sudo, used the runner's unprivileged uid 1001 for execution, and reported SQLite 3.54.0. Before/after driver input SHA-256 is `97e66e6321b955b3a5e00ac9a6297b1520d045cd462186b977dc6d64c89790b6`. The independent Git tree captures the external fixtures. `receipts/shard-1.json` SHA-256 is `d3e37834a28158083be3fca228ad391310ec5c94b5191cfa50797a6d1ca8b0b2`.

The portable Linux build exited 0 in 1,536.75 s. It produced an 83,898,817-byte archive with SHA-256 `94edae98ce239aff68e4434ae673373db5ba62277a31868696a424c49eab46fe`. The initial JSON fuzzer wrapper's 260-second timeout ended during sanitized object compilation. The 300-second retry with two workers exited 0 after 871.09 s total, including 556.944 s building. The receipt recorded 1,211 corpus entries, 313 fuzzed seconds and zero genuine findings. libFuzzer itself returned 77 for the one classified empty-input ARM64 fork-parent exit leak. `fuzz-retry/destination/summary.json` SHA-256 is `fe262f514d54be7d97a30b702d9b238f5eeb0a4674ed5c76a47c09298a82bddb`.

## Focused tests and omission controls

I resumed on `80af4c7d2a91a100b5b1942c451601c26f0a822e` and reconciled the scratch receipts to the archived source. `run-focus.sh` ran each filter below using the same `ci-gate.sh` wrapper and `timeout -k 15s 660s bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release "$filter"`. Each exited 0 on the same commit and tree as shard 1, with 369/369 production objects and matching before/after input hashes. The filters overlap. `jq -s '[.[].results[]] | group_by(.test) | map(.[0])'` gives **85 distinct binaries and 1,084 cases** in the focused runs and **179 distinct binaries and 2,121 cases** with shard 1 included.

| Release filter                | Binaries passed | Cases passed |
| ----------------------------- | --------------: | -----------: |
| `tests/rt/`                   |              37 |          474 |
| `runtime_`                    |              18 |          165 |
| `static`                      |               2 |           39 |
| `kv`                          |              24 |          402 |
| `permissions`                 |               2 |           10 |
| `serve_`                      |               3 |           28 |
| `sqlite_readonly`             |               1 |            5 |
| `native_addon_differential`   |               1 |            4 |
| `openapi_runtime_contract`    |               1 |           11 |
| `admin_models_catalog`        |               1 |           33 |
| `sentinel_upstream_providers` |               1 |           19 |
| `openai_spec_drift`           |               1 |            1 |

`runtime_` includes the 18 native development startup cases. The `serve_` matches include five serving-module cases, seven delivery and drain cases, and 16 kernel quota reserve cases selected by the substring `reserve_`. Packaged startup and refusal checks use real bound artifacts as described below. The registered test copies use synthetic state and artifacts where their original tests specify them.

`fixture-removals.sh` used `git switch -q --detach 4272fd984e847f9a66bea5258a4a5c0ec0e0266c` in the scratch checkout, `git rm -rq -- <paths>`, then a scratch commit per control and `timeout -k 10s 180s bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release <filter>`. Each command compiled successfully and failed the named test with exit 1. The wrapper's copied JSON files selected a stale path after the driver rotated its reports. I recovered each exact receipt from the active driver directory or its `history/` by matching both keys to the omission commit and tree recorded in `receipts/removals/*.identity`. The original `closure` was restored to `4272fd984e847f9a66bea5258a4a5c0ec0e0266c` and its tracked paths rechecked against `closure.paths`.

| Omitted only from scratch                                             | Selected filter               | Cases passed/run | Observed dependency                                   |
| --------------------------------------------------------------------- | ----------------------------- | ---------------: | ----------------------------------------------------- |
| `static/`                                                             | `static_differential`         |              0/1 | Preloaded document and asset files                    |
| `favicon.png`                                                         | `static_differential`         |              0/1 | `/favicon.ico` and `/favicon.png` bytes               |
| `favicon-32.png`                                                      | `static_differential`         |              0/1 | `/favicon-32.png` bytes                               |
| `README.md`                                                           | `static_assets`               |            37/38 | Published operator wording                            |
| Root `src/`                                                           | `openapi_runtime_contract`    |             9/11 | UOS dispatch and unpublished-literal scans            |
| `src/models/catalog.ts`, `src/admin/codex.ts`, `src/handler/index.ts` | `admin_models_catalog`        |            31/33 | Passive render and explicit refresh source assertions |
| `scripts/openai-spec-pin.json`                                        | `openai_spec_drift`           |              0/1 | Official API property allowlists                      |
| `tests/fixtures/sentinel-historical-framing.json`                     | `sentinel_upstream_providers` |            18/19 | Historical framing replay                             |
| `tools/native-kv/inputs.json`                                         | `runtime_native_startup`      |             0/18 | Linux development addon trust pin                     |

## Sealed package and oracle results

From the portable scratch checkout, `bash cpp/gateway/packaging/portable/verify.sh "$scratch/portable/uos-gateway-linux-arm64.tar.gz" "$scratch/portable/verify"` ran in a fresh `env -i` with the same isolated Docker config as the build. It exited 0 in 719.15 s. Each of Debian 12 and 13 at 256 and 512 MiB completed and accounted for 1,161 mocked Chat requests and reopened with the same count, for 4,644 total. Each reported Landlock ABI 8, two seccomp filters, 401 on an unauthorized Chat request, no OOM events, and a healthy initial and post-load listener. Cgroup peaks were 37,785,600–41,369,600 bytes. The binary SHA-256 was `7c806b731607672294c496cb19e594e3b2aec0c6f72e5a9645a618659b13a906` in all four runs.

The independent oracle run used `paired-current.sh`, whose receipt binds the freshly sealed native package to the reduced package commit and tree via `VERSION`, and binds Bun to the detached `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` worktree and frozen lock. It compared the oracle KV pin with the packaged pin and consumed the seal receipt's independent provenance hash. `conformance/run.py` ran via `bench/request_path/isolated.sh` with `--source` set to that worktree and `--package` set to the separately built seal. It executed 162 first-phase and 24 post-restart cases, took 2.679 s in the suite and exited 1 in 5.83 s wall time. **184 matched and passed, two shared failures, zero functional differences.** `chat-invalid-role` and `responses-invalid-tool-type` expected 400 and zero dispatch. Both implementations returned 200 and dispatched to the mock. IPv4 and IPv6 route-denial controls returned `ENETUNREACH`. `paired/output/results.json` SHA-256 is `0049c8cb5361275c31f084471d8a64ff9d12f78664c7d9d121513fa82d053b7b`. The nine comparator controls passed independently.

The packaged resource probe at `packaged-resources.cRquY5we/probe.py` reused that seal. It ran with `sudo -n unshare --net --pid --fork --mount-proc --kill-child=SIGKILL`, loopback only, `setpriv --reuid 1001 --regid 1001 --clear-groups --no-new-privs`, `env -i`, and `python3 -BI`. `/health`, `/openapi.json` and `/llms-full.txt` returned 200. The two document responses matched tracked bytes. `/favicon.ico`, `/favicon.png` and `/favicon-32.png` returned 404 with `ENOENT` for the missing PNGs under the sealed root. The mock received zero requests and the gateway shut down naturally. This is an existing packaging defect. The Linux packagers and the current macOS packager copy `static/` and omit its sibling PNGs. With the same isolation command and seal, `all-static.Torzg7jz/verify.py` requested all **38** files under `static/`, including `/` with `Accept: text/markdown` for `home.md`. Every response returned 200 and matched the source bytes by SHA-256. The result SHA-256 is `dd35f15ac0bdd32d3fc8cf2665fb3f65530918e99615a7607ca0deccc395efbf`.

The same probe ran the sealed `src/kv-conversion/v8-helper.mjs` using `deno run --no-config --no-lock --no-prompt --cached-only <helper>` in that network namespace, with a fresh `DENO_DIR`. Deno reported `2.9.7`. The helper round-tripped a synthetic object containing a string, negative zero and a BigInt using `typed-value.ts`. The pair matched the archived source bytes. Two malformed synthetic graph attempts were rejected before the fixture used the required `ref` and `object` tags. The passing probe took 0.37 s total and its result SHA-256 is `abf51eefa925af134a8f2dc183b3a93135e3150cc58d9afc0ba3e295b65eeb43`. This is a helper check on synthetic values. Full database forward/reverse and unsupported-value controls remain in the removal acceptance plan.

`package-refusals.0hyfKsLO/check.py` tested the sealed `bin/uos-gateway` through its actual `--runtime-manifest` entry point in the same OS namespace and sparse environment. **12/12 controls** exited 1 before any listener or state write. It tried no manifest argument, wrong manifest SHA, wrong provenance SHA, a public hostname, each protected port, nonprivate state and a missing marked database. Four further controls removed exactly one file and its manifest record from a private package copy, then bound that copy's new manifest hash. Startup independently refused missing `native/kv/native-kv.node`, `native/permissions/linux-arm64.node`, `src/kv-conversion/v8-helper.mjs` and `src/kv-conversion/typed-value.ts`. The original seal remained intact. The probe took 1.36 s and its result SHA-256 is `f1b3d1a1ad5533f9fefecda9132cc790b46d9b58de4d16b3897b71ee69285097`.

## Inventory handoff

The [removal plan](../CLEANUP-PLAN.md) covers all 34 tracked root entries and all three workflows. I verified the verdicts against `git ls-tree --name-only "$baseline"` and `git ls-tree -r --name-only "$baseline" -- .github/workflows`. The before/after projection used `git ls-tree -rz "$baseline"` and `git cat-file --batch` to count tracked blobs, symlinks and gitlinks and literal LF bytes in text blobs. The 4,251-entry baseline has 1,690,611 text newlines. The two inventory documents bring the current tracked count to 4,253. The plan projects 2,057 tracked entries and about 1.01 million text newlines after the relocations, approved removals, root metadata edits and oracle pin. Platform packaging checks on macOS, the rest of the native shard and fuzz matrices, and the owner-controlled rollout remain outside the Linux evidence of this job.

The documentation gate checks the two changed paths with `bun --no-env-file --no-install run format:check` in the frozen 11-input formatter archive and `git diff --check "$baseline" HEAD` in the checkout. In a separate four-input format-only copy with `package.json`, `bun.lock`, `.prettierrc` and `.prettierignore`, `bun --no-env-file install --frozen-lockfile --ignore-scripts` installed Prettier 3.9.6 as its sole package. Both documents passed `bun --no-env-file --no-install run format:check` there too. The trial lock's SHA-256 is `b948d159e5bf18ad2760216fd28f9ab5be3b23ec557656a314eec95c691d390c`. The working tree's package and lock are the original ones.

`git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development` observed `a3a96bc5fe96ba383936746fae23e64daa69562f` first and later `0a6e31a5cafb48b0b028d397ec0d62a60bbbf3be`. Both have zero `cpp/gateway` paths. `git merge-base --is-ancestor HEAD origin/development` confirmed the docs tip is absent. Both `git push --dry-run --porcelain origin HEAD:refs/heads/cpp-gateway-cleanup-inventory-20261118` and the corresponding push exited 128 with `could not read Username for 'https://github.com': terminal prompts disabled`. These used `GIT_TERMINAL_PROMPT=0`, `GIT_ASKPASS=/bin/false`, a credential-free environment and a 20-second timeout. GitHub CLI authentication was also unavailable. Publication, the PR and the `development` ancestry handoff are pending. The ordered docs commits are on `agent-job`. The local handoff bundle in runner scratch is `cppgw-cleanup-inventory-20261118.bundle`, created with `git bundle create "$scratch/cppgw-cleanup-inventory-20261118.bundle" agent-job "^$baseline"` and checked with `git bundle verify`.

**Summary.** Release shard 1/4 passed 113 binaries and 1,332 cases from a 2,349-path checkout. The focused runs passed 85 distinct binaries, and the sealed package passed 12 startup refusal controls. The C++ gateway needs zero crates from this repository's Rust workspace. The Linux package passed on Debian 12 and 13, and the audit reproduced three favicon 404s and two validation failures shared by Bun and C++. The plan covers 34 root entries and all three workflows. Both documents are committed locally. GitHub write authentication blocked publication and the `development` handoff.
