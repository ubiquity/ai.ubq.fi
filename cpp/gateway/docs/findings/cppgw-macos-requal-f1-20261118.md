# Integrated native gateway on Apple Silicon

## Work contract v1

Qualify the integrated F1 tree with Apple Clang and libc++ on arm64 macOS. Build the five registry-selected vendors,
every production translation unit and the three native executables. Run the runtime tests first, then the complete
native suite. Build the macOS release archive with the workflow entry point and run its packaged `--version`.

| Surface | Acceptance |
| --- | --- |
| Native build | The Darwin driver verifies vendor hashes, uses C11 for C and C++20/libc++ for C++, and links each test closure. |
| Gateway behavior | Preserve functional and security decisions. Use Darwin-specific expectations for observed OS quirks. |
| Test isolation | Keep the real Seatbelt network boundary, empty private `HOME`, fresh `TMPDIR` and credential-free test processes. All inference fixtures use local mocks. |
| Release package | Build through `python3 -B cpp/gateway/packaging/macos/build.py` and verify the arm64 artifact and build identity. |

Owned paths are `tools/macos-build.sh`, `docs/MACOS.md`, this report, Darwin paths in `include`, `src` and `tests`,
and standard-library portability fixes with unchanged Linux semantics. Shared build and packaging code, vendors,
workflows, TypeScript, live services, provider credentials and production state are outside this unit.

Starting commit is `2b23fb68` on the runner's clean `agent-job` checkout. The native driver already consumes the
registry. The first runs will establish whether compiler flags, test assumptions and package dependencies still fit
the integrated tree. Changed C++ is checked by the native compile and affected fixture, then the runtime and full
suites. Script syntax, changed Markdown formatting and `git diff --check` cover the other owned paths.

Budget 20 minutes for the cold runtime qualification and 30 minutes each for the full suite and release package,
using external scratch and one native driver at a time. Follow compiler counts and per-binary receipts, retain the
first diagnostics and reuse verified objects after bounded repairs. The local stop condition is the package version
receipt and a clean full-suite receipt or an exact record of each remaining failure.
