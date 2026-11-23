# Live heap after streamed Responses and catalog fetches

- [x] Reproduce streamed growth and measure buffered, unversioned and fresh versioned controls.
- [ ] Record the growing allocation sites and their owners before changing request lifetimes.
- [ ] Fix retention at the source and account for the identified process-wide structures.
- [ ] Demonstrate the Release regression gate failing before the fix and passing after it.
- [ ] Record the 500-request comparisons and scoped Release and ASan results.

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
