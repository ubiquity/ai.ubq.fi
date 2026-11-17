#include <coroutine>
#include <memory>

struct OwnedTask {
  struct promise_type {
    OwnedTask get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
};

struct State {
  bool cancelled = false;
  int value = 0;
};

OwnedTask ownerAcrossSuspension(std::shared_ptr<State> state) {
  co_await std::suspend_always{};
  if (!state->cancelled) ++state->value;
}

void independentCancellation(std::weak_ptr<State> state) {
  if (const auto owner = state.lock()) owner->cancelled = true;
}
