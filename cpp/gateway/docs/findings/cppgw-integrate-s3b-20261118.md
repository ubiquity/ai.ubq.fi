# C++ gateway library integration, stage 3b

## Work contract v1

Start at the verified stage 2b tip `af7a0036`. Merge the three reviewed tips
from `origin` as ordinary two-parent commits, in the order below. The integration
owns `cpp/gateway/`. TypeScript, workflows, the pilot outside `cpp/gateway/`,
production services, credentials and live data are outside this stage.

| Surface | Required behavior |
| --- | --- |
| Bundled resources | Use filesystem paths for trusted paths. Preserve untrusted URL and permission validation and the file response contract. |
| Numbers | Remove the reviewed unused helpers. Preserve active formatting, parsing and gateway accounting. |
| Regex | Use the matching quickjs-ng source and keep the gateway's validation and stored-value contracts. Library diagnostics and Unicode set support may differ. |
| Depth and vendors | Preserve the JSON limit of 128 and the independent stored-graph limit of 2,000. Keep all registered libraries and the exact incoming vendor bytes. |

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

| Order | Ref fetched from origin | Incoming tip |
| --- | --- | --- |
| 1 | `agent-jobs/cppgw-slim-file-urls-20261118-44298551168-1` | `caa5ee5c544ac2745533e6a97e1cc5e6f18a31e0` |
| 2 | `agent-jobs/cppgw-slim-numbers-20261118-44298517687-1` | `dcf103473cbca9c185640c69b7ad0fb432187388` |
| 3 | `agent-jobs/cppgw-std-regex-c2-44298488608-1` | `8ed010b9da77b0c87f8a3bf089e85934cc692651` |

Dependency preparation is in progress in `/var/tmp/cppgw-integrate-s3b.2sXMMKTy`.
The merge, release and sanitizer receipts are pending.
