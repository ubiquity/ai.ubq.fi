#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace uos::kv {
  namespace {
    rt::Task<std::shared_ptr<rt::Kv>> openDefaultKv() { throw rt::JsError("Native KV must be initialized before use"); }

    struct State {
      KvOpener selectedOpener = openDefaultKv;
      std::optional<rt::Promise<std::shared_ptr<rt::Kv>>> openPromise;
      std::shared_ptr<rt::Kv> openedKv;
      double nextOpenAttemptAtMs = 0;
      double openFailureCount = 0;
    };

    State& state() {
      static State value;
      return value;
    }

    /**
     * Uniform value in `[0, 1)` with the same 53-bit resolution as `Math.random`.
     * The reconnect jitter below is the only consumer: it spreads the retries of
     * independent hosts across time, so predictability costs nothing -- but the
     * platform CSPRNG is free here and keeps a reproducible sequence out of the
     * reconnect schedule.
     */
    double randomUnitInterval() {
      std::array<std::uint32_t, 2> words{};
      rt::crypto::getRandomValues(std::span<std::uint32_t>(words));
      const double high = words[0];
      const double low = words[1];
      return (high * rt::Math::pow(2, 21) + rt::shiftRightUnsigned(low, 11)) / rt::Math::pow(2, 53);
    }

    double retryDelayMs(double failureCount) {
      const double capped = rt::Math::min(5'000.0, 250 * rt::Math::pow(2, rt::Math::min(5.0, rt::Math::max(0.0, failureCount - 1))));
      return rt::Math::trunc(capped * (0.75 + randomUnitInterval() * 0.5));
    }

    rt::Task<std::shared_ptr<rt::Kv>> attemptOpenKv() {
      rt::Reason failure;
      try {
        const auto opener = state().selectedOpener;
        rt::Promise<std::shared_ptr<rt::Kv>> opening(opener());
        std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        state().openedKv = kv;
        state().openFailureCount = 0;
        state().nextOpenAttemptAtMs = 0;
        co_return kv;
      } catch (...) {
        failure = std::current_exception();
      }
      state().openFailureCount += 1;
      state().nextOpenAttemptAtMs = rt::dateNow() + retryDelayMs(state().openFailureCount);
      rt::console::error("[ai.ubq.fi] Failed to open KV; a later request will retry:", failure);
      co_return nullptr;
    }

    // `() => Promise.resolve(kv)`
    rt::Task<std::shared_ptr<rt::Kv>> resolvedKv(std::shared_ptr<rt::Kv> kv) { co_return kv; }
  }

  void initializeKv(std::shared_ptr<rt::Kv> kv) {
    if (state().openedKv || state().openPromise) throw rt::JsError("KV has already been initialized");
    state().openedKv = std::move(kv);
  }

  rt::Promise<std::shared_ptr<rt::Kv>> getKv() {
    State& current = state();
    if (current.openedKv) return rt::Promise<std::shared_ptr<rt::Kv>>::resolve(current.openedKv);
    if (rt::dateNow() < current.nextOpenAttemptAtMs) return rt::Promise<std::shared_ptr<rt::Kv>>::resolve(std::shared_ptr<rt::Kv>());
    if (!current.openPromise) {
      rt::Promise<std::shared_ptr<rt::Kv>> attempt(attemptOpenKv());
      current.openPromise = attempt.finally_([] { state().openPromise.reset(); });
    }
    return *current.openPromise;
  }

  void setKvForTest(std::shared_ptr<rt::Kv> kv) { setKvForTest(std::move(kv), KvOpener()); }

  // An empty `opener` is `undefined`.
  void setKvForTest(std::shared_ptr<rt::Kv> kv, KvOpener opener) {
    State& current = state();
    current.openedKv = kv;
    current.selectedOpener = opener ? std::move(opener) : KvOpener([kv] { return resolvedKv(kv); });
    current.openPromise.reset();
    current.openFailureCount = 0;
    current.nextOpenAttemptAtMs = 0;
  }

  void resetKvForTest() { setKvForTest(nullptr, openDefaultKv); }
}
