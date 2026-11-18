# C++ gateway KEEP audit

Source baseline: `b54357a92c0f2669335d83f04047c3f7de32f925` (`agent-jobs/cppgw-std-icu-20261117-44188151110-1`), tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`.

## Work contract v1

Audit rows 17–55 of the [replacement plan](../OSS-REPLACEMENT-PLAN.md) under the owner's library-first policy. Record the current size, production callers, a replacement or deletion decision, net savings, risk, and ownership boundaries for each row. Write runnable engineering briefs for the selected lanes.

| Surface | Contract |
| --- | --- |
| Public HTTP and upstream I/O | Preserve wire contracts, validation, cancellation, receipts, and exactly-once accounting. |
| Storage and security | Preserve bytes and value types already persisted, ordering, authorization, trust, launch confinement, and fail-closed behavior. |
| Runtime internals | Use the current C++20 build and pinned dependency closure as the starting point. Evaluate Bun-specific behavior where it reaches a gateway contract. |
| Verification | Recount against Git source and validate brief selectors with `bash cpp/gateway/tools/ci-build.sh --list`. Format changed Markdown and run `git diff --check`. |

The owned paths are this finding and new Markdown under `cpp/gateway/docs/briefs/`. Source, fixtures, build settings, deployments, providers, and other lanes are outside this report. The open questions are live API usage, current ledger boundaries, and the net size of the compatibility code each replacement would need. Stop after all 39 verdicts, the risk-ranked estimates, and at least eight briefs are checked.

## Audit status

Guidance read: [CI](../CI.md), [build](../BUILD.md), [runtime](../RUNTIME.md), [replacement plan](../OSS-REPLACEMENT-PLAN.md), and [porter rules](../../PORTING.md).
