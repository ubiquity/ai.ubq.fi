# C++ gateway branch cleanup

## Work contract v1

Remove the TypeScript and Rust gateways and the independent C++ pilot from the integration branch after relocating native build inputs. Keep the C++ gateway, replay fixtures, static assets, native CI and packaging, and an independently pinned Bun comparison. The change is confined to the branch's tracked files. Production, DNS, credentials, remote tags, provider traffic and unrelated gateway defects are outside the scope of this PR. Keep all native tests, delivery and accounting behavior, offline codec and artifact validation, and fail-closed fixture confinement. Use only local provider mocks in credential-free tests.

| Surface             | Acceptance                                                                                                                                                                                 |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Tracked source      | Ordered buildable commits for relocation, legacy removal groups and root rewrites. Report every move, retained root entry and tracked file and text-LF count.                              |
| C++ tests           | `python3 cpp/gateway/tools/links.py --check`, release shard 1/4 and focused runtime, static, KV, permissions, startup, passkey and relocated fixture tests from the cleaned checkout.      |
| Releases            | Build and verify the Linux ARM64 portable package using the moved addon sources. Keep the sealed artifact contracts and macOS package input layout.                                        |
| Paired comparison   | Fetch the exact legacy commit and use a detached worktree for Bun. Run the full local-mock comparison with an independently bound native package. Preserve existing failing expectations.  |
| Tooling and handoff | Check changed-file formatting, both native workflows with `actionlint`, and tracked paths and live source references with `git grep`. Deliver a stacked PR against the integration branch. |

The pilot's independence requires a path search and a reduced build. The cold release shard is expected to take about 35 minutes and the portable build about 30 minutes without competing jobs. Per-command logs, driver receipts and package stages show progress. If a command fails, inspect its retained log and retry only the failed gate where a recovery is available. Stop after collecting the named evidence or documenting an exact blocker. Hosted CI owns the full release, sanitizer and fuzz matrix.

## Starting point

- Integration commit `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`, tree `c41de1cd9476af1ff2e9ed958263096e9ba70f96`, on `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1`.
- Inventory tip `d6dd1c62` was fetched and merged first as `e205250f81797c2f2062eea96cfd22c5743087cd`, tree `93d9d5571a2c343435a33ebd229258b46194ef15`. The inventory adds two documents, both read before source changes. [Plan](../CLEANUP-PLAN.md) · [Inventory findings](cppgw-cleanup-inventory-20261118.md)
- Legacy oracle commit `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`, tree `d4f9c3beb10816358c85e92c1c231421fa35fa48`, is an ancestor of the integration branch.
- Runner scratch is `/var/tmp/cppgw-cleanup-removal.Vk5FzVNc`. The checkout is on Ubuntu 24.04.5 ARM64 with four CPUs and Docker 28.0.4. Build output and downloads go to scratch.
- The inventory observed two shared paired failures, `chat-invalid-role` and `responses-invalid-tool-type`. Both runtimes dispatched to a mock and answered 200 where the suite expects 400 and no dispatch. The expectations are retained here.

## Changes committed

- `6d32bb89` separated the Bun oracle from the native candidate. The historical commit was fetched successfully by hash. The candidate seal binds the packaged binary and the archive's build profile to the native commit and tree. The Linux KV input pin is compared byte for byte before both sides use the same independently bound addon.
- `3dc1f8c2` staged the 16 relocated inputs, switched CMake, packagers, production readers and fixtures to their new source paths, and added both PNGs to the host, portable and macOS packaging inputs. The installed V8 helper and KV binding layouts are preserved. The offline converter entrypoints require a sealed package through `prepareOfflineCli`.
- `fe503b11` replaced tests that read the legacy application with source scans of native dispatch and live catalog and refresh tests using local fixtures. `b9e8e578` updated the request-path, regex and standalone smoke harnesses to accept the pinned oracle. The small JSON benchmark uses the original one-line Bun config as a local fixture.
- The last tree with both gateways and all updated native consumers was commit `b9e8e57807f2a1f8632b8ff96ecc5ff3d7980d06`, tree `5a66ba76dfbf08aef6b022a7c6c83aebebcecfe2`. `git rm -r` removed the TypeScript application and old port tools in `cccc326d`, the Rust workspace in `a5404ab9`, legacy service and Worker config in `126651e3` and the top-level C++ pilot in `7cd55122`.
- `git ls-files cpp | awk '!/^cpp\\/gateway\\//'` listed 39 pilot paths before removal. `git grep -nE 'cpp/(include/pilot|src/|tests/|tools/|CMakeLists\\.txt|CMakePresets\\.json)|include/pilot|pilot::' -- cpp/gateway .github/workflows ':!**/*.md'` found no native build, test or package dependency. The native Clang-Tidy config is under `cpp/gateway` and opts out of inheritance.
- The native README and operator guides link to the historical source. The root manifest and lock now install only Prettier 3.9.6 with pinned Bun 1.4.0. The staged hook checks staged bytes. The native workflow adds changed-file formatting with an actual diff and keeps the existing release, sanitizer and fuzz jobs.

### Relocated files

| From                                                  | To                                                               |
| ----------------------------------------------------- | ---------------------------------------------------------------- |
| `src/runtime/permissions-native.c`                    | `cpp/gateway/packaging/confinement/permissions-native.c`         |
| `src/kv-conversion/v8-helper.mjs`                     | `cpp/gateway/runtime/legacy-codec/v8-helper.mjs`                 |
| `src/kv-conversion/typed-value.ts`                    | `cpp/gateway/runtime/legacy-codec/typed-value.ts`                |
| `scripts/build-native-kv.ts`                          | `cpp/gateway/packaging/native-kv/build-native-kv.ts`             |
| `scripts/openai-spec-pin.json`                        | `cpp/gateway/tests/fixtures/openai-spec-pin.json`                |
| `tests/fixtures/sentinel-historical-framing.json`     | `cpp/gateway/tests/fixtures/sentinel-historical-framing.json`    |
| `docs/research/cpp-pi-standby-baseline-2026-11-13.md` | `cpp/gateway/docs/history/cpp-pi-standby-baseline-2026-11-13.md` |
| `tools/native-kv/Cargo.lock`                          | `cpp/gateway/packaging/native-kv/Cargo.lock`                     |
| `tools/native-kv/DARWIN.md`                           | `cpp/gateway/packaging/native-kv/DARWIN.md`                      |
| `tools/native-kv/LICENSE`                             | `cpp/gateway/packaging/native-kv/LICENSE`                        |
| `tools/native-kv/README.md`                           | `cpp/gateway/packaging/native-kv/README.md`                      |
| `tools/native-kv/atomic-write-rollback.patch`         | `cpp/gateway/packaging/native-kv/atomic-write-rollback.patch`    |
| `tools/native-kv/inputs-darwin-arm64.json`            | `cpp/gateway/packaging/native-kv/inputs-darwin-arm64.json`       |
| `tools/native-kv/inputs.json`                         | `cpp/gateway/packaging/native-kv/inputs.json`                    |
| `tools/native-kv/lock-derivation.json`                | `cpp/gateway/packaging/native-kv/lock-derivation.json`           |
| `tools/native-kv/napi-workspace.patch`                | `cpp/gateway/packaging/native-kv/napi-workspace.patch`           |

The moved build script uses its adjacent pins and explicit artifact output and scratch paths. The Linux and Darwin addon notes and the Pi research note have updated instructions or links. The other 12 moved files preserve their bytes. The public `static/` tree and root PNGs also preserve their bytes.

### Retained root entries

| Entry             | Purpose                                                            |
| ----------------- | ------------------------------------------------------------------ |
| `.githooks/`      | Staged whitespace and format checks                                |
| `.github/`        | Native CI and ARM64 package workflows                              |
| `.gitignore`      | Private config, state, caches and build outputs                    |
| `.gitleaksignore` | Existing scanner exceptions, kept byte for byte                    |
| `.prettierignore` | Vendor and byte-pinned inputs excluded from formatting             |
| `.prettierrc`     | Shared formatting settings                                         |
| `AGENTS.md`       | Native development, behavioral contracts and service ownership     |
| `README.md`       | Gateway entry point and published provider and upload guidance     |
| `bun.lock`        | Frozen formatter-only dependency graph                             |
| `cpp/`            | Gateway, tests, vendors, tooling, packages and historical evidence |
| `favicon-32.png`  | The `/favicon-32.png` resource                                     |
| `favicon.png`     | The `/favicon.ico` and `/favicon.png` resource                     |
| `package.json`    | Format command and exact Prettier dependency                       |
| `static/`         | All 38 public asset and contract files                             |

The three remaining `.ts` files are the native addon build helper, the offline V8 codec's typed-value module and the import-free Bun JSON benchmark. CMake still consumes the native source paths in `PORT-MANIFEST.json`. `PORTED.txt` preserves the historical inventory, and the retired manifest and port-assignment generators remain at the legacy pin. The auxiliary addon builds an upstream locked Cargo package independently of the removed first-party Rust workspace.

## Tracked size

`git ls-tree -rz <revision>` counted tracked entries. The `git cat-file --batch` measurement counted literal LF bytes in text blobs, excluding symlinks, the submodule gitlink and blobs containing NUL. The supplied integration branch includes later code and vendors beyond the inventory's earlier baseline.

| Tree                                 | Tracked paths | Text lines |
| ------------------------------------ | ------------: | ---------: |
| Supplied integration head `aae39d0e` |         4,276 |  1,699,773 |
| With the inventory merged `e205250f` |         4,278 |  1,700,285 |
| Cleaned tree with this report        |         2,041 |  1,014,800 |

The last tree before the source relocation was `6d32bb895dafdb8095ba0cd167d7d2a5cf1dfc2c`, tree `c8dd6f0ad85246a4325774edfcdbca4fbb7a2cb0`. The removal commits deleted 1,173 TypeScript-era and port-tool paths after copying inputs, 977 Rust paths, 68 legacy operations and workflow paths, and 39 pilot paths. Their effects include the moves listed above.

## Cleaned Linux ARM64 verification

The release tests and portable package used this checkout at commit `87a42abfe5bb810e0a5cfccb7f5e73feb3d5e3dc`, tree `de3f395c6fb6a3a1861f278d2311c4f9f0126a86`. The follow-up commits contain documentation and this report. All builds and artifacts used `/var/tmp` outside the checkout.

The runner installed Clang 21.1.8 from the signed LLVM Noble repository and used `sudo -n apt-get install -y --no-install-recommends ninja-build pkg-config libssl-dev zlib1g-dev libbrotli-dev libzstd-dev libicu-dev mold util-linux gpg libnode-dev patch`. The upstream archive checks matched the pins in `packaging/portable/dependencies.sh`. Boost JSON and Container were built static with `./bootstrap.sh --with-libraries=json,container --with-toolset=gcc` and `./b2 --ignore-site-config --user-config=/dev/null -j2 toolset=gcc --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 cxxflags=-march=armv8-a stage`. ICU 78.3 used `CC=gcc-14 CXX=g++-14` and `./configure --prefix="$scratch/icu78" --enable-shared --disable-static --disable-tests --disable-samples --disable-extras`, followed by `make -j2` and `make install`. SQLite 3.54.0 used `CC=gcc-14 CFLAGS='-O2 -DSQLITE_DQS=3' ./configure --prefix="$scratch/sqlite" --disable-static --disable-readline`, `make -j1` and `make install`.

Each release invocation used this environment with new `home` and `tmp` directories. The driver proved real network, mount and PID isolation via sudo, dropped to the runner uid, and verified SQLite 3.54.0 within the boundary.

```bash
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin HOME="$check/home" TMPDIR="$check/tmp" \
  LC_ALL=C.UTF-8 LANG=C.UTF-8 TZ=UTC CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4
```

`python3 -B cpp/gateway/tools/links.py --check`, the vendor source check, and `python3 -B cpp/gateway/tools/cmake-sources.py "$scratch/receipts/sources.cmake"` passed. `bash cpp/gateway/tools/ci-build.sh --list` and `ctest --test-dir "$scratch/cmake" -N` each found 450 tests. `UOS_GATEWAY_BUILD_EVIDENCE=cleaned-tree-configure UOS_GATEWAY_TOOL_ROOT="$scratch" cmake -S cpp/gateway -B "$scratch/cmake" -G Ninja -DUOS_GATEWAY_TESTS=ON` registered all 450 with zero skips and resolved 325 manifest modules plus 44 runtime units. The release driver compiled or reused all 369 production objects in every accepted receipt. The pre-removal cache warm-up compiled those objects and passed 19 startup cases, and its receipt correctly rejected acceptance because the checkout changed during that run.

| `ci-build.sh --config release` argument | Binaries passed | Cases passed | Driver seconds |
| --------------------------------------- | --------------: | -----------: | -------------: |
| `--shard 1/4`                           |             113 |        1,333 |        611.125 |
| `tests/rt/`                             |              37 |          479 |        129.921 |
| `runtime_`                              |              18 |          166 |         87.801 |
| `static`                                |               2 |           39 |          1.605 |
| `kv`                                    |              24 |          403 |         75.503 |
| `storage_value_compatibility`           |               1 |            4 |          4.944 |
| `permissions`                           |               2 |           11 |          4.455 |
| `serve_`                                |               3 |           28 |         15.999 |
| `passkey`                               |               6 |           74 |         24.663 |
| `sqlite_readonly`                       |               1 |            6 |          5.640 |
| `native_addon_differential`             |               1 |            4 |          0.807 |
| `openapi_runtime_contract`              |               1 |           11 |          1.923 |
| `admin_models_catalog`                  |               1 |           33 |          1.555 |
| `sentinel_upstream_providers`           |               1 |           19 |         21.467 |
| `openai_spec_drift`                     |               1 |            1 |          0.896 |

The focused filters passed 92 distinct binaries and 1,171 cases. Including shard 1/4, the union passed 184 distinct binaries and 2,194 cases. All 15 accepted receipts recorded matching before and after source SHA-256 `21add12b59389e361a157cfab405ec92e2f4eb4e7f7429b0ac2e5acc4bcb1b31`. The shard spent 571.748 seconds building objects and 38.855 seconds linking and running tests. Its receipt is `receipts/release-shard-1.json`, SHA-256 `2b27d0f3188eeb7472fc940a98abd974a35cdc81b32c9b2de3525e20b94b08f8`. The temporary route test added an out-of-table method for an existing path and a separate unpublished `/uos/` literal under native `src/handler/`. Exactly the two relevant cases failed. The control was removed before the clean rerun passed all 11 cases.

## Portable package and paired comparison

Using `env -i`, fresh home and temp directories, and the scratch Docker config, I ran `PORTABLE_JOBS=4 bash cpp/gateway/packaging/portable/build.sh "$archive"` and `bash cpp/gateway/packaging/portable/verify.sh "$archive" "$scratch/portable-final/verify"`. The first relocated-input package built in 1,243.53 seconds. The exact-tree rebuild took 277.05 seconds with C++ cache hits. `sha256sum --check SHA256SUMS` passed, and `VERSION` and the payload's build profile both named the tested Git commit and tree.

| Artifact or receipt                | SHA-256                                                            |
| ---------------------------------- | ------------------------------------------------------------------ |
| Portable archive, 83,772,818 bytes | `331187ceb33afae8cc109c3c5df22bb053844e0433558f34536fa7b58f63c3bd` |
| Sealed gateway binary              | `d7a6bac82bc8ffc407d6bbec348727b5c3d974add803c304bd88d31598f797f0` |
| Host-sealed package manifest       | `9fc53202c9c3ce8fd4ed5265088cf91b21fcd5ee5f3272dc07f79c723e9f9ebb` |
| KV addon                           | `b792c0d0044cb77c9c576192037f5c05408ec6323ea71b7d73e3749d7d9b008d` |
| Bound KV provenance                | `b27aae214e855f7ebb3f1825b5a57d1f72fc2c6be0fca525ebc25b0b02f1f2e3` |
| Confinement addon                  | `f6d3f288e2183250c06e7987c7885a8b5837aae6499edde5461e7e8b3e1b5f6e` |
| Packaged offline KV proof          | `0912845a3afde5f4ab9b825ac0eb6cffa13d855ca889d8b40b9eb77bde52adcd` |
| Packaged startup refusal controls  | `3d98535ee6471fe5f39a7a0c12812286bb40e12c5e07665ffc823cb14726dae7` |
| Paired results                     | `018991ebaeb2df5597da224f4d968d57a154c3706d552c7ec725a62d929a1b4c` |

The portable verifier exited 0 in 55.09 seconds. Both Debian releases passed at 256 and 512 MiB. Each run served and accounted for 1,161 mocked Chat requests, read back the same count after restart, installed two seccomp filters with Landlock ABI 8, and refused unauthenticated Chat with HTTP 401. Cgroup peaks were 38,150,144–38,875,136 bytes with zero OOM events. All three favicon routes returned HTTP 200 and `image/png` with the hashes of the tracked PNGs. The validator also compared the two sealed PNGs to the checkout with `cmp`.

I sealed the verified archive on the runner with `python3 -B "$work/bundle/seal.py" "$work/package" > "$work/seal-receipt.json"`. In new OS namespaces, the additional `python3 -BI "$scratch/package-refusals.py" "$work"` run passed 12 actual startup refusal controls, including freshly rebound copies missing each moved codec file or native addon. The synthetic round trip ran `"$work/package/bin/uos-kv-proof" --runtime-manifest "$work/offline-manifest.json" "$work/offline-roundtrip" "$scratch/pinned-deno/deno" 5ecc9a6b862d61d4cf8cae3ac5f29488ffc839d675cfd60c11d908121dfe5bf1`. It exited 0 in 1.90 seconds with 10 forward rows and 11 reverse rows, successful Deno latest-state readback, and refusal of interrupted publication and unsupported fixtures.

`BUN=/home/runner/.bun/bin/bun bash cpp/gateway/conformance/run.sh "$work/paired" "$work/seal-receipt.json"` used the exact pinned detached Bun checkout and the independent native seal. It ran 162 cases before restart and 24 afterward in 12.05 seconds total. Both IPv4 and IPv6 outside routes were denied with `ENETUNREACH`. Of 186 cases, 184 matched and passed. The two pre-existing shared failures, `chat-invalid-role` and `responses-invalid-tool-type`, returned 200 and dispatched `deepseek.success` in both runtimes, where each case expects 400 and no dispatch. The suite exited 1 with no differences between implementations. All nine `python3 -B cpp/gateway/conformance/compare_test.py` controls passed.

## Source audit and handoff

- `git ls-files` found zero removed root paths, zero Rust workspace entries and zero C++ pilot entries. `git diff --name-only --diff-filter=D aae39d0e HEAD -- 'cpp/gateway/tests/*.cpp'` was empty. The Links registry, source manifest, public asset tree, root PNGs and `.gitleaksignore` matched the integration head byte for byte. All test inputs that need historical TypeScript now use the pinned oracle or preserved fixtures.
- `git grep -nE 'cpp/(include/pilot|src/|tests/|tools/|CMakeLists\.txt|CMakePresets\.json)|include/pilot|pilot::|src/runtime/permissions-native\.c|scripts/(build-native-kv|lint|format)\.(ts|sh)|scripts/openai-spec-pin\.json'` against the native build, tests, hook and workflows found zero obsolete checkout lookups. Searches for `tools/native-kv`, `ops/runtime-bunfig.toml` and the old smoke launcher found only paths within the verified oracle in active tooling. Historical references and the installed `src/kv-conversion` payload path are retained.
- `bun --no-env-file install --frozen-lockfile --ignore-scripts --backend=copyfile` installed only Prettier 3.9.6 in scratch from the format lock. `bun --no-env-file --no-install run format:check -- <changed paths>` and `git diff --check aae39d0e HEAD` passed. The hook rejected misformatted staged bytes and passed the corrected Markdown at a staged path containing spaces. ShellCheck and Python parsing passed on the changed tooling.
- `actionlint -config-file "$scratch/receipts/github-runner-labels.yaml" .github/workflows/cpp-gateway-ci.yml .github/workflows/cpp-gateway-release.yml` passed with ShellCheck. The scratch config registers `ubuntu-26.04-arm` because actionlint 1.7.12's built-in labels predate that GitHub-hosted runner. [Integration lint evidence](cppgw-integrate-f2-20261118.md)
- Publication is blocked in the runner. `GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-cleanup-removal-20261118` exited 128 because HTTPS had no username or write authentication. The local commits stack on `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1`. [Integration PR #1383](https://github.com/ubiquity/ai.ubq.fi/pull/1383)
- Pending evidence includes the hosted release, sanitizer and fuzz matrix, the macOS ARM64 package, and the optional host-package and performance harness runs. Production rollout and qualification use the separate operator process.

## Summary

Removed the TypeScript and Rust gateways, legacy deployment tooling and the separate C++ pilot. Kept the native gateway, all 450 test registrations, public assets, and the build and codec inputs the package needs. The Bun comparison fetches the pinned legacy commit into a detached worktree.

The tree went from 4,276 to 2,041 tracked paths and from 1,699,773 to 1,014,800 text lines. Linux ARM64 release shard 1/4 passed 113 binaries and 1,333 cases. With the focused checks, 184 distinct binaries and 2,194 cases passed. The Debian 12 and 13 package checks, offline KV round trip, and workflow lint also passed.

The paired comparison passed 184 of 186 cases and reproduced two existing validation failures in both gateways, with zero differences between them. GitHub write authentication blocked publication of the stacked PR. The hosted matrix and macOS package checks are pending.
