# C++ gateway UTC calendar refactor

## Work contract v1

Use C++20 calendar and duration types for UTC extraction and normalized date arithmetic. Preserve the wide
TimeClip range, the ISO and HTTP wire formats, input truncation, and the current parser's provider decisions.
Remove fixture-only mutation after checking all callers. The owned paths are the calendar, formatting and Date
implementation around the existing parser in `src/rt/date.cpp`, their declarations in `include/uos/rt/date.hpp`,
the relevant `tests/rt/date_test.cpp` cases, the deviations appendix and this report. Clock and timers, provider
routing, persistence, other tests, build files, TypeScript and deployment are outside this scope.

| Surface | Required behavior |
| --- | --- |
| ISO timestamps and UTC day boundaries | Preserve millisecond truncation, offsets, negative epoch values, leap dates and signed extended years. |
| HTTP Date and Codex Retry-After | Preserve the weekday and canonical formatting, exact parse round-trip, classification and applicable quota reset. |
| Filesystem and stored timestamps | Retain the existing filesystem, migration and persisted-value comparisons. |

Record the full Release baseline before source edits. Run the named date, filesystem, clock, Codex 429,
migration and delivery selections, plus every link closure in Release and ASan/UBSan. The CI driver links the
date object into every target. Each fixture runs inside the driver's Linux namespaces using local mocks, an
empty home, a fresh temporary directory and a credential-free environment. Downloads and build products go
under `/var/tmp/cppgw-slim-dates.bj9TzzhL`. The cold baseline and sanitizer builds each have a 90-minute
budget with four workers. Use their object logs, incremental `results.tsv` and final receipts for progress and
results. Preserve failed evidence and retry a recoverable prerequisite or resource failure with the same inputs.
The stop condition is matching baseline and candidate coverage, exact commit and tree receipts, and a clean Git
handoff or a documented external blocker.

## Starting point

The clean `agent-job` checkout matches `origin/agent-jobs/cppgw-integrate-f1-20261118-44343309770-1` at
`2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`, tree `d4f9c3beb10816358c85e92c1c231421fa35fa48`. The incoming
F1 report records 450 passing Release binaries and 5,104 passing cases. Its two shared validation failures
belong to the separate paired conformance suite. This runner is Ubuntu 24.04.5 on AArch64 with four CPUs and
15 GiB RAM. Local baseline results, the fixture-only API audit and publication access are pending.

First-party production counts use physical lines, including comments and blanks. `src/rt/date.cpp` starts at
353 lines and `include/uos/rt/date.hpp` starts at 71, for 424 lines across the owned production files.
