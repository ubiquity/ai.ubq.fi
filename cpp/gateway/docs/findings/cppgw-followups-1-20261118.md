# C++ gateway date, string and Apple Silicon follow-ups

## Work contract v1

This follow-up targets `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1` at
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. Bring in the three reviewed branches in the requested
order using ordinary merge commits. Retain every deviation entry from both sides and commit each
merge and fix separately.

| Surface | Required behavior |
| --- | --- |
| UTC date helpers | Use the C++20 calendar and preserve the gateway's UTC, validated input and date-range contracts. |
| Fixed protocol strings | Use byte comparisons for the Codex quota and Responses tokens. Keep Unicode behavior for callers that need it. |
| Apple Silicon | Build and link every registered vendor, including QuickJS-ng `libregexp`, and retain the four Darwin stored-value fixture updates. |
| Provider traffic | Use local mocks only. Run with an empty home, a fresh temporary directory and a clean environment. |

The owned paths are the three branches' changes under `cpp/gateway/`, the two macOS build entry
points and this report. The workflow, TypeScript, the rest of `cpp/`, live providers and deployments
are out of scope. Build products and upstream dependencies belong in scratch outside the checkout.

The acceptance gates are `python3 cpp/gateway/tools/links.py --check`, the date, string, Codex quota,
Responses request, clock, filesystem and stored-value tests in `release` and `asan`, and
`bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4`. Check shell and Python syntax for
the macOS build changes on Linux. The Linux runner cannot execute either macOS build. GitHub write
access is unverified. Stop after the local gates and follow-up handoff, with the remaining evidence
recorded here.
