# Native gateway server and storage lifetime fixes

## Work contract v1

Recheck D01–D14 against integration base `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`
with audit tip `fa1cd486b94214e5110529a35fd7c4e0ff5c93a9` merged. First fix HTTP
write ownership, then WebSocket shutdown, then ownership and teardown, then the
comparison-compiler expressions. Keep each fix with its evidence in one commit.

| Surface | Required behavior |
| --- | --- |
| HTTP and WebSocket | Keep wire bytes, status and close codes, backpressure, and actual delivery receipts. Shutdown still aborts and drains its admitted work in order. |
| Worker boundary | Exercise refused, cancelled, dropped, and delivered bodies using local mocks. Preserve one settlement using the semantic terminal and delivery outcome. |
| Kernel, cache, and KV | Preserve quota timing, key and value bytes, consistency, and error types. Exercise lease teardown with fake time. |

The edit scope is the assigned C++ runtime, RT storage and platform files, storage,
cache, kernel and worker modules and headers, their tests and registry entries, and
this report. Provider implementations, TypeScript, workflows, C++ outside the
gateway, deployment, and live storage are outside this work. Fixture runs use a
fresh home and temporary directory in the driver's isolated loopback namespace
with a credential-free environment.

Use `bash cpp/gateway/tools/ci-build.sh --config release <test>` and the matching
`--config asan` for link-dependent tests after each family. Use all four sanitizer
shards for the final affected set, one full release shard, and
`python3 cpp/gateway/tools/links.py --check`. Reverify stale audit line numbers and
interleavings before editing. The unknowns are the current reachability of each
finding and whether the fixture I/O backend can observe D01's buffer lifetime
under ASan. A cold driver configuration compiles the production object graph once.
Check progress through incremental driver logs and reuse the object cache for
focused verification. Stop at verified verdicts for all rows or commit the
building subset with the pending rows and their evidence gaps stated.

## Defect ledger

A commit listed as “this commit” is the commit that introduced that verdict or fix.

| ID | Verdict | Commit | Test |
| --- | --- | --- | --- |
| D01 | LEFT — Reverification and backpressured disconnect regression pending. | — | Pending |
| D02 | LEFT — Failed-bind ownership reverification pending. | — | Pending |
| D03 | LEFT — Drain waiter abandonment reverification pending. | — | Pending |
| D04 | LEFT — Early SSE registry ownership reverification pending. | — | Pending |
| D05 | LEFT — HTTP stream abort listener reverification pending. | — | Pending |
| D06 | LEFT — Upgrade and shutdown ordering reverification pending. | — | Pending |
| D07 | LEFT — Memory KV await-expression reverification pending. | — | Pending |
| D08 | LEFT — Cache fingerprint await-expression reverification pending. | — | Pending |
| D09 | LEFT — Abandoned quota lease reverification pending. | — | Pending |
| D10 | LEFT — Worker body read listener reverification pending. | — | Pending |
| D11 | LEFT — Worker request bridge reverification pending. | — | Pending |
| D12 | LEFT — Worker delivery abort listener reverification pending. | — | Pending |
| D13 | LEFT — Pending delivery receipt retention reverification pending. | — | Pending |
| D14 | LEFT — Quota and usage await-expression reverification pending. | — | Pending |

## Verification and open questions

The audit merged cleanly and added four files under its findings and analysis
paths. Dependency preparation and the first driver build are pending.

[Area B audit](cppgw-lifetime-b-20261118.md)
