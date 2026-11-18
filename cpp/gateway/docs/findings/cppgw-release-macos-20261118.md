# Apple Silicon C++ gateway release findings

## Work contract v1

Execute the Apple Silicon job's `run` blocks in workflow order, then prove two clean builds of the same commit
produce identical archives. Extract into fresh directories, exercise a local gateway health or version request,
check the Mach-O dependencies and third-party licences, and follow the Mac install and rollback instructions.
`actionlint` validates the workflow. Record the exact tree, commands, measurements and remaining boundaries.

Changes are limited to `packaging/macos/`, the workflow's `macos` job, the Mac sections of `RELEASES.md`, and this
report. Fix the Darwin build driver only if a reproduced failure requires it. Preserve Linux startup and the native
runtime tests. Use an empty home, fresh temporary directories, a scrubbed environment and local mocks for runtime
checks. Production service activation, database conversion and provider traffic are out of scope.

The starting commit is `23e097b75cd5c657797f5325ab4363b5bb27b828`, tree `531ff31ff2d1c3a9c226bb63b0ee038598027df7`.
GitHub Actions run `44307885223`, attempt 1, is a native arm64 runner with macOS 26.7.2 (25G332) and Apple Clang
21.0.0 (`clang-2100.1.1.101`). The prior release report covers its own Linux receipt. Mac linkage and the local
smoke surface are the outstanding acceptance checks.

## First run and repair

The original macOS install block passed in 21.17 seconds. The original build block exited 1 after 525.10 seconds,
with 367 out of 369 production translation units compiled. Apple Clang diagnosed `-Wsign-conversion` in the
vendored simdutf header included by `src/rt/base64.cpp` and in ICU headers included by `src/rt/string.cpp`. The
Darwin driver now marks those vendor inputs as system headers and records the vendored header hashes explicitly
in the object cache. First-party compiler flags and test expectations are retained.

The Mac archive exposes an offline version command backed by the packaged source commit. The Mac-only entrypoint
passes all other arguments to the existing gateway entrypoint. The workflow now checks the installed executable's
version from a fresh extraction under a network-denying Seatbelt profile.
