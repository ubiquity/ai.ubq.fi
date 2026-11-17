# C++ gateway performance

A shared-host JSON component comparison is now measured below. The packaged gateway's latency/throughput and paired native-versus-Bun idle/concurrent RSS remain pending. C++ JSON warm medians were 2.85–6.09 times Bun in this baseline; this is not a whole-gateway throughput result. The Raspberry Pi memory decision cannot be made from compiler RSS, a binary size, unpaired Bun RSS, or the retained SDK build.

## JSON component baseline

| Input bytes | Operation | Cold C++ / Bun us  | C++ warm median / p95 us | Bun warm median / p95 us | C++ / Bun ops/s   | C++ / Bun median |
| ----------- | --------- | ------------------ | ------------------------ | ------------------------ | ----------------- | ---------------- |
| 1,088       | parse     | 240.60 / 103.32    | 14.53 / 25.23            | 4.91 / 7.43              | 61,280 / 201,660  | 2.96x            |
| 1,088       | stringify | 28.64 / 28.60      | 7.93 / 8.22              | 2.00 / 3.33              | 117,898 / 451,788 | 3.97x            |
| 15,968      | parse     | 335.96 / 155.88    | 226.42 / 234.82          | 60.73 / 71.90            | 4,384 / 16,184    | 3.73x            |
| 15,968      | stringify | 145.00 / 79.48     | 113.79 / 137.88          | 39.90 / 94.11            | 8,423 / 20,232    | 2.85x            |
| 1,027,878   | parse     | 14902.54 / 4041.71 | 14936.23 / 16320.59      | 3823.28 / 4334.96        | 66 / 258          | 3.91x            |
| 1,027,878   | stringify | 8591.06 / 2214.97  | 8265.06 / 8591.82        | 1357.52 / 2414.22        | 120 / 632         | 6.09x            |

This is the isolated `uos-json-bench` component, not the gateway executable. Thirty-six interleaved process trials completed with matching input/canonical checksums. Each runtime/case has three actual first-call samples (cold median), then 24 warm batch means, eight from each process after its first batch. Warm p95 is the nearest-rank percentile of batch-average microseconds per operation, not individual-operation tail latency. Throughput is the reported warm operations/second. Two compiler slots were active on the shared host; this is not an idle-host measurement. All six rows are a before-optimization baseline; profiling and any source fix remain with the assigned JSON owner.

Native benchmark SHA-256: `b94ee2a374e0d64f87b163743e9b19407d9d9aac37b9a12cc450598bec763244`; Bun binary SHA-256: `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9`. Raw receipt: `/tmp/claude-1002/prod/smoke_perf/json-interleaved-v15/json/json-results.json`, SHA-256 `2dce6197c1db6d81b52081b63aaec5b477ec5bc2b8e95a6134e768599671c74c`; summary: `/tmp/claude-1002/prod/smoke_perf/json-summary-v15.json`. The raw receipt binds actual input bytes/hashes, source hashes, commands, exits and samples. It does not establish packaged-gateway RSS, request latency or production acceptance.

## Comparison matrix

| Workload                                                    | Native median / p95 | Bun median / p95 | Native / Bun throughput    | Runtime RSS                  | Status  |
| ----------------------------------------------------------- | ------------------- | ---------------- | -------------------------- | ---------------------------- | ------- |
| Regex: six supervisor stress shapes and real redaction path | Not measured        | Not measured     | Not measured, operations/s | Not measured                 | Pending |
| SSE relay, 64 fixed frames                                  | Not measured        | Not measured     | Not measured, relays/s     | Not measured                 | Pending |
| Buffered / streamed chat, concurrency 1 and 8               | Not measured        | Not measured     | Not measured, requests/s   | Not measured                 | Pending |
| Buffered / streamed Responses, concurrency 1 and 8          | Not measured        | Not measured     | Not measured, requests/s   | Not measured                 | Pending |
| Settled idle, 32 post-exec samples                          | Not applicable      | Not applicable   | Not applicable             | Native/Bun not measured      | Pending |
| Same scripted concurrent load, at most 16 requests          | Not measured        | Not measured     | Not measured, requests/s   | Native/Bun peak not measured | Pending |

Timing units must be explicit in each final row: nanoseconds or microseconds for microbenchmarks, milliseconds for requests. Retain sample count, warmup, median, nearest-rank p95, throughput denominator, response bytes and correctness checksum. Native/Bun columns must describe identical inputs, upstream scripts and concurrency. Cold startup and warm request latency are separate workloads.

## Method and safety

The smoke/performance owner uses the same packaged gateway executable and ordinary HTTP/authentication/accounting/durable-storage handlers. An alternate toy server, auth bypass or memory-only store cannot stand in for the gateway. The runtime owner has published the closed `--runtime-manifest` startup source, with package validation, private state, fixed loopback fixture mapping, real OS confinement and no development fallback. The rebuilt primary executable, genuine artifacts/package and executed same-binary startup still require their own receipts; syntax checks and fixture self-checks do not replace them.

Run each fixture in a fresh user/network/mount/PID namespace with only loopback enabled, an actual loopback positive control and an outside-route denial before launch. Use OS-assigned ports, synthetic keys, an empty task HOME, private cwd/state/TMPDIR, and no dotenv or inherited credential variables. Bun must be pinned 1.4.0 with `--no-env-file --no-install`; neither process may reach real providers or a production database. Real network bytes, semantic terminal, physical delivery, durable settlement and SIGTERM shutdown are separate observations.

The agreed bounds are at most 180 seconds and 4,096 dispatches per run, at most 16 concurrent requests, a 2 GiB gateway RSS limit and at least 2 GiB host memory reserve. Stop only the owned process group/namespace on a failed boundary, resource breach or deadline. Preserve every failed run and diagnostic; rerun only after the relevant input or condition changes.

Record actual child RSS after exec, including settled idle median/peak and concurrent peak, rather than inherited pre-exec high-water accounting. Report KiB or MiB consistently and retain the raw samples. Compiler memory and filesystem build allocation belong in [BUILD.md](BUILD.md). Host load and competing compilation must be recorded; short interleaved native/Bun samples and agreed quiet windows prevent an uncontrolled before/after claim.

## Evidence required for final numbers

Each result must bind the exact native executable SHA-256, source/header manifest, compiler/profile/options, linked library closure, Bun version/binary hash, workload and upstream-script hashes, argv/environment-name list, namespace proof, exit status, raw samples and output parity. A later consumed-source change makes the affected performance receipt stale. Health alone does not prove chat/Responses, stream progress, WebSocket relay, settlement or restart durability.

The historical supervisor redaction observation was 3–6 seconds for the `-O0` C++ flow versus 19–237 milliseconds for Bun, with unmatched inputs and an unoptimized C++ build. Preserve it as the reason to measure; it is not a controlled primary comparison. The SDK frame-allocation control measured a 14.649 ns/operation cost for 256-byte frames, not a request-path or runtime-RSS result. No speculative regex rewrite or allocator inference follows from those observations.

Final result references, primary executable identity and the requested before/after measurements remain pending from the smoke/performance and IDNA/regex owners. [OPEN-STAGES.md](OPEN-STAGES.md) retains the corresponding acceptance gates.
