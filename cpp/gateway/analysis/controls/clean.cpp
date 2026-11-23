#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int checkedOptional(const std::optional<int>& value) {
  return value ? *value : 0;
}

std::size_t owningView() {
  const std::string owner = "persistent owner";
  const std::string_view view = owner;
  return view.size();
}

int movedOwner() {
  auto original = std::make_unique<int>(3);
  auto owner = std::move(original);
  original = std::make_unique<int>(2);
  return *original + *owner;
}

int releasedAllocation() {
  int* value = static_cast<int*>(std::malloc(sizeof(int)));
  if (!value) return 0;
  *value = 1;
  const int result = *value;
  std::free(value);
  return result;
}
