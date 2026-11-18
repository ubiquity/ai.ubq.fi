# Boost.Process v2 migration

## Work contract v1

Replace the C++ gateway's POSIX subprocess launcher, pipe setup and exit watcher with Boost.Process v2 and Boost.Asio. Preserve the source-compatible API and the callers' argv, environment, working directory, stdio, exit, signal, and child-ownership behavior. Child setup must preserve the existing execution boundary, including signal reset and descriptor handling. Record platform-specific behavior and any intentional departure from Bun.

The affected surfaces are direct process calls in the offline KV conversion/proof code and the Codex smoke tool, plus all test binaries through their common runtime link list. Linux ARM64 is the execution target for this job. Darwin compatibility is a source and library qualification item.

Edits are limited to the owned process source, header, test, and documentation paths in the task. Provider traffic, deployment, build infrastructure edits and unrelated repairs are out of scope.

The baseline is commit `b54357a92c0f2669335d83f04047c3f7de32f925`, tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`. The three owned production files contain 1,001 lines (165 + 610 + 226), and `process_test.cpp` contains 465.

Acceptance requires the full Linux ARM64 release and ASan/UBSan suites from `bash cpp/gateway/tools/ci-build.sh --config release` and `bash cpp/gateway/tools/ci-build.sh --config asan`. The driver's all-runtime link rule makes every registered binary an affected test. Both gates use the driver's namespace isolation and local fixtures. Library linkage, Darwin wait behavior and the exact test count are being checked before the implementation. The job stops after the final-tree gate receipts and branch handoff are recorded, or when a prerequisite prevents further verification.
