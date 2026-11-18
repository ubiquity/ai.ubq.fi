# C++ gateway coroutine and callback lifetime audit

Audit contract v1. Source commit `33ea9b20d1f02b96bb86a24c0530641ccd4fd525`, tree `3bbd8cfdc60058651e0494c6ae560ca01e340940`, from the suite-repair branch. The scope is all first-party code in `src/` and `include/` under `cpp/gateway/`. The report and `analysis/lifetime/` are the audit's write paths.

## Scope and evidence

| Surface | Ownership question |
| --- | --- |
| Coroutine lambdas and parameters | Who retains the closure, its receiver, and every borrowed argument from initial suspension until completion? |
| Socket, timer, abort and stream callbacks | Can dispatch outlive the receiver, or can reciprocal ownership prevent release? |
| Container traversal and lookup | Can another coroutine erase, replace or invalidate a borrowed element while the first coroutine is suspended? |
| Await expressions | Does the expression retain a temporary, consume a result inline, or directly destructure the await result against the port's GCC 12 rules? |

The review preserves request routing, status and wire contracts, admission and settlement, cancellation semantics, persistent writes and shutdown ordering. Direct child awaits, fail-fast combinators and detached operations have distinct ownership requirements. The same checks apply across provider adapters. Request-path memory safety has first priority. Production code and tests stay at the audited source revision.

Tool selection follows the portable build driver and `tools/tidy.sh` native compilation flags. The runner is Ubuntu 26.04.1 x86_64 with four CPUs, Clang and clang-tidy 21.1.8, Python 3.14.4 and Bun 1.4.0. Platform-specific paths outside active Linux preprocessing and any lifetime without a proven current call path will be recorded explicitly. Focused sanitizer checks use synthetic inputs and isolated environment and scratch paths. Completion requires per-site verdicts, reconciled counts, proposed fixes and the exact commands and outcomes. The report and data receive changed-file formatting and Git whitespace checks.

## Status

Source inventory and tool setup started. Candidate verdicts, exact receipts, sanitizer results and proposed fixes are pending.
