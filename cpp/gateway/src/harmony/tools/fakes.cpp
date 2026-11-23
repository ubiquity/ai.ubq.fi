#include "uos/harmony/tools/fakes.hpp"

#include "uos/harmony/tools/result.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace uos::harmony::tools::fakes {
  using backend::BrowserLineMatch;
  using backend::BrowserPage;
  using backend::BrowserSearchResult;
  using backend::ShellExecResult;
  using result::ToolExecutionError;

  // ---------------------------------------------------------------------------
  // Fake shell
  // ---------------------------------------------------------------------------

  FakeShell::FakeShell() : FakeShell(std::vector<FakeShellEntry>{}) {}

  FakeShell::FakeShell(std::vector<FakeShellEntry> entries_) : FakeShell(std::move(entries_), std::nullopt) {}

  FakeShell::FakeShell(std::vector<FakeShellEntry> entries_, std::optional<Fallback> fallback)
    : entries(std::move(entries_)), fallback_(fallback ? std::move(*fallback) : Fallback{.exit_code = 127}) {}

  ShellExecResult FakeShell::exec(std::string_view command) {
    for (FakeShellEntry& entry : entries) {
      const bool matched = std::holds_alternative<std::string>(entry.command) ? std::get<std::string>(entry.command) == command
                                                                              : std::get<rt::Regex>(entry.command).test(command);
      if (!matched) continue;
      if (entry.timed_out == true) return {.exit_code = -1, .stdout = "", .stderr = "", .timed_out = true};
      return {
        .exit_code = entry.exit_code.value_or(0),
        .stdout = entry.stdout.value_or(""),
        .stderr = entry.stderr.value_or(""),
        .timed_out = false,
      };
    }
    const Fallback& fallback = fallback_;
    return {
      .exit_code = fallback.exit_code.value_or(127),
      .stdout = fallback.stdout.value_or(""),
      .stderr = fallback.stderr ? *fallback.stderr : "fake shell: no scripted response for " + rt::jsonQuote(command),
      .timed_out = false,
    };
  }

  // ---------------------------------------------------------------------------
  // Fake workspace
  // ---------------------------------------------------------------------------

  namespace {
    /**
     * Code-unit ascending order — the exact order `Array.prototype.sort` applies by
     * default, stated explicitly so fake listings stay deterministic and locale
     * independent (`README.md` keeps sorting before `docs/x`).
     */
    double compareCodeUnits(const std::string& a, const std::string& b) {
      if (rt::lessThan(a, b)) return -1;
      if (rt::lessThan(b, a)) return 1;
      return 0;
    }

    // The settled `Promise.resolve(value)`: awaiting it does not suspend again.
  }

  FakeWorkspaceBackend::FakeWorkspaceBackend() : FakeWorkspaceBackend(FakeWorkspaceOptions{}) {}

  FakeWorkspaceBackend::FakeWorkspaceBackend(FakeWorkspaceOptions opts)
    : label_(opts.label ? std::move(*opts.label) : std::string("fake-workspace")),
      files_(opts.files ? std::move(*opts.files) : rt::OrderedMap<std::string, std::string>()),
      writeScope_(opts.writeScope ? std::move(*opts.writeScope) : std::vector<std::string>{"**"}),
      shell_(opts.shell ? std::move(opts.shell) : std::make_shared<FakeShell>()) {}

  std::string FakeWorkspaceBackend::label() const { return label_; }

  std::string FakeWorkspaceBackend::read(const std::string& rel) {
    const std::string path = resolve_(rel);
    const std::string* content = files_.get(path);
    if (content == nullptr) throw ToolExecutionError("not_found", "file not found: " + path);
    return *content;
  }

  std::vector<std::string> FakeWorkspaceBackend::listFiles(const std::string& rel) {
    const std::string path = resolve_(rel);
    const std::string prefix = path.empty() ? std::string() : path + "/";
    // Code-unit order (the default `sort` order), stated explicitly so the fake
    // keeps returning the same listing regardless of locale.
    std::vector<std::string> listing;
    for (const std::string& file : files_.keys()) {
      if (rt::startsWith(file, prefix)) listing.push_back(file);
    }
    rt::sort(listing, compareCodeUnits);
    return listing;
  }

  bool FakeWorkspaceBackend::isAllowedWrite(const std::string& rel) {
    static const rt::Regex NEGATION("^!");
    const std::optional<std::string> path = backend::normalizeWorkspacePath(rel);
    if (!path) return false;
    bool allowed = false;
    for (const std::string& pattern : writeScope_) {
      if (backend::globMatch(rt::replace(pattern, NEGATION, ""), *path)) allowed = !rt::startsWith(pattern, "!");
    }
    return allowed;
  }

  std::string FakeWorkspaceBackend::describeWriteScope() {
    std::string joined;
    for (std::size_t index = 0; index < writeScope_.size(); ++index) {
      if (index) joined += ", ";
      rt::append(joined, writeScope_[index]);
    }
    return joined;
  }

  void FakeWorkspaceBackend::write(const std::string& rel, const std::string& content) {
    const std::string path = resolve_(rel);
    if (!isAllowedWrite(path)) {
      throw ToolExecutionError("write_scope", "write scope violation: " + path + " is not writable (scope: " + describeWriteScope() + ")");
    }
    files_.set(path, content);
  }

  backend::WorkspaceBackend::AppliedPatch FakeWorkspaceBackend::applyPatch(const std::string& rel, const Patch& patch) {
    const std::string path = resolve_(rel);
    if (!isAllowedWrite(path)) {
      throw ToolExecutionError("write_scope", "write scope violation: " + path + " is not writable (scope: " + describeWriteScope() + ")");
    }
    if (patch.add) {
      if (files_.has(path)) {
        throw ToolExecutionError("patch_failed", "patch add refused: " + path + " already exists");
      }
      files_.set(path, patch.new_);
      return {.applied = true, .detail = "created " + path};
    }
    const std::string* stored = files_.get(path);
    if (stored == nullptr) {
      throw ToolExecutionError("patch_failed", "patch failed: " + path + " does not exist");
    }
    const std::string content = *stored;
    if (patch.old.empty()) {
      // Empty `old` prepends `new` at the start of the file (an empty string
      // would otherwise "occur" at every offset and always be ambiguous).
      files_.set(path, rt::concat(patch.new_, content));
      return {.applied = true, .detail = "patched " + path};
    }
    const std::int64_t first = rt::indexOf(content, patch.old);
    if (first == -1) {
      throw ToolExecutionError("patch_failed", "patch failed: " + path + " does not contain the expected old text");
    }
    if (rt::includes(content, patch.old, first + 1)) {
      throw ToolExecutionError("patch_failed", "patch failed: old text occurs more than once in " + path);
    }
    files_.set(path, rt::concat(rt::concat(rt::slice(content, 0, first), patch.new_), rt::slice(content, first + rt::length(patch.old))));
    return {.applied = true, .detail = "patched " + path};
  }

  rt::Task<ShellExecResult> FakeWorkspaceBackend::execShell(std::string command, ExecShellOptions /*opts*/) {
    ShellExecResult result = shell_->exec(command);
    return rt::fulfilledTask(std::move(result));
  }

  std::string FakeWorkspaceBackend::resolve_(const std::string& rel) const {
    if (rel.empty()) return "";  // normalized workspace root
    std::optional<std::string> path = backend::normalizeWorkspacePath(rel);
    if (!path) throw ToolExecutionError("path_escape", "path escapes workspace root: " + rel);
    return std::move(*path);
  }

  // ---------------------------------------------------------------------------
  // Fake browser
  // ---------------------------------------------------------------------------

  FakeBrowserBackend::FakeBrowserBackend() : FakeBrowserBackend(FakeBrowserOptions{}) {}

  FakeBrowserBackend::FakeBrowserBackend(FakeBrowserOptions opts)
    : pages_(opts.pages ? std::move(*opts.pages) : rt::OrderedMap<std::string, BrowserPage>()),
      searchFallback_(opts.searchFallback ? std::move(*opts.searchFallback) : std::vector<BrowserSearchResult>()) {
    if (opts.search) {
      for (FakeBrowserSearchEntry& entry : *opts.search) searchIndex_.set(rt::toLowerCase(entry.query), std::move(entry.results));
    }
  }

  std::vector<BrowserSearchResult> FakeBrowserBackend::search(const std::string& query) {
    const std::vector<BrowserSearchResult>* results = searchIndex_.get(rt::toLowerCase(query));
    return results != nullptr ? *results : searchFallback_;
  }

  BrowserPage FakeBrowserBackend::open(const std::string& url) {
    const BrowserPage* page = pages_.get(url);
    if (page == nullptr) throw ToolExecutionError("not_found", "no page at " + url);
    currentUrl_ = url;
    return *page;
  }

  backend::BrowserBackend::CurrentPageMatches FakeBrowserBackend::findOnCurrentPage(const std::string& query) {
    if (!currentUrl_) {
      throw ToolExecutionError("invalid_args", "no page open; call browser.open first");
    }
    const BrowserPage* page = pages_.get(*currentUrl_);
    if (page == nullptr) throw ToolExecutionError("not_found", "no page at " + *currentUrl_);
    const std::string needle = rt::toLowerCase(query);
    std::vector<BrowserLineMatch> matches;
    const std::vector<std::string> lines = rt::split(page->content, "\n");
    for (std::size_t index = 0; index < lines.size(); ++index) {
      if (rt::includes(rt::toLowerCase(lines[index]), needle)) matches.push_back({.line = static_cast<double>(index + 1), .text = lines[index]});
    }
    return {.url = *currentUrl_, .matches = std::move(matches)};
  }

  // ---------------------------------------------------------------------------
  // Fake plan
  // ---------------------------------------------------------------------------

  backend::PlanBackend::UpdatedPlan FakePlanBackend::update(const std::vector<std::string>& plan) {
    plan_ = plan;
    return {.items = static_cast<double>(plan_.size())};
  }

  std::vector<std::string> FakePlanBackend::current() const { return plan_; }

  // ---------------------------------------------------------------------------
  // Defaults and assembly
  // ---------------------------------------------------------------------------

  backend::ToolBackends createFakeToolBackends() { return createFakeToolBackends(FakeToolBackendsOptions{}); }

  backend::ToolBackends createFakeToolBackends(FakeToolBackendsOptions opts) {
    FakeWorkspaceOptions workspace;
    workspace.files = std::move(opts.files);
    workspace.writeScope = std::move(opts.writeScope);
    workspace.shell = opts.shell;
    FakeBrowserOptions browser;
    browser.pages = opts.browserPages ? std::move(*opts.browserPages) : FAKE_BROWSER_PAGES;
    browser.search = opts.browserSearch ? std::move(*opts.browserSearch) : FAKE_BROWSER_SEARCH;
    browser.searchFallback = std::move(opts.browserSearchFallback);
    return backend::ToolBackends{
      .workspace = std::make_shared<FakeWorkspaceBackend>(std::move(workspace)),
      .browser = std::make_shared<FakeBrowserBackend>(std::move(browser)),
      .plan = std::make_shared<FakePlanBackend>(),
    };
  }
}
