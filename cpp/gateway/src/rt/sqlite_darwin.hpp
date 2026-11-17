#pragma once

#if defined(__APPLE__)
#include "uos/rt/sqlite.hpp"
#include "uos/rt/url.hpp"

#include <sqlite3.h>
#undef SQLITE_OPEN_READONLY
#undef SQLITE_OPEN_READWRITE
#undef SQLITE_OPEN_CREATE
#undef SQLITE_OPEN_URI
#undef SQLITE_PREPARE_PERSISTENT
#undef SQLITE_FCNTL_PERSIST_WAL
#if !defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#undef SQLITE_OK
#undef SQLITE_ROW
#undef SQLITE_DONE
#undef SQLITE_INTEGER
#undef SQLITE_FLOAT
#undef SQLITE_TEXT
#undef SQLITE_BLOB
#undef SQLITE_CONFIG_URI
#undef SQLITE_UTF8
#undef SQLITE_UTF16
#undef SQLITE_TRANSIENT
#endif

#include <string>
#include <string_view>

namespace uos::rt::sqlite::detail {
  inline sqlite3_vfs*& darwinReadonlyParent() {
    static sqlite3_vfs* parent = nullptr;
    return parent;
  }

  template <class Api> const char* darwinReadonlyVfs(const Api& sqlite) {
    static sqlite3_vfs* registered = [&] {
      sqlite3_vfs* parent = sqlite.vfs_find(nullptr);
      if (!parent) throw SQLiteError("unable to open database file", "SQLITE_CANTOPEN", 14, -1);
      static sqlite3_vfs readonly = *parent;
      darwinReadonlyParent() = parent;
      readonly.pNext = nullptr;
      readonly.zName = "uos-darwin-readonly";
      readonly.xOpen = [](sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* outputFlags) {
        // Apple's readonly WAL reader cannot create a missing WAL. Removing
        // CREATE preserves SQLite's prepare/step timing, including pragmas
        // that do not access the schema, without leaving an empty sidecar.
        if ((flags & SQLITE_OPEN_WAL) != 0) flags &= ~constants::SQLITE_OPEN_CREATE;
        sqlite3_vfs* original = darwinReadonlyParent();
        return original->xOpen(original, name, file, flags, outputFlags);
      };
      if (sqlite.vfs_register(&readonly, 0) != 0) throw SQLiteError("unable to open database file", "SQLITE_CANTOPEN", 14, -1);
      return &readonly;
    }();
    return registered->zName;
  }

  inline std::string darwinDatabasePath(std::string path, int& flags, bool& readonly) {
    // Bun uses Apple's SQLite: URI names are enabled, and a readonly WAL
    // connection cannot create its missing shared-memory file. Preserve both
    // contracts when using the separately pinned upstream SQLite library.
    const auto nul = path.find('\0');
    if (nul != std::string::npos) path.resize(nul);
    readonly = (flags & constants::SQLITE_OPEN_READONLY) != 0;
    const bool fileUri = path.starts_with("file:");
    if (!fileUri && (flags & constants::SQLITE_OPEN_READONLY) == 0) return path;
    if (!fileUri) path = pathToFileURL(path).href();
    flags |= constants::SQLITE_OPEN_URI;
    const auto fragment = path.find('#');
    const auto question = path.find('?');
    const bool hasQuery = question != std::string::npos && (fragment == std::string::npos || question < fragment);
    const std::size_t end = fragment == std::string::npos ? path.size() : fragment;
    const std::string_view query = hasQuery ? std::string_view(path).substr(question + 1, end - question - 1) : std::string_view();
    const URLSearchParams parameters(query);
    const auto mode = parameters.get("mode");
    readonly = readonly || (mode && *mode == "ro");
    if (readonly) {
      // Prepend to retain every original URI byte and override readonly_shm=0,
      // which also cannot permit creation through Apple's readonly connection.
      if (hasQuery) path.insert(question + 1, query.empty() ? "readonly_shm=1" : "readonly_shm=1&");
      else path.insert(end, "?readonly_shm=1");
    }
    return path;
  }
}
#endif
