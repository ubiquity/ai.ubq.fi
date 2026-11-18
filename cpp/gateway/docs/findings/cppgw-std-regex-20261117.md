# QuickJS-ng libregexp qualification

## Continuation contract v5

The incoming `ab5c6b8815c0d286e324267f025e2a2ad5b9a0bd` tree
`699073d8a39533704addbde1d8c0998cc3a2a980` already replaces the C++ compiler,
matcher, and Unicode tables with QuickJS-ng 0.18.0. This continuation qualifies
that implementation and repairs any remaining gateway regressions within the
regex lane. The inherited implementation, earlier receipts, and prior failure
analysis are recorded in [the previous handoff](cppgw-std-regex-r1.md).

| Surface | Required behavior and check |
| --- | --- |
| Regex API | Strict C++20 port flags, original vector coverage, classified QuickJS results, UTF-16 and byte offsets, captures, and stateful methods. |
| Image and authentication validation | Preserve the gateway's accepted payloads and rejection decisions, including large inline images. |
| SSE, redaction, and compaction | Preserve segmentation, capture spans, replacement, and large-input completion. |
| KV interchange | Preserve valid source and flags through round trips and reject malformed expressions. QuickJS supplies the regex syntax diagnostic. |
| Resource limits | Use the existing deadline, poll, allocation, and compiler-stack limits. Verify with regression cases, ASan/UBSan, and the 120-second fuzz target. |

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
