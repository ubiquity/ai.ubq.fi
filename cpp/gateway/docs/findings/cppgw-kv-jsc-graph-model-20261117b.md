# Sparse and cyclic stored values

## Work contract v1

Repair the C++ JavaScriptCore stored-value reader and typed-value interchange
for sparse arrays, shared nodes, and cycles. The replacement plan keeps the
existing codec because it has no standalone library implementation. This lane
owns only the value model, JSC codec, affected interchange/conversion code,
component tests, and this report. The TypeScript oracle, wire formats, committed
fixtures, deployment configuration, and production state are outside the edit
scope.

| Surface         | Required behavior                                                                          |
| --------------- | ------------------------------------------------------------------------------------------ |
| Ordinary values | Existing Bun vectors, error semantics, and codec performance                               |
| Sparse arrays   | Bun's length, own indices, and undefined behavior with allocation based on present entries |
| Object graph    | Preserve identity across JSC, typed interchange, and both offline conversion directions    |
| Offline fixture | Run the original forward/reverse proof against disposable, stopped data with pinned Deno   |

The baseline is `55afcc8b13b2ddd10747355745da19e5264f7b76`. Bun 1.4.0
deserializes `05000000012cfbff00ffffffff` to an array of length 16,775,980
with no own entries. Two existing C++ tests encode the known graph
divergences. The stored-value test already records the Bun cycle result
alongside the C++ error. The typed-value differential already records Bun's
reference identities. The repair will exercise those Bun expectations and
retain the recorded literals.

The selected gates are the strict port-flag build, original and added KV
vectors, the affected test link closures in release and ASan/UBSan, the
unchanged synthetic offline proof, and a matched codec benchmark. Build
objects, pinned dependencies, test HOME/TMPDIR, and proof data live outside
the checkout. The job closes when these gates finish, with any independent
runner or fixture blocker recorded against the exact attempted scope.

## Status

Toolchain and oracle setup started. No implementation or acceptance results yet.

## Continuation at `a705abda`

The inherited commit contains the graph model, the sparse reader and the first
13 sparse-array vectors. The strict Clang 21.1.8 release build passed the
three focused binaries (17 cases). This run included the existing 86-row
serialized corpus and the typed interchange differential. The portable runner
needed `--system-header-prefix=third_party/` for Clang to treat the vendored
simdutf header as a system header, as the sanitizer driver already does.

The continued work uses contract v1 with the same file and behavior scope.
The acceptance surface includes numeric keys in JSC's array property block.
Bun 1.4.0 accepts the valid index 4,294,967,293 in that block because its raw
index word is reserved as the block marker. The inherited reader rejects it
as a named property, and its writer would write the raw marker. The reader
and writer must handle that index while retaining the existing named-property
refusal. Bun observations used the unchanged TypeScript `encodeTypedValue`
to check length, ordered present entries, explicit undefined and a hole read.
All 13 inherited sparse vectors matched. The numeric property-block probe
adds coverage beyond those vectors.

The test closures all include `src/rt`; the release and sanitizer sweeps
therefore select the full standalone registry. The separate synthetic proof
uses the pinned Deno 2.9.7 ARM64 binary with SHA-256
`5ecc9a6b862d61d4cf8cae3ac5f29488ffc839d675cfd60c11d908121dfe5bf1`.
Both runs use isolated scratch state with explicit, provider-free environments.
The benchmark compares the original and current trees on the committed KV
fixture inputs. Compilation reuses objects per profile.

## Implementation and focused verification

- The JSC reader and typed-value decoder allocate graph nodes once. The object
  tables resolve both forward population and back-references to those nodes.
  Internal edges borrow the graph, and copies held by callers retain it.
  C++ mutable access keeps its snapshot behavior. The JSC writer counts
  anonymous objects as well as graph nodes so reference widths follow the
  wire format, including when views share a backing buffer.
- Arrays keep a bounded dense prefix and indexed present entries, with a
  separate logical length for sparse arrays. The codec, interchange,
  portability check, and value equality visit present slots. A direct indexed
  read yields undefined for a hole, while iteration and the interchange
  distinguish holes from own entries containing undefined. Named array,
  Map and Set properties still follow the existing refusal.
- Bun accepted both newly recorded property-block cases. A numeric property
  grew an array from length 4 to 4,294,967,294. With both end indices present,
  Bun returned them in ascending order. The test exercises 15 arrays from
  both JSC versions 5 and 14 through read, interchange and write. Its
  2 MiB allocation guard bounds cumulative requested heap bytes, which
  also bounds peak live heap bytes for these calls.
- The wider build identified two vector API dependencies. The catalog
  reads `KvArray::front()`, and the admin kernel writes array entries by
  mutable iteration. The sparse array implements both, so those callers
  remain byte-identical. The Bun-recorded cyclic coercion probe also confirmed
  that reading a self-referencing array as RegExp flags raises
  `RangeError: Maximum call stack size exceeded.` The interchange now keeps
  that recursion within its existing depth bound.
- The C++ test's original Bun cycle and reference expectations remain in the
  vectors. The inherited implementation changed assertions that documented the
  C++ cycle refusal and copied references. The original proof source, fixture
  and Deno helper match the baseline byte for byte.

### ARM64 toolchain and proof

Ubuntu 24.04, Clang 21.1.8, Boost 1.92.0, OpenSSL 3.0.13, SQLite 3.45.1,
ICU 74.2, zlib 1.3, Brotli 1.1.0, and Zstd 1.5.5. The official Boost tarball
SHA-256 was `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`.
Built both static archives with `./bootstrap.sh --with-libraries=json,container`
and `./b2 -j4 --with-json --with-container link=static variant=release stage`
under `/tmp/kv-graph/deps`.

The focused `-O2` check passed 19 of 19 cases in three binaries after the
writer and graph changes. The earlier proof/conversion/runtime group passed
74 of 74 cases in three binaries. The release proof invoked
`proof::importMetaMain()` with the repository's linked native KV test
binding and real pinned Deno. A loopback-free network namespace, private
mount namespace, and PID namespace isolated the proof, with the runner home
mounted over and an empty child environment. The proof produced 10 forward
rows, 11 latest-state reverse rows, and two cursor translations each way.
Deno readback passed, and the interruption published no destination or
receipt. Proof SHA-256
`0f497f8f94c260eb0e6a926275094d6c5790b38bd867e7d0a6c8cfac34f6dbaa`.

The scratch builder uses the current `tools/links.py` and portable driver's
object and test functions. For the focused run this was
`python3 -B /tmp/kv-graph/build-tests.py release --proof --jobs 2 rt/kv_jsc_sparse_test.cpp rt/kv_jsc_deserialize_test.cpp kv_conversion_typed_value_differential_test.cpp`.
The proof wrapper prepared `tests/helpers/linked_native_kv.cpp`, pointed the
resource root at copies of the original helper files, and called the original
proof main with the new fixture directory, binary path and pin. It ran via
`sudo -n unshare --net --mount --pid --fork --mount-proc --propagation private --kill-child=SIGKILL -- /bin/bash /tmp/kv-graph/proof-namespace.sh 1001 1001 release-3 release`.
All generated files and test databases were under the scratch path.

Both sweeps use `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror`,
`BOOST_JSON_NO_LIB`, `BOOST_CONTAINER_NO_LIB`, `BOOST_SYSTEM_NO_LIB`,
`BOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING`, and
`UOS_GATEWAY_NATIVE_LIBRARIES=1`. Third-party sources compile without those
warning flags. The plain and instrumented full sweeps use `-O0`; the
instrumented sweep uses `-fsanitize=address,undefined -gline-tables-only -fno-omit-frame-pointer`
and runs with LeakSanitizer enabled. Each test receives an empty home,
private temporary storage and an OS network boundary. Cold sweeps are
in progress. Ninja tracks header dependencies and recompiles affected
objects on a rerun.

### Codec benchmark

The scratch benchmark used the unchanged 86 Bun vectors in
`tests/rt/kv_jsc_deserialize_vectors.inc`. The baseline codec came from
`git archive 55afcc8b cpp/gateway`. Both binaries used Clang 21.1.8
`-O2`, the flags above and Boost 1.92.0. Each group ran a warmup and
five batches of 50,000 passes over its vectors. Reads measured the reader
on Bun bytes and consumed the value kind. Writes measured serialization
of the C++ fixture values and consumed output size. Times are the median
wall ns/op on CPU 3. Allocations come from a separate pass counting
C++ heap requests. The usage record write remained within 0.1% in this
pair. All other groups reduced elapsed time. Some small typed terminals
request more heap bytes because they keep graph identity and backing links.

| Vector group           | Rows | Read ns/op before → after | Write ns/op before → after | Read allocations/op | Write allocations/op |
| ---------------------- | ---: | ------------------------: | -------------------------: | ------------------: | -------------------: |
| Primitives             |   33 |             241.9 → 154.9 |              201.3 → 150.0 |         4.27 → 2.09 |          6.52 → 2.73 |
| Arrays                 |    5 |             385.9 → 249.6 |               144.1 → 92.4 |         9.00 → 6.00 |          6.40 → 2.40 |
| Objects, maps, sets    |    9 |             881.4 → 570.4 |              291.9 → 247.2 |       18.11 → 13.00 |         10.00 → 6.00 |
| Typed bytes            |   15 |             252.9 → 238.1 |               143.7 → 96.9 |         7.60 → 8.60 |          6.67 → 2.67 |
| Usage record           |    1 |         2,350.4 → 1,459.7 |              997.4 → 998.0 |       33.00 → 22.00 |        20.00 → 16.00 |
| Shared and enveloped   |    7 |             823.5 → 600.5 |              284.3 → 245.9 |       19.14 → 14.86 |          8.86 → 4.86 |
| Dates, regexes, errors |   16 |             716.4 → 686.9 |              451.0 → 395.0 |       11.88 → 12.19 |         10.94 → 6.94 |

The supplied 13-byte array decoded and reserialized in nine heap requests
totaling 1,000 bytes on the final codec. The read retained a length of
16,775,980, and the v14 serialization was 13 bytes.

The out-of-scope `src/rt/kv_js.cpp` projection still constructs dense JSON
output for arrays. Its cyclic JSON stringify path hits its recursion
guard and emits a RangeError, while Bun's JSON.stringify emits a TypeError.
Storage codecs and interchange avoid the dense projection.

## Continuation at `d8214f50`

The inherited implementation and tests were clean at commit
`d8214f50b1a1480785f5dee89215ea79478ef304`, tree
`6a0c25d44e3c0f015b767f9aa0b295b624d6a716`. The next gate retains
work contract v1. The 7 production files have 691 added and 472 deleted
physical lines against `55afcc8b`. The existing reader and wire format
remain in use.

```sh
python3 -B /tmp/kv-graph/build-tests.py release --jobs 4 \
  rt/kv_jsc_sparse_test.cpp rt/kv_jsc_deserialize_test.cpp \
  kv_conversion_typed_value_differential_test.cpp \
  kv_conversion_proof_differential_test.cpp kv_conversion_convert_differential_test.cpp
git diff --check 55afcc8b
```

The optimized inherited build passed all 40 cases in 5 binaries.
The Bun 1.4.0 recheck used the test's hex strings, `node:v8.deserialize`,
and the repository's TypeScript `encodeTypedValue` under an empty home and
`bun --no-env-file --no-install`. All 15 lengths, present-index lists and
hole reads matched. The allocation probe measured 6,184 to 9,171 total
requested heap bytes per complete read, interchange, write and reread.

The earlier plain sweep completed on this same source tree with 439/442
binaries and 5,023/5,029 cases. All first-party units compiled. The SQLite
case failures reproduced with the distribution's 3.45.1, and the URL/IDNA
cases failed with ICU 74.2. Focused relinks of the same tests passed 29/29
and 2/2 in both profiles with SQLite 3.54.0 and ICU 78.3. The remaining
plain failure was an elapsed-time assertion in the unrelated supervisor
redactor at `-O0`. The interrupted instrumented run and these diagnostic
runs precede the final corrected sweeps.

Fetching `development` and the C++ port branch succeeded. GitHub CLI
reported no authenticated session and `git push --dry-run` to the
existing task branch could not obtain an HTTPS username. The task
commits are preserved on `agent-job` for the branch owner.

## Continuation at `9ddb2713`

The inherited tree `b49e579950d6a39455e557e73dfaf8981db80bd2` built with
Clang 21.1.8 and the fatal port warnings. Both optimized codec binaries
passed, with 9/9 cases. This check used SQLite 3.54.0 and ICU 78.3 alongside
the pinned Boost 1.92.0 static archives. The release receipt SHA-256 is
`bbc6a4ad6663e5b396997ea9e1dc986cbdbfbc6a21cfa44ce77a62f283becb2d`.

```sh
python3 -B /tmp/kv-graph/verify.py release --jobs 3 \
  rt/kv_jsc_sparse_test.cpp rt/kv_jsc_deserialize_test.cpp
python3 -B /tmp/kv-graph/verify.py asan --jobs 4 --all
```

The sanitizer run selects all 442 standalone binaries because each links the
changed storage runtime. The scratch driver uses the repository's Links,
Ninja, vendor, and fixture-boundary helpers. It checks for source changes
after the run and records the compiler exit, per-binary outcomes, and case
totals. Test processes have an empty home and private tmpfs and run in
network, mount, and PID namespaces. Compilation progress is recorded in
`/tmp/kv-graph/verify-asan/compile-*.log`. The planned follow-up is the
optimized rerun of the plain supervisor test.

### Plain sweep closure

Clang 21.1.8 at `-O2` passed 37/37 cases in
`codex_supervisor_brief_test` and 41/41 in `codex_supervisor_secret_test`
on commit `e6262ddd3c13f628304acc0bf83aa3a05b97777f`, tree
`f9c29ea7644ac888584e994f461670d2b5f65afe`. The earlier 439 passing binaries,
the two library-version reruns, and the optimized supervisor reruns provide
passing plain evidence for all 442 binaries and 5,029 cases across runs.

```sh
nice -n 7 python3 -B /tmp/kv-graph/verify.py release --jobs 3 codex_supervisor_brief_test.cpp
python3 -B /tmp/kv-graph/verify.py release --jobs 2 codex_supervisor_secret_test.cpp
```

The plain brief and secret receipts have SHA-256
`864911e8f66225471f73e1d49b03dcffc053a9f6f3e4fb0c467d177e638ae5c1` and
`5714e841ce36c09cf85cb71186775008c64518e90dbd6adcf274efc8354e5f3d`, respectively. The source, input vectors, and fixtures
are identical to those used in the earlier plain sweep.

### Sanitizer profile checks

Clang 21.1.8 compiled all 832 translation units in the cold ASan/UBSan
run. Three binaries required the portable sanitizer profile's `-O1` setting.
The `-O0` run missed two 2-second deadlines in the supervisor secret test
and exhausted the stack in the JSON matcher and the typed-value test's
recursive C++ copy of a 2,000-level inline array. The baseline typed-value
binary passed 7/7 at `-O0` with the same runner and libraries. The deeper
stack use in the new inline value copy is a limitation of the unoptimized
sanitizer build. The normal builds pass the original fixture.

```sh
python3 -B /tmp/kv-graph/verify.py asan --optimize --jobs 3 codex_supervisor_secret_test.cpp
python3 -B /tmp/kv-graph/verify.py asan --optimize --jobs 3 kv_conversion_typed_value_differential_test.cpp
python3 -B /tmp/kv-graph/verify.py asan --optimize --jobs 3 rt/json_test.cpp
(cd /tmp/kv-graph/baseline && python3 -B /tmp/kv-graph/verify-baseline.py asan --jobs 3 kv_conversion_typed_value_differential_test.cpp)
```

The `-O1` reruns passed 41/41 supervisor, 10/10 typed-value and 25/25 JSON
cases. ASan, UBSan and leak detection were active for every run. The JSC
corpus and sparse allocation guard also passed in the cold sanitizer run,
with 9/9 cases combined. Production sources and test inputs remained
identical across the continuation commits.

### Terminal sanitizer results

The cold `-O0` run compiled in 1,776.247 seconds and finished with
438/442 passing binaries and 4,982/4,984 reported passing cases. The
native Sentinel retention fixture was the fourth exception. It hit the
300-second runner limit before emitting its case summary. The fixture
passed 10/10 cases in 64.813 seconds at `-O1`.

```sh
python3 -B /tmp/kv-graph/verify.py asan --optimize --jobs 3 --timeout 600 \
  sentinel_replay_retention_metadata_test.cpp
```

The cold run and four focused reruns together cover 442/442 binaries and
5,029/5,029 passing cases under the sanitizers. The receipts share the
source input SHA-256 `fd7c32392981195072e11050a659abd289331588331ce15b3b5f6aa806fd7ebb`.
Only the report changed between the verified trees.

| Receipt                             | SHA-256                                                            |
| ----------------------------------- | ------------------------------------------------------------------ |
| Cold sanitizer sweep                | `9b38cdadc80d35aae0dff43964ba01942d2b8ed93a304fa285e23378b9f46dfe` |
| Supervisor secret, `-O1`            | `c83f3b463d1259c4a6ba2d44751294cdf284029e46b92f398aaa6a5d973faf75` |
| Typed value, `-O1`                  | `185404f34f6a5563615e052dc7c7bb97ec34c2dcf9a03f0ab49a7529e65c3abb` |
| JSON, `-O1`                         | `7ac6745bb52ae4fa3ac732c56508267d7a6580209e7499f70cb2a2204565dbe3` |
| Sentinel retention, `-O1`           | `fc984f20be4fc45f4c76a48d413b1ee6ec4a565204dda270091fbaff59a84757` |
| Baseline typed-value control, `-O0` | `a69b718d31d702fcd7c4df4113faa97b40ef43f6cdbdf652203bf1035472bca2` |

### Handoff

The storage and interchange implementation preserves sparse lengths and
graph references in the existing wire formats. The 15 Bun array vectors,
the 2 MiB allocation guard, and the unchanged cyclic conversion proof
passed. The supplied sparse value used 1,000 bytes of C++ heap requests
for a read and write. The ordinary-value benchmarks above show faster
reads in each group, with the usage-record write within 0.1% of baseline.

`docs/PORT-NOTES.md` and `docs/rt/kv-durable.md` still describe the earlier
cycle refusal. Those documents and the dense JSON projection are outside
this lane's owned paths.

The local task branch is `agent-job`. The refreshed
`origin/development` at `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` lacks the
task commits. Publishing and integration require authenticated GitHub write access.

```sh
GIT_TERMINAL_PROMPT=0 git push origin HEAD:refs/heads/agent-jobs/cppgw-kv-jsc-graph-model-r3-44202164351-1
```

The push exited 128 because Git could not obtain a GitHub HTTPS username.
The checkout is clean and retains the task commits on `agent-job`.
