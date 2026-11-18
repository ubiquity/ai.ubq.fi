# Native C++ gateway final integration

## Work contract v1

Merge the reviewed release integration and then the regex and runtime cleanup as ordinary two-parent merges.
The starting checkout is `45118332b287b7ba9534bb66ab1518f1990a9d9e` on `agent-job`, at the published port branch tip.
Ubuntu 24.04.5 on Linux ARM64 supplies four cores, 15 GiB of RAM, Bun 1.4.0 and passwordless namespace setup.

| Surface | Required behavior |
| --- | --- |
| JSON, HTTP and WebSocket | Retain simdjson, the 128-container JSON limit, llhttp and Beast. Preserve request and response formats, statuses, delivery and durable settlement. |
| Regex and both KV codecs | Use QuickJS-ng libregexp. Preserve malformed-value rejection, stored bytes, graph bounds and the typed values offered by Bun. |
| Filesystem and numbers | Use standard filesystem paths for trusted resources. Retain request path validation and authorization, numeric formatting, quota and paid accounting. |
| Vendors, workflows and packaging | Use a single registry for dependencies and a single workflow for Release, sanitizers and robustness. Preserve vendor bytes, tests from both sides, target-specific packaging, and default Linux confinement. |

The integration owns `cpp/gateway/` and the C++ gateway workflows. TypeScript, the other C++ projects, production
services, credentials and live provider access are outside the work. Dependency preparation and outputs use `/var/tmp`.
Tests use fresh homes and temporary directories, an empty environment with allowlisted settings, real fixture isolation
and loopback mocks. The four reported merge conflicts, final suite totals and package and fuzz outcomes are verified below.

Acceptance requires all four `ci-build.sh --config release --shard N/4` selections, a covering `--config asan` selection,
`python3 cpp/gateway/tools/links.py --check`, `actionlint`, two 600-second workflow fuzz campaigns and
packaged native versus Bun agreement on all 186 cases. The stop condition is a clean, committed tree preserving both
incoming merge tips, current guides, exact receipts and a record of any remaining blockers.

## Incoming history

| Input | Fetched tip | Prior Linux ARM64 Release result |
| --- | --- | ---: |
| Release integration and packaging | `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` | 450 binaries, 5,104 cases |
| Filesystem, numeric and QuickJS-ng integration | `9acb016ae9900ffac255b7f3cec02c735f6ddeda` | 448 binaries, 5,095 cases |

Both inputs include `af7a0036b823ef2357c26b9b5f03f2677796b775`. The first input descends from the starting tip.

| Merge | Commit |
| --- | --- |
| Release integration into the port branch | `fbbfef06689d0b1bd82abecae638062a8c19d5c8` |
| QuickJS-ng and runtime cleanup into the integrated tree | `ecce29dc92c627b669680b557a5dec3cd7124dea` |

The regex merge keeps both sets of deviation notes, combines the vendor registrations and fuzz targets, and uses the
unified CI workflow's full sanitizer inventory. The retired standalone sanitizer runner stays deleted. Work resumed
from these two committed merges and the package build that was already running against the merged source.

[Release integration findings](cppgw-integrate-f1-20261118.md) · [Regex and runtime integration findings](cppgw-integrate-s3b-20261118.md)

Runner scratch is `/var/tmp/cppgw-integrate-f2.zcCiN79E`. Dependency preparation, compilation and verification use
persistent execution handles. Logs and receipts remain in scratch.
