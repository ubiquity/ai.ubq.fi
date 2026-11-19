# C++-only gateway branch

## Checklist

- [x] Start the report on the supplied Gemini integration tree.
- [ ] Merge the residuals branch and record the request-allowlist revert under STATE A.
- [ ] Build and run the selected State A request tests.
- [ ] Inventory tracked language sources, toolchain commands and runtime dependencies.
- [ ] Remove the historical comparison and formatter.
- [ ] Remove unnecessary package runtime artifacts and offline tooling.
- [ ] Update CI and current operating documents.
- [ ] Add the ARM64 cutover checklist and runtime environment table.
- [ ] Verify registrations, tracked files, workflows and whitespace.
- [ ] Build the gateway and run the selected release tests in native isolation.
- [ ] Build and verify the portable package.
- [ ] Record final results and leave the checkout clean.

## STATE A

Starting tree: `13502438` (`agent-jobs/cppgw-gemini-merge-20261119-44556239421-1`).
The residuals fetch completed with `git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1`.

## Scope

Local verification uses qualified native dependencies and the repository's loopback-only fixture driver. Build outputs and logs stay outside the checkout. The hosted matrix is the full-suite verification path.
