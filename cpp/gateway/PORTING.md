# TypeScript to C++ gateway port: rules for every porter

This tree (`cpp/gateway/`) is a module-for-module C++20 port of the first-party
TypeScript runtime (`src/`, `serve.ts`, `lib/jev_compaction/`). The TypeScript is the
oracle. The port is a translation, not a redesign: preserve behaviour, including
quirks, error text, status codes, field order, and log shapes. The bounded pilot in
`cpp/` (everything outside `cpp/gateway/`) is a separately qualified artifact: read
it for idioms, never edit it.

Runtime references, mandatory reading before porting: `docs/rt/values.md`, `text.md`,
`async.md`, `web.md`, `regex.md`, `kv.md`, `platform.md`, `record.md`, `websocket.md`, `sqlite.md` and `gzip.md` (when your modules use WebSocket, SQLite or gzip), and `docs/PORT-NOTES.md`
(conventions fixed by earlier porters). They give the C++ call for
every JavaScript construct; use `uos::rt`, never reinvent it.

`PORT-MANIFEST.json` (regenerate with `python3 cpp/gateway/tools/manifest.py` from
the repository root) lists every module, its imports, its direct tests, and its
C++ paths.

## Host safety (this machine serves production)

- The live gateway runs on this host from another checkout. Never touch
  `.data/` of the repository root, `.env*`, any `kv*.sqlite3`, systemd units,
  ports 7999/8000, or any process you did not start. Never read secrets.
- Two cores, about 2 GiB of spare memory, about 2 GiB of spare disk. Compile
  **only** through `cpp/gateway/tools/` wrappers, which serialise compiler runs,
  lower their priority and cap their memory. Never run `g++`, `cmake`, `ninja`,
  `bun test` over the whole suite, `tsc`, or `bun install` directly. Running one
  focused bun test file to learn the oracle's behaviour is fine.
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
- One TS module per header/impl pair. Do not merge, split, or re-export modules.
  A TS `export { x } from "./y"` becomes a `using` declaration in the header.
- Headers include what they use; `#pragma once`; no `using namespace` in headers.
- The qualified compiler is GCC 12.2 in C++20 mode: no C++23 library or language
  features (`std::expected`, `std::print`, deducing `this`, `std::ranges::to`).

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

| TypeScript                                                                                                                                          | C++                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| --------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `string`                                                                                                                                            | `std::string` holding UTF-8. JS string operations count UTF-16 code units: wherever `.length`, `slice`, `substring`, `charCodeAt`, `padStart`, index access, or a regex can observe non-ASCII text, use the `uos::rt` UTF-16 helpers.                                                                                                                                                                                                             |
| `number`                                                                                                                                            | `double` unless the value is provably an integer in every path, then `std::int64_t`. Number-to-string and JSON number output must match JavaScript; use the `uos::rt` formatters, never `std::to_string` or iostreams for doubles.                                                                                                                                                                                                                |
| `boolean`, `bigint`                                                                                                                                 | `bool`, `std::int64_t`/`rt::BigInt` as the range requires                                                                                                                                                                                                                                                                                                                                                                                         |
| `unknown`, `any`, `Record<string, unknown>`, parsed JSON                                                                                            | `boost::json::value` / `boost::json::object` (alias `uos::json`). Objects keep insertion order like JS; integer-like keys are the documented exception, handle them where the TS can observe it.                                                                                                                                                                                                                                                  |
| `interface` / type alias of an object                                                                                                               | `struct` with the same field names and order, when the TS constructs and reads it as a typed value. If it is only ever a view over wire JSON, keep it as `json::object` and access by key.                                                                                                                                                                                                                                                        |
| `T \| undefined`, optional field                                                                                                                    | `std::optional<T>`                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `T \| null`                                                                                                                                         | `std::optional<T>` only when `undefined` cannot also occur; when both occur and the TS distinguishes them, use `rt::Nullable<T>`                                                                                                                                                                                                                                                                                                                  |
| string literal union                                                                                                                                | `enum class` plus `toString`/`parse` helpers when exhaustively switched; otherwise `std::string`                                                                                                                                                                                                                                                                                                                                                  |
| discriminated union                                                                                                                                 | `std::variant` of structs, or a struct with a kind enum when variants share most fields                                                                                                                                                                                                                                                                                                                                                           |
| `T[]`, `Map`, `Set`                                                                                                                                 | `std::vector<T>`; `rt::OrderedMap`/`rt::OrderedSet` when iteration order is observable (it is insertion order in JS), otherwise `std::unordered_map`/`std::unordered_set`                                                                                                                                                                                                                                                                         |
| `Uint8Array`, `ArrayBuffer`                                                                                                                         | `std::vector<std::uint8_t>` / `std::span<const std::uint8_t>`                                                                                                                                                                                                                                                                                                                                                                                     |
| `Promise<T>`, `async`                                                                                                                               | `rt::Task<T>` (`boost::asio::awaitable<T>`) for async functions, `rt::Promise<T>` for promises held as values, on the single gateway IO thread (see `docs/rt/async.md`; its sections 3 and 4 on coroutine and lifetime rules are mandatory reading). JS is single-threaded; the port stays single-threaded, so no mutexes for state the TS guards only by run-to-completion. Preserve every suspension point's position relative to side effects. |
| `AbortSignal`, timers, `Date.now()`, `crypto`, `fetch`, `Request`/`Response`/`Headers`, `ReadableStream`, `TextEncoder`, `URL`, env access, Deno KV | the matching `uos::rt` type. Never call the OS clock, random source, environment, sockets or filesystem directly from a ported module unless the TS module itself does through `node:*`; then use the `uos::rt` wrapper for that Node API.                                                                                                                                                                                                        |
| `throw new Error(m)` / custom error classes                                                                                                         | exception types deriving from `rt::JsError` carrying `name` and `message` with the same text; `catch` sites translate one for one                                                                                                                                                                                                                                                                                                                 |
| closures, callbacks                                                                                                                                 | `std::function` or templates; capture lifetimes explicitly (no dangling references across `co_await`)                                                                                                                                                                                                                                                                                                                                             |
| regular expressions                                                                                                                                 | `rt::Regex` where provided; otherwise a hand-written matcher with the same accepted language. `std::regex` is forbidden (performance, locale, no UTF-16 semantics).                                                                                                                                                                                                                                                                               |

When JS semantics leak (truthiness of `""`/`0`/`NaN`, `??` versus `||`, `==` coercion,
`Number()` parsing, `JSON.stringify` skipping `undefined`, sort stability and
comparator coercion, `toFixed`, `Math.round` on halves, integer overflow past 2^53,
property enumeration order, `Date` parsing/formatting), reproduce the JS result
exactly. If in doubt, run a three-line bun script to see what the oracle does.

## Fidelity rules

- Translate every branch. No `TODO`, stub, placeholder, "simplified", or silently
  dropped path. If something cannot be ported yet because a dependency is not
  ported, declare the dependency's interface in its own correct header location
  (declaration only), use it, and list it in your report.
- Do not fix bugs you find in the TS. Port them and list them in your report.
- Comments from the TS are carried over when they explain a constraint. Do not add
  comments that narrate the translation.
- User-visible strings, error codes, header names, JSON keys, log event names and
  numeric constants are copied byte for byte.

## Tests

- Port every bun test that directly targets your modules (see the manifest's
  `tests` lists) into `cpp/gateway/tests/`, one C++ file per TS test file, same
  test names, same assertions, using the `uos::rt::test` harness. A test file that
  targets several modules is ported by whoever ports the last module it needs;
  until then port the cases your modules can satisfy and list the rest.
- Where the TS has no direct tests, write differential cases: run the TS module
  under bun on chosen inputs, record the outputs as literals in the C++ test.
- A module is done only when its C++ compiles warning-free under the wrapper's
  flags and its ported tests pass.

## Reporting

End with a short report: files written, tests ported and their pass/fail counts
(real output, not expectation), dependencies you declared but did not implement,
TS bugs preserved, `rt` gaps you hit, and anything you could not port and why.
