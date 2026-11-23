# uos::rt kv: the `#kv` store for ported modules

`#include "uos/rt/kv.hpp"`. Every ported module that takes a `Deno.Kv` takes an
`rt::Kv&`. `rt::MemoryKv` is an in-memory store with the observable semantics of the
production store (`src/storage/kv.ts`: @deno/kv 0.14 SDK + patched native SQLite addon

- the `native-kv.ts` guard), for tests and for every place the TS opens
  `openKv(":memory:")`. The durable store is `rt::SqliteKv` (`uos/rt/kv_sqlite.hpp`,
  `kv-durable.md`): the same SDK layer over a C++ port of the native SQLite backend, reading and
  writing the production file; `storage::kv::openKv(path)` opens it behind the guard.

Oracle: the SDK with the repository's patch and the published linux-arm64 addon under Bun
1.4.0 on throwaway databases (the addon lacks only the rollback patch, whose behaviour the
tests take from `tests/storage-bun-kv.test.ts`). Every expected value in
`tests/rt/kv_test.cpp` was measured there.

## TypeScript construct -> C++ call

`co_await` is `await`; the `Task` is `rt::Task<T>` (`boost::asio::awaitable<T>`).

| TypeScript                                                                                              | C++                                                                                                                                                                                             |
| ------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `kv: Kv` parameter                                                                                      | `rt::Kv& kv`                                                                                                                                                                                    |
| `["a", id, 1n, true, bytes]` key                                                                        | `rt::KvKey{"a", id, rt::BigInt(1), true, rt::KvBytes{...}}`                                                                                                                                     |
| `[...prefix, id]`                                                                                       | `rt::KvKey{prefix, id}` (a KvKey argument is spread) or `rt::kvKey(prefix, id)`                                                                                                                 |
| `await kv.get(key)`                                                                                     | `co_await kv.get(key)` -> `KvEntryMaybe<KvValue>`                                                                                                                                               |
| `(await kv.get<T>(key)).value`                                                                          | `(co_await kv.get<T>(key)).value`, see "Typed reads"                                                                                                                                            |
| `entry.versionstamp === null`                                                                           | `!entry.versionstamp` (`std::optional<std::string>`)                                                                                                                                            |
| `await kv.getMany<[A, B]>([k1, k2])`                                                                    | `co_await kv.getMany(keys)` then `entries[i].as<A>()`; one T for all: `getMany<T>`                                                                                                              |
| `await kv.set(key, value, { expireIn })`                                                                | `co_await kv.set(key, value, {.expireIn = ms})` -> `KvCommitResult`                                                                                                                             |
| `await kv.delete(key)`                                                                                  | `co_await kv.delete_(key)`                                                                                                                                                                      |
| `kv.list({ prefix }, { limit, cursor, reverse, batchSize })`                                            | `kv.list({.prefix = p}, {.limit = n, .cursor = c, .reverse = true, .batchSize = b})` -> `std::unique_ptr<KvListIterator>` (validates now, throws TypeError)                                     |
| `for await (const e of it) { ... }`                                                                     | `while (auto e = co_await it->next()) { ... }` (`e` is `std::optional<KvEntry<>>`)                                                                                                              |
| `break` out of `for await`                                                                              | `co_await it->return_();`                                                                                                                                                                       |
| `it.cursor`                                                                                             | `it->cursor()`                                                                                                                                                                                  |
| `await Array.fromAsync(it)`                                                                             | `co_await rt::kvListToArray(*it)`                                                                                                                                                               |
| `kv.atomic().check(e).set(k, v).sum(k2, 1n).commit()`                                                   | `auto op = kv.atomic(); op->check(e).set(k, v).sum(k2, 1); co_await op->commit();`                                                                                                              |
| `.check(...entries)` / `.check({ key, versionstamp: null })`                                            | `->check(entries)` (any mix of entries, AtomicChecks, vectors) / `->check(rt::AtomicCheck{key, std::nullopt})`                                                                                  |
| `.min(k, n)` / `.max(k, n)` / `.mutate(...m)`                                                           | `->min(k, n)` / `->max(k, n)` / `->mutate({rt::KvMutation{...}})`                                                                                                                               |
| `result.ok`, `result.versionstamp`                                                                      | same fields; `{ ok: false }` has an empty versionstamp                                                                                                                                          |
| `new KvU64(n)`, `u.value`                                                                               | `rt::KvU64(n)` (throws like the TS), `u.value` (`uint64_t`) / `u.toBigInt()`                                                                                                                    |
| `value instanceof KvU64`                                                                                | `value.isU64()`                                                                                                                                                                                 |
| `value instanceof Uint8Array`                                                                           | `value.isBytes()` for a Uint8Array owning its whole buffer (every top-level read); with views: `rt::kvIsUint8Array(value)`                                                                      |
| `kv.close()`                                                                                            | `kv.close()`                                                                                                                                                                                    |
| `openKv(":memory:")` in a test                                                                          | `rt::MemoryKv kv(now)` with `now` = `std::function<double()>` epoch ms                                                                                                                          |
| two `openKv(path)` on one file                                                                          | `auto db = std::make_shared<rt::MemoryKvDatabase>(now); rt::MemoryKv a(db), b(db);`                                                                                                             |
| `new Proxy(kv, { get(...) })`                                                                           | subclass `rt::ForwardingKv` / `rt::ForwardingAtomicOperation`                                                                                                                                   |
| `WeakMap<Deno.Kv, T>` / `WeakSet<Deno.Kv>` (`admissionTails`, `stateGenerations`)                       | `rt::WeakMap<rt::Kv, T>` / `rt::WeakSet<rt::Kv>` (`get`, `has`, `set`, `delete_`; uos/rt/weak.hpp, also through http.hpp), keyed by `kv.identity()`: see "Identity"                             |
| `kv === other` (two live references)                                                                    | `&kv == &other` (or `kv.identity() == other.identity()`)                                                                                                                                        |
| `let cached: Kv \| null = kv; ... cached === kv`                                                        | `std::weak_ptr<const void> cached = kv.weakIdentity(); ... cached.lock().get() == kv.identity()` (never a raw `rt::Kv*`)                                                                        |
| hand-written `Kv` fakes                                                                                 | subclass `rt::Kv`; build `atomic()` on `rt::GenericAtomicOperation` or subclass `AtomicOperation`                                                                                               |
| `await openKv(path)` on a database file (the durable store)                                             | `co_await storage::kv::openKv(path)` (rt::SqliteKv behind the guard); standalone `rt::SqliteKv kv(path, options)` (kv-durable.md)                                                               |
| `typeof kv.enqueue === "function"`, `kv.enqueue(v, { delay, keysIfUndelivered })`                       | `dynamic_cast<rt::KvEnqueue*>(&kv)`, `->enqueue(v, rt::KvEnqueueOptions{...})` (SqliteKv and the guard over it; not MemoryKv)                                                                   |
| `deserialize(bytes)` from node:v8 (Bun)                                                                 | `rt::kvJscDeserialize(bytes)` (also `storage::kv::deserialize()`)                                                                                                                               |
| `typeof entry.value` / `isRecord(v)` (utils: arrays too) / `isRecord` without arrays, on a stored value | `rt::kvTypeof(v)` / `rt::kvIsObject(v)` / `rt::kvIsRecord(v)` (uos/rt/kv_js.hpp; see "Stored values seen from JavaScript")                                                                      |
| `normalizeX(entry.value)` where the guard is written against `unknown` (json::value)                    | `normalizeX(rt::storedJsonView(entry.value))`, options per the guard's shape (below)                                                                                                            |
| `JSON.stringify(entry.value)`, `String(entry.value)`                                                    | `rt::kvJsonStringify(v)` (std::nullopt is undefined), `rt::kvToString(v)`                                                                                                                       |
| `value instanceof Uint8Array` (views too)                                                               | `rt::kvIsUint8Array(v)`, bytes `rt::kvUint8ArrayBytes(v)` (not `isBytes()`, which misses views)                                                                                                 |
| `{ request_key: key }` / `value.request_key as KvKey`                                                   | `rt::kvValueFromKey(key)` / `rt::kvKeyFromValue(v)` (SDK error "Unsupported keyPart: <typeof> <String(part)>"); one part: `rt::kvValueFromKeyPart`, `rt::toKvKeyPart`, `rt::kvKeyPartFromValue` |
| `key[i] === part`, `isSameKvKey(a, b)`                                                                  | `rt::kvKeyPartStrictEquals(a, b)`, `rt::kvKeyStrictEquals(a, b)`                                                                                                                                |

`enqueue`, `listenQueue`, `watch`, `Symbol.dispose` and `consistency: "eventual"`
behaviour are not part of `rt::Kv`: no first-party module calls them (the TS fakes only
throw), and the SQLite store ignores `consistency` (`KvGetOptions` keeps the field).
`enqueue` (paid-fallback's reconciliation jobs) is the separate capability `rt::KvEnqueue`,
detected with `dynamic_cast`; nothing listens or watches.

## Identity

Every `rt::Kv` object (MemoryKv, ForwardingKv, GuardedKv, a test fake) owns an identity
token made with it and destroyed with it: `kv.identity()` (`const void*`) and
`kv.weakIdentity()` (`std::weak_ptr<const void>`), the protocol `rt::WeakMap` / `rt::WeakSet`
key on. So, as in JS: an entry dies with its Kv; a Kv constructed later never matches an old
entry, even when it reuses the address (sequential test stores often do, which is why a stored
raw `Kv*` must never stand for a JS reference); a wrapper is its own object (`WeakMap.get` on a
`new Proxy(kv, ...)` does not see kv's entry, so a `ForwardingKv` is a separate key). A dead
key's value is released when the map next purges (a later `set`); a value holding a locked
`weakIdentity()` of its own key keeps that entry forever (no ephemerons).

```cpp
// const admissionTails = new WeakMap<Kv, Promise<void>>();
rt::WeakMap<rt::Kv, rt::Promise<void>> admissionTails;  // a namespace-scope variable of the .cpp
// const priorAdmission = admissionTails.get(kv) ?? Promise.resolve();
rt::Promise<void> priorAdmission = admissionTails.get(kv).value_or(rt::Promise<void>::resolve());
admissionTails.set(kv, admissionRelease);
// if (admissionTails.get(kv) === admissionRelease) admissionTails.delete(kv);
if (const auto* tail = admissionTails.find(kv); tail && *tail == admissionRelease) admissionTails.delete_(kv);
// WeakMap<object, ...> keyed by `kv ?? environmentScope`: two maps, or one keyed by a token
// holder (any type with identity()/weakIdentity()) when other objects share the map.
```

## Common shapes

```cpp
// Typed read. Absent -> std::nullopt; another type -> TypeError("KV value is ..., not ...").
const auto entry = co_await kv.get<std::optional<json::object>>(key);
if (!entry.value) co_return std::nullopt;

// Set with expiry (expireIn: positive safe integer milliseconds).
co_await kv.set(key, KvValue(record), {.expireIn = 60'000});

// Compare-and-set loop.
for (;;) {
  const rt::KvEntryMaybe<> current = co_await kv.get(key);
  json::value next = update(current.value.toJson());
  auto operation = kv.atomic();
  operation->check(current).set(key, rt::KvValue(next));
  const rt::KvCommitResult result = co_await operation->commit();
  if (result.ok) break;
}

// Prefix page with a resumable cursor.
auto iterator = kv.list({.prefix = prefix}, {.limit = 100, .cursor = cursor});
while (auto row = co_await iterator->next()) handle(row->key, row->value);
const std::string next = iterator->cursor();  // "" when the range is exhausted
```

## GCC 12.2 rules inside `co_await` (binding for every porter)

Two compiler bugs affect any statement that contains `co_await`:

1. A braced list that makes an `std::initializer_list` (a `std::vector`, `json::object`,
   `json::array` or `KvValue::object({...})` literal) does not compile ("array used as
   initializer").
2. A braced aggregate with a non-trivially-destructible member (`KvCommitResult{false,
""}`, `KvListOptions{.cursor = c}`, `KvArrayBufferView{...}`) compiles but is destroyed
   twice at run time (heap corruption).

Build such values in a previous statement and pass the name. `KvKey{...}` and `kv.get({"a",
id})` are safe: KvKey's brace constructor is variadic, not an initializer list.
`KvSetOptions{.expireIn = n}` and `KvGetOptions{...}` are safe (trivial members).

## Keys

`KvKeyPart` holds bytes (`KvBytes`), a string (WTF-8, docs/rt/text.md), a number (any C++
arithmetic type except `bool` becomes a double), an `rt::BigInt`, or a bool. Equality is
encoding equality (`-0 != 0`, every NaN equal); `<=>` is Deno KV order. `KvKey` derives from
`std::vector<KvKeyPart>`.

- `packKey(key)` / `packKeyPart(part)`: the SDK tuple codec with the repository's number
  patch (strings are encoded like `TextEncoder`: lone surrogates become U+FFFD).
- `unpackKey(bytes)`: the SDK decoder and its exact errors; strings decode like
  `new TextDecoder()` (a leading BOM is dropped, ill-formed bytes become U+FFFD), so a key
  read back from `list` can differ from the key written (`get` returns the caller's key).
- `compareKvKeys`, `formatVersionstamp(version)`, `packCursor(bytes)` / `unpackCursor`.
  Cursors are the SDK's `base64(JSON {"lastYieldedKeyBytes": hex})`; gateway records
  persist them, so the format is part of the contract.

## Values

`KvValue` is a tree holding one of: `KvUndefined`, `nullptr`, `bool`, `double`,
`std::string`, `BigInt`, `KvU64`, `KvBytes` (a Uint8Array owning its whole buffer),
`KvArrayBuffer`, `KvArrayBufferView` (other typed arrays/DataView, and Uint8Array views
into a larger buffer), `KvDate`, `KvRegExp`, `KvError`, `KvArray`, `KvObject`, `KvMap`,
`KvSet`, `KvHole` (array holes, from converted legacy data only).

- JSON-shaped values: construct from `json::value` / `json::object` / `json::array`
  (numbers become doubles); read with `toJson()` (as `json::parse(JSON.stringify(v))` would:
  integral numbers below 1e21 become int64 or uint64, other numbers stay doubles,
  undefined members and holes are dropped, undefined array elements become null; other
  types throw TypeError "KV value is <kind>, not JSON").
- `KvObject` keeps JS property order (array-index keys ascending, then insertion order).
  `KvMap`/`KvSet` keep insertion order with SameValueZero keys (`-0` stored as `+0`).
- `==` is deep equality like `assert.deepStrictEqual`: object members, Map entries and Set
  members in any order; numbers by Object.is.
- `value["k"]`, `value[i]`, `truthy()`, `kind()`, `is<T>()`, `as<T>()`, `asNumber()` etc.
- What a read returns (`kvStoredValue`): a top-level `KvU64` or Uint8Array is stored raw and
  comes back as `KvU64`/`KvBytes`; everything else is a structured clone: a nested KvU64
  becomes the plain object `{ value: n }`, an Error keeps only a standard class name,
  RegExp flags take canonical order.
- `assertPortableKvValue`: an `KvError` with a `cause` anywhere rejects the commit
  (TypeError "Bun KV cannot preserve Error.cause; persist an explicit error record instead").
- `kvJscDeserialize(bytes)`: Bun's node:v8 `deserialize`, the inverse of what Bun writes
  (history-dependent forms, shared references as copies, Buffer envelopes); Bun's errors for
  malformed bytes; TypeError "Serialized value <what> has no KvValue representation" for what
  a KvValue cannot hold (cycles, boxed primitives, ...); nesting capped at 2000 (kv-durable.md).
- `kvJscSerialize(value)`: Bun's node:v8 bytes (JSC SerializedScriptValue 14). The store's
  limits measure it; it writes the canonical form where JSC depends on a value's history
  (int32 tags for integral numbers in int32 range, the 8-bit string form when every code
  unit is <= U+00FF).

### Stored values seen from JavaScript (`uos/rt/kv_js.hpp`)

What a JS guard observes on a value read back from the store (Bun 1.4.0, values passed through
node:v8 serialize/deserialize; a top-level KvU64 or Uint8Array stored raw), pinned in
`tests/rt/kv_js_test.cpp`:

| Stored kind                                                | typeof      | isRecord (non-array) | Object.keys         | other reads JS can make                                                                    |
| ---------------------------------------------------------- | ----------- | -------------------- | ------------------- | ------------------------------------------------------------------------------------------ |
| undefined (also an array hole read as an element)          | "undefined" | false                | throws              |                                                                                            |
| null                                                       | "object"    | false                | throws              |                                                                                            |
| bigint                                                     | "bigint"    | false                | []                  | `Number.isFinite` false; JSON.stringify throws                                             |
| Date, Map, Set, RegExp, Error, ArrayBuffer, DataView       | "object"    | true                 | []                  | inherited/non-enumerable: `size`, `getTime`, `source`, `lastIndex`, Error `name`/`message` |
| KvU64 (top level; nested ones read back as `{ value: n }`) | "object"    | true                 | ["value"]           | `value` is a bigint                                                                        |
| Uint8Array (views too) and other typed arrays              | "object"    | true                 | ["0", "1", ...]     | `length`; elements are numbers (bigints for BigInt64Array)                                 |
| object member holding undefined                            |             |                      | listed              | `in`/hasOwn true, the read is undefined                                                    |
| array element holding undefined / a hole                   |             |                      | listed / not listed | both read as undefined; JSON.stringify prints null                                         |

`rt::storedJsonView(value[, options])` turns a stored value into the json::value a guard over
`unknown` would see. JSON parts are kept (numbers canonical: NaN, Infinity and -0 stay numbers);
Date, Map, Set, RegExp, Error, ArrayBuffer and DataView are `{}`. JSON has no slot for undefined or
bigint, and no single mapping is exact for every guard, so these are options
(`rt::StoredJsonViewOptions`, designated initializers):

| Option                              | Values (default first)                           | Exact for                                                                                                                                                                                                                 |
| ----------------------------------- | ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `undefinedMember`                   | `omit`, `emptyObject`, `null`                    | omit: member reads (`=== undefined`, typeof, `??`, truthiness, typed tests); emptyObject: presence tests (`hasOwn`, `in`, `Object.keys`, exhaustive-key checks) and guards that reject a present member of the wrong type |
| `undefinedElement`, `hole`          | `null`, `emptyObject`                            | null: JSON.stringify's view; emptyObject: guards that give null a meaning (e.g. a reasoning level `null` is "none")                                                                                                       |
| `topLevelUndefined`                 | `null`, `emptyObject`                            | null: isRecord/typeof/truthiness of undefined; pass nullptr to a guard's pointer overload for `=== undefined`                                                                                                             |
| `bigint`                            | `emptyObject`, `null`, `decimalString`, `number` | emptyObject fails every number/string/boolean/null test as a bigint does (isRecord excepted)                                                                                                                              |
| `kvU64Members`, `typedArrayMembers` | `true`, `false`                                  | true: `{ value: <bigint> }` and `{ "0": n, ... }`, the own enumerable members                                                                                                                                             |

The tables abbreviate the enumerators: `omit` is `rt::StoredJsonViewOptions::UndefinedView::omit`
(`using View = rt::StoredJsonViewOptions::UndefinedView;` then `.hole = View::emptyObject`), the
bigint ones are `rt::StoredJsonViewOptions::BigIntView::...`. Options compose like any aggregate:
`rt::storedJsonView(entry.value, rt::StoredJsonViewOptions{.undefinedMember = View::emptyObject})`
(outside a `co_await` statement).

Earlier local helpers and the options that reproduce them exactly (migrate by replacing the helper
with the call; every row was checked with a bun differential that runs the module's TS guard on a
stored value and on the helper's view of it, replacing each member and element of a valid record by
undefined, a hole, 5n, a Date, a Map, a Uint8Array, a KvU64, NaN and an Error, and adding an
undefined member to each object):

| Local helper                                                                                                                                                                                | Options                                                                                                                                                                       | TS guards checked (variants, differences)                                                                                                                                                            |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `quota_v2::kernelQuotaValue` (src/kernel/quota_v2.cpp; used by cache/kv-migration-usage, kv-migration-base)                                                                                 | `{.undefinedMember = omit, .undefinedElement = null, .hole = null, .topLevelUndefined = emptyObject, .kvU64Members = false, .typedArrayMembers = false}`                      | normalizeKernelQuotaPolicyV2 (65, 0), normalizeKernelQuotaWindowV2 (81, 0); the default is exact too                                                                                                 |
| `storedView` with the `isUndefined() ? null` wrapper (src/api_key_policy.cpp, paid_fallback/{index,ledger_admission,ledger_backfill,ledger_settlement}.cpp, auth/local_development_key.cpp) | as above with `.topLevelUndefined = null`                                                                                                                                     | normalizeApiKeyUsageRequestV3 (97, 0), normalizeApiKeyUsageWindowV3 (73, 0), isPaidFallbackReconciliationGate (17, 0); the default is exact too                                                      |
| `storedView(value, 0)` in src/authority/native_api_key_policy.cpp                                                                                                                           | as kernelQuotaValue                                                                                                                                                           | (its caller passes nullptr for undefined itself)                                                                                                                                                     |
| `storedView` in src/runtime_config.cpp                                                                                                                                                      | `{.undefinedMember = emptyObject, .undefinedElement = emptyObject, .hole = emptyObject, .topLevelUndefined = emptyObject, .kvU64Members = false, .typedArrayMembers = false}` | normalizeRuntimeConfig (306, 1: a stored Error as a model entry, whose inherited `name` the TS reads as a slug); the default would differ in 12 (hasOnlyKeys/hasOwn guards, `null` reasoning levels) |
| `storedView` in src/paid_fallback/rollups.cpp                                                                                                                                               | as runtime_config                                                                                                                                                             | isPaidFallbackUsageRollup (113, 0); the default is exact too                                                                                                                                         |
| `storedView` in src/metered_quota.cpp                                                                                                                                                       | `{.undefinedMember = omit, .undefinedElement = null, .hole = emptyObject, .topLevelUndefined = null, .kvU64Members = false, .typedArrayMembers = false}`                      | isMeteredQuotaState (145, 0; the runtime_config form would differ in 4: `x === undefined \|\| ...` optional members); the default is exact too                                                       |

For a new guard, check how it distinguishes an absent member from a member with an undefined value.
The paired comparison with the TypeScript gateway lives at [commit `ca045a83ebaab6c50c20567d1a825638abac0891`](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891/cpp/gateway/conformance).

The other operations: `rt::kvJsonStringify(v[, space | gap])` is JSON.stringify (std::nullopt for
undefined; typed arrays print index members, Date its toJSON, Map/Set/RegExp/Error/buffers `{}`,
TypeError "JSON.stringify cannot serialize BigInt." for any bigint, a KvU64 included);
`rt::kvToString(v)` is String() (arrays and typed arrays joined, Date.prototype.toString in UTC,
"/source/flags", Error.prototype.toString, "[object Object]" for a KvU64 or object; an object with
an own `toString` member throws TypeError "No default value"); `rt::kvIsUint8Array` /
`rt::kvUint8ArrayBytes` see views; key parts: `rt::toKvKeyPart(v)` throws the SDK's Error
"Unsupported keyPart: <typeof> <String(v)>" (convert where the TS uses the key: the SDK throws when
encoding), `rt::kvKeyFromValue(array)` skips holes as the SDK's flatMap does,
`rt::kvKeyPartStrictEquals` compares Uint8Array parts by identity (the same KvKeyPart object).

### Typed reads

`get<T>` / `getMany<T>` / `entry.as<T>()` convert with `kvValueAs<T>`: `KvValue` as is;
`json::value` via `toJson()` (null when absent); `std::optional<U>` (nullopt for
null/undefined); `json::object` / `json::array` (TypeError for another shape, including an
absent entry, so use `std::optional<json::object>` when the key may be missing); any
`KvValue` alternative (`KvU64`, `std::string`, `double`, `KvBytes`, ...); otherwise
`json::value_to<T>(toJson())`.

## Store semantics MemoryKv reproduces

- Versions: each committed write (empty and check-only commits included) takes the next
  version; the native store adds a random 1..9 (`MemoryKvOptions::versionIncrement`; default
  +1). Versionstamp = 16 hex digits of the version + "0000". A failed check returns
  `{ok: false}` and writes nothing.
- Atomic commits: checks first (`std::nullopt` or `""` = must be absent; a set check must
  match exactly), then mutations in order inside one transaction. `sum`/`min`/`max` on an
  absent key store the operand; on a non-KvU64 row the whole commit rolls back and throws
  TypeError "Atomic arithmetic requires an absent key or KvU64 value" (cause: Error "Failed
  to perform 'sum' mutation on a non-U64 value in the database"). Every arithmetic write
  rewrites the row's version and clears its expiry; `sum` wraps modulo 2^64.
- Native limits, in validation order: Error "TooManyChecks" (> 10), "TooManyMutations"
  (> 1000), then per check "KeyTooLong" (key > 2049 bytes) and "InvalidVersionstamp"
  (decoded stamp not 0 or 10 bytes), per mutation "KeyTooLong" (> 2048) and "ValueTooLong"
  (> 65536 serialized bytes), then "AtomicWriteTooLarge" (check keys + mutation keys +
  values > 819200). SDK-side errors come first: decodeHex ("Invalid byte 'x'", "Odd length
  hex string"), then Error.cause. Reads: "TooManyReadRanges" (getMany > 10 keys),
  "KeyTooLong" (read key > 2049; `list({prefix})` adds 2 bytes to the start key).
- SDK validation (thrown before any wait): "Key cannot be empty", "Bad 'expireIn', expected
  optional positive integer, found <n>", selector/limit/batchSize TypeErrors, KvU64 errors,
  "Cannot call '.<method>' after '.close' is called" (`set`/`delete` report `.commit`; an
  iterator started before close fails with Error "db not found").
- List: the SDK generator (batches of `min(batchSize ?? 100, 500, limit)` rows; a write
  beyond the fetched batch is seen, one inside it is not); `{prefix}` never returns the
  prefix row itself; the cursor resumes after its key even when it lies outside the
  selector (the SDK quirk persisted cursors rely on); `cursor()` throws before the first
  entry unless one was passed, is "" after an exhausted scan and keeps the continuation
  when the scan stopped at `limit`. An invalid cursor fails on the first `next()`.
- Expiry: `expireIn` is an absolute deadline taken at commit; expired rows stay readable
  until a sweep deletes them: when a handle opens and every 60 s after
  (`MemoryKvOptions::sweepIntervalMs`; 0 sweeps before every operation).
- Timing: an operation does its work when it starts and its result arrives one macrotask
  (posted handler) later; SDK validation errors and buffered list entries arrive at once.

## Test helpers

- `MemoryKv`, `MemoryKvDatabase` (`row(key)` / `rows()` / `version()` expose the table as
  the TS tests read it through `bun:sqlite`).
- `ForwardingKv` / `ForwardingAtomicOperation`: forward everything; override the calls a
  test intercepts (the `new Proxy(kv, ...)` wrappers in the sentinel, capacity and facade
  tests). A subclass that overrides `check` or `get`/`getMany` adds `using
...::check;` / `using rt::Kv::get; using rt::Kv::getMany;`.
- `kvStructuredClone(value)`: `structuredClone` on the value model, for fakes that clone
  what they store.
- `GenericAtomicOperation(commitFn)`: the SDK's recorder with its validation, for fakes
  that keep their own rows (`tests/helpers/counting-kv.ts`, `sentinel-kv-stub.ts`, the
  `ContendedKv`-style classes); fakes implement `KvListIterator` for `list`.
- `tests/helpers/kv-budget-harness.ts` drives whole gateway modules and is ported with
  them; `CountingKv` and `SentinelKvStub` are ported with their tests on top of `rt::Kv`.

## Not reproducible

- Shared references and cycles (the TS `cycle` test): a `KvValue` is a tree.
- Accessor properties, functions, symbols, boxed primitives, class identity, and Node's
  `Buffer` (a nested Buffer has its own Bun encoding) have no C++ value.
- JSC's history-dependent encodings: a TS number that JSC holds as a double in int32 range
  is 4 bytes longer than the canonical form; size limits can differ by that near 64 KiB.
- An Error's `line`/`column`/`sourceURL`/`stack` are what the porter sets, not the JS
  creation site.
- Exact macrotask interleaving with other IO (the native completion time varies) and
  SQLite busy retries (the guard's `retrySqliteBusy`) belong to the durable store
  (`rt::SqliteKv`, whose completions arrive when its worker thread is done).
