# Ada 4.0.0 URL replacement

## Contract v1

Replace the URL parser, serializer, IDNA conversion and form handling in OSS plan rows 4 and 5. Use the pinned
Ada release as the behavior reference. Preserve the public C++ API, linked search parameters, gateway origin and
routing validation, and the Node POSIX file URL contract in row 34. TypeScript, other C++ lanes, hosted workflows,
credentials and deployment are outside this task.

| Surface | Implementation and invariant |
| --- | --- |
| Absolute and relative URLs | Ada parses, resolves and serializes. Port constructors throw the existing typed error. |
| IDNA and IP hosts | Ada canonicalizes hosts. The adapter checks known admission gaps for malformed A-labels, Bidi and joiners. |
| Query strings | Ada parses, sorts and serializes. C++ ownership keeps href updates and the live query list synchronized. |
| File paths | The Node POSIX adapter retains local-host and encoded-slash checks and literal filename handling. |
| Gateway callers | The existing link closures test HTTP, redirects, authentication, provider routing and storage without live provider access. |

Writes are limited to the URL sources and tests, Ada assets and vendor registration, the URL fuzz target and
seeds, and this report and deviation entry. The baseline is
`093a9e0511e9d0deae5495dc324e53ff4dccd719`, tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`. Bun
1.4.0 is available to confirm the old behavior. The [earlier qualification](cppgw-oss-ada-url-20261117.md)
identified the expected behavior differences and the validity gaps to check.

The acceptance surface is strict plain and ASan/UBSan builds of the URL component and its consumer closures,
the original vectors with classified expectation changes, a 120-second sanitizer fuzz run and a paired
`-O2` benchmark. Build objects, probes and logs live under `/tmp/cppgw-std-ada-20261117`. Each
test gets an empty home, fresh temp directory and credential-free environment. On this four-core ARM64 runner
the complete dual-profile build can take tens of minutes. Compile each translation unit once per profile,
record per-test outcomes and retry recoverable prerequisite or toolchain failures. Stop after the gates and
report are complete, or record the exact partial counts and blockers if the runner cuts off verification.

## Initial evidence

- Ada `ada.h` and `ada.cpp` matched both task SHA-256 pins. The Apache-2.0 notice comes from the same
  release tag. See the [vendor provenance](../../third_party/ada/PROVENANCE.md).
- This Ubuntu 24.04 ARM64 runner has GCC 14.2.0 and Bun 1.4.0. Boost.JSON and Boost.Container were built
  statically from the official Boost 1.92.0 archive. SQLite 3.54.0 was built with `SQLITE_DQS=3` for
  the existing consumer fixtures.
- `origin/development` at `0c46932008125e18704fc019282935cccf2cb9df` has no `cpp/gateway`. The port
  checkout starts at `origin/claude/cpp-gateway-port`. A credential-free dry-run push to the task branch
  exited 128 because the remote requires a GitHub username. The local job branch is the handoff.

## Verification

Pending implementation and runs.
