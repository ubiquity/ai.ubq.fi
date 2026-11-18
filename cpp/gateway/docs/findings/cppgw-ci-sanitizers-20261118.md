# C++ gateway sanitizer and robustness CI

## Work contract v1

Extend the Ubuntu 26.04 ARM64 gateway checks to run the same test inventory in Release and with ASan, UBSan and LeakSanitizer. Discover every robustness target from `fuzz/targets.json`. Pull requests to `claude/cpp-gateway-port` and pushes to agent result branches run five-minute campaigns. Pushes to the port branch run 40-minute campaigns.

Keep the existing Release triggers, checks, fixture isolation, read-only workflow permissions, per-ref cancellation and evidence uploads. Limit changes to the owned C++ workflow, CI driver, runner and documentation paths. The task explicitly calls for removal of the retired `tests/ci-sanitize.txt`. Application code, test fixtures, the robustness registry and deployment are outside this change.

Qualify the workflow with `actionlint`, a cold full-inventory ASan shard and two five-minute robustness campaigns on the disposable x86_64 Linux runner. Use the driver's extra compiler flags to supply the missing `HEX` constant for local qualification. Record inventory coverage, exact commands, timings, exits and any upstream defects. Retain namespace isolation and run with fresh home and temporary directories and a credential-free environment. Four shards for both configurations are the measurement candidate within the 240-minute job limit.

The parent checkout starts at `1769741ee6d408f214c9c9dd4d8bcc7f5180a904`, tree `60acf0c92c9dbc99c34cbacb3f65478ec1192472`. The known `src/rt/crypto.cpp` compile blocker and the existing x86_64-specific fixture failures are outside the owned paths. Checkout and hosted delivery status will be recorded after the local gates finish.

## Continuation and measurement plan

The continuation starts at `5705ff938c90303b291a80f817cb29a10ce97e6f`, tree `9fae04d838c685dc52eda38aaaf17b7864b67923`. It retains the earlier commit that makes `summary.py` render either suite configuration. Ubuntu Clang 21.1.8 compiled the inherited `src/rt/crypto.cpp` with the strict driver flags plus the local `HEX` definition. SQLite 3.54.0 was built from the SHA-256-checked source using the same options as CI. The cold ASan shard started on the inherited tree with four compiler workers and the genuine Linux fixture boundary via passwordless sudo.

The cold run will produce one driver receipt and per-target outcomes. The working estimate is 30–60 minutes, with Ninja progress, object counts and incremental test results in scratch. Run the JSON and regex campaigns on the same disposable host, with four workers and a budget of 300 seconds each after compilation. Keep their build and campaign timings separate from the suite and record any overlap. A failed setup or build must surface its diagnostics. A failing gateway test or harness finding will be reported to its owning lane without changing source or assertions.

The unified workflow will select the complete inventory in both configurations and fan out from the robustness registry. Agent pushes, port PRs, pool merges and manual dispatch will use 300 seconds; port branch pushes will use 2,400 seconds. The existing fork-exit leak and regex timeout exceptions remain visible in campaign evidence. A libFuzzer exit without a completed campaign or classified finding must fail the job.

The initial full build applied `-DHEX` to every translation unit. The fuzz build showed that it also rewrites the valid `HEX` declaration in `src/rt/console.cpp`. `src/jev_compaction/hybrid.cpp` has another local `HEX` declaration. Both initial runner processes were stopped, and the retry uses an external `clang++` wrapper that adds the definition when compiling `src/rt/crypto.cpp` alone. The ordinary commands in CI use unmodified Clang.
