#pragma once

#include <cstdint>
#include <optional>

#if defined(__linux__)
#include <string>
#include <string_view>
#include <vector>
#endif

namespace uos::rt::admission_resources {
  struct Headroom {
    // Null means the required kernel counter was unavailable.
    std::optional<std::uint64_t> memoryBytes;
    std::optional<std::uint64_t> descriptors;
  };

  Headroom readHeadroom();

#if defined(__linux__)
  namespace detail {
    // Capture the exact counter files during bootstrap, before Landlock.
    void appendLinuxReadPaths(std::vector<std::string>& paths);
    bool isLinuxReadPath(std::string_view path);
    int openLinuxReadGrant(std::string_view path);

    std::optional<std::uint64_t> linuxHostHeadroom(std::string_view meminfo);
    std::optional<std::uint64_t> linuxLimitedHeadroom(std::string_view maximum, std::string_view current);
  }
#endif
}
