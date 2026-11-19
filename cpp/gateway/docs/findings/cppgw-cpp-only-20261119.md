# C++-only gateway branch

## Checklist

- [x] Start the report on the supplied Gemini integration tree.
- [x] Merge the residuals branch and record the request-allowlist revert under STATE A.
- [x] Build and run the selected State A request tests.
- [x] Inventory tracked language sources, toolchain commands and runtime dependencies.
- [x] Remove the historical comparison and formatter.
- [x] Call the linked Linux confinement implementation directly.
- [x] Retire the offline legacy conversion executables and helper.
- [x] Remove unnecessary package runtime artifacts and offline tooling.
- [x] Update CI and current operating documents.
- [x] Add the ARM64 cutover checklist and runtime environment table.
- [x] Verify registrations, tracked files, workflows and whitespace.
- [ ] Build the gateway and run the selected release tests in native isolation.
- [ ] Build and verify the portable package.
- [ ] Record final results and leave the checkout clean.

## STATE A

Starting tree: `13502438` (`agent-jobs/cppgw-gemini-merge-20261119-44556239421-1`).
`git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1` fetched `a5b0e14a36b228272161ab71def2269b34e17d68`. `git merge --no-ff --no-edit FETCH_HEAD` created `0ced7f44c3fdf77323201dd969b102248bf72133` without conflicts.

**STATE A commit:** `ca045a83ebaab6c50c20567d1a825638abac0891`, tree `07cded5948418115e3080701f7be15c16a1857ba`. `git revert --no-edit d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1` conflicted only in the residuals findings. The resolution preserved the merged observations for the other fixes. `git revert --continue` removed both ingress allowlists and their three tests. Both ingress model-length guards and the oversized-model regression from `b58afd3d1` remained as merged.

## Scope

Local verification uses qualified native dependencies and the repository's loopback-only fixture driver. Build outputs and logs stay outside the checkout. The hosted matrix is the full-suite verification path.

## Inventory and dependency decisions

- `git ls-files | rg -i '\.(ts|tsx|js|jsx|mjs|cjs|rs)$|Cargo|package\.json|bun\.lock|bunfig|prettier|native-kv|legacy-codec|\.node$|rust-toolchain'` found three TypeScript files, the offline `v8-helper.mjs`, one Cargo lock, two Bun configs, root formatter files and 14 browser scripts under `static/`. No tracked Rust source, add-on binary, or `tests/helpers/*.mjs` remained. `git grep -Il -i -E '(bun|deno|nodejs|cargo|rustc|rustup|typescript|npm|native.kv|legacy.codec|conformance|prettier)'` and targeted searches for artifact paths identified the active callers, packaging, launcher and docs. The historical findings and recorded C++ fixtures are evidence and test inputs.
- `src/storage/native_addon.cpp` verifies the external add-on and returns C++ placeholders. `src/storage/kv.cpp` opens `rt::SqliteKv` directly, and `src/rt/kv_sqlite.cpp` implements the store. `src/rt/kv_jsc*.cpp` implement the persisted value codec. The `serve.cpp` bootstrap gate checks preparation, which only verified that unused artifact. Keep the marked database path, marker and data format.
- `src/runtime/permissions_linux.cpp` hashes the external helper before returning function pointers to its own Landlock and seccomp implementation. `runtime/startup.cpp` verifies both artifacts before opening the store or binding the listener. Neither artifact is loaded. Remove their build and verification paths and keep the linked confinement and manifest checks for the remaining payload.
- `runtime/legacy-codec/typed-value.ts` is imported by `v8-helper.mjs`. The C++ `kv_conversion/legacy_codec.cpp` starts the pinned codec subprocess only for offline conversion and the round-trip proof. Native storage and serving have no call path to that helper. Native snapshot reads and portable typed-value handling are independent and remain covered.
- The existing opt-in Gemini provider executes the owner-installed `agy` binary, and confines child execution to that file and the system ARM64 loader. The gateway, workflow and package do not install that executable or a language runtime for it.

Removed the paired harness and pins, comparison modes from the smoke and benchmark drivers, and the root formatter dependencies. The native HTTP smoke and JSON, regex and request-path measurements remain. The hook uses Git whitespace checks. Parsed all affected Python with `ast.parse` and ran `git diff --check`.

The confinement change keeps the same Landlock and seccomp implementations and policy. It removes the lookup of a separate, never-loaded helper from the confinement API. Three helper-file-only cases were retired. The kernel, strict and reduced boundary, and Gemini child checks remain. Clang 21 strict syntax checks and the registration gate passed.

Removed the pinned offline V8 subprocess, its converter and round-trip proof entry points, and their exclusively offline CLI, conversion and proof tests. The file-digest case, private SQLite snapshot API and tests, schema and queue checks, native profile export, typed values, and cursor fixtures remain. The executable and package target lists now build the gateway only. The CMake inventory and native registration gates passed after the removal.

Both Linux package paths now bind the single gateway executable, static assets, remaining licenses, runtime and operator closure, and the launch schema. The launch manifest no longer has an add-on provenance field. Startup still requires a private manifest and state, verifies the package and host libraries, and installs linked confinement before opening the marked store. Storage opens only a supplied local path, and serving refuses an uninitialized store before touching a listener. The Node-API artifact-only tests were retired. The startup, storage, per-process durability and bind-failure checks now use the linked native store. Compared all resolved test closures with STATE A. Only paths retired with these capabilities and the two deliberately revised test closures changed.

The portable mock proof now checks refusal of a wrong launch manifest hash and a changed packaged static file before any state is opened. It runs both checks against disposable files with a clean environment inside the existing no-network container.

Updated the runtime, root, release and operator guides for the single-executable package. The runtime table covers the two production `UOS_GATEWAY_*` switches and every matching environment read by the native build and test tools. The cutover checklist uses the existing systemd draft, sealer and health contract. The Arch host, copied production state, live observation, switch and rollback remain UNVERIFIED.

Pre-existing outside-scope content: `static/docs/llms-agents.md` still documents legacy auth-upload commands for the live service. It remains a public asset as supplied. The portable build still reports the checked-in runtime release SHA `unknown` through health. Archive identity is bound independently to the full commit and tree, but this health value blocks production acceptance under the existing exact-release requirement. No deployment or identity change was made.

## Runner and exact commands

The Linux ARM64 runner uses Ubuntu 24.04.5, kernel 7.0.0-1017-azure, four CPUs and Landlock ABI 8. Clang 21.1.8 uses the signed Noble LLVM repository (fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`). GCC 14.2.0 built Boost 1.92.0 JSON and Container static archives. Installed the native development packages with `sudo apt-get install -y -qq clang-21 libclang-rt-21-dev mold ninja-build libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux iproute2 pkg-config`. Downloaded with `curl -q -fLsS --retry 3` and checked the Boost, ICU and SQLite archive hashes against the [existing verified pins](cppgw-input-limits-20261119.md#runner-and-exact-commands) before extraction. The reusable scratch root is `/var/tmp/cppgw-cpp-only-20261119`.

```sh
# In the extracted Boost source, after pinning gcc-14 in boost-user-config.jam.
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --user-config=../boost-user-config.jam --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage

# In the separate icu-build and extracted SQLite source directories, respectively.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
```

The scratch test wrapper `run-release.sh` runs from the detached worktree at `$scratch/state-a`. It uses a fresh `run` directory for every invocation and the repository CI driver for all compilation, links and test isolation. The Python selector only restricts `links.all_tests()` before calling `driver.main()` with `--config release`. It selects closures containing either request source for STATE A and the patterns in `$scratch/final-groups.tsv` for final gates.

```sh
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC   HOME="$run/home" TMPDIR="$run/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4   UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite"   PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig"   UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0"   UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib"   python3 -B "$scratch/selected.py" "$@"
```

STATE A ran `bash "$scratch/run-release.sh" state-a "$index" 10` for indices 0, 10, 20, 30, 40, 50 and 60. All receipts bind the exact STATE A commit and tree and unchanged input SHA-256 `3ab20c3a1b83656aa230cb33703ceff243f540672fa80668773045ea2db9194e`. The driver proved loopback-only networking in fresh network, mount and PID namespaces via `sudo`, dropped back to uid 1001, and probed SQLite 3.54.0 inside the boundary. Every linked test passed.

| Group start | Binaries | Cases | Driver wall time |
| --- | ---: | ---: | ---: |
| 0 | 10 | 151 | 725.994 s |
| 10 | 10 | 136 | 42.142 s |
| 20 | 10 | 141 | 51.718 s |
| 30 | 10 | 168 | 61.883 s |
| 40 | 10 | 77 | 38.475 s |
| 50 | 10 | 129 | 56.778 s |
| 60 | 1 | 1 | 14.293 s |
| Total | 61 | 803 | 991.283 s |

The first group built all 371 STATE A production objects outside the checkout. Later groups reused them. `openai_compat_part11_test` passed 23 cases including both oversized-model checks.

## Static checks and gates in progress

```sh
python3 cpp/gateway/tools/links.py --check
git ls-files | grep -E '\.(ts|tsx|js|mjs|cjs|rs)$|Cargo|package\.json|bun\.lock|bunfig'
git grep -nE '\b(bun|cargo|rustc|npm|node) ' -- .github cpp/gateway/packaging cpp/gateway/tools cpp/gateway/cmake
git diff --check 13502438 HEAD
"$scratch/bin/actionlint-1.7.12" -config-file "$scratch/actionlint.yaml" .github/workflows/cpp-gateway-ci.yml .github/workflows/cpp-gateway-release.yml
```

Registration resolved 449 binaries. The filename scan returned 14 tracked browser scripts under `static/` only. The command scan returned zero matches. Whitespace and both workflow checks passed. The first bare actionlint attempt flagged the existing `ubuntu-26.04-arm` and `macos-26` labels in its older catalog. The verified 1.7.12 release recognizes `macos-26`. Its temporary local configuration adds the pre-existing `ubuntu-26.04-arm` label for schema validation. No runner label or workflow trigger was changed.

The final test worktree is now at `3d44d15e2db235f5b72f7b304eac51db6e873f28`, tree `662efec391f9153d4dcbecfd8d28d07a78a31789`. Its Release groups run sequentially with logs and receipts named `final-<group>` in scratch.

The portable build runs from `$scratch/package-tree` at commit `d5454f1702eb22a89e7f6e570893a6ae5e844e7d`, tree `b5724b1ae685e43b05e59e41c5a10a6f90dd0bfb`, with the same shipping code and package inputs as the test tree. Its background command uses `env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$run/home" TMPDIR="$run/tmp" /usr/bin/time -p bash cpp/gateway/packaging/portable/build.sh "$scratch/uos-gateway-linux-arm64.tar.gz"`. Poll `package-build.log` and `package-build.status`. When it completes, run the verifier from the same package worktree, with a fresh empty home and temporary root and a new evidence path.
