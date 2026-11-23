# Cross-module conventions and port receipts

Use pinned libraries for generic operations and retain the gateway's application, wire, validation, security, storage and settlement contracts. The conventions and wave notes below record the earlier implementation and fixture results. Current changes follow [PORTING.md](../PORTING.md) and record deliberate generic differences in [Deviations from Bun](DEVIATIONS-FROM-BUN.md).

Keep application-specific field order, coercion, status, error code and log shape where consumers rely on them. Use explicit runtime helpers for those requirements. Preserve established coroutine ownership, cancellation and receipt semantics. Promote library changes using focused application tests and robustness checks against the exact candidate. [Replacement plan](OSS-REPLACEMENT-PLAN.md) [Input limits](RUNTIME.md#input-limits)

## Records and unions (from src/types.ts and the authority contract modules)

- Records are structs: TS field names and order; numbers `double`; `T | null` and `?: T` are
  `std::optional`; `?: T | null` is `rt::Nullable`. TS-optional members carry a `{}` default
  initializer, required ones do not (so `-Wextra` rejects a designated initializer that omits
  a required member).
- Literal unions are `std::string` aliases unless exhaustively switched (then `enum class` with
  `toString`/`parse<Name>`, e.g. `CodexResetRedemptionState`).
- Discriminated unions are `std::variant` over structs named `<Union><Kind>` with a static
  `kind`/`type` member.
- Unnamed nested TS object types are nested structs (`CodexAuthState::NativeOwner`).
- Every snake_case record has Boost.JSON `tag_invoke` `value_from`/`value_to`: `value_from`
  writes members in declaration order; `value_to` is strict (throws `rt::TypeError`) and drops
  undeclared members. This is a C++ addition: where the TS validates unchecked storage itself,
  read `json::value` and validate first.
- `ResponsesRequest` / `ChatCompletionRequest` are `json::object` aliases.
- Several modules are named `types` (`uos::types`, `uos::catalog::types`,
  `uos::lib::jev_compaction::types`, `uos::harmony::types`): always write `::uos::types::X`
  fully qualified.

## Bytes with JS reference identity

- Where the TS shares one `Uint8Array` between holders (request body capture, sentinel
  zeroing), use `std::shared_ptr<rt::Bytes>` (request.hpp does). `.fill(0)` on secrets:
  `OPENSSL_cleanse`, not `std::fill`.

## Historical buffered-body observations

- A Response built from another Response's buffered body marks the source `bodyUsed` only
  after the stream is read. After an empty buffered Request body is read to its end, rt
  reports `bodyUsed` true where Bun reports false.

## Test helpers and deferred test cases

- Ported TS test helpers live in `tests/helpers/<name>.{hpp,cpp}`, namespace
  `uos::tests::helpers::<name>` (`sentinel_recorded_upstream` exists). Reuse, do not re-port.
- When you port the last module a partly ported test file needs, add the remaining cases to
  the existing `tests/<name>_test.cpp` (check it first) instead of starting a new file.
- `tests/sentinel_upstream_providers_test.cpp` hard-codes `REPLAY_ROUTES` constants from
  unported config/provider modules: switch to the real constants when those are ported.
- Frozen-object identity checks (`toBe` on a snapshot, TypeError on mutation) are ported as
  equality plus a check that mutating the returned copy cannot affect the source.

## API shapes fixed in wave 1 chunk 6

- JS number constants are `inline constexpr double`.
- `unknown` parameters: overloads `(const json::value&)` and `(const json::value*)` with
  `nullptr` meaning `undefined`.
- Structs of callbacks (`StreamDeadline`) hold `std::function` members passed by value.
- `SupervisorState` is an `enum class` with `toString`/`parseSupervisorState`.
- bun:sqlite is `uos::rt::sqlite` (`docs/rt/sqlite.md`, mandatory when your module uses
  SQLite). `storage::sqlite_readonly` and `native_schema::bun_sqlite` delegate to it
  (`bun_sqlite::adapt(rt::sqlite::Database)`). All SQLite use goes through `rt::sqlite`.
- Test fixtures: bun's per-test timeout is emulated with a referenced timer where the rt loop
  would otherwise run out of work (unreferenced `AbortSignal.timeout`).

## Facilities declared inside modules because rt lacks them (wave 1 chunk 2)

- node:http `IncomingMessage` (abstract, with `IncomingListener`/`IncomingEvent`) is declared
  in `include/uos/runtime/http_body.hpp`; the `src/http.ts` porter implements it.
- Pass options with a no-options overload rather than a `= {}` default argument, so callers
  never build an options temporary inside `co_await`.
- `task: Promise<unknown>` parameters whose value is never read are `rt::Promise<void>`.
- Test fixture files must not be named `kv*.sqlite3` (host rule); use another name.

## Wave 1 chunk 4 (jev compaction, native addon, telemetry queue)

- Native KV: there is no Node-API host. `uos::storage::native_addon::nativeKvRequire()` is a
  replaceable loader seam whose default exposes no functions, so `prepareNativeKvBackend`
  fails closed. `src/storage/kv.ts` / the durable KV component installs the natively linked
  backend there. Tests that need an open KV use `rt::MemoryKv`.
- `import.meta.url` is derived from `__FILE__` (compile-time source path).
- Interfaces implemented by callers (`JevAsker`) are abstract classes held by
  `std::shared_ptr`.
- Tests that assert on provider credentials being absent must clear those variables with
  `rt::env::ScopedEnvironment` first (the agent environment has some set). Never print them.
- A BigInt cannot live in `json::value`; tests passing `1n` as an arbitrary unserializable
  payload substitute another unserializable value and say so in a comment.

## Wave 1 chunk 8 (utils, api-keys, defaults, codex contracts, permissions)

- `uos::utils::isRecord` accepts arrays (TS quirk), unlike `rt::isRecord`. A module that
  imports `isRecord` from `src/utils.ts` must call `utils::isRecord`.
- Codex DTOs are typed structs with `toJson` row codecs (keys in declaration order).
  Snapshots, meters and payloads that are stored and read back as wire JSON stay
  `json::object`.
- TS object-identity comparisons (`===` on objects) are rewritten as equivalent value logic
  and must be pinned by a differential test.
- `src/runtime/websocket.ts` is ported on `uos::rt::ws` (reference `docs/rt/websocket.md`,
  mandatory for http-upgrade, supervisor-transport, native-auth-transport, live/sideband and
  the HTTP server stage). `new WebSocket(url, opts)` is `ws::WebSocket::create(url, opts)`;
  subclasses and test fakes derive from `ws::WebSocket(nullptr, ...)`; the server upgrade seam
  is `ws::UpgradeRequest` + `ws::UpgradeSocket`.
- Tests that await only an `AbortSignal.timeout` add a referenced keep-alive timer.

## Record structs: use the shared helper

- `uos/rt/record.hpp` (`uos::rt::record::Reader` / `Writer`, reference in `docs/rt/record.md`)
  implements the strict struct<->JSON `tag_invoke` pair. Use it for every record struct; do
  not copy or rewrite it. Name a nested reader by its full path
  (`"CodexAuthState.native_owner"`) so error text carries the path.

## Wave 1 chunk 7 (config, typed-value, legacy codec, anthropic replay)

- Module-relative file paths (`import.meta.url` + relative) resolve against `process.cwd()`
  (the release root in production) unless the module already uses the `__FILE__` idiom.
- Tests that need a different process environment per scenario re-launch the test binary
  itself (from `/proc/self/cmdline`) as a child; see `tests/config_differential_test.cpp`.
- `rt::KvValue` has no object identity: shared nodes decode as copies, cycles throw. Deep
  KvValue nesting is capped at 2000 levels in typed-value.
- `ChatReplayPublication::ToolName` (anthropic_replay) duplicates `ClaudeToolName`
  (anthropic_response_wire); the `anthropic-chat-responses` porter converts between them.

## Wave 1 chunk 3 (authority, reliability, worker)

- `contracts::Record<T>` (`include/uos/authority/contracts.hpp`) is a typed dictionary in JS
  property order with `[[Set]]` handling of `__proto__`; reuse it for `Record<string, T>`.
- `AuthorityOperations` is declared in `admission_state.hpp` and aliased in `authority_rpc`.
- `rt::RequestInit` / `rt::ResponseInit` have member defaults: partial designated
  initialization works (outside `co_await` expressions).
- TS `Reflect.ownKeys` extra-key rejection on typed records is modelled with an
  `extraOwnKeys` member (see `AccessPublication`).
- Tests needing HTTP use `rt::defaultFetch` over an `rt::MockTransport` routed by origin.

## Wave 2 chunk 6 (native-codex-mutations, native-api-key, codex/routing-state)

- Nullable parameters: `State | null` passed as `const State*` (nullptr is null) where the
  callee only reads it; `Selection | null` values as `const std::optional<T>&`; nullable
  results are `std::optional`.
- Anonymous TS result shapes get named structs (`CodexExpiredProbeClaim`,
  `CodexLocatedRoutingSlot`, `CodexAdmissionRefreshPlan` variant, ...): grep the header
  before inventing a name.
- `export * from "./x"` is an explicit list of `using` declarations in the header.
- Routing types: `RoutingAccount : CodexRoutingAccount` adds `CodexAuthState auth`;
  `CodexBlockedRoutingAccount : RoutingAccount` adds `quotaResetAtMs`; `RouteSelection` is a
  `std::variant`. `routingStateCache` is an `extern` struct with public members;
  `getCachedRoutingState()` returns a copy, so the capacity-routing porter rewrites
  `slot !== cached.slots[index]` identity checks as value logic.
- JS `array[index]` with a computed number index (canonical indexes only, negative or
  fractional reads nothing) is NOT `rt::at` (that is `Array.prototype.at`). Use `rt::element(vec, index)`
  (returns a pointer; null means undefined).
- Large differential suites use the `Replay` pattern of `tests/native_codex_capacity_test.cpp`
  (JSON inputs plus SHA-256 digests in a `.inc`).
- `tests/native_codex_conformance_test.cpp` is shared by three chunks: extend, never rewrite.

## Wave 2 chunk 2 (models/codex-models, authority/native-paid-policy)

- `CodexModelsSnapshot` is `json::object` (validated wire snapshot); `T | null | undefined`
  snapshot parameters are `const std::optional<CodexModelsSnapshot>&`.
- `PromptCacheCapabilities` / `RuntimePromptCacheCapabilities` are
  `std::variant<PromptCacheCapabilitiesUnsupported, PromptCacheCapabilitiesV1>`; TS
  `=== false` is `holds_alternative<...Unsupported>` and the arm serialises as `false`.
  `rt::record::Reader` cannot read that variant as a member (class types must be objects):
  read it through `compactPromptCacheCapabilities` or as `json::value`.
- String constants are `inline constexpr std::string_view`.
- `hasStrictPaidFallbackPolicy` and friends take `json::value` (pass typed records with
  `json::value_from(record)`).
- When a later chunk completes a partly ported test, also update the header comment listing
  the sources it links.

## Wave 2 chunk 1 (authority/native-paid)

- Gate helpers have a typed overload (`NativePaidRow<PaidFallbackReconciliationGateV3>`) and a
  raw one (`NativePaidRow<json::value>`). KV and native-SQL callers pass the raw row
  (`{.value = entry.value, .versionstamp = entry.versionstamp}`) so a malformed gate counts
  as absent as in TS; a strict `value_to` would throw.
- Guards on `unknown` take `const json::value&` plus a pointer overload (nullptr is
  undefined). `T | null` parameters are `const std::optional<T>&`; pass a typed optional
  where overloads exist (a bare `std::nullopt` is ambiguous for the lease helpers).
- Exported key-prefix constants are `inline const` values built from literals (no
  static-initialisation-order dependence). Follow this for every module-level constant that
  another module's static initialiser could read.
- Test helper `uos::tests::helpers::native_paid_conformance_vectors` exists for the
  native-authority parity tests.

## Wave 2 chunk 5 (native-codex-429, reliability/retry, sentinel/replay-limits)

- `CodexKnownProbeLease` aliases `native_codex_contracts::CodexProbeLease`.
  `Codex429Classification` is a plain struct holding the `rt::Response` handle (no codec).
- replay-limits exports are `inline constexpr double`; dependents define their aliases
  `constexpr` and compare sizes with `static_cast<double>(size)`. `rt::Math::trunc/floor/ceil/abs/max/min`
  are constexpr.
- Recursive TS scanners over untrusted nesting run on an explicit stack in C++ and throw
  `RangeError` past `rt::kJsonMaxDepth` (128).
- TOOLING HAZARD: writing a file whose content contains backslash-u escapes (`\uXXXX`) through
  the agent's Write tool or a heredoc can silently decode them. Emit such literals with a
  python script using `chr(92)` and grep the file afterwards to confirm.

## Wave 2 chunk 7 (harmony fakes, models/metadata, reliability/failure, codex/external-auth)

- `Readonly<Record<string,T>>` options: `std::optional<rt::OrderedMap<std::string,T>>`.
  Optional shared collaborators (`shell?: FakeShell`): `std::shared_ptr<T>`, nullptr is
  undefined. `string | RegExp` members: `std::variant<std::string, rt::Regex>`.
- A function returning an already-settled Task skips the microtask turn JS takes: the caller
  adds `co_await rt::microtask()` where JS await ordering is observable (router/harness after
  `execShell`).
- `ModelMetadataHint` numbers are `rt::Nullable<double>`; `resolveModelMetadata(id)` stands for
  `sources = {}`; wrap `codexSnapshotMetadataHint` results in `rt::orNull` when passing as
  `codex`.
- `ExternalNativeCodexDocument::Auth` is the `Omit<CodexAuthState, ...>` shape; native-auth
  builds `types::CodexAuthState` from it plus `updated_at_ms`.
- Only snake_case (wire) records get `value_from`/`value_to`; camelCase in-memory types do not.
- Header comments of partly ported tests name the deferred cases: finish them there.

## Cycle clusters and static-initialisation safety (binding from wave 4 on; good practice now)

1. A header includes the header that DEFINES a type, never one that re-exports it.
2. Exported error classes are defined fully inline in their header (constructors included).
3. Exported constants: literals are `inline constexpr` (double, `std::string_view`,
   `std::array<std::string_view, N>`); anything else is `inline const` in the header. Never
   `extern` plus a .cpp definition for a value another file may read during static
   initialisation.
4. Sets, maps and objects derived at module load from other modules' values are
   construct-on-first-use function-local statics.
5. Registries written at import time (`registerX(fn)` at module top level): the slot is
   construct-on-first-use; the registering .cpp registers from a namespace-scope initialiser.
6. Live `let` bindings and exported mutable containers are `extern`, defined in the .cpp, and
   only touched after `main` starts.
7. A load-time capture of `globalThis.fetch` becomes `rt::defaultFetch()` read at call time.
8. A dynamic `import()` of a first-party module becomes a direct call delayed by one
   microtask, with the same error handling.
9. Tests are assigned by link closure, not only by direct import: a test binary lists every
   .cpp it transitively calls.

## Wave 2 chunk 8 (kv, storage/kv, native-codex, native-kernel, incident outbox, ...)

- KV handle: `auto p = kv::getKv(); auto kv = co_await p.wait();` gives
  `std::shared_ptr<rt::Kv>` (null pointer is null). `Kv | null` parameters are
  `std::shared_ptr<rt::Kv>`; `kv?: Kv | null` is `std::optional<std::shared_ptr<rt::Kv>>`.
- Tests: `kv::setKvForTest(std::make_shared<rt::MemoryKv>(...))` and ALWAYS
  `kv::setKvForTest(nullptr)` at the end (module state outlives the test loop). A test that
  truly needs `openKv` reuses `prepareFixtureBackend`/`SdkScope` from
  `tests/storage_bun_kv_test.cpp`.
- Validators of KV-read `unknown` values take `const rt::KvValue&` (stored values can hold
  non-JSON parts); pure JSON guards keep taking `json::value`.
- Record structs drop undeclared stored members on rewrite; where the TS merge must keep
  extras, merge on the stored `KvObject` instead (as `ledger_stats` does).
- native-codex aliases: `CodexRoutingSlotIdentity` = `CodexRoutingIdentity`,
  `CodexRoutedAccount` = `CodexRoutingAccount`, `CodexRoutingAccumulator` =
  `CodexRoutingAccumulation`; TS ReadonlyMaps are `rt::OrderedMap`.
- Hard-coded test constants to replace when their modules land:
  `SENTINEL_REPLAY_MANIFEST_PREFIX`, `SENTINEL_REPLAY_TTL_MS` (3 sentinel tests).

## Wave 2 chunk 3 (responses-stream, http, anthropic streams, admission, failover stream)

- `ResponsesStreamEvent` is `{raw, value, type, terminal}` plus an identity token: copies are
  the same JS object. TS `===` on events compares `identity()`; never mutate a shared event.
- `ResponsesStreamIterator` is `rt::AsyncGenerator<ResponsesStreamEvent>`; a hand-written TS
  iterator becomes a generator body whose `return()` hook catches `rt::GeneratorReturn`
  around the yield and rethrows.
- `ResponsesStreamError{.cause = reason, .kind = "..."}`; `instanceof` is
  `responses_stream::responsesStreamErrorOf(reason)`.
- `(...) => void | Promise<void>` callbacks are `responses_stream::MaybeAsyncCallback<A...>`.
- `await it.return(r).catch(() => {})` is `try { co_await it.return_(); } catch (...) {}`
  then `co_await rt::microtask();`.
- Racing a read against an abort promise: if the abort promise is already rejected, feed it
  through one `.then` turn so a ready read wins (see `read` in anthropic_chat_stream.cpp).
- Inside `uos::http` write `::uos::json` (the module exports a function named `json`); callers
  write `http::json(...)`. `OpenaiErrorOptions::param` is
  `std::optional<std::optional<std::string>>` (outer empty: no property; inner empty: null).
- Every test file starts with a `// Links:` comment listing its exact unit.sh source list.
  Do the same.
- Admission test seam: `setInferenceAdmissionControllerForTest(std::nullopt)` restores.

## Wave 3 chunk 3 (cache/prompt-analytics*)

- `tests/helpers/counting_kv.{hpp,cpp}` is the port of `tests/helpers/counting-kv.ts`
  (`uos::tests::helpers::counting_kv::CountingKv`, held by `std::make_shared`, subclassable).
  Its API is shared: do not change it.
- TURN EXACTNESS: awaiting a first-party async function as
  `rt::Promise<T> p(f(...)); T v = co_await p.wait();` matches Bun's interleaving (proven by a
  concurrent CAS differential). Use it wherever concurrent interleaving is observable.
- `Partial<Record<Counter, T>>` is `prompt_analytics_core::CounterRecord<T>`.
  `PromptCacheAnalyticsEvent` nullable strings are `rt::Nullable<std::string>` (callers
  convert `string | null` with `rt::orNull`). Views have `tag_invoke` pairs: embed with
  `json::value_from(view)`.
- Option structs that derive from another options struct are built by assignment.
  `now` callbacks are `std::function<double()>` (empty means `Date.now`).
- Module state persists across ASYNC_TEST cases: a test that creates a module-level queue or
  sets the KV resets both before its loop ends (`rt::scopeExit`,
  `resetOptionalPromptCacheAnalyticsForTest()`, `kv::setKvForTest(nullptr)`).
- Do not name a test namespace alias `read` (collides with POSIX `read()`).

## rt additions (2026-11-16, rt-misc batch): use these instead of local helpers

- `WeakMap<Deno.Kv, T>`: `rt::WeakMap<rt::Kv, T>` / `rt::WeakSet<rt::Kv>` (`uos/rt/weak.hpp`;
  `Kv::identity()`); never key by `&kv`. `rt::WeakMap<rt::Promise<T>, V>` also works.
- `error.code`: `rt::JsError::code` (`std::optional<std::string>`). HAZARD: inside a member
  function of any JsError subclass, a parameter or local named `code` now fails `-Wshadow`;
  name it `code_`.
- `rt::fs::symlinkSync`, `renameSync`, `utimesSync`.
- Tests: `EXPECT_THROWS_MATCHING(expr, pattern)` is `assert.throws(fn, /regex/)`.
- `rt::join(strings, sep)`, `rt::fulfilledTask(v)` / `rt::fulfilledTask()` for
  `Promise.resolve(v)` behind a Task-typed interface, `ReadableStream::disturbed()`.
- Editing a shared header: write a temp file, `touch` it, then rename (a stale mtime leaves
  dependents' objects looking up to date).

## Wave 3 chunk 7 (codex-models-availability, kv-conversion convert/proof/cli, codex/session)

- `if (import.meta.main) { ... }` becomes an exported `rt::Task<void> importMetaMain()` that
  reads `rt::process::argv()`; a C++ entry point calls `setArgv`, awaits it and returns
  `rt::process::exitStatus()`.
- `kvOverride?: Kv | null` is `std::optional<std::shared_ptr<rt::Kv>>` (nullopt: undefined,
  use `getKv`; null pointer: null) plus an overload without the argument.
- Callbacks returning `Promise<undefined>` use `rt::Task<std::monostate>` in templates.
- Tests that create files use `rt::os::tmpdir()`; run them with `TMPDIR=<scratch dir>` and say
  so in the `// Links:` header.
- Hermetic tests of code that spawns `deno`: a fake `deno` shell script (see
  `tests/kv_conversion_convert_differential_test.cpp`).
- `tests/codex_model_availability_test.cpp` has a reusable `StubKv`; later availability,
  routing and dispatch cases extend that file.
- rt::sqlite -> `native_schema::bun_sqlite`: use `bun_sqlite::adapt(rt::sqlite::Database)`
  (convert.cpp predates it and has a local adapter).

## Wave 3 chunk 6 (kernel/attestation, paid-fallback/ledger-state, analytics, debug-routing)

- KV queue: `rt::Kv` has no `enqueue`. The capability interface is
  `paid_fallback::ledger_state::KvEnqueue` (+ `KvEnqueueOptions`), detected with
  `dynamic_cast`. A KV fake or store that supports `enqueue` derives from it (ledger-backfill
  port, the queue fake of paid-fallback.test.ts, later the durable store).
- A `kv: Kv` parameter is a non-null `std::shared_ptr<rt::Kv>`.
- Input/patch structs: an unset `std::optional` is an absent property (a merge keeps the
  stored value); `rt::Nullable` set to null is an explicit null.
- `nowMs = Date.now()` default parameters become overloads.
- `ledger_state.hpp` re-exports all native-paid keys and constants under `ledger_state::`.
- Counting or forcing commit conflicts in tests: subclass `rt::ForwardingKv` and
  `rt::ForwardingAtomicOperation` (see `ConflictingKv` in `tests/paid_fallback_test.cpp`).
- `tests/paid_fallback_test.cpp` and `tests/kernel_coverage_test.cpp` list their deferred
  cases in the header: extend those files.
- Differential vectors tag special numbers as `{"$num": text}`.

## Wave 2 chunk 4 (runtime/http + node:http server, http-upgrade, supervisor transport/inventory/follow, local-admin, native-auth-transport)

- The HTTP SERVER EXISTS: `uos::runtime::http::node_http` (`createServer`, `Server`,
  `IncomingMessage`, `ServerResponse`) and `serveHttp`. Tests that drive the gateway over HTTP
  are now portable: use `tests/helpers/loopback_http` (`ScopedLoopbackFetch` makes `fetch`
  reach loopback TCP; `RawClient` stands in for node:net). Wire behaviour is pinned in
  `tests/runtime_http_differential_vectors.inc`.
- `ServeHandler` is `std::function<rt::Task<rt::Response>(rt::Request, ServeHandlerInfo)>`;
  `info.remoteAddr` is the `Addr` variant (`std::get_if<NetAddr>`). `ServeUpgradeHandler`
  returns `rt::Task<http_upgrade::UpgradeResult>`; an empty `std::function` is undefined.
- `http.hpp` / `http_upgrade.hpp` include each other with classic include guards: include
  either.
- `SupervisorConnection::call(method, params, signal)` returns
  `Task<std::optional<json::value>>`; test seam
  `setSupervisorWebSocketConstructorForTest(ws::WebSocketConstructor)`.
- A JSON-serialised supervisor session follows the JS object key order (see `snapshotJson` in
  `tests/codex_supervisor_inventory_differential_test.cpp`), not struct order.
- FOOTGUN: `rt::get(value, 0)` picks the string-key overload with a null pointer. Write
  `rt::get(array, std::size_t{0})`.
- A node:http listener reading a body registers `on("data")` and then calls `resume()`.

## Wave 3 chunk 5 (catalog/store, runtime-config, kv-conversion/profile-json, paid-fallback/rollups)

- gzip / CompressionStream / DecompressionStream: `uos::rt::gzip` (`docs/rt/gzip.md`; zlib via
  dlopen; `gzipSync`, `Inflater`, async `gzip`/`gunzip`, Bun's error texts). Never write
  another codec.
- Store functions that take `rt::Kv&` (catalog store): the store must outlive the tasks and
  any `RefreshLeaseHeartbeat` (call `stop()` before destroying the Kv).
- Runtime config records are typed structs with strict `value_from`/`value_to`; use the
  `rt::KvValue` overload of `normalizeRuntimeConfig` for values read from KV.
  `loadRuntimeConfig` overloads: (), (shared_ptr; nullptr is null), (optional<shared_ptr>, nowMs).
- Unchecked `JSON.parse` output goes to `profile_json::toKey(const json::array&)` and
  `toValue(const json::value*)`.
- JSON guards over KV-read values use a stored-value view mapping non-JSON parts to `{}`
  (local `storedView` in runtime_config.cpp / rollups.cpp) after confirming it is exact for
  that guard.
- `tests/quota_projection_test.cpp` and `tests/codex_catalog_test.cpp` list deferred cases in
  their headers: extend them.

## Wave 3 chunk 1 (kernel quota-v2 cluster, kernel/usage)

- Stored values go to the native-kernel normalizers through
  `quota_v2::kernelQuotaValue(entry.value)` (a lossless-enough KvValue -> json view: non-JSON
  parts become `{}`); `quota_v2::kernelQuotaRead(entry)` builds `KernelQuotaRead<>`. Applies
  to cache/kv-migration-usage and kv-migration-base.
- `export *` inside an import cycle: each re-exported header ends with a
  `namespace <re-exporter> { using ...; }` block and the re-exporting header includes them at
  its END, so names resolve in any include order. Renamed re-exports are inline forwarders.
- `ok`-discriminated unions: `<Union>Ok` / `<Union>NotOk` structs with
  `static constexpr bool ok`.
- `KernelQuotaReservation{signal, commit, release}`: the promises returned by `commit` /
  `release` are already running; `release(std::nullopt)` uses `request_incomplete`.
- `withKernelQuotaLock<T>` takes `std::function<rt::Task<T>()>`: bind the lambda and the Task
  to named locals.
- Overload ambiguity: with `(const json::value*, double)` and `(double, double)` overloads an
  int literal `0` is ambiguous: pass `0.0`.
- `for await` early exit: call the iterator's `return_()` on early return.
- CountingKv is subclassable (not final): subclass it for FaultyKv/ConflictKv-style fakes.

## Wave 3 chunk 8 (static, sentinel replay model/observation/envelope/retention-schema, passport, admin/sentinel-incident)

- `src/static.ts` is namespace `uos::static_`; its module state is built on first use and paths
  resolve against the working directory (tests `chdir` to the repo root and set
  `config.isDeploy` before the first call).
- Dependency structs: fields are `std::function`; an empty one means TS undefined (default).
- Passport: `authenticatePassportSession/Admin` return `Task<optional<variant>>` (nullopt is
  null); `handlePassportRoute(Request, path)` returns `Task<optional<Response>>`;
  `resetPassportCacheForTest()` between cases.
- Sentinel: `SentinelSseInspector{push, finish}` (`finish(std::nullopt)` is "eof");
  `observation::PersistDependencies` is the seam for the replay-store port; retention schema
  validators take `rt::KvValue`; test seams `setSentinelReplayBudgetForTest`,
  `setSentinelReplayRetentionFaultsForTest`.
- `tests/helpers/admin_auth_harness` ports the kvStub part of admin-auth-harness.ts: porters
  of auth/index.ts, admin/api-keys.ts and passport.test.ts EXTEND it. Fetch mocks are a scoped
  `rt::GlobalFetchOverride` (fetch-mock.ts is not ported).
- Mutable exported objects (`TEXT_DECODER`) are `extern`, never read during static init.
- Local helpers repeated across modules that rt should own (use the existing local copies,
  report new needs): `typeof v === "object"` on `rt::KvValue`, KvKey <-> KvValue conversion,
  `JSON.stringify` over `rt::KvValue`.

## Wave 3 chunk 4 (provider/capacity-rollups, embeddings/ledger, codex/supervisor)

- `isX(v) ? v : null` over a stored value: provide and use a narrowing helper
  (`providerCapacityRollupPointOf(v)`); stored points are `rt::KvValue(json::value_from(p))`.
- `EmbeddingsIdempotencyLease` holds `std::shared_ptr<rt::Kv>`; acquire returns a variant of
  `...Acquired` / `...Replay` / `...Error`. Number-literal unions are `double` aliases.
- `tests/provider_capacity_rollups_test.cpp` writes `METERED_QUOTA_STATE_KEY` as a literal and
  skips codex/index + account-routing reset hooks: switch to the real symbols when they land.
- `tests/openapi_runtime_contract_test.cpp` lists 4 deferred cases in its header.
- SSE follow streams: abort listener and cancel hold stream state weakly (no shared_ptr cycle).

## Wave 3 chunk 2 (auth/passkeys incl. WebAuthn library port, kernel/policy-queue)

- `tests/helpers/passkeys_harness` is the reusable passkeys KV stub (`seedPasskeySession`,
  `setBeforeAtomicCommit`, `withEnv`): the auth/index porter extends it.
- `PasskeySession` is typed (`user.id`, `user.handle`, `user.credential_ids`);
  `getPasskeySessionForRequest(req, std::optional<std::string>)`.
- Where the TS spreads a stored record into a write, merge on the stored `KvObject` so
  undeclared members survive (passkeys does; prefer this over typed rewrite for spreads).
- Test idiom: `#define AWAIT(name, expr)` binds the Task, then the result, before an
  assertion reads it (keeps tests inside the GCC 12 coroutine rules).
- Process-wide module caches (CRL cache): tests depending on them run cases in a fixed order.
- ECDSA P-256/384/521, Ed25519 and RSA-PSS WebCrypto exist only locally in passkeys.cpp; a
  module needing them reports it (do not copy).
- `tests/auth_responses_branch_coverage_test.cpp` lists deferred api-key-policy and
  stream-translation cases in its header.

## Shared rt sources: staging rule

- Every `src/rt/*.cpp` links into every test binary. A new or rewritten rt source must be
  staged under a dot-prefixed name (`src/rt/.name.cpp`, ignored by the glob), compile-checked,
  then `touch`ed and renamed into place. A half-written rt .cpp breaks every agent's build.

## Wave 4 chunks 2+3 (sentinel replay-read, replay-retention-bootstrap, replay-retention-metadata)

- These modules take `rt::Kv&`; a handle passed to `runBootstrapBatch` / `bootstrapBeforeMutation`
  must outlive any background owner fill (tests settle with `co_await rt::sleep(1'000)` in fake
  time before returning).
- Typed views: `replay_read::toSentinelReplayManifest(const rt::KvValue&)` and
  `replay_retention_bootstrap::toSentinelReplayLegacyReservation(...)`: replay-store and
  replay-retention use them instead of `value_to` on `toJson()`.
- Prefix identity: `metadataEntryMatches(entry, prefix)` compares prefixes by object identity:
  pass `replay_model::SENTINEL_REPLAY_REQUEST_PREFIX` itself, never a copy.
- Bootstrap test helpers (`CountListReads`, `HoldNextList`, `HoldNextLedgerCommit`,
  `InterposeLedgerReads`, `HoldPoint`) live in `tests/sentinel_replay_bootstrap_retry_test.cpp`:
  the replay-retention/replay-store porter extends that file, and ports
  `tests/helpers/sentinel-kv-stub.ts`.
- The paired comparison with the TypeScript gateway lives at [commit `ca045a83ebaab6c50c20567d1a825638abac0891`](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891/cpp/gateway/conformance).
- More GCC 12.2 forms that fail inside `co_await`: a string literal bound to a
  `const std::string&` parameter; coroutines taking aggregates by value need a non-coroutine
  wrapper taking `const&`.

## Network client (rt::HttpClientTransport)

- Nothing installs it implicitly: tests keep using `MockTransport` / `GlobalFetchOverride` /
  `ScopedLoopbackFetch`; a test that forgets to mock fails with a logic_error. Network errors
  are `rt::FetchError` (`uos/rt/fetch_transport.hpp`); `error.code` is `rt::JsError::code`.
- Build hazard: after replacing a source file, a compile of the OLD file that finishes after
  the rename leaves a newer but stale object. `touch` the file again after renaming.

## Cluster sub-chunk b2 (surplus*, metered, metered-quota)

- START FETCHES AT THE JS CALL POINT: `rt::Promise<rt::Response> p(fetcher(url, init));
co_await p.wait();`. A bare `rt::Fetch` call returns a lazy Task whose body would only run
  at the await.
- Async hooks: `await hook()` is `rt::Promise(hook()).wait()`; JS `await dispatch?.x()` with no
  dispatch is `co_await rt::microtask()`. `x === undefined ? await getKv() : x`: await only in
  the undefined branch (no extra turn otherwise).
- HAZARD (two crashes): a range-for over a sub-object of a temporary dangles
  (`for (x : parse(...).as_array())`, `rt::objectEntries(parse(...).as_object())`). Bind the
  temporary to a local first.
- `new Error(m, { cause })` with a non-Error cause: `metered::ErrorWithCause`
  (`rt::Reason causeValue`); `codex::auth_refresh` has the same shape. Reuse one of them.
- The agent environment has `OPENLUX_API_KEY` (and other provider keys) set: tests assuming no
  provider key clear them. Reset module caches between cases
  (`resetMeteredModelsCacheForTest()`, `resetSurplusModelsCacheForTest()`).
- `storedView` copies differ: the one in metered_quota.cpp drops undefined members, the one in
  runtime_config.cpp maps them to `{}`. For "=== undefined" guards use the dropping form.

## Cluster sub-chunk b1 (api-key-policy, native-api-key-policy, paid-fallback index + ledgers, local-development-key)

- Background backfill: the first `admitPaidFallbackV3` in a process (direct or via
  `reservePaidFallback`) schedules it. Any ASYNC_TEST that admits must await
  `ledger_backfill::scheduledPaidFallbackBackfillForTest()` before its loop ends, and call
  `setPaidFallbackTerminalSweepForTest({})` then later `(nullptr)`.
- Reservation closures: state in structs behind `shared_ptr`; bodies are free coroutine
  functions with by-value parameters called from the `std::function` members.
- Bun differentials for `#kv`-importing modules: a preload using `Bun.plugin` to stub
  `@deno/kv`, the TS `CountingKv` via `setKvForTest`, fixed `Date.now`, rows recorded sorted by
  encoded key: this replays exactly against the C++ CountingKv.
- `tests/paid_fallback_index_local_development_key_differential_test.cpp` defines a test-local
  `loadFullCodexModelsSnapshot` stand-in: remove it once `src/models/codex_models_fetch.cpp`
  is LINKABLE (integration phase).
- `tests/metered_quota_coverage_test.cpp` has a `MeteredKv` fake on `rt::Kv`.

## Cluster sub-chunk b5 (codex auth, native-auth, routing: capacity/mutations/evaluation/serial, account-routing, auth-refresh)

- CREDENTIAL ISOLATION (MANDATORY): any test or bun oracle script that touches codex auth,
  native auth, dispatch, or anything that can reach them MUST clear `HOME`, `CODEX_HOME` and
  `CODEX_AUTH_JSON_B64` (`rt::env::ScopedOverride`; unset in the environment for bun) and
  clear `config::config.codexAuthJsonB64` for its scope. Otherwise `native_auth` reads the
  real `~/.codex/auth.json` of this host, which holds live secrets. Never print credentials.
- `withCodexAdmission<T>` is instantiated for RouteSelection and bool,
  `awaitWithoutCancellingSharedWork<T>` for CodexAuthState: a new T needs an explicit
  instantiation in the .cpp plus an `extern template` line in the header.
- `CodexQuotaResetReconciliationFence::isCurrent` receives the row as JSON; an empty
  `std::function` means "not a function" and fails closed.
- Test hooks (`NativeAuthHooks.readAuth/refresh`) are non-coroutine lambdas returning
  `rt::fulfilledTask(...)`.
- `tests/helpers/codex_diff_kv` (deterministic `DiffKv`, UUID masking, `routingAccountJson`)
  is reusable for codex differential tests. ScriptedKv ports live in
  `tests/codex_auth_coverage_test.cpp` and `tests/codex_capacity_routing_coverage_test.cpp`.

## Cluster sub-chunk b8 (lithos, lithos-rate-limits, cerebras, deepseek/index)

- `deepseek::index::projectDeepSeekReasoningEffort` returns `std::optional<json::value>`
  (nullopt stands for the inherited `Object` function a plain-object lookup returns in JS).
  Plain-object lookups keyed by untrusted strings (`MAP[key] ?? x`) must consider
  "constructor"/"**proto**": record what Bun does and model it.
- Large differential suites may store vectors as base64 of gzip JSON in the `.inc`, decoded
  with `rt::fromBase64` + `rt::gzip::gunzipSync`.
- Test idiom: `rejectionOf(task)` returns the rejection reason; `EXPECT_REJECTS_REASON` checks
  reason identity.
- rt hazard: a range-for over `std::vector<rt::Nullable<std::string>>` can fail to compile
  (recursive constraint in Nullable's `operator==`): use an index loop.

## Durable KV (rt::SqliteKv) is in

- `storage::kv::openKv` / `kv::getKv` open the durable SQLite store; the guard forwards
  `enqueue`. The queue capability is `rt::KvEnqueue` (kv.hpp); `ledger_state::KvEnqueue` is an
  alias. `rt::MemoryKv` does not implement it.
- Tests that need a real `openKv`: `tests/helpers/linked_native_kv.hpp` (`prepareOnce()`), with
  the database under `TMPDIR`, never named `kv*.sqlite3`. NEVER run a test or tool from a
  directory that has a real `.data/kv-bun.sqlite3` (opening writes).
- `storage::kv::deserialize()` works (JavaScriptCore v14 reader); values KvValue cannot hold
  throw TypeError.
- Regenerate `tests/kv_durable_suite_test.cpp` when store cases in `tests/rt/kv_test.cpp` change.

## Cluster sub-chunk b3 (claude-code credentials/accounts/quota/usage/models, claude capacity rollups/history)

- Tests reaching the Claude `process.env` fallback clear `CLAUDE_API_KEY`,
  `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_WORKSPACE_ID` (set in this environment); bun
  generators run under `env -u ...`. Reset caches: `resetClaudeCodeCredentialCacheForTest()`,
  `resetClaudeCodeModelsCacheForTest()`.
- A test registering its own Claude observation recorder disposes it and must not link
  `src/provider/capacity.cpp`.
- Differential pattern for fetch-driven modules: `ScriptedFetch` (recording `rt::Fetch` that
  can hang), counter-based random source (`randomUUID` gives `00000000-0000-4000-8000-<n>`),
  bun fake KV with Deno key order (see `tests/claude_code_differential_test.cpp`).
- Where stored rows must be written back byte-compatibly, keep the raw stored value alongside
  the typed view (see `StoredBucket` in claude_capacity_history.cpp).
- `tests/claude_usage_meters_test.cpp`: one case still deferred (needs `openKv(path)`, now
  available via `tests/helpers/linked_native_kv.hpp`, and capacity.ts `claudeCapacitySource`).

## Cluster sub-chunk b6 (codex reset-settings, banked-reset, -claim, -submission, -pool)

- `tests/helpers/codex_banked_reset_harness` ports codex-banked-reset-harness.ts (harness
  `MemoryKv` with `beforeGet`/`beforeAtomicCommit`/`failCommitNumbers`,
  `FakeCodexUsageResetProvider`, `Deferred<T>`, `TestClock`, `candidate`, `config`,
  `dependencies`, `fullPool`, `inventory`, event generator + invariants): the banked-reset
  phase-C tests and codex-manual-reset tests reuse it.
- Helper .cpp files cannot use `rt::test::report` (test.hpp defines `main`): harness
  assertions throw an `rt::JsError` named "AssertionError".
- No `co_return` (or `co_await`) inside a catch handler: set a flag, act after the handler.
- Differential recipe for concurrency: record under bun with a `beforeGet` hook logging
  `[getNumber, encodeKey(key)]`; the read order pins the turn interleaving.
- `tests/codex_reset_settings_migration_test.cpp` has 8 of 9 cases: the kv-migration porter
  extends it with the validator case.

## Cluster sub-chunk b4 (provider capacity-contract/parse/analytics/events/history/capacity, presentation, selection, health)

- Any binary linking `capacity_events.cpp` (hence `capacity_history.cpp`) also needs
  `capacity.cpp` or a test-local stand-in for `capacity::sampleProviderCapacityOnEvent`.
  Linking `capacity.cpp` installs the Claude capacity observation recorder at static init.
- The JSON conversions declared in `capacity_events.hpp` are DEFINED in `capacity_contract.cpp`.
- Between tests: `resetProviderHealthThrottleForTest()`, `resetProviderSelectionCacheForTest()`.
  The default sample trigger's 15-minute debounce state persists for the life of a binary.
- For scoped fixture homes in C++, use `rt::env::ScopedEnvironment({{"HOME", "/nonexistent/..."}})`.
- HAZARD: a `ForwardingKv` subclass that OWNS its inner store must create it in an earlier
  base class (see `OwnedInnerKv` in tests/provider_capacity_differential_support.hpp);
  `ForwardingKv(*member)` binds garbage because members are built after bases.
- Oracle fake KVs must list keys in Deno KV order and support `{start, end}` selectors.
- Fixtures: avoid `sk-ant-...`-shaped strings (gitleaks runs pre-commit).

## Cluster sub-chunk b9 (openai-telemetry, request-policy, deepseek/responses)

- `ChatOnlyResponsesProfile::projectReasoningEffort` returns `rt::Nullable<json::value>`: null
  is "tier not accepted"; otherwise `rt::objectSet(body, "reasoning_effort", v)` (undefined
  removes the member).
- Telemetry recorders take `const UsageContext*` (pass `&ctx`); state is a
  `shared_ptr<ResponseTelemetryState>` shared by the WeakMap and the context.
- `LITHOS_PROVIDER_HINT()` is an accessor; the three profiles are `constinit`.
- Tests over `ResponseInputItem`: keep an item typed only when `value_from(typed)` deep-equals
  the raw item; otherwise pass the record.
- `performance.now` has no rt override: in replays keep the fake clock at 0 and move the
  start value instead.
- HAZARD (again): references into `json::value(staticObject)` copies and range-for over
  `decode(...).as_array()` dangle.

## rt additions, batch two (2026-11-16 10:55Z): USE THESE, do not write new local helpers

- `uos/rt/kv_js.hpp` (table in `docs/rt/kv.md`, "Stored values seen from JavaScript"):
  `rt::storedJsonView(kvValue[, options])` (JSON view of a stored value for `unknown` guards;
  options choose how undefined members/elements/holes/bigint map), `rt::kvTypeof`,
  `rt::kvIsObject` (utils-style isRecord, arrays included), `rt::kvIsRecord`,
  `rt::kvValueFromKey`, `rt::kvKeyFromValue`, `rt::toKvKeyPart` (SDK error text),
  `rt::kvKeyStrictEquals`, `rt::kvIsUint8Array`, `rt::kvToString`, `rt::kvJsonStringify`.
- `new Error(m, { cause })` with any cause: `rt::withCause(rt::JsError(m), reason)`; read with
  `rt::errorCause(e)`. HAZARD: inside a JsError subclass member function a parameter or local
  named `causeValue` (or `code`) fails `-Wshadow`.
- `Promise.race([reader.read(), abortPromise])`: use `reader.readPromise()` /
  `cancelPromise()` (same turns as Bun); no compensation needed.
- `rt::get(value, 0)` now reads index 0 (the earlier FOOTGUN note is retired); range-for over
  `std::vector<rt::Nullable<T>>` now compiles (the b8 hazard is retired).
- `rt::Headers` keeps each name's first spelling for the wire; the fetch transport sends caller spelling like Bun. The final migration fixes appended Cookie values to use "; " in `get("cookie")`, iteration and the wire; ordinary headers retain ", ".
- The exact local-helper inventory in /tmp/claude-1002/wave/rt-misc2/MIGRATION.md was migrated on 2026-11-17. Preserve each caller's storedJsonView options and object/array semantics; the accepted migration report records deliberate unchanged helpers.

## Cluster sub-chunk b7 (codex dispatch, responses state/operations/runners, experiment, codex-models-fetch, codex/index)

- LINK CLOSURE: anything that reaches `codex/dispatch` needs the 75-source `// Links:` list of
  `tests/codex_dispatch_differential_test.cpp` (capacity_events -> capacity ->
  codex/experiment closes a cycle); responses/index tests need the 82-source list of
  `tests/codex_responses_differential_test.cpp`. Copy those lists.
- `CodexResponseContext`: installed `std::function` members capture a `std::weak_ptr` and lock
  per call; operation functions take `std::shared_ptr`. `CodexCredentialAttempt` is passed as
  `std::shared_ptr` wherever the TS mutates it.
- Dispatch coordinator `claim()` / `cancelBeforeTransport()` return Tasks waiting on an
  already-running promise: `co_await` them directly (wrapping adds a turn).
- A helper that only forwards another module's Task must be a plain function, not a coroutine
  (a coroutine forwarder adds turns).
- `tests/codex_retry_after_test.cpp` and `tests/codex_account_email_test.cpp` are already
  fully ported.

## Wave 5 chunk 4 (harmony types/render/conversation/classifier/parse, tools/schemas, reliability context/feedback/surfaces, codex supervisor-brief, auth-repair)

- `harmony::types`: `ConversationTurn` and `HarmonyTurn` are variants of
  `ConversationTurn<Role>` / `HarmonyTurn<Kind>` structs; `HarmonyAdapterError` has its own
  `code` and sets `JsError::code`. `context::ContextBudgetKind` is an enum class
  `{short_, medium, large}`.
- `TOOL_SCHEMAS` is `inline const authority::contracts::Record<CanonicalToolSchema>`.
  `validateToolArguments` returns a variant (`...Valid` / `...Invalid`). ROUTER PORTER: the TS
  calls it before its `try`, so the Object.prototype-name TypeError must escape `runTool`.
- Object.prototype key names (`constructor`, `__proto__`, ...) and `key in plainObject` have no
  rt helper yet: local copies exist in harmony/tools/schemas.cpp, harmony/reliability/
  feedback.cpp, request_policy.cpp, auth/passkeys.cpp. Reuse one; report the need.
- The handler/index porter calls `handleAdminCodexSupervisorBrief(rt::Request)`.
- Placeholders to replace when probes.ts/adapter.ts land: `WEATHER_TOOL`, `ANSWER_SCHEMA` in
  `tests/harmony_render_test.cpp`; deferred parts are named in the headers of harmony_render,
  harmony_tools_schemas, codex_supervisor_brief and codex_external_native_auth tests.
- Timing bounds from TS tests (e.g. "< 2000 ms") can miss at -O0: keep the bound and add a
  documented scaling fallback, as `codex_supervisor_brief_test` does.

## Cluster integration C7 (native resume, 2026-11-16)

- Paid-fallback and v3-cutover ports retain all 27 TypeScript cases, now freshly verified. Preserve named Tasks, microtask-aware KV fakes, credential isolation, and settling scheduled backfill before each test loop closes.
- `loadStrictKeyRecord` intentionally accepts policy-valid legacy rows without unrelated ApiKeyRecord fields and preserves raw `window_ms` stringification, matching the TypeScript cast. The existing phase-B index differential test is owned by C0 until its stand-in replacement finishes.

## Wave 5 chunk 8 (migration guards/projections, DeepSeek stream, capacity sync)

- Migration guards and `StrictApiKeyPair` use `shared_ptr<rt::Kv>` and raw `KvValue` rows; preserve extra stored members through `KvObject` spreads. Import options inherit classify options and use nested aggregate initialization. Local stored-value equality/number coercion helpers preserve array-accepting `isRecord`, state-string coercion, and dedicated-ledger precedence.
- `codex_manual_reset_state_test` retains one deferred consume/export integration for w05c7; `oss_stream_lifecycle_test` retains eight provider/Lithos relay cases for w07c2. Raw DeepSeek SSE behavior is separately pinned by 18 Bun-recorded traces. No shared runtime edits or dependency stubs were added.

## Wave 5 chunk 5 (chat translation/completions, hybrid, live calls/sideband)

- Accumulator calls use shared pointers to preserve mutable call identity. `PreparedResponsesStream` requires an explicit iterator. `rt::getString` borrows; copy strings that must outlive their JSON.
- Preserve the TypeScript slash-excluding path scanner and position-dependent refusal duplication. Array-to-object spread has a small local conversion. The shared live-relay test helper uses synthetic credentials, memory KV, mocked providers and ephemeral loopback sockets.
- Remaining test headers name eight API-key assertions, compaction/adapter/waterfall integration, and full handler auth/CORS/request-ID/raw-TCP fixtures; later final-module owners extend those files.

## Wave 5 chunk 1 (Sentinel retention/store/admin, raw stored-member fidelity)

- Decode stored accounting/ledger rows through retention-schema helpers; write with `sentinelReplayAccountingValue` and `sentinelReplayLedgerValue`. Backing rows preserve opaque fields, typed values and property order across staging, publication, cleanup and ledger rewrites. Strict DTO codecs intentionally omit backing metadata.
- Incident mutations preserve the raw outer object and nested provenance; converting the full stored row to JSON rejects valid byte/BigInt extras and typed DTO rewrites silently drop them. Fresh staging/incident regressions pin both failures and repairs.
- Handler/terminal-log/images dependent cases remain in existing test headers; historical TypeScript archive subprocess comparison is still deferred. w05c2 owns replacement of the documented OpenRouter route literal after this acceptance.

## Wave 5 chunk 6 (scope targets, DeepSeek payload/projection/stream/aliases, Lithos models)

- `PROMPT_ADMISSION_PROFILE_WINDOWS()` is an accessor to avoid static initialization dependencies. Stream translator copies share callback-owned state. The wave retained the existing Flash date and classifier results.
- Keep KV selection branches separate from `co_await`: a shared_ptr ternary across await caused GCC12.2 ownership corruption; the scope-target loader now has an explicit-branch lifetime regression.
- Fourteen suites pin complete owned API outputs and argument mutations. Handler/Codex assembly assertions, upstream-wire predicates and scope-experiment harness integration remain named in test headers for their final-module owners.

## Wave 5 chunk 2 (scope experiment model, OpenRouter, Claude access)

- OpenRouter options are `OpenRouterDispatchInit` and `OpenRouterDecisionInput`; `apiKey` uses `rt::Nullable<std::string>`. Scope results are `ReadSampleResultSample` / `ReadSampleResultInconclusive`; stored guards preserve optional versus present undefined.
- The source fixture recorded that an empty first unknown cache_control key suppresses unknown-key rejection. Use explicit vectors for multi-variable `ScopedEnvironment` fixtures to avoid nested-brace string-constructor ambiguity. The documented localeCompare Unicode limits remain.
- The Sentinel OpenRouter literal is replaced with the real route; capture and zero-attempt quota assertions pass on both wires. Higher handler/catalog/admin and complete replay-export cases remain deferred in test headers.

## Wave 5 chunk 3 (auth/admin, native authority contracts and RPC)

- `NativeReply` is wire JSON and preserves extra fields. Contract headers use the authority header to resolve their import cycle; missing dependency DTO codecs stay translation-unit-local in native_authority_contracts.cpp.
- Legacy API-key listing reads raw rows before source coercion; strict ApiKeyRecord decoding wrongly rejects legacy string window/expiry and absent paid fields. Two Bun-recorded golden cases pin that behavior.
- Native authority/SQL engine, admin mutations and router/inference-budget cases remain deferred in existing test headers. The passkeys helper now exposes source auth functions/constants without changing its existing API.

## Wave 6 chunk 5 (Harmony tool router)

- Tool validation precedes the catch; inherited Object.prototype-name TypeErrors escape. searchLineLimit only applies to filesystem search; source clipping omissions remain. Named Tasks/Promises preserve shell lifetimes and Bun microtask order.
- A thrown JSON object with a non-callable own toString must throw TypeError("No default value"); the router handles this locally because the generic runtime helper differs.

## Wave 5 chunk 7 (quota projection, manual reset, promotion, Claude refresh)

- Reuse tests/helpers/manual_reset_harness; preserve raw manual-intent property order in JSON-stringified fences. Use named Tasks for aggregate coroutine arguments and explicit vectors for multi-entry ScopedEnvironment fixtures.
- Real consume/export integration now completes all four manual-state cases. Three admin/kernel route cases, registered-route admin auth and the full scope-experiment harness await their final modules; catalog promotion has thirteen isolated source-recorded scenarios.

## Cluster integration C1 and C3 (routing and auth-cache harnesses)

- Reuse tests/helpers/codex_account_routing_harness and codex_auth_cache_harness. Preserve source Promise interleaving, strong KV snapshots/versions, credential isolation and named coroutine arguments. Routing selection seeding starts hashes with Promise.all semantics, not sequential awaits.
- Auth-cache fixtures with an otherwise unreferenced body-read deadline keep a referenced timer until cancellation; this only keeps the test event loop alive. Helpers are stable for C2/C4 and no core module fixes were required.

## Wave 6 chunk 2 (Harmony adapter/probes/scenarios)

- HarmonyTransport preserves callable identity and accepts one-argument fakes or optional request options. ProbeTurnOptions is the Omit<HarmonyRequestOptions, turns> shape. probes::toJson serializes sanitized records/results; ProbeTurnRun has no serializer.
- JSON vectors erase negative zero; check its sign separately. Preserve raw error extraction/native parsed-call reporting. Reliability-harness integration awaits w07c3; raw JavaScript BigInt roles remain outside the existing JSON carrier.

## Cluster integration C0 (whole-cluster validation)

- All58 cluster modules link with63 dependencies. Source/header static initialization audit and Bun-recorded profile/tier/hint/URL values passed; link/static5 cases and recorder-absence1 case pin registration behavior. Both model-snapshot and sampler stand-ins are removed; real quota constants/reset hooks and five previously deferred rollup/Claude reopen cases are used.
- Full phase-B55 binaries passed547 cases against456 consumed inputs. Focused C0 counts overlap this set. Direct unit manifests may carry a non-executed run-tests.sh annotation; actual build inputs/argv determine reuse, and original records are retained. The remaining rollup router-authentication case belongs to the handler/index porter.

## Wave 6 chunks 1 and 6 (key mutations and migration coordinator)

- Keep raw KV backing rows alongside typed key-policy views; preserve opaque fields, numeric-string coercion and deletion-owner CAS. Construct dependency-derived test keys after startup. Local-development cases use memory CAS, not a native-durability claim.
- Migration preserves raw spreads and concurrent strong pair reads. Its local catalog adapter observes inherited Error.name; a refused final marker/config commit still returns source-compatible success. The scripts/kv-migrate.ts HTTP CLI case remains outside the module manifest.

## Cluster C5 (unknown banked-reset verification)

- CodexUsageResetProvider::verifyApplied returns Task<json::value>. Strict true succeeds, boolean false is verification_not_applied, and nonboolean results retain durable unknown/verification_response_invalid. Do not narrow this boundary back to bool. Null represents absent unknown because the source does not serialize or distinguish those invalid raw values.
- Provider, banked-reset and auth-cache helper signatures were migrated coherently; original snapshots and exact diff are retained. Nested throwing JavaScript contract getters remain outside the JSON carrier and are explicitly recorded in the test header.

## Wave 6 chunks 3 and 8 (scope experiments and live routing)

- Reuse prompt_cache_scope_experiment_harness, resetting provider-health throttle between independent oracle scenarios. Target results are ScopeTargetResolutionResolved/Inconclusive. Preserve durable fences and the source between-cycle auth-pool revision failure. Admin/catalog projection and validation remain for w07c4.
- live/handler reuses live_relay_harness and named Promises for await turns; handleLiveUpgrade(req, info) preserves prepared transport descriptors. Its24 source snapshots mock auth/call/sideband boundaries; full server integration remains for the final handler owner.

## Wave 6 chunk 4 (native authority module family)

- SQL read/page return raw JSON; readAs<T>/pageAs<T> require DTO codecs. Members namespace_ and epoch retain source identity; deletion is delete_. Lifetime updates overlay raw rows, and NativeReply remains wire JSON.
- Exactly six private codec fields were corrected to required-null handling after auditing71 reader/writer pairs: account/blocked-account probeGeneration/probeToken and quota-blocked poolSnapshotJson/capacitySnapshotJson. Do not replace required nulls with omitted properties.
- Reset election preserves the extra source routing write caused by raw-versus-parsed slot identity. Principals use a small local weak-map bridge for generic source-object identity. Full native_authority.ts assembly belongs to w07c5.

## Wave 7 chunk 3 (Harmony reliability harness)

- Reuse declared HarnessEvent variants and actual Harmony exports. The source terminal invalid-streak state trails its final observation; cancellation leaves abandoned bodies uncancelled and waits through active backoff. rt::race has one value type; a private exception sentinel avoids an added mapping turn.

## Wave 6 chunk 7 (waterfall, normalization, wire, health, whitelist)

- NormalizationResultOk<T>/NormalizationResultNotOk carry normalization outcomes; catalogues use json::array. Header helpers return rt::Headers. upstream_wire::ErrorWithStatus carries generic JS Error.status for later Voyage diagnostics.
- Nested JSON reference helpers require a json::object overload, otherwise implicit json::value temporaries dangle. Keep original source stress assertions but bound oracle recording; repeated-identity graphs must not be recursively materialized. Higher handler/catalogue/body assembly cases remain named in test headers.

## Wave 7 chunk 6 (Claude admin)

- Refresh is passive: supplied fetcher/clock options do not initiate a probe. Preserve array-accepting record checks, UTF-16 limits,429 acceptance and malformed200 unvalidated storage. All12 callable source cases plus prior persistence and grouped branch observations pass; handler/index auth/path wiring remains for final ingress.

## Wave 7 chunk 2 (provider stream relays)

- ProviderStreamIterator has callable next/return_, implicitly wraps rt generators, preserves shared lifetime and adds no await turns. ProviderStreamSource is Response/Promise<Response>; frame options carry signal; LithosServedModel accepts a string/getter. OpenRouter uses readPromise/cancelPromise. The MAX_BYTES frame bound actually counts UTF-16 source units.
- Lifecycle source398 needs Lithos handlers, responses-handler and chat/envelope; finish at the last available owner. Seventeen OpenRouter handler cases remain in the test header. Inherited Lithos read-race compensation stays with final rt-misc2 migration.

## Wave 7 chunk 7 (SystemOne and compaction)

- Preserve array-accepting isRecord; local entriesOf provides array/object entries. Compaction keeps shared asker ownership, nullable credential seams and non-coroutine entry wrappers. Stock handler compaction remains unchanged. Four ingress quota cases and SystemOne route registration remain for w10c1; tools/context-replay source-spy cases stay outside this module manifest.

## Wave 7 chunk 1 (paid health and Claude route)

- Preserve exact full-pool proofs, named coroutine arguments, Promise interleaving and one admission lifetime. The bounded route loop retains the source cap.
- Custom observer fixtures use an explicit plain-function bridge for callback identity and scoped exact-recorder restoration. Arbitrary captured std::function callback disposal is not claimed fixed; the only production registration uses the supported capacity callback pointer. Thirteen paid/waterfall handler cases remain named for later modules.

## Wave 7 chunk 5 (NativeAuthority assembly)

- NativeValidatedRequest.rawJson retains the complete trusted verifier input from the same checked read and is not a wire field. save(row, rawOverrides) preserves source spreads; account and owner replacements remain explicit. Shared control-source identity uses the shared_ptr constructor overload.
- Original reset-election data retains quotaResetAtMs/opaque account and owner members through verifier input, proof consumption, receipt refresh and later saves; no extra read/write is introduced. The six required-null codecs and source extra routing write remain.
- Only the proven-nondeterministic concurrent WebCrypto fixture maps generated opaque revisions injectively with CAS/semantic/write-count negative controls; ordinary cases compare exact state. Real three-channel RPC fixtures passed against task SQLite.

## Wave 7 chunk 4 (catalog/model/admin assembly)

- Catalog assembly uses JSON arrays; public snapshots are typed records. Preserve bare versus versioned model contracts, all advertised tier strings, whitelist/provider filtering, SWR generation/lease fences and scope status/redaction behavior. Stage0 nullish readers return503/no-store.
- Fully qualify core defaults/codex namespaces across admin siblings. Bind aggregate coroutine calls to named Tasks; original Bun files run in separate processes to avoid fixture cache coupling. Omitted-revision and immutable release constants are narrowly test-local adapters, not production schema/stamping changes.
- Main104 plus delegated29/47/19 cases passed199 total with exact current inputs. Handler ingress/admission and exact JS clone monkeypatch instrumentation remain explicit boundaries for later owners.

## Wave 8 cycle c1 (OpenAI and provider handlers)

- Handlers take Request, json::object and model by value, plus const UsageContext*. OpenAI key sets use OrderedSet<string>. Lithos Responses has fifth-argument HandleLithosResponsesOptions; OpenRouter has fifth-argument OpenRouterHandlerDeps.
- All six implementations link against real dependencies; no stand-ins. Predecessor compaction/web-tools/tier-window cases remain, including real ephemeral HTTP tier windows. c2 and final handler/envelope integration stay with their owners.

## Fixture isolation follow-up

- Capture wrapper TMPDIR before ScopedEnvironment clears the runtime environment; retain it alongside credential-empty HOME. Assert actual native artifact/database paths remain under task scratch. Accounts, header-meter and runtime-conversion fixtures now do so; all eight linked-native callers were audited, five already preserved scratch. Isolated first-use validation prevents cached initialization from hiding an escape.

## Wave 8 cycle c2 (response request/attempt/handler pipeline)

- Async aggregate inputs own their values; await results and cleanup Tasks in named statements. Public dispatch callback typedefs may borrow only through plain forwarding functions into owning coroutines. All actual callers were audited, not inferred.
- Delivery state retains context copies sharing telemetry; event identity tokens and array-accepting utils::isRecord remain. Buffered accumulation uses a local adapter for array-valued records. Final shared lifecycle case belongs to chat/envelope; all seven c2 module implementations link without stand-ins.

## Wave 9 chunk 5 (Decisions and normalized usage)

- Concrete JSON null is not the nullptr/undefined sentinel. Decisions and the proven SystemOne sibling site now forward normalized null accurately; source missing/null/malformed telemetry regressions pass and other caller sites were audited.
- Preserve pre-aborted Decisions dispatch returning502 without health observation. A local array-key helper implements Object.keys(array). Final gateway ingress remains w10c1.

## Wave 8 chunk 2 (admin assembly)

- Reuse completed admin/KV-budget helpers. runtime(), now() and kernelTestKeyPair() are accessors. Preserve named Tasks and sparse stored-key array semantics; omitted-undefined fixture quirks are not production schema changes. Three projection cases are complete; true ingress and named KV-budget assembly exports remain for w10c1.

## Wave 9 chunk 2 (embeddings/Voyage/jobs)

- Voyage retry errors extend upstream_wire::ErrorWithStatus. Job rewrites retain opaque backing via jobRaw. Float32 encoding canonicalizes both NaN signs like Bun. Empty inputs and empty-first-unknown-key quirks remain. Native env cases prove absent HOME; permission-refusal swallowing has source oracle coverage only. Five handler/index ingress cases remain explicitly named in part2.

## Wave 9 chunks 1 and 3 (images and terminal logging)

- Images use owning context copies, shared telemetry/raw-body state and named Promises with the real Responses handler. This wave recorded empty-first-unknown-field behavior, the unnamed-file TypeError and the Codex-only fallback policy. Real file-limit refusals pass; JS prototype read-count instrumentation and inherited IDNA limits remain explicit.
- Terminal wrappers return rt::Promise<rt::Response>; await .wait(). Callback seams start synchronously and may accept Task-returning lambdas. Heterogeneous discarded allSettled values use a local adapter. Eventual telemetry retention waits bounded queue flush where native crypto timing differs; original nonblocking/cancellation semantics remain separately checked. True final handler ingress stays with w10c1.

## Wave 9 chunk 4 (chat envelope and deferred Lithos lifetime)

- Deferred dispatch owns its UsageContext copy while sharing telemetry state; a borrowed pointer into a returned handler frame caused a proven SIGSEGV and is fixed in the private Lithos input. Reread telemetry snapshots after cancellation. Exercise real deadline signals via FakeClock rather than caller-abort substitution.
- All assigned Part11/lifecycle/cache/Lithos originals pass, including the formerly misclassified callable attestation case and real deferred-wait lifecycle. Duplicate paid-buffered source output remains preserved. Final openai_compat_harness ownership may transfer after this acceptance.

## Final handler/bootstrap source and ingress acceptance

- All six final pairs are implemented and runtime-validated through the six disjoint groups. UOS_ROUTES uses inert inline callbacks; delivery types remain the existing defining contracts. serve::initializeModule explicitly starts the former import migration; serve/smoke importMetaMain boundaries remain uninvoked unless an authorized launcher calls them.
- Smoke pure helpers are tested in standalone real-implementation TUs; only the include-scoped GCC12.2 subobject-linkage diagnostic exception applies there. Production sources remain strict/warning-free. Full smoke mains, installed Codex, live Jev and production activation remain unproved/outside scope.
- Full-handler test links use measured linker-only3GiB, both compiler slots exclusively and6GiB minimum MemAvailable after locking; ordinary compiler2GiB unchanged. Capture::setObserver is synchronous after storage/before forwarding, preserves class layout/default behavior, and supports event journaling without polling.
- Sparse provider usage projection keeps raw ordered JSON fields and source arithmetic; local metered/surplus projection fields are JSON wire objects rather than global DTO changes. The original malformed fake revision adapter remains case-local; real null/CAS semantics are unchanged.

## Final local-helper migration (2026-11-17)

- Replaced 18 stored-view variants and 17 object predicates with exact RT options; every predicate retains array acceptance. Error causes use withCause/errorCause; read races use readPromise without compensation turns; key conversion, stringify, element/join, fulfilled-task, WeakMap and SQLite adapter helpers follow the scoped migration inventory.
- Cookie append/get/iteration/forEach now use semicolon-space; ordinary and Set-Cookie semantics, wire spelling/backing and class layout remain. Seven isolated Bun scenarios and four native cases cover this correction.
- Preserve explicit per-caller storedJsonView undefined/hole/top-level policies. All 1145 required/null/undefined codec lines, raw row backing, opaque members, verification carriers, settlement ownership and deferred lifetimes are unchanged. Borrowed-string narrowing, optional CodexError/ResponsesStreamError carriers and the unlisted reset_settings stored helper deliberately remain.
- Validation: 61 warning-fatal changed-source checks, 83 affected suites/944 cases, and an independent parent32-case check passed. Two historical link failures were corrected only in Links metadata and rerun; evidence is /tmp/claude-1002/wave/rt-misc2-final. The lifecycle declaration fills the resolver's current 40-line header window; extend registry metadata instead of silently overflowing it. Full final sweep remains a separate receipt.

## Final complete portable sweep (2026-11-17)

- All 325 module pairs and unique PORTED entries are present. The frozen Linux/AArch64 GCC 12.2 -O0 fixture sweep selected all 436 targets, including three cluster targets; final current results are 436 PASS and 4995 passing cases, with zero remaining failure, timeout or skip.
- The original 05:16:35-06:43:14 UTC run passed 432 targets/4931 cases and exposed four stale link declarations. Registry-only additions preserved all original/static dependencies for admin-index (88 to126 sources), Lithos wiring (174 to188), OpenAI Part11 (185 to204), and public cache controls (168 to188); exact four-target reruns passed64 cases, with no C++ source or assertion edits. Original failures remain in the history and archived logs.
- When a shared harness starts referencing complete handler routes, update each affected link list with proven defining sources and preserve static-registration dependencies. Global registry changes can retain an unaffected receipt only when its ordered effective source argv, metadata and every consumed hash remain identical. Final reconciliation checked94970 hash observations across1355 unique inputs, plus unchanged runtime data, static assets and framing fixtures.
- Evidence: /tmp/claude-1002/wave/final-sweep/reconciliation/terminal-evidence.json (SHA2567f44a928650011a182f1547942735d89a6eae0c421415245d7d7ca310114618d); parent handoff: /tmp/claude-1002/codex/round-final.json. Production main/build/release, real upstream/client acceptance, production-copy KV checks and performance qualification remain separate stages in OPEN-STAGES.md.
