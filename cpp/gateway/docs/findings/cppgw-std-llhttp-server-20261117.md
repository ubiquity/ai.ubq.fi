# Server HTTP parser replacement with llhttp 9.4.4

## Contract v1

Replace row 6 of the C++ OSS replacement plan on base `093a9e05`. llhttp supplies the request-line, header and chunked-body grammar. The server retains request dispatch, raw header projection, header limits, body flow, pipelining, upgrade boundaries and the existing socket and delivery lifecycle.

| Input                                                        | Acceptance surface                                                                                                                                            |
| ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| HTTP/1.0 and HTTP/1.1 origin-form and absolute-form requests | Preserve method, target, ordered header names and values, Host requirement for HTTP/1.1 and connection behavior.                                              |
| Fixed length or chunked requests                             | Frame boundaries come from llhttp. Continue to stream body data only when the existing connection lifecycle requests it. Preserve bytes for the next request. |
| HTTP Upgrade                                                 | Preserve bytes after the head for the existing upgrade listener.                                                                                              |
| Malformed or oversized requests                              | Use llhttp strict mode and preserve the server's 400 and 431 responses where applicable. Maintain resource bounds.                                            |

Writes are confined to the assigned parser and its immediate glue, the versioned llhttp vendor drop and its three registration files, the report and deviations, and the assigned fuzz and test paths. The fetch client, HTTP session and writer, socket handoff implementation, TypeScript, hosted workflows and deployment are outside this change.

Verify the archive hash and static library registration, use the Links resolver to select every consumer of `src/runtime/http.cpp`, and run those closures with the strict ARM64 flags in release and ASan/UBSan configurations. Run the new libFuzzer harness for 120 seconds and measure old and new request parsing at `-O2` with identical inputs. Record exact case counts and remaining defects. Stop after verifying the committed parser tree and preserving the bounded task commits for the parallel C++ integration. The measured standard-parser deviations and the scope of the existing Bun-recorded fixtures require discovery.

## Continuation checkpoint

Resumed at `d0d2ed8b15725c8005671a4155abe216f727051a`, tree
`fb45fadc7f20371ed9016f47a480abb8eff6059d`. Strict Clang 21.1.8 on Linux ARM64
built the inherited closure and passed all four standalone parser cases. CMake
3.31.6 built the three-member `uos_vendor_llhttp` C archive, and its test
registry resolved all 442 binaries. The retained real-socket wire run passed
102 of 106 comparisons and four of five harness cases. The four differences
are strict parser refusals for an undelimited transfer coding, duplicate
Content-Length, an ASCII DEL in a header and raw UTF-8 in a request target.

The adapter now limits a data call to the remaining payload bytes reported by
llhttp. This preserves delivery of the payload fragment before a delimiter
error, including when both arrive in the same socket read. The parser test
covers this boundary and verifies both orders of the length/transfer-coding
conflict.

## Discovery and local gates

The active checkout has 442 registered binaries. The Links resolver finds 88
that list `src/runtime/http.cpp`, plus the standalone parser test that includes
it. The portable continuation builder compiles each source once and uses the
repository CI fixture boundary with a fresh network, mount and PID namespace,
empty home and temporary directories, and an allowlisted environment. The
release run selects all 442 closures. The sanitizer run selects the 89 server
closures. The standalone test passed 5/5 after the chunk fix.

The archive SHA-256 matched
`5600adb1498f82a9c719b8eb19310021737388e0d92362a6d47be532710d3c96`.
`sha256sum --check SHA256SUMS` and direct `cmp` against the five extracted
upstream inputs passed. The generated upstream C file contains trailing
whitespace. The first-party diff passes `git diff --check` with the verified
vendor directory excluded.

Bun 1.4.0 loopback probes used `bun --no-env-file --no-install` inside an
isolated network namespace. They confirmed all four wire differences, along
with the metadata-limit and connection decisions recorded in
[Deviations from Bun](../DEVIATIONS-FROM-BUN.md).

The CMake registration builds all three C files. The portable production
driver `cmake/ci/driver.py` and the Mac script `tools/macos-build.sh` read the
single-member `library.source` view when building archives. The legacy SDK
`tools/unit.mk` has a vendor object rule for C++ inputs only. These build
drivers are outside the assigned files. The local verifier uses
`library.sources` and the C compiler for the three llhttp inputs.

## Adapter and production line count

llhttp owns method, target, header, transfer-coding, chunk extension and
trailer syntax. Its length and pause positions determine where the next
request or upgrade bytes start. The adapter projects ordered Latin-1
`rawHeaders` and keeps the existing Host requirement, strict request-line
spacing, byte and field budgets, `Expect: 100-continue`, fixed-length body
delivery and request and upgrade dispatch. The connection still controls
when the body flows and when the response settles. A `Proxy-Connection`
token can affect llhttp's keep-alive decision. Only an actual `Connection`
header can authorize the upgrade handoff.

`git diff --numstat 093a9e05..971bd53a -- cpp/gateway/src/runtime/http.cpp`
reports 185 physical lines removed and 223 added. The removed code split and
validated request lines and header fields, recognized methods and transfer
codings, and ran a manual chunk decoding state machine. The added lines
include the callback adapter and the retained bounds and dispatch glue.
The count excludes tests, registration and vendored code.

## Benchmark

Both binaries call `ConnectionState::parseHead` and, for chunked requests,
`decodeChunked`. The baseline includes `http.cpp` from `093a9e05`. Both
link the same optimized runtime and application dependencies. Each binary
reuses a connection and a warmed input buffer, consumes the same bytes in
the indicated segment size and resets the request and response after each
operation. The chunked case has two 4,096-byte data chunks, one extension and
one trailer. Clang 21.1.8 `-O2 -g0` on ARM64 Neoverse N2, pinned to CPU 0.
Timing is median thread CPU time over seven runs of 50,000 operations after
1,000 warmups. A separate 1,000-operation run counts C++ allocations via
regular, array and aligned `operator new`.

| Request                             | Wire bytes | Bytes per segment | Before ns/op | llhttp ns/op | Before allocations/op | llhttp allocations/op |
| ----------------------------------- | ---------: | ----------------: | -----------: | -----------: | --------------------: | --------------------: |
| GET /health                         |         54 |                54 |       666.24 |       334.04 |                    13 |                     4 |
| POST /v1/responses head             |        320 |               320 |      2706.33 |       891.54 |                    50 |                    12 |
| POST /v1/responses head, fragmented |        320 |                16 |      4111.17 |      1233.41 |                    50 |                    12 |
| Chunked POST                        |       8355 |              1024 |      1646.92 |      1392.00 |                    33 |                    17 |

## Fuzz smoke check

The standalone `http_server_library` target builds with the port warning
set and takes inputs through `ConnectionState::parseHead` and
`decodeChunked` in segmented reads. Its four seeds cover head projection,
fixed length, chunks and trailers, and buffered upgrade bytes.

The 120-second single-process libFuzzer run with ASan, UBSan and leak
detection exited 0 after 549,835 inputs in 121 seconds. It reported 2,012
coverage counters, 4,606 features, a peak RSS of 393 MiB and zero artifacts.
The binary SHA-256 was
`82c45af0d083d08157eeeb6f6c0090b512e66153034fc2b94228dc6b28a50064`.
The first campaign using the stock fork-mode runner finished its workers
after 122 seconds with zero input findings. Its parent hit the runner's
documented ARM64 libFuzzer leak at exit. The separate single-process
receipt exercises the same binary with leak detection enabled.

The stock Linux CI driver's single-source C++ compile was checked with
`clang++-21 -std=c++20 -x c++ -c cpp/gateway/third_party/llhttp/llhttp.c`.
It exits 1 when the generated C assigns callback pointers. A complete
archive requires all three inputs compiled as C, as in the CMake receipt.

## Optimized closure results

On source and test commit `971bd53a2d3851cd133143335a9e62174cd5e7ee`, tree
`00b0fa90af5d78346dcd49fa2400784341b9ded5`, the strict `-O2 -g0` build
completed all 89 HTTP-linked binaries. It passed 88/89 binaries and
1,077/1,078 harness cases. All seven standalone parser cases passed. The
wire differential case passed 102/106 comparisons. The four differences
were the documented requests that strict llhttp refuses before dispatch.

A separate wire check used an include directory outside the checkout.
Its vector copy changed only the four expected responses to
`HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n<CLOSED>`.
Its SHA-256 was
`dc10ee28d823cee12974e28e0f732bec4e3c302fc343a1aa2c71e30a91a10aa6`.
The same test passed all five harness cases and all 106 wire comparisons
with that copy. The checkout's vector file is outside the listed owned
paths. The checked-in run retains the four mismatches pending scope approval.

## Reproduction commands

Commands ran from the checkout root with Clang 21.1.8 and inherited Boost
1.92.0 JSON and Container static archives and SQLite 3.54.0 under the
scratch directory. System link inputs were OpenSSL 3, zlib, Brotli, Zstd,
ICU, pthreads and libdl. The scratch verifier uses the repository's Links
resolver, compile rule, isolation and test runner. It compiles `llhttp.c`,
`api.c` and `http.c` with `clang-21 -std=c11` and archives them together.

```bash
scratch=/tmp/llhttp-server.186Y8HcO
python3 -B cpp/gateway/tools/links.py --check
(cd cpp/gateway/third_party/llhttp && sha256sum --check SHA256SUMS)
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 UOS_GATEWAY_BUILD_EVIDENCE=local-parser-job \
  cmake -S cpp/gateway -B "$scratch/cmake" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang-21 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
  -DCMAKE_BUILD_TYPE=Release -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_TESTS=ON
cmake --build "$scratch/cmake" --target uos_vendor_llhttp --parallel 2
ar t "$scratch/cmake/libuos_vendor_llhttp.a"

isolated=(env -i PATH=/home/runner/.bun/bin:/usr/local/bin:/usr/bin:/bin
          LANG=C.UTF-8 TZ=UTC PYTHONDONTWRITEBYTECODE=1)
"${isolated[@]}" python3 "$scratch/verify.py" release component --compiler clang++-21 --cc clang-21 --jobs 4
"${isolated[@]}" python3 "$scratch/verify.py" release affected --compiler clang++-21 --cc clang-21 --jobs 3
"${isolated[@]}" python3 "$scratch/verify.py" asan affected --compiler clang++-21 --cc clang-21 --jobs 3
"${isolated[@]}" python3 "$scratch/verify.py" release unaffected --compiler clang++-21 --cc clang-21 --jobs 3
"${isolated[@]}" python3 "$scratch/wire_strict.py" release
"${isolated[@]}" bash "$scratch/benchmark/build.sh"
for build in before after; do
  "${isolated[@]}" HOME="$scratch/benchmark/home" TMPDIR="$scratch/benchmark/tmp" \
    taskset -c 0 "$scratch/benchmark/$build"
done
```

First-party objects used `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DUOS_GATEWAY_NATIVE_LIBRARIES=1`.
The include paths were `cpp/gateway/include`, `cpp/gateway` and the prepared
Boost and SQLite system headers. The sanitizer run used `-O1 -gline-tables-only
-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`
with ASan leak detection and fatal UBSan errors. Links used mold with one thread.

The fuzz build used the same pinned compilers through `clang` and `clang++`
symlinks in `$scratch/fuzz-tools`. Both fuzz runs used a fresh network
namespace and dropped to the runner user before running the harness.

```bash
sudo -n /usr/bin/unshare --net --setgid "$(id -g)" --setuid "$(id -u)" -- /usr/bin/env -i \
  "PATH=$scratch/fuzz-tools:/usr/bin:/bin" LANG=C.UTF-8 TZ=UTC \
  "HOME=$scratch/fuzz-isolation/home" "TMPDIR=$scratch/fuzz-isolation/tmp" \
  "CPLUS_INCLUDE_PATH=$scratch/boost_1_92_0:$scratch/sqlite-autoconf-3540000" \
  "LIBRARY_PATH=$scratch/boost_1_92_0/stage/lib:$scratch/sqlite-autoconf-3540000" \
  /usr/bin/python3 cpp/gateway/fuzz/ci.py http_server_library --seconds 120 --out "$scratch/fuzz" --jobs 2 --compiler clang++

campaign="$scratch/fuzz-single"
sudo -n /usr/bin/unshare --net --setgid "$(id -g)" --setuid "$(id -u)" -- /usr/bin/env -i \
  PATH=/usr/lib/llvm-21/bin:/usr/bin:/bin LANG=C.UTF-8 TZ=UTC \
  "HOME=$campaign/home" "TMPDIR=$campaign/tmp" \
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /usr/bin/timeout 150 "$scratch/fuzz/uos_fuzz_http_server_library" \
  -seed=17112026 -max_len=32768 -timeout=3 -rss_limit_mb=768 -malloc_limit_mb=128 \
  -max_total_time=120 -print_final_stats=1 "-artifact_prefix=$campaign/artifacts/" "$campaign/corpus"
```

## Terminal qualification and handoff

| Gate                                     | Binaries passed / built | Harness cases passed / run |
| ---------------------------------------- | ----------------------: | -------------------------: |
| Optimized, all registered closures       |               440 / 442 |              5,028 / 5,031 |
| Optimized, HTTP-linked closures          |                 88 / 89 |              1,077 / 1,078 |
| ASan and UBSan, HTTP-linked closures     |                 88 / 89 |              1,077 / 1,078 |
| Optimized, standalone llhttp parser      |                   1 / 1 |                      7 / 7 |
| ASan and UBSan, standalone llhttp parser |                   1 / 1 |                      7 / 7 |

All 89 sanitizer run logs had zero ASan, UBSan and leak diagnostics. The
sanitized wire test with the same four scratch expectations passed all
five harness cases and all 106 wire comparisons. The command was
`${isolated[@]} python3 "$scratch/wire_strict.py" asan`. The tracked wire
test remains the sole nonpassing HTTP-linked binary in both profiles.

The remaining 353 optimized binaries passed 352/353 binaries and
3,951/3,953 cases. The URL/IDNA differential binary failed two cases,
including the U+180E host mapping and Unicode scalar table expectations
on the runner's ICU 74.2. Its link list includes neither the server parser
object nor the llhttp archive. The application, runtime and test inputs
for that binary match the pre-task tree.

The HTTP-linked runs used the commit and tree above, with source digest
`ed7a5fbfd8bc372840b9dc0366c50763adb07a7b855ea1c63904f34876a54c66`.
The other optimized run used `e11e93ed93e6f70a6281aed792945a9f8ad89a13`,
tree `2405b139d7e870bb869f283dd3fefe836f43f5f6`, with source digest
`a5cc80ef877b90af3a574be29dc95e3eb06d40257b86d67001561398e60c1560`.
The verifier checked both digests after the runs. The subsequent commits
contain only documentation. Pinned Prettier 3.9.6 passed the new Markdown,
the continuation of this report and the changed JSON files.

The local branch has the completed parser commits and the run receipts.
Full acceptance awaits the four tracked fixture updates and multi-file C
support in the non-CMake build drivers. The push dry run to
`origin/pool-partial/cppgw-std-llhttp-server-20261117-44188151110` failed
because this runner has no GitHub write credential. The work remains
committed on the local task branch.
