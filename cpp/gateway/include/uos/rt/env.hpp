#pragma once
// `process.env` / `Bun.env` (the same object in Bun) and node:process
// `loadEnvFile`. The JS view is a copy of the OS environment taken at first
// use; writes and deletes change only the view (as in Bun they never reach
// libc's environ, so a child spawned without an `env` option still gets the
// original variables). Single-threaded, like the JS it replaces.
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

  // `process.loadEnvFile(path)` (default ".env", against the current directory):
  // parses the file with Bun's .env rules (see parseEnvFile) and adds each
  // variable that process.env does not already have; existing values win.
  // Errors are the readFileSync ones (rt::fs::ErrnoError: ENOENT "open" with the
  // path, EISDIR "read", an empty path names no path).
  void loadEnvFile(std::string_view path = ".env");
  // Bun's .env parser on its own: the assignments in order of first appearance,
  // a later duplicate replacing the value. Lines are `KEY=value`, `KEY: value`
  // (a colon needs one whitespace after it) or `export KEY=value`; keys use
  // [A-Za-z0-9_.-]. Unquoted values end at '#', CR or LF and are trimmed of
  // spaces, tabs, VT and FF. "...", '...' and `...` values may span lines (CRLF
  // and a lone CR become LF); in double quotes only \n and \r are escapes (other
  // backslash pairs stay as written); the rest of the line after a closing quote
  // is ignored; a quote that never closes leaves the raw line text. No $
  // expansion. A leading UTF-8 BOM is skipped.
  std::vector<std::pair<std::string, std::string>> parseEnvFile(std::string_view contents);

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
