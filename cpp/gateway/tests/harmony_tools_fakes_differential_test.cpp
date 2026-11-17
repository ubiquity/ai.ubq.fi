// Differential cases for src/harmony/tools/fakes.ts. Its bun tests (tests/harmony-tools-router.test.ts,
// tests/reliability-harness.test.ts) need the unported router and reliability harness, so the fakes
// are replayed here on recorded scenarios: scripted shells (RegExp entries with g/y/i/u flags,
// fallbacks, JSON-quoted unknown commands), workspaces (code-unit sorted listings with non-BMP and
// private-use paths, negated write scopes, patches that split surrogate pairs), browsers (Unicode
// case mapping, CRLF pages), plans, and createFakeToolBackends defaults. Expected values were
// recorded from the TypeScript module under Bun 1.4.0 (tests/harmony_tools_fakes_differential_vectors.inc).
#include "uos/harmony/tools/fakes.hpp"

#include "uos/harmony/tools/result.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "harmony_tools_fakes_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::tools::fakes;
  using uos::harmony::tools::backend::BrowserBackend;
  using uos::harmony::tools::backend::BrowserPage;
  using uos::harmony::tools::backend::BrowserSearchResult;
  using uos::harmony::tools::backend::PlanBackend;
  using uos::harmony::tools::backend::ShellExecResult;
  using uos::harmony::tools::backend::ToolBackends;
  using uos::harmony::tools::backend::WorkspaceBackend;
  using uos::harmony::tools::result::ToolExecutionError;

  std::string text(const json::value& value) { return std::string(value.as_string()); }

  std::vector<std::string> strings(const json::value& value) {
    std::vector<std::string> out;
    for (const json::value& item : value.as_array()) out.push_back(text(item));
    return out;
  }

  std::optional<std::string> optionalText(const json::object& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    return value != nullptr ? std::optional<std::string>(text(*value)) : std::nullopt;
  }

  std::shared_ptr<FakeShell> shellOf(const json::object& options) {
    const json::object* spec = rt::getObject(options, "shell");
    if (spec == nullptr) return nullptr;
    std::vector<FakeShellEntry> entries;
    for (const json::value& item : spec->at("entries").as_array()) {
      const json::object& entry = item.as_object();
      const json::value& command = entry.at("command");
      FakeShellEntry parsed{.command = command.is_string() ? std::variant<std::string, rt::Regex>(text(command))
                                                           : std::variant<std::string, rt::Regex>(rt::Regex(text(command.at("regex")), text(command.at("flags"))))};
      parsed.exit_code = rt::getNumber(entry, "exit_code");
      parsed.stdout = optionalText(entry, "stdout");
      parsed.stderr = optionalText(entry, "stderr");
      parsed.timed_out = rt::getBool(entry, "timed_out");
      entries.push_back(std::move(parsed));
    }
    const json::object* fallback = rt::getObject(*spec, "fallback");
    if (fallback == nullptr) return std::make_shared<FakeShell>(std::move(entries));
    FakeShell::Fallback parsed;
    parsed.exit_code = rt::getNumber(*fallback, "exit_code");
    parsed.stdout = optionalText(*fallback, "stdout");
    parsed.stderr = optionalText(*fallback, "stderr");
    parsed.timed_out = rt::getBool(*fallback, "timed_out");
    return std::make_shared<FakeShell>(std::move(entries), parsed);
  }

  BrowserPage pageOf(const json::value& value) {
    return {.url = text(value.at("url")), .title = text(value.at("title")), .content = text(value.at("content"))};
  }

  std::vector<BrowserSearchResult> resultsOf(const json::value& value) {
    std::vector<BrowserSearchResult> out;
    for (const json::value& item : value.as_array()) out.push_back({.title = text(item.at("title")), .url = text(item.at("url")), .snippet = text(item.at("snippet"))});
    return out;
  }

  std::optional<rt::OrderedMap<std::string, std::string>> filesOf(const json::object& options, std::string_view key) {
    const json::object* files = rt::getObject(options, key);
    if (files == nullptr) return std::nullopt;
    rt::OrderedMap<std::string, std::string> out;
    for (const json::key_value_pair* entry : rt::objectEntries(*files)) out.set(std::string(entry->key()), text(entry->value()));
    return out;
  }

  std::optional<rt::OrderedMap<std::string, BrowserPage>> pagesOf(const json::object& options, std::string_view key) {
    const json::object* pages = rt::getObject(options, key);
    if (pages == nullptr) return std::nullopt;
    rt::OrderedMap<std::string, BrowserPage> out;
    for (const json::key_value_pair* entry : rt::objectEntries(*pages)) out.set(std::string(entry->key()), pageOf(entry->value()));
    return out;
  }

  std::optional<std::vector<FakeBrowserSearchEntry>> searchOf(const json::object& options, std::string_view key) {
    const json::value* search = rt::get(options, key);
    if (search == nullptr) return std::nullopt;
    std::vector<FakeBrowserSearchEntry> out;
    for (const json::value& item : search->as_array()) out.push_back({.query = text(item.at("query")), .results = resultsOf(item.at("results"))});
    return out;
  }

  std::optional<std::vector<BrowserSearchResult>> fallbackOf(const json::object& options, std::string_view key) {
    const json::value* fallback = rt::get(options, key);
    return fallback != nullptr ? std::optional<std::vector<BrowserSearchResult>>(resultsOf(*fallback)) : std::nullopt;
  }

  struct Subject {
    std::shared_ptr<FakeShell> shell;
    std::shared_ptr<WorkspaceBackend> workspace;
    std::shared_ptr<BrowserBackend> browser;
    std::shared_ptr<PlanBackend> plan;
  };

  Subject subjectOf(const json::object& scenario) {
    const std::string kind = text(scenario.at("kind"));
    const json::object* options = scenario.at("options").if_object();
    const json::object none;
    const json::object& given = options != nullptr ? *options : none;
    Subject subject{.shell = shellOf(given), .workspace = nullptr, .browser = nullptr, .plan = nullptr};
    if (kind == "workspace") {
      if (options == nullptr) {
        subject.workspace = std::make_shared<FakeWorkspaceBackend>();
      } else {
        FakeWorkspaceOptions workspace;
        workspace.label = optionalText(given, "label");
        workspace.files = filesOf(given, "files");
        if (const json::value* scope = rt::get(given, "writeScope")) workspace.writeScope = strings(*scope);
        workspace.shell = subject.shell;
        subject.workspace = std::make_shared<FakeWorkspaceBackend>(std::move(workspace));
      }
    } else if (kind == "browser") {
      if (options == nullptr) {
        subject.browser = std::make_shared<FakeBrowserBackend>();
      } else {
        FakeBrowserOptions browser;
        browser.pages = pagesOf(given, "pages");
        browser.search = searchOf(given, "search");
        browser.searchFallback = fallbackOf(given, "searchFallback");
        subject.browser = std::make_shared<FakeBrowserBackend>(std::move(browser));
      }
    } else if (kind == "plan") {
      subject.plan = std::make_shared<FakePlanBackend>();
    } else if (kind == "backends") {
      ToolBackends backends;
      if (options == nullptr) {
        backends = createFakeToolBackends();
      } else {
        FakeToolBackendsOptions assembly;
        assembly.files = filesOf(given, "files");
        if (const json::value* scope = rt::get(given, "writeScope")) assembly.writeScope = strings(*scope);
        assembly.shell = subject.shell;
        assembly.browserPages = pagesOf(given, "browserPages");
        assembly.browserSearch = searchOf(given, "browserSearch");
        assembly.browserSearchFallback = fallbackOf(given, "browserSearchFallback");
        backends = createFakeToolBackends(std::move(assembly));
      }
      subject.workspace = backends.workspace;
      subject.browser = backends.browser;
      subject.plan = backends.plan;
    }
    return subject;
  }

  json::value pageJson(const BrowserPage& page) { return json::object{{"url", page.url}, {"title", page.title}, {"content", page.content}}; }

  json::value resultsJson(const std::vector<BrowserSearchResult>& results) {
    json::array out;
    for (const BrowserSearchResult& result : results) out.push_back(json::object{{"title", result.title}, {"url", result.url}, {"snippet", result.snippet}});
    return out;
  }

  json::value stringsJson(const std::vector<std::string>& values) {
    json::array out;
    for (const std::string& value : values) out.emplace_back(json::string(value));
    return out;
  }

  // Every synchronous operation; `{ ok }` or the ToolExecutionError as `{ error }`.
  json::value runSync(const Subject& subject, const std::string& name, const json::array& row) {
    try {
      json::value ok;
      if (name == "shell.exec") ok = json::value_from(subject.shell->exec(text(row[1])));
      else if (name == "workspace.label") ok = json::string(subject.workspace->label());
      else if (name == "workspace.read") ok = json::string(subject.workspace->read(text(row[1])));
      else if (name == "workspace.listFiles") ok = stringsJson(subject.workspace->listFiles(text(row[1])));
      else if (name == "workspace.isAllowedWrite") ok = subject.workspace->isAllowedWrite(text(row[1]));
      else if (name == "workspace.describeWriteScope") ok = json::string(subject.workspace->describeWriteScope());
      else if (name == "workspace.write") {
        subject.workspace->write(text(row[1]), text(row[2]));
        ok = nullptr;
      } else if (name == "workspace.applyPatch") {
        const WorkspaceBackend::Patch patch{.old = text(row[2].at("old")), .new_ = text(row[2].at("new")), .add = row[2].at("add").as_bool()};
        const WorkspaceBackend::AppliedPatch applied = subject.workspace->applyPatch(text(row[1]), patch);
        ok = json::object{{"applied", applied.applied}, {"detail", applied.detail}};
      } else if (name == "browser.search") ok = resultsJson(subject.browser->search(text(row[1])));
      else if (name == "browser.open") ok = pageJson(subject.browser->open(text(row[1])));
      else if (name == "browser.findOnCurrentPage") {
        const BrowserBackend::CurrentPageMatches found = subject.browser->findOnCurrentPage(text(row[1]));
        json::array matches;
        for (const auto& match : found.matches) matches.push_back(json::object{{"line", rt::makeNumber(match.line)}, {"text", match.text}});
        ok = json::object{{"url", found.url}, {"matches", std::move(matches)}};
      } else if (name == "plan.update") ok = json::object{{"items", rt::makeNumber(subject.plan->update(strings(row[1])).items)}};
      else if (name == "plan.current") ok = stringsJson(std::dynamic_pointer_cast<FakePlanBackend>(subject.plan)->current());
      else throw std::logic_error("unknown operation " + name);
      return json::object{{"ok", std::move(ok)}};
    } catch (const ToolExecutionError& error) {
      return json::object{{"error", json::object{{"name", error.name}, {"code", error.code}, {"message", error.message}}}};
    }
  }
}

ASYNC_TEST("differential: fake backends replay the recorded Bun scenarios") {
  const json::value vectors = rt::jsonParse(kFakesVectors);
  int operations = 0;
  for (const json::value& scenario : vectors.at("scenarios").as_array()) {
    const Subject subject = subjectOf(scenario.as_object());
    for (const json::value& op : scenario.at("ops").as_array()) {
      const json::array& row = op.as_array();
      const std::string name = text(row[0]);
      json::value actual;
      if (name == "workspace.execShell") {
        const WorkspaceBackend::ExecShellOptions options{.timeoutMs = 1000};
        const std::string command = text(row[1]);
        rt::Task<ShellExecResult> execution = subject.workspace->execShell(command, options);
        const ShellExecResult result = co_await std::move(execution);
        actual = json::object{{"ok", json::value_from(result)}};
      } else {
        actual = runSync(subject, name, row);
      }
      const std::string got = rt::jsonStringify(actual);
      const std::string wanted = rt::jsonStringify(row.back());
      if (got != wanted) {
        EXPECT_EQ(got, wanted);
        std::fprintf(stderr, "      scenario %s, operation %s\n", rt::jsonStringify(scenario.at("options")).c_str(), rt::jsonStringify(op).c_str());
      }
      operations += 1;
    }
  }
  EXPECT_EQ(operations, 372);
}

TEST("differential: the built-in site map and search index match Bun") {
  const json::value vectors = rt::jsonParse(kFakesVectors);
  json::object pages;
  for (const auto& [url, page] : FAKE_BROWSER_PAGES) pages[url] = pageJson(page);
  EXPECT_EQ(rt::jsonStringify(pages), rt::jsonStringify(vectors.at("FAKE_BROWSER_PAGES")));
  json::array search;
  for (const FakeBrowserSearchEntry& entry : FAKE_BROWSER_SEARCH) search.push_back(json::object{{"query", entry.query}, {"results", resultsJson(entry.results)}});
  EXPECT_EQ(rt::jsonStringify(search), rt::jsonStringify(vectors.at("FAKE_BROWSER_SEARCH")));
}

TEST("execShell runs the scripted shell when called, like Promise.resolve(exec())") {
  // A g-flag entry alternates between matching and not matching "x"; the unawaited call still
  // consumed one match, as the eagerly evaluated TS argument does.
  auto shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{FakeShellEntry{.command = rt::Regex("x", "g"), .stdout = "global"}});
  FakeWorkspaceOptions options;
  options.shell = shell;
  FakeWorkspaceBackend workspace(std::move(options));
  const WorkspaceBackend::ExecShellOptions execOptions{.timeoutMs = 1};
  { [[maybe_unused]] rt::Task<ShellExecResult> unawaited = workspace.execShell("x", execOptions); }
  EXPECT_EQ(shell->exec("x").exit_code, 127.0);
  EXPECT_EQ(shell->exec("x").stdout, "global");
}

TEST("write and applyPatch consult the overridable isAllowedWrite, as JS method calls do") {
  class OpenWorkspace final : public FakeWorkspaceBackend {
  public:
    using FakeWorkspaceBackend::FakeWorkspaceBackend;
    bool isAllowedWrite(const std::string&) override { return true; }
  };
  FakeWorkspaceOptions options;
  options.writeScope = std::vector<std::string>{"!**"};
  OpenWorkspace workspace(std::move(options));
  workspace.write("anything.txt", "x");
  EXPECT_EQ(workspace.read("anything.txt"), "x");
  EXPECT_EQ(workspace.applyPatch("anything.txt", {.old = "x", .new_ = "y", .add = false}).detail, "patched anything.txt");
  EXPECT_EQ(workspace.describeWriteScope(), "!**");
}
