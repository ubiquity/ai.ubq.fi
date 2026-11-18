# C++ gateway date, string and Apple Silicon follow-ups

## Work contract v1

This follow-up targets `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1` at
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. Bring in the three reviewed branches in the requested
order using ordinary merge commits. Retain every deviation entry from both sides and commit each
merge and fix separately.

| Surface                | Required behavior                                                                                                                  |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| UTC date helpers       | Use the C++20 calendar and preserve the gateway's UTC, validated input and date-range contracts.                                   |
| Fixed protocol strings | Use byte comparisons for the Codex quota and Responses tokens. Keep Unicode behavior for callers that need it.                     |
| Apple Silicon          | Build and link every registered vendor, including QuickJS-ng `libregexp`, and retain the four Darwin stored-value fixture updates. |
| Provider traffic       | Use local mocks only. Run with an empty home, a fresh temporary directory and a clean environment.                                 |

The owned paths are the three branches' changes under `cpp/gateway/`, the two macOS build entry
points and this report. The workflow, TypeScript, the rest of `cpp/`, live providers and deployments
are out of scope. Build products and upstream dependencies belong in scratch outside the checkout.

The acceptance gates are `python3 cpp/gateway/tools/links.py --check`, the date, string, Codex quota,
Responses request, clock, filesystem and stored-value tests in `release` and `asan`, and
`bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4`. Check shell and Python syntax for
the macOS build changes on Linux. The Linux runner cannot execute either macOS build. GitHub write
access is unverified. Stop after the local gates and follow-up handoff, with the remaining evidence
recorded here.

## Merge and build changes

| Follow-up                           | Fetched tip                                | Merge commit                               |
| ----------------------------------- | ------------------------------------------ | ------------------------------------------ |
| C++20 UTC calendar                  | `a9ba3f3dd255d487117b319bd907b87924d2d5af` | `30c02865b06b912adf5c9db3183c55fc40dd7f2f` |
| Byte comparisons and string cleanup | `76bf1b523f95f2c690fb21ce08d738b8e9686c39` | `24d1ad4709028b3f1ba4466fa7fd0121148a3ac6` |
| Apple Silicon build and fixtures    | `e8b8bbc32920a4d9d345485d9c6569ddad8d41dc` | `96ae7d3a4db2026280d887f443d2d48d2bed8277` |

The only conflict was in `docs/DEVIATIONS-FROM-BUN.md` during the string merge. The resolution kept the
existing integration guidance, the UTC calendar section and the protocol string section. The date and
macOS merges needed no manual resolution.

The merged `tools/macos-build.sh` archives all six registry entries and applies the registry's GNU C11
flags to QuickJS-ng's `libregexp.c` and `libunicode.c`. The production and runtime test closures both
select `libuos_vendor_quickjs-libregexp.a`. Commit `8f693261eadd24150fbc98bb81a6ee9e3544099f` makes the
packager require the complete archive set from the clean-source receipt, then links every registered
archive and records its version and license. A comparison of the Darwin fixtures confirmed that all
50 verification and 28 bootstrap inputs are intact. Four malformed-JSON cases still require
`SyntaxError` and now expect the integrated parser's `Invalid JSON` message.

## Linux verification in progress

Ubuntu 24.04.5 ARM64 has four CPUs and 15 GiB RAM. The pinned dependencies, build objects, logs and receipts
are in `/var/tmp/cppgw-followups-1-20261118.HlJk57Xn`. `python3 cpp/gateway/tools/links.py --check`
passed before the compile with 450 resolved test paths. The cold release and sanitizer object builds
are expected to take tens of minutes each. Failures retain logs and receipts for diagnosis before a
focused retry. A GitHub dry-run push failed because the runner has no write authentication.
