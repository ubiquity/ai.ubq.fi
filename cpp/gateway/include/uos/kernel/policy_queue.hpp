#pragma once
// Port of src/kernel/policy-queue.ts.
//
// The queue normalizer reads the stored array as rt::KvValue (a stored element may hold any
// structured-clone type), as the TS reads `unknown` items.
#include "uos/rt/async.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::kernel::policy_queue {
  rt::Task<void> recordKernelPolicyQueue(std::string owner, std::string repo, std::string route);

  // std::nullopt is null.
  rt::Task<std::optional<std::vector<::uos::types::KernelPolicyQueueItem>>> listKernelPolicyQueue();
}
