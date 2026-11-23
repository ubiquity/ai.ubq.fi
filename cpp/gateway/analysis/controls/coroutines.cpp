#include <coroutine>
#include <mutex>

struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
};

Task referenceParameter(int& value) {  // CONTROL: cppcoreguidelines-avoid-reference-coroutine-parameters
  co_await std::suspend_always{};
  ++value;
}

void capturedCoroutine(int value) {
  auto callback = [value]() -> Task {  // CONTROL: cppcoreguidelines-avoid-capturing-lambda-coroutines
    co_await std::suspend_always{};
    (void)value;
  };
  (void)callback;
}

Task hostileRaii(std::mutex* mutex) {
  std::lock_guard<std::mutex> lock(*mutex);  // CONTROL: misc-coroutine-hostile-raii
  co_await std::suspend_always{};
}
