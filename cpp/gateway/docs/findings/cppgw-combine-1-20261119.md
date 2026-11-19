# Native gateway follow-up integration

## Work contract v1

Integrate the seven reviewed branches onto the F2 gateway in the supplied order. Each source tip must remain in the
combined branch's ancestry. The owned paths are this report, the native gateway files needed to resolve integration
issues, and the README and Bun-deviation guide totals. The job covers the combined branch and Linux ARM64 test fixtures.

| Area                                             | Acceptance evidence                                                                                                                                                                                 |
| ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Cleanup and packaging                            | Inspect active build, test, and packaging references against the paths removed by the cleanup.                                                                                                      |
| Standard library and Apple Silicon               | Carry forward the helper changes, vendor build coverage, and the recorded Apple Silicon qualification.                                                                                              |
| Runtime, server, provider, and handler lifetimes | Compile the shared runtime with the four fixes and verify each new regression test is registered exactly once.                                                                                      |
| Integrated suite                                 | Run `python3 cpp/gateway/tools/links.py --check`, every `bash cpp/gateway/tools/ci-build.sh --config <release or asan> --shard N/4` with real fixture isolation, and `actionlint` on the workflows. |

Starting tree `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. The test count, combined sanitizer behavior, and
publication status are pending. The gate requires all seven merge commits, both complete suites, and a record of
verification and outstanding work. Build and test outputs live outside the checkout, with clean test environments and
local mocks.

## Merge record

Each branch was fetched with `git fetch origin "$branch"` and merged with
`git merge --no-ff --no-edit -m "Merge $branch" FETCH_HEAD`. All seven merges were clean. After each one,
`python3 -B cpp/gateway/tools/links.py --check` resolved the listed inventory.

| Source branch            | Tip                                        | Merge commit                               | Binaries | Change                                                                                                                    |
| ------------------------ | ------------------------------------------ | ------------------------------------------ | -------: | ------------------------------------------------------------------------------------------------------------------------- |
| `cppgw-cleanup-removal`  | `fc4f9ac29d91998eb30dcba70e9ae5505bf882e6` | `b5ed3f62d330aac11e3c7dd56c5fa4566d75484d` |      450 | Retired the TypeScript and Rust gateways and the C++ pilot. Relocated native build and package inputs.                    |
| `cppgw-followups-1`      | `824687e6b5e2f561ebc588f3456f83e33e1b3228` | `86d3310176030de6763e09559fdafde2b3882f01` |      450 | Moved date and string helpers to the C++ standard library and extended the Apple Silicon build to all vendored libraries. |
| `cppgw-macos-requal-fu1` | `2d7ba4880f1e66be6c94ab2714334b8682ba5f7a` | `752378643d360df87f0cad36b50e90beb7b70334` |      450 | Recorded the Apple Silicon run of follow-ups 1, with 450 binaries and 5,119 cases.                                        |
| `cppgw-lifetime-fix-a`   | `113823bbc0675751cfb16d96d25d9c9f5ee79261` | `e1fafb8c23acda149d845db55d643091b5687e86` |      450 | Fixed runtime network, coroutine and callback lifetime issues.                                                            |
| `cppgw-lifetime-fix-b`   | `83b4a34bba1ac3d2357c526b729c15246ddace4f` | `3e171b2abc44442d09b362bfe3c78c6c34d2f32b` |      450 | Fixed server, worker, quota and storage lifetimes, including the HTTP write buffer.                                       |
| `cppgw-lifetime-fix-c`   | `f2656dfb36b8197f2bae0d033857102cb190fece` | `b8c33c8ffb740f7e4ebd43eea49d26031b3eded7` |      451 | Fixed provider and Codex request lifetimes and added the KV fan-out test binary.                                          |
| `cppgw-lifetime-fix-d`   | `b161db8ad081398b7dea3d6fd54b3ae2893a1ce7` | `57efff745ba8eec831ad1142ed9eb71dfc0cb458` |      451 | Fixed handler, authentication and administration lifetimes and startup cleanup.                                           |

## Integration fixes

- `d93cc0a8` updated the new startup regression in `tests/serve_test.cpp` to use the relocated
  `packaging/native-kv` manifest on both platforms.
- `2529815a` added the new provider KV fan-out binary to `tests/LINKS.txt` once, retaining the link closure
  declared in the test. The combined Links check resolved all 451 binaries.

## Verification checkpoint

The combined gates select the native build and test files, package path references, and both remaining workflows.
The runner is Ubuntu 24.04.5 ARM64 with four logical CPUs. Clang 21 and mold came from apt. The prepared Boost
1.92.0, ICU 78.3 and SQLite 3.54.0 inputs and the gate command and logs are under
`/var/tmp/cppgw-combine-1-20261119.FjNwtHT3`. The two workflows passed actionlint 1.7.11
with ShellCheck and the `ubuntu-26.04-arm` label configured as in the F2 lint receipt.

The full suite runs release then ASan, with four shards per profile and common objects reused within a profile.
Allow about 75 minutes for Release and up to 120 minutes for sanitizers, based on the source branch receipts.
The wrapper records the full commit and tree, incremental build and test logs, per-shard exit statuses and copied
driver receipts. A failed shard stops the sequence for diagnosis and a focused correction before the full gate
resumes. Both test totals and the final handoff remain pending.
