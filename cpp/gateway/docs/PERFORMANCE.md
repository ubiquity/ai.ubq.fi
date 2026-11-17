# C++ gateway performance

The primary GCC 16 runtime comparison is not measured yet. Gateway idle/concurrent RSS, throughput and latency versus Bun remain pending; the Raspberry Pi memory decision cannot be made from compiler RSS, a binary size, or the retained SDK build. No speedup or memory-saving percentage is established.

## Comparison matrix

| Workload                                                    | Native median / p95 | Bun median / p95 | Native / Bun throughput    | Runtime RSS                  | Status  |
| ----------------------------------------------------------- | ------------------- | ---------------- | -------------------------- | ---------------------------- | ------- |
| JSON parse, 1 KiB / 16 KiB / 1 MiB                          | Not measured        | Not measured     | Not measured, operations/s | Not measured                 | Pending |
| JSON stringify, 1 KiB / 16 KiB / 1 MiB                      | Not measured        | Not measured     | Not measured, operations/s | Not measured                 | Pending |
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
