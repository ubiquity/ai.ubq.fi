# TypeScript to C++ gateway port: rules for every porter

This tree (`cpp/gateway/`) ports the first-party TypeScript gateway (`src/`, `serve.ts`, `lib/jev_compaction/`) to C++20. The TypeScript and the gateway's fixtures define required application behavior. Use pinned standard or mature open-source libraries for generic runtime operations. The bounded pilot elsewhere under `cpp/` has its own qualification and ownership.

Runtime references, mandatory reading before porting: `docs/rt/values.md`, `text.md`,
`async.md`, `web.md`, `regex.md`, `kv.md`, `platform.md`, `record.md`, `websocket.md`, `sqlite.md` and `gzip.md` (when your modules use WebSocket, SQLite or gzip), and `docs/PORT-NOTES.md`
(current conventions and historical port notes). Use the `uos::rt` interfaces for shared runtime operations. This document governs new generic-library substitutions. [Replacement plan](docs/OSS-REPLACEMENT-PLAN.md)

`PORT-MANIFEST.json` lists each module, its imports, its direct tests and its
C++ paths. `tools/cmake-sources.py` checks the historical modules against
`PORTED.txt`, includes the native adapters and validates the complete source list.

## Host safety (this machine serves production)

- The live gateway runs on this host from another checkout. Never touch
  `.data/` of the repository root, `.env*`, any `kv*.sqlite3`, systemd units,
  ports 7999/8000, or any process you did not start. Never read secrets.
- Two cores, about 2 GiB of spare memory, about 2 GiB of spare disk. Compile
  **only** through `cpp/gateway/tools/` wrappers, which serialise compiler runs,
  lower their priority and cap their memory. Use native tests and recorded fixtures for application readback.
- No debug info, no sanitizer builds, no extra build directories. Keep scratch
  under the per-agent directory the wrappers give you and delete it when done.
- No downloads or package installs. No git commands that write (no commit,
  checkout, stash, reset, worktree). The orchestrator owns git.
- Stay inside the files you were assigned plus their tests. If a shared file
  (anything under `rt/`, `tools/`, `CMakeLists.txt`, this document) needs a
  change, describe it in your final report instead of editing it.

## Layout and naming

| TypeScript                     | C++                                                                     |
| ------------------------------ | ----------------------------------------------------------------------- |
| `src/a/b-c.ts`                 | `cpp/gateway/include/uos/a/b_c.hpp` + `cpp/gateway/src/a/b_c.cpp`       |
| `serve.ts`                     | `include/uos/serve.hpp` + `src/serve.cpp`                               |
| `lib/jev_compaction/x.ts`      | `include/uos/lib/jev_compaction/x.hpp` + `src/lib/jev_compaction/x.cpp` |
| `tests/foo.test.ts`            | `cpp/gateway/tests/foo_test.cpp`                                        |
| module scope of `src/a/b-c.ts` | `namespace uos::a::b_c` (top-level `src/x.ts` is `uos::x`)              |

- Path segments: `-` becomes `_`; a segment that is a C++ keyword gets a trailing
  `_`. Nothing else is renamed.
- **Keep TypeScript identifiers verbatim**: `camelCase` functions and variables,
  `PascalCase` types, `SCREAMING_CASE` constants, same parameter order. A reviewer
  must be able to grep a TS name and land on its C++ twin. A name that is a C++
  keyword gets a trailing `_`.
- Module-private TS helpers go in an anonymous namespace in the `.cpp`, in source
  order. Exported symbols are declared in the header, in source order.
- Keep one TS application module per header/impl pair. A TS `export { x } from "./y"` becomes a `using` declaration in the header. Generic library integration may replace the underlying implementation while preserving its application-facing API.
- Headers include what they use; `#pragma once`; no `using namespace` in headers.
- Use C++20 across the primary GCC 16.1 release profile, the retained GCC 12.2 comparison SDK and supported Clang/libc++ profiles. [Build profiles](docs/BUILD.md)

## GCC 12.2 coroutine miscompile (mandatory rule)

GCC 12.2 destroys temporaries twice when they live inside a `co_await` full-expression,
and mishandles a `co_await` result passed directly as a function argument. Both crash at
run time (double free), not at compile time. In every coroutine:

- Never build a temporary inside a `co_await` expression: no braced aggregates
  (`co_await kv.set(key, Options{...})`), no implicit conversions that create a
  `std::string`/`json::value`/`std::function`, no lambdas written inline as arguments.
  Bind each argument to a named local first, then `co_await f(a, b)`.
- A braced list that creates a `std::initializer_list` (vector, `json::object`,
  `KvValue::object({...})` literals) in a statement containing `co_await` fails to
  compile ("array used as initializer"). Build the value in the previous statement.
  `KvKey{...}` is the one safe exception (variadic constructor).
- Never use a `co_await` result directly as an argument or operand
  (`g(co_await f())`, `(co_await f()).value`). Write `auto r = co_await f();` and use `r`.
- Passing an aggregate BY VALUE to a coroutine inside a `co_await` expression also
  double-frees, even when the aggregate is an already-named local. Bind the Task first:
  `auto task = f(options); auto r = co_await std::move(task);`. Avoid range-for loops over
  a braced list whose body suspends.
- A member coroutine needs its object alive until it completes: never call one on a
  temporary (`co_await Client(x).call()`); hold the object in a named variable.

## Coroutine closure ownership

A capturing coroutine lambda stores its captures in the closure object, not in the coroutine frame. When a returned Task or Promise can outlive that object, use a free function or captureless coroutine with owning parameters passed by value. Shared mutable results and dependencies must be shared owners in each child frame; never borrow a handler-local container across an asynchronous suspension.

`rt::all`, `allSettled`, `race` and `any` retain the TypeScript cancellation contract: rejection or selection does not implicitly cancel siblings. A parent may finish while another child is pending, so that child must own everything it will access. Do not add cancellation, drain-before-rejection or another await to hide a lifetime defect. `rt::async` can own a closure but does not extend objects captured by reference; audit the referents as well as the closure. A member coroutine likewise needs an explicit owner for its object.

A noncoroutine callback that returns another Task has a different lifetime: verify that the callee owns its parameters and that any callback owner survives its actual asynchronous use. Classify each site from the reached ownership path; catching ordinary dependency errors alone is not an ownership proof. Add a post-fix functional regression for early rejection plus a pending sibling that later completes, and verify the fixed code with a bounded sanitizer gate.

## Type mapping

| TypeScript                                                                                                                                          | C++                                                                                                                                                                                                                                        |
| --------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `string`                                                                                                                                            | `std::string` holds UTF-8 or canonical WTF-8 where the storage contract requires it. Use the `uos::rt` UTF-16 helpers when required limits or offsets count code units.                                                                    |
| `number`                                                                                                                                            | `double` unless all paths prove an integer that fits `std::int64_t`. Use `uos::rt` formatting where callers require a defined wire or storage representation.                                                                              |
| `boolean`, `bigint`                                                                                                                                 | `bool`, `std::int64_t`/`rt::BigInt` as the range requires                                                                                                                                                                                  |
| `unknown`, `any`, `Record<string, unknown>`, parsed JSON                                                                                            | `boost::json::value` / `boost::json::object` (alias `uos::json`). Use the required ordering helper at the caller when a protocol observes member order.                                                                                    |
| `interface` / type alias of an object                                                                                                               | `struct` with the same field names and order, when the TS constructs and reads it as a typed value. If it is only ever a view over wire JSON, keep it as `json::object` and access by key.                                                 |
| `T \| undefined`, optional field                                                                                                                    | `std::optional<T>`                                                                                                                                                                                                                         |
| `T \| null`                                                                                                                                         | `std::optional<T>` only when `undefined` cannot also occur; when both occur and the TS distinguishes them, use `rt::Nullable<T>`                                                                                                           |
| string literal union                                                                                                                                | `enum class` plus `toString`/`parse` helpers when exhaustively switched; otherwise `std::string`                                                                                                                                           |
| discriminated union                                                                                                                                 | `std::variant` of structs, or a struct with a kind enum when variants share most fields                                                                                                                                                    |
| `T[]`, `Map`, `Set`                                                                                                                                 | `std::vector<T>`; `rt::OrderedMap`/`rt::OrderedSet` when iteration order is observable (it is insertion order in JS), otherwise `std::unordered_map`/`std::unordered_set`                                                                  |
| `Uint8Array`, `ArrayBuffer`                                                                                                                         | `std::vector<std::uint8_t>` / `std::span<const std::uint8_t>`                                                                                                                                                                              |
| `Promise<T>`, `async`                                                                                                                               | `rt::Task<T>` (`boost::asio::awaitable<T>`) for async functions and `rt::Promise<T>` for held promises. Preserve ordering of application side effects, cancellation and ownership across suspensions. [Async contracts](docs/rt/async.md)  |
| `AbortSignal`, timers, `Date.now()`, `crypto`, `fetch`, `Request`/`Response`/`Headers`, `ReadableStream`, `TextEncoder`, `URL`, env access, Deno KV | the matching `uos::rt` type. Never call the OS clock, random source, environment, sockets or filesystem directly from a ported module unless the TS module itself does through `node:*`; then use the `uos::rt` wrapper for that Node API. |
| `throw new Error(m)` / custom error classes                                                                                                         | Use `rt::JsError` carriers where callers read a type, code, cause or message. Map boundary failures to the ordinary error specified by the application or protocol.                                                                        |
| closures, callbacks                                                                                                                                 | `std::function` or templates; capture lifetimes explicitly (no dangling references across `co_await`)                                                                                                                                      |
| regular expressions                                                                                                                                 | Use `rt::Regex` with the selected library after qualification. Preserve the validation match and limits the caller requires.                                                                                                               |

## Runtime libraries and application requirements

- Replace generic parsing, encoding, text, math and protocol primitives with the pinned library or standard-library implementation. Retain the smallest adapter for required policy, value ownership and wire or storage representation. Remove quirk-only fallback parsers and formatting shims.
- Preserve gateway API schemas, statuses, required headers and signed bytes, authentication and authorization decisions, persistence codecs and schema, delivery receipts, durable accounting and settlement. Use the repository's codec-safe migration and rollback procedures for any storage change.
- Preserve limits and fail-closed validation for untrusted input. Apply shared defaults from `include/uos/rt/limits.hpp` at each qualified boundary. Keep smaller existing bounds on the same operation and use a documented larger exception only for a proven application flow. Keep ordinary status, close code or error mappings. [Runtime limits](docs/RUNTIME.md#input-limits)
- Preserve existing application paths and cite any unimplemented dependency. Keep the established coroutine ownership rules and the source comments that explain constraints.

## Tests

- Keep application fixtures and run affected real entry points against the candidate. Preserve contract-sensitive assertions, stored bytes, client/provider framing, validation decisions and one admission and settlement lifetime.
- Retain valid generic coverage and update expected values for documented standard-library differences. Record the behavior, an example, why the application still works, and its verification receipt. [Deviations from Bun](docs/DEVIATIONS-FROM-BUN.md)
- For each newly adopted parsing or library boundary, add a bounded robustness harness and coordinate registry changes with the build owner. Prove each new input limit with a boundary case and the relevant ordinary error.
- Use the approved wrappers, warning settings and fixture isolation. Hand off focused results with exact inputs, commands, outcomes, paths, line deltas and remaining gaps. [Testing](docs/TESTING.md)
