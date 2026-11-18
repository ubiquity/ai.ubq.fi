# Integrated C++ gateway follow-ups on Apple Silicon

## Work contract v1

Qualify the integrated C++ gateway with Apple Clang and libc++ on arm64 macOS. The runner's clean `agent-job`
checkout starts at `824687e6b5e2f561ebc588f3456f83e33e1b3228` (tree `3cf7060f40468468b031b292e551f945e2f141c0`),
the tip of `agent-jobs/cppgw-followups-1-20261118-44398005611-1`.

- Compile the native source and registered vendors, including QuickJS-ng libregexp, and link all three executables.
- Verify the C++20 calendar, helper cleanup and filesystem paths in the registered fixtures. Preserve validation,
  authentication, wire and storage contracts, and accounting.
- Use the Darwin Seatbelt boundary, an empty home, a fresh temporary directory and a credential-free environment.
  Provider fixtures use loopback mocks.
- Run the macOS workflow's Python build entry point and check the extracted arm64 gateway's `--version` against the
  source commit.

Owned paths are `tools/macos-build.sh`, `docs/MACOS.md`, this report, Darwin-guarded paths under `include`,
`src` and `tests`, and standard-library portability fixes that leave Linux behavior intact. The Python packager,
vendor sources and registry, workflow files, TypeScript, other `cpp/` directories and service activation are outside
this unit. Keep all build output and dependency downloads in external scratch.

Use `bash cpp/gateway/tools/macos-build.sh` with `--suite rt --require-clean`, then `--suite all --require-clean`,
then `python3 -B cpp/gateway/packaging/macos/build.py` and an offline `--version` check. For any changed source,
rerun its affected fixture before the full gate. Check the shell driver with `bash -n` if edited. Check changed
Markdown with `bun --no-env-file run format:check` and the final diff with `git diff --check`.

The new calendar, regex vendor and package linkage are unqualified on this host. Use their observed diagnostics
to decide whether a fix is needed. Budget 10 minutes for dependency preparation, 25 for the cold runtime build,
35 for the full suite, and 30 for packaging. Keep one build driver active at a time and follow compile progress
and the terminal receipts. Reuse valid objects after a repair and retain each failed gate's first diagnostics.
Stop with exact passing suite and package receipts or a diagnosis of each remaining failure.
