// Analysis input only: never compile, link or execute these deliberate defects.
#include <cstdlib>
#include <memory>
#include <utility>
#include <release.hpp>

int malloc_branch(bool cleanup) {
  int* pointer = static_cast<int*>(std::malloc(sizeof(int)));
  if (!pointer) return 0;
  if (cleanup) { std::free(pointer); return 1; }
  return 0; // expect-malloc
}

int new_branch(bool abandon) {
  int* pointer = new int(42);
  if (abandon) return 0; // expect-new-leak
  int result = *pointer;
  delete pointer;
  return result;
}

int stale_new_pointer() {
  int* pointer = new int(42);
  delete pointer;
  return *pointer; // expect-new-uaf
}

void mismatched_release() {
  int* pointer = new int(42); // allocation-owned
  fixture_sdk_release(pointer);
}

void consume_value(int);
void uninitialized_argument() {
  int value;
  int* alias = &value;
  consume_value(*alias); // expect-uninitialized
}

void genuine_same_dispatch(std::unique_ptr<int> self) {
  auto destination=std::move(self);
  *self=7; // expect-genuine-move
}
