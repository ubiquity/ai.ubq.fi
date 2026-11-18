# File URL conversion and trusted resource paths

## Work contract v1

Resolve packaged static assets and the offline codec helper from the configured resource root, with the existing working-directory fallback in fixtures. Resolve development startup inputs from the source file and permissions inputs from the executable. Preserve the external POSIX file URL checks, SQLite URI flags, startup checks, helper pins and coroutine lifetime rules.

The brief assigns the file URL adapter regions in `rt/url`, static resource resolution in `static.cpp`, and the path construction regions in `kv_conversion/legacy_codec.cpp`, `storage/native_addon.cpp`, `runtime/native_kv_bootstrap.cpp` and `runtime/permissions_linux.cpp`. Their tests and an appendix to `DEVIATIONS-FROM-BUN.md` are in scope. Provider behavior, stored-value codecs, broader URL parsing, deployment, and the TypeScript gateway are outside this work.

The starting checkout is `7bde4d44a00ed5322901a59e98f80a5f60bafa50` with tree `0ded9458c6cbf069d117bdfabeaaa946810ea6ab`. The incoming [Ada receipt](cppgw-std-ada-20261117.md) reported 441 release binaries and 5,025 passing cases. Its sanitizer run reached the same totals after three runner-specific retries. Local baseline results and the exact toolchain will be recorded below.

Acceptance uses `bash cpp/gateway/tools/ci-build.sh --config release` for the baseline and final tree, and `--config asan` for the final link closures. Every test links `src/rt/url.cpp`, so the final profiles run the full registry. The listed asset, conversion, and startup tests cover the changed callers. Reserved-byte path checks cover the remaining file URL boundary. The runner uses disposable fixture state, an empty home, and Linux network, mount, and PID namespaces. The work ends after reconciling the baseline and final receipts, scope, line counts, and branch handoff.
