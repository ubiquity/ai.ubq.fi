#pragma once

#include "uos/rt/async.hpp"

#include <memory>
#include <utility>

namespace uos::admin::detail {
  // Invoke the Task factory at the original call site so a synchronous throw
  // still stops setup there. The pending Task can outlive its fail-fast join.
  template <class Callback, class... Args>
  auto invokeOwnedTask(Callback callback, Args&&... args) {
    auto owner = std::make_shared<Callback>(std::move(callback));
    auto task = (*owner)(std::forward<Args>(args)...);
    return rt::async([owner = std::move(owner), task = std::move(task)]() mutable {
      (void)owner;
      return std::move(task);
    });
  }
}
