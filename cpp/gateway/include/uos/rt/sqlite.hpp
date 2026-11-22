#pragma once
// bun:sqlite (`Database`, `Statement`, `SQLiteError`, `constants`) as Bun 1.4.0 behaves on
// Linux, over the SQLite C library. The SDK ships libsqlite3.so.0 (3.40.1) but no sqlite3.h
// and no link library, so the library is loaded with dlopen on first use and the C API
// functions are declared here; Bun links its own SQLite 3.53.2. docs/rt/sqlite.md has the
// TS -> C++ table, the value mapping, the error model and the version differences.
//
// Object model: Database and Statement are handles with JavaScript reference semantics
// (copies share one object; `==` is identity). A statement keeps the SQLite connection
// alive, so statements may be used after `db.close()` exactly where Bun allows it.
// Strings are WTF-8 (docs/rt/text.md); SQL text is converted like Bun converts it.
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt::sqlite {
  using Bytes = std::vector<std::uint8_t>;

  // ---- values read from SQLite -------------------------------------------------------

  // One column value as bun:sqlite returns it: NULL -> null (nullptr); INTEGER -> a bigint
  // (std::int64_t) when the statement uses safe integers, else a number (double, rounded to
  // nearest like Number(bigint)); REAL -> number (double, -0 and infinities kept); TEXT ->
  // string (UTF-8 decoded like TextDecoder with the BOM kept: each ill-formed subpart becomes
  // U+FFFD); BLOB -> a fresh Uint8Array (Bytes).
  using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string, Bytes>;

  // A row object: column name -> value. Bun's key order: the result columns in order, and a
  // name used by several columns appears once, with the value and at the position of its
  // last column (`SELECT 1 AS a, 2 AS b, 3 AS a` -> { b: 2, a: 3 }). Array-index names
  // ("0", "1") keep column order too (Bun stores them as plain properties).
  using Row = OrderedMap<std::string, Value>;

  // `row.k` typed reads (nullptr: the row has no such column or it holds another type).
  inline bool isNull(const Value* value) noexcept { return value != nullptr && std::holds_alternative<std::nullptr_t>(*value); }
  inline const std::int64_t* getBigInt(const Value* value) noexcept { return value ? std::get_if<std::int64_t>(value) : nullptr; }
  inline const double* getNumber(const Value* value) noexcept { return value ? std::get_if<double>(value) : nullptr; }
  inline const std::string* getString(const Value* value) noexcept { return value ? std::get_if<std::string>(value) : nullptr; }
  inline const Bytes* getBytes(const Value* value) noexcept { return value ? std::get_if<Bytes>(value) : nullptr; }

  // A row read as the JSON-shaped `Record<string, unknown>` the TS treats it as: null,
  // numbers (canonical, rt::makeNumber) and strings. A bigint or Uint8Array column has no
  // json::value form: TypeError "bun:sqlite value is a bigint, not JSON" / "... a Uint8Array, not JSON".
  json::value toJson(const Value& value);
  json::object toJson(const Row& row);

  // ---- bound parameters --------------------------------------------------------------

  // One JavaScript value passed to get/all/iterate/run, bound like Bun binds it:
  //   null, undefined (std::nullopt)    -> NULL
  //   boolean (bool)                    -> INTEGER 1 / 0
  //   number (double)                   -> INTEGER when it is an integer in [-2^51, 2^51 - 1]
  //                                        and not -0, else REAL (NaN -> NULL in SQLite)
  //   bigint (C++ integers, rt::BigInt) -> INTEGER; outside the int64 range: RangeError
  //                                        "BigInt value '<n>' is out of range" under safe
  //                                        integers, else wrapped modulo 2^64 (asIntN(64))
  //   string (WTF-8)                    -> TEXT; a lone surrogate goes through SQLite's own
  //                                        UTF-16 conversion, as Bun binds UTF-16 strings
  //   Uint8Array / any TypedArray view  -> BLOB of its bytes (an empty view is X'')
  // A column Value binds as what it is (std::int64_t = bigint, double = number). A
  // json::value binds as the JS value it describes (numbers as numbers); an array or object
  // element throws TypeError "Binding expected string, TypedArray, boolean, number, bigint
  // or null" when it is bound. A JS number held in a C++ integer binds as INTEGER even
  // beyond 2^51 (Bun: REAL); pass such numbers as double.
  class Binding {
  public:
    Binding(std::nullptr_t) noexcept : value_(nullptr) {}
    Binding(std::nullopt_t) noexcept : value_(nullptr) {}
    Binding(bool value) noexcept : value_(value) {}
    Binding(double value) noexcept : value_(value) {}
    template <class T>
      requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
               !std::is_same_v<T, char32_t> && !std::is_same_v<T, wchar_t>)
    Binding(T value) : value_(std::int64_t{0}) {
      if constexpr (std::is_signed_v<T>) {
        value_ = static_cast<std::int64_t>(value);
      } else if (static_cast<std::uint64_t>(value) <= static_cast<std::uint64_t>(INT64_MAX)) {
        value_ = static_cast<std::int64_t>(value);
      } else {
        value_ = BigInt::fromUint64(static_cast<std::uint64_t>(value));
      }
    }
    Binding(const BigInt& value);
    Binding(std::string value) : value_(std::move(value)) {}
    Binding(std::string_view value) : value_(std::string(value)) {}
    Binding(const char* value) : value_(std::string(value)) {}
    Binding(Bytes value) : value_(std::move(value)) {}
    Binding(std::span<const std::uint8_t> value) : value_(Bytes(value.begin(), value.end())) {}
    Binding(const Value& value);
    Binding(const json::value& value);
    template <class T>
    Binding(const std::optional<T>& value) : Binding(value ? Binding(*value) : Binding(nullptr)) {}

  private:
    // A json::value array or object: not bindable.
    struct Unbindable {};
    friend struct BindingAccess;
    std::variant<std::nullptr_t, bool, double, std::int64_t, BigInt, std::string, Bytes, Unbindable> value_;
  };

  // A JavaScript object of named parameters: `{ $id: 1, ":name": "x" }`, or `{ id: 1 }` on a
  // `strict` database. Keys are property names; a `?` or `?NNN` parameter reads the key
  // "<index - 1>" (docs/rt/sqlite.md has the full rule).
  using Named = OrderedMap<std::string, Binding>;

  // `{ changes, lastInsertRowid }` of run(): changes is the number of rows changed by the
  // run, triggers included (sqlite3_total_changes difference); lastInsertRowid is the
  // connection's last rowid, a bigint (std::int64_t) under safe integers.
  struct Changes {
    double changes = 0;
    std::variant<double, std::int64_t> lastInsertRowid = 0.0;
  };

  // bun:sqlite's SQLiteError: an Error named "SQLiteError" with SQLite's message, `code` the
  // name of the extended result code ("SQLITE_CONSTRAINT_PRIMARYKEY"; absent for a code Bun
  // does not name), `errno` that code (1555) and `byteOffset` the UTF-8 byte offset of the
  // failing token in the SQL text handed to SQLite (-1 when none).
  struct SQLiteError : JsError {
    std::optional<std::string> code;
    int errno_ = 0;
    int byteOffset = -1;
    SQLiteError(std::string message_, std::optional<std::string> code_, int errnoValue, int byteOffset_);
  };

  // The name bun:sqlite gives an extended or primary result code, std::nullopt for others.
  std::optional<std::string> resultCodeName(int code);

  // `import { constants } from "bun:sqlite"`
  namespace constants {
    inline constexpr int SQLITE_OPEN_READONLY = 1;
    inline constexpr int SQLITE_OPEN_READWRITE = 2;
    inline constexpr int SQLITE_OPEN_CREATE = 4;
    inline constexpr int SQLITE_OPEN_DELETEONCLOSE = 8;
    inline constexpr int SQLITE_OPEN_EXCLUSIVE = 16;
    inline constexpr int SQLITE_OPEN_AUTOPROXY = 32;
    inline constexpr int SQLITE_OPEN_URI = 64;
    inline constexpr int SQLITE_OPEN_MEMORY = 128;
    inline constexpr int SQLITE_OPEN_MAIN_DB = 256;
    inline constexpr int SQLITE_OPEN_TEMP_DB = 512;
    inline constexpr int SQLITE_OPEN_TRANSIENT_DB = 1024;
    inline constexpr int SQLITE_OPEN_MAIN_JOURNAL = 2048;
    inline constexpr int SQLITE_OPEN_TEMP_JOURNAL = 4096;
    inline constexpr int SQLITE_OPEN_SUBJOURNAL = 8192;
    inline constexpr int SQLITE_OPEN_SUPER_JOURNAL = 16384;
    inline constexpr int SQLITE_OPEN_NOMUTEX = 32768;
    inline constexpr int SQLITE_OPEN_FULLMUTEX = 65536;
    inline constexpr int SQLITE_OPEN_SHAREDCACHE = 131072;
    inline constexpr int SQLITE_OPEN_PRIVATECACHE = 262144;
    inline constexpr int SQLITE_OPEN_WAL = 524288;
    inline constexpr int SQLITE_OPEN_NOFOLLOW = 16777216;
    inline constexpr int SQLITE_OPEN_EXRESCODE = 33554432;
    inline constexpr int SQLITE_PREPARE_PERSISTENT = 1;
    inline constexpr int SQLITE_PREPARE_NORMALIZE = 2;
    inline constexpr int SQLITE_PREPARE_NO_VTAB = 4;
    inline constexpr int SQLITE_DESERIALIZE_READONLY = 4;
    inline constexpr int SQLITE_FCNTL_LOCKSTATE = 1;
    inline constexpr int SQLITE_FCNTL_GET_LOCKPROXYFILE = 2;
    inline constexpr int SQLITE_FCNTL_SET_LOCKPROXYFILE = 3;
    inline constexpr int SQLITE_FCNTL_LAST_ERRNO = 4;
    inline constexpr int SQLITE_FCNTL_SIZE_HINT = 5;
    inline constexpr int SQLITE_FCNTL_CHUNK_SIZE = 6;
    inline constexpr int SQLITE_FCNTL_FILE_POINTER = 7;
    inline constexpr int SQLITE_FCNTL_SYNC_OMITTED = 8;
    inline constexpr int SQLITE_FCNTL_WIN32_AV_RETRY = 9;
    inline constexpr int SQLITE_FCNTL_PERSIST_WAL = 10;
    inline constexpr int SQLITE_FCNTL_OVERWRITE = 11;
    inline constexpr int SQLITE_FCNTL_VFSNAME = 12;
    inline constexpr int SQLITE_FCNTL_POWERSAFE_OVERWRITE = 13;
    inline constexpr int SQLITE_FCNTL_PRAGMA = 14;
    inline constexpr int SQLITE_FCNTL_BUSYHANDLER = 15;
    inline constexpr int SQLITE_FCNTL_TEMPFILENAME = 16;
    inline constexpr int SQLITE_FCNTL_MMAP_SIZE = 18;
    inline constexpr int SQLITE_FCNTL_TRACE = 19;
    inline constexpr int SQLITE_FCNTL_HAS_MOVED = 20;
    inline constexpr int SQLITE_FCNTL_SYNC = 21;
    inline constexpr int SQLITE_FCNTL_COMMIT_PHASETWO = 22;
    inline constexpr int SQLITE_FCNTL_WIN32_SET_HANDLE = 23;
    inline constexpr int SQLITE_FCNTL_WAL_BLOCK = 24;
    inline constexpr int SQLITE_FCNTL_ZIPVFS = 25;
    inline constexpr int SQLITE_FCNTL_RBU = 26;
    inline constexpr int SQLITE_FCNTL_VFS_POINTER = 27;
    inline constexpr int SQLITE_FCNTL_JOURNAL_POINTER = 28;
    inline constexpr int SQLITE_FCNTL_WIN32_GET_HANDLE = 29;
    inline constexpr int SQLITE_FCNTL_PDB = 30;
    inline constexpr int SQLITE_FCNTL_BEGIN_ATOMIC_WRITE = 31;
    inline constexpr int SQLITE_FCNTL_COMMIT_ATOMIC_WRITE = 32;
    inline constexpr int SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE = 33;
    inline constexpr int SQLITE_FCNTL_LOCK_TIMEOUT = 34;
    inline constexpr int SQLITE_FCNTL_DATA_VERSION = 35;
    inline constexpr int SQLITE_FCNTL_SIZE_LIMIT = 36;
    inline constexpr int SQLITE_FCNTL_CKPT_DONE = 37;
    inline constexpr int SQLITE_FCNTL_RESERVE_BYTES = 38;
    inline constexpr int SQLITE_FCNTL_CKPT_START = 39;
    inline constexpr int SQLITE_FCNTL_EXTERNAL_READER = 40;
    inline constexpr int SQLITE_FCNTL_CKSM_FILE = 41;
    inline constexpr int SQLITE_FCNTL_RESET_CACHE = 42;
  }

  // The `options` object of `new Database(filename, options)`. A member is present
  // (`"strict" in options`) when it holds a value; readonly/create/readwrite/strict/
  // safeIntegers are read for truthiness as Bun does.
  struct DatabaseOptions {
    std::optional<bool> readonly{};
    std::optional<bool> create{};
    std::optional<bool> readwrite{};
    std::optional<bool> strict{};
    std::optional<bool> safeIntegers{};
  };

  class Database;
  class RowIterator;

  // A prepared statement (`db.query(sql)` / `db.prepare(sql)`).
  //
  // Execution follows Bun: get/all/run reset the statement first (and rebind when given
  // parameters) and again when they finish, also after a failed step; get() reads the first
  // row and then steps through the remaining rows, so an error in a later row is thrown;
  // run() steps through every row; iterate() never resets at its start (it continues an
  // unfinished iteration) and resets when the rows are exhausted, not after an error.
  // Parameters given to a call replace every binding (the others become NULL); a call
  // without parameters keeps the previous bindings. On a statement without parameters every
  // argument is ignored. An empty positional list is a call without parameters (`get(...[])`).
  //
  // After finalize(): Error "Statement has finalized". After the database closed it: Error
  // "Database has closed" (db.close() finalizes the query() statements, db.close(true)
  // every statement; a prepare() statement survives db.close() and keeps working).
  class Statement {
  public:
    struct Impl;
    explicit Statement(std::shared_ptr<Impl> impl_);

    // `statement.get()`, `get(...params)`, `get({ $a: 1 })`: the first row, std::nullopt for null.
    std::optional<Row> get() const;
    std::optional<Row> get(const std::vector<Binding>& params) const;
    std::optional<Row> get(const Named& params) const;
    template <class... A>
      requires(sizeof...(A) >= 1 && (std::is_constructible_v<Binding, A&&> && ...))
    std::optional<Row> get(A&&... params) const {
      return get(std::vector<Binding>{Binding(std::forward<A>(params))...});
    }

    // `statement.all(...)`
    std::vector<Row> all() const;
    std::vector<Row> all(const std::vector<Binding>& params) const;
    std::vector<Row> all(const Named& params) const;
    template <class... A>
      requires(sizeof...(A) >= 1 && (std::is_constructible_v<Binding, A&&> && ...))
    std::vector<Row> all(A&&... params) const {
      return all(std::vector<Binding>{Binding(std::forward<A>(params))...});
    }

    // Visit owned rows in order, with the same reset and binding behavior as all().
    void forEachRow(const std::vector<Binding>& params, const std::function<void(Row)>& visit) const;

    // `statement.iterate(...)`: a lazy generator; the parameters are bound by the first next()
    // without resetting the statement (Error "bad parameter or other API misuse" when the
    // statement is in the middle of another iteration, as in Bun).
    RowIterator iterate() const;
    RowIterator iterate(const std::vector<Binding>& params) const;
    RowIterator iterate(const Named& params) const;
    template <class... A>
      requires(sizeof...(A) >= 1 && (std::is_constructible_v<Binding, A&&> && ...))
    RowIterator iterate(A&&... params) const;

    // `statement.run(...)`
    Changes run() const;
    Changes run(const std::vector<Binding>& params) const;
    Changes run(const Named& params) const;
    template <class... A>
      requires(sizeof...(A) >= 1 && (std::is_constructible_v<Binding, A&&> && ...))
    Changes run(A&&... params) const {
      return run(std::vector<Binding>{Binding(std::forward<A>(params))...});
    }

    // `statement.paramsCount` (the highest parameter index).
    int paramsCount() const;
    // `statement.safeIntegers()` / `statement.safeIntegers(enabled)` (returns the statement).
    bool safeIntegers() const;
    const Statement& safeIntegers(bool enabled) const;
    // `statement.isFinalized`
    bool isFinalized() const;
    // `statement.finalize()`: idempotent.
    void finalize() const;

    friend bool operator==(const Statement& a, const Statement& b) noexcept { return a.impl == b.impl; }

  private:
    friend class Database;
    friend class RowIterator;
    std::shared_ptr<Impl> impl;
  };

  // The generator `statement.iterate()` returns. next() is `generator.next()`: the next
  // row, std::nullopt once done. After it threw it is done. Range-for works; leaving the
  // loop early leaves the statement where it is, as `break` out of for...of does.
  class RowIterator {
  public:
    std::optional<Row> next();

    class iterator {
    public:
      using iterator_category = std::input_iterator_tag;
      using value_type = Row;
      using difference_type = std::ptrdiff_t;
      using pointer = Row*;
      using reference = Row&;
      iterator() = default;
      explicit iterator(RowIterator* owner_) : owner(owner_) { advance(); }
      Row& operator*() { return *current; }
      Row* operator->() { return &*current; }
      iterator& operator++() {
        advance();
        return *this;
      }
      void operator++(int) { advance(); }
      friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.owner == b.owner; }

    private:
      void advance() {
        current = owner->next();
        if (!current) owner = nullptr;
      }
      RowIterator* owner = nullptr;
      std::optional<Row> current;
    };
    iterator begin() { return iterator(this); }
    iterator end() { return iterator(); }

  private:
    friend class Statement;
    enum class Pending { none, positional, named };
    explicit RowIterator(std::shared_ptr<Statement::Impl> statement_) : statement(std::move(statement_)) {}
    std::shared_ptr<Statement::Impl> statement;
    Pending pending = Pending::none;
    std::vector<Binding> positional;
    std::optional<Named> named;
    bool done = false;
  };

  template <class... A>
    requires(sizeof...(A) >= 1 && (std::is_constructible_v<Binding, A&&> && ...))
  RowIterator Statement::iterate(A&&... params) const {
    return iterate(std::vector<Binding>{Binding(std::forward<A>(params))...});
  }

  template <class Fn> class Transaction;

  namespace detail {
    enum class TransactionKind { default_, deferred, immediate, exclusive };
    // The statements `getController` prepares for one database.
    struct TransactionController;
    // wrapTransaction: BEGIN (or SAVEPOINT when already in a transaction), body, COMMIT (or
    // RELEASE); when the body or the commit throws while a transaction is open, ROLLBACK
    // (or ROLLBACK TO + RELEASE) and rethrow.
    void runTransaction(const Database& database, const TransactionController& controller, TransactionKind kind, const std::function<void()>& body);
  }

  class Database {
  public:
    struct Impl;

    // `new Database()`: an anonymous in-memory database, filename ":memory:".
    Database();
    // `new Database(filename)`: read-write, created when missing. The filename is trimmed
    // like String.prototype.trim; "" and ":memory:" open an in-memory database. A "file:"
    // name is a plain path (Bun's SQLite does not interpret URIs unless SQLITE_OPEN_URI).
    explicit Database(std::string_view filename);
    // `new Database(filename, { readonly, create, readwrite, strict, safeIntegers })`:
    // readonly -> SQLITE_OPEN_READONLY; create -> read-write-create; readwrite adds
    // SQLITE_OPEN_READWRITE; with "strict" or "safeIntegers" present and no flag yet,
    // read-write-create. Error "Cannot open an anonymous database in read-only mode."
    // for an in-memory name with SQLITE_OPEN_READONLY; SQLiteError when SQLite cannot open
    // it (SQLITE_CANTOPEN "unable to open database file"; SQLITE_MISUSE "bad parameter or
    // other API misuse" for flags without READONLY or READWRITE, or with both).
    Database(std::string_view filename, const DatabaseOptions& options);
    // `new Database(filename, flags)`: the sqlite3_open_v2 flags as given (e.g.
    // SQLITE_OPEN_READONLY | SQLITE_OPEN_URI to open "file:...?immutable=1").
    Database(std::string_view filename, int flags);

    // `db.filename`: the trimmed name as given ("" stays ""), ":memory:" for new Database().
    const std::string& filename() const;

    // `db.query(sql)`: the statement cached for this exact SQL text (Bun keeps the 20 most
    // recently used; an evicted statement stays usable; a finalized one is prepared again).
    // Error "SQL query cannot be empty." for ""; RangeError "Cannot use a closed database"
    // after close(); SQLiteError when SQLite rejects the first statement of the text (the
    // rest is ignored); Error "Statement has finalized" for text without a statement.
    Statement query(std::string_view sql) const;
    // `db.prepare(sql)`: a new statement, never cached. RangeError "Invalid SQL statement" for "".
    Statement prepare(std::string_view sql) const;

    // `db.run(sql, ...params)` / `db.exec(...)`: runs every statement of the text in order.
    // Parameters bind to the first statement only (Error "SQLite query expected N values,
    // received M" when the counts differ, even for a statement without parameters). Each
    // statement after the first starts after skipping ASCII whitespace and ';' (byteOffset
    // counts from there). A statement whose step fails throws its SQLiteError only when its
    // text ends the SQL string; otherwise the failure is ignored and the next statement runs
    // (Bun's behaviour). Errors: "Database has closed" (closed), "SQL string mustn't be
    // blank" (""), "Query contained no valid SQL statement; likely empty query." (nothing to
    // run), SQLiteError when a statement does not compile.
    Changes run(std::string_view sql) const;
    Changes run(std::string_view sql, const std::vector<Binding>& params) const;
    Changes run(std::string_view sql, const Named& params) const;
    template <class... A>
      requires(sizeof...(A) >= 1 && (std::is_constructible_v<Binding, A&&> && ...))
    Changes run(std::string_view sql, A&&... params) const {
      return run(sql, std::vector<Binding>{Binding(std::forward<A>(params))...});
    }
    template <class... A> Changes exec(std::string_view sql, A&&... params) const { return run(sql, std::forward<A>(params)...); }

    // `db.transaction(fn)`: prepares the transaction statements on first use (RangeError
    // "Cannot use a closed database" when closed) and returns the wrapped function; call it
    // as `tx(args...)`, `tx.deferred(...)`, `tx.immediate(...)`, `tx.exclusive(...)`.
    template <class Fn> Transaction<std::decay_t<Fn>> transaction(Fn&& fn) const;

    // `db.inTransaction`: Error "Database has closed" after close().
    bool inTransaction() const;
    void reportPageCacheMemory(std::size_t pageBytes) const noexcept;
    // `db.fileControl(op, value)`: SQLite's result code (SQLITE_NOTFOUND 12 for an in-memory
    // database); std::nullopt (undefined) after close().
    std::optional<int> fileControl(int op, int value) const;
    // `db.close(throwOnError)`: clears the query cache, finalizes the query() and
    // transaction statements (throwOnError: every statement) and closes; a later close does
    // nothing except finalizing remaining statements when throwOnError. SQLiteError when
    // sqlite3_close fails under throwOnError.
    void close(bool throwOnError = false) const;

    // `Database.MAX_QUERY_CACHE_SIZE`
    static constexpr std::size_t MAX_QUERY_CACHE_SIZE = 20;

    friend bool operator==(const Database& a, const Database& b) noexcept { return a.impl == b.impl; }

  private:
    std::shared_ptr<const detail::TransactionController> controller() const;
    std::shared_ptr<Impl> impl;
  };

  // The function `db.transaction(fn)` returns.
  template <class Fn> class Transaction {
  public:
    template <class... A> decltype(auto) operator()(A&&... args) const { return invoke(detail::TransactionKind::default_, std::forward<A>(args)...); }
    template <class... A> decltype(auto) deferred(A&&... args) const { return invoke(detail::TransactionKind::deferred, std::forward<A>(args)...); }
    template <class... A> decltype(auto) immediate(A&&... args) const { return invoke(detail::TransactionKind::immediate, std::forward<A>(args)...); }
    template <class... A> decltype(auto) exclusive(A&&... args) const { return invoke(detail::TransactionKind::exclusive, std::forward<A>(args)...); }
    // `tx.database`
    const Database& database() const noexcept { return database_; }

  private:
    friend class Database;
    Transaction(Database database, std::shared_ptr<const detail::TransactionController> controller, Fn fn)
      : database_(std::move(database)), controller_(std::move(controller)), fn_(std::move(fn)) {}

    template <class... A> auto invoke(detail::TransactionKind kind, A&&... args) const {
      using Result = std::remove_cvref_t<std::invoke_result_t<Fn&, A&&...>>;
      if constexpr (std::is_void_v<Result>) {
        detail::runTransaction(database_, *controller_, kind, [&] { std::invoke(fn_, std::forward<A>(args)...); });
      } else {
        std::optional<Result> result;
        detail::runTransaction(database_, *controller_, kind, [&] { result.emplace(std::invoke(fn_, std::forward<A>(args)...)); });
        return Result(std::move(*result));
      }
    }

    Database database_;
    std::shared_ptr<const detail::TransactionController> controller_;
    mutable Fn fn_;
  };

  template <class Fn> Transaction<std::decay_t<Fn>> Database::transaction(Fn&& fn) const {
    return Transaction<std::decay_t<Fn>>(*this, controller(), std::forward<Fn>(fn));
  }

  // The SQLite library version in use ("3.40.1" from the SDK; Bun bundles 3.53.2).
  std::string libraryVersion();
}
