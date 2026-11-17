// Reduced witnesses for clang-tidy's flow-insensitive move analysis. They retain
// the original branching shape; the real parser harness supplies runtime coverage.
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

std::vector<std::string> insertAt(std::vector<std::string> members, std::size_t position, std::string value) {
  std::vector<std::string> rebuilt;
  std::size_t cursor = 0;
  for (auto& member : members) {
    if (cursor++ == position) rebuilt.push_back(std::move(value));
    rebuilt.push_back(std::move(member));
  }
  return rebuilt;
}

std::string parserState(std::vector<std::vector<std::string>> stack, std::vector<std::string> tokens) {
  std::string last;
  std::size_t cursor = 0;
  while (cursor < tokens.size()) {
    last = tokens[cursor++];
    while (true) {
      if (stack.empty()) return last;
      auto& frame = stack.back();
      frame.push_back(std::move(last));
      if (cursor < tokens.size() && tokens[cursor] == ",") {
        ++cursor;
        break;
      }
      last = frame.front();
      stack.pop_back();
    }
  }
  return last;
}
