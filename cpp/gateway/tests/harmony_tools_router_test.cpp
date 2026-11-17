// Links: tests/harmony_tools_router_test.cpp src/harmony/tools/router.cpp
// src/harmony/tools/backend.cpp src/harmony/tools/fakes.cpp src/harmony/tools/result.cpp
// src/harmony/tools/schemas.cpp
// Port of all 11 tests in tests/harmony-tools-router.test.ts, plus focused Bun parity cases.
#include "uos/harmony/tools/fakes.hpp"

#include "uos/harmony/tools/result.hpp"
#include "uos/harmony/tools/router.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace backend = uos::harmony::tools::backend;
  using namespace uos::harmony::tools::fakes;
  using uos::harmony::tools::result::ToolExecutionError;
  using uos::harmony::tools::result::ToolResult;
  using uos::harmony::tools::router::RunToolOptions;
  using uos::harmony::tools::router::runTool;

#define AWAIT(name, expr) auto name##Task = (expr); auto name = co_await std::move(name##Task)

  rt::OrderedMap<std::string, std::string> files() {
    return {
      {"src/config.txt", "PORT = 8000\nPORT = 8000\n"},
      {"docs/spec.txt", "Section 1: stable line content\nSection 2: nothing here\n"},
      {"docs/notes.txt", "nothing here either\n"},
    };
  }

  backend::ToolBackends backends(FakeToolBackendsOptions opts = {}) {
    auto merged = files();
    if (opts.files) for (const auto& [path, content] : *opts.files) merged.set(path, content);
    opts.files = std::move(merged);
    return createFakeToolBackends(std::move(opts));
  }

  rt::Task<ToolResult> call(backend::ToolBackends backends_, std::string tool, json::object args, RunToolOptions opts = {}) {
    return runTool(std::move(backends_), std::move(tool), json::value(std::move(args)), std::move(opts));
  }

  void ok(const ToolResult& result) {
    EXPECT_EQ(result.ok, true);
    EXPECT(!result.error_code);
  }

  class InspectWorkspace : public FakeWorkspaceBackend {
  public:
    std::function<std::string(const std::string&)> readHook{};
    std::function<rt::Task<backend::ShellExecResult>(std::string, ExecShellOptions)> shellHook{};

    std::string read(const std::string& rel) override {
      return readHook ? readHook(rel) : FakeWorkspaceBackend::read(rel);
    }
    rt::Task<backend::ShellExecResult> execShell(std::string command, ExecShellOptions opts) override {
      return shellHook ? shellHook(std::move(command), std::move(opts)) : FakeWorkspaceBackend::execShell(std::move(command), std::move(opts));
    }
  };

  // assert.throws(fn, /re/): the RegExp is tested against String(error).
  template <class F> bool throwsMatching(F fn, const char* pattern) {
    try {
      fn();
    } catch (const ToolExecutionError& error) {
      return rt::Regex(pattern).test(error.toString());
    }
    return false;
  }
}

TEST("router: fake workspace write scope is enforced at the backend boundary too") {
  FakeWorkspaceOptions options;
  options.files = files();
  options.writeScope = std::vector<std::string>{"src/**"};
  FakeWorkspaceBackend workspace(std::move(options));
  EXPECT_EQ(workspace.isAllowedWrite("src/config.txt"), true);
  EXPECT_EQ(workspace.isAllowedWrite("docs/spec.txt"), false);
  EXPECT_EQ(workspace.isAllowedWrite("../outside"), false);
  EXPECT(throwsMatching([&] { workspace.write("docs/spec.txt", "x"); }, "write scope violation"));
  EXPECT(throwsMatching([&] { workspace.read("nope.txt"); }, "not found"));
  EXPECT_EQ(workspace.listFiles("docs").size(), std::size_t{2});
}

ASYNC_TEST("router: filesystem.read returns bounded file content verbatim") {
  AWAIT(result, call(backends(), "filesystem.read", json::object{{"path", "docs/spec.txt"}}));
  ok(result);
  EXPECT_EQ(result.output.value_or(""), "Section 1: stable line content\nSection 2: nothing here\n");
}

ASYNC_TEST("router: filesystem.read reports missing files and path escapes machine-readably") {
  AWAIT(missing, call(backends(), "filesystem.read", json::object{{"path", "docs/nope.txt"}}));
  EXPECT_EQ(missing.ok, false);
  EXPECT_EQ(missing.error_code.value_or(""), "not_found");
  EXPECT(rt::Regex("not found").test(missing.error.value_or("")));
  const std::vector<std::string> paths{"../secret.txt", "/etc/passwd", "a/../../b", "../../etc/passwd"};
  for (const auto& path : paths) {
    AWAIT(escaped, call(backends(), "filesystem.read", json::object{{"path", path}}));
    EXPECT_EQ(escaped.ok, false);
    EXPECT_EQ(escaped.error_code.value_or(""), "path_escape");
    EXPECT(rt::Regex("path escapes workspace root").test(escaped.error.value_or("")));
  }
}

ASYNC_TEST("router: filesystem.find matches full paths and basenames") {
  AWAIT(full, call(backends(), "filesystem.find", json::object{{"path", "docs"}, {"pattern", "*.txt"}}));
  ok(full);
  EXPECT_EQ(full.output.value_or(""), "docs/notes.txt\ndocs/spec.txt");
  AWAIT(deep, call(backends(), "filesystem.find", json::object{{"path", "."}, {"pattern", "**/*.txt"}}));
  ok(deep);
  EXPECT_EQ(rt::split(deep.output.value_or(""), "\n"), (std::vector<std::string>{"docs/notes.txt", "docs/spec.txt", "src/config.txt"}));
  AWAIT(none, call(backends(), "filesystem.find", json::object{{"path", "docs"}, {"pattern", "*.md"}}));
  ok(none);
  EXPECT_EQ(none.output.value_or(""), "(no matches)");
  AWAIT(defaultPattern, call(backends(), "filesystem.find", json::object{{"path", "docs"}}));
  ok(defaultPattern);
  EXPECT_EQ(rt::split(defaultPattern.output.value_or(""), "\n"), (std::vector<std::string>{"docs/notes.txt", "docs/spec.txt"}));
}

ASYNC_TEST("router: filesystem.search is case-insensitive, line numbered and bounded") {
  AWAIT(result, call(backends(), "filesystem.search", json::object{{"path", "docs"}, {"query", "SECTION 1"}}));
  ok(result);
  EXPECT_EQ(result.output.value_or(""), "docs/spec.txt:1:Section 1: stable line content");
  AWAIT(empty, call(backends(), "filesystem.search", json::object{{"path", "docs"}, {"query", "gibberish"}}));
  ok(empty);
  EXPECT_EQ(empty.output.value_or(""), "(no matches)");
  AWAIT(escaped, call(backends(), "filesystem.search", json::object{{"path", "../"}, {"query", "x"}}));
  EXPECT_EQ(escaped.ok, false);
  EXPECT_EQ(escaped.error_code.value_or(""), "path_escape");
}

ASYNC_TEST("router: shell.exec returns exit code, stdout, stderr and error codes") {
  auto shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{
    {.command = std::string("echo hi"), .stdout = "hi\n"},
    {.command = std::string("both"), .stdout = "out\n", .stderr = "err\n"},
    {.command = std::string("boom"), .exit_code = 3, .stderr = "boom stderr\n"},
  });
  auto backendsWithShell = createFakeToolBackends(FakeToolBackendsOptions{.files = files(), .shell = shell});
  AWAIT(success, call(backendsWithShell, "shell.exec", json::object{{"command", "echo hi"}}));
  ok(success);
  EXPECT_EQ(success.exit_code.value_or(-1), 0.0);
  EXPECT_EQ(success.stdout.value_or(""), "hi\n");
  EXPECT(success.stderr.has_value());
  EXPECT_EQ(success.stderr.value_or(""), "");
  EXPECT_EQ(success.output.value_or(""), "hi");
  AWAIT(merged, call(backendsWithShell, "shell.exec", json::object{{"command", "both"}}));
  ok(merged);
  EXPECT_EQ(merged.output.value_or(""), "out\nerr");
  EXPECT_EQ(merged.stdout.value_or(""), "out\n");
  EXPECT_EQ(merged.stderr.value_or(""), "err\n");
  AWAIT(failed, call(backendsWithShell, "shell.exec", json::object{{"command", "boom"}}));
  EXPECT_EQ(failed.ok, false);
  EXPECT_EQ(failed.error_code.value_or(""), "exec_failed");
  EXPECT_EQ(failed.exit_code.value_or(-1), 3.0);
  EXPECT_EQ(failed.stderr.value_or(""), "boom stderr\n");
  EXPECT(rt::Regex("boom stderr").test(failed.error.value_or("")));
  AWAIT(unknown, call(backendsWithShell, "shell.exec", json::object{{"command", "nope"}}));
  EXPECT_EQ(unknown.ok, false);
  EXPECT_EQ(unknown.error_code.value_or(""), "exec_failed");
  EXPECT_EQ(unknown.exit_code.value_or(-1), 127.0);
  auto timeoutShell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{{.command = std::string("sleep"), .timed_out = true}});
  auto timeoutBackends = createFakeToolBackends(FakeToolBackendsOptions{.files = files(), .shell = timeoutShell});
  AWAIT(timedOut, call(timeoutBackends, "shell.exec", json::object{{"command", "sleep"}}));
  EXPECT_EQ(timedOut.ok, false);
  EXPECT_EQ(timedOut.error_code.value_or(""), "timeout");
  EXPECT(!timedOut.exit_code);
  EXPECT(rt::Regex("timed out").test(timedOut.error.value_or("")));
}

ASYNC_TEST("router: editor.apply_patch creates, patches, and rejects scope and patch errors") {
  auto withScope = backends(FakeToolBackendsOptions{.writeScope = std::vector<std::string>{"**", "!protected/**"}});
  AWAIT(created, call(withScope, "editor.apply_patch", json::object{{"path", "answer.txt"}, {"add", true}, {"new", "done"}}));
  ok(created);
  EXPECT_EQ(created.output.value_or(""), "created answer.txt");
  AWAIT(readBack, call(withScope, "filesystem.read", json::object{{"path", "answer.txt"}}));
  ok(readBack);
  EXPECT_EQ(readBack.output.value_or(""), "done");
  AWAIT(addExisting, call(withScope, "editor.apply_patch", json::object{{"path", "answer.txt"}, {"add", true}, {"new", "x"}}));
  EXPECT_EQ(addExisting.ok, false);
  EXPECT_EQ(addExisting.error_code.value_or(""), "patch_failed");
  EXPECT(rt::Regex("already exists").test(addExisting.error.value_or("")));
  AWAIT(missing, call(withScope, "editor.apply_patch", json::object{{"path", "nope.txt"}, {"old", "x"}, {"new", "y"}}));
  EXPECT_EQ(missing.ok, false);
  EXPECT_EQ(missing.error_code.value_or(""), "patch_failed");
  EXPECT(rt::Regex("does not exist").test(missing.error.value_or("")));
  // Ambiguous old text (occurring twice) must fail rather than guess.
  AWAIT(ambiguous, call(withScope, "editor.apply_patch", json::object{{"path", "src/config.txt"}, {"old", "PORT = 8000"}, {"new", "PORT = 9000"}}));
  EXPECT_EQ(ambiguous.ok, false);
  EXPECT_EQ(ambiguous.error_code.value_or(""), "patch_failed");
  EXPECT(rt::Regex("more than once").test(ambiguous.error.value_or("")));
  AWAIT(patched, call(withScope, "editor.apply_patch", json::object{{"path", "docs/spec.txt"}, {"old", "Section 1: stable line content"}, {"new", "Section 1: stable"}}));
  ok(patched);
  EXPECT_EQ(patched.output.value_or(""), "patched docs/spec.txt");
  AWAIT(readPatched, call(withScope, "filesystem.read", json::object{{"path", "docs/spec.txt"}}));
  ok(readPatched);
  EXPECT(rt::Regex("^Section 1: stable\n").test(readPatched.output.value_or("")));
  AWAIT(scope, call(withScope, "editor.apply_patch", json::object{{"path", "protected/keep.txt"}, {"old", "a"}, {"new", "b"}}));
  EXPECT_EQ(scope.ok, false);
  EXPECT_EQ(scope.error_code.value_or(""), "write_scope");
  EXPECT(rt::Regex("write scope violation").test(scope.error.value_or("")));
  AWAIT(insert, call(withScope, "editor.apply_patch", json::object{{"path", "src/config.txt"}, {"old", ""}, {"new", "# top\n"}}));
  ok(insert);
  AWAIT(afterInsert, call(withScope, "filesystem.read", json::object{{"path", "src/config.txt"}}));
  ok(afterInsert);
  EXPECT(rt::startsWith(afterInsert.output.value_or(""), "# top\n"));
}

ASYNC_TEST("router: task.update_plan stores the plan and reports its size") {
  AWAIT(result, call(backends(), "task.update_plan", json::object{{"plan", json::array{"read", "edit", "verify"}}}));
  ok(result);
  EXPECT_EQ(result.output.value_or(""), "plan updated (3 items)");
  AWAIT(empty, call(backends(), "task.update_plan", json::object{{"plan", json::array{}}}));
  ok(empty);
  EXPECT_EQ(empty.output.value_or(""), "plan updated (0 items)");
  AWAIT(invalid, call(backends(), "task.update_plan", json::object{{"plan", json::array{"ok", ""}}}));
  EXPECT_EQ(invalid.ok, false);
  EXPECT_EQ(invalid.error_code.value_or(""), "invalid_args");
  EXPECT(rt::Regex("invalid arguments").test(invalid.error.value_or("")));
}

ASYNC_TEST("router: browser tools use deterministic fake backends with current-page state") {
  FakeToolBackendsOptions options;
  options.files = files();
  options.browserPages = rt::OrderedMap<std::string, backend::BrowserPage>{
    {"https://docs.example.com/guide", {.url = "https://docs.example.com/guide", .title = "Harmony Guide", .content = "Tools are called here.\nThe final answer follows.\n"}},
  };
  options.browserSearch = std::vector<FakeBrowserSearchEntry>{
    {.query = "harmony", .results = {{.title = "Harmony Guide", .url = "https://docs.example.com/guide", .snippet = "Tools are called here."}}},
  };
  options.browserSearchFallback = std::vector<backend::BrowserSearchResult>{{.title = "Anything", .url = "https://example.com/any", .snippet = "fallback result."}};
  auto backendsWithBrowser = createFakeToolBackends(std::move(options));
  AWAIT(search, call(backendsWithBrowser, "browser.search", json::object{{"query", "HARMONY"}}));
  ok(search);
  EXPECT_EQ(search.output.value_or(""), "- Harmony Guide\n  https://docs.example.com/guide\n  Tools are called here.");
  AWAIT(fallback, call(backendsWithBrowser, "browser.search", json::object{{"query", "unknown-query"}}));
  ok(fallback);
  EXPECT(rt::Regex("fallback result").test(fallback.output.value_or("")));
  auto emptyBrowser = createFakeToolBackends(FakeToolBackendsOptions{.files = files(), .browserSearchFallback = std::vector<backend::BrowserSearchResult>{}});
  AWAIT(noResults, call(emptyBrowser, "browser.search", json::object{{"query", "something"}}));
  ok(noResults);
  EXPECT_EQ(noResults.output.value_or(""), "(no results)");
  // find before any open is a machine-readable invalid state.
  AWAIT(findFirst, call(backendsWithBrowser, "browser.find", json::object{{"query", "tool"}}));
  EXPECT_EQ(findFirst.ok, false);
  EXPECT_EQ(findFirst.error_code.value_or(""), "invalid_args");
  AWAIT(opened, call(backendsWithBrowser, "browser.open", json::object{{"url", "https://docs.example.com/guide"}}));
  ok(opened);
  EXPECT(rt::startsWith(opened.output.value_or(""), "Harmony Guide\nhttps://docs.example.com/guide\n\n"));
  AWAIT(found, call(backendsWithBrowser, "browser.find", json::object{{"query", "FINAL"}}));
  ok(found);
  EXPECT_EQ(found.output.value_or(""), "2:The final answer follows.");
  AWAIT(notFound, call(backendsWithBrowser, "browser.open", json::object{{"url", "https://example.com/missing"}}));
  EXPECT_EQ(notFound.ok, false);
  EXPECT_EQ(notFound.error_code.value_or(""), "not_found");
}

ASYNC_TEST("router: unavailable backends and invalid calls return bounded envelopes") {
  auto layered = backends();
  backend::ToolBackends withoutBrowser{.workspace = layered.workspace, .plan = layered.plan};
  const std::vector<std::string> tools{"browser.search", "browser.open", "browser.find"};
  for (const auto& tool : tools) {
    json::object args = tool == "browser.open" ? json::object{{"url", "x"}} : json::object{{"query", "x"}};
    AWAIT(result, call(withoutBrowser, tool, std::move(args)));
    EXPECT_EQ(result.ok, false);
    EXPECT_EQ(result.error_code.value_or(""), "unavailable");
  }
  AWAIT(invalid, call(layered, "filesystem.read", json::object{{"path", 42}}));
  EXPECT_EQ(invalid.ok, false);
  EXPECT_EQ(invalid.error_code.value_or(""), "invalid_args");
  AWAIT(unknown, call(layered, "does.not.exist", json::object{}));
  EXPECT_EQ(unknown.ok, false);
  EXPECT_EQ(unknown.error_code.value_or(""), "invalid_args");
}

ASYNC_TEST("router: tool outputs are clipped to the configured output limit") {
  auto large = createFakeToolBackends(FakeToolBackendsOptions{.files = rt::OrderedMap<std::string, std::string>{{"big.txt", std::string(10'000, 'x')}}});
  AWAIT(result, call(large, "filesystem.read", json::object{{"path", "big.txt"}}));
  ok(result);
  EXPECT_EQ(rt::length(result.output.value_or("")), 8000 + rt::length("…[truncated]"));
  EXPECT(rt::Regex("\\[truncated\\]$").test(result.output.value_or("")));
}

// Bun 1.4.0 snapshots: /tmp/claude-1002/wave/w06c5/adoption-20261116T1955/bun-edge.log.
ASYNC_TEST("router: inherited tool names reject outside the backend catch") {
  const std::vector<std::string> inherited{
    "toString", "toLocaleString", "valueOf", "hasOwnProperty", "propertyIsEnumerable", "isPrototypeOf",
    "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__", "__proto__", "constructor",
  };
  for (const auto& tool : inherited) {
    auto task = runTool(backends(), tool, json::value(json::object{}));
    bool rejected = false;
    try {
      const auto result = co_await std::move(task);
      (void)result;
    } catch (const rt::TypeError& error) {
      rejected = true;
      EXPECT_EQ(error.message, "undefined is not an object (evaluating 'toolSchema.parameters.properties')");
    }
    EXPECT(rejected);
  }
  AWAIT(nullArguments, runTool(backends(), std::string("constructor"), json::value(nullptr)));
  EXPECT_EQ(nullArguments.error.value_or(""), "invalid arguments: arguments must be an object");
  EXPECT_EQ(nullArguments.error_code.value_or(""), "invalid_args");
  AWAIT(undefinedArguments, runTool(backends(), std::string("filesystem.read"), static_cast<const json::value*>(nullptr)));
  EXPECT_EQ(undefinedArguments.error.value_or(""), "invalid arguments: arguments must be an object");
  EXPECT_EQ(undefinedArguments.error_code.value_or(""), "invalid_args");
  RunToolOptions options{.outputLimit = 1};
  json::value arguments = json::object{{"path", "docs/spec.txt"}};
  AWAIT(pointerArguments, runTool(backends(), std::string("filesystem.read"), &arguments, options));
  EXPECT_EQ(pointerArguments.output.value_or(""), "S…[truncated]");
}

ASYNC_TEST("router: find and patch path failures and missing plan preserve envelopes") {
  const std::vector<std::string> tools{"filesystem.find", "editor.apply_patch"};
  for (const auto& tool : tools) {
    AWAIT(escaped, call(backends(), tool, json::object{{"path", "../outside"}}));
    EXPECT_EQ(escaped.error.value_or(""), "path escapes workspace root: ../outside");
    EXPECT_EQ(escaped.error_code.value_or(""), "path_escape");
  }
  auto withoutPlan = backends();
  withoutPlan.plan.reset();
  AWAIT(unavailable, call(withoutPlan, "task.update_plan", json::object{{"plan", json::array{}}}));
  EXPECT_EQ(unavailable.error.value_or(""), "task.update_plan is not available in this configuration");
  EXPECT_EQ(unavailable.error_code.value_or(""), "unavailable");
}

ASYNC_TEST("router: backend Error and thrown-value formatting matches Bun") {
  const std::vector<rt::Reason> failures{
    rt::makeReason(ToolExecutionError("patch_failed", "")), rt::makeReason(rt::JsError("backend busted")), rt::makeReason(rt::TypeError("")),
    rt::thrown(json::value("boom")), rt::thrown(json::value(42)), rt::thrown(json::value(nullptr)), rt::thrown(std::nullopt),
    rt::thrown(json::value(json::object{})), rt::thrown(json::value(json::array{})),
  };
  const std::vector<std::string> expected{
    "", "internal error: backend busted", "internal error: TypeError", "internal error: boom", "internal error: 42",
    "internal error: null", "internal error: undefined", "internal error: [object Object]", "internal error: ",
  };
  for (std::size_t index = 0; index < failures.size(); ++index) {
    auto workspace = std::make_shared<InspectWorkspace>();
    workspace->readHook = [failure = failures[index]](const std::string&) -> std::string { rt::rethrow(failure); };
    backend::ToolBackends injected{.workspace = workspace};
    AWAIT(result, call(injected, "filesystem.read", json::object{{"path", "x"}}, RunToolOptions{.outputLimit = 1}));
    EXPECT_EQ(result.ok, false);
    EXPECT_EQ(result.error.value_or(""), expected[index]);
    EXPECT_EQ(result.error_code.value_or(""), index == 0 ? "patch_failed" : "internal");
    EXPECT(!result.output);
    EXPECT(!result.exit_code);
  }
}

ASYNC_TEST("router: search limits and Unicode casing preserve source comparisons") {
  auto workspace = createFakeToolBackends(FakeToolBackendsOptions{.files = rt::OrderedMap<std::string, std::string>{
    {"one.txt", "ÉCHO\r\nİstanbul\necho\nEcho\n"}, {"two.txt", "echo"},
  }});
  const std::vector<double> limits{0, -1, 1, 1.5, rt::Number::NaN, rt::Number::POSITIVE_INFINITY};
  const std::vector<std::string> expected{
    "(no matches)", "(no matches)", "one.txt:3:echo", "one.txt:3:echo\none.txt:4:Echo",
    "one.txt:3:echo\none.txt:4:Echo\ntwo.txt:1:echo", "one.txt:3:echo\none.txt:4:Echo\ntwo.txt:1:echo",
  };
  for (std::size_t index = 0; index < limits.size(); ++index) {
    RunToolOptions options{.outputLimit = 1, .searchLineLimit = limits[index]};
    AWAIT(result, call(workspace, "filesystem.search", json::object{{"path", "."}, {"query", "eCHO"}}, options));
    ok(result);
    EXPECT_EQ(result.output.value_or(""), expected[index]);
  }
  AWAIT(unicode, call(workspace, "filesystem.search", json::object{{"path", "."}, {"query", "i̇STANBUL"}}));
  EXPECT_EQ(unicode.output.value_or(""), "one.txt:2:İstanbul");
  auto many = createFakeToolBackends(FakeToolBackendsOptions{.files = rt::OrderedMap<std::string, std::string>{{"x", rt::repeat("match\n", 205)}}});
  AWAIT(bounded, call(many, "filesystem.search", json::object{{"path", "."}, {"query", "match"}}));
  const auto lines = rt::split(bounded.output.value_or(""), "\n");
  EXPECT_EQ(lines.size(), std::size_t{200});
  EXPECT_EQ(lines.front(), "x:1:match");
  EXPECT_EQ(lines.back(), "x:200:match");
}

ASYNC_TEST("router: output clipping uses UTF-16 and numeric slice semantics") {
  auto workspace = createFakeToolBackends(FakeToolBackendsOptions{.files = rt::OrderedMap<std::string, std::string>{{"x", "A😀Z"}}});
  const std::vector<double> limits{0, 2, 2.5, -1, rt::Number::NaN, rt::Number::POSITIVE_INFINITY};
  const std::string splitPair = rt::concat("A", rt::fromCharCode(0xd83d)) + "…[truncated]";
  const std::vector<std::string> expected{"…[truncated]", splitPair, splitPair, "A😀…[truncated]", "A😀Z", "A😀Z"};
  for (std::size_t index = 0; index < limits.size(); ++index) {
    AWAIT(result, call(workspace, "filesystem.read", json::object{{"path", "x"}}, RunToolOptions{.outputLimit = limits[index]}));
    ok(result);
    EXPECT_EQ(result.output.value_or(""), expected[index]);
  }
}

ASYNC_TEST("router: find outputs ignore output and line limits") {
  auto workspace = createFakeToolBackends(FakeToolBackendsOptions{.files = rt::OrderedMap<std::string, std::string>{{"one.txt", "x"}, {"two.txt", "x"}}});
  RunToolOptions options{.outputLimit = 1, .searchLineLimit = 0};
  AWAIT(found, call(workspace, "filesystem.find", json::object{{"path", "."}}, options));
  EXPECT_EQ(found.output.value_or(""), "one.txt\ntwo.txt");
  AWAIT(opened, call(workspace, "browser.open", json::object{{"url", "https://example.com/index.html"}}, options));
  EXPECT_EQ(opened.output.value_or(""), "E…[truncated]");
  AWAIT(browserFound, call(workspace, "browser.find", json::object{{"query", "page"}}, options));
  EXPECT_EQ(browserFound.output.value_or(""), "2:Find content on this page.");
  AWAIT(browserEmpty, call(workspace, "browser.find", json::object{{"query", "missing"}}, options));
  EXPECT_EQ(browserEmpty.output.value_or(""), "(no matches)");
  AWAIT(search, call(workspace, "browser.search", json::object{{"query", "example"}}, options));
  EXPECT_EQ(search.output.value_or(""), "-…[truncated]");
}

ASYNC_TEST("router: shell clips raw streams separately and preserves timeout text") {
  auto shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{
    {.command = std::string("merge"), .stdout = "  out \t\n", .stderr = "err \n"},
    {.command = std::string("empty-failure"), .exit_code = 3, .stdout = "  \n", .stderr = "\t\n"},
    {.command = std::string("timeout"), .timed_out = true},
  });
  auto workspace = createFakeToolBackends(FakeToolBackendsOptions{.shell = shell});
  AWAIT(merged, call(workspace, "shell.exec", json::object{{"command", "merge"}}, RunToolOptions{.outputLimit = 4}));
  EXPECT_EQ(merged.output.value_or(""), "  ou…[truncated]");
  EXPECT_EQ(merged.stdout.value_or(""), "  ou…[truncated]");
  EXPECT_EQ(merged.stderr.value_or(""), "err …[truncated]");
  EXPECT_EQ(merged.exit_code.value_or(-1), 0.0);
  AWAIT(empty, call(workspace, "shell.exec", json::object{{"command", "empty-failure"}}, RunToolOptions{.outputLimit = 4}));
  EXPECT_EQ(empty.error.value_or(""), "exit…[truncated]");
  EXPECT_EQ(empty.error_code.value_or(""), "exec_failed");
  EXPECT_EQ(empty.stdout.value_or(""), "  \n");
  EXPECT_EQ(empty.stderr.value_or(""), "\t\n");
  EXPECT_EQ(empty.exit_code.value_or(-1), 3.0);
  AWAIT(timeout, call(workspace, "shell.exec", json::object{{"command", "timeout"}}, RunToolOptions{.shellTimeoutMs = 0, .outputLimit = 1}));
  EXPECT_EQ(timeout.error.value_or(""), "command timed out after 0ms");
  EXPECT_EQ(timeout.error_code.value_or(""), "timeout");
  EXPECT(!timeout.output);
  EXPECT(!timeout.exit_code);
  EXPECT(!timeout.stdout);
  EXPECT(!timeout.stderr);
}

ASYNC_TEST("router: shell forwards options and preserves Bun microtask order") {
  auto events = std::make_shared<std::vector<std::string>>();
  auto workspace = std::make_shared<InspectWorkspace>();
  workspace->shellHook = [events](std::string command, backend::WorkspaceBackend::ExecShellOptions opts) {
    events->push_back("backend:" + command + ":" + rt::numberToString(opts.timeoutMs) + ":" + (opts.signal && opts.signal->aborted() ? "true" : "false"));
    return rt::fulfilledTask(backend::ShellExecResult{.exit_code = 0, .stdout = "x\n", .stderr = "", .timed_out = false});
  };
  backend::ToolBackends injected{.workspace = workspace};
  rt::AbortController controller;
  controller.abort();
  RunToolOptions options{.signal = controller.signal(), .shellTimeoutMs = 0};
  auto task = call(injected, "shell.exec", json::object{{"command", "trace"}}, options);
  rt::Promise<ToolResult> pending(std::move(task));
  auto done = pending.then([events](const ToolResult&) { events->push_back("result"); });
  events->push_back("called");
  rt::queueMicrotask([events] {
    events->push_back("microtask-1");
    rt::queueMicrotask([events] {
      events->push_back("microtask-2");
      rt::queueMicrotask([events] { events->push_back("microtask-3"); });
    });
  });
  co_await done.wait();
  EXPECT_EQ(*events, (std::vector<std::string>{"backend:trace:0:true", "called", "microtask-1", "microtask-2", "result", "microtask-3"}));
}

// Bun 1.4.0 snapshots: /tmp/claude-1002/wave/w06c5/adoption-20261116T1955/bun-coercion.log.
ASYNC_TEST("router: thrown-value string conversion can itself reject") {
  const std::vector<json::value> unstringifiable{
    json::object{{"toString", "x"}}, json::object{{"toString", nullptr}}, json::object{{"toString", json::object{}}}, json::array{json::object{{"toString", 0}}},
  };
  for (const auto& value : unstringifiable) {
    auto workspace = std::make_shared<InspectWorkspace>();
    workspace->readHook = [value](const std::string&) -> std::string { throw rt::ThrownValue(value); };
    backend::ToolBackends injected{.workspace = workspace};
    auto task = call(injected, "filesystem.read", json::object{{"path", "x"}});
    bool rejected = false;
    try {
      const auto result = co_await std::move(task);
      (void)result;
    } catch (const rt::TypeError& error) {
      rejected = true;
      EXPECT_EQ(error.message, "No default value");
    }
    EXPECT(rejected);
  }
  const std::vector<json::value> ordinary{json::object{{"valueOf", "x"}}, json::object{{"value", json::object{{"toString", nullptr}}}}};
  for (const auto& value : ordinary) {
    auto workspace = std::make_shared<InspectWorkspace>();
    workspace->readHook = [value](const std::string&) -> std::string { throw rt::ThrownValue(value); };
    backend::ToolBackends injected{.workspace = workspace};
    AWAIT(result, call(injected, "filesystem.read", json::object{{"path", "x"}}));
    EXPECT_EQ(result.error.value_or(""), "internal error: [object Object]");
    EXPECT_EQ(result.error_code.value_or(""), "internal");
  }
}

ASYNC_TEST("router: rejected shell backend becomes an internal envelope") {
  auto workspace = std::make_shared<InspectWorkspace>();
  workspace->shellHook = [](std::string, backend::WorkspaceBackend::ExecShellOptions) {
    return rt::Promise<backend::ShellExecResult>::reject(rt::makeReason(rt::JsError("async shell failure"))).wait();
  };
  backend::ToolBackends injected{.workspace = workspace};
  AWAIT(result, call(injected, "shell.exec", json::object{{"command", "x"}}));
  EXPECT_EQ(result.error.value_or(""), "internal error: async shell failure");
  EXPECT_EQ(result.error_code.value_or(""), "internal");
  EXPECT(!result.exit_code);
}
