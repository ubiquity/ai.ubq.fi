# Native gateway memory under Codex CLI traffic

## Checklist

- [x] Identify the integration base and qualify the runner, SQLite and namespace prerequisites.
- [x] Implement a loopback reproduction with a populated Bun/JSC KV database.
- [x] Measure Release memory at startup and after 1, 10, 100 and 1,000 sessions. Isolate the catalog and completion paths.
- [x] Profile allocation peaks and retained bytes, and measure the effect of `malloc_trim(0)`.
- [ ] Fix measured waste and repeat the same workload after each fix.
- [ ] Add the Release budget check, document small-board use, and record the focused, shard and registration gates.

## Runner

The starting tree is `8f11c1a264bcdd6bef668139bf1a59031dd962fe` on `claude/cpp-gateway-port`. The runner has four ARM64 CPUs, Ubuntu 26.04.1, Linux 7.0.0-1020-azure, glibc 2.43, Clang 21.1.8, Boost 1.90, ICU 78.2 and Heaptrack 1.5.0. The job's compiler cache is in use.

The runner used the [C++ workflow dependencies](../../../../.github/workflows/cpp-gateway-ci.yml) and built SQLite 3.54.0 under `/usr/local` using the workflow's tarball SHA-256 `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`, `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'` and `make -j4`. `pkg-config --modversion sqlite3` returned `3.54.0`. The runner permits fresh network, mount and PID namespaces through noninteractive sudo. Measurements use loopback mocks and an environment created from an allowlist.

## Reproduction

The [benchmark driver](../../bench/codex_memory/README.md) uses the existing package launcher harness and synthetic production-smoke auth and model fixtures. It seeds exact-version catalog chunks through the gateway, restarts on the populated database, verifies local dispatch and validates text, token usage and delivery. Its synthetic catalog repeats an 8 KiB instruction template across 535 upstream records. Production catalog bytes were unavailable.

Release objects build via `CC=clang CXX=clang++ UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4` with a fresh empty build home and temp, the restored compiler cache and a scratch `UOS_CI_BUILD_DIR`. The package uses those Release production objects and vendor archives, plus `runtime/startup.cpp`, `entrypoints/gateway.cpp` and `src/release.cpp` built with the same flags and the selected Git SHA. The benchmark package producer seals the binary, assets and host closure from a Git archive of that SHA. Ubuntu's multiarch path is selected by the request-path packager. In a private mount namespace, the link, seal and isolated runs see the pinned SQLite at the distribution's library path. The package copy has its default build RUNPATH removed via `patchelf --remove-rpath` and SHA-256 `6c7ccae25f289041e33cbae8daeee19dcd458d0c4fb026d34650bf97e69910bf`. The suite uses the original build at `/usr/local` with SHA-256 `2fb9976405cd8f7afcc474acc90b93c087c4ced76b5302ef75dcd79279f9b238`. The baseline package manifest SHA-256 is `98965354c8402aa3f280092e7e9eff9484d939e68564c1e99f27dbd87bb606f5` and the executable SHA-256 is `7cb64c6448a97815551bc1b437eed2451cc38b75863fc21ae5608c198ef3c9d7`.

The client sends 63,031 bytes and receives 535 model records in 4,812,874 bytes with ETag `"uos-catalog-6dd07e19f58e388cc4b76341260cf951"`. Setup creates eight API keys, a one-account Codex pool, published source and served catalog chunks, and durable usage from 16 settled requests. The fixed fixture size is 540,000 rows and about 0.98 GB on disk. The first trial used 1,750-byte padding values, which spilled into overflow pages in the `WITHOUT ROWID` table and produced 2.54 GB. The final generator uses 700-byte values for three quarters of the padding. The gateway never reads the padding namespace in this workload.

## Baseline attribution

A 975,888,384-byte database and the uninstrumented release package gave the following serial run. The default traffic mode sends a complete catalog request and a completion each time. Each value is KiB. The baseline command used `--no-trim`.

| Checkpoint | VmRSS | RssAnon | VmHWM | [heap] |
| --- | ---: | ---: | ---: | ---: |
| Start | 30,496 | 7,136 | 30,496 | 1,656 |
| Catalog 1 | 121,252 | 93,156 | 121,252 | 1,656 |
| Completion 1 | 125,788 | 93,212 | 125,788 | 1,712 |
| Catalog 10 | 127,836 | 95,260 | 127,836 | 1,712 |
| Completion 10 | 127,836 | 95,260 | 127,836 | 1,712 |
| Catalog 100 | 127,836 | 95,260 | 127,836 | 1,712 |
| Completion 100 | 127,836 | 95,260 | 127,836 | 1,712 |
| Catalog 1,000 | 127,836 | 95,260 | 127,836 | 1,712 |
| Completion 1,000 | 127,836 | 95,260 | 127,836 | 1,712 |

A first session with the trim probe on the 989,523,968-byte database produced the following readings. `[heap]` is resident KiB from the mapping's `Rss` field in `smaps`. The kernel's status counters can lag slightly across threads.

| Checkpoint | VmRSS KiB | RssAnon KiB | VmHWM KiB | [heap] KiB |
| --- | ---: | ---: | ---: | ---: |
| Start | 30,464 | 7,080 | 30,464 | 1,580 |
| First catalog | 121,220 | 93,100 | 121,220 | 1,580 |
| First completion | 125,752 | 93,152 | 125,752 | 1,632 |
| After trim | 56,620 | 24,020 | 127,680 | 1,560 |

`malloc_trim(0)` returned 1 and released 69,132 KiB of anonymous RSS, with 18,410,496 allocated bytes before and after, three arenas, and free allocator bytes falling from 81,896,448 to 52,536,320. The small `[heap]` mapping covers the main arena. Most of the retained RSS was in other anonymous mappings on this glibc build. A single-arena control moved it to `[heap]` without reducing the untrimmed size.

With only the catalog request, anonymous memory went from 5,216 to 93,288 KiB and fell to 23,468 KiB after a successful trim. With only the completion, it went from 6,464 to 8,568 KiB and fell to 8,292 KiB after a successful trim. The catalog causes a large transient with allocator retention plus a one-time memo. The completion causes a small warm-up from the parser, connections and KV statements. The 2.54 GB control plateaued at 95,884 KiB anonymous between sessions 100 and 1,000. The 1,024-row control also reproduced the jump, from 6,284 to 94,404 KiB, falling to 25,244 KiB after trim.

The glibc defaults start the trim and mmap thresholds at 128 KiB. Dynamic tuning raises the mmap threshold when large direct mappings are freed and sets the trim threshold to twice that value. Setting either threshold fixes the pair. The two fixed-threshold controls removed about 42 MiB of untrimmed anonymous RSS. An arena cap alone gave the same large rise and moved it between mappings. The code-backed live catalog bytes remained after trim with all three controls.

| First session, trim probe | Arenas | RssAnon after catalog KiB | RssAnon after completion KiB | RssAnon after trim KiB | VmHWM KiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Defaults | 3 | 93,268 | 93,324 | 24,188 | 127,848 |
| `glibc.malloc.arena_max=1` | 1 | 91,360 | 91,416 | 20,328 | 123,844 |
| `glibc.malloc.mmap_threshold=131072` | 3 | 50,256 | 50,308 | 24,504 | 112,124 |
| `glibc.malloc.trim_threshold=131072` | 3 | 49,876 | 49,876 | 24,060 | 112,108 |

### Heaptrack

Each profile restarted the baseline package on the 540,000-row 2.54 GB fixture. The profiler used `heaptrack --record-only` and was stopped with the fixture's allocations still live. `heaptrack_print --print-peaks 1 --print-allocators 0 --print-temporary 0 --print-leaks 1 -n 12` reported the following MB. The retained column is heap still allocated at the checkpoint, including deliberate caches.

| Isolated trial | Peak live heap MB | Retained MB |
| --- | ---: | ---: |
| Idle | 1.79 | 1.66 |
| Catalog | 81.93 | 17.21 |
| Completion | 4.65 | 2.31 |
| Catalog and completion | 81.93 | 17.78 |

| Trial and allocation site | Peak MB | Retained MB |
| --- | ---: | ---: |
| Catalog, Boost.JSON strings from parsed catalog copies | 22.25 | 4.45 |
| Catalog, copies of served and original JSON strings in `LoadedCodexCatalog` | 19.25 | 9.63 |
| Catalog, string growth in serializer and parser | 9.17 | Small |
| Catalog, response body copy, ETag text and UTF-8 copy | 4.81 each | 0 |
| Catalog, Boost.JSON object storage and copied member names | 4.51 combined | 0.94 |
| Completion, Boost.JSON strings from request and upstream projection | 1.42 | Below 0.02 |
| Completion, simdjson arrays and parser capacity | 0.36 | 0.36 (0.29 above idle) |
| Completion, string growth including the padded input and the wire serializer | 0.27 | 0.14 |
| Completion, SQLite allocations | 0.24 | 0.25 (0.04 above idle) |
| Completion, prepared subscription request and response state | 0.20 combined | Below 0.02 |
| Completion, two 64 KiB HTTP connection buffers | 0.13 | 0.13 |

The catalog memo already has a 32-version LRU and checks durable generation and retention before a hit. A cold hit inflates and verifies both bodies, parses both and keeps the served DOM. A warm hit copies that entire memo. The response path copies the model array again for provider enrichment and filtering, then serializes and hashes it. The large JSON parser buffers are local to a parse above 2 MiB. The 63,031-byte completion exercises the much smaller reusable thread-local parser and padded string. The trace shows OpenSSL startup at about 1.02 MB across all lanes. Static registration on the first HTTP request retains roughly 0.34 MB across asset text copies. ICU, regex compile scratch and terminal log serialization each sit below the largest completion sites. There is one root ICU collator on demand, regex programs belong to their owning objects, and terminal logging writes each record without a growing process-local backlog.

Catalog and auth reads use KV point ranges with `LIMIT 1`; prefix reads use bounded SQL batches. Catalog chunks are reassembled in memory as one document per body. The SQLite WAL connection uses the measured pager defaults `cache_size=-2000` and `mmap_size=0`. The prepared-query cache holds 20 entries. On Heaptrack, SQLite retained 0.21 MB at idle, 0.25 MB after the catalog and 0.25 MB after a standalone completion. The database's total size and statement cache do not explain the jump.

### Early fixture controls

Each tuple lists VmRSS, RssAnon, VmHWM and resident `[heap]` in KiB. The 2.54 GB control was a complete session run with the trim probe. The 4.6 MB smoke was a single session. The intermediate setup failure was a mismatched synthetic admin token during usage verification. The fixture now uses the request-path bench's token for both setup and verification.

| Fixture | Start | Catalog 1 | Completion 1 | Catalog/Completion 10 | Catalog/Completion 100 | Catalog/Completion 1,000 | Trim |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2.54 GB | 29200/5768/29200/264 | 122004/93836/122004/264 | 126420/93836/126420/264 | 126484/93836/126484/264 | 128532/95884/128532/264 | 127236/95884/128532/264 | 58400/27032/129284/264 |
| 4.6 MB | 29720/6284/29720/784 | 122524/94352/122524/784 | 127056/94404/127056/836 | | | | 57896/25244/128976/764 |

## Share the catalog and move request data

The first change shares the verified catalog with in-flight requests, applies provider enrichment on one working copy, and moves the serialized result into the response. ETag hashing reads the validated UTF-8 bytes directly. The cold and stale paths still verify durable content and auth generations. Existing catalog store assertions now use the shared handle, with the same comparisons for body, source, eviction and generation.

The measured source is `d361e1014da121f50a4c1f97c66479259330b323`. The manifest SHA-256 is `ef1f2fceb6f27a047e1213e859758f7052d75c4ca9199f1eba6170fef0c1130a` and the executable SHA-256 is `3d12eb096e03a6fb04dbcc41e6da8eba0449a4d2adcbb97faf8d0e18836631a7`. All 11 Release catalog binaries passed (125 cases). The first-session anonymous reading dropped from 93,324 to 42,992 KiB in matched trim-probe runs and fell to 25,548 KiB after trim. The catalog-only Heaptrack peak fell from 81.93 to 59.70 MB, and allocation calls fell from 347,767 to 164,744. Retained heap was 17.26 MB after the catalog, with source and served bytes still held in the memo. All catalog trials preserved the 4,812,874-byte payload and ETag.

| Checkpoint with trim probe | VmRSS KiB | RssAnon KiB | VmHWM KiB | [heap] KiB |
| --- | ---: | ---: | ---: | ---: |
| Start | 30,440 | 6,996 | 30,440 | 1,484 |
| Catalog 1 | 70,220 | 42,424 | 72,376 | 1,484 |
| Completion 1 | 74,944 | 42,476 | 74,944 | 1,536 |
| Catalog and completion 10 | 79,040 | 46,572 | 79,040 | 1,536 |
| Catalog and completion 100 | 79,040 | 46,572 | 79,040 | 1,536 |
| Catalog and completion 1,000 | 79,040 | 46,572 | 79,040 | 1,536 |
| Trim after 1,000 | 57,924 | 25,456 | 81,004 | 1,464 |

A 128 KiB fixed mmap threshold trial on this build had 27,344 KiB of anonymous memory after the first session and 24,236 KiB after trim. The unmodified threshold still let the first catalog grow the arena. The next change keeps the verified served view in an 8 MiB serialized-byte LRU and reads the source body when a stale refresh needs it.

## Keep only the served catalog in the memo

The memo applies its 32-version limit and an 8 MiB budget for the sum of served JSON lengths. This limits the amount of catalog data represented by retained strings and DOMs. Bigger catalogs can still be served from verified durable state. The fresh path skips the original body. Stale refresh loads and verifies that body before sending an upstream conditional request. The first focused run reached 123/126 cases because three OpenRouter tests still read the original body from the eager field. The tests now read it through the source loader and retain the original byte, ETag and revalidation assertions. The rerun passed all 11 binaries and 126 cases, including a new byte-eviction case with an in-flight reader.
