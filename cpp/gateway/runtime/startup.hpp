#pragma once

#include "uos/rt/async.hpp"

#include <optional>
#include <string>

namespace uos::runtime::startup {
  // The native executable has no implicit development/source-checkout fallback.
  struct LaunchArguments {
    std::string manifestPath;
    std::optional<std::string> codexAuthSource;
  };
  LaunchArguments parseArguments(int argc, const char* const* argv);

  // Owns verified resources, fixture routing and the real durable store for the
  // full source-faithful serve/shutdown lifetime.
  rt::Task<void> run(LaunchArguments arguments);
}
