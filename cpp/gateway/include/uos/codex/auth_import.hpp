#pragma once

#include "uos/rt/async.hpp"

#include <cstddef>
#include <string>

namespace uos::codex::auth_import {
  // Called by the confined launcher with disjoint owner-private, checkpointed databases.
  rt::Task<std::size_t> importCodexAuthPool(std::string sourcePath, std::string destinationPath);
}
