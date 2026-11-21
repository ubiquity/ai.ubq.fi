# Native gateway memory under Codex CLI traffic

## Checklist

- [x] Identify the integration base and qualify the runner, SQLite and namespace prerequisites.
- [x] Implement a loopback reproduction with a populated Bun/JSC KV database.
- [x] Measure Release memory at startup and after 1, 10, 100 and 1,000 sessions. Isolate the catalog and completion paths.
- [x] Profile allocation peaks and retained bytes, and measure the effect of `malloc_trim(0)`.
- [x] Fix measured waste and repeat the same workload after each fix.
- [x] Add the Release budget check and document small-board use.
- [x] Record the final focused, shard and registration gates.

## Runner

The starting tree is `8f11c1a264bcdd6bef668139bf1a59031dd962fe` on `claude/cpp-gateway-port`. The runner has four ARM64 CPUs, Ubuntu 26.04.1, Linux 7.0.0-1020-azure, glibc 2.43, Clang 21.1.8, Boost 1.90, ICU 78.2 and Heaptrack 1.5.0. The job's compiler cache is in use.

The runner installed the workflow dependencies and built SQLite 3.54.0 under `/usr/local` using the workflow's tarball SHA-256 `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`, `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'` and `make -j4`. `pkg-config --modversion sqlite3` returned `3.54.0`. The runner permits fresh network, mount and PID namespaces through noninteractive sudo. Measurements use loopback mocks and an environment created from an allowlist. [C++ workflow dependencies](../../../../.github/workflows/cpp-gateway-ci.yml)

## Reproduction

The reported service moved from about 5 MiB anonymous and 15 MiB resident at idle to 57 MiB anonymous and 103 MiB peak resident after the first two requests. The runner reproduces the pattern with synthetic data and its own library mappings. It shows that the first catalog accounts for nearly all of the increase, with a smaller live memo and a much larger allocator high-water effect.

The benchmark driver uses the existing package launcher harness and synthetic production-smoke auth and model fixtures. It seeds exact-version catalog chunks through the gateway, restarts on the populated database, verifies local dispatch and validates text, token usage and delivery. Its synthetic catalog repeats an 8 KiB instruction template across 535 upstream records. Production catalog bytes were unavailable. [Benchmark driver](../../bench/codex_memory/README.md)

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

Catalog and auth reads use KV point ranges with `LIMIT 1`. Prefix reads use bounded SQL batches. Catalog chunks are reassembled in memory as one document per body. The SQLite WAL connection uses the measured pager defaults `cache_size=-2000` and `mmap_size=0`. The prepared-query cache holds 20 entries. On Heaptrack, SQLite retained 0.21 MB at idle, 0.25 MB after the catalog and 0.25 MB after a standalone completion.

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

Source `66e0f1a3a234dd8c0e2b8d53f7c129f70b99ae96` has manifest SHA-256 `ca8198148a1aa1adaf463672e17b716f45711b19c0879fb8ddfdbc9237854176` and executable SHA-256 `e36eaa27328780c5b231f24f41876f9e175ef11bb770a1760d1de0ffcd8a1a34`. The first-session anonymous reading fell from 42,992 to 39,220 KiB. Trim returned 1 and left 19,632 KiB, down from 25,548 KiB. The live allocator count went from 18,502,224 to 13,661,936 bytes, including direct mappings, about one original catalog body.

The catalog-only Heaptrack peak dropped from 59.70 to 49.36 MB and live bytes dropped from 17.26 to 12.44 MB. In the 1,000-session run without a trim probe, the adaptive allocator settled at 42,480 KiB anonymous and 74,864 KiB resident from session 10.

## Fix the glibc mmap threshold

With the bounded memo and the default adaptive thresholds, the warmed allocator still held about 19 MiB of anonymous RSS that trim could release. Setting the mmap threshold to 128 KiB for the same package brought first-session anonymous RSS down to 22,852 KiB, peak RSS to 60,528 KiB and trimmed anonymous RSS to 19,948 KiB. The gateway entry point now fixes glibc's mmap threshold at that initial value before starting runtime threads. Large allocations can use individually releasable mappings throughout the lifetime of the service.

## Final Release measurement

The measured runtime commit is `3bcb04602f530d4d1e0148736ec5919d084cc48a`. The manifest SHA-256 is `203d8151768166b84d517010c13d921265df2f48b08ff617b06a134717138b80` and executable SHA-256 is `60bf72de0a9211b366db6e28f8e247e195f458873fd153d6de674974d25355ff`. The uninstrumented command used `--no-trim` with the default 1,000 sessions on the 989,941,760-byte database. The Responses request SHA-256 is `82da9db93bcfe28856cdb0def03cedf617c52b0b14e734571fda82b03b5a583f`. The final and baseline runs delivered the same catalog bytes and ETag, 1,000 successful streamed responses and 1,000 unique delivery terminals. The test key had 1,016 settled requests after setup and traffic.

| Checkpoint | VmRSS KiB | RssAnon KiB | VmHWM KiB | [heap] KiB |
| --- | ---: | ---: | ---: | ---: |
| Start | 30,196 | 6,880 | 30,196 | 1,396 |
| Catalog 1 | 51,672 | 23,876 | 61,952 | 1,396 |
| Completion 1 | 56,336 | 23,932 | 61,952 | 1,452 |
| Catalog 10 | 56,336 | 23,932 | 67,200 | 1,452 |
| Completion 10 | 56,336 | 23,932 | 67,200 | 1,452 |
| Catalog 100 | 56,336 | 23,932 | 69,312 | 1,452 |
| Completion 100 | 56,336 | 23,932 | 69,312 | 1,452 |
| Catalog 1,000 | 56,336 | 23,932 | 69,312 | 1,452 |
| Completion 1,000 | 56,336 | 23,932 | 69,312 | 1,452 |

First-session anonymous RSS went from 91.0 to 23.4 MiB. Peak RSS over 1,000 sessions went from 124.8 to 67.7 MiB. Both resident readings were flat from 100 to 1,000 sessions. The final Heaptrack result remained at 49.36 MB peak live and 12.44 MB retained for the catalog. The allocator change reduced resident pages while preserving the live allocation profile.

## Regression check

The check sends one catalog request and 100 completions on the fixed fixture. The two final trials had 53,536 and 53,040 KiB of resident memory after trim. The 68 MiB limit gives 30% headroom above the higher reading. The 32 MiB anonymous limit uses pre-trim checkpoints so the check also detects allocator retention during ordinary use.

| Package | Highest warm RssAnon KiB | Post-trim VmRSS KiB | Trim return | Check |
| --- | ---: | ---: | ---: | --- |
| Final | 23,540 | 53,536 | 1 | Pass |
| Final repeat | 23,272 | 53,040 | 1 | Pass |
| Baseline | 94,404 | 58,204 | 1 | Fail |

Run the serial measurement, first-session trim sample, profiles or budget with the same package and a fresh output directory per invocation.

```sh
bash cpp/gateway/bench/request_path/isolated.sh --codex-memory --package "$sealed_package" --out "$scratch/serial" --no-trim
bash cpp/gateway/bench/request_path/isolated.sh --codex-memory --package "$sealed_package" --out "$scratch/first" --requests 1
bash cpp/gateway/bench/request_path/isolated.sh --codex-memory --package "$sealed_package" --out "$scratch/catalog" --traffic catalog --requests 1 --heaptrack
bash cpp/gateway/bench/request_path/isolated.sh --codex-memory --package "$sealed_package" --out "$scratch/check" --check
```

## Supplemental measurements

Each tuple contains VmRSS, RssAnon, VmHWM, resident [heap] and virtual [heap] size in KiB. Budget rows use the 100th completion. ([heap] denotes the main arena mapping.)

| Trial | Database bytes | Startup | First catalog | Completion | After trim |
| --- | ---: | --- | --- | --- | --- |
| baseline first, setup repeat | 989,523,968 | 30464 / 7080 / 30464 / 1580 / 1700 | 121220 / 93100 / 121220 / 1580 / 1700 | 125752 / 93152 / 125752 / 1632 / 1700 | 56620 / 24020 / 127680 / 1560 / 1700 |
| baseline catalog, setup repeat | 975,888,384 | 28664 / 5216 / 28664 / 1760 / 1884 | 121472 / 93288 / 121472 / 1764 / 1884 |   | 51652 / 23468 / 123520 / 1756 / 1884 |
| baseline completion, setup repeat | 989,941,760 | 29900 / 6464 / 29900 / 960 / 1084 |   | 40260 / 8568 / 40260 / 1016 / 1084 | 39984 / 8292 / 42240 / 940 / 1084 |
| baseline first, without probe | 975,888,384 | 29204 / 5824 / 29204 / 340 / 408 | 119960 / 91844 / 119960 / 340 / 408 | 124440 / 91844 / 124440 / 340 / 408 |   |
| baseline single arena, setup repeat | 989,802,496 | 26944 / 3496 / 26944 / 2092 / 2160 | 119748 / 91564 / 119748 / 90156 / 92272 | 124228 / 91564 / 124228 / 90156 / 92272 | 53136 / 20472 / 124212 / 19060 / 65648 |
| baseline fixed mmap, setup repeat | 989,802,496 | 30084 / 6636 / 30084 / 1132 / 1256 | 79048 / 50864 / 111968 / 1132 / 1256 | 83584 / 50920 / 111968 / 1188 / 1256 | 57788 / 25124 / 111968 / 1112 / 1256 |
| baseline fixed trim, setup repeat | 975,888,384 | 30536 / 7088 / 30536 / 1584 / 1708 | 77448 / 49264 / 111712 / 1584 / 1708 | 81984 / 49320 / 111712 / 1640 / 1708 | 56220 / 23556 / 111712 / 1572 / 1708 |
| baseline first | 989,802,496 | 30700 / 7252 / 30700 / 1748 / 1872 | 121452 / 93268 / 121452 / 1748 / 1872 | 125988 / 93324 / 125988 / 1804 / 1872 | 56852 / 24188 / 127848 / 1736 / 1872 |
| baseline catalog | 976,490,496 | 28664 / 5216 / 28664 / 1760 / 2044 | 121632 / 93448 / 121632 / 1924 / 2044 |   | 51816 / 23632 / 123676 / 1864 / 2044 |
| baseline completion | 975,888,384 | 30156 / 6772 / 30156 / 1268 / 1392 |   | 40516 / 8876 / 40516 / 1324 / 1392 | 40224 / 8584 / 42432 / 1248 / 1392 |
| baseline single arena | 975,888,384 | 28788 / 5340 / 28788 / 1888 / 2012 | 119544 / 91360 / 119544 / 1888 / 2012 | 124080 / 91416 / 124080 / 1944 / 2012 | 52992 / 20328 / 123844 / 1820 / 2012 |
| baseline fixed mmap | 992,014,336 | 29476 / 6028 / 29476 / 528 / 648 | 78440 / 50256 / 112124 / 528 / 648 | 82972 / 50308 / 112124 / 580 / 648 | 57168 / 24504 / 112124 / 504 / 648 |
| baseline fixed trim | 989,523,968 | 29080 / 5648 / 29080 / 148 / 216 | 78044 / 49876 / 112108 / 148 / 216 | 82524 / 49876 / 112108 / 148 / 216 | 56708 / 24060 / 112108 / 148 / 216 |
| baseline idle trim | 992,014,336 | 29536 / 6152 / 29536 / 652 / 772 |   |   | 31428 / 8044 / 31500 / 652 / 772 |
| baseline Heaptrack idle | 2,540,957,696 | 35316 / 8164 / 36392 / 1696 / 1816 |   |   |   |
| baseline Heaptrack catalog | 2,540,957,696 | 36136 / 8980 / 37308 / 464 / 540 | 128492 / 97048 / 128652 / 464 / 540 |   |   |
| baseline Heaptrack completion | 2,540,957,696 | 34960 / 7812 / 35220 / 1344 / 1464 |   | 47932 / 13104 / 48140 / 1388 / 1464 |   |
| baseline Heaptrack session | 2,541,101,056 | 35028 / 7872 / 36272 / 1404 / 1520 | 127384 / 95940 / 127516 / 1404 / 1520 | 132864 / 97132 / 132996 / 1444 / 1520 |   |
| shared catalog first | 975,888,384 | 28960 / 5516 / 28960 / 2052 / 2060 | 70788 / 42992 / 72960 / 34820 / 36876 | 75460 / 42992 / 75460 / 34820 / 36876 | 58016 / 25548 / 77312 / 15408 / 20492 |
| shared catalog fixed mmap | 989,941,760 | 29088 / 5644 / 29088 / 2180 / 2248 | 55140 / 27344 / 65488 / 14468 / 14536 | 59812 / 27344 / 65488 / 14468 / 14536 | 56704 / 24236 / 65488 / 9384 / 14536 |
| shared catalog Heaptrack | 975,888,384 | 34812 / 7660 / 35988 / 1184 / 1300 | 76192 / 45136 / 78980 / 1184 / 1300 |   |   |
| bounded memo first | 989,802,496 | 29224 / 5840 / 29224 / 332 / 400 | 67084 / 39220 / 67084 / 332 / 400 | 71692 / 39220 / 71692 / 332 / 400 | 52104 / 19632 / 73608 / 332 / 400 |
| bounded memo fixed mmap | 989,941,760 | 29244 / 5860 / 29244 / 352 / 420 | 50716 / 22852 / 60528 / 352 / 420 | 55324 / 22852 / 60528 / 352 / 420 | 52420 / 19948 / 60528 / 352 / 420 |
| bounded memo Heaptrack | 989,941,760 | 35084 / 7992 / 36396 / 1520 / 1640 | 72496 / 41372 / 72720 / 1520 / 1640 |   |   |
| fixed allocator first | 975,888,384 | 28888 / 5508 / 28888 / 2052 / 2056 | 50364 / 22504 / 59800 / 14340 / 14344 | 54972 / 22504 / 59800 / 14340 / 14344 | 51948 / 19480 / 59800 / 9336 / 14344 |
| fixed allocator Heaptrack | 976,490,496 | 34488 / 7404 / 34588 / 932 / 1008 | 55516 / 24400 / 65596 / 932 / 1008 |   |   |
| final budget | 989,941,760 | 29876 / 6492 / 29876 / 984 / 1104 | 51352 / 23488 / 62084 / 984 / 1104 | 56012 / 23540 / 62084 / 1036 / 1104 | 53536 / 21064 / 62084 / 992 / 1104 |
| final budget repeat | 989,941,760 | 29540 / 6220 / 29540 / 712 / 836 | 51016 / 23216 / 61104 / 712 / 836 | 55680 / 23272 / 61104 / 768 / 836 | 53040 / 20632 / 61104 / 728 / 836 |
| baseline budget | 989,941,760 | 29664 / 6280 / 29664 / 776 / 900 | 122468 / 94348 / 122468 / 776 / 900 | 127004 / 94404 / 127004 / 832 / 900 | 58204 / 25604 / 128984 / 776 / 900 |

The allocator columns are byte counts from `mallinfo2`. Live arena and mmap bytes were the same before and after each trim.

| Trim trial | Arenas | Live arena bytes | Mmap bytes | Free arena bytes before → after |
| --- | ---: | ---: | ---: | ---: |
| baseline first, setup repeat | 3 | 18,410,496 | 0 | 81,896,448 → 52,536,320 |
| baseline catalog, setup repeat | 3 | 17,811,328 | 0 | 80,586,880 → 53,323,904 |
| baseline completion, setup repeat | 3 | 2,604,064 | 0 | 6,894,560 → 6,894,560 |
| baseline single arena, setup repeat | 1 | 18,407,680 | 0 | 76,078,848 → 48,815,872 |
| baseline fixed mmap, setup repeat | 3 | 8,778,384 | 9,633,792 | 34,450,800 → 34,450,800 |
| baseline fixed trim, setup repeat | 3 | 8,781,328 | 9,633,792 | 32,813,552 → 32,813,552 |
| baseline first | 3 | 18,411,472 | 0 | 79,974,448 → 52,711,472 |
| baseline catalog | 3 | 17,811,648 | 0 | 80,750,400 → 53,487,424 |
| baseline completion | 3 | 2,577,120 | 0 | 7,236,896 → 7,236,896 |
| baseline single arena | 1 | 18,409,568 | 0 | 75,925,408 → 48,662,432 |
| baseline fixed mmap | 3 | 8,756,176 | 9,633,792 | 33,850,416 → 33,850,416 |
| baseline fixed trim | 3 | 8,760,320 | 9,633,792 | 33,403,904 → 33,403,904 |
| baseline idle trim | 3 | 1,953,696 | 0 | 5,128,288 → 5,128,288 |
| shared catalog first | 3 | 13,685,328 | 4,816,896 | 28,270,000 → 11,492,784 |
| shared catalog fixed mmap | 3 | 8,878,944 | 9,633,792 | 10,200,224 → 10,200,224 |
| bounded memo first | 3 | 8,845,040 | 4,816,896 | 29,313,296 → 12,536,080 |
| bounded memo fixed mmap | 3 | 9,007,920 | 4,816,896 | 10,296,528 → 10,296,528 |
| fixed allocator first | 3 | 8,875,120 | 4,816,896 | 10,007,440 → 10,007,440 |
| final budget | 3 | 9,374,608 | 4,816,896 | 10,630,256 → 10,630,256 |
| final budget repeat | 3 | 9,239,856 | 4,816,896 | 10,490,576 → 10,490,576 |
| baseline budget | 3 | 18,736,224 | 0 | 80,751,520 → 53,488,544 |
| baseline 1,024 rows | 3 | 18,399,504 | 0 | 81,092,336 → 53,829,360 |
| baseline 2.54 GB, 1,000 sessions | 3 | 20,040,736 | 0 | 80,962,528 → 51,602,400 |
| shared catalog, 1,000 sessions | 3 | 15,246,592 | 4,816,896 | 28,338,944 → 9,464,576 |

## Final verification

The accepted commit is `e96224b08552cd1a4522798602638f1c96fc9180`, tree `cf4218659393de6addb18eec055c3bef90d1c640`. All three native receipts confirm stable build inputs with SHA-256 `abf7386d4e2fb735dbedefe6db0da1d918c1f86692895985b5f5f297661e117c` and SQLite 3.54.0. The commands used Clang with four jobs and the CI driver's credential-free OS isolation.

| Command | Binaries passed | Cases passed | Receipt SHA-256 |
| --- | ---: | ---: | --- |
| `python3 -B "$scratch/affected.py" --config release` | 75 | 1,047 | `e7efc45ed505fd3d0396abc6024e25706e70a3135fc0b6a378ab7d4af0bc6122` |
| `python3 -B "$scratch/affected.py" --config asan` | 75 | 1,047 | `01e642f3aae05a505ff8eb29defa26205dc06b176d407b143a9cb2f74356417c` |
| `bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4` | 114 | 1,334 | `2e2129b2ea38b669cf65cdf1db1c9c311565f78ae905f41f7e897e53c71a7c32` |

The affected invocation selects the full registered link closure for the three changed application sources and calls the same CI driver. This is the scratch `affected.py` used for both configurations.

```python
from pathlib import Path
import runpy
import sys

gateway = Path("cpp/gateway").resolve()
sys.path.insert(0, str(gateway / "tools"))
import links

changed = {"src/catalog/index.cpp", "src/catalog/models.cpp", "src/catalog/store.cpp"}
registry = links.read_registry()
tests = [test for test in links.all_tests() if changed.intersection(links.resolve(test, registry).sources)]
assert len(tests) == 75
links.all_tests = lambda: tests
runpy.run_path(str(gateway / "cmake/ci/driver.py"), run_name="__main__")
```

`python3 -B cpp/gateway/tools/links.py --check` passed all 453 registrations. `git diff --check` passed. The prewarm sanitizer run overlapped edits and the driver rejected it as acceptance evidence. The final runs above used committed inputs throughout.

The accepted commit was also sealed and run with `--check`. The manifest SHA-256 is `7eacbd0cfa92ad0a639fdcd35635ca8f711ed77fb75f9158f4b84613c823ad34`, the executable SHA-256 is `d83fc59813fee2394255dfce07ab9ff1110e7ec3955e736c57686c355ce82921`, and the budget receipt SHA-256 is `e9b6ebe14e55cea141e06e8abf2a40c17e3adc1a4d9bff46959d2536fada5aab`. The 989,941,760-byte fixture gave the following readings in KiB. Trim returned 1 with three arenas, 9,232,752 live arena bytes, 4,816,896 direct-mapped bytes and 10,534,544 free arena bytes before and after. The original request-path launcher passed `--scenarios models stream --requests 1 --warmup 0 --repetitions 1 --concurrency 1` with the same package, including the progressive SSE proof and two delivered terminals.

| Checkpoint | VmRSS | RssAnon | VmHWM | [heap] |
| --- | ---: | ---: | ---: | ---: |
| Start | 29,636 | 6,256 | 29,636 | 748 |
| Catalog | 51,112 | 23,252 | 60,544 | 748 |
| Completion 1, 10 and 100 | 55,776 | 23,308 | 60,544 | 804 |
| Trim | 53,116 | 20,648 | 60,544 | 760 |

### Committed-diff review

The native Codex 0.171.0 CLI used both refs fetched into an isolated scratch checkout. The base was `8f11c1a264bcdd6bef668139bf1a59031dd962fe` and the head was the accepted commit above. The review model was `gpt-6.1-sol`. The loopback-only command returned 124 after logging `failed to connect to Responses WebSocket stage=Unknown`. There was no complete review result.

```sh
timeout -k 2s 20s codex review --base refs/review/base \
  -c 'model="gpt-6.1-sol"' -c 'review_model="gpt-6.1-sol"' \
  -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="none"' \
  -c 'shell_environment_policy.set={PATH="/usr/bin:/bin"}'
```
