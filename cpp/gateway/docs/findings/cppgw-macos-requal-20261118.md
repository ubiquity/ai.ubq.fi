# Apple Silicon requalification after simdjson, simdutf and ICU

## Work contract v1

Requalify the C++ gateway on arm64 macOS from source commit
`b54357a92c0f2669335d83f04047c3f7de32f925` (tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`).
Use Apple Clang and libc++, the registry-selected vendor sources and Homebrew ICU 78, the Linux major.
The native SQLite fixtures require the separately prepared 3.54.0 library.

| Surface | Acceptance |
| --- | --- |
| Production and vendor build | Every production translation unit compiles with the port warning set, vendor sources compile on their own warning flags, and production links resolve the selected archives and ICU. |
| Runtime and application | Run the runtime suite first, then every registered binary in the full suite. Record real harness totals and first diagnostic lines for any failure. |
| Network and platform | Seatbelt permits loopback fixtures and denies external networking. Test environments have private empty homes and fresh temp directories. Linux contracts and security decisions retain their existing semantics. |
| Documented startup | Measure the gateway's idle RSS if the production launcher admits Darwin. Record its exact blocker otherwise. |

Owned paths are `tools/macos-build.sh`, `docs/MACOS.md`, this report, Darwin branches in
`include/`, `src/` and `tests/`, and standard-library portability changes with identical Linux behavior.
Production activation, provider calls, credentials, live state, deployment, shared Linux build
tools, workflows, vendor source changes, other agents' paths and application TypeScript are outside
this unit. Use the native compile and suite gates plus script syntax, changed Markdown formatting
and `git diff --check`.

The runner reports macOS 26.7.2 build 25G332 on arm64, Apple Clang 21.0.0
(`clang-2100.1.1.101`), 3 CPUs and 7 GiB of RAM. The starting script already knows the vendor and
ICU metadata. The first compile will establish whether current sources and dependencies still
match that wiring. The documented launcher has Linux-native requirements to verify from source
and the executable. Initial build and test budget is 30 minutes, using three compile slots and
one build handle at a time. Object-count progress and external logs identify the active phase.
Retain diagnostics and use incremental reruns for owned repairs. Stop local qualification after
the clean full result and any exact remaining failures have been recorded.

## Progress

Read the current CI, build, runtime, OSS replacement, porting and macOS instructions. Starting
from a clean checkout. Build outputs, logs and dependency downloads are external to the checkout.
