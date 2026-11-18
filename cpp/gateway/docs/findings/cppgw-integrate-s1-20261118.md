# C++ gateway library integration, stage 1

## Work contract v1

Start at small-helpers tip `6393434b110e2d1afe93e6c7005848219336d9b9`.
Merge the accepted ICU branch first and the accepted llhttp server branch
second, with two-parent commits. Own the shared registries, build paths and
fixture reconciliation needed under `cpp/gateway/`. Preserve the accepted
vendor bytes and leave TypeScript, hosted workflows, other `cpp/` paths and
parallel integration lanes outside these edits. The handoff is the stage 1
branch for the maintainer's next integration.

| Surface               | Acceptance                                                                                                                            |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| Unicode               | ICU casing, collation and IDNA with the build's actual Unicode data version                                                           |
| HTTP server           | Strict standard request parsing and original status, size, framing, raw-field, dispatch and delivery contracts for supported requests |
| Date and Headers      | Dates received by gateway callers, rejection of malformed input and bounded error messages                                            |
| Other runtime helpers | Preserve the merged helper branch's console, path, filesystem and environment contracts                                               |
| Application           | Unchanged authentication, validation, stored data, accounting and provider routing                                                    |

Resolve the supported date formats from TypeScript and C++ call sites before
editing date expectations. Check each wire difference against HTTP and URI rules
and repository client evidence. The actual merge conflicts, compiler behavior
and fixture results are open at discovery.

Prepare the ARM64 compiler, Boost 1.92.0 and SQLite 3.54.0 outside the
checkout. Use the same ICU 78.3 as the selected native release if available.
Compile C vendor files with the C compiler. Test with a credential-free
environment and the driver's isolated loopback fixtures. Verify all four
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4` selections,
the affected sanitizer selection, vendor archive registration and the changed
documents. Stop at a clean stage branch with both merges, exact receipts and the
runtime source line-count comparison.

## Progress

The runner is Ubuntu 24.04.5 on AArch64 with four CPUs. The checkout's
`agent-job` branch matches the specified small-helpers tip. Both incoming
branch tips have been fetched from `origin`.

## Continuation and merge resolutions

The resumed checkout started at `e6bfbcab395600c153bf695e25854fff8462da90`.
`git log --first-parent --merges -2 --format='%H %P %s'` and a fresh fetch
verified both two-parent commits and their accepted parent tips.

| Order | Merge                                      | Accepted second parent                                   |
| ----- | ------------------------------------------ | -------------------------------------------------------- |
| 1     | `d7976742f4aa351478d6b975c47c0b7c0399d28f` | ICU `b54357a92c0f2669335d83f04047c3f7de32f925`           |
| 2     | `e6bfbcab395600c153bf695e25854fff8462da90` | llhttp server `441f5ff457d03ce47b472f0b8025274ac38b543b` |

`git show --remerge-diff` found one add/add documentation conflict in each
merge, both in `docs/DEVIATIONS-FROM-BUN.md`. The first merge kept the helper
sections and appended ICU case mapping and collation. The second preserved both
sides with conflict markers. Commit `0bc13c83` removed the markers and duplicate
title, and retained every section. The fuzz registry contains both new targets.
Git reported no textual conflicts in CMake, the vendor registry or `tests/LINKS.txt`.
Both new standalone tests resolve through their existing Links declarations.

The inherited llhttp merge already incorporates the CMake, SDK unit, Mac,
portable CI and benchmark archive changes. `tools/vendor_sources.py` and
`cmake/vendors.json` resolve the same three llhttp files and consumer closures.
The portable driver uses separate `CC` and `CXX` rules and keeps the C++ warning
set off vendor compilation. CMake configured with `-DUOS_GATEWAY_TESTS=ON` on
this runner and built `uos_vendor_llhttp` with the C compiler. `ar t` returned
`llhttp.c.o`, `api.c.o` and `http.c.o`. `git diff --exit-code` against the
accepted llhttp parent found identical bytes throughout `third_party/llhttp`.

## Date formats checked before fixture changes

The audit used `rg -n -i 'retry-after|rate.?limit.reset|resets_at|last_refresh|expires_at'`
and `rg -n 'Date::parse|Date\.parse|new Date\('` in `src/`, `serve.ts` and the
corresponding C++ modules. Local provider and native-auth fixtures supply the examples.

| Source                                                                                | Formats and existing handling                                                                                                                                                                                           |
| ------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Codex, Claude Code, Lithos and Voyage `Retry-After`                                   | Delay seconds take the numeric path. Absolute values use IMF-fixdate such as `Mon, 16 Nov 2026 06:00:00 GMT`. `authority/native-codex-429` additionally requires a canonical round trip for a durable reset identity.   |
| Lithos and Cerebras reset headers                                                     | `provider/lithos-rate-limits` parses explicit duration units such as `3.23s`, `750ms` and `1m30s`. `provider/cerebras-rate-limits` parses numerical windows separately.                                                 |
| Codex 429 reset body and JWT access expiry                                            | `authority/native-codex-429` and `codex/auth` read epoch seconds as JSON numbers and convert to milliseconds.                                                                                                           |
| Codex native `last_refresh`, banked-credit `expires_at`, and Claude usage `resets_at` | The native-auth generation and credit fixtures use offset-bearing ISO 8601, including fractional seconds. Claude usage snapshots cover ISO and IMF-fixdate, and retain raw reset text when a deadline cannot be parsed. |
| Persisted observation and expiry times                                                | `observed_at_ms`, `expires_at_ms` and numeric KV Date nodes use epoch milliseconds directly.                                                                                                                            |

The inherited date code covers these formats. The audit found the three synthetic
`last_refresh` spellings and the single-character Claude reset in the differential
fixtures only. They need no change to the production date grammar.
