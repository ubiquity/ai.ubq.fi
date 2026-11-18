# C++ gateway library integration, stage 1

## Work contract v1

Start at small-helpers tip `6393434b110e2d1afe93e6c7005848219336d9b9`.
Merge the accepted ICU branch first and the accepted llhttp server branch
second, with two-parent commits. Own the shared registries, build paths and
fixture reconciliation needed under `cpp/gateway/`. Preserve the accepted
vendor bytes and leave TypeScript, hosted workflows, other `cpp/` paths and
parallel integration lanes outside these edits. The handoff is the stage 1
branch for the maintainer's next integration.

| Surface | Acceptance |
| --- | --- |
| Unicode | ICU casing, collation and IDNA with the build's actual Unicode data version |
| HTTP server | Strict standard request parsing and original status, size, framing, raw-field, dispatch and delivery contracts for supported requests |
| Date and Headers | Dates received by gateway callers, rejection of malformed input and bounded error messages |
| Other runtime helpers | Preserve the merged helper branch's console, path, filesystem and environment contracts |
| Application | Unchanged authentication, validation, stored data, accounting and provider routing |

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
