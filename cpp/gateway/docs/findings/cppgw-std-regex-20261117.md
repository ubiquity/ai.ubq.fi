# QuickJS-ng libregexp qualification

## Continuation contract v5

The incoming `ab5c6b8815c0d286e324267f025e2a2ad5b9a0bd` tree
`699073d8a39533704addbde1d8c0998cc3a2a980` already replaces the C++ compiler,
matcher, and Unicode tables with QuickJS-ng 0.18.0. This continuation qualifies
that implementation and repairs any remaining gateway regressions within the
regex lane. The inherited implementation, earlier receipts, and prior failure
analysis are recorded in [the previous handoff](cppgw-std-regex-r1.md).

| Surface                             | Required behavior and check                                                                                                                       |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| Regex API                           | Strict C++20 port flags, original vector coverage, classified QuickJS results, UTF-16 and byte offsets, captures, and stateful methods.           |
| Image and authentication validation | Preserve the gateway's accepted payloads and rejection decisions, including large inline images.                                                  |
| SSE, redaction, and compaction      | Preserve segmentation, capture spans, replacement, and large-input completion.                                                                    |
| KV interchange                      | Preserve valid source and flags through round trips and reject malformed expressions. QuickJS supplies the regex syntax diagnostic.               |
| Resource limits                     | Use the existing deadline, poll, allocation, and compiler-stack limits. Verify with regression cases, ASan/UBSan, and the 120-second fuzz target. |

The owned paths are the regex runtime and public header, its vendor build
registration, regex-only tests and fixtures, fuzz target and corpus, and the
findings and deviations. Application modules, other tests, shared build drivers,
TypeScript, workflows, and deployment are outside the continuation. The portable
driver currently accepts one C++ translation unit per vendor, while QuickJS has two
C translation units. Use a scratch compiler launcher and a separately compiled
Unicode object to run the unchanged driver. Keep production and runner outputs
outside the checkout and run test children in the driver's OS fixture boundary.

Run `bash cpp/gateway/tools/ci-build.sh --config release rt/regex_test.cpp` before
the four release shards, then run the ASan/UBSan configuration. A cold build may
take tens of minutes per configuration. Track Ninja object counts and per-test
results on the running build, keep failure logs, and use focused isolated reruns
to diagnose discrepancies. Run a two-minute fuzz campaign and compare the parent
and candidate on the same release toolchain and fixtures, including allocator
calls. The handoff is ready when those results, any remaining scope blockers, and
the exact source identities are committed to the job branch.

## Runner and preflight

The runner is Ubuntu 24.04.5, Linux AArch64, four CPUs, with Clang 21.1.8
from the LLVM apt repository and Bun 1.4.0. Boost.JSON and Boost.Container were
built as static libraries with `./bootstrap.sh --with-libraries=json,container`
and `./b2 -j2 toolset=gcc cxxflags=-fPIC link=static variant=release threading=multi
--with-json --with-container stage` from the official Boost 1.92.0 source
archive (SHA-256 `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`).
The suite used ICU 78.3 and SQLite 3.54.0 built from their official archives.
`SELECT "qt";` succeeded on the staged shared SQLite library built with
`CFLAGS='-O2 -DSQLITE_DQS=3'`. The other runtime dependencies came from Ubuntu
packages (OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, and Zstd 1.5.5).

The QuickJS-ng archive verified against
`28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd`.
`sha256sum -c SHA256SUMS` passed all eight entries, and each checked-in file
matched the bytes extracted from the archive. The portable driver compiled
369/369 release production translation units under the fatal port warnings.
The preflight regex binary linked but ran zero cases because the fixture's
private `/tmp` hid `libicuuc.so.78`. The rerun stages the ICU and SQLite
shared libraries under a separate `/var/tmp` directory and uses it as the
binary's runtime library search path. The driver keeps its private fixture
filesystem and loopback-only network boundary.
