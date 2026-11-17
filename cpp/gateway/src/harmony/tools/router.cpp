#include "uos/harmony/tools/router.hpp"

#include "uos/harmony/tools/schemas.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <exception>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace uos::harmony::tools::router {
  namespace {
    using backend::ToolBackends;
    using backend::WorkspaceBackend;
    using result::clipToolText;
    using result::toolFailure;
    using result::ToolResult;

    ToolResult pathError(const std::string& rel) { return toolFailure("path_escape", "path escapes workspace root: " + rel); }

    std::optional<std::string> requiredPath(const json::object& args) {
      return backend::normalizeWorkspacePath(rt::toString(rt::get(args, "path")));
    }

    ToolResult readFile(WorkspaceBackend& workspace, const json::object& args, double outputLimit) {
      const auto rel = requiredPath(args);
      if (!rel) return pathError(rt::toString(rt::get(args, "path")));
      return {.ok = true, .output = clipToolText(workspace.read(*rel), outputLimit)};
    }

    ToolResult findFiles(WorkspaceBackend& workspace, const json::object& args) {
      const auto rel = requiredPath(args);
      if (!rel) return pathError(rt::toString(rt::get(args, "path")));
      // Validation (schemas.ts) guarantees `pattern` is a non-empty string when
      // present, so the absent case is the only one the default applies to.
      const std::string pattern(rt::getString(args, "pattern").value_or("**"));
      std::vector<std::string> files;
      for (const auto& path : workspace.listFiles(*rel)) {
        const auto segments = rt::split(path, "/");
        const std::string& base = segments.back();
        if (backend::globMatch(pattern, path) || (!rt::includes(pattern, "/") && backend::globMatch(pattern, base))) files.push_back(path);
      }
      return {.ok = true, .output = files.empty() ? "(no matches)" : rt::join(files, "\n")};
    }

    ToolResult searchFiles(WorkspaceBackend& workspace, const json::object& args, double lineLimit) {
      const auto rel = requiredPath(args);
      if (!rel) return pathError(rt::toString(rt::get(args, "path")));
      const std::string query = rt::toLowerCase(rt::toString(rt::get(args, "query")));
      std::vector<std::string> lines;
      for (const auto& file : workspace.listFiles(*rel)) {
        if (static_cast<double>(lines.size()) >= lineLimit) break;
        const std::string content = workspace.read(file);
        const auto split = rt::split(content, "\n");
        for (std::size_t index = 0; index < split.size(); ++index) {
          if (static_cast<double>(lines.size()) >= lineLimit) break;
          if (rt::includes(rt::toLowerCase(split[index]), query)) {
            lines.push_back(file + ":" + rt::numberToString(static_cast<double>(index + 1)) + ":" + split[index]);
          }
        }
      }
      return {.ok = true, .output = lines.empty() ? "(no matches)" : rt::join(lines, "\n")};
    }

    rt::Task<ToolResult> execShell(std::shared_ptr<WorkspaceBackend> workspace, json::object args, RunToolOptions opts) {
      const std::string command = rt::toString(rt::get(args, "command"));
      const double timeoutMs = opts.shellTimeoutMs.value_or(result::SHELL_DEFAULT_TIMEOUT_MS);
      const double outputLimit = opts.outputLimit.value_or(result::TOOL_OUTPUT_LIMIT);
      WorkspaceBackend::ExecShellOptions shellOptions{.timeoutMs = timeoutMs, .signal = opts.signal};
      auto task = workspace->execShell(command, shellOptions);
      rt::Promise<backend::ShellExecResult> pending(std::move(task));
      const auto result = co_await pending.wait();
      if (result.timed_out) co_return toolFailure("timeout", "command timed out after " + rt::numberToString(timeoutMs) + "ms");
      const std::string stdoutText = clipToolText(result.stdout, outputLimit);
      const std::string stderrText = clipToolText(result.stderr, outputLimit);
      // Model-facing output: trimmed non-empty lines joined (raw streams stay in
      // stdout/stderr fields).
      std::vector<std::string> streams;
      for (const auto* text : {&result.stdout, &result.stderr}) {
        std::string trimmed = rt::trimEnd(*text);
        if (!trimmed.empty()) streams.push_back(std::move(trimmed));
      }
      const std::string merged = rt::join(streams, "\n");
      if (result.exit_code != 0) {
        co_return ToolResult{
          .ok = false,
          .error = clipToolText(merged.empty() ? "exit code " + rt::numberToString(result.exit_code) : merged, outputLimit),
          .error_code = "exec_failed",
          .exit_code = result.exit_code,
          .stdout = stdoutText,
          .stderr = stderrText,
        };
      }
      co_return ToolResult{.ok = true, .output = clipToolText(merged, outputLimit), .exit_code = result.exit_code, .stdout = stdoutText, .stderr = stderrText};
    }

    ToolResult applyPatch(WorkspaceBackend& workspace, const json::object& args) {
      const auto rel = requiredPath(args);
      if (!rel) return pathError(rt::toString(rt::get(args, "path")));
      if (!workspace.isAllowedWrite(*rel)) {
        return toolFailure("write_scope", "write scope violation: " + *rel + " is not writable (scope: " + workspace.describeWriteScope() + ")");
      }
      const bool add = rt::getBool(args, "add") == true;
      const std::string old(rt::getString(args, "old").value_or(""));
      const std::string next(rt::getString(args, "new").value_or(""));
      const WorkspaceBackend::Patch patch{.old = old, .new_ = next, .add = add};
      const auto applied = workspace.applyPatch(*rel, patch);
      return {.ok = true, .output = applied.detail};
    }

    ToolResult updatePlan(const ToolBackends& backends, const json::object& args) {
      const auto& planBackend = backends.plan;
      if (!planBackend) return toolFailure("unavailable", "task.update_plan is not available in this configuration");
      std::vector<std::string> plan;
      for (const auto& item : *rt::getArray(args, "plan")) plan.emplace_back(item.as_string());
      const auto stored = planBackend->update(plan);
      return {.ok = true, .output = "plan updated (" + rt::numberToString(stored.items) + " items)"};
    }

    ToolResult browserSearch(const ToolBackends& backends, const json::object& args, double outputLimit) {
      const auto& browser = backends.browser;
      if (!browser) return toolFailure("unavailable", "browser.search is not available in this configuration");
      const auto results = browser->search(rt::toString(rt::get(args, "query")));
      if (results.empty()) return {.ok = true, .output = "(no results)"};
      std::vector<std::string> blocks;
      for (const auto& result : results) blocks.push_back("- " + result.title + "\n  " + result.url + "\n  " + result.snippet);
      return {.ok = true, .output = clipToolText(rt::join(blocks, "\n"), outputLimit)};
    }

    ToolResult browserOpen(const ToolBackends& backends, const json::object& args, double outputLimit) {
      const auto& browser = backends.browser;
      if (!browser) return toolFailure("unavailable", "browser.open is not available in this configuration");
      const auto page = browser->open(rt::toString(rt::get(args, "url")));
      const std::string text = page.title + "\n" + page.url + "\n\n" + page.content;
      return {.ok = true, .output = clipToolText(text, outputLimit)};
    }

    ToolResult browserFind(const ToolBackends& backends, const json::object& args) {
      const auto& browser = backends.browser;
      if (!browser) return toolFailure("unavailable", "browser.find is not available in this configuration");
      const auto found = browser->findOnCurrentPage(rt::toString(rt::get(args, "query")));
      if (found.matches.empty()) return {.ok = true, .output = "(no matches)"};
      std::vector<std::string> lines;
      for (const auto& match : found.matches) lines.push_back(rt::numberToString(match.line) + ":" + match.text);
      return {.ok = true, .output = rt::join(lines, "\n")};
    }

    // String(value) skips a non-callable own toString; inherited valueOf returns the object.
    std::string thrownValueString(const json::value& value) {
      std::vector<const json::value*> pending{&value};
      while (!pending.empty()) {
        const auto* next = pending.back();
        pending.pop_back();
        if (next->is_object() && rt::hasOwn(next->get_object(), "toString")) throw rt::TypeError("No default value");
        if (next->is_array()) for (const auto& item : next->get_array()) pending.push_back(&item);
      }
      return rt::toString(value);
    }

    rt::Task<ToolResult> runToolImpl(ToolBackends backends, std::string tool, std::optional<json::value> args, RunToolOptions opts) {
      const double outputLimit = opts.outputLimit.value_or(result::TOOL_OUTPUT_LIMIT);
      const auto validated = schemas::validateToolArguments(tool, args ? &*args : nullptr);
      if (const auto* invalid = std::get_if<schemas::ValidateToolArgumentsResultInvalid>(&validated)) {
        co_return toolFailure("invalid_args", "invalid arguments: " + invalid->reason);
      }
      const auto& arguments = std::get<schemas::ValidateToolArgumentsResultValid>(validated).arguments;

      ToolResult failure;
      try {
        if (tool == "filesystem.read") co_return readFile(*backends.workspace, arguments, outputLimit);
        if (tool == "filesystem.find") co_return findFiles(*backends.workspace, arguments);
        if (tool == "filesystem.search") co_return searchFiles(*backends.workspace, arguments, opts.searchLineLimit.value_or(result::SEARCH_LINE_LIMIT));
        if (tool == "shell.exec") {
          auto task = execShell(backends.workspace, arguments, opts);
          rt::Promise<ToolResult> pending(std::move(task));
          auto result = co_await pending.wait();
          co_return result;
        }
        if (tool == "editor.apply_patch") co_return applyPatch(*backends.workspace, arguments);
        if (tool == "task.update_plan") co_return updatePlan(backends, arguments);
        if (tool == "browser.search") co_return browserSearch(backends, arguments, outputLimit);
        if (tool == "browser.open") co_return browserOpen(backends, arguments, outputLimit);
        if (tool == "browser.find") co_return browserFind(backends, arguments);
        co_return toolFailure("invalid_args", "unknown tool " + rt::jsonQuote(tool));
      } catch (const result::ToolExecutionError& error) {
        failure = toolFailure(error.code, error.message);
      } catch (...) {
        const rt::Reason error = std::current_exception();
        const auto errorMessage = rt::errorMessage(error);
        const auto thrown = rt::thrownValue(error);
        const std::string message = rt::isError(error) && errorMessage && !errorMessage->empty()
                                      ? *errorMessage
                                      : thrown ? thrownValueString(*thrown) : rt::reasonToString(error);
        failure = toolFailure("internal", "internal error: " + message);
      }
      co_return failure;
    }
  }

  rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, json::value args) {
    return runToolImpl(std::move(backends), std::move(tool), std::move(args), RunToolOptions{});
  }

  rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, json::value args, RunToolOptions opts) {
    return runToolImpl(std::move(backends), std::move(tool), std::move(args), std::move(opts));
  }

  rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, const json::value* args) {
    return runToolImpl(std::move(backends), std::move(tool), args ? std::optional<json::value>(*args) : std::nullopt, RunToolOptions{});
  }

  rt::Task<result::ToolResult> runTool(backend::ToolBackends backends, std::string tool, const json::value* args, RunToolOptions opts) {
    return runToolImpl(std::move(backends), std::move(tool), args ? std::optional<json::value>(*args) : std::nullopt, std::move(opts));
  }
}
