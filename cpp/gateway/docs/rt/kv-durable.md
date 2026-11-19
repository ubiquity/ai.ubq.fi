# uos::rt SqliteKv: the durable `#kv` store

`#include "uos/rt/kv_sqlite.hpp"` (`src/rt/kv_sqlite.cpp`, the shared SDK layer in
`src/rt/kv_sdk.{hpp,cpp}`, the value reader in `src/rt/kv_jsc_deserialize.cpp`). The design is
`kv-durable-design.md` (option C: the native backend reimplemented in C++ over `rt::sqlite`);
this page is what was built and how it was proven.

## What it is

```
storage::kv::openKv(path)            src/storage/kv.ts: path checks, codec marker, exclusive
  -> storage::kv::openNativeKv()     marked creation, busy retry (unchanged ports)
       default: rt::SqliteKv          the @deno/kv SDK (kv_sdk, shared with MemoryKv) over a C++
                                      port of denokv 9fe65cc's SQLite backend + the rollback patch
  -> storage::native_kv::guardNativeKv   the native-kv.ts guard (now forwards `enqueue`)
```

- `rt::SqliteKv(path, SqliteKvOptions)` is the SDK's store (NapiBasedKv), not the guarded one:
  `set`/`delete` are BaseKv's, commit errors are the backend's, its list iterator is the SDK's
  GenericKvListIterator (cursor kept at the last entry after the scan). `openKv` wraps it in the
  guard exactly as the TS wraps the addon.
- One handle = one SQLite connection driven by one worker thread (the addon's `sw-0`): SQLite
  never blocks the event loop (busy waits, fsync), operations of a handle run in the order they
  started, and each completes as a handler posted to the caller's io_context (which counts as
  outstanding work, so neither `rt::Loop` nor a fake clock moves on while SQLite works).
- `rt::KvEnqueue` (kv.hpp) is the queue capability; `paid_fallback::ledger_state::KvEnqueue` is an
  alias of it. SqliteKv and the guard over it implement it; MemoryKv does not.
- `storage::kv::deserialize()` defaults to `rt::kvJscDeserialize` (node:v8 `deserialize` under
  Bun), which also enables kv-conversion's bun-to-deno transcoding.
- `native_addon::nativeKvRequire()` defaults to the linked store's interface: the nine exports
  exist, so `prepareNativeKvBackend` still verifies the release artifact and the pinned interface
  as ported, but nothing calls them (they throw `rt::JsError` naming the bypass): the C++ SDK talks
  to the backend directly instead of through the addon's protobuf requests.

| TypeScript                                                                        | C++                                                                                        |
| --------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `await openKv(path)` (#kv)                                                        | `co_await storage::kv::openKv(path)` (unchanged; now opens SqliteKv)                       |
| `openKv(path, { implementation: "sqlite", napi, encodeV8, decodeV8 })` (@deno/kv) | `storage::kv::openNativeKv()(path, options)`; standalone: `rt::SqliteKv kv(path, options)` |
| `kv.enqueue(value, { delay, keysIfUndelivered })`                                 | `dynamic_cast<rt::KvEnqueue*>(kv.get())->enqueue(value, options)`                          |
| `deserialize(bytes)` (node:v8, Bun)                                               | `rt::kvJscDeserialize(bytes)`                                                              |
| `openKv(":memory:")` in a test                                                    | still `rt::MemoryKv`; `openKv(":memory:")` now gives a private in-memory SqliteKv          |

`SqliteKvOptions`: `encodeV8`/`decodeV8` (default Bun's codec behind assertPortableKvValue),
`now` (Date.now for expireIn/enqueue deadlines and the backend's sweep clock; empty = `rt::dateNow`
plus idle sweeps on the system clock), `versionIncrement` (default uniform 1..9),
`sweepIntervalMs`/`sweepJitterMs` (60 000 / 1 000; 0/0 sweeps before every operation). Tests fix
the increment and the clock; production passes nothing.

## File layout

- Schema: the C++ runs denokv's migration statements verbatim, so `sqlite_master.sql` is
  identical to a TS-created file (checked against the text a TS-created file holds, including the
  ALTER TABLE splices: `version integer not null\n, seq integer not null default 0)`), and a file
  left at migration 1 or 2 upgrades to the same text. `migration_state` = (0, 3), `data_version`
  one row k=0, `PRAGMA journal_mode=wal`, page size 4096, user_version/application_id 0. The C++
  sets `synchronous=FULL` and `busy_timeout=5000` per connection (rusqlite's default timeout).
  `uos_kv_metadata` (codec `bun-jsc-v1`) is created by `storage::kv::openKv` as before.
- Rows: tuple keys (`rt::packKey`); `v_encoding` 1 = Bun/JavaScriptCore serialized bytes for supported
  values, 2 = KvU64 8 bytes LE, 3 = raw bytes of a top-level Uint8Array
  (also a view: the viewed bytes). `version` = the commit's data_version; `seq` 0;
  `expiration_ms` absolute ms or -1 (plain sets and arithmetic reset it).
- Versions: each committed write runs `update data_version set version = version + step` inside
  the write transaction (step uniform 1..9); failed checks and failed (rolled back) writes do not
  advance it; a sweep that deleted rows advances it by exactly 1.
- Transactions: `BEGIN DEFERRED`, a `SAVEPOINT denokv_atomic_write` around each write (a failed
  write rolls back to it, the transaction still commits, the write reports its error), BUSY on
  BEGIN/COMMIT retried after 5..19 ms. A write whose read snapshot went stale, or that waited 5 s
  for another connection's lock, reports Error "database is locked" (the guard retries it).
- Queue: `enqueue` inserts `queue(ts = Date.now() + delay, id = uuid v4, data = node:v8 payload,
backoff_schedule = "[100,200,400,800]" (the SDK ignores the caller's schedule),
keys_if_undelivered = JSON of the packed keys)` (TEXT values, as rusqlite binds them), in the
  same atomic write, with denokv's limits. Opening a handle requeues messages a dead dequeuer left
  in `queue_running` past their deadline (or, schedule exhausted, writes them to their
  undelivered keys), as denokv's startup `queue_cleanup` does, then every ~60 s.
- Expiry: rows stay visible until a sweep deletes `expiration_ms <= now`: when a handle opens,
  then at min(earliest expiry the last sweep saw, last sweep + 60 s) + jitter < 1 s (denokv's
  `collect_expired` cadence). Sweeps run before the next operation once due and, on the real
  clock, while the handle is idle.

## Compatibility evidence

The marked Bun/JSC format is shared to the extent shown by these tests and the recorded cross-reader runs.
Rehearse with a stopped copy of production state before cutover. [Cutover checklist](../CUTOVER.md)

The TypeScript side ran the pinned legacy `#kv` opener (`src/storage/kv.ts` with
`prepareNativeKvBackend`) over the pinned native-kv artifact (denokv 9fe65cc + both patches; its
provenance matches the [moved inputs](../../packaging/native-kv/inputs.json)) under Bun 1.4.0,
on throwaway databases in scratch. The C++ side ran `storage::kv::openKv` (SqliteKv + guard).
[Historical rebuild and cross-reader script](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/scripts/test-native-kv.ts)

|                           | Proven by running                                                                                                                                                                                                                                                   | Hermetic test                                                                                                                                                                                                                                                                                              |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| (a) TS writes, C++ reads  | a TS-written corpus file (86 reader-vector values, keys of every part type, KvU64 and byte rows, sum/min/max, expiry, delete, two queue rows, a CAS pair): the C++ dump (entries, versionstamps, raw rows) is identical to the TS dump, 230 lines                   | `kv_durable_interop_test.cpp` rebuilds the recorded TS rows (`kv_durable_interop_vectors.inc`) and checks every value and versionstamp, the queue rows untouched, the sweep of the expired row at its time, and the production opener                                                                      |
| (b) C++ writes, TS reads  | the same corpus written by C++: the TS dump equals the C++ dump (230 lines); 96 of 112 `kv.v` are byte-identical to the TS-written file, the rest differ only by Bun's history (an integral double, 5 shared references, a Buffer envelope) or Error creation sites | the C++ write's dump hashes to the recorded sha256 of the TS dump; `kv_runtime_conversion_test.cpp` writes its native fixtures through openKv, as the TS does, and kv-conversion's `verifyNativeSchema(..., "bun-jsc-v1")` and immutable snapshot reader accept them (WAL header, no sidecars after close) |
| (c) mixed processes       | a TS process and a C++ process alternated 25+25 rounds (CAS counter, a shared list cursor each resumed from the other's, enqueue), then 100+100 concurrent CAS increments: counter 50, contended 200, 50 log entries each seen once, 50 queue rows, identical dumps | two C++ processes run the same protocol on one file                                                                                                                                                                                                                                                        |
| (d) MemoryKv suite        |                                                                                                                                                                                                                                                                     | `kv_durable_suite_test.cpp`: kv_test.cpp's 35 store cases over guard(SqliteKv)                                                                                                                                                                                                                             |
| (e) crash safety          |                                                                                                                                                                                                                                                                     | `kv_sqlite_test.cpp`: a C++ child commits pairs until SIGKILLed; every acknowledged commit, no partial one, `integrity_check` ok; `storage_bun_kv_test.cpp`'s SIGKILL case through openKv                                                                                                                  |
| (f) expiry across restart | a TS-written 1.5 s row swept by the C++ open after expiry (data_version +1) and a C++-written one swept by the TS open                                                                                                                                              | `kv_sqlite_test.cpp`, `storage_bun_kv_test.cpp`, the recorded row in (a)                                                                                                                                                                                                                                   |

Values: `kv_jsc_deserialize_test.cpp` reads 86 Bun-serialized values (every KvValue kind, the
forms JSC picks from history, shared references, pool and object-table widths, Buffer envelopes)
and 65 malformed or unusual inputs with Bun's outcome; the C++ writer is byte-identical to Bun for
every canonical vector.

Measured on the way: the native addon's busy timeout is 5 s and its error text exactly
"database is locked"; a VE_BYTES value comes back in TS as a view into the protobuf response
buffer (the store's model, KvBytes, is the viewed bytes).

The later graph codec preserves sparse array entries, cycles and shared references. Its fixtures
cover Bun's recorded array vectors and a sparse allocation guard, and the original cyclic
offline conversion proof passed on the graph-repair revision. Dense JSON projection can
materialize holes and rejects cycles. [Graph repair verification](../findings/cppgw-kv-jsc-graph-model-20261117b.md)

## Operational notes

- Two processes on one file (a TS gateway and a C++ gateway, a gateway and an offline tool) are
  fenced by SQLite's locks and CAS: proven above. Never open the file with two different SQLite
  libraries inside one process. In the Bun probe, bun:sqlite and the addon both held the write
  lock. The C++ gateway uses its single linked SQLite backend.
- A handle's open sweeps expired rows, which is a write: while another process holds the write
  lock, the open (and the first operation) waits, as the native addon's does.
- For cutover, keep a single writer and rehearse native and Bun reads on a consistent stopped
  copy of the marked `.data/kv-bun.sqlite3`. Preserve the latest database and any WAL under
  the existing deployment lock. Offline tools require a stopped source and a new destination.
  [Cutover checklist](../CUTOVER.md)
- The portable C++ gateway statically links SQLite 3.54.0. The historical SDK test used 3.40.1
  and the native addon used 3.53.1. The cross-reader evidence above is scoped to those fixtures.
  [Release linkage](../RUNTIME.md#build-and-package)

## Limits and what is not proven

- No `listenQueue`/dequeue, `watch` or the NAPI protobuf transport (no first-party module calls
  them); the nine NAPI exports of the default interface throw if called.
- Values KvValue cannot hold fail closed on read with TypeError "Serialized value <what> has no
  KvValue representation". Examples include boxed primitives, Float16Array, resizable or
  length-tracking buffers, Blob and DOMException. Nesting beyond 2,000 containers raises RangeError.
- Readers accept serializer versions <= 14 with version-14 rules (Bun 1.4 writes 14).
- A corrupt LE64 row (not 8 bytes) is an error here; the addon's Rust code panics there.
- Read-only database files are detected when a write first fails with SQLITE_READONLY (the addon
  asks SQLite at open); writes then fail with "Can not write to a read-only database.".
- Completion order relative to other macrotasks is not fixed (as with the addon's thread);
  MemoryKv's "exactly one macrotask later" is MemoryKv's.
- Not exercised: the production database itself (host rule), long-running soak under real
  traffic, power loss (SIGKILL only), disks that ignore fsync.
