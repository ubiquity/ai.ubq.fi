# Slim C++ gateway console

## Work contract v1

Base commit `6393434b110e2d1afe93e6c7005848219336d9b9`, tree
`89b42863ac7dd6a07610e2d15aa320efc42547af`. Replace the remaining
JavaScript-shaped inspection and substitution code from plan row 54 with
owned text, Boost.JSON fields and typed error diagnostics.

| Input or integration | Required behavior |
| -------------------- | ----------------- |
| Text and pre-serialized records | Literal bytes, argument order and one sink newline |
| Structured values | Shared safe JSON serialization, with bounded diagnostics |
| Exceptions | Diagnostic name, message, cause and existing OS fields |
| Log, info and debug | Synchronous capture and stdout forwarding |
| Warn and error | Synchronous capture and stderr forwarding |
| Nested captures | Method filtering, original args, stable reentrant observation and exception propagation |

The owned paths are the console formatter and its argument declarations,
the console fixture, the appended deviation and this report. The observer
implementation and callers, including terminal and telemetry events,
keep their integration seams. HTTP, persisted data, redaction decisions,
accounting, provider selection, and other lanes are outside this change.
The checkout lacks the linked keep audit, so the caller inventory comes
from the checked-in source and the brief. The existing fuzz caller and
three recorded out-of-scope failures are part of the compatibility check.

Verify the named fixtures with `bash cpp/gateway/tools/ci-build.sh`, then
run the complete ARM64 release and ASan/UBSan registries because every test
links the console translation unit. Record source identity, commands,
compilation and runtime outcomes, any baseline failures, and physical line
counts. Builds use scratch space and the driver's real OS fixture boundary.
This unit ends with committed console changes and exact verification evidence.

## Initial checkpoint

Linux AArch64, Ubuntu 24.04. The production files total 553 physical lines
(`console.cpp` 386, `console.hpp` 167). The console fixture is 482 lines.
GitHub runner job `44254409709`. Caller inventory and dependency setup are
in progress.
