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

## Verification checkpoint

Dependency preparation built owner-pinned Boost 1.92.0 JSON and Container static libraries, ICU 78.3 and SQLite 3.54.0 with `SQLITE_DQS=3` in runner scratch. The host installed Clang 21.1.8 and the system OpenSSL 3, zlib, Brotli and Zstd development packages. Bun 1.4.0 was present. The Docker toolchain target prepared its pinned Bookworm libraries successfully. Test execution and portable package receipts are pending.
