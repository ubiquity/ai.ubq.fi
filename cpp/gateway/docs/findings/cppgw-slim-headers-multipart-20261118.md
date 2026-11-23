# C++ gateway Headers and inbound multipart

## Work contract v1

Base `6393434b110e2d1afe93e6c7005848219336d9b9`, tree `89b42863ac7dd6a07610e2d15aa320efc42547af`.
Implement and commit Headers first, then the inbound upload decoder. Measure first-party production lines per brief.

| Work            | Owned paths                                                                                                                                | Behavior to preserve                                                                                                                                              |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Headers         | `src/rt/headers.cpp`, `include/uos/rt/headers.hpp`, `tests/rt/headers_test.cpp`                                                            | ASCII names and ByteString validation, sorted lookup and iteration, first wire spelling, Cookie and Set-Cookie joins, mutation during traversal                   |
| Inbound uploads | The assigned regions of `src/rt/http.cpp`, `include/uos/rt/http.hpp`, `src/images.cpp`, `src/live/calls.cpp`, and `tests/rt/http_test.cpp` | Ordered text and file parts, multipart and URL-encoded acceptance, Images limits and MIME precedence, Live 1 MiB limit and cancellation, capture and scrub timing |

Both briefs may append to `docs/DEVIATIONS-FROM-BUN.md`. Existing transport writers, HTTP framing, MIME lookup and sniffing, route and accounting policy, and files in other lanes are outside the implementation scope. Qualification uses local mocks in the driver's OS-isolated fixture environment. Deployment and credential operations are outside the task.

Run each brief's named fixtures with `bash cpp/gateway/tools/ci-build.sh --config release <test>`. Because every runtime object links into each test, run the full ARM64 release and ASan/UBSan inventories and compare any failures with the known small-helpers baseline. A cold build is expected to take tens of minutes per profile. Watch the driver logs and final receipts, retry recoverable toolchain failures, and preserve baseline failures for the integration owner.

## Headers

Each sorted field owns its lowercase name, first spelling, and combined value. Set-Cookie remains in its own ordered
vector. The rebased source already removed the diagnostic-only name table. Both value mutation methods now use the
bounded diagnostic, and validation still precedes every mutation.

- Production lines, measured with `wc -l`, fell from **297 to 246 (51 removed)**. This is four lines below the
  brief's target range. The rebased diagnostic cleanup had already removed 34 lines.
- A GCC 14 ARM64 `sizeof(uos::rt::Headers)` probe on the base and this change measured **104 to 48 bytes** before
  dynamic allocations.
- GCC 14 strict compilation of the changed implementation and test passed. The generated scenarios and wire-projection
  regressions passed **6/6** through the driver's Linux namespace helper.

## Inbound multipart

Headers commit `12c14bda0180276d54197c89dd5c7fc435392fbc`. Images and Live are the only production form readers.
The multipart step also migrates the dependent body fixtures in `tests/images_test.cpp`,
`tests/provider_images_branch_coverage_test.cpp`, and the typed-body case in `tests/rt/fetch_transport_test.cpp`.
The new decoder validates the same content-type and boundary rules. Decoded file data owns its bytes independently of
the raw buffer retained for replay or scrubbed on exit.

Images and Live use the same decoder with their existing read ceilings. Images reads scalar values by first occurrence,
preserves the order of `image` and `image[]`, and counts every `mask` part before applying the media, size, and fanout
limits. Live reads the first SDP and session values and decodes file parts with the shared UTF-8 codec. Existing replay
capture still runs only after successful decoding and before Images field validation.

The MIME table and sniffing region retained SHA-256 `71a0eb1fd521fccbe0f38ab00422fa84ebb4dc0216029c612d782e30dc5104c2`.
The decoder scans parts without allocating an intermediate
list of boundary slices. The removed production surface includes the Blob and File constructors and reads, FormData
mutation and outbound encoding, and the unused asynchronous body-form adapters.

| Production path           |    Before |     After | Net removed |
| ------------------------- | --------: | --------: | ----------: |
| `src/rt/http.cpp`         |     1,259 |       997 |         262 |
| `include/uos/rt/http.hpp` |       325 |       230 |          95 |
| `src/images.cpp`          |       566 |       575 |          −9 |
| `src/live/calls.cpp`      |       182 |       184 |          −2 |
| **Total**                 | **2,332** | **1,986** |     **346** |

The test migration keeps the 346 generated HTTP scenarios and uses raw Requests for the 91 multipart parser fixtures.
The two retired Blob/FormData cases now exercise owned file bytes after scrubbing, MIME precedence, and ordered
text/file collisions. The existing redirect diagnostic expectations remain in the generated test.

Strict GCC 14 syntax checks passed for the three changed translation units and four affected test translation units.
The repository's `bun --no-env-file --no-install run size:check` passed for all 851 TypeScript paths in its inventory.

## Runner and commands

Ubuntu 24.04.5 LTS, Linux AArch64 Neoverse-N2, four CPUs. Release uses GCC 14.2.0. The sanitizer profile uses Clang
21.1.8 with address and undefined behavior sanitizers, line-table debug info and fatal sanitizer findings.

- Installed `libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev libsqlite3-dev mold` with apt, then
  `clang-21 libclang-rt-21-dev` from the signed Noble 21 repository at `apt.llvm.org`. The loaded OpenSSL, zlib,
  Brotli and Zstd pkg-config versions are 3.0.13, 1.3, 1.1.0 and 1.5.5.
- Verified the official Boost 1.92.0 `boost-1.92.0-b2-nodocs.tar.xz` SHA-256
  `ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411`. Ran
  `CXX=g++-14 ./bootstrap.sh --with-toolset=gcc --with-libraries=json,container` and
  `./b2 --user-config=uos-user-config.jam -j3 toolset=gcc-14 variant=release link=static threading=multi --with-json --with-container stage`.
  The user config bound GCC 14 to `/usr/bin/g++-14`.
- Verified the documented SQLite 3.54.0 archive SHA-256
  `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`. Ran
  `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`,
  `make -j1`, `sudo -n make install`, and `sudo -n ldconfig`.
- Verified official ICU 78.3 `icu4c-78.3-sources.tgz` SHA-256
  `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`. Ran
  `../icu/source/configure --prefix=/opt/uos-cppgw-slim-icu --disable-samples --disable-tests --disable-extras`,
  `make -j2`, and `sudo -n make install`. Its linked RPATH keeps the ICU fixture version available to children
  that clear their environment.
- Both driver profiles run through `env -i` with an empty HOME and fresh TMPDIR. The driver runs every binary in
  private Linux network, mount, and PID namespaces with a loopback reachability check, an outside route denial,
  and fresh tmpfs-backed HOME and TMPDIR. The restricted unprivileged setup used the driver's sudo fallback and
  dropped to uid 1001 for tests.
- `python3 -B cpp/gateway/tools/links.py --check` resolved all 441 test paths, and
  `git diff --check 6393434b` passed.
- The pinned Prettier 3.9.6, installed outside the checkout with both lockfiles present, passed
  `bun --no-env-file --no-install run format:check` for this report and the appended deviation sections. The
  full deviation document reports existing formatting differences on its byte-identical base prefix.

## Qualification

Source commit `92494f961ea45cbe9e4debe64eb1b1915d79ec44`, tree `e0b695ae5b6f01995fbcd38cfd574360693d2d34`.
The driver used `UOS_CI_BUILD_DIR=/tmp/uos-cppgw-slim/build`,
`PKG_CONFIG_PATH=/opt/uos-cppgw-slim-icu/lib/pkgconfig`, and
`UOS_CI_LDFLAGS="-L/tmp/uos-cppgw-slim/boost/stage/lib -Wl,--disable-new-dtags,-rpath,/opt/uos-cppgw-slim-icu/lib"`.
Release used `CXX=g++-14` and `UOS_CI_CXXFLAGS="-isystem /tmp/uos-cppgw-slim/boost"`. Sanitizers used
`CXX=clang++-21` and added `--system-header-prefix=third_party/ -gline-tables-only -fno-sanitize-recover=all`
to the same include flag. The vendor header option keeps fatal first-party warnings enabled.

The named gates use two jobs per profile. Both profiles select all 441 binaries when the test path is omitted.
The full release gate uses two jobs and the full sanitizer gate uses four.

```bash
bash cpp/gateway/tools/ci-build.sh --config release "$test"
bash cpp/gateway/tools/ci-build.sh --config asan "$test"
bash cpp/gateway/tools/ci-build.sh --config release
bash cpp/gateway/tools/ci-build.sh --config asan
```

Both profiles reported **11/12 binaries and 151/152 cases** for the named fixtures. The Headers fixture ran
1,418 steps in 158 generated scenarios. The shared HTTP fixture reported exactly the known two malformed-redirect
wording mismatches in its 1,512-step generated test case. Both redirects threw `TypeError`, and all upload
comparisons matched.

| Test path                                        | Release cases | ASan/UBSan cases |
| ------------------------------------------------ | ------------: | ---------------: |
| `tests/rt/headers_test.cpp`                      |           6/6 |              6/6 |
| `tests/rt/fetch_transport_test.cpp`              |         33/33 |            33/33 |
| `tests/runtime_http_sse_headers_test.cpp`        |           2/2 |              2/2 |
| `tests/responses_native_headers_test.cpp`        |           9/9 |              9/9 |
| `tests/cluster_w04c1_headers_test.cpp`           |           1/1 |              1/1 |
| `tests/rt/http_test.cpp`                         |           5/6 |              5/6 |
| `tests/images_test.cpp`                          |         37/37 |            37/37 |
| `tests/live_calls_http_test.cpp`                 |         11/11 |            11/11 |
| `tests/live_handler_differential_test.cpp`       |         24/24 |            24/24 |
| `tests/provider_images_branch_coverage_test.cpp` |           7/7 |              7/7 |
| `tests/runtime_http_body_test.cpp`               |         10/10 |            10/10 |
| `tests/bounded_response_body_test.cpp`           |           6/6 |              6/6 |

Each named receipt has identical input hashes before and after. The scoped result covers Images validation and
fanout ceilings, capture and replay scrubbing, Live's 1 MiB rejection, body cancellation, and the original
loopback header writer fixtures.

### SQLite loader correction

The first full sweep of each profile reported **437/441 binaries and 5,022/5,028 cases**. The three baseline
fixtures failed, and `tests/rt/sqlite_test.cpp` ran with the distribution's 3.45.1 library. Although pkg-config
selected 3.54.0 at build time, the runner's dynamic loader cache put the distribution aliases first.

I copied the built `libsqlite3.so.3.54.0` into the runner's `/opt/uos-cppgw-slim-sqlite`, added the
`libsqlite3.so.0` and `libsqlite3.so` symlinks there, and registered that directory first via
`/etc/ld.so.conf.d/00-uos-cppgw-slim-sqlite.conf` and `sudo -n ldconfig`. The pinned shared library's SHA-256 is
`9d029200f5848720d4c3e4dad9192f7c6b5d0fc9b380ff94582f93fe1ae4f74d`. The isolated
`tests/rt/sqlite_test.cpp` gate then passed **29/29 cases** in both profiles with the same compilation flags and
source tree. Both corrected full sweeps reuse that binding and the compiled object caches.

### Full ARM64 sweeps with SQLite 3.54.0

Both unfiltered commands ran the entire inventory. Each profile compiled all 369 production files with warnings
treated as errors. The rerun wall times include cached object validation, fresh linkage and fixture execution.

| Profile             | Binaries passed | Cases passed | Build failures | Timeouts | Exit | Rerun wall time |
| ------------------- | --------------: | -----------: | -------------: | -------: | ---: | --------------: |
| GCC 14 release      |         438/441 |  5,025/5,028 |              0 |        0 |    1 |       159.691 s |
| Clang 21 ASan/UBSan |         438/441 |  5,025/5,028 |              0 |        0 |    1 |       213.908 s |

The driver recorded stable input SHA-256
`973e6455576abeca612336993efd1aa7b75715c5bbc181ca17bd534e0cfa23da` in both profiles. The receipt SHA-256
values under `/tmp/uos-cppgw-slim/qualification/pinned` are
`4a87aab4a734f577397cb181d45b000e44f54e6ec5e40388b1ff6270caedffc4` for release and
`0dc28b00bbe927600c627ac3e3a4ecb374e0bed8721622c0863d999d3b51ee70` for ASan/UBSan. The sanitizer logs had
zero findings. Cold production-object preparation took 1,128.616 seconds in release and 1,610.194 seconds in the
sanitizer profile. The drivers detected the in-progress fixture edits during cache preparation, so the qualification
uses the subsequent stable-input receipts on the committed source tree.

The same three documented baseline fixtures account for every failed case.

| Fixture                                           | Observed failure                                                                                                   |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| `tests/claude_code_usage_schema_test.cpp`         | Legacy reset date `"0"` normalizes to null.                                                                        |
| `tests/codex_external_auth_differential_test.cpp` | Three legacy string forms of `last_refresh` are rejected in one generated case.                                    |
| `tests/rt/http_test.cpp`                          | The generated test still expects the old wording for two malformed Location values. Both values raise `TypeError`. |

### Checkout handoff

The Headers and multipart changes are two commits on `agent-job`. The full sweeps qualify the implementation
commit, and the appended report changes only Markdown. The intended task ref is
`refs/heads/agent-jobs/cppgw-slim-headers-multipart-20261118`. The push exited 128 because GitHub
authentication was unavailable. The refreshed `origin/development` at
`2ac5aae69b0f0bac77cf1c6f329995cf1187616c` does not contain the task history. Publication and merge remain with
the integration owner.

### Summary

Headers removed 51 production lines and dropped from 104 to 48 bytes per object on ARM64. Inbound multipart
removed 346 production lines. After the SQLite 3.54.0 loader correction, both profiles passed 438 of 441 binaries
and 5,025 of 5,028 cases. The three failures match the documented baseline. ASan/UBSan reported zero findings.
The commits are local because GitHub authentication was unavailable.
