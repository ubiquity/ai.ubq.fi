# llhttp HTTP client response findings

## Work contract v1

Replace OSS plan rows 7 and 8 with the pinned llhttp 9.4.4 release. The baseline is the clean C++ port at `093a9e0511e9d0deae5495dc324e53ff4dccd719`. The HTTP response parser must preserve the runtime interfaces, response headers, streaming and failure boundaries required by gateway callers. Use strict llhttp behavior for wire grammar. Preserve bounded head reads, header counts where applicable, raw field order and duplicates, trailing bytes at message boundaries, and ordinary JavaScript-style failures.

| Consumer            | Boundary and behavior                                                                                                            |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| Fetch response head | Parse incrementally, skip informational responses, reject unsolicited upgrades, and preserve status, reason and ordered fields   |
| Fetch response body | Parse Content-Length, chunked and EOF-delimited bodies, honor bodyless response semantics, and keep content decoding separate    |
| WebSocket handshake | Parse a bounded response head, retain the raw header representation, and hand bytes beyond the head to the existing upgrade path |

The request writer, networking lifecycle, content decoders, WebSocket upgrade validation and frame handling, server request parsing, TypeScript, deployment and credentials are outside this change. The owned production regions are the fetch response parser, its internal parser types and the WebSocket response-head function. Vendor registration, component tests and fuzzing, documentation and the report are the other assigned areas.

The body pump and existing component expectations are outside the enumerated paths. Scope clarification is pending. The initial work uses the owned paths. Further unknowns include llhttp's strict grammar at the accepted boundaries and support for multi-file C archives in existing build entry points.

Verify the archive and individual file hashes, compile with the port's fatal warning flags, run component and affected closures plain and under ASan/UBSan, and run the library harness for 120 seconds. Compare the baseline and candidate in a matched optimized parser benchmark with allocation counting. Stop after the gates settle or a specific scope, toolchain or runner limit prevents further verification. Record commands, exact outcomes and the remaining work here.

## Runner

Ubuntu 24.04.5 ARM64 with four CPUs, GCC 14.2.0 and Bun 1.4.0. Build outputs and downloads use an external temporary directory. Test and oracle processes use fresh working, home and temporary directories and an environment allowlist. Passwordless privileged network namespace setup is available. Unprivileged user namespace mapping fails on this runner.

```sh
curl -fsSL https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.4.4 -o "$scratch/llhttp.tar.gz"
printf '%s  %s\n' 5600adb1498f82a9c719b8eb19310021737388e0d92362a6d47be532710d3c96 "$scratch/llhttp.tar.gz" | sha256sum -c
```

The archive hash matched. Runtime, test and performance receipts are pending.

## Continuation contract v2

The continuation starts at commit `542c1aff6f3305f39541e6f2553db7eef7cd1482`, tree
`2beb8ee0c43c44f0b9d5abbbfa686d8ef344d4c4`. Verify the committed replacement and finish qualification using strict library grammar. Fetch preserves bounded heads, ordered fields, interim responses, bodyless semantics, streamed delivery and safe connection reuse. WebSocket preserves the 16 KiB head bound, Latin-1 fields, and the first frame when it arrives in the same read as the upgrade. The owned paths cover the shared parser, the WebSocket response head, vendor registration, the new tests and fuzz target, and documentation. The request writer, connection lifecycle, content decoding, other lanes, deployment and credentials are outside this work. Two scope questions remain open because the existing fixed-length and EOF body pump and the Bun-era expectation table are in files absent from the owned-path list.

The completion gates are strict GCC 14 optimized and Clang 21 ASan/UBSan builds, isolated fixture runs through the Links registry, 120 seconds of libFuzzer on the library adapter, and same-runner optimized parser timing and allocation counts against `093a9e05`. The focused build is expected to take roughly two minutes, a full compile and suite tens of minutes, and fuzz execution two minutes after compilation. Each build uses its own external output and one execution handle, with status summaries from its logs and per-test receipts. A compile or fixture failure is diagnosed against the inherited tree before attributing it to the parser. Stop after the authorized changes and gates settle, recording unresolved ownership or runner limits with the exact evidence.

## Inherited build

Rebuilt official Boost 1.92.0 with `./bootstrap.sh --with-libraries=json,container --with-toolset=gcc` and
`./b2 -j2 toolset=gcc-14 --with-json --with-container variant=release link=static runtime-link=shared threading=multi --stagedir="$scratch/boost-stage" stage`. Built SQLite 3.54.0 with
`./configure --prefix="$scratch/sqlite-prefix" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`
and `make -j1 && make install`. The SQLite archive matched the checksum in [CI.md](../CI.md). Installed Clang 21.1.8 from the LLVM Noble repository. Bun reported 1.4.0 with `bun --no-env-file --version`.

The pinned llhttp archive matched its published SHA-256. All five vendored files matched the specified archive entries by `cmp`, and `sha256sum -c SHA256SUMS` passed 5/5. The vendored licence is the upstream archive's `LICENSE` entry.

The inherited focused release gate compiled 44/44 runtime objects and seven test binaries with GCC 14.2.0 and the fatal port warnings. Six binaries passed and one failed, with 74/75 cases passing. The new library test passed 8/8. The existing fetch transport binary passed 32/33. Its grouped Bun response grammar case failed on expected strict-grammar differences. Existing WebSocket runtime, lifetime and connection fixtures passed. The runner used the repository's Linux network, mount and PID fixture boundary through passwordless sudo and restored the invoking uid before each test. Inputs were unchanged across the run.

The portable build driver at `cmake/ci/driver.py` currently generates a vendor archive from only `Library.source` with the C++ driver. It omits the other two llhttp C translation units. The scratch runner uses the repository's source closure, fixture boundary and test runner, with an external Ninja generation adjustment to compile all three vendor sources as C and archive them. The native CMake registration already selects all three sources. The portable driver itself is outside the assigned paths.

## Candidate and source scope

The checked source commit is `8c988f466cd96416877d2058156f0e4f35fb7015`, tree
`ba45a45c30c4753daee7b825a67166c5f0e92634`. Compared with `093a9e05`, the three first-party production files add 210 lines and remove 284 lines, for a net reduction of 74 physical lines. The removed code includes the fetch status and field scanner, numeric Content-Length parser, chunked state machine, and WebSocket status and field scanner.

| Production file                | Added | Removed |
| ------------------------------ | ----: | ------: |
| `fetch_transport_http.cpp`     |   148 |     223 |
| `fetch_transport_internal.hpp` |    40 |      20 |
| `websocket_client.cpp`         |    22 |      41 |

The client adapter pauses llhttp at the head and message boundaries. It keeps the fetch head and field quotas, HTTP/1.x and reason-phrase validation, raw field order and duplicates, and the fetch bodyless response rules. The transfer-coding guard rejects repeated or nonfinal `chunked` and catches cases such as `Transfer-Encoding: chunked,` where llhttp would otherwise stream chunk delimiters as close-delimited payload. The WebSocket adapter keeps its 16 KiB head quota and preserves bytes belonging to the first frame. It moves ASCII strings directly and converts Latin-1 values only when they contain a non-ASCII byte. Socket-level request writing, response decoding and connection lifecycle retain their existing paths.

The live fetch pump uses llhttp to classify Content-Length and EOF-delimited responses, then performs the remaining fixed-length delivery and EOF completion in `src/rt/fetch_transport.cpp`. The new parser can consume those bodies and tests exercise that interface. Connecting the interface to the live pump requires ownership of the response-read and body-framing blocks in that file. The Bun-era assertion table in `tests/rt/fetch_transport_test.cpp` needs a separate scoped update. The original test remains in the registry and ran in each relevant gate. [Recorded grammar and error differences](../DEVIATIONS-FROM-BUN.md)

The llhttp sources, licence, checksums, provenance and common CMake/Python registration are byte-identical to the server lane at `d0d2ed8b15725c8005671a4155abe216f727051a`. The client registry adds `src/rt/fetch_transport_http.cpp` and `src/rt/websocket_client.cpp` as consumers. The combined registry will also need the server lane's consumers.

## Commands and focused verification

The external scratch directory is `/tmp/cppgw-std-llhttp-client-resume`. The scratch `check.sh` invokes `python3 -B "$scratch/gateway-check.py" --config release|asan`. That adapter calls the existing portable driver's Links resolution, Ninja build and isolated fixture runner. It emits the three vendor C compilation edges and one static archive, and passes `--system-header-prefix=third_party/` to the Clang compile of the existing simdutf consumer. The latter fixed a build failure from signedness warnings in the upstream simdutf header while retaining `-Werror` for first-party code. The driver used `-O2` with GCC 14.2 for release and `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` with Clang 21.1.8 for sanitizers, with the fatal port warning flags and native-library defines in both profiles.

```sh
"$scratch/check.sh" release focused 4
"$scratch/check.sh" release full 3
"$scratch/check.sh" asan focused 2
"$scratch/check.sh" asan runtime 1
python3 -B cpp/gateway/tools/vendor_sources.py cmake
cmake --build "$scratch/native-cmake" --target uos_vendor_llhttp --parallel 2
ar t "$scratch/native-cmake/libuos_vendor_llhttp.a"
```

The native CMake build used GCC 14, Ninja, `UOS_GATEWAY_BUILD_EVIDENCE=1`, and `UOS_GATEWAY_TOOL_ROOT="$scratch"`. Its archive contains `llhttp.c.o`, `api.c.o`, and `http.c.o`. GCC 14 and Clang 21 also compiled the final WebSocket source independently with the specified strict warning flags and `-fsyntax-only`.

The final candidate passed 44/44 focused runtime compilations. The Clang sanitizer gate ran seven binaries and 75 cases. Six binaries and 74 cases passed. Fetch and WebSocket parser cases passed 8/8, WebSocket client cases 17/17, and the WebSocket lifetime and application runtime tests 4/4 and 2/2. The fetch transport binary passed 32/33, with the Bun-specific expectation group failing. No address, undefined-behavior, or leak diagnostics were reported. The source digest at the end matched the initial digest.

The final fuzz command was `python3 -B cpp/gateway/fuzz/ci.py fetch_response_library --seconds 120 --jobs 1 --out "$scratch/final-fuzz"`. It used Clang 21, the seven committed seed files, an empty home, a fresh temporary directory, and an isolated network namespace with no interfaces raised. The log reported 1,938,757 executions in 122 seconds, exit 0, zero crashes, timeouts or out-of-memory events, and zero finding artifacts. The resulting binary SHA-256 is `1b3937360931bd9abd0e97f5e77aca47974d307b6e50583d75ec7229168d07ec`.

Bun 1.4.0 ran the deviation fixtures using `bun --no-env-file --no-install response.ts` against synthetic loopback responses in a separate network namespace. A C probe of the verified llhttp C archive checked reason-phrase spacing, parser status codes, and the numerical size of a padded chunk. All fixtures and compilation outputs stayed outside the checkout. No provider environment was passed into tests or the Bun oracle.

## Measured parser costs

Both benchmark binaries used GCC 14.2, `-O2 -ffunction-sections -fdata-sections`, the same driver and Boost 1.92 headers, and `mold --gc-sections`. The benchmark compiled the deployed parser entry points from the reference commit and candidate, plus the same runtime dependencies. Only the candidate linked the three llhttp C files. The allocation counter wrapped `malloc`, `calloc`, `realloc` and C++ allocation operators. Each process warmed up 500 operations per workload, then ran nine batches of 100,000 operations on the short workloads, 35,000 on split heads, and 10,000 on the 64 KiB body. Four processes ran in reference, candidate, candidate, reference order pinned to CPU 3. The table reports the mean of the two batch medians in thread CPU nanoseconds per operation. All response lengths and body sizes matched.

| Workload                              | Before ns/op | After ns/op | Before allocations/op | After allocations/op |
| ------------------------------------- | -----------: | ----------: | --------------------: | -------------------: |
| Fetch Content-Length head             |        394.1 |       466.7 |                     7 |                    7 |
| Fetch split head in 128-byte feeds    |      3,180.6 |     2,955.7 |                    41 |                   45 |
| Fetch head and four 9-byte SSE chunks |        461.8 |       661.1 |                     9 |                    9 |
| Fetch head and eight 8 KiB chunks     |      2,732.6 |     2,953.5 |                    11 |                   11 |
| WebSocket 101 with first frame        |        652.9 |       503.4 |                     8 |                    8 |

The split-head test repeats head parsing on each cumulative read, as the live fetch caller does today. The chunk tests include the synthetic head setup that the live standalone chunk decoder currently uses. The allocation count covers the parser and adapter and excludes socket, TLS and content-decoder work. The reference and candidate binaries had SHA-256 digests `8d4c92f59bf95370890798eefea02652087b9d2cf13b28d8311fe65127eee8a5` and `343451f73fdf5a2e4a4d119594438953573a1d1b4cf51cdc96454c3198a70b42`.

## Build integration and branch handoff

The standard portable driver and the current macOS shell builder assume one implementation source per vendor archive. The production-host `tools/unit.mk` has only a C++ vendor object rule. Those entry points need multi-source C archive support. This Linux disposable runner used all three sources and the native CMake static target verified the intended archive.

The GitHub push dry run returned `could not read Username for 'https://github.com': terminal prompts disabled`. The task commits are on the local `agent-job` branch. The fetched C++ integration branch already contains the port baseline. The fetched `origin/development` does not contain it, so integrating the lane there would also bring in the broader C++ port. Publication and integration remain pending.

## Release sweep and expanded sanitizer results

The optimized ARM64 sweep compiled all 369 production translation units and ran all 442 registered binaries. It passed 440 binaries and 5,029 of 5,032 cases, with zero compile failures or timeouts. All 405 application-level binaries and all 4,571 of their cases passed. The failures were the unchanged Bun expectation group in `tests/rt/fetch_transport_test.cpp` (32/33) and the URL/IDNA reference test (0/2) using Ubuntu's ICU 74.2. The optimized object phase took 1,413.766 seconds after reusing the completed objects from the earlier attempt. The entire invocation took 1,493.775 seconds.

The expanded Clang 21 sanitizer gate ran the 36 runtime binaries, the new library test, the two runtime HTTP and WebSocket suites, and the bounded-response-body suite. It passed 38 of 40 binaries and 483 of 486 cases. The same two existing test binaries failed on the same assertions, and there were no sanitizer findings. The invocation took 620.872 seconds. Both final receipts record matching input digests and the source commit and tree listed above.

A scratch-only diagnostic copy of the original fetch transport test kept every fixture and updated the 15 expectations that differ under strictly parsed heads and bodies. It passed all 33 cases in the GCC release build. The repository still contains the original expectation table, and the exhaustive sweep counts its failure.

A separate Bun 1.4.0 control used the repository WebSocket loader with the frozen `ws@8.18.3` install. It delivered a text frame coalesced with the valid HTTP/1.1 upgrade and rejected HTTP/1.2, an extra status-space, duplicate lengths, and conflicting framing under its node:http parser. The native WebSocket tests passed on the same source tree.

The changed-content check used `bun --no-env-file run format:check` with the frozen repository formatter in scratch. It passed for the added findings text, the deviations, provenance, and both JSON registries. The existing findings prefix was compared byte-for-byte with the inherited report. First-party `git diff --check` passed. The vendored generated C file contains upstream whitespace and matches the pinned archive.

## Failure controls and final disposition

The scratch strict-expectation control passed all 33 fetch transport cases with GCC and with Clang ASan/UBSan. Both used the original fixture inputs. The control source was generated outside the checkout and the checked-in assertions were used for the suite totals above.

The ICU mismatch resolved on ICU 78.3. The published source archive had SHA-256 `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`. It was built with `CC=gcc-14 CXX=g++-14 ./configure --prefix="$scratch/icu-prefix" --disable-tests --disable-samples --disable-extras`, followed by `make -j3` and `make install`. The control recompiled the unchanged `src/rt/url.cpp` with the new headers and relinked the original IDNA binary against the new libraries. The original IDNA suite passed 2/2 in both the optimized and sanitizer configurations. The gate totals above retain the initial run counts.

```sh
python3 -B "$scratch/expectation-control.py" release
python3 -B "$scratch/expectation-control.py" asan
python3 -B "$scratch/icu-control.py" release
python3 -B "$scratch/icu-control.py" asan
```

The release gateway entry point also linked as an ARM64 ELF executable from the 369 production objects, the startup and entrypoint objects, and the three selected vendor archives. The release and sanitizer test evidence applies to the source tree recorded above. Subsequent commits append documentation only.

The remaining work requires the separately owned fetch response-read and body-pump blocks and the Bun-era expectation table. The client lane can be handed off on its clean local branch. The GitHub write and C++ integration receipts are pending.

## Follow-up implementation and ARM64 release gate

The follow-up resumed at `93b8c4fbd3b360a8ac290cc0dc7f634b9996825e`, tree
`d44de1068c23675c3cfea5099b31af054044652a`. This tree already contains the implementation, updated fetch expectations, SSE
profile, and GET and HEAD fuzz coverage. The verification scope remains the fetch response parser, the live pump,
fetch expectations, and the library harness. All 442 registered binaries link the fetch transport through the common
runtime. [Follow-up work contract and profile commands](cppgw-std-llhttp-client-fu-20261118.md)

The exchange now feeds head fragments and body bytes into one llhttp parser. The message-complete callback and
`llhttp_finish` at orderly EOF determine delivery boundaries for fixed lengths, chunked bodies, EOF-delimited
bodies, HEAD, and bodyless statuses.
Interim responses reset the parser for the next head. The pump moves the fetched byte buffer into the stream and
discards redirect payload directly from the parser callbacks. It preserves head limits, truncation errors,
backpressure, and connection reuse after clean completion.

The inherited baseline's one failing fetch binary was `tests/rt/fetch_transport_test.cpp`, with one grouped case
covering 15 changed expectations. The other failure was `tests/rt/url_idna_test.cpp` with ICU 74.2. The fetch
assertions retain the wire cases and check status, headers, payload, and the stage, type, and code of a failure.
A valid TRACE response delivers its framed payload. Malformed lengths, conflicting framing, and nonstandard
delimiters are rejected. [Recorded Bun differences](../DEVIATIONS-FROM-BUN.md)

The three owned production files add 181 and remove 206 lines since `de4ff431` for a net reduction of 25.
The live `fetch_transport.cpp` pump accounts for 140 removed lines and 83 added lines. The standalone parser
entry points remain in use by the earlier library test and fuzz targets.

The matched GCC 14.2 short SSE benchmark measures thread CPU time per response, including the parser output
buffer and excluding sockets, TLS, compression, and stream scheduling. Four nine-byte events took **351.3 ns/op**
when coalesced and **447.9 ns/op** when fed separately. The earlier parser benchmark was 461.8 ns/op before llhttp
and 661.1 ns/op with the initial llhttp adapter. The matched control on the follow-up runner was 411.8 ns/op before
llhttp and 597.5 ns/op with the inherited adapter for coalesced input. That workload fell from nine to four
allocations per response by retaining the head parser, writing into the stream buffer, and avoiding temporary
header storage. The matched pre-llhttp fragmented control was 438.2 ns/op, about 10 ns/op faster than the final
fragmented measurement. [SSE inputs, allocation counts, and cycle samples](cppgw-std-llhttp-client-fu-20261118.md#sse-profile)

The resumed build reused the preceding attempt's Boost 1.92.0 static archives, SQLite 3.54.0, ICU 78.3, GCC 14.2,
and Clang 21.1.8. The external `gate` wrapper below executes `bash cpp/gateway/tools/ci-build.sh --config <profile>`.
Its compiler and archiver adapters build all three llhttp units as C11 because the checked-in driver creates each
vendor archive from one C++ source. The adapters also embed an ELF `DT_RPATH` pointing to the external ICU and
SQLite directories. The repository driver still selects, builds, and runs the complete registry in its Linux
network, mount, and PID namespaces with a fresh temporary directory and an empty home for each binary.

```sh
scratch=/var/tmp/cppgw-llhttp-final.cIfMo7
"$scratch/gate" release 4
"$scratch/gate" release 3
"$scratch/gate" asan 4
python3 -B cpp/gateway/tools/links.py --check
git diff --check de4ff431
```

Loading ICU through `LD_LIBRARY_PATH` passed the initial process and failed in five binaries whose test children
clear their environment. The first release invocation compiled 369/369 production objects and passed 437/442
binaries and 5,010/5,015 cases in 797.959 seconds. The five child-loading failures were
`config_differential_test`, `kv_durable_interop_test`, `local_admin_auth_test`, `rt/kv_sqlite_test`, and
`storage_bun_kv_test`. The second invocation relinked the cached objects with the embedded path and passed
**442/442 binaries and 5,032/5,032 cases** in 122.712 seconds, with no build failures or timeouts. It used the
unfiltered driver selection in a single shard. The source digest matched before and after both runs
(`c476cfb902971d23bf50fa0930adb407f03a171186a90534d1552a17adf8ac29`).

The focused Clang ASan/UBSan receipt on the same source passes 5/5 fetch-named binaries and 57/57 cases.
The full Clang run is in progress. The build integration issue remains outside this lane's owned paths.

The resumed library fuzz harness exercised both GET and HEAD with fragmented input, standalone chunked bodies,
and WebSocket response heads. Clang 21 compiled the fetch and WebSocket adapters, supporting string and async
translation units, the harness, and the three llhttp units with libFuzzer, ASan, and UBSan. The link reused the
remaining 40 sanitizer runtime objects. From the seven checked-in seeds, `bash "$scratch/fuzz-final/link-run" "$scratch"`
ran 391,994 inputs in 121 seconds, exit 0, with no sanitizer diagnostics or finding artifacts. The script ran the
binary with `-max_len=16384 -timeout=3 -rss_limit_mb=768 -max_total_time=120 -print_final_stats=1` in a private
network namespace with an empty home and fresh temporary directory. The binary's SHA-256 was
`1ca5c39015844cb27cb1bc1e580f4905779c684d333d3efbd5d5059f7e65219f`.

## Final sanitizer continuation

The continuation started at `6f37825659abe01e84f98bf4e3d3b6956f57635f`, tree
`bdbdcd15f0709a16462997d0f0715747c3aee06d`. The release graph reports `ninja: no work to do`. Recomputing the driver
input digest matched the full release and sanitizer receipts for `93b8c4fb`, each with 369 production objects
compiled and an unchanged source digest. The source and fixture inputs are identical to the prior verified tree.

The full Clang 21 ASan/UBSan run used `-O0 -gline-tables-only` after the driver's defaults. It completed in
1,677.940 seconds with 439/442 passing binaries, 4,995/4,997 passing cases, zero build failures and one timeout.
The three exceptions were `codex_supervisor_secret_test` (two 2,000 ms timing assertions), `rt/json_test`
(an ASan stack overflow in `matchesAt` during the 10,000-level JSON depth test), and
`sentinel_replay_retention_metadata_test` (300-second process timeout). All five fetch-named binaries passed,
and the continuation uses the passing receipts for the other binaries. The retry selects just those three
unchanged fixtures using the driver's `-O1` sanitizer profile and real Linux fixture boundary.

```sh
scratch=/var/tmp/cppgw-llhttp-final.cIfMo7
gate_dir="$scratch/resume-control.fWMSKkrq"
ulimit -S -s 65536
env -i "PATH=$scratch/bin:/usr/local/bin:/usr/bin:/bin" LC_ALL=C TZ=UTC \
  "HOME=$gate_dir/home" "TMPDIR=$gate_dir/tmp" \
  "PKG_CONFIG_PATH=$scratch/icu-prefix/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  "CXX=$scratch/bin/clang++-21-driver" \
  "UOS_CI_CXXFLAGS=-isystem $scratch/boost_1_92_0 --system-header-prefix=third_party/ -gline-tables-only" \
  "UOS_CI_LDFLAGS=-L$scratch/boost-stage/lib" "UOS_CI_BUILD_DIR=$gate_dir/out" \
  UOS_CI_JOBS=4 UOS_CI_TIMEOUT=420 python3 -B "$gate_dir/select.py" --config asan
```

The scratch selector used the repository driver for all 44 runtime objects, the three tests and their exact Links
closures. The sudo boundary retained its 8 MiB stack limit. The optimized retry compiled all 76 Ninja targets
and passed 76/76 cases in 203.734 seconds with zero sanitizer diagnostics. Supervisor redaction passed 41/41 cases
in 1.224 seconds, JSON passed 25/25 in 0.220 seconds, and Sentinel retention passed 10/10 in 64.366 seconds.

The release receipt, the original sanitizer receipt and the optimized retry share driver input digest
`c476cfb902971d23bf50fa0930adb407f03a171186a90534d1552a17adf8ac29`. Their receipt SHA-256 digests are
`23cadf65d2c6eced7dd4451b3e707d653a682def0b398a9be435d4aa6991f174`,
`ead139289ca13f62d0121f4b16401143fbc774fa00f73c52ae451411549c190a`, and
`65e95427c98b7693f6473a47e5f6806509112356c18f8f3cbc4aabec06ab7894`, respectively. Reconciling the 439 original
sanitizer passes and the three optimized passes gives **442/442 binaries and 5,032/5,032 cases** under ASan/UBSan.
Each binary's case count matches the complete ARM64 release pass.

The fetched `origin/development` at `70afb98dc1651a7a5851d8ee4f79ace5d4c836d1` lacks the C++ port baseline.
A pull request to that branch from this tip would carry the wider port. `git push --dry-run` to the existing
fetch follow-up branch failed because the GitHub remote requires authentication. The local task commits are
available for the C++ integration lane.

The appended Markdown passed `bun --no-env-file run format:check` with the frozen formatter in scratch.
Whole-report formatting flags the inherited opening table. The report prefix is byte-identical to the starting
commit and `git diff --check de4ff431` passed.

The follow-up removes 206 first-party production lines and adds 181, a net reduction of 25. Short SSE responses
measured 351.3 ns/op coalesced and 447.9 ns/op across four reads, against the original 461.8 ns/op pre-llhttp
benchmark. The release and reconciled sanitizer gates each cover all 442 binaries. The portable driver required
the scratch C archive adapter. Publishing the clean local branch and merging it with the C++ port remain pending.
