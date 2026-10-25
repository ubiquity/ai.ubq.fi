# Rust quality cleanup contract

Version: 9, source-candidate assembly and atomic Worker scope recorded 2026-10-25; earlier version sections below retain their historical provenance. Controller: active goal thread `01a16a00-7860-7440-8727-61bb36ebeefa`. Resume source: `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/HANDOFF-2026-10-23-rust-quality-wave2.md`.

## Outcome and invariants

Complete the recorded Rust quality cleanup: parser and selected Clippy enforcement, removal of re-exports, path-spliced modules and suppressions, compliant crate splits, and measured compile-performance evidence. Preserve all runtime, provider, credential, accounting, confinement and transport behavior. No production deployment, credential operation, provider spend or manual CI run is authorized by this contract. Keep the production release profile unchanged until the complete Worker build comparison exists.

All first-party production crates and separate test targets must ultimately fit the 1000-line cap. Existing debt may only shrink. New test-target accounting may adopt measured existing debt from immutable base `7223352101f448dd2ad46e40e285e689a2788435`, with generated provenance; it may not grandfather new changes. Parser and Clippy baselines must fail closed and never accept an upward update. No re-exports, path attributes, include macros, allow or expect suppressions may be added.

## Canonical lane and authority

Canonical integration lane: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/rust-wave2-integration-20261023`, branch `codex/rust-wave2-integration-20261023`, base `7223352101f448dd2ad46e40e285e689a2788435`, resumed head `7d918a8f166626eb7ecc53c9531126f8f8cc6834` containing completed core tip `c11ed2cbc62073fb9a3ce9389c7fe3ba0e8c4e83`. Existing lane identities are retained, not recomputed. Root `development` and `origin/development` were refreshed and verified at the base on resume. Preserve all root untracked files and unrelated PRs.

Applicable root AGENTS.md SHA-256: `4d669c87131254fb21653eafb3cb8ab15b4326dda3b24035f2680c52c8549897`. Model-routing revision: `1977fdc131130fed3dcb40f920d56da5839328f7e22f230b0ba85852d10476ef`. Delegation revision: `32bff32f358c781e3653389cd915e739c0c6e05c9325920eaa7fc25c9ff3b6bc`. Assigned workers use `gpt-6.1-sol`, max; actual model and effort are unknown without trusted metadata. Maximum total concurrency is eight including controller; children require separately recorded lanes.

## Module ownership

All paths below are relative to `.codex-worktrees/`; worker branches are `codex/<lane>`. Every interrupted cleanup lane starts at the exact base above and retains its preserved partial edits. Workers may commit only owned paths, never push, merge or dispatch CI. Controller integrates with merge commits; one integration writer repairs cross-crate callers after the cleanup lanes land.

| Module           | Worker                  | Lane                                       | Owned surface                                                                                                                                                                            |
| ---------------- | ----------------------- | ------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| m01-parser       | parser_rules            | rust-linter-crate-20261023                 | New gateway-quality crates, root Rust manifests/lock, Clippy settings, quality baseline/scripts/tests/docs, Rust verification scripts and necessary CI quality wiring                    |
| m02-authority    | authority_cleanup       | rust-wave2-authority-20261023              | gateway-authority                                                                                                                                                                        |
| m03-schema       | auth_schema_cleanup     | rust-wave2-schema-20261023                 | gateway-schema, including genuine removal of the argument-count suppression                                                                                                              |
| m03-auth         | auth_schema_cleanup     | rust-wave2-auth-20261023                   | gateway-auth, after schema handback                                                                                                                                                      |
| m04-native       | native_cleanup          | rust-wave2-native-20261023                 | gateway-native                                                                                                                                                                           |
| m05-worker       | worker_cleanup          | rust-wave2-worker-20261023                 | gateway-worker and exact existing observability test-support runtime extraction/manifests                                                                                                |
| m06-providers    | providers_admin_cleanup | rust-wave2-providers-20261023              | gateway-providers                                                                                                                                                                        |
| m06-admin-data   | providers_admin_cleanup | rust-wave2-admin-data-20261023             | gateway-admin-data; preserved gateway-admin-runtime overlap awaits comparison                                                                                                            |
| m07-misc         | misc_cleanup            | rust-wave2-misc-20261023                   | gateway-admin-runtime, admin, catalog-runtime, claude-runtime, extras-runtime, migration and worker-fixture                                                                              |
| m08-integration  | worker_cleanup          | rust-wave2-integration-20261023            | Sole canonical source writer after accepted cleanup tips; defining-module callers, CatalogIdentity callers, approved helper lockfile edges, genuine core nesting fixes and this contract |
| m10-verification | misc_cleanup            | rust-wave2-integration-20261023, read-only | Final selected gates after the controller freezes an exact clean commit and tree; no source mutations                                                                                    |

Version 1a authorizes moving the parser CI step to prepared Rust jobs while preserving all workflow triggers and docs-only behavior. Version 1b authorizes worker dev dependencies on existing routing and observability test-support crates and the exact observability fixture runtime extraction; the parser worker retains ownership of the root Cargo.lock integration.

Version 1c adds `gateway-quality` and `gateway-quality-syntax` to the existing `RUST_NATIVE_ONLY_PACKAGES` classifier and excludes those tool packages from WASM target package flags. Every native and WASM Rust plan runs the parser natively before the selected package commands, even when parser source is unchanged. Existing affected-test selection remains intact; changed quality or selector TypeScript receives its own focused tests, lint and type checks.

New test-target baseline entries may adopt only measured debt already present at immutable base `7223352101f448dd2ad46e40e285e689a2788435`. The adoption generator and its retained provenance must bind that base, source inventory and measured target closures so the adopted counts can be reproduced. No current-tree growth, new violation or upward update is grandfathered by adoption. The parser worker owns those generated baseline and provenance artifacts; integration preserves that ownership until its exact accepted tip lands.

The controller explicitly transferred sole canonical mutation authority to m08 after cleanup integration. M08 may merge a future exact ready tip only after a separate controller acceptance message, retains every accepted worker ancestry, and returns the final clean source commit before m10 starts frozen-candidate validation. M08 owns only the approved three helper dependency edges in the root lockfile before parser integration; no dependency version upgrade is authorized.

Version 1e additionally authorizes the direct `gateway-admin-data.workspace = true` normal dependency in `gateway-worker/Cargo.toml` and its matching root Cargo.lock edge. The Worker implements `gateway_admin_data::profile::SourceDateHost` from its defining crate after the migration forwarding alias is removed. No package version or workspace member changes are authorized by this extension; the parser lockfile owner receives the exact edge before integration.

## Version 2 fixture boundary

The controller approved four cohesive dev-only helper packages for admin, Claude runtime, Extras runtime and migration fixtures, plus exact public authority and catalog fixture reuse in existing `gateway-routing-runtime-test-support`. Their exact lanes, owners and base commits are recorded by the controller after the canonical checkpoint, before any helper writes begin. The single integration writer retains root manifest, lockfile and classifier coordination; helper workers own only their explicitly assigned source surfaces.

Move actual shared fixture implementations while preserving every test case name, golden input, opaque capability and production proof boundary. Preserve existing helper item/function tokens on relocation and use explicit defining-module imports that keep current local binding names. A real dev-only fixture API may expose data fields or a trivial public wrapper over an unchanged private implementation; no production visibility or compatibility facade is authorized. No re-export, path attribute, include macro, source suppression, baseline increase or staged fingerprint waiver is authorized.

Keep the ranked catalog `FixtureCollator` in the new admin helper to retain headroom in the existing routing helper. The admin and Extras helpers remain portable. The Claude and migration helpers retain their existing pinned native SQLite dependency contracts and join `RUST_NATIVE_ONLY_PACKAGES`; only the corresponding classifier entry and its focused tests change. New helper normal dependencies and parent dev-dependency edges must use existing selected versions and features. No application runtime dependency or version upgrade is authorized by these extractions.

## Execution and acceptance

Workers return exact base, head and tree, changed paths, original-to-defining-module API mappings, meaningful focused checks and test-evidence references, residual defects and settled jobs. Controller inspects status, diff summaries and diff checks before accepting each tip. Disjoint source work runs concurrently. Rust compilation is serialized with the existing `/tmp/ai-rust-wasm-20261018/mac-tooling.lock` inode, opened with Python `fcntl.flock(LOCK_EX)`, using the existing pinned `mac-rustup` and `mac-cargo` Rust 1.98.1 toolchain and at most two jobs. No cargo/rustc/clippy processes or lock holders existed when this protocol was reconciled.

Before every gate, map changed paths to the smallest checks. After writers settle, run parser quality, Rust formatting, selected strict Clippy and affected tests on the exact integration tree, using the supported Mac runtime fixtures and serial native tests. Build native bins/examples before native fixture tests. Bind every final receipt to the exact commit/tree; source edits invalidate earlier receipts. Cross-cutting workspace changes require the corresponding combined Rust gates, without unrelated TypeScript suites. Parser tests must exercise inactive and inline modules, recursive cfg_attr, qualified macros, malformed syntax and baseline growth rejection.

Then push each accepted task tip and integration branch, open a bounded PR to development, and merge with ancestry preserved after selected local gates. Use only automatic post-merge hosted CI as its hosted receipt; never claim it as pre-merge evidence. Local review uses actual codex review mode asynchronously and does not gate delivery. Finish by fetching origin/development, proving every accepted tip is its ancestor, proving local development matches it, and leaving the root tracked checkout clean. The goal remains active until all recorded structural debt and required evidence are resolved.

## Version 5 resumed execution

Objective: finish the preserved staged-lint cleanup and its proof artifacts without changing gateway behavior. The controller owns architecture, acceptance, push, pull requests, merges into development and final delivery; m08 is the sole canonical source writer. No production service, deployment, authentication, provider-credit, manual CI, dependency-version or release-profile change is authorized. Every earlier candidate receipt is stale for a later source edit.

The resumed clean canonical source is commit `87bef7d99b4945f1950577316bcbabc766714269`, tree `6dc7a5e25409add65e53d95de2440229b71a7710`, with historical base `7223352101f448dd2ad46e40e285e689a2788435`. The fresh upstream checkpoint is `3393df1e78630d3503ffb1e7659f9a133b705b07`; its eleven changed paths have no Rust, manifest, lockfile, script or CI overlap. No pull request exists for the canonical branch. Keep the resumed base stable until all three current lanes commit; the controller authorized the eventual ancestry-preserving upstream merge after that point. Root development remains separately owned and must not be changed by m08.

| Module                      | Owner                      | Preserved lane                                                                | Exact resumed base                       | Owned surface                                                                                                                 |
| --------------------------- | -------------------------- | ----------------------------------------------------------------------------- | ---------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| m20-staged-auth-schema      | resume_auth_schema         | handoff-2026-10-23-rust-quality-wave2-m20-staged-auth-schema-a148d8e479a      | 87bef7d99b4945f1950577316bcbabc766714269 | Existing dirty gateway-auth and gateway-schema lint fixes                                                                     |
| m21-staged-authority        | resume_authority           | handoff-2026-10-23-rust-quality-wave2-m21-staged-authority-a78a4a6b418        | 87bef7d99b4945f1950577316bcbabc766714269 | Existing dirty gateway-authority lint fixes                                                                                   |
| m22-staged-provider-routing | resume_provider_routing    | handoff-2026-10-23-rust-quality-wave2-m22-staged-provider-routing-a7ffff1c7e8 | 87bef7d99b4945f1950577316bcbabc766714269 | Existing dirty gateway-providers and gateway-routing-runtime lint fixes                                                       |
| m08-integration             | integration_resume         | rust-wave2-integration-20261023                                               | 87bef7d99b4945f1950577316bcbabc766714269 | Contract, exact approved merges, attributed Catalog/admin-runtime imports, required frozen dependency edges and this contract |
| m23-adoption-repro          | adoption_repro             | handoff-2026-10-23-rust-quality-wave2-m23-adoption-repro-a33b18d6b30          | 87bef7d99b4945f1950577316bcbabc766714269 | Existing scanner inventory API, immutable-source Bun reproducer, focused tests and dedicated usage documentation              |
| m24-authority-fixtures      | quality_acceptance_prepare | handoff-2026-10-23-rust-quality-wave2-m24-authority-fixtures-a184fe6c0b4      | 87bef7d99b4945f1950577316bcbabc766714269 | Eight inline Authority test-fixture files transferred from m21                                                                |
| m25-auth-session-fixtures   | bounded_split_discovery    | handoff-2026-10-23-rust-quality-wave2-m25-auth-session-fixtures-aae59323a7e   | 87bef7d99b4945f1950577316bcbabc766714269 | Four Auth session helper and caller files transferred from m20                                                                |

All lane paths remain below `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/` with their existing `codex/` branches. No child, new lane, push, pull request or merge is implied for m08. Integrate an exact worker tip only after the controller separately accepts it. Preserve unrelated dirty paths, every test case, source-bound capability, original provider/credential/accounting/transport behavior and all accepted ancestry. The remaining large-crate debt remains mandatory migration debt; a passing no-growth check is not full compliance.

The former `/tmp/ai-rust*` and `/tmp/codex-agent-tasks` artifacts are absent and do not count as current evidence. One shared preparation at `/tmp/rust-quality-resume-20261024/prepared` contains APFS clones of the installed Rust 1.98.1 toolchain and cached Cargo registry; no install or download was used. The compiler is `prepared/rustup/toolchains/1.98.1-aarch64-apple-darwin/bin/rustc`, commit `48a229ceaefd4985c50990b14116b6d856af0985`; Darwin and wasm32 targets are installed. Pinned Bun 1.4.0 is `/Users/nv/repos/ubiquity/ai.ubq.fi/.data/runtimes/bun/darwin-arm64/1.4.0-539598c775882420b9d8deb7dc14d845f20f7d26f5600c50ab067dde6ac3f3bf/bun`, with SHA-256 `539598c775882420b9d8deb7dc14d845f20f7d26f5600c50ab067dde6ac3f3bf`; ambient Bun 1.3.3 is prohibited.

Source the task-owned `compiler-env.sh` in a non-login shell and serialize every Rust compiler and parser-to-comparator sequence with `/usr/bin/python3 /tmp/rust-quality-resume-20261024/compiler-lock.py COMMAND...`. Its persistent `/tmp/rust-quality-resume-20261024/compiler.lock` inode is `477819262`; never unlink or replace it. Use at most two jobs, frozen offline dependencies, the committed `rustVerificationEnvironment`, `rustToolPath` and `runCommand` with `isolated: true`. The existing `scripts/test-network.ts` Darwin Seatbelt profile and IPC lease guard provide the fixture-network boundary; no replacement wrapper is needed. Each lane now uses a separate target directory; never reuse first-party cached units across worktrees. Retain unique per-attempt diagnostic JSON and source context.

Fresh parser `--check` at the resumed source failed because old structural allowances had shrunk, not because debt grew. For interim diagnostic context generation only, the committed parser's read-only `--report` is permitted; it is never reported as a passing structural gate. Tighten through the supported updater after writers settle, never edit allowances upward or waive a changed source-context fingerprint. Formatting is limited to two passes per changed file per edit round.

The controller authorized a minimal adoption reproducibility repair: deterministic first-party Bun tooling must export immutable source `7223352101f448dd2ad46e40e285e689a2788435`, reuse the current pinned Rust scanner, retain sorted per-source physical-line and per-target reachable-source inventories, and compare the fourteen recorded target allowances without rewriting them. A small scanner inventory output may expose its existing traversal result; do not implement a second module scanner. Bind fresh reproduction to source/tool commit, tree, binary hash, compiler identity, command and complete output. New receipts are fresh reproduction, never recovered historical captures. Historical Clippy diagnostic/capture hashes and receipts remain unchanged; report any unreproducible binding before attempting broad recapture.

Acceptance maps the changed proof tooling to focused scanner and Bun tests, changed-file formatting/lint and strict TypeScript checking; attributed Rust imports receive focused package Clippy/comparator and affected tests. After all writers settle and accepted tips integrate, an independent worker validates one frozen clean commit/tree with the selected native/WASM gates and supported native fixtures. Stop a unit at a reproducibility mismatch, unsafe behavior change, new debt or missing required artifact and return exact evidence rather than guessing. The controller reconciles the final source, remaining debt, receipts, CI and ancestry before declaring readiness.

The controller transferred adoption implementation exclusively to m23 after m08 settled all proof-tooling writes; no proof-tool source change existed at transfer. M20 additionally owns remediation of the exact approximately twenty-six Auth/Schema contexts rejected by its fresh staged comparator after import cleanup; preserve every test and source proof boundary, and request a bounded transfer before editing a caller outside those crates. M08 owns the five freshly observed Catalog wildcard imports in admin, control, state and versioned, with defining-module bindings and no physical crate growth. Other external rejected contexts require owner attribution before repair.

Fresh Cargo metadata can rewrap cached first-party warnings from another worktree as the current package even when an emitted non-test library artifact is `fresh: true`. M21 proved this with one successful, truncate-written JSON stream, an m22 depfile/fingerprint and stale library spans alongside a freshly compiled current test artifact. All staged comparator receipts/maps from the former shared target are invalidated; do not repair source to match them. M21 continues its already-running fresh target `prepared/targets/m21-a78a4a6b418`. M20, m22, m23 and canonical use fresh empty `prepared/lane-targets/m20-a148d8e479a`, `m22-a7ffff1c7e8`, `m23-a33b18d6b30` and `integration-20261024`, respectively. These paths are relative to `/tmp/rust-quality-resume-20261024/`; the installed toolchain/registry and compiler lock remain shared. No first-party cached artifact was copied, deleted or accepted by this correction.

Pinned rustfmt on an explicitly owned changed-file set may run directly in a disjoint lane while that lane has no compiler. It need not wait on the compiler or global evidence lock; retain the exact command, complete log, exit and source identity, with at most two passes per file per edit round. This exception changes no compiler serialization, source freeze or final evidence requirement.

The controller assigned two disjoint fixture writers at the same resumed base, bringing total concurrency to eight including the controller. M24 exclusively owns these paths under `rust/crates/gateway-authority/src/`: `native_control/actor/tests.rs`, `native_control/actor/tests/live_manual.rs`, `native_control/actor/tests/native_import.rs`, `native_control/actor/tests/native_import/effects.rs`, `native_control/actor/tests/native_import/custody_session.rs`, `native_control/actor/tests/renewal_v2.rs`, `paid_backfill/tests.rs` and `paid_backfill/tests/source_control.rs`. M25 exclusively owns `rust/crates/gateway-auth/src/passkeys/session_index_cases.rs`, `rust/crates/gateway-auth/src/passkeys/session_index_cases/support.rs`, `rust/crates/gateway-auth/tests/session_index.rs` and `rust/crates/gateway-auth/tests/support/mod.rs`. Parent workers retain their remaining owned paths and exchange fresh maps directly; no concurrent writer may edit a transferred path. Each fixture unit preserves every test case, capability boundary and defining-module contract, with no production-crate growth, suppression, allowance increase or unrelated manifest change.

M24 and m25 use fresh empty targets `/tmp/rust-quality-resume-20261024/prepared/lane-targets/m24-a184fe6c0b4` and `m25-aae59323a7e`. Their ignored root and lint `node_modules` links were prepared only after byte equality of all four package/lock manifests; the shared Bun declaration patch already matched `ff1269afd7044fffcec0ce8879645702ec0013c9990232e05644f1493e3dc4db`, so hook preparation remains read-only and idempotent. No install or lock generation occurred.

Published adoption inventories keep their proper `.json` names and the existing Prettier formatting. Retain the exact raw bytes and SHA in task artifact custody, and bind each formatted publication by raw and published hashes plus decoded equality proof. Do not disguise JSON as text, add a formatter exclusion or introduce a runtime formatter dependency. Historical adoption identities remain unchanged; all new proof is explicitly fresh reproduction.

A separately authorized build-time Mac WASM preparation may use only task-local tools and current pinned versions. The current committed CI preparer refuses Darwin WASM and its prebuilt URLs are Linux-only. The prepared cache already contains worker-build 0.8.7 archive SHA-256 `1051c05ef088889a30ad3bad40c22a716ba8ca8d02a3fb01cef8c143ba952222` with embedded lock SHA-256 `f87aa44cf93c10023250154e0742d73af880295e38b5464853ecc18a4da30047`; registry index and archive match. Proposed Mac prebuilt inputs, observed from exact official release asset digests on 2026-10-24, are wasm-bindgen 0.2.129 aarch64-apple-darwin archive SHA-256 `81d4a23d56b3c3eb8187658329116d50e0b228a93b343825fb71f70179051cd1` and Binaryen 132 arm64-macos archive SHA-256 `98aad827847af7ef990ed7098d885725c8e5b5aae75073403635617ae4e259aa`. The esbuild 0.28.2 darwin-arm64 package remains bound to its existing root bun.lock SHA-512 integrity. Offline `cargo install --locked --offline --jobs 2 --version =0.8.7 --root TASK_TOOLS worker-build`, hash-verified prebuilt extraction, whole-distribution manifests and existing tool version/realpath gates are the concrete proposal; no preparation, download, install or change to the committed preparer is authorized by this record alone. Missing cached build dependencies fail closed without an automatic online retry.

Version 5 expands the staged-lint remediation to the exact owned rejected function/test/helper contexts confirmed by fresh private captures, including the explicit m24/m25 ownership transfers above. The provider/runtime matrix and all security, credential, accounting, confinement, transport, source-proof and no-growth invariants are unchanged; no allowance, context fingerprint or suppression is waived. M20 fresh private receipt `591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/8a97b929-6fc2-4387-aa0d-95e157a44f2e` reproduced the twenty-six Auth/Schema rejected hashes after a successful strict Cargo run; it is attribution before remediation, not acceptance afterward. Every affected source edit makes its prior source/test receipt stale. Every build or staged-comparator receipt from the former shared first-party target is invalid for acceptance, even when Cargo returned zero; preserve those records as diagnosis only. Final acceptance requires one newly frozen clean version-5 commit/tree and matching selected receipts after integration.

## Version 6 compiler target ownership

The controller approved a small verifier behavior change owned only by m08: `scripts/rust-target-owner.ts`, its focused test file and the actual launch call in `scripts/rust-verify.ts`. Before workspace compiler execution, resolve the actual manifest/workspace source and effective target, including documented separate and equals-form `--manifest-path` and `--target-dir` overrides. An atomic target-owner marker binds the canonical source root; subcrate manifests in one workspace share that owner, immutable archive sources require no Git directory, and same-root edits may retain their cache. Foreign, malformed, symlinked or unowned nonempty first-party targets fail closed before compiler spawn. No source-hash namespace, new environment/CLI knob, adoption bypass, deletion, dependency or production behavior change is authorized. Add focused actual filesystem/race/launch-refusal tests, existing verifier tests, changed-file format/lint and strict TypeScript checks. Keep `rust-verify.ts` below 1000 lines. This driver change invalidates affected earlier driver acceptance receipts; workers continue their already-frozen old drivers on proven private targets, and final acceptance uses a newly frozen version-6 source/driver tree.

Separately approved build-time tool preparation may fetch only the three exact hash-pinned Mac archives above and build cached worker-build 0.8.7 offline with its embedded lock, at most two jobs and the actual fixture-network boundary under the shared compiler lock. Downloads/hash checks are independent; tool compilation follows ready source checks. No online retry, app profile/dependency/global/home/service/runtime-startup change or confinement bypass is authorized. The three archives were freshly downloaded and matched their pinned hashes; extraction retains whole distributions and file hashes. Version gates and worker-build compilation remain separately required.

For the next bounded unit after this delivery, scope normal commit hooks through the existing changed-surface selectors and required gates: Rust-only commits currently spend several minutes on whole TypeScript checking and Cloudflare type generation. This is a recorded follow-up only; current workers retain normal hooks, and no hook file, gate, check, or prior acceptance is suppressed by this contract. The full matching installed graph is `.codex-worktrees/rust-linter-crate-20261023/node_modules`; the root graph was incomplete. Task links were recovered only after all four package/lock comparisons and exact cf/wrangler/Bun declaration checks, preserving the root graph without installs or version changes.

After accepting m25, the controller reassigned bounded_split_discovery to m26-native-publication-lints at the preserved base `87bef7d99b4945f1950577316bcbabc766714269`, lane `handoff-2026-10-23-rust-quality-wave2-m26-native-publication-lints-ab1d6f6f364`, branch `codex/handoff-2026-10-23-rust-quality-wave2-m26-native-publication-lints-ab1d6f6f364`. Its seven owned files are gateway-native/src/authority_http.rs, http_server.rs, import_client.rs, daemon_startup.rs and supervisor_rollout.rs plus gateway-publication/src/managed.rs and transport.rs, all under rust/crates/. Preserve every defining binding, constructor/confinement and rollout behavior; no production growth, suppression, allowance change or API widening. Its fresh empty target is `/tmp/rust-quality-resume-20261024/prepared/lane-targets/m26-ab1d6f6f364`; the existing complete matching hook artifacts and read-only Bun declaration patch are prepared. Other completed writers retain their identities, tips and ancestry pending exact controller acceptance and integration.

The controller accepted and pushed exact m20 `d8c3e36d2a7c19895d31352ba4bc5258f405fe83`, m25 `a8dc66bebffb78439ac928d28eaa50078d7c95a7` and m24 `b98f4f33351bdc361be4907cc333c3302357ad0f`. After the normal-hook clean checkpoint `35dec3383e2b51c1ba1df8ed25503921315b721b`, m08 integrated all three with separate no-fast-forward merges and proved each ancestor. The resulting clean head `a97c8c0372296be16652b3287563e125a5ecd279`, tree `4e06f80e50f09f062dfd734b0802284185619c7e`, is the controller-pushed draft pull request 1301. Component acceptance is not combined or whole-goal acceptance. Combined Auth/Schema must use a new empty target; combined Authority waits for the accepted m21 tip.

Current disjoint successors remain at base `87bef7d99b4945f1950577316bcbabc766714269`: m27-admin-audit-lints, owner resume_auth_schema, lane `handoff-2026-10-23-rust-quality-wave2-m27-admin-audit-lints-ad039767a36`, owns gateway-admin-data/src/migration/audit/mod.rs and gateway-admin/src/service.rs, plus the separately authorized narrow gateway-admin/tests/source_policy.rs regression; m28-admin-runtime-fallibility, owner quality_acceptance_prepare, lane `handoff-2026-10-23-rust-quality-wave2-m28-admin-runtime-fallibility-ab6adc85d57`, owns gateway-admin-runtime/src/parse.rs, src/state.rs and tests/kernel_source.rs only. All paths are under rust/crates/. Their empty private targets are `prepared/lane-targets/m27-ad039767a36` and `m28-ab6adc85d57` beneath the task root, with complete matching hook artifacts and no copied first-party cache. For m28, the controller separately authorized its candidate commit and an ancestry-preserving merge of the exact a97 predecessor into that same lane before full all-target validation; record the effective predecessor when that operation settles. Runtime defaults/execute, Catalog refresh/fixtures and remaining contract fixtures still need separately recorded owners.

The first guard development capture passed focused tests, changed-file lint, strict TypeScript and format/diff checks at `591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/318bce23-6cf4-4d23-b81e-83794352c00c`; it predates a bounded classifier correction and is stale for current driver acceptance. Cargo subcommand classification now ignores global option values so a configuration file named test cannot turn read-only metadata into recursive compilation; the new regression covers actual guarded launch. Fresh driver evidence remains required.

## Version 7 manifest build profiles

The controller approved removing the two Darwin debug-information overrides from `scripts/rust-verify.ts` and updating its focused environment regression. The verifier retains its explicit environment allowlist, so ambient `CARGO_PROFILE_DEV_DEBUG` and `CARGO_PROFILE_TEST_DEBUG` values are excluded and the committed `rust/Cargo.toml` dev/test `line-tables-only` profiles govern. The production release profile, incremental setting, toolchain selection, target ownership and fixture boundary remain unchanged. M08 owns only those driver/test edits plus this contract and the durable decision entry; other module source remains prohibited.

Acceptance maps these paths to the focused target-owner and verifier tests, changed-file lint, strict no-emit TypeScript, changed-file formatting, the source-size ratchet and `git diff --check`, captured once on the final frozen candidate. Existing component captures produced with debug information disabled retain development and behavior history; they are stale for final required-profile acceptance. Final combined native/WASM gates must use the new exact source and driver, with actual rustc flags inspected for line tables. Unknown final compiler evidence is not inferred from the environment test. Stop on any unexpected source mutation, profile mismatch, new debt or missing required artifact; no production-profile change or unrelated component rerun is authorized.

## 2026-10-25 candidate assembly checkpoint

The controller authorized candidate-only, separate no-fast-forward integration of m21 `190215a6a6d90529fe1d4c0ed0429520d71b165e`, m26 `29acc77455d3ddd2db70aea4a3f2bebe216fc7a1` and m28 `6ca4a24f43067c43597d39705137e3e9b7e9e673`. Read-only admission proved their exact owned surfaces, clean status, diff checks, retained test names, unchanged baseline and no new prohibited constructs or growth. Their component acceptance and combined native/WASM acceptance remain pending; admission and ancestry are not readiness. The candidate also reconciles exact upstream `9fa34a022e696928907a01ba0e2d7b8d475f1998`, retaining the complete quality decision and both appended upstream decisions. No remote push, PR merge, deployment or runtime activation is authorized by this checkpoint.

M27 additionally owns the unused `self` removal in `rust/crates/gateway-admin/tests/contracts.rs` and, only if its fresh comparator requires it, explicit defining-name expansion in that same import group. No other caller or source scope transfers. The baseline reproduction worker may create separate task-local Git metadata with the exact detached immutable source identity and read-only canonical object access; archived source bytes and hashes remain unchanged, with no canonical Git writes. A failed native capture is retained as failure and contributes no partial comparator or WASM acceptance.

The separate functional H4 parity unit starts at exact `d4cb17ef10f605bc7d087d83a9580eb34a60af39` and reserves `rust/crates/gateway-worker/src/inference.rs`, one conventional supporting module and its narrow `tools/cloudflare/runtime-matrix` scenario and registration. Its source, failing fake-provider proof and separate functional PR remain with the functional owner. Quality integration does not absorb those writes; coordinate any future merge against the frozen quality candidate.

Recovered task tools are rooted at `/tmp/rust-quality-final-20261025/prepared`; source `/tmp/rust-quality-final-20261025/compiler-env.sh` and use `/usr/bin/python3 /tmp/rust-quality-final-20261025/compiler-lock.py` for compiler sequences. Preserve compiler lock inode `478177048`, unique source-owned targets and all native/Worker artifacts and receipts. Final native import targets and `TMPDIR` stay within `prepared`, matching the restored committed offline profile. The installed Cargo registry is shared without a broad copy; separate tool installation may stage only exact lock-bound artifacts. No debug override, install at runtime or automatic online retry is authorized.

## Version 8 genuine staged-context remediation

Objective: remediate the unallowlisted contexts confirmed on clean canonical source `08431133cc0758c4c215f7ecf7ac4569ca36101a`, tree `e2c01e6d52b349212c94dee2ccc8fea4553ce8d1`, through genuine source cleanup in the exact disjoint modules below. Preserve the complete provider, credential, accounting, native, serialization and effect-order behavior matrix already recorded by this contract and repository policy. This version changes source assignments and admission sequencing; it does not authorize a runtime, provider, credential, service, workflow or deployment change.

The complete native capture compiled all 30 selected packages successfully, then strict comparison rejected 219 previously absent keys over 98 contexts/76 files. The 121 lint keys represent 496 diagnostic occurrences; the 98 context bookkeeping keys represent 101 occurrences and are not another 98 defects. Existing allowed keys did not increase. Independent read-only audit binds all 775 context source files, 235 freshly rebuilt first-party artifacts, matching compiler identity, actual line-table flags and unchanged comparator extraction/counting. No capture fidelity mismatch was found. The source and baseline remained unchanged; WASM and both updaters were never executed.

Failure receipt: `591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/e0e7a375-dc9f-46cc-8b48-ff5933090c7d`. Independent audit SHA-256: `b65ef88e302f8c9ff6d61ff8f99c83c2c4a0aa073df0f6228d00d41cc64994f7`. The full native diagnostics, contexts, rejection/function maps and terminal status are retained under canonical `.data/rust-quality-wave2-20261025/baseline-20261025T0110Z/`, manifest SHA-256 `40e652a11fc732cacf59e4fe7edaf5bea058ec7e0eabbc28a6681010535b55c5`. The planned assignment manifest has SHA-256 `f1eb6dc39f562f9196397b290a5c233945919e246e6ec42138425d6ef1645973`; its frozen copy is retained in custody rather than relying on a temporary-only path. These are failed-comparison and audit records, not final source acceptance.

All six workers start from the exact documentation-only V8 commit containing this checkpoint, a descendant of `08431133cc0758c4c215f7ecf7ac4569ca36101a`. The integration writer returns its full head and tree, and the controller fills that exact SHA into the assignment manifest before creating any lane. The identities below are already derived from the stable goal-plan path; retain them without recomputation. No lane existed when this version was authorized. The controller owns creation, assignments, later admission and remote delivery. The integration worker remains the sole canonical writer.

| Module                         | Owner               | aid10        | Owned paths | Rejected contexts |
| ------------------------------ | ------------------- | ------------ | ----------- | ----------------- |
| m31-migration-production-lints | provider_finish     | `95bfa8b4ec` | 17          | 19                |
| m32-migration-fixture-lints    | baseline_reproduce  | `db2a4cd044` | 15          | 22                |
| m33-worker-context-lints       | small_split_prepare | `f0f2528431` | 15          | 15                |
| m34-native-live-lints          | wasm_prepare        | `66929eb304` | 6           | 8                 |
| m35-admin-catalog-lints        | admin_finish        | `759effc196` | 7           | 15                |
| m36-runtime-projection-lints   | new_worker          | `5ae64da255` | 15          | 17                |

The 75 module paths are disjoint. The remaining file, `rust/crates/gateway-worker/src/inference.rs`, stays with functional M60 controller `01a15eae-8e8f-7c83-ab73-ce9958f6c7f3`; its `run` and `dispatch_attempts` contexts require coordinated functional ownership, and that response remains pending. M33 has no write authority over this reserved file. Separate M60/M61 functional work and the allocated M63 red-sequence compiler gap remain outside these repair modules.

### m31-migration-production-lints

Owner: `provider_finish`. Exact worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/handoff-2026-10-23-rust-quality-wave2-m31-migration-production-lints-a95bfa8b4ec`. Exact branch: `codex/handoff-2026-10-23-rust-quality-wave2-m31-migration-production-lints-a95bfa8b4ec`. The module depends on the frozen V8 source base only; other module paths, shared manifests, lockfiles, baselines, verifier/parser code and unrelated source are prohibited unless the controller separately transfers an exact surface.

Owned paths:

- `rust/crates/gateway-migration/src/core_state.rs`
- `rust/crates/gateway-migration/src/engine.rs`
- `rust/crates/gateway-migration/src/indexes.rs`
- `rust/crates/gateway-migration/src/indexes/disk.rs`
- `rust/crates/gateway-migration/src/initial_owner.rs`
- `rust/crates/gateway-migration/src/inverse.rs`
- `rust/crates/gateway-migration/src/native_capture.rs`
- `rust/crates/gateway-migration/src/native_custody.rs`
- `rust/crates/gateway-migration/src/native_inverse.rs`
- `rust/crates/gateway-migration/src/native_latest.rs`
- `rust/crates/gateway-migration/src/native_stage.rs`
- `rust/crates/gateway-migration/src/paid_controls.rs`
- `rust/crates/gateway-migration/src/profile_bindings.rs`
- `rust/crates/gateway-migration/src/profile_coupling.rs`
- `rust/crates/gateway-migration/src/profile_runtime.rs`
- `rust/crates/gateway-migration/src/projection.rs`
- `rust/crates/gateway-migration/src/staged.rs`

### m32-migration-fixture-lints

Owner: `baseline_reproduce`. Exact worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/handoff-2026-10-23-rust-quality-wave2-m32-migration-fixture-lints-adb2a4cd044`. Exact branch: `codex/handoff-2026-10-23-rust-quality-wave2-m32-migration-fixture-lints-adb2a4cd044`. The module depends on the frozen V8 source base only; other module paths, shared manifests, lockfiles, baselines, verifier/parser code and unrelated source are prohibited unless the controller separately transfers an exact surface.

Owned paths:

- `rust/crates/gateway-migration/examples/native-stage-apply.rs`
- `rust/crates/gateway-migration/examples/native-stage-frames.rs`
- `rust/crates/gateway-migration/tests/core_state.rs`
- `rust/crates/gateway-migration/tests/import_custody.rs`
- `rust/crates/gateway-migration/tests/native_capture.rs`
- `rust/crates/gateway-migration/tests/native_custody.rs`
- `rust/crates/gateway-migration/tests/runtime.rs`
- `rust/crates/gateway-migration/tests/source_paid_state.rs`
- `rust/crates/gateway-migration/tests/staged.rs`
- `rust/crates/gateway-migration/tests/support/native-offline-fixture.rs`
- `rust/crates/gateway-migration/tests/support/worker-custody-fixture.rs`
- `rust/crates/gateway-migration/tests/support/worker_custody_fixture/host.rs`
- `rust/crates/gateway-migration/tests/support/worker_custody_fixture/port.rs`
- `rust/crates/gateway-migration/tests/support/worker_custody_fixture/producers.rs`
- `rust/crates/gateway-migration/tests/support/worker_custody_fixture/staged.rs`

### m33-worker-context-lints

Owner: `small_split_prepare`. Exact worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/handoff-2026-10-23-rust-quality-wave2-m33-worker-context-lints-af0f2528431`. Exact branch: `codex/handoff-2026-10-23-rust-quality-wave2-m33-worker-context-lints-af0f2528431`. The module depends on the frozen V8 source base only; other module paths, shared manifests, lockfiles, baselines, verifier/parser code and unrelated source are prohibited unless the controller separately transfers an exact surface.

Owned paths:

- `rust/crates/gateway-worker/src/admin_support_host.rs`
- `rust/crates/gateway-worker/src/catalog_host.rs`
- `rust/crates/gateway-worker/src/claude_host/admin.rs`
- `rust/crates/gateway-worker/src/claude_host/platform.rs`
- `rust/crates/gateway-worker/src/compaction_host/protocol/tests.rs`
- `rust/crates/gateway-worker/src/extras.rs`
- `rust/crates/gateway-worker/src/image_host.rs`
- `rust/crates/gateway-worker/src/inference_capture.rs`
- `rust/crates/gateway-worker/src/live_host/platform.rs`
- `rust/crates/gateway-worker/src/native_authority.rs`
- `rust/crates/gateway-worker/src/native_host.rs`
- `rust/crates/gateway-worker/src/native_transport.rs`
- `rust/crates/gateway-worker/src/observability_host/inventory.rs`
- `rust/crates/gateway-worker/src/routing_host/paid_catalog_tests.rs`
- `rust/crates/gateway-worker/src/runtime.rs`

### m34-native-live-lints

Owner: `wasm_prepare`. Exact worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/handoff-2026-10-23-rust-quality-wave2-m34-native-live-lints-a66929eb304`. Exact branch: `codex/handoff-2026-10-23-rust-quality-wave2-m34-native-live-lints-a66929eb304`. The module depends on the frozen V8 source base only; other module paths, shared manifests, lockfiles, baselines, verifier/parser code and unrelated source are prohibited unless the controller separately transfers an exact surface.

Owned paths:

- `rust/crates/gateway-native/src/bin/uos-native.rs`
- `rust/crates/gateway-native/src/existing_account_import/import_publication/tests.rs`
- `rust/crates/gateway-native/tests/contracts.rs`
- `rust/crates/gateway-native/tests/supervisor_data.rs`
- `rust/crates/gateway-native/tests/unix_transport.rs`
- `rust/crates/gateway-live-runtime/src/runtime.rs`

### m35-admin-catalog-lints

Owner: `admin_finish`. Exact worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/handoff-2026-10-23-rust-quality-wave2-m35-admin-catalog-lints-a759effc196`. Exact branch: `codex/handoff-2026-10-23-rust-quality-wave2-m35-admin-catalog-lints-a759effc196`. The module depends on the frozen V8 source base only; other module paths, shared manifests, lockfiles, baselines, verifier/parser code and unrelated source are prohibited unless the controller separately transfers an exact surface.

Owned paths:

- `rust/crates/gateway-admin-runtime/src/defaults.rs`
- `rust/crates/gateway-admin-runtime/src/lib.rs`
- `rust/crates/gateway-admin-runtime/tests/contracts.rs`
- `rust/crates/gateway-admin-runtime/tests/manual_reset/mod.rs`
- `rust/crates/gateway-catalog-runtime/src/versioned.rs`
- `rust/crates/gateway-catalog-runtime/tests/control.rs`
- `rust/crates/gateway-catalog-runtime/tests/runtime.rs`

### m36-runtime-projection-lints

Owner: `new_worker`. Exact worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/handoff-2026-10-23-rust-quality-wave2-m36-runtime-projection-lints-a5ae64da255`. Exact branch: `codex/handoff-2026-10-23-rust-quality-wave2-m36-runtime-projection-lints-a5ae64da255`. The module depends on the frozen V8 source base only; other module paths, shared manifests, lockfiles, baselines, verifier/parser code and unrelated source are prohibited unless the controller separately transfers an exact surface.

Owned paths:

- `rust/crates/gateway-claude-runtime/src/pool.rs`
- `rust/crates/gateway-claude-runtime/src/workspace.rs`
- `rust/crates/gateway-claude-runtime/tests/oracle/mod.rs`
- `rust/crates/gateway-claude-runtime/tests/runtime.rs`
- `rust/crates/gateway-compaction/src/engine.rs`
- `rust/crates/gateway-compaction/src/response.rs`
- `rust/crates/gateway-compaction/src/state.rs`
- `rust/crates/gateway-compaction/src/transcript.rs`
- `rust/crates/gateway-core/src/source_usage/tests.rs`
- `rust/crates/gateway-extras-runtime/src/images.rs`
- `rust/crates/gateway-observability/src/capacity.rs`
- `rust/crates/gateway-observability/src/capacity_math.rs`
- `rust/crates/gateway-observability/src/providers.rs`
- `rust/crates/gateway-observability/tests/claude_source_history.rs`
- `rust/crates/gateway-observability/tests/projection_source.rs`

### Source-candidate admission and final enforcement

Repair the actual nesting, long-function, wildcard and fallibility findings in the assigned contexts. Do not raise or transfer allowances, revive old hashes to evade findings, suppress lints, change the comparator, hide source with cfg, remove tests or rename cases. Preserve every source-bound opaque capability, constructor and ownership proof, quota/accounting lifetime, transport receipt, serialization byte contract and effect order. Keep original failure and audit bytes intact. New source files, helper crates, manifest/dependency edges or caller edits outside an owned list require a separate exact scope transfer before writing.

Keep physical production/test-target debt from growing and preserve the 1000-line crate/target rules, conventional module graph and defining-module imports. Existing oversized crates remain mandatory debt. Private test proof must remain private: no production visibility widening, fake capability, replacement fixture, compatibility facade, re-export, path attribute, include macro, allow/expect suppression or source-context waiver is authorized. Genuine new or substantially rewritten crates comply immediately.

Isolated writers return committed source candidates with exact base/head/tree, changed paths and source hashes; pinned syntax/rustfmt receipts; physical no-growth and unchanged-case inventories; diff checks; normal commit-hook exit/full log; and settled jobs. Formatting remains limited to two changed-file passes per edit round. Preserve every existing test name, assertion, golden input and production proof boundary. Normal commits and hooks remain enabled, with matching immutable prepared dependencies; this version adds no hook bypass.

Source-candidate admission records inspected scope, syntax, formatting, no-growth and case-preservation evidence. It makes no compiled, behavior or final-readiness claim. Do not run per-lane cold compiler, parser/comparator, behavior or gate rounds while the functional gap owns the compiler schedule. No writer pushes, merges, dispatches CI or takes another module's files. The controller may admit exact clean candidates into canonical with separate no-fast-forward merges, preserving every tip and all evidence.

After all repair writers and the reserved functional contexts settle, freeze one clean canonical source and run the unchanged selected native/WASM full CLI, strict complete-stage comparator and affected tests on that source. Use the existing shared compiler lock inode `478177048`, at most two jobs and canonical-owned private targets; retain existing canonical caches across same-root edits, and never copy or use a foreign first-party cache. Matching Darwin fixture boundaries, real native prerequisites and actual Worker artifact checks remain mandatory. The independent verifier owns final gates; admission cannot waive a failed result.

Every remediated changed context must produce zero new/unallowlisted findings before the final baseline update. Only the supported downward-only structural and complete-stage Clippy updaters may then tighten recorded debt, preserving historical adoption identities and all prior evidence. A subset, partial stream, stale context or owned-only pass cannot update a complete stage. Journal terminal phase/status, exit and artifact reference before a future helper hands back, so a completed failure cannot look like a live compiler phase.

The latest parser report on `08431133cc0758c4c215f7ecf7ac4569ca36101a` measured 18 oversized production crates, 13 oversized test targets and zero include/path/pubUse/suppression constructs, with no structural growth; that report alone is not a passed strict check. Full crate-size and remaining lint debt cleanup remains the goal after PR #1301. The preserved successor proposals are read-only planning evidence until separately assigned; this V8 repair contract does not authorize their implementation. Production release profiles, credentials, live data, provider spend, services, remote push/PR mutation and workflow enable/dispatch/rerun remain outside worker authority.

## Version 9 source-candidate assembly and scoped transfers

This checkpoint records source-candidate admission from the exact V8 base `50675dcd1af199e49de01a57151b92502ea5def5`, tree `a2aca253b5833f3b034ad17b3dd0c92669fa07f8`. The controller independently inspected each admitted clean tip, scope, ancestry, diff and terminal static handback. Source-candidate admission is not Rust compilation, strict comparator, runtime, actual Worker or final acceptance. No per-lane cold gate was substituted for the required frozen canonical gates, and no baseline or source-context finding was waived.

| Module | Exact source tip                           | Tree                                       | Disposition                                                  |
| ------ | ------------------------------------------ | ------------------------------------------ | ------------------------------------------------------------ |
| M31    | `bca241dd04eceed3d9e519a2618b4e3d4a818928` | `c12e996b7b44dd5d73c7fcd3edaeb3a87023d60a` | Migration production source candidate                        |
| M34    | `5577bea7d8f94f38ddfd08b3450cec5b30497e33` | `f15146640970a3e679f2bf805dcb749b41ceb6ed` | Native and Live source candidate                             |
| M36    | `666126c405e1f64e90b742cac5e87c4890323a5a` | `e6c8eb168dfa8d31edf3ee45fc35dcd593be1aa0` | Runtime projection source candidate                          |
| M35    | `5012d0c367a564b30e2645956cd1ae835c51a5f2` | `22e18eac7db5e36a818c1e923685f2bc7d5da388` | Admin and Catalog unit-v2 source candidate                   |
| M33    | `f0cec8d8392a2cd38f07bb932a9e4c21e3e85a3b` | `d480becfc9ca1036a7c0046e9d15601a2ec0a374` | Worker unit-v2 source candidate; atomic M37 integration only |

M31, M34, M36 and M35 were integrated with separate no-fast-forward merges. M36's actual owner is the external controller's existing `quality_fixtures` worker, as bound by the assigned manifest SHA-256 `f0c475dbf9c8ea8815a876fdc5c275446df435c05c201b8305344723f15b9d20`; the earlier planned `new_worker` slot remains historical. All original lane identities and admitted ancestry are preserved.

M35 unit-v2 permits the seven existing seed-caller tests in its already owned file to use the approved private seed conversion. The controller inspected that exact scope and terminal handback; this transfer adds no production visibility, replacement capability, assertion deletion or unrelated caller authority.

### Atomic M33 and M37 Worker unit

M33 unit-v2 owns the exact capacity-recovery extraction and corresponding workspace, Worker dependency and frozen root lockfile edges. The existing Worker capacity implementation and unchanged tests move into the conventional `gateway-capacity-recovery` crate; the new crate measures 637 owned production lines and remains below 1000. M33's terminal candidate changes 23 paths from the 25 granted paths, including the conventional moves. No package version upgrade, lockfile regeneration or unrelated dependency change is authorized.

The controller accepted M37 only as an uncommitted pure quality fragment from the same V8 source: `inference.rs` and its conventional private child `inference/stream.rs`. Its immutable patch SHA-256 is `20a6fe8522a273bce81eb2f2a5f48cba150f54920bb271062c69218a24d22f65`; terminal handback SHA-256 is `5fd4983494f54a981ccb3a4b2a702de206b18fffc9a05f972a04bc0bf6cff1a9`. The frozen fragment preserves exact original tests and private `RequestResources`, original literal values, clone counts and source-only effect-order checks. It contains no H4 behavior or fixture ancestry. The dirty M37 lane remains preserved without an accepted standalone tip.

M37 alone would grow Worker by 143 lines and is never committed or admitted alone. Canonical first merged exact M33 with `--no-ff --no-commit`, retained its `MERGE_HEAD`, then applied only those two exact M37 patch paths. The combined Worker was independently remeasured at 20516 lines, below the V8 ceiling 20784 by 268; the new capacity crate remained 637. Baseline bytes remained unchanged. One normal direct commit with its ordinary hooks produced ancestry-preserving merge `641152c5b013a573f5ca7ed4b6db7d1a42475f2e`; its parents retain the previous source candidate and exact M33 tip. This atomic non-growing tree is the acceptance unit, with M37 patch/source hashes bound in custody. It is still uncompiled source evidence.

### M32 admission and source-freeze verification

M32 unit-v2 was separately admitted at exact `0c75ffc2571ef59ff7d3d24c4b9d57d090261936`, tree `f58b3719d28b6ea577cf5cae2463ce6f3c71c536`, with its original V8 ancestry retained by a separate no-fast-forward merge. Scalar `Receipt` and case definitions move into existing `gateway-migration-test-support/src/fixture.rs`, with only `serde.workspace = true` added to that helper's `Cargo.toml`. Static source evidence retains 46 cases/305 assertions, the scalar DTO/list/schema order and fixture closure 1922 below the V8 ceiling 1928; the helper measures 218 lines. After exact admission, the integration writer added only the existing pinned `serde` dependency name to `gateway-migration-test-support` in root `Cargo.lock`. A semantic lock-graph comparison and read-only locked offline Cargo metadata bind that one edge; no lockfile regeneration, version or unrelated edge change is authorized.

The supported plan generator is rerun after exact M32 and the capacity member integrate; native 31 packages/24 test packages/52 commands and WASM 26 packages/five commands including the actual Worker build replace the stale native 30/WASM 25 composition. These are selection plans, not executed Rust gates. The full selected native/WASM CLI, strict comparator, affected tests and actual source-bound Worker/native artifacts remain required on a newly frozen clean canonical commit and tree. Native fixture prerequisites, the existing Darwin boundaries, manifest line-table profiles, shared compiler lock and source-owner cache guards remain unchanged. M64's separately owned synthetic native-example sequence retains its reserved compiler slot; this assembly queues no quality compiler.

No baseline updater runs until a complete strict capture passes with zero unallowlisted changed contexts, and updates remain downward-only through supported tools. Preserve every original failure, static log, handback and source identity in ignored canonical custody. The clean source-candidate checkpoint after these edits is frozen only when the controller accepts its exact head/tree; it remains separate from the final post-baseline independent acceptance tree. Full remaining crate-size and staged lint debt cleanup continues beyond PR #1301; no component admission closes that goal.

## 2026-10-25 complete diagnostic and downward-baseline checkpoint

All six admitted V8 cleanup tips retain ancestry, including the atomic M33/M37 unit and the exact M32 fixture/dependency integration recorded above. Subsequent canonical corrections were scoped source work with normal hooks; every changed source invalidated its earlier receipt. The final diagnostic source is `1a000e6ace8cc4a870b6438b952d91f504b2d5c4`, tree `3de370238533ec533435075db5e4ca1e95c98f49`.

| Source commit                              | Scoped correction                                                                                                                                        |
| ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `4ca4cd2bd12b6de7f24e4fd03b4f68332d2e267a` | Use the existing ControlFlow Break variant for the lossless defaults response rail.                                                                      |
| `0e899d723dde4afa061eb253061bb72fbffdf205` | Remove exactly two unused Migration imports and nine redundant reference layers.                                                                         |
| `7671575d0f04337d6b25144b85cf0c37039849ee` | Preserve the explicit fixture missing-row error and the original administrative-observation boolean return.                                              |
| `9cf420990c2f7bf5c7e5ab2503167d281eeae3e4` | Pass the already borrowed Worker request directly.                                                                                                       |
| `a66a3936814140e1feae83b4d8f4ad9023b2c6c1` | Retain the Claude fixture Bootstrap and credential failure rail; relocate the exact shared test executor into the existing defining test-support module. |
| `1a000e6ace8cc4a870b6438b952d91f504b2d5c4` | Preserve the expected kernel-quota GatewayError/status assertion through an explicit negative-fixture error rail.                                        |

The first literal Claude fixture correction formatted to 1521 reachable lines and stopped before commit because its current ceiling was 1520. The separately approved real executor relocation retained its algorithm bytes, all call sites/case names/assertions/literal values, and the two original fixture corrections; the resulting contracts target is 1509 lines and the existing helper crate is 167 lines. No blank/comment/assertion offset, new dependency or production API was used. The kernel refusal fixture measures 1191 reachable lines against the independently measured V8 source ceiling 1209; its larger historical adoption allowance was not used to justify growth.

Complete native 31-package and WASM 26-package Clippy captures both compiled and passed the unchanged strict staged comparator with zero new contexts. The fresh executed receipt is `591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/0ca008a8-ea40-4f5e-bdaa-aa4a5cdcc1ad`. Its native JSONL SHA-256 is `373d64d3d43bf431e0166fb2e781f974174c948a2e117c4f4aa9f04ff611bf2d`, WASM JSONL SHA-256 is `68b5fa9cc0d1639601194a2040169c2aa3eb5049eae4cdc429107c2d5d18affe`, and both saved source-context inventories have SHA-256 `a00996c7c20838617a44e9db1c413d133851b1eb477ece386ed5eb9a6ee069fa`. Actual compiler argv, terminal phase/exit, source/driver/environment bindings and complete raw outputs remain in canonical custody. These captures ran no behavior tests or Worker build.

Only after both full strict stages passed, the controller approved the supported structural updater and the complete native/WASM staged updaters. All three commands exited 0; receipt `591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/880b0300-021b-4f05-8b3c-54b9547d24e8` retains their exact commands and outputs. Every count/key only fell or disappeared, adoption metadata remained decoded-equal, and all 807 independently bound Rust source/manifest inputs stayed byte-identical. No manual allowance, old fingerprint transfer, partial capture, source suppression or update initialization was used.

| Baseline surface                                  | Before | After  |
| ------------------------------------------------- | ------ | ------ |
| Oversized production crates                       | 18     | 18     |
| Recorded oversized production lines               | 164580 | 164064 |
| Oversized separate targets                        | 14     | 13     |
| Recorded oversized target lines                   | 23815  | 21272  |
| Visible re-exports                                | 131    | 0      |
| Path attributes                                   | 27     | 0      |
| Source suppressions                               | 1      | 0      |
| Include macros                                    | 0      | 0      |
| Native staged keys, including context bookkeeping | 4940   | 4017   |
| WASM staged keys, including context bookkeeping   | 991    | 616    |

Remaining staged lint occurrences are native 153 excessive-nesting, 261 expect-used, 101 too-many-lines, 6840 unwrap-used and 82 wildcard-imports; WASM has 130, 196, 82, 31 and 77 respectively. Context occurrence bookkeeping is 2097 native and 380 WASM; it is not a separate defect count. The 18 oversized production crates, 13 oversized targets and this remaining staged debt keep the full Rust quality goal open. No paired compile-performance improvement is claimed from validation timings.

Local review is unavailable, and no verified review findings were ingested. Hosted CI was not manually enabled, dispatched or rerun. The independent final selected CLI, affected tests, actual source-bound native/Worker artifacts and exact final commit/tree acceptance remain pending after this baseline/documentation commit. The successful pre-update captures and updater receipt are preparation evidence, not final acceptance or deployment authority.

The custody ledger at canonical `.data/rust-quality-wave2-20261025/LATEST.json` binds the original baseline, all successful/failed captures, typed correction proofs, the no-growth contradiction and relocation, raw outputs, source contexts, tool/native-KV preparation and terminal records. The previously disclosed failed-bootstrap ancillary observer gap stays explicit; its original raw streams and exact restored journals remain separately verifiable. Separate M60/M61/M63/M64 functional work remains with its controller and does not enter this quality graph or acceptance implicitly.
