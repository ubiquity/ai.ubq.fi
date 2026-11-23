#pragma once
// A single-threaded view of the OS environment, copied at first use.
// Children without an explicit environment inherit the OS environment.
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::rt::env {
  // `process.env[name]`; std::nullopt is undefined.
  std::optional<std::string> get(std::string_view name);
  // `name in process.env`
  bool has(std::string_view name);
  // `process.env[name] = value`. JS stores String(value): pass "42" for 42,
  // "undefined" for undefined. Any name and any bytes are accepted ("A=B", NUL).
  // Assigning an existing name keeps its enumeration position.
  void set(std::string_view name, std::string_view value);
  // `delete process.env[name]` / `Reflect.deleteProperty(process.env, name)`: always true.
  bool remove(std::string_view name);
  // `Object.keys(process.env)`, `Object.entries(process.env)`, `{ ...process.env }`:
  // array-index names ("0" .. "4294967294") first in ascending numeric order, then
  // the others in insertion order (the OS order, then later additions).
  std::vector<std::string> keys();
  std::vector<std::pair<std::string, std::string>> entries();

  // Test hook: the whole view replaced by `variables` (in that order) for the
  // scope, then restored. Nested scopes restore in reverse order.
  class ScopedEnvironment {
  public:
    explicit ScopedEnvironment(std::vector<std::pair<std::string, std::string>> variables);
    ScopedEnvironment(std::initializer_list<std::pair<std::string, std::string>> variables)
      : ScopedEnvironment(std::vector<std::pair<std::string, std::string>>(variables)) {}
    ~ScopedEnvironment();
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    struct Store;

  private:
    std::unique_ptr<Store> saved_;
  };

  // Test hook mirroring the TS tests' save / set-or-delete / restore-in-finally:
  // each name is set (a value) or deleted (std::nullopt) for the scope; the
  // destructor restores the previous value, or deletes a name that was absent.
  class ScopedOverride {
  public:
    explicit ScopedOverride(std::vector<std::pair<std::string, std::optional<std::string>>> updates);
    ScopedOverride(std::initializer_list<std::pair<std::string, std::optional<std::string>>> updates)
      : ScopedOverride(std::vector<std::pair<std::string, std::optional<std::string>>>(updates)) {}
    ~ScopedOverride();
    ScopedOverride(const ScopedOverride&) = delete;
    ScopedOverride& operator=(const ScopedOverride&) = delete;

  private:
    std::vector<std::pair<std::string, std::optional<std::string>>> previous_;
  };
}
