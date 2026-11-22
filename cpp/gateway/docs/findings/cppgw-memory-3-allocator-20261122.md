# Returning freed gateway heap on Linux

- [x] Reproduce retained free heap with mixed concurrent Codex traffic.
- [x] Attribute retained pages by thread, arena and mapping before changing the gateway.
- [ ] Compare allocator options and record memory, throughput, latency and CPU.
- [ ] Guard retained free heap in Release CI and document the runtime policy.
- [ ] Verify the affected Release and ASan suites, registration and committed diff.

## Starting point

The integration base is `834c97e5`. The runner is Ubuntu 26.04.1 on Linux ARM64 with glibc 2.43, Clang 21 and four CPUs. All gateway runs use fresh state and a loopback upstream under the existing namespace and sealed-package checks.

The deployed process grew from 3.8 to 42.5 MiB anonymous over two hours. Live allocator bytes leveled off around 8 MiB as free arena bytes reached 37.1 MiB. The startup executable already fixes glibc's mmap threshold at 128 KiB. The earlier serial Codex benchmark observed arena pages that `malloc_trim(0)` could return. The new workload exercises overlapping requests and varied payload sizes. [Earlier measurements](cppgw-memory-2-20261121.md)

## Reproduction and cause

The existing sealed benchmark now runs eight concurrent scripted clients against the same programmed Codex upstream, exact-version catalog and 512,231-row SQLite fixture. Each round fetches eight 389,001-byte catalogs and delivers 32 streamed turns. The seed varies the width of seven input messages per turn. Upstream request bodies ranged from 1,490 to 563,541 bytes. All 1,920 inference requests produced delivered terminals and exactly 1,920 additional durable uses.

The baseline package came from integration commit `834c97e54ac410c43476f98dfda5120a27dd972e`, tree `3883cf8bf37711b2f1204beebb3ef1874c6e5638`. Its manifest SHA-256 is `f826edf36359459079073e153bc6216d7f42dbdd25806ee705bd24b9315f98bc` and executable SHA-256 is `d822bdfc302aaa306cef3bba75938b942456d84b96d64880bfeab3a423c75d1e`. The harness at `460286402fef20767a140ba6ed4120f21e0ff835` ran via `bash cpp/gateway/bench/codex_real/sqlite.sh bash cpp/gateway/bench/request_path/isolated.sh --codex-real --package <package> --seed-state <writer> --out <new-directory> --burst --sessions 60 --scripted`. The separate allocator sample adds `--trim`. Both launches used Landlock ABI 8 and seccomp TSYNC.

| Round | Anonymous MiB | Allocator in use MiB | Allocator free MiB | Direct mmap MiB |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 7.00 | 2.26 | 3.50 | 0 |
| 1 | 21.06 | 5.11 | 14.65 | 0 |
| 10 | 25.06 | 5.82 | 17.94 | 0 |
| 20 | 23.06 | 7.07 | 14.70 | 0 |
| 40 | 23.06 | 7.52 | 14.25 | 0 |
| 60, idle | 23.18 | 7.73 | 16.04 | 0 |

Free heap grew in steps during warm-up and remained elevated after traffic stopped. The loopback fixture reproduced the gap between live data and resident memory. Its free heap plateaued during the 60 rounds, so this run did not reproduce the full two-hour deployment trajectory. At idle the largest cache gauges were 1.81 MiB of SQLite pages and 0.76 MiB of catalog.

The process had two threads throughout. The main thread ran HTTP, parsing and stream delivery on the Asio event loop. The SQLite worker was the other thread. GDB found two glibc arenas at round 60. The main arena held 19.76 MiB of address space and had a 0.27 MiB top chunk. The SQLite arena held 4.00 MiB with a 2.05 MiB top chunk. Procfs reported 18 MiB resident in `[heap]`, all backed by transparent huge pages, and about 2.1 MiB resident in the secondary heap. Most free bytes were interior holes in the main arena. GDB read an mmap and trim threshold of 131,072 bytes and `no_dyn_threshold=1`, which confirms that the dynamic threshold did not rise in this build. The workload includes individual JSON strings and transient buffers below 128 KiB. Glibc can also satisfy larger requests from existing arena space and coalesce their freed neighbors into larger holes.

A second run used the inherited-socket allocator probe. Calling `malloc_info` from that thread created the third arena, with 1.99 MiB of free space at the checkpoint. The XML reported 14.74 MiB free in the main arena and 2.08 MiB in the SQLite arena. `malloc_trim(0)` reduced anonymous RSS from 24.10 to 14.66 MiB and resident `[heap]` from 20.00 to 8.57 MiB. `mallinfo2` reported 7.82 MiB live, 18.81 MiB free and zero direct mappings on both sides of that call. Glibc discarded physical pages from free blocks it still owns. The top chunk sizes and the successful page discard explain why shrinking only the end of the heap cannot recover most of the resident pages.
