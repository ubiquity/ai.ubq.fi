#include "uos/runtime/http_tasks.hpp"

#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace uos::runtime::http_tasks {
  namespace {
    // A `Set<Promise<unknown>>`: insertion order, one entry per promise identity.
    using TaskSet = rt::OrderedMap<const void*, rt::Promise<void>>;

    rt::WeakMap<rt::Promise<void>, std::shared_ptr<TaskSet>> requestTasks;

    rt::Task<void> drainRequestTasks(std::shared_ptr<TaskSet> tasks, rt::Promise<void> completed) {
      while (tasks->size() != 0) {
        std::vector<rt::Promise<void>> pending = rt::arrayFrom(tasks->values());
        const auto settled = rt::allSettled(std::move(pending));
        co_await settled.wait();
      }
      requestTasks.delete_(completed);
    }
  }

  void trackRequestTask(std::optional<rt::Promise<void>> completed, rt::Promise<void> task) {
    if (!completed) return;
    const std::shared_ptr<TaskSet>* found = requestTasks.find(*completed);
    if (!found) return;
    const std::shared_ptr<TaskSet> tasks = *found;
    const void* identity = task.state().get();
    tasks->set(identity, task);
    const std::weak_ptr<TaskSet> owner = tasks;
    (void)task
      .finally_([owner, identity] {
        if (const std::shared_ptr<TaskSet> live = owner.lock()) live->delete_(identity);
      })
      .catch_(rt::ignoreRejection);
  }

  std::function<rt::Promise<void>()> createRequestTaskDrain(rt::Promise<void> completed) {
    auto tasks = std::make_shared<TaskSet>();
    requestTasks.set(completed, tasks);
    return [tasks, completed]() { return rt::Promise<void>(drainRequestTasks(tasks, completed)); };
  }
}
