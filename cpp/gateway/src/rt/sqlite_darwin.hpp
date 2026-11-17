#pragma once

#if defined(__APPLE__)
#include "uos/rt/sqlite.hpp"
#include "uos/rt/url.hpp"

#include <string>
#include <string_view>

namespace uos::rt::sqlite::detail {
  inline std::string darwinDatabasePath(std::string path, int& flags) {
    // Bun uses Apple's SQLite: URI names are enabled, and a readonly WAL
    // connection cannot create its missing shared-memory file. Preserve both
    // contracts when using the separately pinned upstream SQLite library.
    const auto nul = path.find('\0');
    if (nul != std::string::npos) path.resize(nul);
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
    if ((flags & constants::SQLITE_OPEN_READONLY) != 0 || (mode && *mode == "ro")) {
      // Prepend to retain every original URI byte and override readonly_shm=0,
      // which also cannot permit creation through Apple's readonly connection.
      if (hasQuery) path.insert(question + 1, query.empty() ? "readonly_shm=1" : "readonly_shm=1&");
      else path.insert(end, "?readonly_shm=1");
    }
    return path;
  }
}
#endif
