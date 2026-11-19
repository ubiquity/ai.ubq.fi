# Area B coroutine and callback lifetime audit

Job `cppgw-lifetime-b-20261118`, source `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`.
Source ref `agent-jobs/cppgw-integrate-x1-20261118-44302040764-1`.
The local job checkout starts on `agent-job` at the same commit.

## Contract v1

- Audit the server runtime, KV/SQLite/fs/process, storage, cache, kernel and worker C++ implementation and headers.
- Review escaping coroutine closures, borrowed arguments and receivers, callback teardown and cycles,
  container aliases across suspension, and the port's GCC 12 await-expression rules.
- Trace normal and exceptional completion, early aggregation failure, detached work, stop/abort,
  and owner destruction. Provider-facing paths use local mocks only. Delivery and settlement semantics remain binding for the proposed fixes.
- The writable scope is this report and `analysis/lifetime/b/`. Proposed source changes are diffs only.
- Run grep-based finders, the five requested clang-tidy checks, and focused sanitizer confirmation where cheap.
  Tests use an empty home and fresh temporary directory in a credential-free, isolated environment.
  Time-box tool setup to 20 minutes and continue manual review if a tool cannot cover a file.
- The stop condition is a verdict for every in-scope site, one checked checklist item per file,
  matching report and JSON counts, ranked diffs, and an explicit record of remaining evidence gaps.

## Checklist

97 tracked files selected from the assigned source and header paths. Header declarations
of coroutine bodies are indexed by their definitions unless the declaration itself establishes a lifetime contract.

- [x] `include/uos/runtime/http.hpp`
- [x] `include/uos/runtime/http_body.hpp`
- [x] `include/uos/runtime/http_early_sse.hpp`
- [x] `include/uos/runtime/http_tasks.hpp`
- [x] `include/uos/runtime/http_upgrade.hpp`
- [x] `include/uos/runtime/native_kv_bootstrap.hpp`
- [x] `include/uos/runtime/permissions_linux.hpp`
- [x] `include/uos/runtime/websocket.hpp`
- [x] `src/runtime/http.cpp`
- [x] `src/runtime/http_body.cpp`
- [x] `src/runtime/http_early_sse.cpp`
- [x] `src/runtime/http_tasks.cpp`
- [x] `src/runtime/http_upgrade.cpp`
- [x] `src/runtime/native_kv_bootstrap.cpp`
- [x] `src/runtime/permissions_linux.cpp`
- [x] `src/runtime/websocket.cpp`
- [x] `include/uos/rt/fs.hpp`
- [x] `include/uos/rt/kv.hpp`
- [x] `include/uos/rt/kv_js.hpp`
- [x] `include/uos/rt/kv_sqlite.hpp`
- [x] `include/uos/rt/process.hpp`
- [x] `include/uos/rt/sqlite.hpp`
- [x] `src/rt/fs.cpp`
- [x] `src/rt/kv.cpp`
- [x] `src/rt/kv_internal.hpp`
- [x] `src/rt/kv_js.cpp`
- [x] `src/rt/kv_jsc.cpp`
- [x] `src/rt/kv_jsc_deserialize.cpp`
- [x] `src/rt/kv_key.cpp`
- [x] `src/rt/kv_memory.cpp`
- [x] `src/rt/kv_sdk.cpp`
- [x] `src/rt/kv_sdk.hpp`
- [x] `src/rt/kv_sqlite.cpp`
- [x] `src/rt/kv_value.cpp`
- [x] `src/rt/process.cpp`
- [x] `src/rt/sqlite.cpp`
- [x] `src/rt/sqlite_darwin.hpp`
- [x] `include/uos/storage/key_compatibility.hpp`
- [x] `include/uos/storage/kv.hpp`
- [x] `include/uos/storage/native_addon.hpp`
- [x] `include/uos/storage/native_kv.hpp`
- [x] `include/uos/storage/sqlite_readonly.hpp`
- [x] `include/uos/storage/value_compatibility.hpp`
- [x] `src/storage/key_compatibility.cpp`
- [x] `src/storage/kv.cpp`
- [x] `src/storage/native_addon.cpp`
- [x] `src/storage/native_kv.cpp`
- [x] `src/storage/sqlite_readonly.cpp`
- [x] `src/storage/value_compatibility.cpp`
- [x] `include/uos/cache/kv_migration.hpp`
- [x] `include/uos/cache/kv_migration_base.hpp`
- [x] `include/uos/cache/kv_migration_paid_fallback.hpp`
- [x] `include/uos/cache/kv_migration_projection.hpp`
- [x] `include/uos/cache/kv_migration_usage.hpp`
- [x] `include/uos/cache/kv_migration_validation.hpp`
- [x] `include/uos/cache/prompt_analytics.hpp`
- [x] `include/uos/cache/prompt_analytics_core.hpp`
- [x] `include/uos/cache/prompt_analytics_queue.hpp`
- [x] `include/uos/cache/prompt_analytics_read.hpp`
- [x] `include/uos/cache/scope_experiment.hpp`
- [x] `include/uos/cache/scope_experiment_model.hpp`
- [x] `include/uos/cache/scope_experiment_targets.hpp`
- [x] `include/uos/cache/scope_targets.hpp`
- [x] `include/uos/cache/telemetry_gate.hpp`
- [x] `src/cache/kv_migration.cpp`
- [x] `src/cache/kv_migration_base.cpp`
- [x] `src/cache/kv_migration_paid_fallback.cpp`
- [x] `src/cache/kv_migration_projection.cpp`
- [x] `src/cache/kv_migration_usage.cpp`
- [x] `src/cache/kv_migration_validation.cpp`
- [x] `src/cache/prompt_analytics.cpp`
- [x] `src/cache/prompt_analytics_core.cpp`
- [x] `src/cache/prompt_analytics_queue.cpp`
- [x] `src/cache/prompt_analytics_read.cpp`
- [x] `src/cache/scope_experiment.cpp`
- [x] `src/cache/scope_experiment_model.cpp`
- [x] `src/cache/scope_experiment_targets.cpp`
- [x] `src/cache/scope_targets.cpp`
- [x] `src/cache/telemetry_gate.cpp`
- [x] `include/uos/kernel/attestation.hpp`
- [x] `include/uos/kernel/policy_queue.hpp`
- [x] `include/uos/kernel/quota_reservations.hpp`
- [x] `include/uos/kernel/quota_reserve.hpp`
- [x] `include/uos/kernel/quota_v2.hpp`
- [x] `include/uos/kernel/quota_write.hpp`
- [x] `include/uos/kernel/usage.hpp`
- [x] `src/kernel/attestation.cpp`
- [x] `src/kernel/policy_queue.cpp`
- [x] `src/kernel/quota_reservations.cpp`
- [x] `src/kernel/quota_reserve.cpp`
- [x] `src/kernel/quota_v2.cpp`
- [x] `src/kernel/quota_write.cpp`
- [x] `src/kernel/usage.cpp`
- [x] `include/uos/worker/entrypoint.hpp`
- [x] `include/uos/worker/transport.hpp`
- [x] `src/worker/entrypoint.cpp`
- [x] `src/worker/transport.cpp`

## Tooling and verification

The completed scope is 50 translation units and 47 headers. The grep pass produced 919
matching lines and 41 lines of multiline lambda candidates, including comments and
syntactic matches. The report covers all 443 non-line-comment `co_await` lines and all
66 clang-tidy diagnostics. A site row can group related source locations.

### Runner and dependencies

Ubuntu 24.04 ARM64, Clang and clang-tidy 21.1.8 from apt.llvm.org, Boost 1.92.0 static
JSON and Container, ICU 78.3, SQLite 3.54.0 with `SQLITE_DQS=3`, OpenSSL 3.0.13, zlib 1.3,
Brotli 1.1.0 and Zstd 1.5.5. The setup logs and archives are outside the checkout under
`/tmp/cppgw-lifetime-b-20261118`. The SQLite archive matched the CI guide's SHA-256.
[CI build guide](../CI.md#ubuntu-prerequisites)

```sh
sudo -n apt-get -o Acquire::Retries=1 -o Acquire::https::Timeout=20 update -qq
sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang-21 clang-tidy-21 ninja-build pkg-config libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux mold python3-yaml

# In the extracted Boost, SQLite and ICU source directories, respectively.
./bootstrap.sh --with-libraries=json,container --prefix=/usr/local
./b2 -j2 --with-json --with-container cxxflags=-fPIC variant=release link=static stage
sudo -n ./b2 -j2 --with-json --with-container cxxflags=-fPIC variant=release link=static install

./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
sudo -n make install
sudo -n ldconfig

# In ICU's source/ directory.
./runConfigureICU Linux --prefix=/usr/local --disable-tests --disable-samples
make -j1
sudo -n make install
sudo -n ldconfig
pkg-config --modversion openssl sqlite3 zlib icu-uc libbrotlidec libzstd
```

### Candidate selection and clang-tidy

Run from the repository root using Bash. The explicit check configuration avoids 11
unknown-check warnings that the repository-wide configuration emits on LLVM 21.
The five selected checks parsed all 50 units successfully in 88.768 s with three jobs
and a maximum process RSS of 516,148 KiB. Seven diagnostics flag capturing coroutine
closures and 59 flag reference parameters. Each is mapped to a manually reviewed
site. The other three checks emitted zero diagnostics. Native Linux preprocessing
covers the compiled branches, with fallback and Darwin paths reviewed in source.

```bash
audit=/tmp/cppgw-lifetime-b-20261118
mkdir -p "$audit"
isolated=$(mktemp -d "$audit/scan.XXXXXX")
mkdir "$isolated/home" "$isolated/tmp"
(
  cd cpp/gateway
  mapfile -t paths < <(python3 -B -c 'import json; print(*json.load(open("analysis/lifetime/b/sites.json"))["files"], sep="\n")')
  rg -n -e '\bco_(await|return|yield)\b' -e '\b(?:spawn|co_spawn|rt::(?:all|allSettled|race|any))\s*\(' \
    -e '\[this\b|\b(?:setTimeout|setInterval|addEventListener|queueMicrotask|async_\w+)\b|\.(?:then|catch_|finally_|once)\s*\(' \
    -- "${paths[@]}" > "$audit/candidates.txt"
  rg -n -U '\[[^]\n]+\][^{};\n]*\{(?:[^{}]|\{[^{}]*\}){0,800}\bco_(?:await|return|yield)\b' \
    -- "${paths[@]}" > "$audit/lambda-candidates.txt"
)
cat > "$isolated/area-b.clang-tidy" <<'YAML'
InheritParentConfig: false
Checks: '-*,cppcoreguidelines-avoid-capturing-lambda-coroutines,cppcoreguidelines-avoid-reference-coroutine-parameters,misc-coroutine-hostile-raii,bugprone-dangling-handle,bugprone-use-after-move'
WarningsAsErrors: ''
HeaderFilterRegex: '(^|/)cpp/gateway/(include|src)/'
SystemHeaders: false
CheckOptions:
  bugprone-dangling-handle.HandleClasses: 'std::basic_string_view;std::span'
  misc-coroutine-hostile-raii.RAIITypesList: 'std::lock_guard;std::scoped_lock;std::unique_lock;std::shared_lock'
YAML
mapfile -t sources < <(python3 -B -c 'import json; print(*(p for p in json.load(open("cpp/gateway/analysis/lifetime/b/sites.json"))["files"] if p.endswith(".cpp")), sep="\n")')
selection=()
for file in "${sources[@]}"; do selection+=(--source "$file"); done
env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C HOME="$isolated/home" TMPDIR="$isolated/tmp" \
  bash cpp/gateway/tools/tidy.sh --compiler clang++-21 --jobs 3 \
  --config "$isolated/area-b.clang-tidy" --build-dir "$isolated/tidy" "${selection[@]}"
```

The tidy receipt is bound to commit `c8c624a1e86f52d38d20694270c1d0eecf634e0f`,
tree `7320034f242f648c8fac04bc3b4914a633461ae9`, and original input SHA-256
`c01302fbaeaa1016b240853a5a096a717c2c109144bb7c7e850be93cb3e29b54`.
The lexical scan produced candidates for inspection. Analyzer coverage alone cannot
establish coroutine ownership on a path the analyzer does not model.

### Local sanitizer gates

The driver built all 369 production translation units with Clang ASan and UBSan.
Its fixture boundary provided isolated loopback, an empty home, a fresh tmpfs working
directory, no provider variables, and leak detection. Tests used local fixtures and mocks.

| Driver selection                 | Binaries passed | Cases passed |   Wall time |
| -------------------------------- | --------------: | -----------: | ----------: |
| `runtime_http`                   |               8 |           49 | 1,151.409 s |
| `tests/kernel_quota`             |               5 |           53 |    41.161 s |
| `tests/worker_boundary_test.cpp` |               1 |            6 |    21.829 s |
| `tests/rt/kv`                    |               4 |           85 |    68.678 s |
| `tests/runtime_websocket`        |               1 |            2 |    12.553 s |
| `tests/storage_`                 |               3 |           39 |    34.531 s |
| `--shard 1/4 tests/cache_`       |               1 |           31 |    11.876 s |
| **Total**                        |          **23** |      **265** |             |

The first run was cold and spent 1,125.515 s compiling objects. It recorded the same
commit and tree as clang-tidy. The six later runs recorded commit
`ed4e179da3036686f7f874fefdb2f6d172493fd6` and tree
`38209bd1c35325562caa183e5f3d648eb1637a96`. All seven receipts agree on driver input
SHA-256 `4487834eff106fc599066c9688b3f5231e9a6df888d521efe898af1aac6d3aa0`.
An attempted filter of `tests/native_kv` selected zero tests. The corrected
`tests/storage_` selection includes the native KV differential fixture.

```bash
audit=/tmp/cppgw-lifetime-b-20261118
isolated=$(mktemp -d "$audit/asan-env.XXXXXX")
mkdir "$isolated/home" "$isolated/tmp"
run_asan() {
  env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C HOME="$isolated/home" TMPDIR="$isolated/tmp" \
    CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$audit/asan" \
    bash cpp/gateway/tools/ci-build.sh --config asan "$@"
}
run_asan runtime_http
run_asan tests/kernel_quota
run_asan tests/worker_boundary_test.cpp
run_asan tests/rt/kv
run_asan tests/runtime_websocket
run_asan tests/storage_
run_asan --shard 1/4 tests/cache_
```

### Scratch-only witnesses

- **D02.** Start `serveHttp` on port `-1` with a handler owning a shared marker.
  Await the rejected `ready` and `finished` receipts, discard the server and check
  the marker's weak handle. LeakSanitizer reported **2,494 bytes in 21 allocations**
  with a `ServeState` allocation in the stack. The probe ran in 0.465 s and exited through SIGTRAP.
- **D09.** With fake time `1,800,000,000,000` and injected memory KV, admit one org request,
  copy its abort signal and drop both terminal callbacks. Advance 360,000 ms.
  The reservation's lease advanced from `1800000300000` to `1800000600000` and its
  signal remained live. The functional witness returned its defect status 1 in 0.116 s.
- **D12.** Extend a scratch copy of the existing worker fixture with an owned marker
  in a mock upstream stream's pull callback, a settled semantic terminal, and
  `highWaterMark = 0`. Discard the returned body before reading, clear the override,
  drain registered background work and test the marker's weak handle. The assertion
  failed and LeakSanitizer reported **9,260 bytes in 74 allocations**. The probe
  ran in 0.666 s and exited through SIGTRAP.

The probes use the production object cache and the fixture named on each command for its
link closure. Their sources and logs live in runner scratch. The D01 conclusion comes
from the Asio buffer lifetime contract and source interleaving. An actual access to the
released storage depends on the I/O backend.

```bash
build=$(find "$audit/asan" -name receipt.json -not -path '*/history/*' -print -quit)
isolated=$(mktemp -d "$audit/probe-env.XXXXXX")
mkdir "$isolated/home" "$isolated/tmp"
run_probe() {
  env -i PATH=/usr/lib/llvm-21/bin:/usr/local/bin:/usr/bin:/bin LC_ALL=C HOME="$isolated/home" TMPDIR="$isolated/tmp" \
    python3 -B cpp/gateway/analysis/lifetime/b/probe.py "${build%/*}" "$audit/$1.cpp" "$2"
}
run_probe failed_bind tests/runtime_http_lifetime_test.cpp
run_probe abandoned_quota tests/kernel_quota_reserve_differential_test.cpp
run_probe abandoned_worker_body tests/worker_boundary_test.cpp
```

### Remaining evidence and publication

The unresolved sites need evidence for standalone node-http request timeout delivery,
active listener abandonment, SQLite worker completion after loop destruction, and quota
lock abandonment. The comparison-rule findings are based on source. GCC 12 execution,
alternate I/O backend coverage for D01 and the full hosted suites remain open.

The earlier audit's admin closure regression was fixed before this source revision. Its
fetch-transport exception escape finding remains outside area B and is specific to allocation
failure during cleanup. Adjacent implementations were followed only to determine ownership of
callbacks entered from area B. [Prior clang-tidy audit](cppgw-clang-tidy-audit-20261117.md)

The assigned integration base is outside `origin/development` at
`a3a96bc5fe96ba383936746fae23e64daa69562f`, whose tree lacks `cpp/gateway`.
The branch push preflight exited 128 because GitHub HTTPS authentication was unavailable.
The audit is committed on `agent-job` for integration with its assigned source branch.
Two post-review verification intervals exceeded 15 minutes after all file verdicts
had been committed.

```bash
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
git merge-base --is-ancestor 6d28e858e7ab8b8b1036db4a4adfdade0465ed40 origin/development
GIT_TERMINAL_PROMPT=0 git -c core.askPass=/bin/false push --dry-run origin \
  HEAD:refs/heads/agent-jobs/cppgw-lifetime-b-20261118
```

### Report and index gates

Python syntax, checklist completeness, source-locator bounds, ranked defect references,
report/index equivalence, and whitespace checks passed. Pinned Bun 1.4.0 ran the
repository formatter against the two changed artifacts in scratch with the frozen tool
dependencies. The checkout contains four audit files relative to the assigned base.

```bash
python3 -B cpp/gateway/analysis/lifetime/b/ledger.py complete
python3 -B -c 'from pathlib import Path; [compile(p.read_text(), str(p), "exec") for p in Path("cpp/gateway/analysis/lifetime/b").glob("*.py")]'
git diff --check 6d28e858e7ab8b8b1036db4a4adfdade0465ed40 HEAD
git diff --exit-code 6d28e858e7ab8b8b1036db4a4adfdade0465ed40 HEAD -- cpp/gateway/src cpp/gateway/include cpp/gateway/tests
# In scratch with copies of the changed artifacts and repository tooling.
bun --no-env-file --no-install run format:check cpp/gateway/docs/findings/cppgw-lifetime-b-20261118.md cpp/gateway/analysis/lifetime/b/sites.json
```

## File verdicts

<!-- prettier-ignore-start -->

### `include/uos/runtime/http.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 57-71,94-106,138-152,198-211 | Event registration and dispatch interfaces | **SAFE** | Registration copies the listener handle and emitter dispatch snapshots its registrations. Receiver keep-alive and listeners' capture lifetimes are classified in the implementation. |
| 101-106,127-129,145-152,204-211,282-287 | Borrowed node-http member accessors | **SAFE** | In-scope callers use these member references synchronously while an owning Socket, IncomingMessage, ServerResponse or Server remains live. The WebSocket handoff moves the stream synchronously on the supplied production options. |
| 224-238,254-265 | ServerListener and RequestListener | **SAFE** | An event argument reference is only used in the synchronous wrapper, which copies the request, socket and head into the upgrade task. The server stores the callback by value. |
| 338-368 | ServeHandler, ServeHttpOptions and HttpServer | **SAFE** | Handler arguments are owning values and the handler closure resides in ServeState, kept by request frames. Returned handle closures own ServeState. Listener and server back-edges are reviewed at their registration sites. |

### `include/uos/runtime/http_body.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 27-38,47-51 | IncomingListener identity and invocation | **SAFE** | Listeners use shared callable identity and receive an event reference only for synchronous invocation. The concrete emitter's snapshot retains the active functor if a callback removes its registration. |
| 58-77 | IncomingBody handles | **SAFE** | The wrapper owns both its stream and body state. The implementation weakens the controller edge after stream construction and registers weak listener targets. |

### `include/uos/runtime/http_early_sse.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 12-24 | Early SSE registration API | **SAFE** | The callback is passed as an owning function and invoked synchronously with the headers reference. The registry and captured request lifetime are reviewed at the actual registration. |

### `include/uos/runtime/http_tasks.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 8-11 | Request task receipt API | **SAFE** | Receipts and tasks are copied shared-state handles. An owning drain callback is returned from the implementation. |

### `include/uos/runtime/http_upgrade.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 31-36,44-64 | Upgrade callback interfaces | **SAFE** | Handler and upgrade Task inputs are by value, and returned function wrappers are synchronous bridges into owning free coroutines. Socket-close ownership is reviewed at the registration site. |

### `include/uos/runtime/native_kv_bootstrap.hpp`

The path and receipt queries take references for synchronous calls. The asynchronous bootstrap entry takes an owning optional string. No inline coroutine or callback registration.

### `include/uos/runtime/permissions_linux.hpp`

Synchronous permission contract and owned value types. No coroutine, deferred capture or callback registration.

### `include/uos/runtime/websocket.hpp`

Declaration of the memoized module promise. Constructor call and cache lifetime are reviewed with the implementation.

### `src/runtime/http.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 35-74,1151-1177,1273-1277,1393-1396,1578-1582 | Emitter and server listener dispatch | **SAFE** | Dispatch snapshots registrations and weak or strong target handles are acquired by the callback. A reentrant removal leaves the callable alive through the copied registration. |
| 323-453,621-695,739-773 | llhttp callbacks and parser views | **SAFE** | The parser is immovable inside ConnectionState, runs llhttp callbacks synchronously, uses offsets across reads, and copies target, headers and payload before erasing or growing input. |
| 514-559 | ConnectionState::startRead | **SAFE** | The async completion owns ConnectionState and its fixed readBuffer. Destruction closes the stream but the queued completion still owns the storage. |
| 689-733,796-807,987-1031,1133-1143,1236-1255 | Connection, socket and request links on ordinary completion | **SAFE** | Server map owns attached connections, which own socket and active HTTP objects. Normal close or ws handoff drops the return edges. Finish or dump releases the current request and response. |
| 756-759,783-787,809-833 | Body event and queued resume/end | **SAFE** | Event dispatch or the queued job holds a local IncomingMessage and ConnectionState. Queued work checks that the request still matches before operating. |
| 850-889,987-1000 | Outstanding HTTP write buffer | **DEFECT** ([D01](#d01)) | flush gives Asio a view into output.front() and awaits its callback. An intervening close destroys that vector with output.clear() before the completion runs, violating Asio's buffer lifetime requirement. |
| 892-920,928-962,1001-1031 | Posted drain, finish and close callbacks | **SAFE** | Each dispatch holds an owning connection or response snapshot. After reentrant finish it checks that the same response is current before resetting members. |
| 1036-1090 | Connection timeout | **SAFE** | The timer captures a weak ConnectionState, locks it before onTimer and is cleared on destruction or handoff. |
| 1114-1117,979-983 | Standalone node-http requestTimeout refusal | **UNCLEAR** | destroyAfter drops the outgoing response before close, so a listener waiting specifically for its close may retain state. serveHttp sets requestTimeout to zero. Trace any separately shipped callers that enable it and expect a delivery receipt. |
| 1180-1195,1203-1233,1325-1349 | Server request dispatch, accept and retry on ordinary shutdown | **SAFE** | Deferred jobs own Server. Accept and retry stop once listening is false, and close cancels the retry and clears the listening self-reference and registrations. |
| 1203-1215,1279-1320,1935-1944,1968-1985 | ServeState ownership after bind rejection | **DEFECT** ([D02](#d02)) | A listen error in the early-return branches only rejects ready and finished. There is no close to clear the server-owned strong request/upgrade listeners, and shutdown also exits at the rejected ready await. |
| 1328,1968-1979 | Active listener ownership on external loop destruction | **UNCLEAR** | Explicit close clears the Server self-owner and ServeState listener cycles. Establish whether a supported entry point can abandon a listening HTTP server in a reusable process without running close. |
| 1397-1400,1416-1420 | Socket handoff and message receiver | **SAFE** | The queued handoff check owns ConnectionState. The upgrade path holds the socket through synchronous ws handoff. pause and resume use a raw request identity only for a synchronous comparison. |
| 1485-1502,1505-1567 | Response write and header views | **SAFE** | Header and byte inputs are copied into owned fields or output before these synchronous calls return. The later lifetime of the output front is covered by D01. |
| 1620-1643,1656-1667,1890-1893 | Request body registration and disposal | **SAFE** | The request handler owns the Request and explicitly erases its entry after delivery. Body access is synchronous and the body value does not hold the Request key. |
| 1687-1706,1722-1729 | Early SSE stop listeners | **SAFE** | The listeners weakly lock Readiness, which weakly locks the response. A local or registry handle keeps Readiness alive during a callback. |
| 1707-1721,1861-1872,1888-1896 | Early SSE registry key retained by its own value | **DEFECT** ([D04](#d04)) | The static registry stores a commit callable owning its Request key. If the handler frame is abandoned before the explicit unregister after delivery, the key cannot expire and the registry retains its body and signal indefinitely. |
| 1747-1778,1813-1816 | DrainWait callback self-ownership | **DEFECT** ([D03](#d03)) | DrainWait stores four listeners that each strongly own DrainWait. A frame abandoned at the drain await can drop without any of those events firing, so the self-cycle still retains the response, connection and signal. |
| 1783-1795 | Detached body cancellation | **SAFE** | ByteStream::cancel synchronously builds an owning cancellation task and the coroutine copies Request, Response, outgoing and signal. Cancellation does not borrow the local optional stream handle. |
| 1795-1801,1807-1823 | Stream reader and abort callback on ordinary completion | **SAFE** | The reader task owns its reader slot, the named chunk survives a directly awaited drain, and normal failure or abort removes the listener. Forced-teardown retention of the abort edge is covered separately. |
| 1796-1801,1821 | Response abort listener self-ownership | **DEFECT** ([D05](#d05)) | The abort signal owns a listener that captures that same signal and a reader. If the send coroutine is destroyed during a pending read without dispatching abort, the post-await remove never runs and the listener pins both. |
| 1846-1854,1986-1990 | Active tasks and finished aggregation | **SAFE** | Tasks own their child frames. Settlement listeners weakly acquire ServeState. Shutdown takes a promise snapshot and its detached continuation copies only the finished resolver. |
| 1858-1873,1888-1898 | HTTP request coroutine and normal receipt/drain | **SAFE** | The request frame owns ServeState and the handler closure until its Promise settles. It retains the request, real delivery receipt, and drain callable across their waits, with no alias into the active task map. |
| 1901-1933 | Request finish, close and error listeners | **SAFE** | Listeners own the controller, resolver and one shared settlement bit. The raw response pointer is stored on that same response and both emitting paths keep the response alive in a local owning snapshot. |
| 1935-1944,2022-2039 | Shutdown coroutine and lingering close timer | **SAFE** | Shutdown takes state and close callback by value. The timer holds the callable for each scheduled invocation and its self-reference and state reference are weak. |
| 1992-2015,2040-2062 | Listen callback, abort cleanup and public handles | **SAFE** | Listening and terminal callbacks weakly lock state. The finished reaction removes the external abort registration and clears the state-held listener. Returned handles own state for their use. |
| 1964-1966 | Startup rejection observers | **SAFE** | Both handlers are captureless and promise-owned through settlement. Neither reaction holds ServeState or the server. |

### `src/runtime/http_body.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 32-69,91-118 | Body event and abort handlers | **SAFE** | Registered listeners use weak state and lock it for dispatch. The raw self callbacks are synchronous members of that same live state and are never independently registered. |
| 132-150 | Request body source algorithms and controller | **SAFE** | Source callbacks own body state. The state drops its strong controller after synchronous start and then reaches the stream weakly. EOF, cancel and dispose remove registrations. |

### `src/runtime/http_early_sse.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 14-17 | Early SSE registration cleanup at HTTP caller | **DEFECT** ([D04](#d04)) | Registration stores an owner of its callable in the weak-map value and returns an ordinary callable for deletion. The HTTP caller gives the stored value a strong edge back to the key and drops the explicit cleanup on frame abandonment. |
| 19-33 | Synchronous early SSE lookup and commit | **SAFE** | Commit takes a shared copy of Entry before invoking a possibly reentrant callback. The query that only reads the bit uses its map pointer synchronously. |

### `src/runtime/http_tasks.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 17-24,42-45 | Request drain and settlement aggregation | **SAFE** | The synchronous wrapper copies task set and receipt into the child frame. Each iteration snapshots promise values before allSettled. No iterator or entry reference survives the wait. |
| 27-39 | Settlement removal | **SAFE** | The map lookup is copied before use. The reaction owns the promise identity token and weakly locks the TaskSet so the set and task reaction cannot own each other. |

### `src/runtime/http_upgrade.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 22-42 | Upgrade refusal serialization and close wait | **SAFE** | The frame owns socket and response. Header iteration ends before suspension. The newPromise call invokes its reference-capturing executor synchronously and the socket owns only a copied resolver. |
| 83-99 | Authorization handler and refusal | **SAFE** | The owning Upgrades parameter retains the handler closure. Both request and handler info are value arguments, and refusal takes a Response copy out of the parent frame's variant. |
| 96-111,176-195 | WebSocket added after shutdown snapshot | **DEFECT** ([D06](#d06)) | Authorization checks stopping before the asynchronously resumed module wait. Shutdown can snapshot sockets during that wait. The resumed authorization can then accept a socket the shutdown task never closed. |
| 112-145 | WebSocket construction, handshake callback and close registration | **SAFE** | The selected ws options complete the handoff synchronously. Headers and accept callable are owned, close removes the map edge, and WebSocketRegistry clears attached socket listeners on loop destruction. |
| 148-173 | Upgrade socket close/error listeners | **SAFE** | The upgrade frame owns state and socket. Listeners own a controller with no strong socket edge and are removed after either outcome. |
| 176-195 | WebSocket shutdown timer and aggregate ownership | **SAFE** | The loop traverses an owning socket snapshot. newPromise immediately consumes its socket reference. The timer target is weak and close owns only the timer and resolver. Sibling operations remain independently owned if all rejects. |
| 199-208 | Returned WebSocketUpgrades wrappers | **SAFE** | Both wrappers are synchronous closures that pass an owning copy of Upgrades into free coroutine frames. |

### `src/runtime/native_kv_bootstrap.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 70-89 | Receipt string views | **SAFE** | The parsed JSON is alive for the synchronous call and fields are copied into the returned binding. |
| 143-148 | Deferred native KV preparation | **SAFE** | then owns the callback, which copies repositoryRoot and does synchronous receipt verification and preparation. |

### `src/runtime/permissions_linux.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 116-160 | Seccomp filter builders and syscall view | **SAFE** | Local closures fill the stack buffer synchronously, and the syscall consumes its program pointer before the frame exits. |
| 175-228,242-262,323-374 | Native Landlock and permission callbacks | **SAFE** | Rule data, grants, path views and receipt callbacks stay within synchronous stack scopes. Native permissions stores function pointers to linked functions. |

### `src/runtime/websocket.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 17-25 | WebSocket module memoization | **SAFE** | The free task returns owned constructor function objects without an intermediate suspension. The static retained Promise stores the result and no caller locals. |

### `include/uos/rt/fs.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 103,204-244,262-284 | Filesystem views, FileHandle and path pack | **SAFE** | The fs/promises names wrap synchronous cores. FileHandle owns its descriptor and borrows the read buffer only for one syscall. The template view arrays are consumed by synchronous path helpers. |

### `include/uos/rt/kv.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 77-80,94-99,193-199,335-336,380-419 | KV value and key views | **SAFE** | Input views are copied into owned variants. Reference and find accessors borrow the containing value. The codecs and traced users consume them synchronously or copy before suspension. |
| 659-661,680-702,805-817,824-837 | Atomic builder and forwarding receiver contract | **SAFE** | Application commits retain the named builder and parent KV owner while awaiting. The production commit functions are synchronous forwarding closures that pass an owned store into free coroutine frames. |
| 707-723 | KV iterator ownership contract | **SAFE** | Call sites retain a unique iterator across next or return. kvListToArray borrows the caller-owned iterator and its named call sites await it through completion. |
| 730-771 | Typed KV lookup coroutines | **SAFE** | Keys and options are copied into the frame. Current typed callers retain their store through the initial virtual call and await. Result conversions read only the frame-local entry or vector. |
| 847,860-882,899-917 | Memory KV clock and database owners | **SAFE** | The database owns the copied clock and version callbacks or their static defaults. The handle owns its database and the database tracks native handles weakly. |

### `include/uos/rt/kv_js.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 104-106 | Uint8Array byte view contract | **SAFE** | The span borrows its input value explicitly. Implemented use in key conversion copies the bytes immediately and replay deserialization consumes them synchronously from a named entry. |

### `include/uos/rt/kv_sqlite.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 24-31,40-55,73-82 | SQLite KV owner and callback contract | **SAFE** | Each handle owns its state and joins the database worker on close. Clock and codec callables are value-owned, and codec views are used synchronously. Production enqueue keeps the shared KV in its caller frame. |

### `include/uos/rt/process.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 45-56 | argv and ScopedArgv | **SAFE** | The vector has static storage and ScopedArgv owns the previous contents. Traced consumers use or copy arguments before starting asynchronous work. |
| 84-118 | Signal listener and signal name views | **SAFE** | Listener copies own shared callable identity. Signal-name views refer to static strings, and notification takes a copied listener snapshot in the process event implementation. |
| 126-168,211-225 | Spawn, pipe and Bun file façade | **SAFE** | Options and file path have owning storage. Subprocess and sink handles own their state. A file stream takes its own path and source state at construction. |

### `include/uos/rt/sqlite.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 97-106,241-299,309-354 | SQLite bindings and synchronous row iterator | **SAFE** | Bindings own their bytes and row iterator owns its statement and bindings. Its input iterator borrows the named RowIterator only for synchronous production iteration, and a result row references the current iterator element only until advance. |
| 425,449-482 | SQLite transaction callable | **SAFE** | Transaction owns the functor, connection and controller. Its reference-capturing invocation lambda runs synchronously inside runTransaction and all traced production transaction bodies are synchronous. |

### `src/rt/fs.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 349-376,502-535,607-655 | File wrappers and read/write buffers | **SAFE** | Filesystem operations use borrowed buffers only inside synchronous syscalls. Scoped descriptors close on error or on return, and FileHandle moves invalidate the source descriptor. |
| 675-716 | Path helper views | **SAFE** | The call resolves and normalizes the input pieces synchronously and produces owning path strings. |

### `src/rt/kv.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 73 | GenericAtomicOperation::commit | **SAFE** | Every traced production caller has a named atomic builder through the direct commit await. The builder owns commitFn, and native wrappers start free coroutines with copied StorePtr and mutation vectors. |
| 77-80 | kvListToArray borrowed iterator | **SAFE** | The caller in auth/passkeys.cpp retains its named owning list iterator through the direct await. Each returned entry is moved into the collector's owned vector. |
| 85-97,134 | Forwarding KV and atomic tasks | **SAFE** | These synchronous wrappers consume the receiver only to obtain a child task. Actual callers retain the inner owner through the await, and durable read/write implementations capture their StorePtr before returning. |

### `src/rt/kv_internal.hpp`

Private synchronous codec declarations. Views and references are consumed during the call, and each result type owns its data.

### `src/rt/kv_js.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 49-88,244-282,304-324 | Stored JSON and string conversion | **SAFE** | Typed-array element and variant references are read synchronously, with owned JSON and string outputs. |
| 330-362 | Uint8Array view and KV key conversion | **SAFE** | The input KvValue owns the backing store. The key conversion copies the subspan synchronously, and replay_read consumes it during synchronous deserialization. |

### `src/rt/kv_jsc.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 55-98,108-230 | JSC serializer string pool and visitor | **SAFE** | The Serializer and its pool are local to a synchronous encode. The visitor runs inline and copies fields into the owned output, with no map iterator retained between pool insertions. |

### `src/rt/kv_jsc_deserialize.cpp`

The input span is borrowed only during the synchronous Decoder call. Local string/object pools and byte subspans produce owned KvValue results, including the inner storage envelope. No coroutine or deferred callback.

### `src/rt/kv_key.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 131-190,192-245,261-334,378-491,509-525 | KV key codecs and comparison helpers | **SAFE** | Reference-capturing lambdas are synchronous and parser subspans point to the current call's input. Pack and unpack build owning key results. |
| 343-360 | KV tuple variant accessors | **SAFE** | Byte, string and BigInt references are into the caller's KvKeyPart. The serializing and checking call sites consume them synchronously before any await. |

### `src/rt/kv_memory.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 19-24,88-118,132-192,196-214 | Memory native tasks and transaction state | **SAFE** | The SDK StorePtr owns MemoryNative. Its row reads, undo log and expiry mutation are synchronous, and only copied results and exception pointers survive the macrotask. |
| 248-265,303-324 | Memory KV commit and codec/clock captures | **SAFE** | The builder closure is noncoroutine and gives the task a StorePtr. Stored clock and codec callbacks execute synchronously and the native database holds only weak handles back. |
| 270-302 | Checked list iterator next/return | **SAFE** | A named unique iterator in each traced caller owns ListStream and remains live through each awaited step. The cursor after the await is copied from that still-owned child. |
| 337-359 | Memory KV task wrappers and unconditional atomic writes | **SAFE** | Public read/write wrappers take a store or handle copy before building a Task. The free write coroutines retain the unique operation through commit. |
| 350-354 | Memory delete directly projects the awaited commit result | **DEFECT** ([D07](#d07)) | The operation suspends with a result temporary embedded in the member-access expression. The retained GCC 12 comparison rule requires a named awaited KvCommitResult before reading ok. |

### `src/rt/kv_sdk.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 101-143,145-154 | Mutation and value codec callbacks | **SAFE** | Validation and decoding invoke callback references synchronously on owned message or result data, before any later suspension. |
| 156-197 | KV get and getMany snapshot | **SAFE** | StorePtr owns Native. Keys and read ranges are moved into the native call, and result vectors and rowMap are owned locals created or traversed after the await. |
| 200-231 | KV SDK commit | **SAFE** | StorePtr owns the native target and checks, mutations and enqueues are owning vectors. All view use ends before the await of the native write. |
| 236-288,290-317,319-333 | ListStream and GenericListIterator next/fetch/return | **SAFE** | Callers own and await named iterators sequentially. ListStream owns StorePtr. Fetch starts from copied range bounds, and entry references are taken after fetch and copied before returning. |

### `src/rt/kv_sdk.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 95,105-124,133-143 | Native request and codec interfaces | **SAFE** | Native ranges/messages are owning value parameters. Store owns codecs and the native target, and callback input views are for synchronous use only. |
| 157-191 | List stream owner contract | **SAFE** | GenericListIterator owns its stream, which in turn owns StorePtr and batch storage. The traced callers retain the iterator through every next/return task. |

### `src/rt/kv_sqlite.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 500-634,798-823,846-849 | SQLite Worker thread and backend ownership | **SAFE** | Worker captures this only in its owned thread, which shutdown and destruction join before members die. Jobs move into a local under lock and run unlocked over Worker-owned Backend. |
| 649-678 | Worker result and normal completion handoff | **SAFE** | The coroutine and initiation own Worker, the queued job owns its body, handler, result and work guard, and the worker publishes the copied result to the event loop before resume. Orderly close drains or fails jobs and joins. |
| 651-674 | SQLite completion after forced event-loop teardown | **UNCLEAR** | Worker jobs use an io_context reference. A work guard keeps a run alive but does not own the context. Establish a supported exit path that destroys the context before KV close joins the worker. |
| 682-685,700-739 | SQLite native snapshot/write and refusal | **SAFE** | SDK StorePtr keeps the native target alive. Native inputs and queued write state are owned, and no worker uses references to the request frame's inputs. |
| 749-788,825-844 | SQLite SDK operations and queue delegation | **SAFE** | Free SDK coroutines own StorePtr and applicable arguments. Named operations or tasks cover suspension. The actual paid enqueue path keeps its shared KV wrapper alive until completion. |
| 262,307,318 | Synchronous SQLite result scans | **SAFE** | Statement::all returns a vector of owning rows synchronously. The range-for extends the returned vector through its nonsuspending body. |

### `src/rt/kv_value.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 101-123,190-229,347-379,483-512 | KV value projection and synchronous walk | **SAFE** | The immediate visitors, comparison and validation use aliases only during synchronous calls. Returned element references borrow the named container. Serializers and traced consumers copy before a suspension. |

### `src/rt/process.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 54-68 | Process argv snapshot and override | **SAFE** | Process arguments are copied into process-lifetime vector storage. ScopedArgv owns the previous vector and restores it synchronously. |
| 117-134 | Signal name mapping | **SAFE** | Returned names view static storage and argument names are compared synchronously. |

### `src/rt/sqlite.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 142-207 | Native SQLite API callback table | **SAFE** | Either file-static linked symbols or process-lifetime dynamic library symbols back the copied API table. No callback retains a caller frame. |
| 284-347,911-928,951-970 | SQLite connection, statement cache and close | **SAFE** | Statements own their Connection, which tracks them weakly. Query copies the owning statement before cache mutation, and close locks each tracked statement before finalization. |
| 412-445,528-564,584-628 | SQLite binding and result storage | **SAFE** | SQLITE_TRANSIENT copies text/binary while their owners are live. Column names and values are copied before stepping or resetting the statement. |
| 811-832,866-886 | SQLite lazy RowIterator | **SAFE** | The iterator owns a statement handle and a copy of positional or named bindings. next uses them synchronously and returns an owning Row. |
| 1002-1031 | Synchronous database transaction dispatcher | **SAFE** | The database and controller are owned by Transaction during invocation. RunTransaction calls body inline before commit or rollback. |
| 375-383,804-809 | Synchronous binding visitor and all adapters | **SAFE** | The this-capturing visitor runs inline during construction. Statement::all copies each row before returning and retains no external callback. |

### `src/rt/sqlite_darwin.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 32-56,59-84 | Darwin read-only VFS and URI parsing | **SAFE** | Registration copies the parent into static VFS storage and installs a captureless callback through the process-lifetime parent. The query view is consumed before path mutation. |

### `include/uos/storage/key_compatibility.hpp`

The only API consumes a byte span synchronously. No coroutine or deferred callback.

### `include/uos/storage/kv.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 53-69 | Replaceable native opener and codec interface | **SAFE** | The real opener is a free coroutine taking path/options by value. Test overrides delegate to free tasks and stay installed until their scopes finish. Codec views are synchronous. |

### `include/uos/storage/native_addon.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 44-75 | Native artifact and NAPI callback contracts | **SAFE** | The artifact operations are synchronous and prepared NapiInterface is copied out. Linked export stubs own their messages and have no deferred use of input paths or bytes. |

### `include/uos/storage/native_kv.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 17-35 | retrySqliteBusy callable ownership | **SAFE** | The retry coroutine owns run by value between attempts and awaits each child through result or error before invoking it again. Each call site supplies any pointee owner separately. |

### `include/uos/storage/sqlite_readonly.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 40-50,90-108 | Read-only statement and inspection callback | **SAFE** | The statement is an owning handle. The erased callback runs inline even during a fallback retry, while the caller's callable, optional result and database are alive. |

### `include/uos/storage/value_compatibility.hpp`

A declaration alias to the synchronous KV value walk. No separate lifetime site.

### `src/storage/key_compatibility.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 17-52 | Portable key span | **SAFE** | The validator walks the caller's bytes synchronously and returns no borrow. |

### `src/storage/kv.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 20-29,53-66 | Codec inspection and creation transaction | **SAFE** | The read callback copies the codec string before closing. The reference-capturing transaction runs immediately and synchronously while the named database remains owned. |
| 80-109,112-120 | Linked native KV opener and codec hooks | **SAFE** | The default opener is a free coroutine owning path/options. It uses references to members of its own frame, and the default decoder returns an owning value synchronously. |
| 124-143 | Open and retry coroutine closure | **SAFE** | retrySqliteBusy owns the capturing open closure by value and immediately awaits each invocation. Its copied target/NAPI and the child opener's moved options remain live through the waits. |

### `src/storage/native_addon.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 39-75,92-134 | Artifact verification views | **SAFE** | Named input and provenance JSON retain all field views and file bytes through synchronous verification. The returned artifact owns its strings. |
| 137-179,187-210 | Prepared export callback owners | **SAFE** | Prepared stores owning NAPI callback copies, the require call consumes its path synchronously, and linked export stubs own copied error messages. |

### `src/storage/native_kv.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 25-50 | CheckedAtomicOperation retry receiver | **SAFE** | The retry frame owns commitOnce and the caller's named atomic operation spans the direct commit await. That operation owns both the inner builder and native KV. |
| 55-85,88-106 | CheckedListIterator next and return | **SAFE** | The current scan owns the named CheckedListIterator for each awaited next. The iterator keeps native and inner iterator alive while the retry callback uses this. |
| 118-126 | Guarded reads across retry/fail-fast aggregate | **SAFE** | Read wrappers immediately copy native, key vectors and options into the by-value retry callback. A rejecting sibling can unwind its caller without dropping these owners. |
| 128-141,155-170 | Guarded writes and queue receiver | **SAFE** | Current callers retain the wrapper until the initial member resume. The write frame owns an independent atomic builder after starting, and the awaited enqueue retains the same owning store in its caller. |

### `src/storage/sqlite_readonly.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 19-29,36-52 | Read-only SQL adapters | **SAFE** | Statements copy the shared native statement handle and row conversion returns owned JSON. SQL views do not escape the synchronous calls. |
| 75-107 | Read-only inspection lifetime | **SAFE** | The passed callback is invoked inline against a database opened and closed within that attempt, including the fallback. The caller's callback owner is live throughout both calls. |

### `src/storage/value_compatibility.cpp`

This translation unit delegates to the synchronous runtime value validator, already classified in src/rt/kv_value.cpp.

### `include/uos/cache/kv_migration.hpp`

The migration entry takes an owning shared KV. The progress borrow is private and covered at its definition and call site.

### `include/uos/cache/kv_migration_base.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 129-136 | Migration import options and scan ownership | **SAFE** | Import overloads are ordinary functions that synchronously copy options into ImportState. The returned tasks own their input lines or generator, state, and KV. |

### `include/uos/cache/kv_migration_paid_fallback.hpp`

The inventory overloads own their KV and input sets. Private borrowed collectors are classified in the source.

### `include/uos/cache/kv_migration_projection.hpp`

Both projection entries own their KV and pair vector. Private borrows and snapshots are classified at their definitions.

### `include/uos/cache/kv_migration_usage.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 69-70 | Bounded counter handoff policy borrow | **SAFE** | The caller owns the normalized policy in its frame and awaits the handoff through settlement. The handoff is not raced with sibling work. |

### `include/uos/cache/kv_migration_validation.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 18-20 | Validation error accumulator borrow | **SAFE** | The validation frame owns the error vector and directly awaits both helpers before returning or continuing. |

### `include/uos/cache/prompt_analytics.hpp`

Exports types and APIs defined by the related cache headers. No inline coroutine or independent callback registration.

### `include/uos/cache/prompt_analytics_core.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 88-89,141-151,229-237 | Counter and options storage | **SAFE** | Counter member accessors borrow the named counter object synchronously. Async options carry an owning KV handle and store the optional clock as an owning function. |

### `include/uos/cache/prompt_analytics_queue.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 15-35,46-71,79-94 | Optional telemetry payload and queue options | **SAFE** | Queue values own event metadata and any injected store. No request frame borrow is stored. An injected queue is a copied handle over state and the sink takes its entry by value. |

### `include/uos/cache/prompt_analytics_read.hpp`

Public reads own their options. Group-by validation borrows JSON only in a synchronous check. Read suspension points are reviewed in the source.

### `include/uos/cache/scope_experiment.hpp`

The experiment takes an owning telemetry baseline value. No inline coroutine or registration.

### `include/uos/cache/scope_experiment_model.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 44,158-187 | Experiment options and response read contract | **SAFE** | ReadSample takes Response and expected model by value. State and evidence parsing reads JSON views synchronously and returns owned records. |

### `include/uos/cache/scope_experiment_targets.hpp`

Target resolution, cycle and lease tasks own their KV plus state and target parameters. Implementations cover the await sites.

### `include/uos/cache/scope_targets.hpp`

Inputs, configured bindings and an injected store are supplied by value to the inventory task entries.

### `include/uos/cache/telemetry_gate.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 42-49,129-147 | Telemetry option and request values | **SAFE** | Entries own targets/events/options before the first suspension. An injected KV is a shared owner and result counter strings are owned. |

### `src/cache/kv_migration.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 36-63 | migrateApiKeyQuotaV3 parallel reads and compare-and-set | **SAFE** | Each read owns its key and native retry state if its sibling rejects. Structured binding is over the frame-local tuple after the await, and the named operation owns its copied entries. |
| 65-80,158-165 | Migration progress and handoff await | **SAFE** | The sole migration caller retains progress and its const pair inventory while awaiting the entire child. The handoff wrapper copies the policy before the child starts. |
| 92-109 | Pending ledger migration batch | **SAFE** | Candidate references are into the coroutine's own immutable vector and the named operation remains live during commit and reread. |
| 113-148 | Initial migration settings and catalog | **SAFE** | The frame owns KV and all read results. Inline key lists use the permitted variadic KvKey constructor, and JSON borrows are used synchronously. |
| 150-190 | Migration phases and final write | **SAFE** | Inventory and projection tasks own KV and inputs or are awaited before local inputs change. The frame retains both the finish builder and its result through the terminal write. |

### `src/cache/kv_migration_base.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 298-330 | Imported window reconciliation scan | **SAFE** | Window keys are an owning by-value map and used only by this scan. Nested reservation entries and atomic mutations are owned through their awaited steps. |
| 343-366 | Import entry lookup and write | **SAFE** | The frame owns an immutable entry and shared import state. Direct reads and writes copy keys and values, and the local atomic builder survives commit. |
| 369-414 | Import state and finalization | **SAFE** | Group counters are dereferenced before mutation. Per-line work and window reconciliation get owners or a copied map. The parent awaits each step. |
| 416-438,488-494 | Vector and generator import adapters | **SAFE** | The wrappers synchronously copy options. Vector storage is frame-owned and unmodified except for individual moved strings, and the by-value generator remains live while awaiting next. |
| 496-508 | Count a KV prefix | **SAFE** | A named iterator and owned store stay in this counting task until it returns, independently of any fail-fast aggregation. |

### `src/cache/kv_migration_paid_fallback.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 143-166 | collectPaidFallbackWindows | **SAFE** | inspectPaidFallbackV3 owns knownKeyIds/errors and directly awaits this scan. Its local iterator and row values remain alive and only copied values enter the result. |
| 169-191 | collectPaidFallbackRequests | **SAFE** | The inspecting parent retains knownKeyIds/errors through the direct await. The scan uses read results, not aliases into persistent mutable storage. |
| 194-213 | collectPaidFallbackPending | **SAFE** | The shared KV and local iterator own the scan. The direct caller retains its error vector through the whole await. |
| 216-232 | countPaidFallbackLeases | **SAFE** | The sequential inventory owner retains its known-key set and errors. Each iteration consumes a value result before the next await. |
| 235-251 | countPaidFallbackDeletionGuards | **SAFE** | The error vector belongs to the parent that directly awaits this scan. The iterator and row are local owners. |
| 438-464 | Paid fallback inventory scheduling | **SAFE** | All five borrowed scans run sequentially under the owning inventory frame. Final summaries use owned inventory values after the awaits. |

### `src/cache/kv_migration_projection.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 230-250,408-422 | Candidate and request-row scans | **SAFE** | Frame-owned iterators yield copied entries. The prefix references are static and the result maps/vectors contain copies. |
| 305-333 | Projected pending-marker repair | **SAFE** | The request reference is into the current frame-owned entry, never into the iterator's mutable batch. The named operation owns its mutations and is directly awaited. |
| 361-397 | Collect repairable windows with borrowed lookups | **SAFE** | repairProjectedPaidFallbackWindows retains its local record map and caller-owned candidate set across the structured wait, with all map alias use in individual synchronous statements. |
| 484-522,524-537 | Repair window context across request scan and CAS | **SAFE** | The directly awaiting parent owns its named context and immutable windows. Borrowed projected fields are consumed before the commit starts. |
| 609-650 | Legacy projection commit helper | **SAFE** | The parent passes named keys/records and immediately awaits the helper. The atomic builder copies mutation inputs before suspension and stays owned through commit. |
| 653-706 | Legacy candidate projection and aggregate orchestration | **SAFE** | The serial caller owns candidates, records and mutable hit set through the child. Fail-fast children are KV reads that own their inputs and never capture those references. |
| 716-729 | Collect dedicated ledger references | **SAFE** | inspectPending retains errors and awaits this scan before starting the next. The iterator and result set are local. |
| 732-760,776-793 | Collect pending legacy candidates and validate ledger | **SAFE** | Parent owns references, pairs and errors through sequential waits. A value reference spans a lookup only into the still-owned current entry, then is copied into the result vector. |

### `src/cache/kv_migration_usage.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 111-149 | Seed bounded counter baseline | **SAFE** | The awaiting handoff frame owns policy, keys, entries and existing window. The named atomic builder copies each row/check before commit. |
| 152-180 | Update bounded counter baseline | **SAFE** | The same handoff frame owns all references through the direct await. Result generation uses scalars and the atomic owner stays live. |
| 189-231,799-802 | Counter handoff wrapper and parallel reads | **SAFE** | The ordinary wrapper copies policy before constructing the free task. Reads own their keys and stores if all rejects. The frame builds and awaits each borrowed writer sequentially. |
| 280-291 | Kernel migration default reads | **SAFE** | Each read owns its key and store through the aggregate and any early rejection. The result vector is frame-local. |
| 297-303,382-413 | Legacy kernel delete and migrated write | **SAFE** | The direct parent retains borrowed records, identity and named context through both waits. Child operations own checks and rows. |
| 416-457,814-821 | Legacy kernel entry and scope scans | **SAFE** | The scope frame retains the iterator, current entry and defaults through its child's direct await. Parallel read children own their arguments. Resulting rows and write context are local. |
| 475-502 | API-key usage V3 window scan | **SAFE** | inspectApiKeyUsageV3 owns knownKeyIds and errors across the direct await. Each row and field alias remains local to one iteration. |
| 521-555 | API-key usage V3 request scan | **SAFE** | The parent retains known ids and errors. Map lookups are dereferenced or overwritten synchronously between next calls. |
| 591-625,654-688,843-875 | Parallel kernel quota audit scans | **SAFE** | Each scan owns KV, prefix, scope, iterator and shared scan state. Mutable map pointers are used between awaits and the parent iterates shared state only after all fulfilled. |
| 691-708,716-747,877-885 | Strict API key hash and record inventory | **SAFE** | The parent owns errors and the completed hash index during the sequential awaited scans. Row references are resolved and copied before the next await. |
| 804-812,823-841 | Count baselines and combine usage audits | **SAFE** | The count owns its iterator. Usage inventory owns the set and errors during its sequential scans. Only copied scan results are traversed afterward. |

### `src/cache/kv_migration_validation.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 87-134 | Migration prefix count fan-out | **SAFE** | Every child owns its prefix, iterator and shared KV. Early all rejection cannot release other scans' dependencies. |
| 137-148 | Pending ledger validation scan | **SAFE** | The iterator remains named through each await and early return_ cleanup. The match is an owning string compared before cleanup. |
| 151-166 | Audit bounded API-key aggregates | **SAFE** | The awaiting policy audit owns the policy/record and retains its validation parent's errors. Both named lookup results are owned. |
| 169-191 | Audit one API-key policy | **SAFE** | validateTarget retains the pair snapshot and error vector until the structured await returns. Nested tasks take copied ids or directly awaited references. |
| 194-219 | Validate Codex model snapshot | **SAFE** | Validation owns errors through direct await, and model views refer to frame-owned read results. The inline key lists invoke the supported KvKey variadic constructor. |
| 234-249 | Check reset-usage rows | **SAFE** | The caller keeps errors through the direct await. The iterator is local and only a static prefix and copied rows are used. |
| 252-265 | Settings read fan-out | **SAFE** | Children own their key/store snapshots independently of the fail-fast aggregate. No lambda or vector element is borrowed by a child. |
| 267-355 | End-to-end migration validation | **SAFE** | The parent owns scan state, pair inventory and errors, awaits every borrowed audit sequentially and combines owning scan results afterward. |
| 358-373,375-390 | Redemption and manual intent scans | **SAFE** | The validation frame retains errors. A named iterator and owned entry cover each scan step. No sibling uses these references. |

### `src/cache/prompt_analytics.cpp`

This facade does not define a coroutine or register callbacks. Its exported implementation modules are reviewed individually.

### `src/cache/prompt_analytics_core.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 287-305 | Analytics KV resolution | **SAFE** | The wrappers move the optional shared owner to a free coroutine. Both an injected KV and the environment KV promise survive their respective paths. |
| 387-406 | Resolve cohort and hash model | **SAFE** | The task owns the event and route metadata, and the hash task receives its own string before awaiting the named promise. |
| 409-438 | Counter-key callbacks and atomic writes | **SAFE** | Counter-key functions are invoked synchronously by incrementCounters. The reference to cohort is local to that call and the named owning atomic builder spans commit. |
| 441-448,463-502,521-544 | Admit cohort or overflow and retry | **SAFE** | Each admission child owns a copy of the aggregate. References point into that own frame or the local entries vector, and named builders survive every commit. |
| 546-572 | Record analytics lifecycle | **SAFE** | The task owns its event/options, accepted cohort refers to its own const target variant, and commit gets copied/moved inputs before suspension. |
| 599-616 | Detached prune and fail-fast delete batch | **SAFE** | Prune owns options with a KV handle and a clock holding a copied scalar. Each guarded delete starts and builds its own atomic owner before all can reject. |
| 626-692 | Prune stored ranges and batches | **SAFE** | The frame owns its scan array, iterators, and batch. It moves the entire batch into the deletion task and uses no aliases into it after starting that task. |

### `src/cache/prompt_analytics_queue.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 122-143 | Queued analytics sink | **SAFE** | A by-value entry keeps all fields and any injected KV live. The child commit receives independent cohort/usage values and a shared store. |
| 151-170 | Lazy queue writer and measurement registration | **SAFE** | Queue state owns a captureless noncoroutine writer that moves an entry into an owning task. The measurement callback is synchronous. |
| 173-220 | Enqueue after cohort resolution | **SAFE** | The local target, event and options remain owned through the hash wait. Queue and entry values are copied before enqueue, and closure state is not borrowed from the request. |
| 222-245 | Optional queue flush, close, and reset | **SAFE** | A close task receives a copied queue state before awaiting its promise. The static optional is only read synchronously, so the test reset can drop its handle without invalidating the active task. |

### `src/cache/prompt_analytics_read.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 361-399 | Read analytics with three possible scans | **SAFE** | The coroutine owns KV, window, group-by and the named iterators. Each accumulator consumes its copied row synchronously before the next iterator await. |
| 415-468 | Analytics read orchestration | **SAFE** | Options, grouped data and all scan arguments are owned by the frames. Aggregation uses an immutable result snapshot and finishes synchronously. |

### `src/cache/scope_experiment.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 19-22,37-43 | Parallel experiment persistence reads | **SAFE** | Each read owns a KV/store copy and key, and the tuple is named before decomposition or moves. |
| 44-63,77-97 | Persist or finalize one cycle | **SAFE** | Each task owns state/lease/evidence, awaits its read snapshot, and keeps the named atomic builder through commit. |
| 103-117 | Refresh a pinned experiment session | **SAFE** | The frame owns the store, session, binding and abort signal. The refreshed result is used only as a view into the local owned variant and copied before return. |
| 126-168 | Run an experiment cycle | **SAFE** | The reference-capturing inconclusive callback runs synchronously in its own frame. Transport and parse tasks take value args, and all sample references are consumed before the next suspension. |
| 179-210 | Promotion and terminal write | **SAFE** | Tasks own binding/lease inputs and share a retained promotion receipt. Fresh finalization args are built before waiting. |
| 215-236 | Cycle completion and persistence | **SAFE** | The classification reference is into the caller's immutable owned CycleRun across a directly awaited owned write, and its value is copied when returning. |
| 244-258 | Cycle failure handling | **SAFE** | Child finalization owns KV/state/options and its output is discarded. No projected field or caller borrow survives the wait. |
| 260-270,278-307 | Campaign preflight and top-level orchestration | **SAFE** | The resolved-target alias points into a frame-owned const variant. The acquired state and session are copied into later tasks, which also own the shared promotion receipt. |

### `src/cache/scope_experiment_model.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 73-82,133-145 | Detached cancel and response settlement | **SAFE** | Cancellation gets a stream handle immediately and the catch callback is captureless. The settlement coroutine retains its Response by value through exactly one transition. |
| 322-370 | Completed usage stream iterator and terminal result | **SAFE** | The frame owns Response/model/signal and the returned iterator owns its stream inputs. Payload is copied before either return_ await, and the final probe keeps the Response until settlement. |

### `src/cache/scope_experiment_targets.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 15-26,50-58,62-73,75-99,126-133 | Runtime, target campaign and cycle read fan-outs | **SAFE** | Children are free coroutines with copied KV/store and values. The aggregate result containers are frame-owned and tuples are named before binding. |
| 102-120,167-197 | Read and assert experiment telemetry baseline | **SAFE** | The resolved binding pointer points into a const result owned by the current frame across its read. Baseline and selected target are copied into the return value. |
| 198-218,220-238 | Acquire cycle and renew lease | **SAFE** | Each frame keeps its state, binding and atomic operation. Parallel lease reads own the KV and keys regardless of sibling rejection. |

### `src/cache/scope_targets.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 82-96,110-120,201-206 | Four fingerprint hash awaits with inline string temporaries | **DEFECT** ([D08](#d08)) | Each inline canonicalJson call constructs a string in the same full-expression as a suspending hash coroutine taking that string by value. The port's GCC 12 rule calls for named input and separate task creation. |
| 82-96,110-126 | Owners of fingerprint material and point reads | **SAFE** | The tasks own JSON/catalog/material and key/store parameters. Field aliases refer to owned inputs. Await-expression portability at the hash sites is tracked separately. |
| 145-200 | Derive target roster across hashes | **SAFE** | Range iteration is over an immutable frame-owned catalog. Each hash task gets a copied model and snapshot, and temporary topology/map views are used synchronously. |
| 212-237 | Load catalog and auth pool | **SAFE** | The fan-out uses free children that own KV and keys. Named aggregate results are owned through inventory derivation. No caller views escape either step. |

### `src/cache/telemetry_gate.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 79-101 | Resolve telemetry KV and hash | **SAFE** | Options carry the injected store owner, or the coroutine holds the environment promise. The hash text and child Task are named before the wait. |
| 202-211 | Per-route counter read fan-out | **SAFE** | Each child owns its KV key/retry state. Counter names are static string views used synchronously, and the result vector is owned. |
| 267-298,300-354 | Resolve keys and record telemetry | **SAFE** | All async inputs and hash results are owned. Temporary key construction and mutation occur before awaiting a named atomic operation. |
| 356-467 | Baseline read fan-out and aggregation | **SAFE** | Each named route child receives copied strings and a shared KV independently of early rejection. Aggregation walks a completed immutable vector after the wait. |

### `include/uos/kernel/attestation.hpp`

Verification/context take value-owned Request and options or token; the refresh entry takes no borrowed arguments. Implementation covers cache and cryptographic state.

### `include/uos/kernel/policy_queue.hpp`

Queue-record inputs are owning strings, and list has no borrowed parameters. No inline coroutine or callback registration.

### `include/uos/kernel/quota_reservations.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 38-55,105-119 | Quota reservation task interfaces and callbacks | **SAFE** | Entries copy store/identity/record arguments. The returned reservation closures own a shared context, with timer and retry back-edges reviewed at registration. |
| 58-71,77-101 | withKernelQuotaLock callable, turn and success/error cleanup | **SAFE** | The wrapper owns the coroutine callable by value before entering the lock and awaits it directly. Previous is a copied promise and ordinary operation success and exceptions both release the current turn. |
| 79-98 | Quota lock turn abandoned before cleanup | **UNCLEAR** | The static turn remains pending if its frame is abandoned before leaveKernelQuotaLock. Establish whether an in-service cancel or a supported reusable-loop teardown can drop a live holder or queued turn, and preserve predecessor ordering in any fix. |

### `include/uos/kernel/quota_reserve.hpp`

The reservation overloads own each input and injected KV option. The lock callback ownership is reviewed at implementation.

### `include/uos/kernel/quota_v2.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 82-88 | Kernel reservation public callback handles | **SAFE** | The signal and both functions have independently owned values. The implementation's behavior when consumers drop both functions is reviewed with the timers. |
| 154,181-185 | Quota read task inputs | **SAFE** | Each async read takes store shared ownership or copies the owner and repository strings into the task. |

### `include/uos/kernel/quota_write.hpp`

Quota policy writes/delete take owned strings and option values, including the injected store. Implementation covers lock callbacks and atomic builders.

### `include/uos/kernel/usage.hpp`

Usage read methods own identities and options, with a shared store or a resolved environment handle. No inline coroutine or registration.

### `src/kernel/attestation.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 148-192,208-229 | Fetch and import key material | **SAFE** | The task owns the store, PEM rows, decoded bytes and keys. The range spans an immutable frame-local vector, and synchronous crypto returns an owning key before the microtask. |
| 197-240,469-471,511-515 | Memoized and force-reloaded key promises | **SAFE** | Every waiter copies the promise. Replacing the static slot leaves existing waiters' states live, and the load task takes no address of that slot across suspension. |
| 395-436 | Signature verification and refresh retry | **SAFE** | Each child owns its key vector and shared immutable byte arrays. The range iterator stays in that child frame, and no reference into the static cache survives a wait. |
| 442-455,568-574 | JTI cache cleanup and update | **SAFE** | Iteration and deletion are synchronous, the key is copied before deletion, and no alias into the cache is used after a suspension. |
| 505-566,584-599 | Attestation and repository context composition | **SAFE** | Both frames own the request, options, token and parsed state. Digest and signature children receive copies, and variant-member pointers are used only synchronously. |

### `src/kernel/policy_queue.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 63-81,85-138 | Queue normalization, update and optimistic commit | **SAFE** | The store and atomic builder stay in the frame. Normalization and sort consume only local data synchronously, and the selected record is copied before the commit wait. |
| 150-161 | List policy queue | **SAFE** | The promise, shared store, key and result are owned. Iteration starts after the read and finishes synchronously before return. |

### `src/kernel/quota_reservations.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 90-99,106-119,247-265 | Quota read fan-outs and named tuples | **SAFE** | Each promise starts a KV read owning its store and key. The caller materializes the tuple before binding references, and later tasks copy the projected entries. |
| 127-141,513-545 | Reservation list reconciliation and reclaim | **SAFE** | The coroutine owns each iterator, row and window. Inner tasks receive copied inputs, and return_ is awaited while the same local iterator remains alive. |
| 145-173 | Per-scope lock map operations | **SAFE** | The wrappers synchronously copy the predecessor handle before replacing the map entry, and erase a match without carrying a map iterator across suspension. |
| 178-220,247-278,305-324 | Terminal intent, settlement and renewal KV writes | **SAFE** | The local variants and atomic builders survive their commit waits. Accesses to projected results refer to immutable storage in the same frame. |
| 224-244,280-302,584-597 | Three capturing quota-lock coroutines | **SAFE** | The std::function moves into withKernelQuotaLock by value before invocation and directly awaits its child. The closure owns every capture, including store and scope values. |
| 359-404,424-435,448-470 | Lease and settlement timers on completion or loop teardown | **SAFE** | Timeout callbacks reset their own slot, durable settlement clears all remaining slots, and Loop teardown clears queued callbacks. Retry timers deliberately keep already-admitted settlement live. |
| 380-404,406-420,487-505 | Lease renewal after all reservation handles are dropped | **DEFECT** ([D09](#d09)) | The interval and context own each other. If the last commit/release handle is discarded without recording intent, successful renewals keep moving expiry and retain both the abandoned admission and KV. |
| 406-445,448-483 | In-flight renewal and settlement reactions | **SAFE** | Each relay takes immutable copies or shared context. Promise completion moves reactions out of their source state, compares an owned identity token, and clears the matching slot. |
| 550-603,606-666 | Default window scan and cutover guard | **SAFE** | The frame keeps each scan, task and optional guard. Releasing the guard copies its row and key, and iteration over the two static scopes carries no borrow of a mutable container. |
| 552-553 | Policy lookup with an inline key factory | **DEFECT** ([D14](#d14)) | A factory creates an owning KvKey inside the awaited expression. The comparison profile only exempts the direct variadic construction form from the temporary rule. |

### `src/kernel/quota_reserve.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 88-116 | Reservation read fan-out | **SAFE** | Each eager read owns its own key and KV receiver. The fail-fast aggregate retains its children, and the result tuple is named before decomposition or moving members. |
| 123-167,169-208 | Eligibility, capacity reclaim and durable reservation | **SAFE** | Each frame owns the policy snapshots and atomic builder. The window and reservation references point into that frame's const plan and remain valid when commit resumes. |
| 211-225,241-260 | Reservation attempts and capturing lock operation | **SAFE** | The lock frame owns the by-value closure through the direct await. Each attempt owns its options and moved target, and the retry alias is consumed synchronously. |
| 270-314,228-240 | Public org and effective reservation entries | **SAFE** | Each coroutine retains the selected KV owner and copies or moves all strings and options into independently owned tasks. |

### `src/kernel/quota_v2.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 56-77,170-183 | Snapshot and default policy fan-outs | **SAFE** | Child reads own their KV handles and keys across early rejection. Each aggregate tuple is a named local before any reference binding or move. |
| 99-135 | Read projected policy rows | **SAFE** | The scan frame owns its iterator and moves each row into an owning child. Each key projection is copied into strings and sort runs synchronously on the completed vector. |
| 213-247 | Public snapshot and list projections | **SAFE** | Child scope or identity arguments are owning values and all variant/vector projections are into local results after the last wait. |
| 109-113 | Window lookup with an inline key factory | **DEFECT** ([D14](#d14)) | A factory creates an owning KvKey inside the awaited expression. The comparison profile only exempts the direct variadic construction form from the temporary rule. |

### `src/kernel/quota_write.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 61-65,116-143,236-247 | Policy write, deletion and strong-read fan-outs | **SAFE** | The free read coroutine owns the key and store, and each launched read is independent of its siblings' results. The returned tuple is named before binding structured references. |
| 72-99,144-174,248-272 | Window inheritance, reset guard and compare-and-set | **SAFE** | Parameters and frame-local variant/entry storage own all projections across waits. Named builders hold staged copies, and no shared container iterator is held. |
| 177-199,275-296 | Capturing quota write and delete lock operations | **SAFE** | Both closures capture values and move into the lock frame. That frame directly awaits the invoked coroutine and keeps the callable alive on success or exception. |
| 208-228,305-315 | Public policy write and delete adapters | **SAFE** | Both wrapper and child own identities and options, and returned variants are projected into owning values. |

### `src/kernel/usage.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 266-308 | Point usage and optional daily reads | **SAFE** | The task owns request identities and options, uses a shared store and normalizes each independent result into an owned record. |
| 318-373 | Usage scans and daily lookups | **SAFE** | Each cursor and current row are frame-owned. Keys and daily buckets are copied, and the sort callbacks run synchronously on a completed local vector. |
| 277,281,301,305,332,366 | Six usage lookups with inline key factories | **DEFECT** ([D14](#d14)) | A factory creates an owning KvKey inside the awaited expression. The comparison profile only exempts the direct variadic construction form from the temporary rule. |

### `include/uos/worker/entrypoint.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 25-37,41-53 | Worker dependencies and host lifetime handles | **SAFE** | Fetch wrappers synchronously copy the shared dependency holder into owning task arguments. WaitUntil takes a promise handle by value. The deferred registrations are reviewed in entrypoint.cpp. |

### `include/uos/worker/transport.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 19-42 | Worker transport task arguments | **SAFE** | Input, attempt, abort signal and dispatch callback are passed by value. The worker task holds the shared interface receiver while directly awaiting each virtual operation. |
| 44-65 | Fixed capability callbacks | **SAFE** | The transport stores an immutable owning capability containing its callables. The actual worker and direct fixture call sites retain that receiver across the child's waits. |

### `src/worker/entrypoint.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 39-43,56-88 | Digest and bounded JSON result | **SAFE** | The coroutine owns the input, reader and chunks. It decodes bytes synchronously, records exceptions by value, and copies the reason into its cancellation task. |
| 49-55,60-80 | Bounded body-read abort listener | **DEFECT** ([D10](#d10)) | The request signal owns a listener holding that same signal and a reader. If the body read is abandoned before abort, the normal removal is skipped and the cycle retains the reader. |
| 115-123,231-257,260-268 | Delivery, refusal and dispatch persistence calls | **SAFE** | The synchronous finish calls an owning outbox task. Its finalizer retains only the request and listener until settlement. Each other frame holds its authority/dependency owner and its own handle. |
| 132-178,205-210 | Stream source tasks and per-chunk delivery | **SAFE** | Both source callbacks synchronously forward shared Delivery plus copied controller or reason. The stream-read and cancellation tasks hold their own state, and no iterator or slot reference crosses their waits. |
| 196-198 | Host receipt owns full response state | **DEFECT** ([D13](#d13)) | Delivery keeps the receipt whose pending reaction captures Delivery. A late or abandoned host receipt retains the body and upstream state even after the client discards the response. |
| 125-130,132-135,199-217 | Delivery's abort listener owns itself | **DEFECT** ([D12](#d12)) | The callback stored in Delivery captures Delivery. Dropping an unused response bypasses EOF and cancellation. When the signal was already aborted at registration, even the explicit cancellation cannot dispatch the cleanup listener. |
| 277-292,335-340 | Worker request-to-dispatch abort bridge | **DEFECT** ([D11](#d11)) | The signal owns a callback owning its own Request. Abandoning a suspended dependency before refusal or delivery handoff skips every unregister path. |
| 271-276,285-324,325-335,345-356 | Boundary fetch ownership and upstream terminal observation | **SAFE** | A free frame owns the request and dependencies. Dispatch and terminal callbacks are synchronous bridges into tasks with copied values. Upstream receiver and validation callback stay owned through their respective waits. |

### `src/worker/transport.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 15-28 | Fixed capability construction and validation | **SAFE** | Worker dependencies and direct fixture calls own the transport through the wait. The returned string view points into the named validation result and is consumed immediately. |
| 31-38 | Upstream attempt adapter | **SAFE** | The synchronous method reads capability fields immediately and puts independent values in the returned task. |
| 40-70 | Dispatch marker, upstream request, redirect cancel and observation | **SAFE** | The calling boundary or fixture retains the transport, the coroutine owns its signal and dispatch callback, and the local fetch callable is retained until its task completes. |

<!-- prettier-ignore-end -->

## Ranked defects and counts

D01 is the highest-ranked native lifetime contract violation. The rank separates
request-path buffer ownership from retained resources and comparison-profile portability.

<a id="d01"></a>

### D01. HTTP write storage released before completion, P1 request-path memory safety

- A backpressured write submits an Asio buffer over the output deque's front element.
- A disconnect or explicit destroy closes the stream and clears the deque before the
  completion callback runs. Asio requires the caller to keep the bytes valid through
  completion, including cancellation. An actual post-free access depends on the I/O backend.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ ConnectionState::flush, async write completion
           self->writing = false;
-          if (self->destroyed || failure == asio::error::operation_aborted) return;
+          if (self->destroyed) {
+            self->output.clear();
+            return;
+          }
+          if (failure == asio::error::operation_aborted) return;
@@ ConnectionState::destroy
-        output.clear();
+        if (!writing) output.clear();
         outputBytes = 0;
```

<a id="d09"></a>

### D09. Dropped quota reservation handles leave lease renewal running, P2 retained admission

- A successful quota reservation arms an interval whose callback owns the context, and the
  context owns the interval. The caller drops both terminal callbacks without invoking either.
- With a live loop and healthy KV, each renewal extends the row lease before the expiry callback
  can stop renewal. The abandoned reservation retains KV and occupies capacity indefinitely.

Give the two public callbacks a joint owner that stops renewal when the last callback is
discarded before terminal intent. The lease can then expire naturally, and a recorded terminal
intent keeps its existing renewal and durable retry behavior.

```diff
--- a/cpp/gateway/src/kernel/quota_reservations.cpp
+++ b/cpp/gateway/src/kernel/quota_reservations.cpp
@@ after stopRenewal
+    struct ReservationClient {
+      explicit ReservationClient(ReservationContext live) : context(std::move(live)) {}
+      ~ReservationClient() {
+        if (!context->terminalIntent) stopRenewal(context);
+      }
+      ReservationContext context;
+    };
@@ kernelReservationContext, after assigning leaseExpiresAtMs
+    const auto client = std::make_shared<ReservationClient>(context);
     context->timers.renewal = rt::setInterval([context] { renew(context); }, renewalIntervalMs.value_or(native_kernel::KERNEL_QUOTA_RESERVATION_RENEWAL_MS));
@@ kernelReservationContext, returned callbacks
-      .commit = [context] { return settle(context, "committed", "completed"); },
-      .release = [context](std::optional<std::string> reason) { return settle(context, "released", reason.value_or("request_incomplete")); },
+      .commit = [client] { return settle(client->context, "committed", "completed"); },
+      .release = [client](std::optional<std::string> reason) { return settle(client->context, "released", reason.value_or("request_incomplete")); },
```

<a id="d06"></a>

### D06. A WebSocket can miss shutdown's socket snapshot, P2 request-path retention

- Authorization accepts the request and waits for the WebSocket module promise after
  checking stopping. Even the cached resolved promise resumes on a microtask.
- Shutdown sets stopping and snapshots the set before authorization resumes. The new
  socket is inserted after that snapshot and remains live until independently closed.

```diff
--- a/cpp/gateway/src/runtime/http_upgrade.cpp
+++ b/cpp/gateway/src/runtime/http_upgrade.cpp
@@ authorizeAndUpgrade, after the refusal result branch
+      const rt::Promise<websocket::WebSocketModule> loading = websocket::loadWebSocketModule();
+      const websocket::WebSocketModule module = co_await loading.wait();
+      if (controller.signal().aborted() || socket->destroyed()) co_return;
       if (state->stopping) {
@@ before construction of WebSocketServer
       const WebSocketUpgrade accepted = std::get<WebSocketUpgrade>(*result);
-      const rt::Promise<websocket::WebSocketModule> loading = websocket::loadWebSocketModule();
-      const websocket::WebSocketModule module = co_await loading.wait();
```

<a id="d02"></a>

### D02. Failed bind retains the server and full handler, P2 startup resource leak

- Invalid address/port or bind failure emits error without entering the listening state.
- The error listener rejects both promises. Shutdown cannot pass the rejected ready wait
  and the server's stored handler/upgrade callbacks still own ServeState, which owns the server.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ serveHttp, server error listener
       (*live->rejectReady)(args.error);
       (*live->rejectFinished)(args.error);
+      live->server->close();
```

<a id="d03"></a>

### D03. Abandoned backpressure wait retains itself, P2 response teardown leak

- A response blocked on drain leaves four registrations owning DrainWait, which also owns
  those four callback objects.
- On abandonment of the pending coroutine, dropping the resolver reaction cannot break the
  self-cycle. If no drain/error/close/abort event fires, the connection and signal stay retained.

Give the wait coroutine ownership and make all registered callbacks weak. Its scope guard
removes the registrations on frame destruction as well as on normal return.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@
-    rt::Promise<void> waitForDrain(std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
-      return rt::newPromise<void>([&](const rt::Resolve<void>& resolve, const rt::Reject& reject) {
-        auto wait = std::make_shared<DrainWait>(outgoing, signal, resolve, reject);
-        wait->drain = IncomingListener([wait](const IncomingEvent&) {
-          wait->cleanup();
-          wait->resolve();
-        });
-        wait->failed = IncomingListener([wait](const IncomingEvent& event) { wait->fail(std::get<rt::Reason>(event)); });
-        wait->closed = IncomingListener([wait](const IncomingEvent&) { wait->fail(interrupted()); });
-        wait->closedOnAbort = rt::EventListener([wait] { wait->fail(interrupted()); });
-        outgoing->once("drain", wait->drain);
-        outgoing->once("error", wait->failed);
-        outgoing->once("close", wait->closed);
-        signal.addEventListener("abort", wait->closedOnAbort, {.once = true});
-        if (signal.aborted() || outgoing->destroyed()) wait->fail(interrupted());
-      });
+    rt::Task<void> waitForDrain(std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
+      const auto result = rt::withResolvers<void>();
+      auto wait = std::make_shared<DrainWait>(outgoing, signal, result.resolve, result.reject);
+      const rt::ScopeExit remove([&] { wait->cleanup(); });
+      const std::weak_ptr<DrainWait> weak = wait;
+      wait->drain = IncomingListener([weak](const IncomingEvent&) {
+        if (auto live = weak.lock()) {
+          live->cleanup();
+          live->resolve();
+        }
+      });
+      wait->failed = IncomingListener([weak](const IncomingEvent& event) {
+        if (auto live = weak.lock()) live->fail(std::get<rt::Reason>(event));
+      });
+      wait->closed = IncomingListener([weak](const IncomingEvent&) {
+        if (auto live = weak.lock()) live->fail(interrupted());
+      });
+      wait->closedOnAbort = rt::EventListener([weak] {
+        if (auto live = weak.lock()) live->fail(interrupted());
+      });
+      outgoing->once("drain", wait->drain);
+      outgoing->once("error", wait->failed);
+      outgoing->once("close", wait->closed);
+      signal.addEventListener("abort", wait->closedOnAbort, {.once = true});
+      if (signal.aborted() || outgoing->destroyed()) wait->fail(interrupted());
+      co_await result.promise.wait();
@@ sendResponse
-            const rt::Promise<void> draining = waitForDrain(outgoing, signal);
-            co_await draining.wait();
+            auto draining = waitForDrain(outgoing, signal);
+            co_await std::move(draining);
```

<a id="d04"></a>

### D04. Early SSE entry pins its weak-map key, P2 abandoned request retention

- The readiness commit lambda stores the request strongly in the static registry's value.
- If the frame disappears before the delivery wait returns, the unregister lambda is
  dropped uncalled. The stored value keeps its key alive, including the request body.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ handleRequest
       std::optional<ResponseReadiness> headerReadiness;
+      const rt::ScopeExit removeReadiness([&] {
+        if (headerReadiness) headerReadiness->remove();
+      });
@@ after the real delivery receipt
-      if (headerReadiness) headerReadiness->remove();
+      if (headerReadiness) {
+        headerReadiness->remove();
+        headerReadiness.reset();
+      }
```

<a id="d05"></a>

### D05. Abandoned response read leaves an abort self-cycle, P2 stream teardown leak

- A response waits for a stream read after attaching a listener with a copy of its signal.
- Dropping the send frame before either abort dispatch or explicit listener removal
  leaves the signal owning itself and the reader.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ sendResponse
-      const auto cancel = [reader, signal] {
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
+      const auto cancel = [reader, weakSignal] {
         // Never await a pending provider pull to handle a disconnected client.
-        rt::spawn(reader.cancel(signal.reason()), rt::ignoreRejection);
+        if (auto live = weakSignal.lock()) {
+          const rt::AbortSignal abortSignal(std::move(live));
+          rt::spawn(reader.cancel(abortSignal.reason()), rt::ignoreRejection);
+        }
       };
       const rt::EventListener cancelListener(cancel);
+      const rt::ScopeExit removeCancel([&] { signal.removeEventListener("abort", cancelListener); });
```

<a id="d12"></a>

### D12. Worker response abort registration owns its delivery, P2 worker request retention

- A streaming response stores an abort callback that owns the delivery state. If the client drops
  the returned body without reading or cancelling, no terminal body callback breaks that edge.
- If dispatch returns a body after abort, addEventListener ignores the already-aborted signal.
  The explicit cancellation path still leaves the stored callback owning its own state.

```diff
--- a/cpp/gateway/src/worker/entrypoint.cpp
+++ b/cpp/gateway/src/worker/entrypoint.cpp
@@ cancelDelivery
       state->bodyDone = true;
+      releaseCancellation(state);
       state->abort.abort(reason);
@@ deliverResponse
-      state->signalCancellation = rt::EventListener([state] {
-        state->lifetime.waitUntil(rt::Promise<void>(cancelDelivery(state, state->abort.signal().reason())));
-        state->signalCancellation = rt::EventListener();
+      const std::weak_ptr<Delivery> weak = state;
+      state->signalCancellation = rt::EventListener([weak] {
+        if (auto live = weak.lock()) {
+          live->lifetime.waitUntil(rt::Promise<void>(cancelDelivery(live, live->abort.signal().reason())));
+        }
       });
```

<a id="d11"></a>

### D11. Worker request abort bridge retains its own request, P2 abandoned request retention

- The request signal owns a bridge callback that owns the Request and thus the same signal.
- The worker frame can be destroyed while authentication or another dependency remains pending.
  Neither refusal nor delivery cleanup runs, so the registration keeps the request alive.

```diff
--- a/cpp/gateway/src/worker/entrypoint.cpp
+++ b/cpp/gateway/src/worker/entrypoint.cpp
@@ boundaryFetch, before registering the abort bridge
-      const rt::EventListener abortFromRequest([abort, request] { abort.abort(request.signal().reason()); });
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = request.signal().state();
+      const rt::EventListener abortFromRequest([abort, weakSignal] {
+        if (auto live = weakSignal.lock()) abort.abort(rt::AbortSignal(std::move(live)).reason());
+      });
+      auto removeAbortBridge = rt::scopeExit([&] { request.signal().removeEventListener("abort", abortFromRequest); });
@@ boundaryFetch, response handoff
-        co_return deliverResponse(upstream, requestId, request, abort, abortFromRequest, outbox, lifetime, delivery);
+        auto response = deliverResponse(upstream, requestId, request, abort, abortFromRequest, outbox, lifetime, delivery);
+        removeAbortBridge.release();
+        co_return response;
```

D13 includes destructor cleanup for the handed-off bridge in the shared settlement state.

<a id="d10"></a>

### D10. Worker body-read abort listener pins its signal and reader, P2 abandoned request retention

- The bounded body reader waits on an unfinished stream. Its abort registration owns both the
  reader and the signal that stores the registration.
- Destroying the suspended frame before an abort skips normal listener removal and leaves that
  cycle alive even after the caller discards the Request.

```diff
--- a/cpp/gateway/src/worker/entrypoint.cpp
+++ b/cpp/gateway/src/worker/entrypoint.cpp
@@ readBoundedJson
-      const rt::EventListener cancel([reader, signal] { rt::spawn(reader.cancel(signal.reason()), rt::ignoreRejection); });
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
+      const rt::EventListener cancel([reader, weakSignal] {
+        if (auto live = weakSignal.lock()) {
+          rt::spawn(reader.cancel(rt::AbortSignal(std::move(live)).reason()), rt::ignoreRejection);
+        }
+      });
+      auto removeCancel = rt::scopeExit([&] { signal.removeEventListener("abort", cancel); });
@@ after the read or its exception
       request.signal().removeEventListener("abort", cancel);
+      removeCancel.release();
       reader.releaseLock();
```

<a id="d13"></a>

### D13. Pending worker delivery receipts retain the entire stream, P2 abandoned response retention

- The receipt's reaction owns Delivery, which also owns that pending receipt.
- Even after the returned body is dropped or closed, a late or abandoned host receipt
  retains the upstream response, reader, cancellation state and host lifetime callable.
  The pending reaction is eventually cleared at explicit Loop teardown.

Have the receipt own only the shared settlement latch and its cleanup, preserving the
actual receipt outcome and one settlement. This diff includes the in-place abort
registration owner used together with D11.

```diff
--- a/cpp/gateway/src/worker/entrypoint.cpp
+++ b/cpp/gateway/src/worker/entrypoint.cpp
@@ delivery state definitions
+    struct DeliverySettlement {
+      rt::Request request;
+      rt::EventListener abortFromRequest;
+      std::shared_ptr<DeliveryOutbox> outbox;
+      std::optional<rt::Promise<void>> completed;
+      ~DeliverySettlement() { request.signal().removeEventListener("abort", abortFromRequest); }
+    };
     struct Delivery {
       UpstreamResult upstream;
-      rt::Request request;
       rt::AbortController abort;
-      rt::EventListener abortFromRequest;
-      std::shared_ptr<DeliveryOutbox> outbox;
+      std::shared_ptr<DeliverySettlement> settlement;
       BoundaryLifetime lifetime;
-      std::optional<BoundaryDelivery> delivery;
+      bool externalReceipt = false;
       bool bodyDone = false;
-      std::optional<rt::Promise<void>> completed;
@@ finish
-    rt::Promise<void> finish(const std::shared_ptr<Delivery>& state, DeliveryOutcome outcome) {
+    rt::Promise<void> finish(const std::shared_ptr<DeliverySettlement>& state, DeliveryOutcome outcome) {
@@ cancelDelivery and pullBody
-      rt::Promise<void> finishing = finish(state, "interrupted");
+      rt::Promise<void> finishing = finish(state->settlement, "interrupted");
@@ pullBody, when the reader is done
-          if (!state->delivery) {
-            rt::Promise<void> finishing = finish(state, state->abort.signal().aborted() ? "interrupted" : "unobserved");
+          if (!state->externalReceipt) {
+            rt::Promise<void> finishing = finish(state->settlement, state->abort.signal().aborted() ? "interrupted" : "unobserved");
@@ pullBody, when the reader failed
-        rt::Promise<void> finishing = finish(state, "interrupted");
+        rt::Promise<void> finishing = finish(state->settlement, "interrupted");
@@ deliverResponse, creation and external receipt
+      auto settlement = std::make_shared<DeliverySettlement>(
+        std::move(request), std::move(abortFromRequest), std::move(outbox), std::nullopt);
       auto state = std::make_shared<Delivery>(Delivery{
         .upstream = std::move(upstream),
-        .request = std::move(request),
         .abort = std::move(abort),
-        .abortFromRequest = std::move(abortFromRequest),
-        .outbox = std::move(outbox),
+        .settlement = settlement,
         .lifetime = std::move(lifetime),
-        .delivery = std::move(delivery),
+        .externalReceipt = delivery.has_value(),
         .bodyDone = false,
-        .completed = std::nullopt,
@@
-      if (state->delivery) {
-        state->lifetime.waitUntil(state->delivery->completed.then([state](DeliveryOutcome outcome) { return finish(state, std::move(outcome)); }));
+      if (delivery) {
+        state->lifetime.waitUntil(delivery->completed.then([settlement](DeliveryOutcome outcome) {
+          return finish(settlement, std::move(outcome));
+        }));
@@ deliverResponse, when the upstream has no body
-        if (!state->delivery) state->lifetime.waitUntil(finish(state, "unobserved"));
+        if (!state->externalReceipt) state->lifetime.waitUntil(finish(settlement, "unobserved"));
```

<a id="d07"></a>

### D07. Memory KV delete directly accesses an awaited temporary, P3 comparison-profile rule

- Delete waits for the named operation and receives a temporary KvCommitResult.
- Accessing the result member in that same expression violates the port's GCC 12
  comparison rule. The modern compiler audit does not establish a GCC 12 crash here.

```diff
--- a/cpp/gateway/src/rt/kv_memory.cpp
+++ b/cpp/gateway/src/rt/kv_memory.cpp
@@ unconditionalDelete
-      if (!(co_await operation->commit()).ok) throw JsError("Unconditional KV delete failed");
+      const KvCommitResult result = co_await operation->commit();
+      if (!result.ok) throw JsError("Unconditional KV delete failed");
```

<a id="d08"></a>

### D08. Cache fingerprint hashes pass inline strings to a coroutine, P3 comparison-profile rule

- Each call creates a canonical JSON string and passes it by value in the suspending full-expression.
- The retained GCC 12 comparison rule requires a separate named input and task. This was
  identified by source review, with modern Clang as the build and sanitizer profile.

```diff
--- a/cpp/gateway/src/cache/scope_targets.cpp
+++ b/cpp/gateway/src/cache/scope_targets.cpp
@@ inspectCodexAuthPoolBinding
-      auto hash = co_await utils::sha256Hex(canonicalJson(material));
+      auto text = canonicalJson(material);
+      auto hashing = utils::sha256Hex(std::move(text));
+      auto hash = co_await std::move(hashing);
@@ targetCapabilityFingerprint
-    auto hash = co_await utils::sha256Hex(canonicalJson(material));
+    auto text = canonicalJson(material);
+    auto hashing = utils::sha256Hex(std::move(text));
+    auto hash = co_await std::move(hashing);
@@ derivePromptCacheScopeTargetInventory
-  const auto inventoryHash = co_await utils::sha256Hex(canonicalJson(inventoryMaterial));
-  const auto bindingHash = co_await utils::sha256Hex(canonicalJson(bindingMaterial));
+  auto inventoryText = canonicalJson(inventoryMaterial);
+  auto inventoryHashing = utils::sha256Hex(std::move(inventoryText));
+  const auto inventoryHash = co_await std::move(inventoryHashing);
+  auto bindingText = canonicalJson(bindingMaterial);
+  auto bindingHashing = utils::sha256Hex(std::move(bindingText));
+  const auto bindingHash = co_await std::move(bindingHashing);
```

<a id="d14"></a>

### D14. Inline key factories in quota and usage awaits, P3 comparison-profile rule

- Two quota reads and six usage reads construct an owning key from a factory within the
  awaited expression. These are function-returned temporaries containing key parts.
- Apply the same split to the quota policy read at `quota_reservations.cpp:553`, window read
  at `quota_v2.cpp:111`, and usage reads at `usage.cpp:277,281,301,305,332,366`. The
  documented comparison-profile exception covers direct variadic key construction.

```diff
--- a/cpp/gateway/src/kernel/quota_reservations.cpp
+++ b/cpp/gateway/src/kernel/quota_reservations.cpp
@@ kernelScopeHasPolicy
-      const rt::KvEntryMaybe<> policyEntry = co_await kv->get(native_kernel::kernelScopedPolicyKey(scope, owner, repo), STRONG);
+      auto key = native_kernel::kernelScopedPolicyKey(scope, owner, repo);
+      auto reading = kv->get(std::move(key), STRONG);
+      const rt::KvEntryMaybe<> policyEntry = co_await std::move(reading);
--- a/cpp/gateway/src/kernel/quota_v2.cpp
+++ b/cpp/gateway/src/kernel/quota_v2.cpp
@@ kernelPolicyRow
-      const rt::KvEntryMaybe<> windowEntry = co_await kv->get(kernelScopedWindowKey(scope, owner, repo), STRONG);
+      auto key = kernelScopedWindowKey(scope, owner, repo);
+      auto reading = kv->get(std::move(key), STRONG);
+      const rt::KvEntryMaybe<> windowEntry = co_await std::move(reading);
--- a/cpp/gateway/src/kernel/usage.cpp
+++ b/cpp/gateway/src/kernel/usage.cpp
@@ getKernelUsage, repeat for the other five key factories
-      const rt::KvEntryMaybe<> entry = co_await kv->get(kernelUsageKey(owner, repo));
+      auto key = kernelUsageKey(owner, repo);
+      auto reading = kv->get(std::move(key));
+      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
```

97/97 files reviewed. **DEFECT 17, SAFE 235, UNCLEAR 4.**

The hazard tags overlap across verdicts. The index records 74 closure, 202 borrow,
106 callback, 153 container, and 28 await-expression tags.

**Handoff summary.** Area B covers all 97 files with 14 distinct defect classes across
17 defect sites, 235 safe sites and four unclear sites. The outstanding HTTP write buffer
is the highest-priority request-path finding. Local ASan/UBSan fixtures passed 23 binaries
and 265 cases. Focused probes confirmed the failed HTTP bind leak, the abandoned quota
lease renewal and the discarded worker response leak. GitHub HTTPS authentication blocked
publication of the committed audit.
