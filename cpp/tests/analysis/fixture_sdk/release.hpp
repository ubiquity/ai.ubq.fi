#pragma once
#include <cstdlib>
inline void fixture_sdk_release(int* pointer) {
  std::free(pointer); // expect-mismatch
}
