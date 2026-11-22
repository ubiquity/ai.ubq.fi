# Native gateway memory with the real Codex client

## Checklist

- [x] Identify the integration base and qualify the runner and confinement.
- [x] Generate realistic state and validate single, tool and resumed sessions through the packaged gateway.
- [x] Record baseline samples at 1, 10, 100 and 500 sessions, allocation sites and the trim control.
- [x] Publish resident, allocator and cache counters in health and request terminals.
- [x] Measure each production change against the baseline and run the 500-session endpoint.
- [x] Enforce a 20-session CI memory budget and run the change-scoped Release, ASan and registration gates.
- [ ] Repeat 500 sessions after the builds finish.

## Starting point

The integration base is `707f7182b9a4826650f45f8e0f6e41095dd32974` with tree `b6fdaf36ab238f7ab111ae35b88043602cf97982`. It includes the first investigation's catalog changes. The runner is Ubuntu 26.04.1 on Linux ARM64 with four CPUs. The native, statically linked Codex CLI is 0.171.0. The installed launcher wraps that binary in JavaScript. The fixture invokes the ELF directly.

The workflow's SQLite 3.54.0 archive passed the pinned SHA-256 check. It was built with `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j4` and `sudo make install`. Noninteractive Linux network, mount and PID namespaces are available. [CI prerequisites](../CI.md)

## Harness qualification

Codex 0.171.0 needed its `api_key_model_discovery` feature to consume the custom provider's remote catalog. It sends the catalog instructions as an input message and exposes `exec_command` for shell calls. The fixture requires those instructions and all ten tool results at the upstream and checks that the result of a resume matches the programmed reply. A five-session development trial completed 15 inference requests at up to 417,130 upstream bytes. After the first tool session it measured 17,600 KiB anonymous and 54,788 KiB peak resident. That trial used the same integration base and a 377,511-byte draft catalog. The measurement fixture uses 389,001 bytes.

The public models endpoint passed its schema check with the 15-model Codex source and synthetic subscription. Publishing 535 models through the auth endpoint reached the runtime configuration's existing 4 KiB cap. The real-client mix uses the separate exact-version catalog. The seeded database is freshly compacted by SQLite insertion and has less on-disk fragmentation than the deployment. [Harness](../../bench/codex_real/README.md)

## Baseline

The stock integration build completed the real 500-session workload in 185.5 s. It settled 1,500 inference requests and the active key ended at 6,500 committed requests. The tool and resume bodies reached 428,769 bytes upstream. The package manifest was `a37504f8660215d19c98112a5e4c843db2afa1da9a47cab5a1d4685d3f707d0f` and the executable was `a0546e3cff1f43508457b4cfa3333e98e1e1fb7b99e564ee312ef352c6df4727`.

| Sessions | Requests | VmRSS KiB | RssAnon KiB | VmHWM KiB | Resident heap KiB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 30,484 | 6,332 | 30,484 | 2,048 |
| 1 | 11 | 51,192 | 19,104 | 55,672 | 14,336 |
| 10 | 30 | 56,912 | 24,824 | 57,460 | 20,480 |
| 20 | 60 | 56,912 | 24,824 | 57,808 | 20,480 |
| 100 | 300 | 54,872 | 22,784 | 59,016 | 18,432 |
| 500 | 1,500 | 54,872 | 22,784 | 59,016 | 18,432 |

The separate 1-, 10- and 100-session trials finished at 19,652 / 55,636, 22,072 / 58,980 and 19,304 / 57,424 KiB anonymous / peak resident respectively. Serial trials varied with allocator layout. This loopback runner did not reproduce the deployment's 50 MiB anonymous or 112 MiB peak resident. The baseline itself met the three requested thresholds on this fixture.

The durable fixture had 512,231 rows, 190,027,536 value bytes and a roughly 338 MB SQLite file. The named input counts add up to 512,226 with the catalog chunks included. The five additional rows came from gateway metadata. The 15-model catalog was 389,001 bytes, including 256,000 bytes across six distinct `model_messages` and 110,000 bytes of `base_instructions`. The served and source documents each used five chunks, 535,690 compressed bytes combined.

| Prefix | Rows | Value bytes | Largest value bytes |
| --- | ---: | ---: | ---: |
| API key usage | 413,480 | 116,655,454 | 284 |
| Sentinel replay | 10,595 | 33,072,435 | 48,009 |
| Paid fallback | 47,089 | 20,701,390 | 440 |
| API keys | 17,786 | 10,179,155 | 766 |
| Admin error log | 6,948 | 2,840,622 | 409 |
| Provider capacity | 3,116 | 2,795,052 | 897 |
| Embeddings | 848 | 2,694,096 | 3,177 |
| Metered quota | 2,051 | 346,619 | 169 |
| Prompt cache analytics | 6,792 | 54,336 | 8 |
| Prompt cache telemetry gate | 2,848 | 22,784 | 8 |
| Sentinel incident | 538 | 119,864 | 223 |
| Kernel auth | 72 | 648 | 9 |
| Provider health | 52 | 3,744 | 72 |
| Codex auth pool | 1 | 482 | 482 |

### Allocation attribution

Heaptrack recorded the gateway only. Each control restarted the sealed package against its own seeded database. The driver stopped the recorded process with its allocations live. Retained memory is the heap still allocated at the checkpoint. The separate native trials supply RSS because Heaptrack changes it.

| Trial | Peak live heap MB | Retained heap MB |
| --- | ---: | ---: |
| Idle after readiness | 2.14 | 2.07 |
| API-key authentication | 2.13 | 2.08 |
| Versioned catalog | 7.35 | 5.86 |
| Five-session real mix | 21.95 | 6.51 |
| Programmed stream failure with replay capture | 8.61 | 6.64 |

| Request phase | Peak and retained allocation sites | Lifetime |
| --- | --- | --- |
| API-key authentication | Hash lookup at `authenticateApiKeyToken`, with a 272-byte coroutine allocation and small key and string allocations. The isolated route miss added about 0.01 MB live. | Per-request work and a policy memo with TTL. Expired entries were only removed on access and the memo had no population cap. |
| Quota and usage | `attemptApiKeyUsageReservation` reads the active key and point-reads the current window and request. SQLite's `pcache1Alloc` accounts for 0.13 MB on the hot range-read stack. The merged SQLite allocator site peaks and retains 0.53 MB across the mix. | Shared pager and statement caches, and transient decoded rows. The full-window lease-reclaim path uses an active-window prefix and did not run in this workload. |
| Catalog | `JsonInput` retains 1.56 MB of indexes, 0.65 MB of decoded-string scratch and 0.78 MB of padded input on the isolated catalog lane. The served object and serialized body account for about 0.79 MB retained. | Bounded version memo and per-thread parser high water. Rendering copies are transient. |
| Request body and provider projection | Boost.JSON strings peak at 11.30 MB across the trace. The largest hot copies carry the whole input through `ReadResponsesRequestValue`, `prepareResponsesRequest` and the provider bodies. The read observer reserves 0.43 MB once per large request. Padded parser scratch and indexes account for at least 3.21 MB before the final copy is dropped. | Per-request copies on the peak. Parser scratch retained by the reusable instance. |
| Streaming relay | The transport keeps small buffers around the live stream. `PreparedCodexSubscriptionRequest` copies peak at 1.29 MB and `createCodexResponseState` at about 0.43 MB on each of two stacks. | Scoped to the request and its retry lifetime. Successful tool, reasoning and long-text turns all reached a delivered terminal. |
| Terminal logging and telemetry | The `logTerminal` stack showed multiple allocations at 176 bytes or less at the global peak. Terminal serialization writes to the log and optional writes settle before shutdown. | Per-request and shared telemetry bookkeeping. |
| Replay | A committed text delta followed by `response.failed` produced one encrypted capture. The failure lane uses the same 389 kB catalog and a 69 kB body, plus bounded observation, compression and encryption. The end snapshot includes small OpenSSL and SQLite warm-up allocations. | Per-request capture, durable data and reusable library state. Existing replay retention was active with a completed budget ledger. |

Replaying the allocation and free events provides separate phase peaks. Each stack is charged to its first matching frame, with SQLite reported separately where the background worker stack identifies only the storage layer. Peak values occur at different times. The two replay rows distinguish the passive observation in successful sessions from the encryption and persistence on a failure. [Trace analysis](../../bench/codex_real/profile.py)

| Phase | Baseline peak bytes | Baseline retained bytes | Final peak bytes | Final retained bytes |
| --- | ---: | ---: | ---: | ---: |
| API-key authentication | 5,723 | 556 | 6,132 | 556 |
| Quota and usage | 33,735 | 296 | 33,735 | 296 |
| Catalog and its parser | 5,200,594 | 1,579,947 | 3,788,634 | 802,320 |
| Request handling | 12,651,278 | 2,442,252 | 4,619,360 | 3,560 |
| Streaming and dispatch | 6,897,507 | 151,648 | 2,766,009 | 383,369 |
| Terminal logging and telemetry | 85,803 | 64,624 | 89,131 | 27,072 |
| Passive replay recording | 1,008,631 | 7,450 | 1,008,240 | 7,450 |
| Shared SQLite | 720,065 | 540,920 | 770,036 | 592,939 |

The baseline failure control peaked at 374,484 bytes on replay recording stacks and retained 30,775 bytes there. Its largest transient was the accepted-body clone, followed by encryption and persistence. In the successful mix the largest replay site was the upstream attempt buffer. Inside streaming and dispatch, the baseline's largest stack copied the prepared subscription request. The final stack is the initial preparation. The final retained site includes the reusable parser scratch first allocated while parsing a streamed event. The terminal's largest peak stack was the stream pull callback. Its retained allocations center on response telemetry state.

On the separate one-session native trim run, RssAnon moved from 19,868 to 12,428 KiB. Three arenas held 5,366,528 in-use bytes outside direct mappings, 15,666,432 bytes in free blocks and 1,720,320 directly mapped bytes. `malloc_trim(0)` released 7,440 KiB of resident pages without reducing those allocator counters. That identifies reusable free pages inside arenas alongside live parser scratch. No full API-key or usage-table load occurred in the measured request path.

The production build command was `bash cpp/gateway/tools/ci-build.sh --config release oss_gateway_http_test` with Clang, four jobs and `UOS_CI_CXXFLAGS='-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer'`. The test passed 5/5 cases at the integration SHA. The benchmark used `sqlite.sh` with the existing package writer and `isolated.sh --codex-real`. Real trials used `--sessions 1`, `10`, `100` and `500`. The profiler measured `--codex ... --sessions 5 --heaptrack` and the scripted lanes `--traffic idle|auth|catalog|replay --heaptrack`. Traces were read with `heaptrack_print -p1 -a0 -T0 -l1` and filtered by allocation backtrace for attribution.

## Health and request telemetry

The memory object carries resident and anonymous bytes, glibc in-use, free and direct-mapping bytes, and named cache entries and byte estimates. Health prefers the kernel's mapping rollup. Terminals read the fast statm counters. Cache owners update their counters when held values change, so the snapshot can read them without touching KV or enumerating cache contents. The allocator in-use counter includes direct mappings. SQLite page entries are estimated from the reported cache bytes and the database's page size. [Runtime guide](../RUNTIME.md)

The telemetry commit is `a66cde42abdf25b9cba1d8e0a0ba76c49d1c582a`, tree `9d40e8bbef5f61e2520d47c30c22f672dfee3c82`. The package manifest was `6dc8317eb6d60d3732350363a5deed1ffd233af6971cdeac5983365b5a7a9511`. At session 20, health reported 7,529,824 bytes in use and 13,900,448 bytes in free blocks. The largest gauges were parser scratch at 3,216,540 bytes, the one-entry Codex catalog at 798,169 bytes, SQLite pages at 531,688 bytes and static assets at 352,800 bytes. The one-entry policy memo used an estimated 380 bytes. The terminal rows carried the same field names. Release `oss_gateway_http_test` passed 5/5 cases.

## Move the Responses input and release it after projection

The request path now moves one parsed record into validation. It borrows that record for model and reasoning lookup, moves the two upstream projections into the routing state, and discards the raw and normalized input when the projections own what they need. Delivery and terminal accounting retain their existing owners.

The change is `703734114fd99b2457ac3a1f56dbb74ee5cc3d95`, tree `0a7394b785955dbb5758d4913ed223079169b2af`. The executable hash was `0723f36a769da410f08f1ebcc6c629ade47f6d0861e1fe79c4214af5ea836d74`, with package manifest `8e2e8893c0241f041afb0638c877e918feba64d1babf36396280bdfc776e49c0`. Release `input_normalization_logprobs_test` passed 5/5 cases. The same real five-session Heaptrack cycle fell from 21.95 MB peak live heap to 16.71 MB. The Boost.JSON string site decreased from 11.30 to 6.33 MB and allocation calls decreased from 855,466 to 580,909. Retained heap was 6.63 MB with telemetry and the large parser scratch still present.


## Bound reusable scratch and policies

Commit `90171618d5c3dbcbaf46e6212fa3ec56b801302c` with tree `b2b09adf1c2da5e8b65f07fd0de850dab4ea721d` limits parser reuse to inputs up to 64 KiB. Larger documents use a request-local parser under the existing document limits. The parser gauge fell from 3,216,430 to 234,561 bytes. The five-session Heaptrack peak fell from 16.71 to 13.72 MB, and retained heap from 6.63 to 3.64 MB. The API-key policy memo also has a 1,024-entry and 512 KiB limit and evicts the oldest insertion. Evicted tokens resolve through the existing keyed lookup. The serial fixture uses one policy entry, so it isolates parser savings from high-cardinality cache behavior. The added cache case exercises 1,025 tokens and requires a durable reread after eviction.

The cache package manifest was `b87c977439330c1876f650302575a8dc95f916e1db0b266f787b38bb7f48bfed` and executable `2f484ec41ec65525b9e8ffc9585599cfb550e4299b68319410607e9a9dfd6520`. The Release policy selection passed two binaries and ten cases at this stage. [Memory limits](../../include/uos/rt/limits.hpp)

## Reuse the prepared Codex payload

Commit `5eeb5c3bda2f96b258946808e790ac873024400f` with tree `6837e3d3740a38652c5df142069d98fe83056140` shares the prepared request across transport attempts. Each attempt uses that result's wire text and the same routing, credentials and quota lifecycle. This removes the copies of the prepared object, serialized body and parsed projection from routing-state construction. The Release operations differential test passed its complete state, header and body comparisons. The manifest was `f337c25167d7520af17c3a09499eb35169b45255ed153ffbfa4a9df7bc000f85` and the executable was `c7ac0cefbe7aa1122eb47a92458446dced05b1f1236afdafc57f5260fefebdf1`.

## Read SQLite range rows one at a time

Commit `e90238c3105cdcee86e4a3f5ced1a7a931ddf138` with tree `11b0005ba37c38526fb3fa9949666fd3b70ce09a` visits each SQL row, moving its blobs into the existing bounded KV batch. It explicitly sets a 2,000 KiB pager target and disables mapping the state file. The gateway still reads the same rows in order and returns the same encodings and versionstamps. The SQLite test compiled and passed all 30 cases, including resetting the statement after a failed row conversion. The short native RSS trials vary by a couple of MiB, and the row change removes an intermediate allocation rather than reducing the configured pager target.

| Build, 20 real sessions | First RssAnon KiB | Ending RssAnon KiB | Peak VmHWM KiB |
| --- | ---: | ---: | ---: |
| Telemetry | 20,224 | 22,264 | 58,488 |
| Move the Responses input | 14,464 | 20,184 | 52,732 |
| Bound reusable scratch and policies | 14,152 | 14,152 | 50,464 |
| Share the Codex payload | 8,704 | 10,832 | 43,220 |
| Stream SQLite range rows | 10,980 | 13,028 | 45,288 |

Each trial delivered all 60 successful request terminals and settled exactly 60 additional uses for the active key. The catalog remained 389,001 bytes with ETag `"uos-catalog-a877e72d2e2c6ef38cacf86989f69ca2"`. The final package manifest was `a5a9aad32c91bb4e1d2d72c514eae0a9012897eab462f1395e60b6358aaa968c` and the executable was `63d58f454743c9ec26603747700cd9d347ad4b45b21ba838610159c96bb344e2`. Its five-session Heaptrack peak was 8.91 MB with 3.63 MB retained. The Boost.JSON string site fell to 3.37 MB, compared with 11.30 MB on the integration base. The three per-attempt request-body copies in `createCodexResponseState` disappeared from the top sites.

## Long run alongside builds

The final package completed 500 native Codex sessions and 1,500 delivered inference requests while Release and sanitizer builds were running. The key ended at 6,500 committed requests. The run took 733.4 s and crossed two five-minute catalog refreshes. The upstream observed the initial seed and two exact-version refreshes. The first refresh coincided with a rise from 11,052 KiB to 13,488 KiB anonymous RSS. Later samples plateaued near 13,564 KiB after the second refresh. The durable catalog ETag and wire assertions passed.

| Sessions | VmRSS KiB | RssAnon KiB | VmHWM KiB | Resident heap KiB |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 30,832 | 6,904 | 30,832 | 2,048 |
| 1 | 44,716 | 12,852 | 44,716 | 7,944 |
| 10 | 44,964 | 13,100 | 44,964 | 8,192 |
| 100 | 42,916 | 11,052 | 45,420 | 6,144 |
| 500 | 46,452 | 13,564 | 47,612 | 8,192 |

The final single-session trim reduced anonymous RSS by 1,032 KiB, from 11,844 to 10,812 KiB. Allocator state before and after was 4,044,528 bytes in use, 8,796,432 bytes in free blocks and no direct mappings at the checkpoint. The original trim released 7,440 KiB. The final replay control stored an encrypted failure capture and finished at 8,764 KiB anonymous with a 42,760 KiB resident peak. Across both 500-session traces the terminal usage checks found 1,000 tool replies at 40 output tokens, 400 short replies at 50 and 100 long replies at 4,000. Every total matched input plus output usage.

## Release memory gate

Calibration on the final package measured 13,028 KiB anonymous and 45,288 KiB peak resident with the real CLI. The scripted run measured 12,792 and 44,740 KiB. The gate rounds margins of about 30 percent to 17 MiB anonymous and 58 MiB peak resident. The real `--check` passed at 11,704 / 45,512 KiB with those budgets. The scripted `--check` passed at 11,148 / 44,888 KiB. Both completed 20 sessions and 60 inference requests.

Release shard 1 builds the benchmark from its existing objects and uses the native CLI when installed. The fallback exercises the same state, reply shapes and growing tool history with the scripted client. The workflow saves sample, fixture, upstream and budget receipts in the shard artifact. [CI workflow](../../../../.github/workflows/cpp-gateway-ci.yml) · [Local commands](../../bench/codex_real/README.md)

## Run after the builds finish

The final package completed the same 500 sessions in 162.7 s. It used Codex 0.171.0 with SHA-256 `9ba12ec7712e2905b2ea00791f4ebddc9528629cc955d11d25272de445e10643` and state writer `189a3a58259c954cfd8e13065b855f446c558e00f46d36bc31fdf5b0c7eb954d`. The mock served one versioned catalog during seeding. The workload stretched upstream bodies to 428,799 bytes and settled all 1,500 inference requests.

| Sessions | Integration baseline anonymous KiB | Final anonymous KiB | Final resident KiB | Final resident heap KiB |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 6,332 | 7,724 | 31,736 | 4,096 |
| 1 | 19,104 | 9,772 | 41,656 | 6,144 |
| 10 | 24,824 | 11,820 | 43,704 | 8,192 |
| 100 | 22,784 | 11,820 | 43,704 | 8,192 |
| 500 | 22,784 | 13,868 | 45,752 | 10,240 |

Peak resident memory fell from 59,016 to 46,080 KiB. The quiet trial had one 2,048 KiB step at session 111 and held exactly 13,868 KiB anonymous through session 500. At the step, resident `[heap]` grew from 8,192 to 10,240 KiB, allocator free bytes rose from 5,874,048 to 8,034,928, and allocator in-use bytes fell from 4,869,760 to 4,806,032. Between sessions 100 and 500 the SQLite pager grew from 679,664 to 1,554,416 bytes within its configured target. The parser shifted by 21 bytes and the other tracked cache sizes held steady. This compares resident growth with the bounded cache warm-up and the allocator's reuse of pages already resident.

## Verification

The test driver used Clang 21.1.8, four jobs, the workflow's pinned SQLite 3.54.0 and fresh Linux network, mount and PID namespaces. Release used the frame-pointer and line-table flags used by the profiled packages. The compiler-cache and runtime directories were outside the checkout. The raw SQLite library hash was `2fb9976405cd8f7afcc474acc90b93c087c4ced76b5302ef75dcd79279f9b238`. The benchmark copy, with its package-compatible SONAME and system library placement, was `e1cbbe281d8c6a571930bb04f78e1c05b578abcc15e4e3ef8771d1daddcf2770`.

| Command or selection | Commit and tree | Result |
| --- | --- | --- |
| `bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4` | `cd09f702108838147014b21ac647a099eb85089b` / `7f1bbd7b4febdf0e88e48a6978b630fc278b3773` | 114 binaries, 1,334 cases passed |
| Release `api_key_policy_differential_test` | Same commit and tree as the shard | 2 binaries, 11 cases passed |
| Remaining Release selectors below | `351e42d620d51ec64eead0a4d1aed9c530e31a8c` / `60455c3671c11ad9160368e9408e576c30b414b4` | 17 binaries, 205 cases passed |
| ASan selectors below | `e90238c3105cdcee86e4a3f5ced1a7a931ddf138` / `11b0005ba37c38526fb3fa9949666fd3b70ce09a` | 19 binaries, 216 cases passed |
| ASan `kv_budget_part2_test` | `cd09f702108838147014b21ac647a099eb85089b` / `7f1bbd7b4febdf0e88e48a6978b630fc278b3773` | 1 binary, 24 cases passed |

Each selector ran with `bash cpp/gateway/tools/ci-build.sh --config release <selector>` or the equivalent `--config asan` invocation. The selections were `api_key_policy_differential_test`, `input_normalization_logprobs_test`, `codex_responses_operations_differential_test`, `oss_gateway_http_test`, `rt/json`, `rt/sqlite_test`, `rt/kv_sqlite_test`, `sentinel_replay_retention`, `health` and `codex_catalog`. Release shard 1 also covers `kv_budget_part2_test`. The runs checked the same production code. Later commits updated the test for the new log fields and recorded benchmark evidence.

The first Release shard run found one exact terminal JSON comparison that used the earlier field set. The updated test checks the process fields, cache names, entry counts and byte sizes before comparing the complete terminal record. The former terminal assertions still pass. The registration check `python3 cpp/gateway/tools/links.py --check` resolved all 453 tests, and `git diff --check 707f7182...HEAD` passed. This project's loopback-only rule excludes provider-backed Codex review.
