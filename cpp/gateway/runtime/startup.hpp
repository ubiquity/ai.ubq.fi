#pragma once

#include "uos/rt/async.hpp"

#include <string>

namespace uos::runtime::startup {
  // The native executable has no implicit development/source-checkout fallback.
  std::string manifestArgument(int argc, const char* const* argv);

  // Owns verified resources, fixture routing and the real durable store for the
  // full source-faithful serve/shutdown lifetime.
  rt::Task<void> run(std::string manifestPath);

  // Separate immutable-only policy for explicitly invoked offline conversion
  // tools: preserve caller cwd/argv and the pinned helper's process permissions.
  void prepareOfflineCli(int argc, const char* const* argv);
}
