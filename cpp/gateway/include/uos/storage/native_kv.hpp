#pragma once
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/string.hpp"
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace uos::storage::native_kv {
  /** SQLite's native addon reports busy errors rather than waiting for another writer. */
  template <class T>
  rt::Task<T> retrySqliteBusy(std::function<rt::Task<T>()> run) {
    const double deadline = rt::dateNow() + 5'000;
    for (;;) {
      rt::Reason failure;
      try {
        if constexpr (std::is_void_v<T>) {
          co_await run();
          co_return;
        } else {
          T value = co_await run();
          co_return std::move(value);
        }
      } catch (...) {
        failure = std::current_exception();
      }
      const std::optional<std::string> message = rt::errorMessage(failure);
      if (!rt::isError(failure) || !message || !rt::includes(*message, "database is locked") || rt::dateNow() >= deadline) std::rethrow_exception(failure);
      co_await rt::sleep(10);
    }
  }

  /** Preserve the native API while guarding arithmetic rollback and SQLite contention. */
  // `kv` is the SDK store; the guard keeps it alive, as the TypeScript closures do.
  std::unique_ptr<rt::Kv> guardNativeKv(std::shared_ptr<rt::Kv> kv);
}
