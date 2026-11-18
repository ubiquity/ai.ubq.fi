# Ada 4.0.0 URL replacement

## Contract v1

Replace the URL parser, serializer, IDNA conversion and form handling in OSS plan rows 4 and 5. Use the pinned
Ada release as the behavior reference. Preserve the public C++ API, linked search parameters, gateway origin and
routing validation, and the Node POSIX file URL contract in row 34. TypeScript, other C++ lanes, hosted workflows,
credentials and deployment are outside this task.

| Surface                    | Implementation and invariant                                                                                                |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| Absolute and relative URLs | Ada parses, resolves and serializes. Port constructors throw the existing typed error.                                      |
| IDNA and IP hosts          | Ada canonicalizes hosts. The adapter checks known admission gaps for malformed A-labels, Bidi and joiners.                  |
| Query strings              | Ada parses, sorts and serializes. C++ ownership keeps href updates and the live query list synchronized.                    |
| File paths                 | The Node POSIX adapter retains local-host and encoded-slash checks and literal filename handling.                           |
| Gateway callers            | The existing link closures test HTTP, redirects, authentication, provider routing and storage without live provider access. |

Writes are limited to the URL sources and tests, Ada assets and vendor registration, the URL fuzz target and
seeds, and this report and deviation entry. The baseline is
`093a9e0511e9d0deae5495dc324e53ff4dccd719`, tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`. Bun
1.4.0 is available to confirm the old behavior. The [earlier qualification](cppgw-oss-ada-url-20261117.md)
identified the expected behavior differences and the validity gaps to check.

The acceptance surface is strict plain and ASan/UBSan builds of the URL component and its consumer closures,
the original vectors with classified expectation changes, a 120-second sanitizer fuzz run and a paired
`-O2` benchmark. Build objects, probes and logs live under `/tmp/cppgw-std-ada-20261117`. Each
test gets an empty home, fresh temp directory and credential-free environment. On this four-core ARM64 runner
the complete dual-profile build can take tens of minutes. Compile each translation unit once per profile,
record per-test outcomes and retry recoverable prerequisite or toolchain failures. Stop after the gates and
report are complete, or record the exact partial counts and blockers if the runner cuts off verification.

## Initial evidence

- Ada `ada.h` and `ada.cpp` matched both task SHA-256 pins. The Apache-2.0 notice comes from the same
  release tag. See the [vendor provenance](../../third_party/ada/PROVENANCE.md).
- This Ubuntu 24.04 ARM64 runner has GCC 14.2.0 and Bun 1.4.0. Boost.JSON and Boost.Container were built
  statically from the official Boost 1.92.0 archive. SQLite 3.54.0 was built with `SQLITE_DQS=3` for
  the existing consumer fixtures.
- `origin/development` at `0c46932008125e18704fc019282935cccf2cb9df` has no `cpp/gateway`. The port
  checkout starts at `origin/claude/cpp-gateway-port`. A credential-free dry-run push to the task branch
  exited 128 because the remote requires a GitHub username. The local job branch is the handoff.

## Verification

Pending implementation and runs.

## Continuation and contract v2

The incoming job tip was `602a7840bf54cd44fb7817848d38327edbcda1a9`, tree
`4bf0d4acebb6030847dae104c6b254c727d75071`. It already contained the Ada assets, registry, URL adapter, and
fuzz target. Their production objects and both URL test binaries compiled. The unchanged Bun assertions passed
1 of 6 URL cases and 1 of 2 IDNA cases. The Ada asset hashes and all 441 Links closures resolved.

The continued adapter removes ICU from the URL component. Ada owns URL parsing, IDNA mapping and serialization,
and form-urlencoded operations. A small fail-closed guard validates special-host A-labels using Ada's ToUnicode,
rejects ContextJ characters because Ada can stop before checking later characters, and checks the rest of a
right-to-left host. Comparison with Unicode 17.0 `DerivedBidiClass.txt`, including its defaults, found 132
R/AL/AN code points Ada accepts in an LTR label. The guard rejects those code points until the pinned Ada lookup
can classify them. The corpus and scalar sweep contain the same inputs as before this change.

Out-of-tree verification uses apt.llvm.org Clang 21.1.8 and LLD, GCC 14.2 for a separate strict component compile,
Bun 1.4.0, Boost 1.92.0 JSON and Container static archives, and SQLite 3.54.0 with `SQLITE_DQS=3`.
Plain and sanitized compile-once sweeps run on four ARM64 cores. Fixture processes receive a fresh home and temp
directory, a credential-free environment and a loopback-only network namespace. A first filesystem run showed
that the runner's ext4 volume clamps extreme timestamps. The retry puts fixture scratch on tmpfs.

[Unicode 17.0 Bidi data](https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedBidiClass.txt)

## Implementation and differential results

The first-party URL source and header diff removes 1,686 lines and adds 323, a net reduction of 1,363.
Ada's URL aggregator and search parameter list replace the original parser, serializer, IDNA mapping and
form codec. The adapter handles WTF-8 input conversion, JS record enumeration, typed errors, live
`searchParams` ownership and iterators, and the public `URLRecord` view. The Node POSIX adapter uses
the runtime path resolver and Ada percent encoding and decoding. It retains file-host and encoded-slash checks
and escapes literal POSIX filename characters that Ada's `href_from_file` would strip or interpret as separators.

The final host guard also rejects the leading combining marks missing from Ada's validation tables. A comparison
with Bun and Unicode 17.0 `DerivedGeneralCategory.txt` classified all 247 additional scalar acceptances seen with
the initial ICU-free adapter as leading marks. The guard reuses the runtime's Unicode Mark predicate, while
the library handles mapping and normalization. The final scalar sweep retains 313,410 accepted observations,
rejects 144 previously accepted observations, and adds zero acceptances or rewrites. The curated corpus changes
only the 40 rows covering five special-scheme hosts with ContextJ characters. All 69,282 original authority and
setter grid observations were independently reproduced with Bun 1.4.0. Ada changes 6,997 authority grid
observations and 1,196 setter grid observations. The new corpus checks keep the same inputs and preserve
unchanged expectations. [Deviation ledger](../DEVIATIONS-FROM-BUN.md) · [Unicode 17.0 general categories](https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedGeneralCategory.txt)

## Performance

The old binary uses the original URL source and header from `093a9e05` and the runner's ICU 74.2.
Both binaries use Clang 21.1.8 `-O2` and run on ARM64 core 2. Each row asserts equal output, performs
5,000 warmup operations, and reports the median of seven 100,000-operation samples of thread CPU time.
Allocations count calls to C++ `operator new` in a separate 5,000-operation sample.

| Workload                              | Old ns/op | Ada ns/op | Old allocations/op | Ada allocations/op |
| ------------------------------------- | --------: | --------: | -----------------: | -----------------: |
| HTTPS gateway route                   |   1,051.5 |     141.3 |                 12 |                  3 |
| HTTPS route with two query parameters |   2,013.1 |     279.4 |                 26 |                  5 |
| IPv6 with port                        |     902.7 |     279.9 |                 10 |                  4 |
| IDNA `bücher.example`                 |   1,752.3 |   1,438.0 |                 16 |                 12 |
| Relative URL with base and query      |   2,064.0 |     422.0 |                 27 |                  6 |
| Small form round trip                 |     753.9 |     261.1 |                 10 |                  3 |
| Redirect form with repeated keys      |   2,348.3 |     594.1 |                 36 |                  7 |
| Two live query mutations              |   2,793.7 |     861.8 |                 39 |                 14 |

The first seven inputs are the same as in the earlier qualification. The last workload starts from the two-query
HTTPS route, sets `stream` to `false` and appends `foo=hello world` through the existing live params object.
[Earlier workload inputs](cppgw-oss-ada-url-20261117.md#performance)

## Test commands and first receipts

The paths below are external runner scratch. The direct builder uses the repository's Links registry, compiles
each translation unit once per profile, and adds vendor sources from `cmake/vendors.json`. Its strict first-party
flags are the requested warning set, `-std=c++20 -pthread -ffunction-sections -fdata-sections`, the three
`BOOST_*_NO_LIB` definitions, `BOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING`,
`UOS_GATEWAY_NATIVE_LIBRARIES=1`, and the repository and Boost 1.92 include paths. Ada, simdjson and simdutf
build without the first-party warnings. Plain objects use `-O2`. Sanitized application objects use `-O0`,
with `-O1` on Ada, URL and string, and
`-gline-tables-only -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all`.
LLD links each test; `/usr/bin/unshare --net` raises only loopback for each fixture.

```sh
work=/tmp/cppgw-std-ada-20261117
root="$PWD/cpp/gateway"
(cd "$root/third_party/ada" && sha256sum -c SHA256SUMS)
python3 -B "$root/tools/vendor_sources.py" cmake
python3 -B "$root/tools/generate-tests.py" --registry "$work/registry/tests.json" --cmake "$work/registry/tests.cmake"
python3 "$work/build-suite.py" "$root" "$work" release plan
python3 "$work/build-suite.py" "$root" "$work" san plan
bash "$work/sweep.sh" "$work" "$root" release
bash "$work/sweep.sh" "$work" "$root" san
# Paired baseline and final adapter benchmark.
bash "$work/check/bench.sh" "$work" "$root"
# Component checks with minimal runtime linkage, including Ada.
bash "$work/check/test-focused.sh" "$work" "$root" release
bash "$work/check/test-focused.sh" "$work" "$root" san
# Fuzz build with Clang 21, libFuzzer, ASan and UBSan.
bash "$work/fuzz/smoke.sh" "$work" "$root"
```

Configured CMake with `UOS_GATEWAY_BUILD_EVIDENCE=1 UOS_GATEWAY_TOOL_ROOT="$work/native"` and
`cmake -S "$root" -B "$work/cmake" -G Ninja -DCMAKE_CXX_COMPILER=clang++-21 -DCMAKE_BUILD_TYPE=Release
-DUOS_GATEWAY_TESTS=ON -DUOS_GATEWAY_FOCUS_TEST=tests/rt/url_test.cpp`. The generated 441-test registry
links `libuos_vendor_ada.a` into the URL closure and `ninja -C "$work/cmake" -j1 uos_vendor_ada` passed.
The final two-minute fuzz smoke ran 132,607 inputs in 121 seconds with no sanitizer reports and zero artifacts.
It used all five checked-in seeds, `-seed=17112026 -max_len=16384 -max_total_time=120 -timeout=3 -rss_limit_mb=768`,
an empty home and temp directory, and an isolated network namespace.

The existing production-host `fuzz/run.py` reads `common_sources` and the harness but ignores
per-target `extra_sources`. The disposable `fuzz/ci.py` honors the Ada entry and discovers the
library for the original URL target as well. The host runner is outside the owned paths.
[Host fuzz runner](../../fuzz/run.py) · [Disposable fuzz runner](../../fuzz/ci.py)
