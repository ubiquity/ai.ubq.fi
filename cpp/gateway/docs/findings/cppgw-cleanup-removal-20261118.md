# C++ gateway branch cleanup

## Work contract v1

Remove the TypeScript and Rust gateways and the independent C++ pilot from the integration branch after relocating native build inputs. Keep the C++ gateway, replay fixtures, static assets, native CI and packaging, and an independently pinned Bun comparison. The change is confined to the branch's tracked files. Production, DNS, credentials, remote tags, provider traffic and unrelated gateway defects are outside the scope of this PR. Keep all native tests, delivery and accounting behavior, offline codec and artifact validation, and fail-closed fixture confinement. Use only local provider mocks in credential-free tests.

| Surface | Acceptance |
| --- | --- |
| Tracked source | Ordered buildable commits for relocation, legacy removal groups and root rewrites. Report every move, retained root entry and tracked file and text-LF count. |
| C++ tests | `python3 cpp/gateway/tools/links.py --check`, release shard 1/4 and focused runtime, static, KV, permissions, startup, passkey and relocated fixture tests from the cleaned checkout. |
| Releases | Build and verify the Linux ARM64 portable package using the moved addon sources. Keep the sealed artifact contracts and macOS package input layout. |
| Paired comparison | Fetch the exact legacy commit and use a detached worktree for Bun. Run the full local-mock comparison with an independently bound native package. Preserve existing failing expectations. |
| Tooling and handoff | Check changed-file formatting, both native workflows with `actionlint`, and tracked paths and live source references with `git grep`. Deliver a stacked PR against the integration branch. |

The pilot's independence requires a path search and a reduced build. The cold release shard is expected to take about 35 minutes and the portable build about 30 minutes without competing jobs. Per-command logs, driver receipts and package stages show progress. If a command fails, inspect its retained log and retry only the failed gate where a recovery is available. Stop after collecting the named evidence or documenting an exact blocker. Hosted CI owns the full release, sanitizer and fuzz matrix.

## Starting point

- Integration commit `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`, tree `c41de1cd9476af1ff2e9ed958263096e9ba70f96`, on `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1`.
- Inventory tip `d6dd1c62` was fetched and merged first as `e205250f81797c2f2062eea96cfd22c5743087cd`, tree `93d9d5571a2c343435a33ebd229258b46194ef15`. The inventory adds two documents, both read before source changes. [Plan](../CLEANUP-PLAN.md) · [Inventory findings](cppgw-cleanup-inventory-20261118.md)
- Legacy oracle commit `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`, tree `d4f9c3beb10816358c85e92c1c231421fa35fa48`, is an ancestor of the integration branch.
- Runner scratch is `/var/tmp/cppgw-cleanup-removal.Vk5FzVNc`. The checkout is on Ubuntu 24.04.5 ARM64 with four CPUs and Docker 28.0.4. Build output and downloads go to scratch.
- The inventory observed two shared paired failures, `chat-invalid-role` and `responses-invalid-tool-type`. Both runtimes dispatched to a mock and answered 200 where the suite expects 400 and no dispatch. The expectations are retained here.
