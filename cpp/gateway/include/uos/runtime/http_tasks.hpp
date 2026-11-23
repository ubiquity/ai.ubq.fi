#pragma once
#include "uos/rt/async.hpp"

#include <functional>
#include <optional>

namespace uos::runtime::http_tasks {
  /** Associates durable application writes with the real transport receipt. */
  void trackRequestTask(std::optional<rt::Promise<void>> completed, rt::Promise<void> task);

  std::function<rt::Promise<void>()> createRequestTaskDrain(rt::Promise<void> completed);
}
