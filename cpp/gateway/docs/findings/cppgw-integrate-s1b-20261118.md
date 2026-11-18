# C++ gateway runtime slimming integration, stage 1b

## Work contract v1

Integrate the four accepted slimming branches into one stage 1b branch. Start at Headers/multipart tip
`2e4b3dce72bcb955060ba46547cf0d75b6b1c691` and preserve the three incoming tips as second parents of ordinary merge
commits. The four lanes share base `6393434b110e2d1afe93e6c7005848219336d9b9`.

| Lane                          | Required behavior                                                                                               |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------- |
| Headers and inbound multipart | Keep header validation and wire projection, ordered upload parts, endpoint bounds, and replay and scrub timing. |
| Console                       | Keep captures, severity streams, terminal records, and bounded diagnostic output.                               |
| Environment and filesystem    | Read process variables and explicit configuration inputs. Keep path and file behavior used by gateway callers.  |
| Ordered containers and gzip   | Keep observable ordering and gzip handling for replay, including size and decode checks.                        |

Changes are limited to merges, necessary integration edits under `cpp/gateway/`, and this report. Other agents own
parallel port work, including repairs to the three known application fixtures. Production activation, credential
operations, provider spend, workflows, TypeScript changes, sibling C++ code and vendored libraries are outside scope.
Qualification uses an empty test HOME, fresh TMPDIR, a credential-free environment and the driver's real loopback-only
Linux boundary.

Run the four `bash cpp/gateway/tools/ci-build.sh --config release --shard i/4` gates and focused
`--config asan` tests for the four lanes. Count physical production lines from Git under `src/rt` and `src/runtime`.
Trace native startup, offline CLIs, test setup and the production TypeScript launcher to establish the `.env` contract.
The merge dry runs and first compiler receipts will establish actual conflicts and runner compatibility. Prior runs
took tens of minutes per profile. Watch compile logs and incremental results in runner scratch and retry recoverable
prerequisite failures. The stop condition is a three-merge history, a signed-off source identity, four release receipts
with exactly the three baseline failures, passing focused sanitizer checks, and this completed report.
