# Boost.Beast 1.92 WebSocket qualification

## Contract v1

Qualify Beast for row 13 on `093a9e0511e9d0deae5495dc324e53ff4dccd719`, tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`. Preserve the C++ socket API, gateway wire behavior and validation boundaries. Prefer the library's RFC 6455 and RFC 7692 behavior for generic operations. Adopt each independently qualified slice if the whole stream cannot satisfy those boundaries.

| Caller                                 | Contract                                                                                                                                                                               |
| -------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Inbound HTTP Upgrade and live sideband | Authenticated upgrade, application response headers, pre-read frame bytes, ordered text and binary delivery, bounded queues, early hard message limit and route-specific failure close |
| Outbound live sideband                 | TCP/TLS with certificate checks and custom authorization headers, negotiated compression disabled, bounded messages, refusal status and close propagation                              |
| Codex supervisor and native auth       | `ws+unix` JSON-RPC, explicit compression disablement, connection and payload limits, request-response ordering                                                                         |

Writes are limited to the assigned C++ WebSocket paths and ranges, the job report, and any WebSocket test, fuzz, or dependency registration explicitly assigned to this job. The client HTTP response parser belongs to the HTTP lane. TypeScript, shared HTTP, provider and routing code, workflow files, and deployment are outside scope.

Gate the active paths with strict ARM64 compilation, the resolved WebSocket and application test closures, and ASan/UBSan. Compare release nanoseconds and C++ allocations per operation on identical inputs. Run a two-minute sanitizer fuzz campaign against public socket entry points. The spike will settle whether Beast can reject an oversized message at the frame header and let the route choose the close frame. The work ends with the qualified implementation, source and test receipts, and a clean committed job branch for the port integration.

## Discovery

- `runtime/http_upgrade.cpp` overrides a bare receiver 1009 close and sends the route's code and reason. `runtime_http_test.cpp` and `live_sideband_http_test.cpp` assert 1011 with the application reason and zero oversize message deliveries.
- The existing receiver tests the cumulative declared size before consuming the mask or payload. Beast's public stream has a message limit and a passive control callback. A header-only loopback spike will verify when its limit sends the close frame relative to completion.

## Contract v2 and spike result

Adopt Beast's isolated RFC 6455 primitives for outbound header encoding, masking in both directions, UTF-8 and close-code validation, and the handshake accept token. The existing WebSocket and HTTP lifecycle and all acceptance gates in v1 still apply. The full-stream spike is evidence for the decision below. It does not qualify the narrower production change.

A Beast 1.92 server accepted the RFC sample handshake in paired in-memory streams. With a 1,024-byte limit, a masked frame header declaring 1,025 bytes and carrying no payload triggered `message_too_big`. The close frame was already present at completion as `88 02 03 f1` (1009, empty reason), with zero control callbacks. Beast sends this close before its teardown and before the caller gets the error. A read completion cannot substitute the gateway's close frame. An incremental payload count would wait for oversized payload bytes and lose the existing header-time rejection. Preserving both obligations with the full stream would require intercepting Beast's frame bytes or changing its read implementation.

The spike also observed that Beast accepted `Sec-WebSocket-Key: ?` in a pre-parsed request. The existing strict request validation therefore remains material. Beast's stream requires a Host header on parsed server requests. The Codex Unix client currently connects without one unless configured. The HTTP response-head parser and its pre-read byte transfer remain part of the existing client handshake. See `websocket::stream::control_callback`, `impl/read.hpp`, and `impl/accept.hpp` in the pinned release. [Boost 1.92.0](https://www.boost.org/releases/1.92.0/)

## Contract v3 and continuation

The inherited source was `73d0f35f3d810ca6245e45639ca085de58a960fc`, tree `b1157e3d74454bd84e739d0ae0de0edc56784346`. The focused Clang 21.1.8 `-O2` build on Linux ARM64 passed all three WebSocket binaries and 23 cases. It used the official Boost 1.92.0 source archive with SHA-256 `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`. [Upstream archive metadata](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2.json)

Reproduced the earlier result with `websocket::stream<beast::test::stream>` and `async_read`. A masked binary header with length 1,025, the four mask bytes, and zero payload bytes caused Beast to emit exactly four bytes for close 1009 before completing with `message_too_big`. The completion reported zero payload bytes and zero control callbacks. A bounded caller buffer would detect a large message after receiving payload. A transport adapter capable of changing the automatic close would have to recognize and replace Beast's serialized frame. Keep the header-time limit and virtual close hook for the live relay and HTTP Upgrade contract. A new frame interceptor or a second production framing path would retain the implementation being evaluated.

Extend the qualified slices to client nonce and masking-key generation, received close payload decoding, server nonce validation, and subprotocol tokenization. Retain explicit checks for the nonce's encoded shape, malformed and duplicate subprotocols, and the allowed close-code range. Apply Beast's UTF-8 validation even when a legacy caller sets `skipUTF8Validation`. Preserve wire status and headers, pre-read data, connection lifetimes, configured payload limits, and the client and server option types. The existing caller matrix and path ownership still apply. All gateway callers disable compression, so there is no active deflate implementation to replace.

Run the inherited and final WebSocket closures under the strict port flags. The wider optimized and instrumented builds compile each distinct source once and run the registry under credential-free Linux fixture isolation. Check each receipt against the committed tree, report failures outside this lane, and record exact scope and count for anything the runner cannot finish. Replay the baseline and final frame and handshake workloads with the same compiler and allocator counter. A 120-second WebSocket fuzz run and a clean integration-branch handoff close this continuation.

## Contract v4 and resumed build

The inherited worktree contained an unfinished v3 patch. The focused `-O2` replay passed 3 binaries and 23 cases on that patch, after the earlier continuation had corrected its Beast iterator and socket helper regressions. This continuation qualifies those same production slices and adds a focused validation fixture under the assigned legacy test directory. The client response-head parser remains owned by the separate HTTP lane. The caller matrix in v1 still applies. Both Codex Unix transports explicitly supply `Host: localhost`. Live sideband sets a 4 MiB hard limit and sends `1011` with `realtime sideband frame exceeds payload limit` when the inbound header declares an oversized message.

Run the release registry and instrumented WebSocket and application closures, then extend sanitizer coverage as runner time permits. Use separate temporary output trees, the in-repository Linux namespace fixture boundary, Clang 21 strict flags, and the existing Boost 1.92 archives. A sweep can take several minutes on four cores. Track object and test counts in the driver logs, replay any tooling or dependency failures in the smallest affected closure, and keep unrelated source defects in this report. Stop at a clean task branch with the wire contract, validation, benchmark, fuzz and test receipts bound to committed source. Service activation and integration of other C++ port work are outside this job.

## Contract v5 and final qualification

Resumed from `1e11fb04b467bfff4e98044b5636925c6e6f6856`, tree `56d1a55b4d17e6f960e3f4ac1125727ed80f468e`, with a pending include fix in the fuzz harness. Confirmed the inherited production and test sources with Clang 21.1.8 and the release flags on Linux ARM64. All four WebSocket binaries passed all 27 cases using the pinned Boost archive and SQLite 3.54.0. The fuzz harness's failed build used `asio::use_awaitable` without its header. The pending direct include compiles under the port warning set with libFuzzer, ASan and UBSan.

The v1 caller matrix and v3 adoption boundary apply. Finish the inherited fuzz harness and qualify the existing Beast primitives on the authenticated Upgrade, live sideband and Codex Unix transport paths. The client HTTP response-head parser, TypeScript, shared tools, deployment and other components are outside this task. Measure small and extended frame headers, masked and unmasked sends, receive and the opening accept token using the v1 baseline. Remaining unknowns are the measurement results and the full registry outcomes. Reuse the previous object caches with their original compiler and flags, prioritize the direct callers under sanitizers, and record exact terminal counts or a checkpoint if the runner ends. The build and test drivers log their own progress. Investigate failures in the smallest closure, then record out-of-scope findings here. The final gate is an isolated 120-second fuzz campaign and committed evidence on the job branch.

## Contract v6 and final-tree replay

Resume `a6829f9573c755d0f50e5f5d1113af8232e7ac04`, tree `10689cd5c268686834afcb4f566420f4c0196b92`. The prior strict ARM64 build passed the four WebSocket binaries and 27 cases. The commit after that build added a direct include in the fuzz harness. The seven-binary ASan/UBSan pass on the resulting runtime covered 54 cases. The wider optimized and sanitizer compilations had been interrupted. Both resume with the original object caches and pinned Boost 1.92.0. The test inputs are the same in the resumed tree.

The in-memory Beast spike reproduced close 1009, four wire bytes, zero delivered payload bytes and zero control callbacks before the read reported `message_too_big`. The relay's configured 1011 code and reason remain a route API contract exercised by `runtime_http_test` and `live_sideband_http_test`. Finish qualification of the primitives under the v1 caller matrix. The existing HTTP response-head parser, event and socket lifecycle, and hard header-time message limit are the adapter boundary. The production paths, fixture, fuzz entries and report listed above are the entire change.

Run the portable release registry and the focused sanitizer transport closure with two compiler jobs each, then reuse completed objects for any bounded replays. The release cache has 282 build actions left for 442 tests. The sanitizer closure has 74 actions left for ten transport tests. The expected remaining build times are roughly 20 minutes and 7 minutes, respectively. The drivers record object progress and terminal case counts. Replay a failed closure or preserve a precise checkpoint if the runner ends. Compare baseline and candidate CPU time and allocations using the same offline benchmark. The stop condition is a committed decision with source-bound test, fuzz and benchmark receipts and an explicit integration status.

## Contract v7 and fresh-runner replay

The new Linux ARM64 runner started at `aac3fca985701a41f3ad99b8970c847b40087337`, tree `2f89890f542b20b0b04fdf290e84103a210784e1`. Its build cache was empty. The strict Clang 21.1.8 release build passed the four inherited WebSocket binaries and all 27 cases. The repository's Linux namespace helper proved loopback access and external-route denial before each binary ran. The v1 caller matrix and v3 adoption boundary still apply. The HTTP lane owns client response-head parsing. This unit owns the WebSocket runtime paths, its legacy fixture and fuzz target, and this report. The port integration branch owns aggregation of the parallel components.

Add a wire regression in the owned legacy fixture for a fragmented over-limit message whose peer sends the continuation header alone. The gateway must deliver zero messages and write the route's chosen code and reason before reading any further frame bytes. Recheck the pinned Beast stream's result and its configurable read surfaces. Run the full release registry and the changed transport closures under ASan/UBSan first, then extend to the full sanitizer registry. All test links include the changed runtime. Run the registered libFuzzer target for 120 seconds with ASan and UBSan. Measure the v1 baseline and candidate on identical send, receive and handshake inputs. Release and sanitizer builds may each take tens of minutes on this runner. Follow their object and test receipts, replay isolated failures, and report any remaining evidence gaps. Stop with committed wire, timing, fuzz and verification receipts and the exact integration status.

## Qualified production boundary

Boost.Beast 1.92 supplies outbound frame headers, masking and unmasking, masking-key and client-nonce generation, opening-handshake accept tokens, UTF-8 validation, and close-code and close-payload decoding. Beast's HTTP token iterator and Base64 decoder handle subprotocol tokens and nonce syntax. The adapter still checks duplicate and empty subprotocol offers, nonce length and padding, and the upper close-code bound of 4999. The dependency is already part of the pinned Boost headers. The patch deletes 203 production lines and adds 113 across five files relative to `093a9e0511e9d0deae5495dc324e53ff4dccd719`.

The new wire regression feeds a masked 512-byte binary fragment, followed by a continuation header declaring another 513 bytes under a 1,024-byte limit. The gateway sends `88 0f 03 f3` followed by `fixture limit` and delivers zero messages while the peer withholds the mask and payload. The Beast 1.92 stream waits for the mask. After those four bytes arrive, it sends `88 02 03 f1` and finishes with `message_too_big`, zero control callbacks and 512 bytes copied from the preceding fragment. A single frame declaring 1,025 bytes produced the same Beast close with zero payload bytes copied. The read-size hint reports an estimate based on read state. The control callback runs for received control frames. Neither exposes a data-frame header policy or can supply the route's close frame before Beast writes its own. The gateway retains receive framing and socket lifecycle to enforce its limit and dispatch the route-specific close. [Beast read implementation](https://github.com/boostorg/beast/blob/boost-1.92.0/include/boost/beast/websocket/impl/read.hpp)

The retained adapter assembles HTTP headers and upgrade responses, transfers bytes read past the HTTP head, owns TCP/TLS and Unix connection lifetimes, and dispatches socket events and writes. The existing HTTP layer authenticates upgrades and owns refusal status and custom response headers. Beast's complete stream also owns automatic pong dispatch. Compression is disabled on the live relay, HTTP Upgrade, and both Codex Unix paths. The pinned branch has no active permessage-deflate implementation to replace. Bun 1.4.0 with the lockfile's `ws@8.18.3` reproduced both invalid UTF-8 examples in the deviations ledger. Every original WebSocket test kept its expectation. [Bun deviations](../DEVIATIONS-FROM-BUN.md)

## ARM64 microbenchmark

Ubuntu 24.04.5 on Neoverse N2, Clang 21.1.8, libstdc++ 14 and `-O2`. The baseline uses the pre-adoption WebSocket objects at `093a9e0511e9d0deae5495dc324e53ff4dccd719`. The candidate uses the objects linked for `018eb54982fe352bf88846345233c4e99e68479a`. Both used the same inputs, compiler flags, native dependencies, and non-WebSocket runtime objects. Seven pairs alternated variant order on CPU 3. Each process warmed each operation for 4,096 calls. Time is median thread CPU nanoseconds per operation. A separate build counted glibc `malloc`, `calloc`, and nonzero `realloc` calls, including C++ allocations and OpenSSL calls. Receive tests reused one receiver and a prebuilt complete frame per workload. The accept-token row measures token computation only.

| Operation                            | Baseline ns/op | Beast ns/op | Baseline allocations/op | Beast allocations/op |
| ------------------------------------ | -------------: | ----------: | ----------------------: | -------------------: |
| Encode 64 B unmasked text            |          21.19 |       33.12 |                       1 |                    1 |
| Receive 64 B unmasked text           |         111.00 |       94.86 |                       3 |                    3 |
| Encode 64 B masked text              |          58.62 |       47.45 |                       1 |                    1 |
| Receive 64 B masked text             |         145.99 |      103.02 |                       3 |                    3 |
| Encode 2,048 B masked UTF-8 text     |       1,112.32 |      196.55 |                       1 |                    1 |
| Receive 2,048 B masked UTF-8 text    |       5,088.13 |    1,210.56 |                       3 |                    3 |
| Encode 65,536 B masked binary        |      34,698.12 |    4,799.95 |                       1 |                    1 |
| Receive 65,536 B masked binary       |      32,378.64 |    5,952.41 |                       3 |                    3 |
| Compute RFC 6455 sample accept token |         649.74 |      229.97 |                       7 |                    1 |

The 64-byte unmasked send adds 11.93 ns per frame. Masked encoding and all measured receive workloads improve. Each 64 B row ran 300,000 operations per sample, each 2,048 B row ran 70,000, each 65,536 B row ran 10,000, and each token sample ran 150,000. The benchmark source SHA-256 is `95f9aca5b82f79c68ae53c044a30afc71b3dec982a6f8491c809f369f577ebad`. The raw timing and allocation output SHA-256 is `4da0f319a86f8973a69324d135a75a6536ef1ea58087d869ffb66b7489939eef`.

## Local evidence checkpoint

The source and test tree built for the continued qualification is `018eb54982fe352bf88846345233c4e99e68479a`, tree `0df6ecb12599f5a72281540a8f2a82a1a7fd212f`. The six selected binaries were `websocket_library_test`, `websocket_lifetime_test`, `websocket_test`, `runtime_websocket_test`, `runtime_http_test`, and `runtime_http_lifetime_test`.

| Gate                         | Passing binaries | Passing cases | Failed or skipped | Wall seconds | Receipt SHA-256                                                    |
| ---------------------------- | ---------------: | ------------: | ----------------: | -----------: | ------------------------------------------------------------------ |
| Strict `-O2` release         |                6 |            49 |                 0 |       84.972 | `5b81a5422d28e196096691aaa499f1384bbc26a04fdaeccbc6bb6b16ec7a89fe` |
| Strict `-O1` ASan/UBSan/LSan |                6 |            49 |                 0 |      303.578 | `9e0b5832ff80118d4cc4c89c856782c93025efe8993648467b3beb37bb1fd60c` |

The public-entry fuzz run exited 0 after 122 seconds and 215,742 executions with zero crashes, timeouts, out-of-memory events or sanitizer findings. It started with nine committed seeds and ended with 215 files in the corpus. A separate `-fsyntax-only` check compiled the fuzz harness with the port warning flags and sanitizer defines. The full release registry and wider ASan/UBSan closure are building in separate temporary output trees.

## Commands from this runner

The `work` prefix was `/tmp/boost-websocket.9AztjaOZ`. Clang 21.1.8, its sanitizer runtimes and LLD came from the apt.llvm.org Noble 21 repository. OpenSSL 3.0.13, ICU 74.2, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5 were installed Ubuntu packages. The official Boost 1.92.0 archive matched `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`. Its static dependency archives used Clang 18 and the runner's libstdc++. The official SQLite 3.54.0 archive matched `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`. The runner used the following setup and build invocations.

```sh
work=/tmp/boost-websocket.9AztjaOZ
boost="$work/boost_1_92_0"
sqlite="$work/sqlite-autoconf-3540000"
(cd "$boost" && CXX=clang++-21 CC=clang-21 ./bootstrap.sh --with-libraries=json,container &&
  ./b2 -j2 --with-json --with-container toolset=clang cxxstd=20 variant=release link=static threading=multi stage)
(cd "$sqlite" && ./configure --prefix="$work/sqlite" --disable-shared --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3' &&
  make -j2 libsqlite3.a)

export CXX=clang++-21
export UOS_CI_CXXFLAGS="-isystem $boost -isystem $sqlite --system-header-prefix=third_party/"
export UOS_CI_LDFLAGS="-L$boost/stage/lib -L$sqlite -fuse-ld=lld-21 -Wl,--threads=1"
export UOS_CI_BUILD_DIR="$work/qualify"
export WS_CHECKOUT="$PWD"
focused="websocket_test websocket_lifetime_test websocket_library_test runtime_websocket_test runtime_http_test runtime_http_lifetime_test"
transport="$focused live_sideband_http_test codex_supervisor_test codex_native_auth_test codex_external_native_auth_test"
UOS_CI_JOBS=1 python3 -B "$work/qualify_selection.py" release $focused
UOS_CI_JOBS=2 python3 -B "$work/qualify_selection.py" asan $focused
UOS_CI_JOBS=2 python3 -B "$work/qualify_selection.py" release $transport
UOS_CI_JOBS=2 python3 -B "$work/qualify_selection.py" asan $transport
UOS_CI_JOBS=3 bash cpp/gateway/tools/ci-build.sh --config release
python3 -B cpp/gateway/tools/links.py --check
git diff --check 093a9e0511e9d0deae5495dc324e53ff4dccd719..HEAD
```

The temporary selection launcher imports the repository CI driver and selects the named test closures. The driver resolves test links and vendors, retains `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror` and the prescribed runtime defines, and runs the tests with its Linux namespace helper. Each binary starts with a fresh tmpfs, empty `HOME` and credential-free environment. The scratch launcher used this code.

```python
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(os.environ["WS_CHECKOUT"]) / "cpp/gateway/cmake/ci"))
import driver

list_tests = driver.links.all_tests
inventory = driver.cpp_inventory
names = set(sys.argv[2:])
driver.links.all_tests = lambda: [t for t in list_tests() if Path(t).stem in names]
driver.cpp_inventory = lambda directory, *, recursive=False: (
    inventory("src/rt") if directory == "src" else inventory(directory, recursive=recursive)
)
sys.argv = ["ci-build.sh", "--config", sys.argv[1]]
raise SystemExit(driver.main())
```

```sh
mkdir -p "$work/fuzz-home" "$work/fuzz-tmp"
sudo -n unshare --net --fork --kill-child=SIGKILL -- \
  setpriv --reuid "$(id -u)" --regid "$(id -g)" --init-groups --no-new-privs \
  env -i PATH=/usr/bin:/bin HOME="$work/fuzz-home" TMPDIR="$work/fuzz-tmp" LC_ALL=C.UTF-8 TZ=UTC \
  CPLUS_INCLUDE_PATH="$boost:$sqlite" LIBRARY_PATH="$boost/stage/lib:$sqlite" \
  python3 -B "$PWD/cpp/gateway/fuzz/ci.py" websocket_library --seconds 120 --out "$work/fuzz" --compiler clang++-21 --jobs 1
```

The Beast spike compiled with `clang++-21 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -isystem "$boost" "$work/beast_limit.cpp" -pthread -o "$work/beast_limit"`. It used `websocket::stream<beast::test::stream>`, `read_message_max(1024)` and `async_read` over an in-memory stream pair. The spike source SHA-256 is `7352a5f9cde9522b1132e05dbf1139caa109dfd048c928e4b72e84eeb22e2dcf`.

The benchmark extracted the v1 WebSocket files with `git archive 093a9e0511e9d0deae5495dc324e53ff4dccd719` into scratch. It compiled the four baseline WebSocket objects with the release warnings, runtime defines and `-O2`. It linked each set of WebSocket objects against the other 40 runtime objects, the two selected vendor archives and the same native libraries. The timing builds call `Sender::frame`, `Receiver::write`, and the original SHA-1/Base64 expression or `detail::handshakeAccept`. The allocation builds add `-DCOUNT_ALLOCATIONS` to interpose the three glibc allocators during measurement. Each executable ran as `taskset -c 3 env -i PATH=/usr/bin:/bin HOME="$work/bench-home" TMPDIR="$work/bench-tmp" LC_ALL=C.UTF-8 TZ=UTC "$work/benchmark-$variant-$mode"` for `old` and `new` variants and `time` and `alloc` modes.

The portable CI script is checked in with mode `100644` in this branch. The first direct invocation exited 126. Invoking it through `bash` starts the real driver and fixture probes.

## Contract v8 and qualification replay

Resumed with `018eb54982fe352bf88846345233c4e99e68479a` and the working report above. The adoption boundary and caller matrix are those in v1 and v3. The six-binary strict release replay passed 49/49 cases on this source. The ten transport binaries cover the WebSocket runtime, HTTP upgrade and lifetime, live relay, and both Codex Unix clients. Their results bind the same source and test tree.

| Gate                         | Passing binaries | Passing cases | Failed or skipped | Wall seconds | Receipt SHA-256                                                    |
| ---------------------------- | ---------------: | ------------: | ----------------: | -----------: | ------------------------------------------------------------------ |
| Strict `-O2` release         |               10 |           123 |                 0 |      349.224 | `c44237bf1ae4ee3f214c103bfa769ce32e9c9ffd2d7f13b5b6dc94a8fd12808c` |
| Strict `-O1` ASan/UBSan/LSan |               10 |           123 |                 0 |       48.117 | `c83376121b1192e3c020bef747e88fb611fa25f89f3f1d2244abd98bc1c18774` |

The sanitizer replay reused the existing object cache and compiled the remaining live relay test object. Strict release and sanitizer registries each selected 442 binaries because every test links the affected runtime. The remaining release build resumed 232 object actions. The remaining sanitizer build resumed 517. Both drivers use two workers with fresh fixture environments. The expected runtimes are about 20 minutes for release and 45–55 minutes for sanitizers. Preserve their terminal counts and reproduce any failures against the smallest pre-adoption closure. The component, strict warning, wire, fuzz and benchmark receipts above carry forward when only this report changes.

## Native ARM64 linkage

All 369 production objects were present in the strict release cache. A standalone link used the same archive and entry-point structure as CMake. `runtime/startup.cpp` and the three entry points compiled under the strict warning set. All three links exited 0 and `file` identified each result as an AArch64 ELF executable.

| Executable     | SHA-256                                                            |
| -------------- | ------------------------------------------------------------------ |
| uos-gateway    | `7763d17b65d0ae8feb7703d29667e24a70a372c36e16a1e50ae287bbe1517a96` |
| uos-kv-convert | `3496ba29ec408d833b25e02f1e7c5e4d531cb7ef410d0102fd414f5d3ccd1b19` |
| uos-kv-proof   | `08714921e36396528e0115159dc69e87697237ff6032ecf2c2db805e4b3c411a` |

```sh
gateway="$PWD/cpp/gateway"
build="$work/qualify/clang++-21-92811b799430/release"
out="$work/native-link"
flags=(-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB
  -DBOOST_SYSTEM_NO_LIB -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DUOS_GATEWAY_NATIVE_LIBRARIES=1
  -O2 -I"$gateway/include" -I"$gateway" -isystem "$boost" -isystem "$sqlite" --system-header-prefix=third_party/)
mapfile -t sources < <(find "$gateway/src" -name '*.cpp' -type f | sort)
objects=()
for source in "${sources[@]}"; do
  object="$build/obj/${source#"$gateway/"}.o"
  test -s "$object"
  objects+=("$object")
done
ar rcs "$out/libuos_gateway.a" "${objects[@]}"
clang++-21 "${flags[@]}" -c "$gateway/runtime/startup.cpp" -o "$out/startup.o"
for entry in gateway kv-convert kv-proof; do
  clang++-21 "${flags[@]}" -c "$gateway/entrypoints/$entry.cpp" -o "$out/$entry.o"
  clang++-21 "$out/$entry.o" "$out/startup.o" -Wl,--whole-archive "$out/libuos_gateway.a" -Wl,--no-whole-archive \
    "$build"/libuos_vendor_*.a -L"$boost/stage/lib" -L"$sqlite" -fuse-ld=lld-21 -Wl,--threads=1 \
    -lboost_json -lboost_container -lssl -lcrypto -lsqlite3 -lz -lbrotlidec -lzstd -licuuc -licui18n -pthread -ldl -o "$out/uos-$entry"
  file "$out/uos-$entry"
  sha256sum "$out/uos-$entry"
done
```

## Integration checkpoint

`git merge-tree --write-tree --name-only origin/claude/cpp-gateway-port HEAD` against port tip `45118332b287b7ba9534bb66ab1518f1990a9d9e` reported a single add/add conflict in `DEVIATIONS-FROM-BUN.md`. Appending the WebSocket section to the port branch's existing ledger resolved it in a temporary index, and the upstream ledger bytes before the appended section matched exactly. The preview left both branches intact. The fetched `origin/development` at `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` predates the gateway port and has no `cpp/gateway` tree. The authenticated push check for the existing task branch failed because the runner lacked GitHub credentials.

The inherited local Codex review attempt failed before it could inspect the diff. Its sandbox could not start commands under the runner's approval policy. The socket reference at `docs/rt/websocket.md` also still describes the original 8 KiB random pool and `ScopedRandomSource` override. Beast now generates nonces and masking keys in its secure ChaCha20 mode. That reference is outside this task's document ownership.

## Registry replay and dependency control

The first complete strict release sweep used the runner's ICU 74.2. All 369 production objects compiled and 441/442 binaries passed, for 5,027/5,029 cases. The sole failure was `tests/rt/url_idna_test.cpp`, with eight curated rows and 43 scalar blocks differing from its fixture. The command exited 1 in 1,111.785 seconds. Receipt SHA-256 is `5b9945033f55134052f77b78e5b12bb22dca69ed89c98158484b746865763546`.

Relinking the fixture with all four pre-adoption WebSocket objects reproduced the same two failed cases and identical test diagnostics. The URL implementation SHA-256 is `1ad477bba4c8b1a8e70eae45daddb9434af88027931b96a3cdc3af6242e41af5` in both trees. The mismatch belongs to the runner's older ICU. Recompiling the sole ICU header consumer, `src/rt/url.cpp`, against the port's ICU 78.3 and relinking the fixture passed both cases. Receipt SHA-256 is `82fd20724673c0903c919d32067666722f1f41a1ddb7133638e130626994caf9`. The full release registry is replaying with that dependency. [Native build profiles](../BUILD.md#build-profiles)

A separate release control drove `WebSocketServer::handleUpgrade` against the baseline and candidate over Unix socket pairs. It enumerated all strings up to four bytes over a 14-byte subprotocol alphabet with separators, whitespace and invalid bytes, and all single-byte substitutions in the RFC example key. Both sides made the same 47,515 decisions, accepted 1,938, and selected the same protocol for each accepted request. The decision outputs both hashed to `e674b32eba8fcd3bc4642fda7ce37d292074fa3c30e586ffe1c31e2437c6d322`.

The commands below used the existing strict caches. The baseline replay used the repository driver's `run_test` with a temporary object tree containing the four baseline WebSocket objects. The ICU replay used the same driver to link and run each test under the same namespace fixture boundary. It added `-isystem "$icu/usr/include"` only to the strict `url.cpp` compile and replaced the three ICU link flags with the extracted AArch64 static archives `libicui18n.a`, `libicuuc.a` and `libicudata.a`. All other objects, flags and expected values came from the original build. The package remained in scratch.

```sh
python3 -B "$work/replay_idna_baseline.py" "$work" "$PWD"
nice -n 7 python3 -B "$work/run_validation.py" "$work"
icu="$work/icu78"
mkdir -p "$icu"
curl --fail --location --silent --show-error --max-time 40 \
  https://ports.ubuntu.com/ubuntu-ports/pool/main/i/icu/libicu-dev_78.3-2ubuntu1_arm64.deb \
  -o "$icu/libicu-dev_78.3-2ubuntu1_arm64.deb"
printf '%s  %s\n' 73e2d33adb9d36dd1cce51577b13f67c823fd0d7de7bdea2491bc8cccd08f06a \
  "$icu/libicu-dev_78.3-2ubuntu1_arm64.deb" | sha256sum --check
dpkg-deb -x "$icu/libicu-dev_78.3-2ubuntu1_arm64.deb" "$icu"
nice -n 7 python3 -B "$work/relink_icu78.py" "$work" "$PWD" release idna 1
nice -n 7 python3 -B "$work/relink_icu78.py" "$work" "$PWD" release all 2
UOS_CI_JOBS=4 bash "$work/qualify.sh" "$work" "$PWD" asan
```

The second sanitizer command took over the same cache after the two-worker registry build stopped at 162/517 object actions. The driver retained the completed objects and scheduled 355 remaining actions on four workers. The focused 10-binary sanitizer receipt above covers the component and its direct transport callers.

## Complete release result

The strict Linux ARM64 release replay with ICU 78.3 passed all 442 binaries and 5,029 cases with zero failures, skips or timeouts. The `url.cpp` rebuild took 15.998 seconds and linking and running the registry took 385.317 seconds. The source inputs bound to commit `4d4796869314cc4e95f5911a7b558fe711f09575`, tree `6e01acc32ea66e86ac1d60c3dd6abd4e9bad91b1`, had identical pre- and post-run SHA-256 `7c87b3c86154c35dea3f45a5a8019b4c6b718a9c00e747e9cd5d79c887e68715`. The exact release receipt SHA-256 is `2a480caf8a9362b09b960c5aa596127f7834bb4519097bb8afe6567f3dcda5fd`.

All three executable links also passed with ICU 78.3. The link used the same commands as the native linkage above, with `ar rcs` replacing the single `url.cpp.o` member with the ICU 78.3 release object and the three static ICU archives in place of the system ICU flags. The `uos-gateway`, `uos-kv-convert` and `uos-kv-proof` SHA-256 digests were `1c587298d5e0a9a97086c7004e76b1956978fa1d4be6c7ae83aa02c42b1a2968`, `640c1a4724f2acc81125a1112f2fd8735f3acc434cf183e4e204cef7f69c4f07` and `b414ec3f6fdbc92409470169a92f22cb7bc8dddee541ea3ec51f400a55f2c117`, respectively.

## Complete sanitizer result

The strict `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` registry compiled all 369 production objects with Clang 21.1.8. The fixture driver set `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The system-ICU control passed 441/442 binaries and 5,027/5,029 cases. The same two URL/IDNA cases reported the only failure. The driver took 1,628.515 seconds and its receipt SHA-256 is `9f8eb27859c6f4073cb117d48da65572f9e39b588c09bc9518116997ce850590`.

The ICU 78.3 replay passed all 442 binaries and 5,029 cases with zero sanitizer findings, failures, skips or timeouts. The instrumented `url.cpp` rebuild took 8.548 seconds and the registry took 210.622 seconds to link and run. It bound commit `cc39f515c0fba7a7321f54f88081b53e9410870f`, tree `02640f31b251444d11e1f5a4005da653d187db99`, to the same unchanged source digest `7c87b3c86154c35dea3f45a5a8019b4c6b718a9c00e747e9cd5d79c887e68715` as the release receipt. The exact sanitizer receipt SHA-256 is `7658e6d14995544cb78512232d16e0d7d108feb840e4e8b55d68cad845077a70`.

```sh
cache="$work/qualify/clang++-21-69b7e4110c3c/asan"
flock --shared "$cache/run.lock" python3 -B "$work/relink_icu78.py" "$work" "$PWD" asan all 4
```

## Handoff summary

- Beast 1.92 supplies frame header output, masking, handshake key and accept primitives, and UTF-8 and close decoding. The receive framer retains header-time payload enforcement and the relay's configured close. Beast's full stream waits for the mask and sends its own 1009 before reporting the oversize error. The production patch removes 203 lines and adds 113.
- Strict Linux ARM64 release and ASan/UBSan/LSan each passed 442/442 binaries and 5,029/5,029 cases. All three native entry points linked. The public-entry fuzz campaign ran 215,742 inputs in 122 seconds with zero findings.
- Masked 64 KiB frame encoding moved from 34,698.12 to 4,799.95 ns/op with one allocation on each side. The accept token went from 649.74 to 229.97 ns/op and seven to one allocation. The 64-byte unmasked encode went from 21.19 to 33.12 ns/op with one allocation.
- `GIT_TERMINAL_PROMPT=0 git push --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-std-websocket-c1` exited 128 for missing GitHub authentication. The job commits remain on local `agent-job`. The refreshed `origin/development` at `70afb98dc1651a7a5851d8ee4f79ace5d4c836d1` does not contain the task tip. The C++ port merge preview still has the single shared ledger conflict described above.
