#include "uos/rt/sqlite.hpp"
#if defined(__APPLE__)
#include "sqlite_darwin.hpp"
#endif

#include "uos/rt/json.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#include <sqlite3.h>
// The public binding retains Bun's constant names; do not let C macros replace
// qualified constants:: members below.
#undef SQLITE_OPEN_READONLY
#undef SQLITE_OPEN_READWRITE
#undef SQLITE_OPEN_CREATE
#undef SQLITE_PREPARE_PERSISTENT
#if defined(__APPLE__)
#undef SQLITE_FCNTL_PERSIST_WAL
#endif
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#if defined(__linux__) && defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#include "uos/rt/url.hpp"
#include <unistd.h>
#include <new>
#endif

namespace uos::rt::sqlite {
  namespace {
    // Primary builds use the installed C declarations; the SDK comparison has no headers.
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    using Connection3 = ::sqlite3;
    using Statement3 = ::sqlite3_stmt;
#else
    // Opaque `sqlite3` and `sqlite3_stmt`.
    struct Connection3;
    struct Statement3;

    constexpr int SQLITE_OK = 0;
    constexpr int SQLITE_ROW = 100;
    constexpr int SQLITE_DONE = 101;
    constexpr int SQLITE_INTEGER = 1;
    constexpr int SQLITE_FLOAT = 2;
    constexpr int SQLITE_TEXT = 3;
    constexpr int SQLITE_BLOB = 4;
    constexpr int SQLITE_CONFIG_URI = 17;
    constexpr unsigned char SQLITE_UTF8 = 1;
    constexpr unsigned char SQLITE_UTF16 = 4;
    using Destructor = void (*)(void*);
    // SQLITE_TRANSIENT: SQLite copies the bound bytes.
    const Destructor SQLITE_TRANSIENT = reinterpret_cast<Destructor>(static_cast<std::intptr_t>(-1));

#endif

    struct Api {
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      decltype(&::sqlite3_open_v2) open_v2 = &::sqlite3_open_v2;
      decltype(&::sqlite3_close) close = &::sqlite3_close;
      decltype(&::sqlite3_close_v2) close_v2 = &::sqlite3_close_v2;
      decltype(&::sqlite3_extended_result_codes) extended_result_codes = &::sqlite3_extended_result_codes;
      decltype(&::sqlite3_extended_errcode) extended_errcode = &::sqlite3_extended_errcode;
      decltype(&::sqlite3_errmsg) errmsg = &::sqlite3_errmsg;
      decltype(&::sqlite3_errstr) errstr = &::sqlite3_errstr;
      decltype(&::sqlite3_error_offset) error_offset = &::sqlite3_error_offset;
      decltype(&::sqlite3_prepare_v3) prepare_v3 = &::sqlite3_prepare_v3;
      decltype(&::sqlite3_step) step = &::sqlite3_step;
      decltype(&::sqlite3_reset) reset = &::sqlite3_reset;
      decltype(&::sqlite3_finalize) finalize = &::sqlite3_finalize;
      decltype(&::sqlite3_clear_bindings) clear_bindings = &::sqlite3_clear_bindings;
      decltype(&::sqlite3_bind_parameter_count) bind_parameter_count = &::sqlite3_bind_parameter_count;
      decltype(&::sqlite3_bind_parameter_name) bind_parameter_name = &::sqlite3_bind_parameter_name;
      decltype(&::sqlite3_bind_null) bind_null = &::sqlite3_bind_null;
      decltype(&::sqlite3_bind_int64) bind_int64 = &::sqlite3_bind_int64;
      decltype(&::sqlite3_bind_double) bind_double = &::sqlite3_bind_double;
      decltype(&::sqlite3_bind_text64) bind_text64 = &::sqlite3_bind_text64;
      decltype(&::sqlite3_bind_blob64) bind_blob64 = &::sqlite3_bind_blob64;
      decltype(&::sqlite3_column_count) column_count = &::sqlite3_column_count;
      decltype(&::sqlite3_column_name) column_name = &::sqlite3_column_name;
      decltype(&::sqlite3_column_type) column_type = &::sqlite3_column_type;
      decltype(&::sqlite3_column_int64) column_int64 = &::sqlite3_column_int64;
      decltype(&::sqlite3_column_double) column_double = &::sqlite3_column_double;
      decltype(&::sqlite3_column_text) column_text = &::sqlite3_column_text;
      decltype(&::sqlite3_column_blob) column_blob = &::sqlite3_column_blob;
      decltype(&::sqlite3_column_bytes) column_bytes = &::sqlite3_column_bytes;
      decltype(&::sqlite3_total_changes64) total_changes64 = &::sqlite3_total_changes64;
      decltype(&::sqlite3_last_insert_rowid) last_insert_rowid = &::sqlite3_last_insert_rowid;
      decltype(&::sqlite3_get_autocommit) get_autocommit = &::sqlite3_get_autocommit;
      decltype(&::sqlite3_file_control) file_control = &::sqlite3_file_control;
#if defined(__APPLE__)
      decltype(&::sqlite3_vfs_find) vfs_find = &::sqlite3_vfs_find;
      decltype(&::sqlite3_vfs_register) vfs_register = &::sqlite3_vfs_register;
#endif
      decltype(&::sqlite3_config) config = &::sqlite3_config;
      decltype(&::sqlite3_libversion) libversion = &::sqlite3_libversion;
#else
      int (*open_v2)(const char*, Connection3**, int, const char*) = nullptr;
      int (*close)(Connection3*) = nullptr;
      int (*close_v2)(Connection3*) = nullptr;
      int (*extended_result_codes)(Connection3*, int) = nullptr;
      int (*extended_errcode)(Connection3*) = nullptr;
      const char* (*errmsg)(Connection3*) = nullptr;
      const char* (*errstr)(int) = nullptr;
      int (*error_offset)(Connection3*) = nullptr;
      int (*prepare_v3)(Connection3*, const char*, int, unsigned, Statement3**, const char**) = nullptr;
      int (*step)(Statement3*) = nullptr;
      int (*reset)(Statement3*) = nullptr;
      int (*finalize)(Statement3*) = nullptr;
      int (*clear_bindings)(Statement3*) = nullptr;
      int (*bind_parameter_count)(Statement3*) = nullptr;
      const char* (*bind_parameter_name)(Statement3*, int) = nullptr;
      int (*bind_null)(Statement3*, int) = nullptr;
      int (*bind_int64)(Statement3*, int, long long) = nullptr;
      int (*bind_double)(Statement3*, int, double) = nullptr;
      int (*bind_text64)(Statement3*, int, const char*, unsigned long long, Destructor, unsigned char) = nullptr;
      int (*bind_blob64)(Statement3*, int, const void*, unsigned long long, Destructor) = nullptr;
      int (*column_count)(Statement3*) = nullptr;
      const char* (*column_name)(Statement3*, int) = nullptr;
      int (*column_type)(Statement3*, int) = nullptr;
      long long (*column_int64)(Statement3*, int) = nullptr;
      double (*column_double)(Statement3*, int) = nullptr;
      const unsigned char* (*column_text)(Statement3*, int) = nullptr;
      const void* (*column_blob)(Statement3*, int) = nullptr;
      int (*column_bytes)(Statement3*, int) = nullptr;
      long long (*total_changes64)(Connection3*) = nullptr;
      long long (*last_insert_rowid)(Connection3*) = nullptr;
      int (*get_autocommit)(Connection3*) = nullptr;
      int (*file_control)(Connection3*, const char*, int, void*) = nullptr;
#if defined(__APPLE__)
      decltype(&::sqlite3_vfs_find) vfs_find = nullptr;
      decltype(&::sqlite3_vfs_register) vfs_register = nullptr;
#endif
      int (*config)(int, ...) = nullptr;
      const char* (*libversion)() = nullptr;
#endif
    };

#if !defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#if defined(__APPLE__)
    constexpr const char* DEFAULT_LIBRARY = "libsqlite3.dylib";
#else
    constexpr const char* DEFAULT_LIBRARY = "libsqlite3.so.0";
#endif
    template <class Function> void resolve(void* library, Function& slot, const char* name) {
      void* symbol = ::dlsym(library, name);
      if (!symbol) throw std::runtime_error(std::string("bun:sqlite: ") + DEFAULT_LIBRARY + " lacks " + name);
      std::memcpy(&slot, &symbol, sizeof slot);
    }

    #endif

    Api load() {
      Api api;
#if !defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      void* library = ::dlopen(DEFAULT_LIBRARY, RTLD_NOW | RTLD_LOCAL);
      if (!library) throw std::runtime_error(std::string("bun:sqlite: cannot load ") + DEFAULT_LIBRARY + ": " + ::dlerror());
      resolve(library, api.open_v2, "sqlite3_open_v2");
      resolve(library, api.close, "sqlite3_close");
      resolve(library, api.close_v2, "sqlite3_close_v2");
      resolve(library, api.extended_result_codes, "sqlite3_extended_result_codes");
      resolve(library, api.extended_errcode, "sqlite3_extended_errcode");
      resolve(library, api.errmsg, "sqlite3_errmsg");
      resolve(library, api.errstr, "sqlite3_errstr");
      resolve(library, api.error_offset, "sqlite3_error_offset");
      resolve(library, api.prepare_v3, "sqlite3_prepare_v3");
      resolve(library, api.step, "sqlite3_step");
      resolve(library, api.reset, "sqlite3_reset");
      resolve(library, api.finalize, "sqlite3_finalize");
      resolve(library, api.clear_bindings, "sqlite3_clear_bindings");
      resolve(library, api.bind_parameter_count, "sqlite3_bind_parameter_count");
      resolve(library, api.bind_parameter_name, "sqlite3_bind_parameter_name");
      resolve(library, api.bind_null, "sqlite3_bind_null");
      resolve(library, api.bind_int64, "sqlite3_bind_int64");
      resolve(library, api.bind_double, "sqlite3_bind_double");
      resolve(library, api.bind_text64, "sqlite3_bind_text64");
      resolve(library, api.bind_blob64, "sqlite3_bind_blob64");
      resolve(library, api.column_count, "sqlite3_column_count");
      resolve(library, api.column_name, "sqlite3_column_name");
      resolve(library, api.column_type, "sqlite3_column_type");
      resolve(library, api.column_int64, "sqlite3_column_int64");
      resolve(library, api.column_double, "sqlite3_column_double");
      resolve(library, api.column_text, "sqlite3_column_text");
      resolve(library, api.column_blob, "sqlite3_column_blob");
      resolve(library, api.column_bytes, "sqlite3_column_bytes");
      resolve(library, api.total_changes64, "sqlite3_total_changes64");
      resolve(library, api.last_insert_rowid, "sqlite3_last_insert_rowid");
      resolve(library, api.get_autocommit, "sqlite3_get_autocommit");
      resolve(library, api.file_control, "sqlite3_file_control");
#if defined(__APPLE__)
      resolve(library, api.vfs_find, "sqlite3_vfs_find");
      resolve(library, api.vfs_register, "sqlite3_vfs_register");
#endif
      resolve(library, api.config, "sqlite3_config");
      resolve(library, api.libversion, "sqlite3_libversion");
#endif
      // Linux requires explicit SQLITE_OPEN_URI; Darwin adds it per open below.
      // Configure before the library initializes (the first open).
      api.config(SQLITE_CONFIG_URI, 0);
      return api;
    }

    const Api& api() {
      static const Api loaded = load();
      return loaded;
    }

    // ---- text ----------------------------------------------------------------------------

    // Text SQLite produced (TEXT values, names, messages): UTF-8 decoded with U+FFFD for
    // each ill-formed subpart and the BOM kept, as Bun decodes it.
    std::string fromSqlite(const char* text, std::size_t size) {
      if (!text) return std::string();
      return decodeUtf8(std::string_view(text, size), {.ignoreBOM = true});
    }
    std::string fromSqlite(const char* text) { return text ? fromSqlite(text, std::strlen(text)) : std::string(); }

    // SQL text and filenames go to SQLite as UTF-8; Bun encodes a lone surrogate as U+FFFD.
    std::string toSqlite(std::string_view text) {
      if (isUtf8(text)) return std::string(text);
      const std::vector<std::uint8_t> bytes = encodeUtf8(text);
      return std::string(bytes.begin(), bytes.end());
    }

    int clampLength(std::size_t size) { return static_cast<int>(std::min<std::size_t>(size, INT_MAX)); }

    // ---- errors --------------------------------------------------------------------------

    SQLiteError connectionError(Connection3* connection) {
      const Api& sqlite = api();
      const int code = sqlite.extended_errcode(connection);
      return SQLiteError(fromSqlite(sqlite.errmsg(connection)), resultCodeName(code), code, sqlite.error_offset(connection));
    }

    bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
  }

  SQLiteError::SQLiteError(std::string message_, std::optional<std::string> code_, int errnoValue, int byteOffset_)
    : JsError(std::move(message_), "SQLiteError"), code(std::move(code_)), errno_(errnoValue), byteOffset(byteOffset_) {}

  std::optional<std::string> resultCodeName(int code) {
    static const std::unordered_map<int, std::string> names = {
      {0, "SQLITE_OK"}, {1, "SQLITE_ERROR"}, {2, "SQLITE_INTERNAL"}, {3, "SQLITE_PERM"}, {4, "SQLITE_ABORT"}, {5, "SQLITE_BUSY"},
      {6, "SQLITE_LOCKED"}, {7, "SQLITE_NOMEM"}, {8, "SQLITE_READONLY"}, {9, "SQLITE_INTERRUPT"}, {10, "SQLITE_IOERR"},
      {11, "SQLITE_CORRUPT"}, {12, "SQLITE_NOTFOUND"}, {13, "SQLITE_FULL"}, {14, "SQLITE_CANTOPEN"}, {15, "SQLITE_PROTOCOL"},
      {16, "SQLITE_EMPTY"}, {17, "SQLITE_SCHEMA"}, {18, "SQLITE_TOOBIG"}, {19, "SQLITE_CONSTRAINT"}, {20, "SQLITE_MISMATCH"},
      {21, "SQLITE_MISUSE"}, {22, "SQLITE_NOLFS"}, {23, "SQLITE_AUTH"}, {24, "SQLITE_FORMAT"}, {25, "SQLITE_RANGE"},
      {26, "SQLITE_NOTADB"}, {27, "SQLITE_NOTICE"}, {28, "SQLITE_WARNING"},
      {257, "SQLITE_ERROR_MISSING_COLLSEQ"}, {513, "SQLITE_ERROR_RETRY"}, {769, "SQLITE_ERROR_SNAPSHOT"},
      {266, "SQLITE_IOERR_READ"}, {522, "SQLITE_IOERR_SHORT_READ"}, {778, "SQLITE_IOERR_WRITE"}, {1034, "SQLITE_IOERR_FSYNC"},
      {1290, "SQLITE_IOERR_DIR_FSYNC"}, {1546, "SQLITE_IOERR_TRUNCATE"}, {1802, "SQLITE_IOERR_FSTAT"}, {2058, "SQLITE_IOERR_UNLOCK"},
      {2314, "SQLITE_IOERR_RDLOCK"}, {2570, "SQLITE_IOERR_DELETE"}, {2826, "SQLITE_IOERR_BLOCKED"}, {3082, "SQLITE_IOERR_NOMEM"},
      {3338, "SQLITE_IOERR_ACCESS"}, {3594, "SQLITE_IOERR_CHECKRESERVEDLOCK"}, {3850, "SQLITE_IOERR_LOCK"}, {4106, "SQLITE_IOERR_CLOSE"},
      {4362, "SQLITE_IOERR_DIR_CLOSE"}, {4618, "SQLITE_IOERR_SHMOPEN"}, {4874, "SQLITE_IOERR_SHMSIZE"}, {5130, "SQLITE_IOERR_SHMLOCK"},
      {5386, "SQLITE_IOERR_SHMMAP"}, {5642, "SQLITE_IOERR_SEEK"}, {5898, "SQLITE_IOERR_DELETE_NOENT"}, {6154, "SQLITE_IOERR_MMAP"},
      {6410, "SQLITE_IOERR_GETTEMPPATH"}, {6666, "SQLITE_IOERR_CONVPATH"}, {6922, "SQLITE_IOERR_VNODE"}, {7178, "SQLITE_IOERR_AUTH"},
      {7434, "SQLITE_IOERR_BEGIN_ATOMIC"}, {7690, "SQLITE_IOERR_COMMIT_ATOMIC"}, {7946, "SQLITE_IOERR_ROLLBACK_ATOMIC"},
      {8202, "SQLITE_IOERR_DATA"}, {8458, "SQLITE_IOERR_CORRUPTFS"}, {8714, "SQLITE_IOERR_IN_PAGE"},
      {262, "SQLITE_LOCKED_SHAREDCACHE"}, {518, "SQLITE_LOCKED_VTAB"},
      {261, "SQLITE_BUSY_RECOVERY"}, {517, "SQLITE_BUSY_SNAPSHOT"}, {773, "SQLITE_BUSY_TIMEOUT"},
      {270, "SQLITE_CANTOPEN_NOTEMPDIR"}, {526, "SQLITE_CANTOPEN_ISDIR"}, {782, "SQLITE_CANTOPEN_FULLPATH"}, {1038, "SQLITE_CANTOPEN_CONVPATH"},
      {1294, "SQLITE_CANTOPEN_DIRTYWAL"}, {1550, "SQLITE_CANTOPEN_SYMLINK"},
      {267, "SQLITE_CORRUPT_VTAB"}, {523, "SQLITE_CORRUPT_SEQUENCE"}, {779, "SQLITE_CORRUPT_INDEX"},
      {264, "SQLITE_READONLY_RECOVERY"}, {520, "SQLITE_READONLY_CANTLOCK"}, {776, "SQLITE_READONLY_ROLLBACK"}, {1032, "SQLITE_READONLY_DBMOVED"},
      {1288, "SQLITE_READONLY_CANTINIT"}, {1544, "SQLITE_READONLY_DIRECTORY"},
      {516, "SQLITE_ABORT_ROLLBACK"},
      {275, "SQLITE_CONSTRAINT_CHECK"}, {531, "SQLITE_CONSTRAINT_COMMITHOOK"}, {787, "SQLITE_CONSTRAINT_FOREIGNKEY"},
      {1043, "SQLITE_CONSTRAINT_FUNCTION"}, {1299, "SQLITE_CONSTRAINT_NOTNULL"}, {1555, "SQLITE_CONSTRAINT_PRIMARYKEY"},
      {1811, "SQLITE_CONSTRAINT_TRIGGER"}, {2067, "SQLITE_CONSTRAINT_UNIQUE"}, {2323, "SQLITE_CONSTRAINT_VTAB"},
      {2579, "SQLITE_CONSTRAINT_ROWID"}, {2835, "SQLITE_CONSTRAINT_PINNED"}, {3091, "SQLITE_CONSTRAINT_DATATYPE"},
      {283, "SQLITE_NOTICE_RECOVER_WAL"}, {539, "SQLITE_NOTICE_RECOVER_ROLLBACK"}, {795, "SQLITE_NOTICE_RBU"},
      {284, "SQLITE_WARNING_AUTOINDEX"}, {279, "SQLITE_AUTH_USER"}, {256, "SQLITE_OK_LOAD_PERMANENTLY"}, {512, "SQLITE_OK_SYMLINK"},
    };
    const auto found = names.find(code);
    if (found == names.end()) return std::nullopt;
    return found->second;
  }

  // ---- connection and statement state ----------------------------------------------------

  namespace {
    // One sqlite3 connection. Statements keep it alive; it closes (sqlite3_close_v2) when the
    // last owner goes without db.close(), as Bun's finalizer would.
    struct Connection {
      Connection3* handle = nullptr;
      // db.close() ran: the handle is a zombie kept by prepare() statements, or gone.
      bool closed = false;
      // Every statement prepared on this connection, for db.close() to finalize.
      std::vector<std::weak_ptr<Statement::Impl>> statements;

      explicit Connection(Connection3* handle_) : handle(handle_) {}
      Connection(const Connection&) = delete;
      Connection& operator=(const Connection&) = delete;
      ~Connection() {
        if (!closed && handle) api().close_v2(handle);
      }
      void track(const std::shared_ptr<Statement::Impl>& statement) {
        if (statements.size() >= 64 && statements.size() == statements.capacity()) {
          std::erase_if(statements, [](const std::weak_ptr<Statement::Impl>& entry) { return entry.expired(); });
        }
        statements.push_back(statement);
      }
    };
  }

  struct Statement::Impl {
    enum class State { alive, finalized, closed };

    std::shared_ptr<Connection> connection;
    // Null once finalized.
    Statement3* statement = nullptr;
    State state = State::alive;
    // Prepared by db.query() or db.transaction(): finalized by db.close() too.
    bool owned = false;
    // The database's `strict` and `safeIntegers` options when the statement was prepared.
    bool strict = false;
    bool safeIntegers = false;
    int paramsCount = 0;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
      if (statement) api().finalize(statement);
    }

    void requireAlive() const {
      if (state == State::closed) throw JsError("Database has closed");
      if (state == State::finalized) throw JsError("Statement has finalized");
    }
    void finalizeAs(State next) {
      if (statement) api().finalize(std::exchange(statement, nullptr));
      if (state == State::alive) state = next;
    }
    SQLiteError error() const { return connectionError(connection->handle); }
  };

  struct Database::Impl {
    std::shared_ptr<Connection> connection;
    std::string filename;
    bool strict = false;
    bool safeIntegers = false;
    // The query() cache, least recently used first.
    std::vector<std::pair<std::string, std::shared_ptr<Statement::Impl>>> queryCache;
    std::shared_ptr<const detail::TransactionController> controller;
    memory::ScopedCache pageMemory{memory::Cache::sqlite_pages};
    memory::ScopedCache statementMemory{memory::Cache::sqlite_statements};

    void reportStatements() noexcept {
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      std::size_t bytes = queryCache.capacity() * sizeof(queryCache.front());
      for (const auto& [sql, cached] : queryCache) {
        bytes += memory::bytes(sql) + sizeof(Statement::Impl);
        if (cached->statement) bytes += static_cast<std::size_t>(::sqlite3_stmt_status(cached->statement, SQLITE_STMTSTATUS_MEMUSED, 0));
      }
      statementMemory.set(queryCache.size(), bytes);
#endif
    }
  };

  struct detail::TransactionController {
    Statement commit;
    Statement rollback;
    Statement savepoint;
    Statement release;
    Statement rollbackTo;
    Statement begin;
    Statement beginDeferred;
    Statement beginImmediate;
    Statement beginExclusive;
  };

  // ---- bindings ----------------------------------------------------------------------------

  struct BindingAccess {
    static const auto& value(const Binding& binding) { return binding.value_; }
  };

  Binding::Binding(const BigInt& value) : value_(std::int64_t{0}) {
    static const BigInt minimum(INT64_MIN);
    static const BigInt maximum(INT64_MAX);
    if (value < minimum || value > maximum) value_ = value;
    else value_ = value.toInt64();
  }

  Binding::Binding(const Value& value) : value_(nullptr) {
    std::visit(
      [this](const auto& alternative) {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) value_ = nullptr;
        else value_ = alternative;
      },
      value);
  }

  Binding::Binding(const json::value& value) : value_(nullptr) {
    switch (value.kind()) {
      case json::kind::null: value_ = nullptr; break;
      case json::kind::bool_: value_ = value.get_bool(); break;
      case json::kind::int64: value_ = static_cast<double>(value.get_int64()); break;
      case json::kind::uint64: value_ = static_cast<double>(value.get_uint64()); break;
      case json::kind::double_: value_ = value.get_double(); break;
      case json::kind::string: value_ = std::string(value.get_string()); break;
      case json::kind::array:
      case json::kind::object: value_ = Unbindable{}; break;
    }
  }

  namespace {
    // `value.isAnyInt()` in JavaScriptCore: an integer in the int52 range, not -0.
    bool isAnyInt(double value) {
      if (!(value >= -2251799813685248.0 && value < 2251799813685248.0)) return false;
      const auto integer = static_cast<std::int64_t>(value);
      if (static_cast<double>(integer) != value) return false;
      return integer != 0 || !std::signbit(value);
    }

    const std::uint8_t kNoBytes = 0;
    const char kNoText = 0;

    // Binds one value at a 1-based index. RangeError for a bigint outside int64 under safe
    // integers; TypeError for an object; Error with SQLite's message when SQLite refuses.
    void bindValue(Statement3* statement, Connection3* connection, int index, const Binding& binding, bool safeIntegers) {
      const Api& sqlite = api();
      int status = SQLITE_OK;
      std::visit(
        [&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, std::nullptr_t>) {
            status = sqlite.bind_null(statement, index);
          } else if constexpr (std::is_same_v<T, bool>) {
            status = sqlite.bind_int64(statement, index, value ? 1 : 0);
          } else if constexpr (std::is_same_v<T, double>) {
            if (isAnyInt(value)) status = sqlite.bind_int64(statement, index, static_cast<long long>(value));
            else status = sqlite.bind_double(statement, index, value);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            status = sqlite.bind_int64(statement, index, value);
          } else if constexpr (std::is_same_v<T, BigInt>) {
            if (safeIntegers) throw RangeError("BigInt value '" + value.toString() + "' is out of range");
            status = sqlite.bind_int64(statement, index, value.toInt64());
          } else if constexpr (std::is_same_v<T, std::string>) {
            if (isUtf8(value)) {
              status = sqlite.bind_text64(statement, index, value.empty() ? &kNoText : value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
            } else {
              // A string holding a lone surrogate is a UTF-16 string to Bun: SQLite converts it.
              const std::u16string units = toUtf16(value);
              status = sqlite.bind_text64(statement, index, reinterpret_cast<const char*>(units.data()), units.size() * 2, SQLITE_TRANSIENT, SQLITE_UTF16);
            }
          } else if constexpr (std::is_same_v<T, Bytes>) {
            status = sqlite.bind_blob64(statement, index, value.empty() ? &kNoBytes : value.data(), value.size(), SQLITE_TRANSIENT);
          } else {
            throw TypeError("Binding expected string, TypedArray, boolean, number, bigint or null");
          }
        },
        BindingAccess::value(binding));
      if (status != SQLITE_OK) throw JsError(fromSqlite(sqlite.errmsg(connection)));
    }

    // `rebindStatement` with an array: clears every binding, checks the count, binds in order.
    // A RangeError leaves no binding; other failures keep the values bound before them.
    void bindPositional(Statement3* statement, Connection3* connection, const std::vector<Binding>& values, bool safeIntegers) {
      const Api& sqlite = api();
      sqlite.clear_bindings(statement);
      const int expected = sqlite.bind_parameter_count(statement);
      const std::size_t count = values.size();
      if (count == 0) return;
      if (count != static_cast<std::size_t>(expected)) {
        throw JsError("SQLite query expected " + std::to_string(expected) + " values, received " + std::to_string(count));
      }
      try {
        for (std::size_t index = 0; index < count; ++index) bindValue(statement, connection, static_cast<int>(index) + 1, values[index], safeIntegers);
      } catch (const RangeError&) {
        sqlite.clear_bindings(statement);
        throw;
      }
    }

    bool isNamedPrefix(char c) { return c == '$' || c == ':' || c == '@'; }

    // `rebindStatement` with an object: each parameter reads its property (docs/rt/sqlite.md):
    // `$a`/`:a`/`@a` read "$a" (strict: "a"); `?NNN` reads "?NNN" (strict: "<index - 1>");
    // `?` reads "<index - 1>". Under strict a missing property throws Error `Missing parameter
    // "<name>"`, the name being the key read, "?NNN", or for `?` "<index - 1>" next to named
    // parameters, "?<index>" next to `?NNN` ones and "<index>" otherwise.
    void bindNamed(Statement3* statement, Connection3* connection, const Named& values, bool strict, bool safeIntegers) {
      const Api& sqlite = api();
      sqlite.clear_bindings(statement);
      const int count = sqlite.bind_parameter_count(statement);
      std::vector<std::optional<std::string>> names(static_cast<std::size_t>(count));
      bool hasNamed = false;
      bool hasNumbered = false;
      for (int index = 1; index <= count; ++index) {
        const char* name = sqlite.bind_parameter_name(statement, index);
        if (!name) continue;
        names[static_cast<std::size_t>(index - 1)] = fromSqlite(name);
        if (isNamedPrefix(name[0])) hasNamed = true;
        else hasNumbered = true;
      }
      try {
        for (int index = 1; index <= count; ++index) {
          const std::optional<std::string>& name = names[static_cast<std::size_t>(index - 1)];
          std::string key;
          std::string shown;
          if (name && !name->empty() && isNamedPrefix((*name)[0])) {
            key = strict ? name->substr(1) : *name;
            shown = key;
          } else if (name && !strict) {
            key = *name;
            shown = key;
          } else {
            key = std::to_string(index - 1);
            if (name) shown = *name;
            else if (hasNamed) shown = key;
            else if (hasNumbered) shown = "?" + std::to_string(index);
            else shown = std::to_string(index);
          }
          const Binding* value = values.get(std::string_view(key));
          if (!value) {
            if (strict) throw JsError("Missing parameter \"" + shown + "\"");
            continue;
          }
          bindValue(statement, connection, index, *value, safeIntegers);
        }
      } catch (const RangeError&) {
        sqlite.clear_bindings(statement);
        throw;
      }
    }

    // ---- reading rows ------------------------------------------------------------------------

    // The result columns: every name, and the columns a row object keeps (the last column
    // of each name), in column order.
    struct Columns {
      std::vector<std::string> names;
      std::vector<int> kept;
    };

    Columns columnsOf(Statement3* statement) {
      const Api& sqlite = api();
      Columns columns;
      const int count = sqlite.column_count(statement);
      columns.names.reserve(static_cast<std::size_t>(count));
      for (int index = 0; index < count; ++index) columns.names.push_back(fromSqlite(sqlite.column_name(statement, index)));
      for (int index = 0; index < count; ++index) {
        const std::string& name = columns.names[static_cast<std::size_t>(index)];
        const auto later = std::find(columns.names.begin() + index + 1, columns.names.end(), name);
        if (later == columns.names.end()) columns.kept.push_back(index);
      }
      return columns;
    }

    Value columnValue(Statement3* statement, int index, bool safeIntegers) {
      const Api& sqlite = api();
      switch (sqlite.column_type(statement, index)) {
        case SQLITE_INTEGER: {
          const std::int64_t integer = sqlite.column_int64(statement, index);
          if (safeIntegers) return integer;
          return static_cast<double>(integer);
        }
        case SQLITE_FLOAT: return sqlite.column_double(statement, index);
        case SQLITE_TEXT: {
          const unsigned char* text = sqlite.column_text(statement, index);
          const int size = sqlite.column_bytes(statement, index);
          return fromSqlite(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
        }
        case SQLITE_BLOB: {
          const auto* bytes = static_cast<const std::uint8_t*>(sqlite.column_blob(statement, index));
          const int size = sqlite.column_bytes(statement, index);
          if (!bytes || size <= 0) return Bytes();
          return Bytes(bytes, bytes + size);
        }
        default: return nullptr;
      }
    }

    Row readRow(Statement3* statement, const Columns& columns, bool safeIntegers) {
      Row row;
      for (const int index : columns.kept) row.set(columns.names[static_cast<std::size_t>(index)], columnValue(statement, index, safeIntegers));
      return row;
    }

    std::variant<double, std::int64_t> lastRowid(Connection3* connection, bool safeIntegers) {
      const std::int64_t rowid = api().last_insert_rowid(connection);
      if (safeIntegers) return rowid;
      return static_cast<double>(rowid);
    }

    // ---- statement execution -----------------------------------------------------------------

    // The JavaScript `Statement` picks the no-argument methods for a statement without
    // parameters, and `get(...[])` is a call without arguments.
    bool takesArguments(const Statement::Impl& statement, const std::vector<Binding>& params) { return statement.paramsCount != 0 && !params.empty(); }

    // Readies a statement for get/all/run: reset, then the new bindings if any.
    void restart(Statement::Impl& statement, const std::vector<Binding>* positional, const Named* named) {
      statement.requireAlive();
      api().reset(statement.statement);
      if (positional) bindPositional(statement.statement, statement.connection->handle, *positional, statement.safeIntegers);
      if (named) bindNamed(statement.statement, statement.connection->handle, *named, statement.strict, statement.safeIntegers);
    }

    // get/all/run leave the statement reset, also when a step failed.
    void finish(Statement::Impl& statement, int status) {
      if (status == SQLITE_DONE) {
        api().reset(statement.statement);
        return;
      }
      SQLiteError error = statement.error();
      api().reset(statement.statement);
      throw error;
    }

    std::optional<Row> getRow(Statement::Impl& statement) {
      const Api& sqlite = api();
      int status = sqlite.step(statement.statement);
      std::optional<Row> row;
      if (status == SQLITE_ROW) {
        row = readRow(statement.statement, columnsOf(statement.statement), statement.safeIntegers);
        do {
          status = sqlite.step(statement.statement);
        } while (status == SQLITE_ROW);
      }
      finish(statement, status);
      return row;
    }

    template <class Visit>
    void visitRows(Statement::Impl& statement, Visit&& visit) {
      const Api& sqlite = api();
      std::optional<Columns> columns;
      int status = sqlite.step(statement.statement);
      try {
        while (status == SQLITE_ROW) {
          if (!columns) columns = columnsOf(statement.statement);
          visit(readRow(statement.statement, *columns, statement.safeIntegers));
          status = sqlite.step(statement.statement);
        }
      } catch (...) {
        sqlite.reset(statement.statement);
        throw;
      }
      finish(statement, status);
    }

    std::vector<Row> allRows(Statement::Impl& statement) {
      std::vector<Row> rows;
      visitRows(statement, [&rows](Row row) { rows.push_back(std::move(row)); });
      return rows;
    }

    Changes runStatement(Statement::Impl& statement) {
      const Api& sqlite = api();
      Connection3* const connection = statement.connection->handle;
      const long long before = sqlite.total_changes64(connection);
      int status = SQLITE_ROW;
      while (status == SQLITE_ROW) status = sqlite.step(statement.statement);
      const long long changed = sqlite.total_changes64(connection) - before;
      finish(statement, status);
      return Changes{.changes = static_cast<double>(changed), .lastInsertRowid = lastRowid(connection, statement.safeIntegers)};
    }

    // ---- databases ---------------------------------------------------------------------------

#if defined(__linux__) && defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    struct LiveReadMethods {
      sqlite3_io_methods methods;
      decltype(sqlite3_io_methods::xShmMap) map;
    };

    const char* liveReadVfs() {
      static sqlite3_vfs* registered = []() -> sqlite3_vfs* {
        auto* parent = ::sqlite3_vfs_find("unix");
        if (!parent) return nullptr;
        static sqlite3_vfs vfs = *parent;
        vfs.pNext = nullptr;
        vfs.zName = "uos-live-readonly";
        // Reserve aligned per-file storage for a copy of the unix I/O methods.
        constexpr auto alignment = static_cast<int>(alignof(LiveReadMethods));
        vfs.szOsFile = (parent->szOsFile + alignment - 1) / alignment * alignment + static_cast<int>(sizeof(LiveReadMethods));
        vfs.xOpen = [](sqlite3_vfs* vfsIn, const char* name, sqlite3_file* file, int flags, int* outputFlags) {
          if ((flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_WAL)) == 0) return SQLITE_CANTOPEN;
          flags &= ~(constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE);
          flags |= constants::SQLITE_OPEN_READONLY;
          const auto rc = ::sqlite3_vfs_find("unix")->xOpen(vfsIn, name, file, flags, outputFlags);
          if (rc == SQLITE_OK && (flags & SQLITE_OPEN_MAIN_DB) != 0) {
            auto* storage = reinterpret_cast<char*>(file) + vfsIn->szOsFile - sizeof(LiveReadMethods);
            auto* methods = new (storage) LiveReadMethods{*file->pMethods, file->pMethods->xShmMap};
            methods->methods.xShmMap = [](sqlite3_file* main, int page, int size, int, void volatile** region) {
              const auto* current = reinterpret_cast<const LiveReadMethods*>(main->pMethods);
              const int result = current->map(main, page, size, 0, region);
              // An orphaned WAL index can otherwise trigger a full private WAL recovery.
              return result == SQLITE_READONLY_CANTINIT ? SQLITE_READONLY_RECOVERY : result;
            };
            file->pMethods = &methods->methods;
          }
          return rc;
        };
        vfs.xDelete = [](sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; };
        return ::sqlite3_vfs_register(&vfs, 0) == SQLITE_OK ? &vfs : nullptr;
      }();
      if (!registered) throw SQLiteError("Live read-only SQLite VFS is unavailable", "SQLITE_CANTOPEN", 14, -1);
      return registered->zName;
    }
#endif

    std::shared_ptr<Database::Impl> openDatabase(std::string filename, int flags, bool strict, bool safeIntegers, std::string property,
                                                 const char* vfs = nullptr) {
      const bool anonymous = filename.empty() || filename == ":memory:";
      if (anonymous && (flags & constants::SQLITE_OPEN_READONLY) != 0) throw JsError("Cannot open an anonymous database in read-only mode.");
      const Api& sqlite = api();
#if defined(__APPLE__)
      bool readonly = false;
      const std::string path = anonymous ? std::string(":memory:") : detail::darwinDatabasePath(toSqlite(filename), flags, readonly);
#else
      const std::string path = anonymous ? std::string(":memory:") : toSqlite(filename);
#endif
      Connection3* handle = nullptr;
#if defined(__APPLE__)
      const int status = sqlite.open_v2(path.c_str(), &handle, flags, readonly ? detail::darwinReadonlyVfs(sqlite) : vfs);
#else
      const int status = sqlite.open_v2(path.c_str(), &handle, flags, vfs);
#endif
      if (status != SQLITE_OK) {
        if (!handle) throw SQLiteError(fromSqlite(sqlite.errstr(status)), resultCodeName(status), status, -1);
        SQLiteError error = connectionError(handle);
        sqlite.close_v2(handle);
        throw error;
      }
      sqlite.extended_result_codes(handle, 1);
#if defined(__APPLE__)
      // Apple's SQLite, used by Bun on Darwin, keeps WAL sidecars by default.
      // A later fileControl(PERSIST_WAL, 0) must still disable that behavior.
      int persistWal = 1;
      sqlite.file_control(handle, nullptr, constants::SQLITE_FCNTL_PERSIST_WAL, &persistWal);
#endif
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      // Bun and the retained SDK reject expression trees deeper than 1000.
      ::sqlite3_limit(handle, SQLITE_LIMIT_EXPR_DEPTH, 1000);
#endif
      auto database = std::make_shared<Database::Impl>();
      database->connection = std::make_shared<Connection>(handle);
      database->filename = std::move(property);
      database->strict = strict;
      database->safeIntegers = safeIntegers;
      return database;
    }

    // `SQL.prepare`: RangeError on a closed database or empty text, SQLiteError when SQLite
    // rejects it, Error "Statement has finalized" (from reading paramsCount) for text holding
    // no statement.
    std::shared_ptr<Statement::Impl> prepareStatement(const Database::Impl& database, std::string_view sql, unsigned flags, bool owned) {
      const Connection& connection = *database.connection;
      if (connection.closed) throw RangeError("Cannot use a closed database");
      if (sql.empty()) throw RangeError("Invalid SQL statement");
      const Api& sqlite = api();
      const std::string text = toSqlite(sql);
      Statement3* prepared = nullptr;
      if (sqlite.prepare_v3(connection.handle, text.data(), clampLength(text.size()), flags, &prepared, nullptr) != SQLITE_OK) throw connectionError(connection.handle);
      if (!prepared) throw JsError("Statement has finalized");
      auto statement = std::make_shared<Statement::Impl>();
      statement->connection = database.connection;
      statement->statement = prepared;
      statement->owned = owned;
      statement->strict = database.strict;
      statement->safeIntegers = database.safeIntegers;
      statement->paramsCount = sqlite.bind_parameter_count(prepared);
      database.connection->track(statement);
      return statement;
    }

    // `SQL.run`: every statement of the text; parameters bind to the first one.
    Changes runDatabase(const Database::Impl& database, std::string_view sql, const std::vector<Binding>* positional, const Named* named) {
      const Connection& connection = *database.connection;
      if (connection.closed) throw JsError("Database has closed");
      if (sql.empty()) throw JsError("SQL string mustn't be blank");
      const Api& sqlite = api();
      const std::string text = toSqlite(sql);
      const long long before = sqlite.total_changes64(connection.handle);
      const char* tail = text.data();
      const char* const end = text.data() + text.size();
      bool executed = false;
      while (tail < end) {
        while (tail < end && (isAsciiSpace(*tail) || *tail == ';')) ++tail;
        if (tail == end) break;
        Statement3* statement = nullptr;
        const char* next = nullptr;
        if (sqlite.prepare_v3(connection.handle, tail, clampLength(static_cast<std::size_t>(end - tail)), 0, &statement, &next) != SQLITE_OK)
          throw connectionError(connection.handle);
        if (!next) next = end;
        if (!statement) {
          if (next <= tail) break;
          tail = next;
          continue;
        }
        if (!executed) {
          try {
            if (positional) bindPositional(statement, connection.handle, *positional, database.safeIntegers);
            if (named) bindNamed(statement, connection.handle, *named, database.strict, database.safeIntegers);
          } catch (...) {
            sqlite.finalize(statement);
            throw;
          }
        }
        executed = true;
        int status = SQLITE_ROW;
        while (status == SQLITE_ROW) status = sqlite.step(statement);
        if (status != SQLITE_DONE && next == end) {
          SQLiteError error = connectionError(connection.handle);
          sqlite.finalize(statement);
          throw error;
        }
        sqlite.finalize(statement);
        tail = next;
      }
      if (!executed) throw JsError("Query contained no valid SQL statement; likely empty query.");
      return Changes{.changes = static_cast<double>(sqlite.total_changes64(connection.handle) - before), .lastInsertRowid = lastRowid(connection.handle, database.safeIntegers)};
    }

    int optionFlags(const DatabaseOptions& options) {
      int flags = 0;
      if (options.readonly.value_or(false)) flags = constants::SQLITE_OPEN_READONLY;
      if (options.create.value_or(false)) flags = constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE;
      if (options.readwrite.value_or(false)) flags |= constants::SQLITE_OPEN_READWRITE;
      if ((options.strict || options.safeIntegers) && flags == 0) flags = constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE;
      return flags;
    }
  }

  // ---- values ------------------------------------------------------------------------------

  json::value toJson(const Value& value) {
    return std::visit(
      [](const auto& alternative) -> json::value {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) return nullptr;
        else if constexpr (std::is_same_v<T, std::int64_t>) throw TypeError("bun:sqlite value is a bigint, not JSON");
        else if constexpr (std::is_same_v<T, double>) return makeNumber(alternative);
        else if constexpr (std::is_same_v<T, std::string>) return json::value(alternative);
        else throw TypeError("bun:sqlite value is a Uint8Array, not JSON");
      },
      value);
  }

  json::object toJson(const Row& row) {
    json::object object;
    for (const auto& [name, value] : row) object.emplace(name, toJson(value));
    return object;
  }

  // ---- Statement -----------------------------------------------------------------------------

  Statement::Statement(std::shared_ptr<Impl> impl_) : impl(std::move(impl_)) {}

  std::optional<Row> Statement::get() const {
    restart(*impl, nullptr, nullptr);
    return getRow(*impl);
  }
  std::optional<Row> Statement::get(const std::vector<Binding>& params) const {
    restart(*impl, takesArguments(*impl, params) ? &params : nullptr, nullptr);
    return getRow(*impl);
  }
  std::optional<Row> Statement::get(const Named& params) const {
    restart(*impl, nullptr, impl->paramsCount != 0 ? &params : nullptr);
    return getRow(*impl);
  }

  std::vector<Row> Statement::all() const {
    restart(*impl, nullptr, nullptr);
    return allRows(*impl);
  }
  std::vector<Row> Statement::all(const std::vector<Binding>& params) const {
    restart(*impl, takesArguments(*impl, params) ? &params : nullptr, nullptr);
    return allRows(*impl);
  }
  std::vector<Row> Statement::all(const Named& params) const {
    restart(*impl, nullptr, impl->paramsCount != 0 ? &params : nullptr);
    return allRows(*impl);
  }

  void Statement::forEachRow(const std::vector<Binding>& params, const std::function<void(Row)>& visit) const {
    restart(*impl, takesArguments(*impl, params) ? &params : nullptr, nullptr);
    visitRows(*impl, visit);
  }

  RowIterator Statement::iterate() const { return RowIterator(impl); }
  RowIterator Statement::iterate(const std::vector<Binding>& params) const {
    RowIterator iterator(impl);
    if (takesArguments(*impl, params)) {
      iterator.pending = RowIterator::Pending::positional;
      iterator.positional = params;
    }
    return iterator;
  }
  RowIterator Statement::iterate(const Named& params) const {
    RowIterator iterator(impl);
    if (impl->paramsCount != 0) {
      iterator.pending = RowIterator::Pending::named;
      iterator.named = params;
    }
    return iterator;
  }

  Changes Statement::run() const {
    restart(*impl, nullptr, nullptr);
    return runStatement(*impl);
  }
  Changes Statement::run(const std::vector<Binding>& params) const {
    restart(*impl, takesArguments(*impl, params) ? &params : nullptr, nullptr);
    return runStatement(*impl);
  }
  Changes Statement::run(const Named& params) const {
    restart(*impl, nullptr, impl->paramsCount != 0 ? &params : nullptr);
    return runStatement(*impl);
  }

  int Statement::paramsCount() const {
    impl->requireAlive();
    return impl->paramsCount;
  }
  bool Statement::safeIntegers() const {
    impl->requireAlive();
    return impl->safeIntegers;
  }
  const Statement& Statement::safeIntegers(bool enabled) const {
    impl->requireAlive();
    impl->safeIntegers = enabled;
    return *this;
  }
  bool Statement::isFinalized() const { return impl->state != Impl::State::alive; }
  void Statement::finalize() const { impl->finalizeAs(Impl::State::finalized); }

  // ---- RowIterator ---------------------------------------------------------------------------

  std::optional<Row> RowIterator::next() {
    if (done) return std::nullopt;
    try {
      Statement::Impl& impl = *statement;
      impl.requireAlive();
      const Pending binding = std::exchange(pending, Pending::none);
      if (binding == Pending::positional) bindPositional(impl.statement, impl.connection->handle, positional, impl.safeIntegers);
      if (binding == Pending::named) bindNamed(impl.statement, impl.connection->handle, *named, impl.strict, impl.safeIntegers);
      const Api& sqlite = api();
      const int status = sqlite.step(impl.statement);
      if (status == SQLITE_ROW) return readRow(impl.statement, columnsOf(impl.statement), impl.safeIntegers);
      if (status == SQLITE_DONE) {
        sqlite.reset(impl.statement);
        done = true;
        return std::nullopt;
      }
      throw impl.error();
    } catch (...) {
      done = true;
      throw;
    }
  }

  // ---- Database ------------------------------------------------------------------------------

  Database::Database() : impl(openDatabase(":memory:", constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE, false, false, ":memory:")) {}

  Database::Database(std::string_view filename) : impl(nullptr) {
    std::string trimmed = trim(filename);
    impl = openDatabase(trimmed, constants::SQLITE_OPEN_READWRITE | constants::SQLITE_OPEN_CREATE, false, false, trimmed);
  }

  Database::Database(std::string_view filename, const DatabaseOptions& options) : impl(nullptr) {
    std::string trimmed = trim(filename);
    const bool present = options.strict.has_value() || options.safeIntegers.has_value();
    impl = openDatabase(trimmed, optionFlags(options), present && options.strict.value_or(false), present && options.safeIntegers.value_or(false), trimmed);
  }

  Database::Database(std::string_view filename, int flags) : impl(nullptr) {
    std::string trimmed = trim(filename);
    impl = openDatabase(trimmed, flags, false, false, trimmed);
  }

  Database Database::openLiveReadOnly(std::string_view absolutePath) {
#if defined(__linux__) && defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    // The Unix VFS may fchown a shared-memory descriptor when run as root.
    if (::geteuid() == 0 || !absolutePath.starts_with('/'))
      throw JsError("Live read-only SQLite requires an absolute path and an unprivileged caller.");
    auto uri = pathToFileURL(absolutePath);
    uri.searchParams().set("mode", "ro");
    uri.searchParams().set("readonly_shm", "1");
    auto database = openDatabase(uri.href(), constants::SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOFOLLOW,
                                 false, true, std::string(absolutePath), liveReadVfs());
    auto* handle = database->connection->handle;
    if (::sqlite3_db_config(handle, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr) != SQLITE_OK ||
        ::sqlite3_db_config(handle, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) != SQLITE_OK ||
        ::sqlite3_busy_timeout(handle, 5) != SQLITE_OK)
      throw JsError("Live read-only SQLite connection could not be configured.");
    return Database(std::move(database));
#else
    (void)absolutePath;
    throw JsError("Live read-only SQLite requires the native Linux SQLite build.");
#endif
  }

  const std::string& Database::filename() const { return impl->filename; }

  Statement Database::query(std::string_view sql) const {
    if (sql.empty()) throw JsError("SQL query cannot be empty.");
    auto& cache = impl->queryCache;
    const auto cached = std::find_if(cache.begin(), cache.end(), [sql](const auto& entry) { return entry.first == sql; });
    if (cached != cache.end()) {
      std::shared_ptr<Statement::Impl> statement = cached->second;
      cache.erase(cached);
      const bool finalized = statement->state != Statement::Impl::State::alive;
      if (finalized) statement = prepareStatement(*impl, sql, constants::SQLITE_PREPARE_PERSISTENT, true);
      cache.emplace_back(std::string(sql), statement);
      if (finalized) impl->reportStatements();
      return Statement(std::move(statement));
    }
    std::shared_ptr<Statement::Impl> statement = prepareStatement(*impl, sql, constants::SQLITE_PREPARE_PERSISTENT, true);
    if (MAX_QUERY_CACHE_SIZE > 0) {
      if (cache.size() >= MAX_QUERY_CACHE_SIZE) cache.erase(cache.begin());
      cache.emplace_back(std::string(sql), statement);
      impl->reportStatements();
    }
    return Statement(std::move(statement));
  }

  Statement Database::prepare(std::string_view sql) const { return Statement(prepareStatement(*impl, sql, 0, false)); }

  Changes Database::run(std::string_view sql) const { return runDatabase(*impl, sql, nullptr, nullptr); }
  Changes Database::run(std::string_view sql, const std::vector<Binding>& params) const {
    return runDatabase(*impl, sql, params.empty() ? nullptr : &params, nullptr);
  }
  Changes Database::run(std::string_view sql, const Named& params) const { return runDatabase(*impl, sql, nullptr, &params); }

  bool Database::inTransaction() const {
    const Connection& connection = *impl->connection;
    if (connection.closed) throw JsError("Database has closed");
    return api().get_autocommit(connection.handle) == 0;
  }

  std::optional<int> Database::fileControl(int op, int value) const {
    const Connection& connection = *impl->connection;
    if (connection.closed) return std::nullopt;
    int argument = value;
    return api().file_control(connection.handle, nullptr, op, &argument);
  }

  void Database::reportPageCacheMemory(std::size_t pageBytes) const noexcept {
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    if (impl->connection->closed || pageBytes == 0) return;
    int used = 0;
    int unused = 0;
    if (::sqlite3_db_status(impl->connection->handle, SQLITE_DBSTATUS_CACHE_USED, &used, &unused, 0) == SQLITE_OK && used >= 0) {
      const auto bytes = static_cast<std::size_t>(used);
      impl->pageMemory.set(bytes / pageBytes, bytes);
    }
#else
    (void)pageBytes;
#endif
  }

  void Database::close(bool throwOnError) const {
    impl->queryCache.clear();
    impl->statementMemory.set(0, 0);
    impl->pageMemory.set(0, 0);
    impl->controller.reset();
    Connection& connection = *impl->connection;
    for (const std::weak_ptr<Statement::Impl>& entry : connection.statements) {
      if (const std::shared_ptr<Statement::Impl> statement = entry.lock()) {
        if (throwOnError || statement->owned) statement->finalizeAs(Statement::Impl::State::closed);
      }
    }
    std::erase_if(connection.statements, [](const std::weak_ptr<Statement::Impl>& entry) {
      const std::shared_ptr<Statement::Impl> statement = entry.lock();
      return !statement || statement->state != Statement::Impl::State::alive;
    });
    if (connection.closed) return;
    const Api& sqlite = api();
    if (!throwOnError) {
      sqlite.close_v2(connection.handle);
      connection.closed = true;
      return;
    }
    if (sqlite.close(connection.handle) != SQLITE_OK) throw connectionError(connection.handle);
    connection.closed = true;
    connection.handle = nullptr;
  }

  std::shared_ptr<const detail::TransactionController> Database::controller() const {
    if (!impl->controller) {
      Statement commit(prepareStatement(*impl, "COMMIT", 0, true));
      Statement rollback(prepareStatement(*impl, "ROLLBACK", 0, true));
      Statement savepoint(prepareStatement(*impl, "SAVEPOINT `\t_bs3.\t`", 0, true));
      Statement release(prepareStatement(*impl, "RELEASE `\t_bs3.\t`", 0, true));
      Statement rollbackTo(prepareStatement(*impl, "ROLLBACK TO `\t_bs3.\t`", 0, true));
      Statement begin(prepareStatement(*impl, "BEGIN", 0, true));
      Statement beginDeferred(prepareStatement(*impl, "BEGIN DEFERRED", 0, true));
      Statement beginImmediate(prepareStatement(*impl, "BEGIN IMMEDIATE", 0, true));
      Statement beginExclusive(prepareStatement(*impl, "BEGIN EXCLUSIVE", 0, true));
      impl->controller = std::make_shared<const detail::TransactionController>(detail::TransactionController{
        .commit = commit,
        .rollback = rollback,
        .savepoint = savepoint,
        .release = release,
        .rollbackTo = rollbackTo,
        .begin = begin,
        .beginDeferred = beginDeferred,
        .beginImmediate = beginImmediate,
        .beginExclusive = beginExclusive,
      });
    }
    return impl->controller;
  }

  void detail::runTransaction(const Database& database, const TransactionController& controller, TransactionKind kind, const std::function<void()>& body) {
    const Statement* before = nullptr;
    const Statement* after = nullptr;
    const Statement* undo = nullptr;
    if (database.inTransaction()) {
      before = &controller.savepoint;
      after = &controller.release;
      undo = &controller.rollbackTo;
    } else {
      switch (kind) {
        case TransactionKind::default_: before = &controller.begin; break;
        case TransactionKind::deferred: before = &controller.beginDeferred; break;
        case TransactionKind::immediate: before = &controller.beginImmediate; break;
        case TransactionKind::exclusive: before = &controller.beginExclusive; break;
      }
      after = &controller.commit;
      undo = &controller.rollback;
    }
    try {
      before->run();
      body();
      after->run();
    } catch (...) {
      if (database.inTransaction()) {
        undo->run();
        if (!(*undo == controller.rollback)) after->run();
      }
      throw;
    }
  }

  std::string libraryVersion() { return api().libversion(); }
}
