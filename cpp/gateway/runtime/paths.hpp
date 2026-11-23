#pragma once

#include "uos/rt/process.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace uos::runtime::paths {
  inline std::string& configuredResourceRoot() {
    static std::string root;
    return root;
  }

  inline std::string resourceRoot() {
    const std::string& root = configuredResourceRoot();
    return root.empty() ? rt::process::cwd() : root;
  }

  inline void setResourceRoot(std::string root) {
    std::string& current = configuredResourceRoot();
    if (root.empty() || (!current.empty() && current != root)) throw std::invalid_argument("Immutable runtime resource root cannot change");
    current = std::move(root);
  }
}
