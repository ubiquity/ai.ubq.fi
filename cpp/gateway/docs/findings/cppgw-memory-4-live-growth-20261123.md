# Live heap after streamed Responses and catalog fetches

- [x] Reproduce streamed growth and measure buffered, unversioned and fresh versioned controls.
- [x] Record the growing allocation sites and their owners before changing request lifetimes.
- [x] Release the measured timers and promise reactions, and account for their process-wide owners.
- [x] Demonstrate the Release regression gate failing before the fix and passing after it.
- [x] Record the 500-request comparisons and scoped Release and ASan results.

The integration base is `d81580d6d2cae4b0766c8f1536c9df0fb5b4a7ad` with tree `18bb1fcebeecc2a950ed495594198d81bb3dcc25`. The runner is Ubuntu 26.04.1 on Linux ARM64 with four CPUs. The reported deployment grew from 8 to 43 MiB of live allocator bytes while free blocks stayed at 8–12 MiB. The local fixture will exercise the same API-key route with a loopback Lithos upstream and the same exact-version Codex catalog shape.

## Pre-fix Release measurements

The CI driver used Clang 21.1.8, Boost 1.90, glibc 2.43, four workers and pinned SQLite 3.54.0. The release profile adds `UOS_CI_CXXFLAGS='-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer'` for Heaptrack. The package ran under Landlock and seccomp with a loopback-only network, empty home and temporary directory, and synthetic state. The baseline workload commit is `c6d9d74e80cd1f9cf4907001352bf8ea94b6c0df` with tree `86db9765d633fc7fa8dd549a5ad61e4fa37db53c`. Its test driver reused all 368 production objects from the exact integration base. The startup difference maps the two fixed Lithos URLs into the isolated fixture. The sealed manifest is `c568f4391671ee94347eeb8b3c1e5be3662d3a3e9a53f838d7acbd7bce9982cc` and the executable SHA-256 is `6f8e24ab6bdd5a49665ae6b7356db717c9e6364ffe4f642f30bbf9a020475a19`.

| Requests after 32 warm-ups per route | Live bytes before | Live bytes after | Change per request |
| --- | ---: | ---: | ---: |
| 500 public models | 4,463,408 | 4,458,480 | -10 B |
| 500 buffered Responses, 200 kB input | 4,458,480 | 6,180,896 | +3,445 B |
| 500 fresh versioned models, 400,641 bytes returned | 5,792,544 | 5,785,904 | -13 B |
| 500 streamed Responses, alternating 2 and 200 kB input | 5,785,904 | 11,610,032 | +11,648 B |

The last buffered sample included transient allocation. Another health read at the start of the following lane measured 5,792,544 bytes. SQLite pager growth accounted for 369,920 bytes across the buffered lane and 361,216 bytes across the streaming lane. The streamed lane continued rising at every 50-request checkpoint. The fixed 300-request Release check failed with 3,926,656 bytes of sampled growth against its 1,048,576-byte budget. The measured versioned lane contributed zero to that growth.

The exact-base package also ran the previous serial catalog lane with its 389,001-byte catalog and no Lithos configured. From request 32 to 500 its live heap moved from 3,361,216 to 3,380,656 bytes, about 42 bytes per request. A Heaptrack diff of 125 versus 25 fresh versioned requests on the Lithos fixture showed 6.99 kB fewer live allocations at the longer checkpoint. A fresh hit alone did not reproduce the deployment's catalog slope.

## Streaming allocation owners

Heaptrack 1.5 compared separate trials of 25 and 125 completed Lithos streams, each after 32 warm-ups. Both gateways served the same SSE frames and were stopped at their live checkpoint for tracing. The diff added 1.03 MB of live allocations. These were the largest growing sites.

| Allocation site | Added live bytes | Owner |
| --- | ---: | --- |
| `AbortSignal::timeout` for `readWithDeadline` and request deadlines | 312,800 | The event loop's `TimerQueue` holds the callback and signal state of each armed timeout. |
| `copyOutcome<ReadResult>` | 218,500 | A settled race's result holds a copy of the winning read. The losing Lithos abort promise is left pending after its listener is removed, and its queued reaction holds both promise states. |
| `createExactTimer` | 176,800 | The same queue retains each `TimerState`. |
| Timer queue map nodes | 108,860 | One ordered-map entry per scheduled deadline. |
| SQLite allocator | 100,660 | Primarily pager pages, already tracked by the SQLite gauge. |
| `AbortSignal::any` | 55,200 | Dependent signal storage retained by request deadlines or their weak bookkeeping. |

`readWithDeadline` was the top stack for the timer and signal allocations. It removes its abort listener and settles the private deadline promise after a read, but keeps the 30-minute timer armed. The largest stack accounted for 999 retained signal states in the 100-request diff. `raceLithosReaderRead` showed retained `withResolvers`, `raceOf` and reaction allocations. Its pending abort promise owns a reaction that captures the abort promise itself and the winning result. The runtime's waiting-promise list still links those promises. The regular buffered-body race already settles its abort promise after removing the listener.

The fresh catalog checks exercise the render and delivery path. The stale refresh and receive-backpressure controls are separate because the deployed cache state and socket timings were unknown.

## Catalog controls and remaining deadline timers

A scratch fixture aged the saved exact-version catalog by ten minutes and made its loopback refreshes return 503. It delivered 500 stale catalogs and made 103 refresh attempts in one trial. Individual immediate health samples sometimes included roughly 680 kB of live refresh work. With a 150 ms drain period at each checkpoint, 500 requests moved live bytes from 4,005,424 to 4,045,008. A separate fresh run with a 32 KiB downstream receive buffer moved from 3,440,752 to 3,474,096 bytes. Neither showed a slope near the reported 19 kB per request.

The two stream fixes in `23d80e470b696501582e671565a57133d3bd460e` reduced the 300-request gate to 522,240 bytes. The new gauges in `25ee71f1932d98ff75312551ac90dffe014bcef2` showed another deadline lifetime. The 500 buffered calls added exactly 1,000 queued timers and the 500 streamed calls added 498 with two short timers expiring during the run. The pending promise count returned to six at each settled checkpoint. The remaining timers came from the whole-request inference deadline created by `dispatchLithosUpstreamResult` and the optional read deadline created by `readBoundedResponseBody`. Both scoped operations settled before their 30-minute timers fired. The residual stream growth was 969,824 bytes, including 391,680 bytes in the SQLite pager. The 500 fresh catalog requests added zero timers or pending promises.

The provider-enriched catalog control enabled loopback OpenRouter and Surplus model discovery. Their cache gauges stabilized at 416,248 and 283,842 bytes. Across 500 fetches of the 1,329,991-byte enriched catalog, allocator in-use moved from 4,538,496 to 4,549,840 bytes. Pending promises stayed at six and queued timers decreased from four to two. The deployment's reported versioned-catalog growth remains unexplained by these controls.

A separate baseline stream run increased the mock from 8 to 22 text deltas with the same input mix. Live bytes rose from 3,720,640 to 10,158,624 across 300 requests, or 21,460 bytes per request. Three seconds after the last batch it still held 10,158,544 bytes. The larger slope follows the additional SSE reads and retained per-read timers.

## Matched tests after the fix

The matched trials used harness `edb04cd8b49fc28e795df613ec46bca6cb59d2e5` with tree `d4ea5dd02b3599c94592af1a43697a4ce2f31c95`. They sampled after fully reading each reply, receiving the inference terminals and allowing 50 ms for teardown after each batch. Both packages ran a further three seconds idle after the last sample.

| Lane | Pre-fix start | Pre-fix after 500 | Fixed start | Fixed after 500 |
| --- | ---: | ---: | ---: | ---: |
| Unversioned models | 4,430,400 | 4,419,744 | 3,986,624 | 3,981,808 |
| Buffered Responses | 4,419,744 | 5,848,944 | 3,981,808 | 4,434,576 |
| Versioned models | 5,848,944 | 5,847,632 | 4,434,576 | 4,430,096 |
| Streamed Responses | 5,847,632 | 11,776,144 | 4,430,096 | 4,853,872 |
| Idle after all lanes | | 11,774,976 | | 4,852,784 |

All values are allocator in-use bytes. The SQLite pager accounted for 361,216 of the 423,776 bytes added across the fixed streaming lane, and 382,976 of the 452,768 bytes added across the fixed buffered lane. The four queued timers and six pending promises held steady from warm-up through the last batch. Two short timers expired during the idle period. Both runs delivered 1,064 unique inference terminals and committed exactly 1,064 additional quota uses. Each run fetched its exact-version catalog 532 times including warm-up.

The fixed 300-request Release regression check measures the sum of the highest live-byte increases in the streaming and versioned lanes. The pre-fix package exited 1 at 3,601,792 bytes. The fixed package exited 0 at 133,744 bytes. Both used a 1,048,576-byte allowance. The existing scripted session gate passed at 8,608 KiB anonymous and 43,340 KiB peak RSS. The burst gate passed with 17,260,928 free arena bytes and 4,718,432 idle anonymous bytes above live allocation.

For both packages the matched loopback launch used these commands, with fresh output directories.

```sh
bench=cpp/gateway/bench
timeout 240s bash "$bench/codex_real/sqlite.sh" bash "$bench/request_path/isolated.sh" --codex-real \
  --package "$package" --seed-state "$seed_state" --out "$out" --scripted --check-live-growth
timeout 240s bash "$bench/codex_real/sqlite.sh" bash "$bench/request_path/isolated.sh" --codex-real \
  --package "$package" --seed-state "$seed_state" --out "$out_500" --scripted --live-growth all --sessions 500
```

The fixed package was linked from `a022a88ff76f25c8c50bb4051131a4e248bf2167` with tree `11195db1624b0b0e1240a4507c4aa9e3987cb0a3`. Its manifest SHA-256 is `1a675c7877b966cb5386d7eae8855ed7289ce395e2dd64522b144319b0767304` and executable SHA-256 is `520007419786e111ac1635dd54b020ed9333269ce113fae98aec595ea394aa81`. The matched check receipts have SHA-256 `6c8b2302706f84c4235e83c74d3e8d429310707f5da60002a77b07e354799c81` (pre-fix) and `d3410a24f235393261c55d6962a0ad96d5671215277b701ba0555b9bf4599d82` (fixed). The matched 500-request receipts have SHA-256 `0c1f053d58be8edd1d6e6a0bb28cbf60d0324bbe346af7356e0f7b1a54236ee4` and `056a04075832b3c7d1bd18bf206592d3c839ec656b3acb3dce2887eee8d53d3d` respectively.

## Scoped verification

Release and ASan, UBSan and LeakSanitizer each passed 31 distinct binaries and 378 cases on the fixed package's source commit. Both configurations used the same source input hash `299d65af8c1940132161961d1eff16f2996780c46037b9c26a8cd29fe58b8361` and the CI driver's private network, mount and PID namespaces. Release used the profiling compile flags above. The commands used Clang with four jobs and ran these selectors in both configurations.

```sh
for config in release asan; do
  for selector in oss_ lithos responses_stream codex_catalog bounded_response_body deepseek_waterfall \
    health_test.cpp rt/abort rt/async rt/clock serve_delivery_http runtime_native_startup; do
    bash cpp/gateway/tools/ci-build.sh --config "$config" "$selector"
  done
done
python3 -B cpp/gateway/tools/links.py --check
git diff --check claude/cpp-gateway-port...HEAD
```

The registration check passed all 453 entries. The requested committed-diff review fetched base `d81580d6d2cae4b0766c8f1536c9df0fb5b4a7ad` and head `a022a88ff76f25c8c50bb4051131a4e248bf2167`. It ran native `codex review --base refs/review/base -c 'model="gpt-6.1-sol"' -c 'review_model="gpt-6.1-sol"' -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="none"' -c 'shell_environment_policy.set={PATH="/usr/bin:/bin"}'` with an empty home and a network namespace. It exited 124 after 25 seconds and logged `failed to connect to Responses WebSocket stage=Unknown`. No completed review result was available.
