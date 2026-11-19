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
- [x] Build the gateway and run the selected release tests in native isolation.
- [x] Build and verify the portable package.
- [x] Record final results and leave the checkout clean.

## STATE A

Starting tree: `13502438` (`agent-jobs/cppgw-gemini-merge-20261119-44556239421-1`).
`git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1` fetched `a5b0e14a36b228272161ab71def2269b34e17d68`. `git merge --no-ff --no-edit FETCH_HEAD` created `0ced7f44c3fdf77323201dd969b102248bf72133` without conflicts.

**STATE A commit:** `ca045a83ebaab6c50c20567d1a825638abac0891`, tree `07cded5948418115e3080701f7be15c16a1857ba`. `git revert --no-edit d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1` conflicted only in the residuals findings. The resolution preserved the merged observations for the other fixes. `git revert --continue` removed both ingress allowlists and their three tests. Both ingress model-length guards and the oversized-model regression from `b58afd3d1` remained as merged.

## Scope

Local verification uses qualified native dependencies and the repository's loopback-only fixture driver. Build outputs and logs stay outside the checkout. The hosted matrix is the full-suite verification path.

## Inventory and dependency decisions

- `git ls-files | rg -i '\.(ts|tsx|js|jsx|mjs|cjs|rs)$|Cargo|package\.json|bun\.lock|bunfig|prettier|native-kv|legacy-codec|\.node$|rust-toolchain'` found three TypeScript files, the offline `v8-helper.mjs`, one Cargo lock, two Bun configs, root formatter files and 15 browser scripts under `static/`. No tracked Rust source, add-on binary, or `tests/helpers/*.mjs` remained. `git grep -Il -i -E '(bun|deno|nodejs|cargo|rustc|rustup|typescript|npm|native.kv|legacy.codec|conformance|prettier)'` and targeted searches for artifact paths identified the active callers, packaging, launcher and docs. The historical findings and recorded C++ fixtures are evidence and test inputs.
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
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$run/home" TMPDIR="$run/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  python3 -B "$scratch/selected.py" "$@"
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

Registration resolved 449 binaries. The filename scan returned 15 tracked browser scripts under `static/` only. The command scan returned zero matches. Whitespace and both workflow checks passed. The first bare actionlint attempt flagged the existing `ubuntu-26.04-arm` and `macos-26` labels in its older catalog. The verified 1.7.12 release recognizes `macos-26`. Its temporary local configuration adds the pre-existing `ubuntu-26.04-arm` label for schema validation. No runner label or workflow trigger was changed.

The final test worktree is now at `3d44d15e2db235f5b72f7b304eac51db6e873f28`, tree `662efec391f9153d4dcbecfd8d28d07a78a31789`. Its Release groups run sequentially with logs and receipts named `final-<group>` in scratch.

The portable build runs from `$scratch/package-tree` at commit `d5454f1702eb22a89e7f6e570893a6ae5e844e7d`, tree `b5724b1ae685e43b05e59e41c5a10a6f90dd0bfb`, with the same shipping code and package inputs as the test tree. Its background command uses `env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$run/home" TMPDIR="$run/tmp" /usr/bin/time -p bash cpp/gateway/packaging/portable/build.sh "$scratch/uos-gateway-linux-arm64.tar.gz"`. Poll `package-build.log` and `package-build.status`. When it completes, run the verifier from the same package worktree, with a fresh empty home and temporary root and a new evidence path.

The first startup/static Release group passed five binaries and 60 of 61 cases. The static scan still required a repository-root command in the root README after the old upload section was removed. Restored that command as the entry to the native build/test instructions and kept the static assertions. The CI driver also no longer sets the leftover terminal-only environment variable from the old runtime. Requalify both changes with the final focused groups.

The storage source is a port of the upstream denokv SQLite backend. Kept its original MIT notice as `packaging/DENOKV-LICENSE` and included it in all package assemblers. This notice covers the linked implementation that remains. The recipe-only notices were removed earlier. The provisional portable build was already compiling at `d5454f17`. Reuse its build cache for the accepted archive that includes the retained notice, and verify that archive with its matching commit.

## Focused final Release gates

After updating the README and CI fixture environment, repeated the seven groups at commit `a6e70a210a3f62f203808edb3c3989ccf1870f9c`, tree `d1687cf0586d12cf5e0714a51452b14242ee17a8`. Each command was `bash "$scratch/run-release.sh" "$pattern"` with the pattern from `$scratch/final-groups.tsv`. `accepted-<group>.json` receipts bind unchanged input SHA-256 `a59500446147dc51130dcfd80ab2cf60d4792a345ee49a099f655ff3298f644e`. All 366 production objects were available, with 44 shared runtime objects. The rerun reused the warmed objects.

| Group | Binaries | Passed cases | Driver wall time |
| --- | ---: | ---: | ---: |
| Startup, HTTP, delivery, and static assets | 6 | 61 | 7.117 s |
| Strict, reduced and Gemini confinement | 4 | 38 | 1.780 s |
| Runtime KV and SQLite | 6 | 119 | 7.442 s |
| Storage, durable interop and budgets | 9 | 176 | 2.086 s |
| Snapshots, schema, cursors and typed values | 7 | 37 | 1.377 s |
| Edited storage and serving fixtures | 8 | 70 | 2.464 s |
| Inference and residual regressions | 5 | 128 | 5.569 s |
| Total | 45 | 629 | 27.835 s |

`env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC HOME="$run/home" TMPDIR="$run/tmp" /usr/bin/time -p python3 -B "$scratch/native-extras.py"` used the release receipt compiler flags, source closures and vendor objects to link the gateway and the auxiliary native targets from the same worktree. Full commands are in `$scratch/native-extras/*.compile.log` and `*.link.log`. The gateway ELF is 41,213,136 bytes, SHA-256 `3e8bbba42eab2b4f5ec0337d8ca24d547245cf08297aff3a0220925b8b8fbd70`. The standalone bind-failure gate passed 3 checks in 0.064 s, including one close of its real durable store. The native JSON runner passed 18 trials across three inputs in 2.621 s. The native regex binary passed the redaction smoke in 1.618 s. All three ran through the CI OS boundary. The native extras invocation took 26.50 s.

## Package gate status

The interim portable build on `d5454f17` was cancelled with SIGINT after identifying the license that covers the linked KV port. It exited 130 during compilation and published no archive. The accepted build runs the same background build command from `$scratch/package-tree` at `bdcbf891dc45c945c6805d3508d574a084b588f5`, tree `da68f4e4102c12545a929c5858bf76ccb3baa4f2`. The log is `package-build-accepted.log`. The cache already reused compiled objects from the cancelled run.

The verifier is queued in the same background command after successful publication. It uses a separate empty home and fresh temp, the same committed worktree and `bash cpp/gateway/packaging/portable/verify.sh "$scratch/uos-gateway-linux-arm64.tar.gz" "$scratch/package-evidence"` under `env -i` and `/usr/bin/time -p`. Poll `package-build-accepted.status`, `package-verify.log` and `package-verify.status`. A separate loopback benchmark/smoke check is queued after verification in `package-followups.log`. The local portable build, packaged proofs and follow-ups are the remaining verification work.

## Portable package results

The accepted build at `bdcbf891dc45c945c6805d3508d574a084b588f5` finished in 658.93 s. The committed tree recorded in the archive is `da68f4e4102c12545a929c5858bf76ccb3baa4f2`. The Bookworm image used Clang 21.1.5 and the glibc 2.36 ceiling. Ccache reported 154 hits and 382 misses across 536 cacheable calls. The archive is 27,508,858 bytes with SHA-256 `99cdeeece8afaa34a94671015453d7da88acdc11e540da307561600c65e77ffc`. The payload has 65 bound files and one gateway executable. Listing the archive found the SQLite and denokv notices and no legacy program, add-on, dependency lock or copied codec helper.

`verify.sh` passed in 41.83 s. Each container used two CPUs, its listed memory cap, zero swap, ABI 8, an unprivileged uid and Docker's no-network boundary. ELF linkage and relocation checks passed on both Debian releases. The same gateway SHA-256 `8fdcb2ba70b6ef69dce65a061bfb377c7853fd73bdfa2ac2f2013b8aa9c1bc4a` ran in all four proofs. The sealed manifest hashes are `8aed6ae2113b3c5a4a624862aef6f04799c9faf3d2e749924bbf51694f82c136` on Debian 12 and `1a956f12d080e484f1118d087b453cc3fdfc3fa7886cc5fa3aec5fa40fce29df` on Debian 13.

| Debian | Cap | Completed and accounted | Accounted after reopen | Integrity refusals | Cgroup peak | Median requests/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 12 | 512 MiB | 1,161 | 1,161 | 2 | 83.25 MiB | 387.91 |
| 12 | 256 MiB | 1,161 | 1,161 | 2 | 83.20 MiB | 383.83 |
| 13 | 512 MiB | 1,161 | 1,161 | 2 | 83.74 MiB | 386.12 |
| 13 | 256 MiB | 1,161 | 1,161 | 2 | 83.73 MiB | 383.99 |

Every proof checked packaged favicons, required authentication and observed no OOM events. Each verified the additional seccomp filter and refused a bad binding or modified asset with empty state. Evidence is in `$scratch/package-evidence/debian-{12,13}`. Runtime health returned `git_sha: unknown` and `deployment_id: unknown` in all four proofs, confirming the existing release-identity issue called out in CUTOVER.md.

After sealing the same archive on the Ubuntu runner with `python3 -B "$run/bundle/seal.py" "$scratch/host-sealed"`, ran `bash cpp/gateway/bench/request_path/isolated.sh --package "$scratch/host-sealed" --out "$scratch/native-benchmark" --scenarios small stream reject --repetitions 1 --concurrency 2 --requests 6 --warmup 1` and `python3 -B cpp/gateway/bench/request_path/summarize.py "$scratch/native-benchmark/results.json"`. All 18 timed requests passed, including progressive SSE and the rejected-request scenario. `python3 -B "$scratch/check-native-smoke.py"` ran the native HTTP smoke over the CI fixture boundary with the same sealed package. It passed in 1.719 s with 20 captures, seven terminals and usage increasing from six to seven across graceful shutdown and reopen, with eight mock dispatches before and after. It covered progressive SSE and WebSocket delivery.

The clean-environment CI driver control command was `CXX=clang++-21 UOS_CI_SELFTEST_CONFIG=release python3 -B cpp/gateway/cmake/ci/test_driver.py` with the same explicit PATH, empty HOME and fresh TMPDIR. All seven controls passed in 5.129 s. The final audits parsed 15 changed Python files, five shell files and the launch schema, resolved 366 source units and 449 test registrations, and found no differences from STATE A in the tracked C++ replay data, browser assets or OpenAI schema files.

The archived conversion commands, Mac package execution, target-host Arch deployment, live inference and rollback were outside local qualification. The full Release, sanitizer and fuzz matrix is assigned to automatic branch CI. No workflow was dispatched. The installed local review launcher is a Node script, so local model review was skipped under the job's runtime and provider-traffic rules. The checkout is clean. The build artifacts and logs are outside it.

## Summary

STATE A (`ca045a83`) merges the residual fixes and restores forwarding for unknown Chat roles and Responses tool types. Both oversized-model guards are present.

Removed the paired TypeScript comparison, Bun formatting, unused add-ons and offline conversion and proof commands. The branch ships the C++20 gateway, native tests, browser assets and licenses. The gateway opens the existing marked SQLite format directly and applies linked Landlock and seccomp before listening.

Build the ARM64 gateway and archive with `cpp/gateway/packaging/portable/build.sh`. Run focused Release tests with `cpp/gateway/tools/ci-build.sh --config release <filter>` and verify the archive with `cpp/gateway/packaging/portable/verify.sh`.

STATE A passed 61 binaries and 803 cases. The final focused Release run passed 45 binaries and 629 cases. Registration, whitespace and workflow checks passed. All four Debian package proofs passed, including integrity refusals and durable reopen.

The Arch cutover and Mac package remain unverified. The Linux package reports `unknown` for the production health identity. The existing public auth guide still points to the legacy upload tool.
