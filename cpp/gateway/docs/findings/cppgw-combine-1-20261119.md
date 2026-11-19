# Native gateway follow-up integration

## Work contract v1

Integrate the seven reviewed branches onto the F2 gateway in the supplied order. Each source tip must remain in the
combined branch's ancestry. The owned paths are this report, the native gateway files needed to resolve integration
issues, and the README and Bun-deviation guide totals. The job covers the combined branch and Linux ARM64 test fixtures.

| Area | Acceptance evidence |
| --- | --- |
| Cleanup and packaging | Inspect active build, test, and packaging references against the paths removed by the cleanup. |
| Standard library and Apple Silicon | Carry forward the helper changes, vendor build coverage, and the recorded Apple Silicon qualification. |
| Runtime, server, provider, and handler lifetimes | Compile the shared runtime with the four fixes and verify each new regression test is registered exactly once. |
| Integrated suite | Run `python3 cpp/gateway/tools/links.py --check`, every `bash cpp/gateway/tools/ci-build.sh --config <release or asan> --shard N/4` with real fixture isolation, and `actionlint` on the workflows. |

Starting tree `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. The test count, combined sanitizer behavior, and
publication status are pending. The gate requires all seven merge commits, both complete suites, and a record of
verification and outstanding work. Build and test outputs live outside the checkout, with clean test environments and
local mocks.
