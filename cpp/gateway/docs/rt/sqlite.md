# uos::rt sqlite: `bun:sqlite`

`#include "uos/rt/sqlite.hpp"` (`src/rt/sqlite.cpp`), namespace `uos::rt::sqlite`. Oracle: Bun
1.4.0 on Linux arm64, whose bun:sqlite JS layer (dumped from the binary: the 20-entry query
cache, `wrapTransaction`, the argument dispatch) and native layer were measured on throwaway
databases. Every expected value in `tests/rt/sqlite_test.cpp` is a line Bun printed for the
same scenario. The C++ runs on the SDK's `libsqlite3.so.0` (**3.40.1**); Bun bundles
**3.53.2**: see "Version differences".

## Inventory (the mandatory surface)

Every use of bun:sqlite in `src/`, `serve.ts`, `lib/jev_compaction/` and `tests/` (none in
`serve.ts` or `lib/`):

| Construct                                                                                                                        | Where                                                                        |
| -------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| `new Database(path)`, `new Database(":memory:")`                                                                                 | tests (authority, native-*, storage-bun-kv, sqlite-readonly)                 |
| `{ strict: true }`                                                                                                               | storage/kv.ts (create the marker table)                                      |
| `{ readonly: true, strict: true }`                                                                                               | storage/sqlite-readonly.ts                                                   |
| `{ readonly: true }`, `{ create: true }`, `{ readonly: true, create: false }`                                                    | storage-bun-kv test; codex/supervisor-inventory.ts                           |
| `{ readonly: true, safeIntegers: true }`, `{ create: true, strict: true, safeIntegers: true }`, `{ safeIntegers: true }`         | kv-conversion/proof.ts, convert.ts; conversion tests                         |
| `new Database(uri, SQLITE_OPEN_READONLY \| SQLITE_OPEN_URI)` with `?immutable=1`                                                 | kv-conversion/convert.ts, kv-runtime-conversion test                         |
| a `file:...?immutable=1` name with options (URI not interpreted)                                                                 | storage/sqlite-readonly.ts fallback                                          |
| `db.query(sql).get() / .all() / .iterate()`, `?.column`                                                                          | everywhere                                                                   |
| `db.query(sql).all(...bindings)` (`string \| number \| null`), `.run()`, `.run(value)`                                           | authority/native-* test adapters (`AuthoritySqlStorage`, `NativeSqlStorage`) |
| `db.prepare(sql).all(n)`, `.all(...ids)`                                                                                         | codex/supervisor-inventory.ts                                                |
| `insert.run(Uint8Array, Buffer, 1n, 9n, 0n, 9500n)` on a cached statement                                                        | convert.ts                                                                   |
| `db.run(sql)`, `db.run(sql, [values])` incl. `bigint`, `Uint8Array`, `9223372036854775807n`                                      | kv.ts, convert.ts, tests                                                     |
| `run("PRAGMA journal_mode=WAL\|DELETE")`, `run("BEGIN IMMEDIATE")`, `COMMIT`, `ROLLBACK`, `CREATE TRIGGER ... RAISE(ABORT, ...)` | convert.ts, tests                                                            |
| `statement.paramsCount`, `"safeIntegers" in statement`, `statement.safeIntegers(true)`                                           | kv-conversion/native-schema.ts                                               |
| `db.transaction(fn)()`, `.immediate()`, nesting through the adapters                                                             | kv.ts, tests                                                                 |
| `db.inTransaction`                                                                                                               | native-runtime-binding test                                                  |
| `db.fileControl(constants.SQLITE_FCNTL_PERSIST_WAL, 0)`                                                                          | sqlite-readonly test                                                         |
| `db.close()`, `db.close(true)`                                                                                                   | everywhere                                                                   |
| `SQLiteError.code === "SQLITE_CANTOPEN"`, messages matched by regex (`/readonly/i`)                                              | sqlite-readonly.ts, tests                                                    |
| row values: bigint (safe integers), `Uint8Array` (`Buffer.from(row.k).toString("hex")`), numbers, strings, null                  | convert.ts, native-schema.ts, proof.ts                                       |

Unused (not provided): `values()`, `raw()`, `columnNames`/`columnTypes`/`declaredTypes`, `as()`,
`toString()`/`toJSON()`, `prepare(sql, params, flags)` arguments, `serialize`/`deserialize`,
`loadExtension`, `setCustomSQLite`, `Database.open`, `handle`, `clearQueryCache`,
`Symbol.dispose`. Provided although unused: object (named) parameters, `deferred`/`exclusive`,
`changes`/`lastInsertRowid` (no call site reads them).

## TypeScript construct -> C++ call

`Database` and `Statement` are handles with JS reference semantics: copies share one object,
`==` is `===`. All members are `const` (they act on the shared object).

| TypeScript                                                                       | C++                                                                                                     |
| -------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `new Database()` / `new Database(path)`                                          | `rt::sqlite::Database db;` / `Database db(path);`                                                       |
| `new Database(path, { readonly: true, strict: true })`                           | `Database db(path, {.readonly = true, .strict = true});`                                                |
| `new Database(uri, constants.SQLITE_OPEN_READONLY \| constants.SQLITE_OPEN_URI)` | `Database db(uri, constants::SQLITE_OPEN_READONLY \| constants::SQLITE_OPEN_URI);`                      |
| `db.filename`                                                                    | `db.filename()`                                                                                         |
| `const s = db.query(sql)` / `db.prepare(sql)`                                    | `const Statement s = db.query(sql);` / `db.prepare(sql)`                                                |
| `s.get()` -> row or null                                                         | `std::optional<Row> row = s.get();`                                                                     |
| `s.get()?.value`                                                                 | `const Value* v = row ? row->get(std::string_view("value")) : nullptr;` then `rt::sqlite::getString(v)` |
| `s.all()` / `s.all(1, "x")` / `s.all(...bindings)`                               | `s.all()` / `s.all(1.0, "x")` / `s.all(bindings)` (`std::vector<Binding>`; empty = no arguments)        |
| `s.get({ $id: 1 })`                                                              | `s.get(rt::sqlite::Named{{"$id", 1.0}})`                                                                |
| `for (const row of s.iterate()) ...`                                             | `for (Row& row : s.iterate()) ...`                                                                      |
| `const it = s.iterate(); it.next()`                                              | `RowIterator it = s.iterate(); std::optional<Row> row = it.next();`                                     |
| `[...s.iterate()]`                                                               | `std::vector<Row> rows; for (Row& row : s.iterate()) rows.push_back(row);`                              |
| `s.run(...)` -> `{ changes, lastInsertRowid }`                                   | `Changes c = s.run(...);` (`c.changes`, `c.lastInsertRowid`: `double` or `std::int64_t`)                |
| `db.run(sql)`, `db.run(sql, [a, b])`, `db.run(sql, a, b)`, `db.exec(...)`        | `db.run(sql)`, `db.run(sql, std::vector<Binding>{a, b})`, `db.run(sql, a, b)`, `db.exec(...)`           |
| `s.paramsCount`, `s.isFinalized`, `s.finalize()`                                 | `s.paramsCount()`, `s.isFinalized()`, `s.finalize()`                                                    |
| `s.safeIntegers()`, `s.safeIntegers(true)`                                       | `s.safeIntegers()`, `s.safeIntegers(true)` (returns the statement)                                      |
| `"safeIntegers" in s && typeof s.safeIntegers === "function"`                    | always true                                                                                             |
| `db.transaction(fn)(a)` / `.deferred(a)` / `.immediate(a)` / `.exclusive(a)`     | `db.transaction(fn)(a)` / `.deferred(a)` / `.immediate(a)` / `.exclusive(a)`; returns `fn`'s result     |
| `tx.database`                                                                    | `tx.database()`                                                                                         |
| `db.inTransaction`                                                               | `db.inTransaction()`                                                                                    |
| `db.fileControl(op, value)` -> number or undefined                               | `std::optional<int> rc = db.fileControl(op, value);`                                                    |
| `db.close()` / `db.close(true)`                                                  | `db.close()` / `db.close(true)`                                                                         |
| `catch (e) { if (e.code === "SQLITE_BUSY") ... }`                                | `catch (const rt::sqlite::SQLiteError& e) { if (e.code == "SQLITE_BUSY") ... }`                         |
| `e.errno`, `e.byteOffset`                                                        | `e.errno_`, `e.byteOffset`                                                                              |
| `Database.MAX_QUERY_CACHE_SIZE`                                                  | `Database::MAX_QUERY_CACHE_SIZE` (20)                                                                   |
| a row as `Record<string, unknown>` JSON                                          | `rt::sqlite::toJson(row)` -> `json::object`                                                             |

```cpp
// src/storage/kv.ts prepareDatabase
const rt::sqlite::Database database(path, {.strict = true});
database.transaction([&] {
  database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
  database.run("INSERT INTO uos_kv_metadata (key, value) VALUES (?, ?)", "codec", BUN_KV_CODEC);
}).immediate();
database.close(true);

// convert.ts transferRows: a snapshot row's bigints and bytes bind back as what they are
const rt::sqlite::Statement insert = output.query("INSERT INTO kv(k,v,v_encoding,version,seq,expiration_ms) VALUES(?,?,?,?,?,?)");
const auto column = [&row](std::string_view name) -> const rt::sqlite::Value& { return *row.get(name); };
insert.run(column("k"), convertedBytes, column("v_encoding"), column("version"), column("seq"), column("expiration_ms"));

// `exec(query, ...bindings).toArray()` of the authority storage adapters
std::vector<json::object> rows;
for (const Row& row : database.query(query).all(bindings)) rows.push_back(rt::sqlite::toJson(row));
```

## Conventions for porters

- bun:sqlite is synchronous: port its calls as plain calls (no `co_await`). Inside a coroutine,
  never write a `Named{...}` or `std::vector<Binding>{...}` literal in a statement that
  contains `co_await` (GCC 12.2 rules, docs/rt/async.md): build it in the previous statement.
- JS numbers bind as `double`, bigints as `std::int64_t`/`rt::BigInt`; a `Value` read from a
  row binds back as what it was (bigint stays bigint). Spell object parameters
  `rt::sqlite::Named{...}` and positional arrays `std::vector<Binding>`; never pass a bare
  braced list.
- Read columns with `row.get(std::string_view("name"))` (nullptr = undefined) and
  `rt::sqlite::getBigInt/getNumber/getString/getBytes/isNull`; convert a JSON-shaped row with
  `rt::sqlite::toJson(row)`.
- `Database`/`Statement` copies are the same object: capture them by value in lambdas and
  callbacks; a `RowIterator` must outlive its range-for.
- Every SQLite use in the process goes through this binding (the URI setting is global; the
  `storage::sqlite_readonly` and `kv_conversion::native_schema` stand-ins now delegate here).

## Values

| SQLite -> JS (`Value` alternative) |                                                                                                                                                               |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| NULL                               | `nullptr`                                                                                                                                                     |
| INTEGER, safe integers off         | `double`: `Number(bigint)` rounding (`9223372036854775807` -> 9223372036854775808)                                                                            |
| INTEGER, safe integers on          | `std::int64_t` (the bigint)                                                                                                                                   |
| REAL                               | `double` (-0 and +-Infinity kept; SQLite stores NaN as NULL)                                                                                                  |
| TEXT                               | `std::string`: UTF-8 decoded like `TextDecoder` with the BOM kept, each ill-formed subpart (WTF-8 lone-surrogate bytes included) -> U+FFFD; embedded NUL kept |
| BLOB                               | `Bytes`: a fresh copy each read; empty BLOB -> empty `Bytes`                                                                                                  |

Safe integers: the database option sets the default of every statement prepared afterwards;
`s.safeIntegers(b)` changes one statement (reads and the bigint range check).

Row keys (`Row` = `rt::OrderedMap<std::string, Value>`): result columns in order; a name used
by several columns keeps the **last** column's value **and position**
(`SELECT 1 AS a, 2 AS b, 3 AS a` -> `{ b: 2, a: 3 }`); array-index names ("0", "10") stay in
column order (Bun stores them as plain properties, `Object.keys` shows column order). Names are
case-sensitive; `__proto__` is an ordinary key.

| JS value -> parameter (`Binding`)                                                | binds                                                                                                                                                                                     |
| -------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `null`, `undefined` (`nullptr`, `std::nullopt`)                                  | NULL                                                                                                                                                                                      |
| `true` / `false` (`bool`)                                                        | INTEGER 1 / 0                                                                                                                                                                             |
| number (`double`)                                                                | INTEGER when an integer in [-2^51, 2^51 - 1] and not -0, else REAL (2^51 -> REAL, -0 -> REAL -0.0, NaN -> NULL)                                                                           |
| bigint (C++ integer types, `rt::BigInt`, a `std::int64_t` `Value`)               | INTEGER; outside int64: RangeError `BigInt value '<n>' is out of range` under safe integers, else wrapped (`BigInt.asIntN(64, n)`)                                                        |
| string (`std::string`, `string_view`, `const char*`)                             | TEXT; well-formed UTF-8 as is; a lone surrogate goes through SQLite's UTF-16 conversion as in Bun (`"a\uD800b"` -> F0 90 81 A2: the surrogate absorbs the next unit; at the end ED A0 80) |
| `Uint8Array`, `Buffer`, any TypedArray/DataView (`Bytes`, `span<const uint8_t>`) | BLOB of the viewed bytes; empty -> X'' (not NULL)                                                                                                                                         |
| `json::value`                                                                    | as the JS value; array/object element -> TypeError `Binding expected string, TypedArray, boolean, number, bigint or null`                                                                 |

A JS number held in a C++ integer binds as INTEGER even beyond 2^51 (Bun: REAL): pass `double`.
SQL text (and filenames) are converted like Bun: a lone surrogate becomes U+FFFD.

## Execution model

- **Argument dispatch** (JS layer): a statement without parameters ignores every argument; a
  call with an empty argument list keeps the previous bindings (`get(...[])`). Positional
  values: all bindings are cleared, then Error `SQLite query expected N values, received M`
  if the count differs (N = highest index, `?3` counts 3). Object: see below.
- **get/all/run** reset first (rebind if given values), step, and reset again at the end, also
  after a failed step. get() steps through every remaining row after reading the first (an
  error in a later row is thrown, the row discarded); run() steps through all rows.
- **iterate** is lazy: arguments bind at the first `next()` _without_ a reset (Error `bad
parameter or other API misuse` when the statement is in the middle of another iteration);
  it never resets at its start (a second iterator continues the first), resets when exhausted,
  not after an error (the next step auto-resets). Breaking out leaves the statement as is.
- **Binding failures**: TypeError (object) and `Missing parameter` keep the values bound before
  the failure; a RangeError (bigint) clears all bindings; an SQLite bind failure is a plain
  Error with SQLite's message.
- **Object parameters**: `$a`/`:a`/`@a` read key `"$a"` (strict: `"a"`; a prefixed key is then
  missing); `?NNN` reads `"?NNN"` (strict: `"<index-1>"`); `?` reads `"<index-1>"`. Extra keys
  are ignored; inherited ones are never read. Non-strict: a missing key leaves NULL. Strict:
  Error `Missing parameter "<name>"`, name = the key read, or `?NNN`, or for `?`:
  `<index-1>` next to named parameters, `?<index>` next to `?NNN`, else `<index>`.
- **changes** = rows changed during the call, triggers included (`sqlite3_total_changes`
  difference; DDL/SELECT 0). **lastInsertRowid** = the connection's last rowid (bigint under
  safe integers: the statement's flag for `s.run`, the database option for `db.run`).
- **db.run(sql, ...)**: before each statement ASCII whitespace and `;` are skipped
  (byteOffset counts from there); parameters bind the first statement only, and their count is
  checked against it even when it has none; a statement whose step fails throws only when its
  text reaches the end of the SQL (`"INSERT dup;"` throws, `"INSERT dup;  "` or
  `"INSERT dup; INSERT ok"` do not: the failure is ignored and the next statement runs); a
  compile error in any statement throws (earlier statements stay executed); a NUL ends the text.
- **Query cache**: `query` keys on the exact SQL text, keeps the 20 most recently used
  (a hit moves to the end); an evicted statement stays usable; a finalized cached statement is
  prepared again. `prepare` never caches.
- **Lifetime**: `close()` finalizes the `query()` and transaction statements and closes with
  `sqlite3_close_v2`; `prepare()` statements keep working on the closed connection (their
  `run` then reports changes 0, lastInsertRowid 0). `close(true)` finalizes every statement
  (also after an earlier `close()`) and uses `sqlite3_close`. A statement finalized by close
  reports `Database has closed`, one finalized by `finalize()` `Statement has finalized`
  (whichever came first). A database whose handles all went away is closed with
  `sqlite3_close_v2` once its last statement is gone.
- **Transactions** (`wrapTransaction`): `BEGIN [DEFERRED|IMMEDIATE|EXCLUSIVE]`, or
  ``SAVEPOINT `\t_bs3.\t` `` when `db.inTransaction`; body; `COMMIT` / `RELEASE`. When the body
  or the commit throws and a transaction is still open: `ROLLBACK`, or `ROLLBACK TO` +
  `RELEASE`, then the original exception is rethrown (a failing COMMIT, e.g. a deferred foreign
  key or SQLITE_BUSY, is rolled back and thrown). The nine statements are prepared when
  `transaction()` is first called on a database (RangeError `Cannot use a closed database` if
  closed); calling the function after `close()` throws `Database has closed`.

## Error model

`SQLiteError` (`name` "SQLiteError", `rt::JsError`): `message` = `sqlite3_errmsg`, `code` = the
name of `sqlite3_extended_errcode` (`resultCodeName`, Bun's table of primary and extended
codes; absent for others), `errno_` = that code, `byteOffset` = `sqlite3_error_offset` (UTF-8
bytes from the start of the text given to SQLite; -1 for run-time errors). Examples (all
measured): `near "SELEC": syntax error` (SQLITE_ERROR 1, offset 0); `UNIQUE constraint failed:
t.id` (SQLITE_CONSTRAINT_PRIMARYKEY 1555); `NOT NULL constraint failed: t.n` (1299); `CHECK
constraint failed: c > 0` (275); `FOREIGN KEY constraint failed` (787); a trigger's
`RAISE(ABORT, 'msg')` -> `msg` (SQLITE_CONSTRAINT_TRIGGER 1811); `cannot store TEXT value in
INTEGER column s.x` (3091); `database is locked` (SQLITE_BUSY 5); `attempt to write a readonly
database` (8); `unable to open database file` (SQLITE_CANTOPEN 14); `file is not a database`
(26); `bad parameter or other API misuse` (SQLITE_MISUSE 21, open flags without READONLY or
READWRITE or with both).

Plain errors, exact text: Error `SQL query cannot be empty.` (query "", checked before
closed); RangeError `Cannot use a closed database` (query/prepare/transaction on a closed db);
RangeError `Invalid SQL statement` (prepare ""); Error `Statement has finalized` (text without a
statement, at query/prepare; a finalized statement); Error `Database has closed` (run,
inTransaction, statements closed by close); Error `SQL string mustn't be blank` (run "");
Error `Query contained no valid SQL statement; likely empty query.`; Error `SQLite query
expected N values, received M`; Error `Missing parameter "x"`; TypeError `Binding expected
string, TypedArray, boolean, number, bigint or null`; RangeError `BigInt value 'n' is out of
range`; Error `Cannot open an anonymous database in read-only mode.` A failure to load
`libsqlite3.so.0` or one of its symbols is `std::runtime_error` (a deployment fault).

## Opening and URIs

Options -> flags exactly as Bun's constructor: `readonly` -> READONLY; `create` ->
READWRITE|CREATE; `readwrite` adds READWRITE; `strict`/`safeIntegers` present and no flag yet ->
READWRITE|CREATE; `{}` -> 0 -> SQLITE_MISUSE. Missing file: CANTOPEN with `readonly`,
`readwrite` or numeric READONLY/READWRITE; created with no options, `create` or `strict`. A
directory: CANTOPEN read-write, SQLITE_IOERR_READ 266 read-only. A non-database file opens and
fails at the first statement that reads it (NOTADB). The filename is trimmed like
`String.prototype.trim`; `""` and `":memory:"` open in memory (`filename()` keeps `""`).

URIs: the SDK's SQLite is built with SQLITE_USE_URI=1; the binding turns URI handling off
(`sqlite3_config(SQLITE_CONFIG_URI, 0)` before the library initializes) so, as in Bun, a
`file:` name is a plain path (`file:x.db?mode=ro` creates that file; sqlite-readonly.ts's
immutable fallback gets CANTOPEN) unless `SQLITE_OPEN_URI` is passed. Every SQLite user in the
process must go through this binding for that setting to hold.

## Version differences (SDK 3.40.1 vs Bun's 3.53.2), measured

- REAL -> TEXT (`CAST(x AS TEXT)`, `x || ''`, `hex(real)`, `quote`): 3.40.1 prints 15
  significant digits (`0.333333333333333`, `9.00719925474099e+15`, `2.25179981368525e+15`);
  3.53.2 prints a round-trip form (`0.33333333333333332`, `9007199254740992.0`). Values read
  as REAL are identical; only SQL-side text rendering differs. Pinned in the test (`VERSION`).
- A statement that names no table (`SELECT 1`) compiles and runs on a non-database file in
  3.40.1; 3.53.2 reads the schema first and fails with SQLITE_NOTADB.
- Functions added after 3.40 are "no such function" in 3.40.1 (measured: `concat`,
  `octet_length`; likewise `concat_ws`, `string_agg`, `timediff`, `jsonb_*`). The gateway's SQL
  uses only `json_extract`, `json_remove`, `MIN`, `COUNT`, `RETURNING`, `ON CONFLICT ... DO
UPDATE`, `WITHOUT ROWID`, triggers and CHECK, all in 3.40.1.
- Build options that differ: SDK `MAX_FUNCTION_ARG=127` (Bun 1000), `MAX_PAGE_COUNT=1073741823` (Bun 0xfffffffe),
  `MAX_DEFAULT_PAGE_SIZE=32768` (Bun 8192), `SECURE_DELETE` (deleted content is zeroed on disk),
  `ENABLE_LOAD_EXTENSION` (the SQL function still answers "not authorized"), `USE_URI` (turned
  off at run time, above). The PRAGMA defaults the gateway can observe are equal (synchronous
  2, journal_mode, page_size 4096, cache_size -2000, foreign_keys 0, recursive_triggers 0,
  trusted_schema 1, busy_timeout 0, temp_store 0, auto_vacuum 0), as are double-quoted string
  literals and `MAX_VARIABLE_NUMBER=250000`.
- Every error line recorded for the test suite (compile, constraint, trigger, run-time, open,
  busy, read-only and binding failures, with their codes and byte offsets) came out the same
  on 3.40.1.

## Limits and behaviour not reproduced

- `s.get([])` (an explicit empty array clears the bindings) is not expressible: an empty
  `std::vector<Binding>` is `get(...[])` (keeps them). No call site passes `[]`.
- Bun's `lastInsertRowid` for a statement whose `safeIntegers(false)` overrides a
  `safeIntegers: true` database is a garbage number (a bigint read as a double); the binding
  returns the rowid as a number.
- Non-ASCII leading characters in `db.run` text (U+0085, U+FEFF) are not skipped/handled as
  Bun does; the gateway's SQL is ASCII.
- `rt::sqlite::toJson(row)` throws TypeError for bigint and BLOB columns (a JS row would carry
  them as `unknown`); rt's JSON enumeration puts array-index keys first, Bun's rows keep column
  order (no gateway column is named like an index).
- A `Database` whose last handle is dropped without `close()` closes when its last statement is
  gone (Bun closes only on `close()`/GC); open transactions then roll back.
- Not provided (no call site): see the inventory.

## Migration of the stand-ins

(a) **`storage::sqlite_readonly` — done.** `SQLiteError` is `rt::sqlite::SQLiteError`;
`constants::SQLITE_FCNTL_PERSIST_WAL` is rt's; `Database`/`Statement` keep their API
(json::object rows, boolean options, BLOB -> `std::logic_error`) and delegate to rt (511 ->
221 lines). Its callers (`src/storage/kv.cpp`, `tests/sqlite_readonly_test.cpp`,
`tests/storage_bun_kv_test.cpp`) are unchanged. Behaviour that moved to Bun's (no test pins
the old one): `query("")` after close reports the empty text; repeated column names follow
the last-column rule; `run` follows Bun's multi-statement rules and `changes` counting; a
statement outliving its `Database` keeps working; the query cache is the 20-entry LRU; text
without a statement throws at `query`; lone surrogates in SQL/filenames become U+FFFD.

(b) **`kv_conversion::native_schema::bun_sqlite` — done.** `Value` and `Row` are now
`rt::sqlite::Value`/`Row` (the identical types); the abstract `Statement`/`Database` stay for
the test fakes; `bun_sqlite::adapt(rt::sqlite::Database)` returns the interface over the real
binding. Verified with a temporary test (deleted): `createNativeSnapshotReader(adapt(
Database("file://.../private # % ?.sqlite3?immutable=1", READONLY | URI)))`, `verifyNativeSchema`,
exact bigints through get/all/iterate, `validateNativeRow`, the parameter and readonly errors,
no sidecars. convert.ts ports `createNativeSnapshotReader(new Database(uri.href, flags))` as
`createNativeSnapshotReader(bun_sqlite::adapt(rt::sqlite::Database(uri.href(), flags)))`. Where
it hands its output `Database` itself to `verifyNativeSchema`/`digestRows` (a `NativeReader`
without the `safeIntegers(true)` call: the database option provides it), build the reader from
the statement directly:

```cpp
NativeReader reader;
reader.query = [output](const std::string& sql) {
  const rt::sqlite::Statement statement = output.query(sql);
  NativeReadStatement read;
  read.get = [statement] { return statement.get(); };
  read.all = [statement] { return statement.all(); };
  read.iterate = [statement]() -> bun_sqlite::RowIterator {
    auto rows = std::make_shared<rt::sqlite::RowIterator>(statement.iterate());
    return [rows] { return rows->next(); };
  };
  return read;
};
```

`tests/kv_runtime_conversion_test.cpp` keeps its fake: its real-database case needs `openKv`
(no native KV store in C++) and the other removes the `safeIntegers` method, which a real
statement always has.

(c) **authority durable_store on the real binding — plan, verified.** `DurableAuthorityStore`
needs no change (it takes `AuthoritySqlStorage`). In `tests/authority_durable_test.cpp`
(owner's file):

1. Replace the "SQLite stand-in" block (`FakeFile`, `bindingJson`, `CREATE`, the stand-in
   `Database`) by a `Database` class with the same members over `rt::sqlite::Database`:
   `all(query, bindings)` = `database.query(query).all(converted)` with each row through
   `rt::sqlite::toJson`; `run(query, bindings)` = `database.query(query).run(converted)`;
   `get(query)` = `database.query(query).get()` through `toJson`; `transaction(callback)` =
   `database.transaction(callback)()`; `close()`. `converted` maps each
   `AuthoritySqlStorage::Binding` with `std::visit([](const auto& v) { return
rt::sqlite::Binding(v); }, binding)`. `Storage` and `storage()` stay as they are.
2. The three `onDisk` cases: an RAII directory from `rt::fs::mkdtempSync(rt::path::join(
rt::os::tmpdir(), "uos-authority-durable-"))` holding `authority.sqlite`, removed with
   `rmSync({.recursive = true, .force = true})`; `std::make_shared<Database>(path)` replaces
   `std::make_shared<Database>(file)` (also for the reopen after `close()`).
3. The other seven cases: `std::make_shared<Database>(":memory:")`.
4. Include `uos/rt/sqlite.hpp`, `uos/rt/fs.hpp`, `uos/rt/process.hpp`; drop the stand-in
   paragraph of the header comment.
   Run with the same sources: 10/10 passed on the real binding (a temporary copy, deleted).
   The native authority tests (`native-authority`, `native-principal-control`,
   `native-runtime-binding`) use the same adapter shape (`query(q).all(...bindings)`,
   `transaction(cb)()`) when ported.

## Loading: dlopen versus linking

The binding declares the 34 C functions it uses and resolves them with `dlopen("libsqlite3.so.0")`
on first use, because the SDK has no `sqlite3.h` and no `libsqlite3.so` link name and
`tools/env.sh` links no SQLite. Linking `$UOS_TARGET_LIB/libsqlite3.so.0` directly would fail
at start-up instead of first use but still needs the hand-written declarations; adding
`sqlite3.h` to the SDK would let the compiler check them. The better long-term fix is to
build SQLite 3.53.2 (Bun's version, with Bun's compile options) into the gateway: every
difference listed above would disappear.
