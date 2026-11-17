#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void movedOwner() {
  std::vector<int> original{1};
  std::vector<int> owner = std::move(original);
  original.push_back(2);  // CONTROL: bugprone-use-after-move
  owner.push_back(3);
}

int uncheckedOptional(std::optional<int> value) {
  return value.value();  // CONTROL: bugprone-unchecked-optional-access
}

std::size_t danglingView() {
  std::string_view value = std::string("temporary owner");  // CONTROL: bugprone-dangling-handle
  return value.size();
}

int freedAllocation() {
  int* value = static_cast<int*>(std::malloc(sizeof(int)));
  if (!value) return 0;
  *value = 1;
  std::free(value);
  return *value;  // CONTROL: clang-analyzer-unix.Malloc
}

int nullDereference() {
  int* value = nullptr;
  return *value;  // CONTROL: clang-analyzer-core.NullDereference
}
