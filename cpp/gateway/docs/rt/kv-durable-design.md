# Design: a durable C++ KV store for the production Bun database

This is the historical design memo. The linked C++ store is the current implementation. [Durable KV](kv-durable.md)

Status: implemented as option C (`rt::SqliteKv`, `rt::kvJscDeserialize`); `kv-durable.md` says
what was built, how it differs from this plan (the queue is kept and written, not refused) and the
compatibility evidence. This page stays as the design record.

Sources: `src/storage/{kv,native-kv,native-addon,sqlite-readonly,value-compatibility,
key-compatibility}.ts`, `src/storage/patches/deno-kv-0.14.0.patch`, the @deno/kv 0.14.0
SDK (`esm/{proto_based,kv_util,kv_key,napi_based}.js`), `src/kv-conversion/*.ts`,
`tools/native-kv/*`, `src/runtime/native-kv-bootstrap.ts`, AGENTS.md "Runtime and Migration
Scope". Facts marked _measured_ come from throwaway databases created under bun with the
published addon; no production database was opened.

## 1. What the TypeScript does today

```
ported module --rt::Kv--> guard (native-kv.ts) --> SDK (validation, key codec, cursors,
list batching, value packing) --NAPI, protobuf--> native-kv.node (denokv 9fe65cc sqlite
backend + rollback patch) --> .data/kv-bun.sqlite3
```

- `openKv(path)` (src/storage/kv.ts): rejects empty and http(s) paths; loads the addon only
  through `preparedNativeKvBackend()` (provenance-verified, see section 6); for a file path:
  `lstat` must be a regular file (symlinks refused), then `verifyCodec` reads
  `uos_kv_metadata.codec` through a read-only connection (falling back to an `immutable=1`
  URI only for a sidecar-free, stable, stopped file) and requires `bun-jsc-v1`, else Error
  "KV codec is missing or incompatible; convert the legacy database offline before opening
  it with Bun". A missing file is created exclusively (`O_EXCL`, mode 0600) and gets
  `CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID`
  with rows `codec=bun-jsc-v1` and `created_bun_version=<Bun.version>` in one IMMEDIATE
  transaction, before the native backend creates its own tables. The whole open is retried
  on "database is locked" for 5 s.
- Values: `encodeV8 = assertPortableKvValue + node:v8 serialize` (JavaScriptCore bytes),
  `decodeV8 = deserialize`. Bun's codec is not V8's; AGENTS.md treats them as
  incompatible and only a codec-marked file may be opened. The legacy `.data/kv.sqlite3`
  stays untouched; `src/kv-conversion` converts stopped databases offline in either
  direction.
- The guard adds no locking of its own: it retries "database is locked" for reads, list
  pages and commits (5 s, 10 ms sleeps), maps the backend's arithmetic type failure to
  TypeError "Atomic arithmetic requires an absent key or KvU64 value", turns `set`/`delete`
  into checked atomic commits, and fixes the prefix-list start (`[...prefix, Uint8Array()]`).

## 2. The file a compatible store must read and write

Schema (`src/kv-conversion/native-schema.ts` verifies exactly this; normalized SQL text
must match, so create it with these statements or denokv's migrations, which normalize to
the same text):

```sql
CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0);
CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL, v_encoding INTEGER NOT NULL, version INTEGER NOT NULL,
                 seq INTEGER NOT NULL DEFAULT 0, expiration_ms INTEGER NOT NULL DEFAULT -1) WITHOUT ROWID;
CREATE INDEX kv_expiration_ms_idx ON kv (expiration_ms);
CREATE TABLE migration_state (k INTEGER NOT NULL PRIMARY KEY, version INTEGER NOT NULL);   -- one row (0, 3)
CREATE TABLE queue (ts INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL,
                    keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (ts, id));
CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL,
                    backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (deadline, id));
CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
```

- `PRAGMA user_version = 0`, `application_id = 0`; journal mode WAL, `synchronous` FULL,
  page size 4096 (_measured_). `data_version` holds one row `k = 0`.
- `kv.k`: the SDK tuple encoding of the key (`rt::packKey`, byte order = list order).
  The converter additionally rejects legacy negative-NaN number parts
  (`assertPortableKvKeyBytes`).
- `kv.v` / `v_encoding`: 1 = JSC SerializedScriptValue (version 14 header `0e 00 00 00`,
  string constant pool, see `rt::kvJscSerialize`), 2 = KvU64 as 8 little-endian bytes,
  3 = raw bytes of a top-level Uint8Array. Encoding 2 must be exactly 8 bytes.
- `kv.version`: the commit version of the row's last write (arithmetic writes included);
  versionstamp = big-endian version (8 bytes) + 2 zero bytes, shown as 20 hex digits. Each
  committed atomic write advances `data_version.version` by a random step (1..9 _measured_);
  failed checks and rolled-back writes do not advance it. `kv.seq` and
  `data_version.seq` were 0 in every _measured_ row; preserve whatever is stored.
- `kv.expiration_ms`: absolute epoch milliseconds (`Date.now() + expireIn` at commit), -1
  without expiry; a plain set or any arithmetic write resets it to -1. Expired rows stay
  visible to reads, checks and lists until a sweep deletes `expiration_ms <= now`: at open
  and about every 60 s per open handle (_measured_ 60.9 s).
- Queues: the gateway never enqueues; the converter refuses non-empty queue tables. A C++
  store keeps the tables and should refuse to open with queued rows.

## 3. Semantics to keep (already specified by MemoryKv)

Everything in docs/rt/kv.md "Store semantics" applies unchanged: SDK validation, native
limits and their order, the transaction (checks, then mutations in order, one savepoint per
atomic write; the rollback patch restores earlier mutations, expiry changes and the
version increment of a failed write), list batching and cursor format (persisted by the
sentinel retention ledger and translated by `kv-conversion/cursors.ts`), and close. The
durable store should reuse that layer: MemoryKv's validation, list generator and commit
path are written against a row map, and only the row access changes.

## 4. Concurrency and durability

- One process, one handle: the gateway keeps a single KV handle (tools/native-kv README);
  concurrent callers share it, and CAS is the only cross-request fence.
- Other processes may open the same file (tests run two Bun workers; offline tools read
  it). SQLite reports "database is locked" instead of waiting; callers retry for 5 s.
- WAL: a stopped database is "drained and checkpointed" when the `-wal` file is empty or
  absent (the converter's precondition). AGENTS.md: durable request writes must drain
  before telemetry and KV closure; never convert a live database.
- An acknowledged commit survives SIGKILL (storage-bun-kv test): commit with
  `synchronous=FULL` in WAL mode before resolving.
- The native addon resolves on a worker thread; the C++ store should run SQLite calls off
  the event-loop thread (or accept blocking) and post completions back, which keeps
  MemoryKv's "completes one macrotask later" contract.

## 5. Options

A. Load `native-kv.node` from C++. It is a Node-API module: it needs a Node-API host
(napi_* symbols, an event loop, threadsafe functions) and protobuf request bytes. A C++
gateway would have to implement that host surface. Rejected.

B. Build the denokv SQLite backend (Rust, commit 9fe65cc + rollback patch) as a static
library behind a small C ABI. Byte-identical behaviour for free, but a second Rust
artifact with its own provenance pipeline, protobuf or C structs at the boundary, and
Rust/C++ async integration. Viable fallback.

C. Re-implement the backend in C++ over SQLite (`libsqlite3.so.0` is in the SDK sysroot;
`sqlite3.h` is not, so either declare the ~20 C API functions used, which are ABI
stable, or add the header to the SDK). Work: schema creation/verification, the codec
marker, snapshot reads, atomic writes with savepoints, the sweeper, busy handling, and a
JSC deserializer (the serializer exists). Recommended.

## 6. How the native addon fits

`tools/native-kv` pins denokv `9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5` with
`atomic-write-rollback.patch` (savepoint per atomic write) and `napi-workspace.patch`,
builds `native-kv.node` with Rust 1.98.1 into a provenance record, and
`native-addon.ts` loads it only after verifying the release-bound provenance hash, the
frozen inputs, the artifact hash and its nine NAPI exports (`open`, `close`, `snapshotRead`,
`atomicWrite`, `dequeueNextMessage`, `finishMessage`, `startWatch`,
`dequeueNextWatchMessage`, `endWatch`). Option C removes the addon from the C++ process; the
TS deployment keeps it, and both must agree on the file format above. Option B would reuse
the same source and patches through the same provenance discipline.

## 7. Risks

- Value codec. TS-written rows use whatever form JSC chose (int32 or double for the same
  number, 8- or 16-bit strings, object references for shared or cyclic graphs, Bun's
  `ff 42 55 4e` envelope for a nested Buffer, Error line/column/URL/stack). The reader must
  accept every form; the writer emits the canonical form, which Bun reads back as the same
  JS value. Cycles have no `KvValue` representation: extend the model or fail closed.
- Format drift. The marker `bun-jsc-v1` pins Bun 1.4.0's serializer version 14; a Bun
  upgrade that changes it needs a new marker and a conversion.
- Version monotonicity across implementations: always `data_version + step` inside the
  write transaction, never a cached value.
- Expiry: sweep with exactly `expiration_ms <= now`; never on read.
- Schema text: the converter compares normalized `sqlite_master.sql`; a C++-created file
  with different DDL text is refused by the conversion tools.
- Locking: SQLite busy errors under multi-process access must keep the TS messages so the
  ported guard logic stays identical.

## 8. Tests that would prove compatibility

1. Schema and marker: a C++-created file passes `openKv` (codec check) and
   `verifyNativeSchema`; a TS-created file opens in C++; legacy, codec-mismatched and
   symlinked files are refused byte-for-byte unchanged (port of storage-bun-kv's file
   tests).
2. Row interop corpus (the vectors in tests/rt/kv_test.cpp plus a larger corpus generated
   the same way, with bun's `serialize`): TS writes, C++ reads equal `KvValue`s; C++ writes, TS reads `deepEqual` values;
   canonical values are byte-identical in `kv.v`.
3. The whole MemoryKv suite run against the durable store through `rt::Kv`
   (parameterize the test bodies by store).
4. Two processes (one Bun worker, one C++ worker) incrementing one counter under CAS:
   every increment lands (port of "separate Bun processes preserve each quota increment").
5. Cursors written by one implementation continue in the other; persisted sentinel cursors
   survive `kv-conversion` round trips (`digestRows` equal).
6. Expiry across restart, sweep timing, and SIGKILL after an acknowledged commit.
