#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace uos::rt {
  namespace asio = boost::asio;

  namespace detail {
    struct AbortSignalState {
      struct Entry {
        EventListener listener;
        bool once = false;
        // The internal listener of the `onabort` event handler.
        bool handler = false;
        bool removed = false;
      };
      bool aborted = false;
      Reason reason;
      std::vector<std::shared_ptr<Entry>> listeners;
      std::optional<EventListener> onabort;
      std::shared_ptr<Entry> onabortEntry;
      // AbortSignal.any: a dependent signal follows its (non-dependent) sources.
      bool dependent = false;
      std::vector<std::weak_ptr<AbortSignalState>> sources;
      std::vector<std::weak_ptr<AbortSignalState>> dependents;
      // Dependents that have listeners: JS keeps such a signal alive while a
      // source can still abort it, so the sources own it.
      std::vector<std::shared_ptr<AbortSignalState>> pinned;
      bool pinnedBySources = false;
    };
  }

  namespace {
    using State = detail::AbortSignalState;

    void updatePin(const std::shared_ptr<State>& signal) {
      const bool wanted = signal->dependent && !signal->aborted && !signal->listeners.empty();
      if (wanted == signal->pinnedBySources) return;
      signal->pinnedBySources = wanted;
      for (const auto& weak : signal->sources) {
        auto source = weak.lock();
        if (!source) continue;
        auto& pinned = source->pinned;
        if (wanted) pinned.push_back(signal);
        else pinned.erase(std::remove(pinned.begin(), pinned.end(), signal), pinned.end());
      }
    }

    void removeEntry(State& signal, const std::shared_ptr<State::Entry>& entry) {
      entry->removed = true;
      auto& listeners = signal.listeners;
      listeners.erase(std::remove(listeners.begin(), listeners.end(), entry), listeners.end());
    }

    // Fires the abort event, then releases what an aborted signal never needs again.
    void runAbortSteps(const std::shared_ptr<State>& signal) {
      auto snapshot = signal->listeners;
      for (const auto& entry : snapshot) {
        if (entry->removed) continue;
        if (entry->once) removeEntry(*signal, entry);
        EventListener listener = entry->listener;
        try {
          listener();
        } catch (...) {
          detail::reportUncaught(std::current_exception(), false);
        }
      }
      for (const auto& entry : signal->listeners) entry->removed = true;
      signal->listeners.clear();
      signal->onabortEntry.reset();
      if (signal->dependent) updatePin(signal);
      signal->sources.clear();
    }

    void signalAbort(std::shared_ptr<State> signal, Reason reason) {
      if (signal->aborted) return;
      signal->aborted = true;
      signal->reason = reason ? std::move(reason) : abortErrorReason();
      std::vector<std::shared_ptr<State>> dependentsToAbort;
      for (const auto& weak : signal->dependents) {
        auto dependent = weak.lock();
        if (!dependent || dependent->aborted) continue;
        dependent->aborted = true;
        dependent->reason = signal->reason;
        dependentsToAbort.push_back(std::move(dependent));
      }
      signal->dependents.clear();
      auto pinned = std::exchange(signal->pinned, {});
      runAbortSteps(signal);
      for (const auto& dependent : dependentsToAbort) runAbortSteps(dependent);
    }

    int legacyCode(const std::string& name) noexcept {
      static const std::pair<const char*, int> table[] = {
        {"IndexSizeError", 1},          {"HierarchyRequestError", 3},     {"WrongDocumentError", 4},  {"InvalidCharacterError", 5},
        {"NoModificationAllowedError", 7}, {"NotFoundError", 8},          {"NotSupportedError", 9},   {"InUseAttributeError", 10},
        {"InvalidStateError", 11},      {"SyntaxError", 12},              {"InvalidModificationError", 13}, {"NamespaceError", 14},
        {"InvalidAccessError", 15},     {"TypeMismatchError", 17},        {"SecurityError", 18},      {"NetworkError", 19},
        {"AbortError", 20},             {"URLMismatchError", 21},         {"QuotaExceededError", 22}, {"TimeoutError", 23},
        {"InvalidNodeTypeError", 24},   {"DataCloneError", 25},
      };
      for (const auto& [entryName, value] : table) {
        if (name == entryName) return value;
      }
      return 0;
    }
  }

  DOMException::DOMException(std::string message_, std::string name_) : JsError(std::move(message_), std::move(name_)) {}

  int DOMException::code() const noexcept { return legacyCode(name); }

  Reason abortErrorReason() { return makeReason(DOMException("The operation was aborted.", "AbortError")); }
  Reason timeoutErrorReason() { return makeReason(DOMException("The operation timed out.", "TimeoutError")); }

  AbortSignal::AbortSignal(std::shared_ptr<detail::AbortSignalState> signalState) : state_(std::move(signalState)) {}

  bool AbortSignal::aborted() const noexcept { return state_->aborted; }
  const Reason& AbortSignal::reason() const noexcept { return state_->reason; }

  void AbortSignal::throwIfAborted() const {
    if (state_->aborted) rethrow(state_->reason);
  }

  void AbortSignal::addEventListener(std::string_view type, EventListener listener, AddEventListenerOptions options) const {
    if (type != "abort" || !listener || state_->aborted) return;
    for (const auto& entry : state_->listeners) {
      if (!entry->handler && entry->listener == listener) return;
    }
    auto entry = std::make_shared<State::Entry>();
    entry->listener = std::move(listener);
    entry->once = options.once;
    state_->listeners.push_back(std::move(entry));
    updatePin(state_);
  }

  void AbortSignal::removeEventListener(std::string_view type, const EventListener& listener) const {
    if (type != "abort" || !listener) return;
    for (const auto& entry : state_->listeners) {
      if (!entry->handler && entry->listener == listener) {
        removeEntry(*state_, entry);
        break;
      }
    }
    updatePin(state_);
  }

  void AbortSignal::setOnabort(std::optional<EventListener> handler) const {
    if (!handler || !*handler) {
      state_->onabort.reset();
      if (state_->onabortEntry) {
        removeEntry(*state_, state_->onabortEntry);
        state_->onabortEntry.reset();
      }
    } else {
      const EventListener listener = *handler;
      if (state_->onabort) *state_->onabort = listener;
      else state_->onabort.emplace(listener);
      if (state_->onabortEntry) {
        state_->onabortEntry->listener = listener;
      } else if (!state_->aborted) {
        auto entry = std::make_shared<State::Entry>();
        entry->listener = listener;
        entry->handler = true;
        state_->onabortEntry = entry;
        state_->listeners.push_back(std::move(entry));
      }
    }
    updatePin(state_);
  }

  std::optional<EventListener> AbortSignal::onabort() const { return state_->onabort; }

  AbortSignal AbortSignal::abort(Reason reason) {
    auto state = std::make_shared<State>();
    state->aborted = true;
    state->reason = reason ? std::move(reason) : abortErrorReason();
    return AbortSignal(std::move(state));
  }

  AbortSignal AbortSignal::timeout(double ms) {
    if (!(ms >= 0) || std::isinf(ms)) throw TypeError("Value " + numberToString(ms) + " is outside the range [0, 9007199254740991]");
    auto state = std::make_shared<State>();
    auto timer = detail::createExactTimer([state] { signalAbort(state, timeoutErrorReason()); }, ms);
    timer.unref();
    return AbortSignal(std::move(state));
  }

  AbortSignal AbortSignal::any(const std::vector<AbortSignal>& signals) {
    auto result = std::make_shared<State>();
    for (const auto& signal : signals) {
      if (signal.aborted()) {
        result->aborted = true;
        result->reason = signal.reason();
        return AbortSignal(std::move(result));
      }
    }
    result->dependent = true;
    auto addSource = [&result](const std::shared_ptr<State>& source) {
      for (const auto& existing : result->sources) {
        if (existing.lock() == source) return;
      }
      result->sources.push_back(source);
      auto& dependents = source->dependents;
      dependents.erase(std::remove_if(dependents.begin(), dependents.end(), [](const std::weak_ptr<State>& weak) { return weak.expired(); }), dependents.end());
      dependents.push_back(result);
    };
    for (const auto& signal : signals) {
      if (!signal.state()->dependent) {
        addSource(signal.state());
        continue;
      }
      for (const auto& weak : signal.state()->sources) {
        if (auto source = weak.lock()) addSource(source);
      }
    }
    return AbortSignal(std::move(result));
  }

  AbortController::AbortController() : signal_(std::make_shared<State>()) {}

  void AbortController::abort(Reason reason) const { signalAbort(signal_.state(), std::move(reason)); }

  namespace detail {
    void runAbortable(const AbortSignal& signal, std::shared_ptr<PromiseCore> out, Task<void> child) {
      auto cancel = std::make_shared<asio::cancellation_signal>();
      std::weak_ptr<AbortSignalState> weak = signal.state();
      EventListener onAbort([out, cancel, weak] {
        auto state = weak.lock();
        out->reject(state ? state->reason : Reason());
        cancel->emit(asio::cancellation_type::terminal);
      });
      signal.addEventListener("abort", onAbort, {.once = true});
      asio::co_spawn(currentLoop().executor(), std::move(child), asio::bind_cancellation_slot(cancel->slot(), [cancel, signal, onAbort](std::exception_ptr error) {
                       signal.removeEventListener("abort", onAbort);
                       if (error) reportUncaught(error, false);
                     }));
    }
  }

  Task<void> sleep(double ms, AbortSignal signal) {
    if (signal.aborted()) return detail::rejected<void>(signal.reason());
    auto out = std::make_shared<detail::PromiseState<void>>();
    auto timer = std::make_shared<Timeout>();
    std::weak_ptr<detail::AbortSignalState> weak = signal.state();
    EventListener onAbort([out, timer, weak] {
      clearTimeout(*timer);
      auto state = weak.lock();
      out->reject(state ? state->reason : Reason());
    });
    *timer = setTimeout(
        [out, signal, onAbort] {
          signal.removeEventListener("abort", onAbort);
          out->fulfill();
        },
        ms);
    signal.addEventListener("abort", onAbort, {.once = true});
    return detail::awaitOwned(std::move(out));
  }
}
