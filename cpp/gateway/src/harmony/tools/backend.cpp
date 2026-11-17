#include "uos/harmony/tools/backend.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

namespace uos::harmony::tools::backend {
  std::optional<std::string> normalizeWorkspacePath(std::string_view rel) {
    if (rel.empty() || rt::startsWith(rel, "/")) return std::nullopt;
    std::vector<std::string> parts;
    for (const std::string& segment : rt::split(rel, "/")) {
      if (segment.empty() || segment == ".") continue;
      if (segment == "..") return std::nullopt;
      parts.push_back(segment);
    }
    std::string joined;
    for (std::size_t index = 0; index < parts.size(); ++index) {
      if (index) joined += "/";
      rt::append(joined, parts[index]);
    }
    return joined;
  }

  namespace {
    rt::Regex patternToRegExp(std::string_view pattern) {
      static const rt::Regex SPECIAL("[.+^${}()|[\\]\\\\]", "g");
      // `pattern[i]` reads UTF-16 code units.
      const std::u16string units = rt::toUtf16(pattern);
      const auto unit = [&units](std::size_t index) -> std::optional<char16_t> {
        return index < units.size() ? std::optional<char16_t>(units[index]) : std::nullopt;
      };
      std::string re = "^";
      std::size_t i = 0;
      while (i < units.size()) {
        const char16_t c = units[i];
        if (c == u'*') {
          if (unit(i + 1) == u'*') {
            // `**` matches across slashes; collapse `**/` or trailing `**`.
            if (unit(i + 2) == u'/') {
              re += "(?:.*/)?";
              i += 3;
            } else {
              re += ".*";
              i += 2;
            }
          } else {
            re += "[^/]*";
            i += 1;
          }
        } else if (c == u'?') {
          re += "[^/]";
          i += 1;
        } else if (c == u'/') {
          re += "/";
          i += 1;
        } else {
          rt::append(re, rt::replace(rt::fromUtf16(std::u16string(1, c)), SPECIAL, "\\$&"));
          i += 1;
        }
      }
      re += "$";
      return rt::Regex(re);
    }
  }

  bool globMatch(std::string_view pattern, std::string_view path) { return patternToRegExp(pattern).test(path); }

  namespace {
    // The member's JS type for messages.
    std::string describe(const json::value* value) {
      if (!value) return "undefined";
      if (value->is_null()) return "null";
      if (value->is_array()) return "array";
      return std::string(rt::typeOf(*value));
    }
    [[noreturn]] void fail(std::string_view key, const json::value* member, std::string_view wanted) {
      throw rt::TypeError("ShellExecResult." + std::string(key) + " is " + describe(member) + ", not " + std::string(wanted));
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ShellExecResult& value) {
    json::object object;
    object["exit_code"] = rt::makeNumber(value.exit_code);
    object["stdout"] = json::string(value.stdout);
    object["stderr"] = json::string(value.stderr);
    object["timed_out"] = value.timed_out;
    out = std::move(object);
  }

  ShellExecResult tag_invoke(const json::value_to_tag<ShellExecResult>&, const json::value& value) {
    const json::object* object = value.if_object();
    if (!object) throw rt::TypeError("ShellExecResult is " + describe(&value) + ", not object");
    const json::value* exitCode = rt::get(*object, "exit_code");
    const json::value* stdoutText = rt::get(*object, "stdout");
    const json::value* stderrText = rt::get(*object, "stderr");
    const json::value* timedOut = rt::get(*object, "timed_out");
    if (!rt::getNumber(exitCode)) fail("exit_code", exitCode, "number");
    if (!rt::getString(stdoutText)) fail("stdout", stdoutText, "string");
    if (!rt::getString(stderrText)) fail("stderr", stderrText, "string");
    if (!rt::getBool(timedOut)) fail("timed_out", timedOut, "boolean");
    return {
      .exit_code = *rt::getNumber(exitCode),
      .stdout = std::string(*rt::getString(stdoutText)),
      .stderr = std::string(*rt::getString(stderrText)),
      .timed_out = *rt::getBool(timedOut),
    };
  }
}
