# C++ gateway library integration, stage 3b

## Work contract v1

Start at the verified stage 2b tip `af7a0036`. Merge the three reviewed tips
from `origin` as ordinary two-parent commits, in the order below. The integration
owns `cpp/gateway/`. TypeScript, workflows, the pilot outside `cpp/gateway/`,
production services, credentials and live data are outside this stage.

| Surface           | Required behavior                                                                                                                                        |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Bundled resources | Use filesystem paths for trusted paths. Preserve untrusted URL and permission validation and the file response contract.                                 |
| Numbers           | Remove the reviewed unused helpers. Preserve active formatting, parsing and gateway accounting.                                                          |
| Regex             | Use the matching quickjs-ng source and keep the gateway's validation and stored-value contracts. Library diagnostics and Unicode set support may differ. |
| Depth and vendors | Preserve the JSON limit of 128 and the independent stored-graph limit of 2,000. Keep all registered libraries and the exact incoming vendor bytes.       |

Compare the fetched branch reports and actual deltas before resolving conflicts.
The conflict set and detailed fixture differences are open at discovery. Build
and run affected tests after each merge, then run all four
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4` selections and
`--config asan` for regex, KV, URL, static files, numbers and permissions.
Verify the Links registry and the vendored bytes. Every test uses an empty home,
fresh temporary storage and the portable driver's isolated mock network.
The stage ends with the ordered merge ancestry, passing gates, and a report of
the exact commands, counts, measurements and any unfinished work.

## Starting checkout

Ubuntu 24.04.5 on AArch64 with four CPUs. The checkout starts at the stage 2b
report commit with that stage's 448 release binaries and 5,086 cases.
[Stage 2b findings](cppgw-integrate-s2b-20261118.md)

| Order | Ref fetched from origin                                  | Incoming tip                               |
| ----- | -------------------------------------------------------- | ------------------------------------------ |
| 1     | `agent-jobs/cppgw-slim-file-urls-20261118-44298551168-1` | `caa5ee5c544ac2745533e6a97e1cc5e6f18a31e0` |
| 2     | `agent-jobs/cppgw-slim-numbers-20261118-44298517687-1`   | `dcf103473cbca9c185640c69b7ad0fb432187388` |
| 3     | `agent-jobs/cppgw-std-regex-c2-44298488608-1`            | `8ed010b9da77b0c87f8a3bf089e85934cc692651` |

Dependency and test evidence live outside the checkout at
`/var/tmp/cppgw-integrate-s3b.2sXMMKTy`. The runner uses Clang 21.1.8,
Boost 1.92.0, SQLite 3.54.0 with `SQLITE_DQS=3`, and ICU 78.3.

## Merge checkpoints

1. `84f5adbaadf6d0a0a0806fa1cde8135d1ebd1309` integrates the filesystem-path
   branch. Git conflicted only in `docs/DEVIATIONS-FROM-BUN.md`. The resolution
   retains the stage 2b sections and appends the trusted-resource section.
   The first focused build on `46a1492a` compiled all 369 production objects,
   passed ten binaries and 98 cases, and found one build failure. The new
   Linux permissions fixture called the removed `rt::path::basename` twice.
   The merge resolution uses `std::filesystem::path::filename()` for both
   checks and retains the assertions. The 11-binary rerun passed all 104 cases.
2. `029bd0d1ddd11426b1a416c07c2d4079a601df51` integrates the numeric cleanup.
   Git conflicted only in the deviations ledger. The resolution appends the
   numeric section. The automatic Number fixture merge carries forward the
   stage 2b BigInt rejections for `+`, `-` and `0x `. A source search found
   no calls to the deleted helpers after the merge. The focused gate is running.

The requested regex tip records the continuation sweep in
[`cppgw-std-regex-20261117.md`](cppgw-std-regex-20261117.md).
The named `cppgw-std-regex-c2.md` has no blob at that tip.
The two KV fixtures and their previous diagnostics are present in that sweep.
