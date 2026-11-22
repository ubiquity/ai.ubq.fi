# Native gateway memory with the real Codex client

## Checklist

- [x] Identify the integration base and qualify the runner and confinement.
- [x] Generate realistic state and validate single, tool and resumed sessions through the packaged gateway.
- [x] Record baseline samples at 1, 10, 100 and 500 sessions, allocation sites and the trim control.
- [ ] Publish resident, allocator and cache counters in health and request terminals.
- [ ] Measure each production change against the baseline and run the 500-session endpoint.
- [ ] Enforce a 20-session CI memory budget and run the change-scoped Release, ASan and registration gates.

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

On the separate one-session native trim run, RssAnon moved from 19,868 to 12,428 KiB. Three arenas held 5,366,528 in-use bytes outside direct mappings, 15,666,432 bytes in free blocks and 1,720,320 directly mapped bytes. `malloc_trim(0)` released 7,440 KiB of resident pages without reducing those allocator counters. That identifies reusable free pages inside arenas alongside live parser scratch. No full API-key or usage-table load occurred in the measured request path.

The production build command was `bash cpp/gateway/tools/ci-build.sh --config release oss_gateway_http_test` with Clang, four jobs and `UOS_CI_CXXFLAGS='-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer'`. The test passed 5/5 cases at the integration SHA. The benchmark used `sqlite.sh` with the existing package writer and `isolated.sh --codex-real`. Real trials used `--sessions 1`, `10`, `100` and `500`. The profiler measured `--codex ... --sessions 5 --heaptrack` and the scripted lanes `--traffic idle|auth|catalog|replay --heaptrack`. Traces were read with `heaptrack_print -p1 -a0 -T0 -l1` and filtered by allocation backtrace for attribution.
