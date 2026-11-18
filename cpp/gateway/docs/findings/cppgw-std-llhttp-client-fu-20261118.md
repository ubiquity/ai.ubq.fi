# Fetch response framing follow-up

## Work contract v3

The starting commit is `de4ff43111082c5bf9c7cbbc5c2f8a3c219804a6`, tree
`37903b18b303babb6a2e664d27dc3786a613660c`. Carry the llhttp 9.4.4 parser from the response
head through the live body pump, bring fetch assertions into line with standard HTTP, and measure the short SSE path
against both the pre-llhttp reference and the inherited client. Preserve status, headers, body bytes, error types,
backpressure, redirect draining, connection boundaries, truncation errors and the existing head limits.

| Response                       | Completion and delivery                                                                                                    |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------- |
| Content-Length                 | Deliver only the declared bytes. Reject premature EOF and keep surplus bytes out of the body.                              |
| Chunked and SSE                | Deliver payload progressively, preserve message boundaries across fragmented input, and reject ambiguous transfer framing. |
| Close-delimited                | Feed raw payload until an orderly EOF. Network failures and timeouts still fail the body.                                  |
| HEAD, 204, 304 and interim 1xx | Honor HTTP bodyless semantics, skip interim heads, and reject an unsolicited upgrade.                                      |

Owned production changes are the fetch parser and internal types plus the live pump and its parser call. The fetch
expectation files, library fuzz adapter, deviations and findings are also in scope. The request writer, content
decoders, WebSocket, vendor/build integration, unrelated tests, providers, production and credentials are outside the
change. Fixture traffic is isolated loopback only, with an empty home and fresh temporary directory.

The inherited findings name one fetch assertion failure and a separate ICU-sensitive IDNA failure. Establish the
actual set of failures from the registered tests. The portable driver is reported to omit two vendored C sources
from the archive, so check the stock driver and keep any runner-only build adaptation in scratch. Build SQLite
3.54.0, Boost 1.92.0 and ICU 78.3 outside the checkout.

Acceptance uses `bash cpp/gateway/tools/ci-build.sh --config release` with all registered tests, the same selection
with Clang ASan/UBSan, the library fuzz harness, and the matched short SSE measurement. Because the portable driver
adds all runtime objects to every binary, the fetch link closure is the full suite. The release and sanitizer compiles
each take tens of minutes on four CPUs. Use external logs and per-test receipts, reuse immutable build objects between
iterations, and diagnose build or fixture failures against the inherited source. Stop after the gates settle or a
specific runner, build, ownership or publication limit prevents further progress. Record exact commands and counts
and keep task commits local only while remote publication is unavailable.

## Baseline and implementation

The portable inventory contains 442 binaries. The driver adds all 44 runtime objects to every resolved link list,
including `src/rt/fetch_transport_http.cpp`. A baseline run of the five paths matching `fetch` under the same
ARM64 library versions passed 4/5 binaries and 56/57 cases. The sole failing binary was
`tests/rt/fetch_transport_test.cpp`, with one grouped case covering 15 Bun-specific expectations. The earlier
full sweep's second failure was URL/IDNA with ICU 74.2. ICU 78.3 is installed for this run.

The response table now checks head or body failure, `FetchError` identity and error code. It retains all
wire inputs. The rejected inputs cover nonstandard status lines and delimiters, duplicate and conflicting lengths,
malformed numeric lengths and invalid chunk syntax. The formerly empty TRACE expectations now require the
server's valid framed body and test connection reuse after its consumption. HEAD surplus bytes still close the
connection. [Documented differences](../DEVIATIONS-FROM-BUN.md)

One parser in the exchange reads each head fragment once, then resumes into the body. The llhttp message-complete
callback marks the boundary for Content-Length, chunked and bodyless responses. Orderly socket EOF calls
`llhttp_finish`. Truncated bodies, resets and timeouts remain errors. Redirect draining discards parser spans
without allocating a payload. The transport moves uncompressed bytes from the parser's output buffer into the
stream, with one allocation per read containing payload. Existing content decoders consume the framed bytes.
The standalone framing query and decoder remain available to their legacy test and fuzz callers.

| Production path                       | Added | Removed | Net |
| ------------------------------------- | ----: | ------: | --: |
| `src/rt/fetch_transport.cpp`          |    83 |     140 | −57 |
| `src/rt/fetch_transport_http.cpp`     |    85 |      60 | +25 |
| `src/rt/fetch_transport_internal.hpp` |    13 |       6 |  +7 |
| Total since `de4ff431`                |   181 |     206 | −25 |

The first implementation passed all five focused release binaries and 57/57 cases. The performance commit is
`7de8fba90a903c541ac2067d720ba5a5f3c580be`, tree
`a4afc3eca2c4abdfafe2367cc25ede8c4b35b48f`. It reserves initial field storage once and uses Boost.Beast for
case-insensitive comparison and transfer-coding list entries. The latter preserves commas inside quoted
parameters while retaining rejection of repeated, nonfinal and parameterized `chunked` or a disagreement with llhttp's
framing. The common one-token value avoids list iteration.

## ARM64 setup and commands

Ubuntu 24.04.5 on four ARM64 CPUs, GCC 14.2.0, Clang 21.1.8 from apt.llvm.org, OpenSSL 3.0.13,
zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. Downloads and builds ran under `/tmp/cppgw-llhttp-fu.Bht8xj`.
Boost 1.92.0, SQLite 3.54.0 and ICU 78.3 matched their published archive hashes.

```sh
./bootstrap.sh --with-libraries=json,container --with-toolset=gcc
./b2 -j2 toolset=gcc-14 --with-json --with-container variant=release link=static runtime-link=shared threading=multi --stagedir="$scratch/boost-stage" stage
CC=gcc-14 ./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1 && make install
CC=gcc-14 CXX=g++-14 ./configure --prefix="$scratch/icu" --disable-tests --disable-samples --disable-extras
make -j2 && make install
```

The stock driver emits a C++ compile of `llhttp.c`. GCC 14 rejected it with invalid pointer conversions, and its
graph only lists that file for the llhttp archive. The scratch compiler wrapper dispatches that C source to the
matching C compiler. `UOS_CI_LDFLAGS` supplies an external static archive of `llhttp.c`, `api.c` and `http.c`,
compiled as C11 with the corresponding release or ASan/UBSan flags. The driver still resolves, builds and runs the
whole registry. Under Clang the vendored-header prefix option isolates upstream simdutf warnings while keeping
first-party warnings fatal. ICU and SQLite shared libraries are staged under an external `/var/tmp` directory visible
to the driver's private `/tmp` mount. The first baseline fixture launch reached the namespace but could not load ICU
from the hidden host `/tmp`. The corrected launch used the same compiled objects.

```sh
scratch=/tmp/cppgw-llhttp-fu.Bht8xj
source "$scratch/env.sh"
gate release 2 "$scratch/inherited" python3 -B "$scratch/focused.py" fetch
gate release 2 "$PWD" python3 -B "$scratch/focused.py" fetch
gate release 3 "$PWD" bash cpp/gateway/tools/ci-build.sh
gate asan 2 "$PWD" bash cpp/gateway/tools/ci-build.sh
python3 -B cpp/gateway/tools/links.py --check
git diff --check de4ff431
```

`gate` adds `--config release` or `--config asan`, invokes the requested command in the specified checkout, and sets
the dependency and build paths in a credential-free environment. The focused adapter limits the production inventory
to `src/rt` plus the tests' Link closures. The full commands invoke the repository script directly with the entire
inventory. Each executable receives a new Linux network, mount and PID namespace with loopback, no outside route,
an empty home and a fresh temporary directory.

## SSE profile

The scratch benchmark uses the actual parser entry points from `093a9e05`, `de4ff431` and the candidate. The head is
HTTP/1.1 200 with `Content-Type: text/event-stream` and `Transfer-Encoding: chunked`. Four nine-byte
`data: a\n\n` through `data: d\n\n` events are followed by the terminating chunk. The coalesced case feeds the full
body in one call. The fragmented case feeds one event per call, with the terminator in the last call. The benchmark
checks message boundaries and every payload byte before timing. It includes preparing the output `ByteChunk`
and excludes sockets, TLS, decompression and stream scheduling.

```sh
bash "$scratch/bench.sh" "$PWD"
bash "$scratch/bench-run.sh" pre-stream inherited-stream candidate candidate inherited-stream pre-stream
sudo -n perf record -q -F 1800 -e cycles:u --call-graph dwarf,1536 -o "$scratch/profile/inherited-stream.data" -- \
  unshare --net --setuid "$(id -u)" --setgid "$(id -g)" -- env -i PATH=/usr/bin:/bin LC_ALL=C \
  HOME="$scratch/profile/home" TMPDIR="$scratch/profile/tmp" taskset -c 3 "$scratch/bench/inherited-stream" sse 600000
sudo -n perf report --stdio --no-children --sort=symbol --percent-limit=1.1 -i "$scratch/profile/inherited-stream.data"
```

GCC 14.2 used `-O2 -g1 -ffunction-sections -fdata-sections` and mold with `--gc-sections`. Each process ran
500 warmup operations and nine 100,000-operation batches pinned to CPU 3. Times are the mean of two per-process
medians in thread CPU nanoseconds per operation. Allocation measurements wrap `operator new`, `malloc`, `calloc`
and `realloc` over a separate 1,000-operation batch.

| Workload                        | Pre-llhttp ns/op | Inherited llhttp ns/op | Final ns/op | Allocations/op, pre / inherited / final |
| ------------------------------- | ---------------: | ---------------------: | ----------: | --------------------------------------: |
| Head and framing setup          |            275.8 |                  293.0 |       262.2 |                               6 / 4 / 3 |
| Four SSE events, coalesced      |            411.8 |                  597.5 |   **351.3** |                               9 / 9 / 4 |
| Four SSE events, separate reads |            438.2 |                  672.0 |   **447.9** |                             10 / 10 / 7 |

The prior report measured 461.8 ns/op before llhttp and 661.1 ns/op with the inherited adapter in its parser benchmark.
The matched output-buffer benchmark on this runner measured 397.6 ns/op after carrying the parser through the
body. In cycle samples, the inherited path spent 27.2% in llhttp dispatch with two parsed heads and 13.6%
in allocation and free. After merging the parser lifetimes, 10.7% of samples were in first-party case-insensitive
comparisons and 10.8% were in allocation and free. Beast comparisons and the initial field reserve removed
another allocation and brought the final coalesced result to 351.3 ns/op. In the separate-read workload, four
llhttp continuations still cost about 10 ns/op more overall than the old parser on this runner.
