# C++ gateway string helpers

## Work contract v1

Use standard C++20 string-view operations for fixed ASCII protocol tokens and
the converted UTF-16 fallback. Retire the uncalled `codePointAt` and `trimStart`
APIs with equivalent fixture coverage. The work owns the row 32 string regions,
the Codex quota header-name checks, the Responses message-type prefix, their
fixtures, and an appendix in the deviations ledger. The UTF codec (including
`fromCodePoint`), ICU, percent escapes, regex, provider streaming, build tools,
TypeScript and workflows are outside this change. This task uses local mocks
and has no deployment or production state effects.

| Surface                  | Required behavior                                                      |
| ------------------------ | ---------------------------------------------------------------------- |
| Codex quota header names | Remove the same `x-` header families and preserve unrelated headers.     |
| Responses item type      | Preserve type classification and invalid-request field and status.     |
| Protocol ASCII tokens    | Match the same bytes next to arbitrary UTF-8, including malformed data. |
| User text and regex      | Preserve UTF-16 indices, coercion, lone surrogates and output bytes.     |
| Expansion and trimming  | Preserve bounds, whitespace, empty input and stable output.             |

Resolve caller and fixture coverage from this revision before changing the API.
Measure the retained indexing paths and request-path work on the ARM64 runner.
The acceptance surface includes the six named release fixtures, the full release
inventory, and the ASan+UBSan Links closure. Every test links `src/rt/string.cpp`,
so the sanitizer closure is also the full inventory. Stop after reconciling
both suites with the baseline, reporting source lines and timings, and committing
the result. External publication depends on the repository's GitHub access.

## Starting point

The regex integration is already present at `51408a849928ae47a0a9b3fc1f6da02dbf80dcc9`.
The starting branch resolves to `9acb016ae9900ffac255b7f3cec02c735f6ddeda`, tree
`72b5114c61fd335e63f43202708a419f01b11df0`. Its previous Linux ARM64 receipt
records 448/448 release binaries and 5,095/5,095 cases. The two standalone
packaged conformance failures recorded there are outside this test inventory.
[Stage 3b findings](cppgw-integrate-s3b-20261118.md)

The owned production files start at 1,485 physical lines. This runner is Ubuntu
24.04.5 on ARM64 with four Neoverse-N2 vCPUs. The local baseline build uses a
detached checkout of the starting commit and scratch dependencies and evidence
under `/var/tmp/cppgw-slim-strings.75hrNmNR`.
