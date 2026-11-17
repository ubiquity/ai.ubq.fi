# Native gateway request-path benchmark

## Contract v1

Measure the full loopback request path of the optimized native gateway and the Bun 1.4.0 TypeScript gateway at port
baseline `0edaf83367bbb29eaf50076afd428818f4b3e4bb`. Use the real HTTP and durable KV paths from the existing
production smoke fixture. Both processes get a fresh private state root and the same scripted DeepSeek upstream in a
Linux network namespace with only loopback. The request matrix covers small buffered chat, buffered chat with a 1 MiB
JSON body, progressive chat SSE, buffered Responses, the authenticated bare models list, and rejected chat without
authentication. The last two cases must make zero upstream calls.

Only `cpp/gateway/bench/request_path/` and this report are in scope. No production lines, tests, dependencies,
workflow, deployment, or release state change. No real credentials or provider traffic. The component replacement,
equivalence, and fuzz gates do not apply to this report-only job.

Acceptance uses a strict `-O2` ARM64 build, frozen Bun dependencies, the benchmark driver's response and upstream
checks, three or more interleaved native/Bun repetitions per scenario, and frame-pointer `perf` or Callgrind profiles
for the two largest relative throughput deficits. The report will reconcile all results and include rerun commands.
Runner preflight found GNU C++ 14.2, pinned Bun, and PMU access through `sudo` in the disposable runner. Exact
request-path performance, the two profile scenarios, and any build or fixture incompatibilities remain to be measured.

## Progress

- Verified the official Boost 1.92.0 archive SHA-256
  `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` and installed the Bun lockfile in a detached
  scratch source copy with dotenv and install scripts disabled.
- GCC 14.2 completed all 369 application and runtime translation units, two vendored archives, startup and three
  packaged executables with the strict port warning flags. The packager needed the benchmark-only multiarch lookup
  described below. The native entrypoint and confinement checks ran unchanged.
- The first paired smoke passed all 12 scenario/runtime combinations, with 96 measured and 24 warm-up requests.
  Both gateways relayed all eight individually gated SSE deltas before the upstream could send the next frame.
- The three-repetition benchmark and profiles are pending.
