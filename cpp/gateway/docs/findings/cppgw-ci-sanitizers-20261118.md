# C++ gateway sanitizer and robustness CI

## Work contract v1

Extend the Ubuntu 26.04 ARM64 gateway checks to run the same test inventory in Release and with ASan, UBSan and LeakSanitizer. Discover every robustness target from `fuzz/targets.json`. Pull requests to `claude/cpp-gateway-port` and pushes to agent result branches run five-minute campaigns. Pushes to the port branch run 40-minute campaigns.

Keep the existing Release triggers, checks, fixture isolation, read-only workflow permissions, per-ref cancellation and evidence uploads. Limit changes to the owned C++ workflow, CI driver, runner and documentation paths. The task explicitly calls for removal of the retired `tests/ci-sanitize.txt`. Application code, test fixtures, the robustness registry and deployment are outside this change.

Qualify the workflow with `actionlint`, a cold full-inventory ASan shard and two five-minute robustness campaigns on the disposable x86_64 Linux runner. Use the driver's extra compiler flags to supply the missing `HEX` constant for local qualification. Record inventory coverage, exact commands, timings, exits and any upstream defects. Retain namespace isolation and run with fresh home and temporary directories and a credential-free environment. Four shards for both configurations are the measurement candidate within the 240-minute job limit.

The parent checkout starts at `1769741ee6d408f214c9c9dd4d8bcc7f5180a904`, tree `60acf0c92c9dbc99c34cbacb3f65478ec1192472`. The known `src/rt/crypto.cpp` compile blocker and the existing x86_64-specific fixture failures are outside the owned paths. Checkout and hosted delivery status will be recorded after the local gates finish.
