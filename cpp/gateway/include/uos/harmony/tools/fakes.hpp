#pragma once
/**
 * Deterministic fake tool backends (plan m04).
 *
 * These fakes are the offline stand-ins for the real infrastructure routing:
 * no network, no browser daemon, no live shell, no filesystem access outside
 * the injected maps. They are the guarantee that the canonical tool surface
 * (schema validation, envelopes, path/write boundaries, command exit/
 * stdout/stderr semantics) is testable deterministically.
 *
 * - {@link FakeWorkspaceBackend}: an in-memory file tree plus a scripted
 *   {@link FakeShell} and write-scope enforcement.
 * - {@link FakeBrowserBackend}: a page map, a search index and current-page
 *   state (the state a real browser daemon would own).
 * - {@link FakePlanBackend}: an in-memory ordered plan.
 *
 * {@link createFakeToolBackends} assembles a complete {@link ToolBackends}
 * with a small built-in site map so harnesses get deterministic behavior out
 * of the box (the benchmark reference adapter relies on this).
 */
#include "uos/harmony/tools/backend.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::harmony::tools::fakes {
  // ---------------------------------------------------------------------------
  // Fake shell
  // ---------------------------------------------------------------------------

  /** One scripted shell response. `command` matches exactly or by RegExp. */
  struct FakeShellEntry {
    std::variant<std::string, rt::Regex> command;
    std::optional<double> exit_code{};
    std::optional<std::string> stdout{};
    std::optional<std::string> stderr{};
    /** Simulate a time-out instead of returning an exit code. */
    std::optional<bool> timed_out{};
  };

  /** Deterministic scripted shell: first matching entry wins, unknown falls back. */
  class FakeShell {
  public:
    // Readonly<Omit<FakeShellEntry, "command">>
    struct Fallback {
      std::optional<double> exit_code{};
      std::optional<std::string> stdout{};
      std::optional<std::string> stderr{};
      std::optional<bool> timed_out{};
    };

    // Matching a RegExp entry with the g or y flag moves that entry's lastIndex, as in JS.
    std::vector<FakeShellEntry> entries;

    FakeShell();
    explicit FakeShell(std::vector<FakeShellEntry> entries_);
    // std::nullopt is an absent fallback ({ exit_code: 127 }).
    FakeShell(std::vector<FakeShellEntry> entries_, std::optional<Fallback> fallback);

    backend::ShellExecResult exec(std::string_view command);

  private:
    Fallback fallback_;
  };

  // ---------------------------------------------------------------------------
  // Fake workspace
  // ---------------------------------------------------------------------------

  // A Readonly<Record<string, T>> option is an rt::OrderedMap: the fakes copy it into their
  // own Map and only ever look keys up or sort them, so property order is never observed.
  struct FakeWorkspaceOptions {
    /** Label used in error messages; default `fake-workspace`. */
    std::optional<std::string> label{};
    /** Initial file tree, keyed by workspace-relative path. */
    std::optional<rt::OrderedMap<std::string, std::string>> files{};
    /** Allowed write scope globs (`!` negates); default `["**"]`. */
    std::optional<std::vector<std::string>> writeScope{};
    /** Scripted shell; default rejects every command with exit 127. nullptr is undefined. */
    std::shared_ptr<FakeShell> shell{};
  };

  /** In-memory workspace backend with scripted shell and write-scope checks. */
  class FakeWorkspaceBackend : public backend::WorkspaceBackend {
  public:
    FakeWorkspaceBackend();
    explicit FakeWorkspaceBackend(FakeWorkspaceOptions opts);

    std::string label() const override;
    std::string read(const std::string& rel) override;
    std::vector<std::string> listFiles(const std::string& rel) override;
    bool isAllowedWrite(const std::string& rel) override;
    std::string describeWriteScope() override;
    void write(const std::string& rel, const std::string& content) override;
    AppliedPatch applyPatch(const std::string& rel, const Patch& patch) override;
    // Runs the scripted shell when called and returns the settled result
    // (`Promise.resolve(this.#shell.exec(command))`).
    [[nodiscard]] rt::Task<backend::ShellExecResult> execShell(std::string command, ExecShellOptions opts) override;

  private:
    std::string label_;
    rt::OrderedMap<std::string, std::string> files_;
    std::vector<std::string> writeScope_;
    std::shared_ptr<FakeShell> shell_;

    std::string resolve_(const std::string& rel) const;
  };

  // ---------------------------------------------------------------------------
  // Fake browser
  // ---------------------------------------------------------------------------

  struct FakeBrowserSearchEntry {
    /** Exact query, matched case-insensitively. */
    std::string query;
    std::vector<backend::BrowserSearchResult> results;
    friend bool operator==(const FakeBrowserSearchEntry&, const FakeBrowserSearchEntry&) = default;
  };

  struct FakeBrowserOptions {
    /** Pages reachable through browser.open, keyed by URL. */
    std::optional<rt::OrderedMap<std::string, backend::BrowserPage>> pages{};
    /** Search index entries, matched case-insensitively by exact query. */
    std::optional<std::vector<FakeBrowserSearchEntry>> search{};
    /** Results returned for any query without an index entry. */
    std::optional<std::vector<backend::BrowserSearchResult>> searchFallback{};
  };

  /** Deterministic offline browser: page map, search index, current-page state. */
  class FakeBrowserBackend : public backend::BrowserBackend {
  public:
    FakeBrowserBackend();
    explicit FakeBrowserBackend(FakeBrowserOptions opts);

    std::vector<backend::BrowserSearchResult> search(const std::string& query) override;
    backend::BrowserPage open(const std::string& url) override;
    CurrentPageMatches findOnCurrentPage(const std::string& query) override;

  private:
    rt::OrderedMap<std::string, backend::BrowserPage> pages_;
    rt::OrderedMap<std::string, std::vector<backend::BrowserSearchResult>> searchIndex_;
    std::vector<backend::BrowserSearchResult> searchFallback_;
    std::optional<std::string> currentUrl_{};
  };

  // ---------------------------------------------------------------------------
  // Fake plan
  // ---------------------------------------------------------------------------

  /** In-memory ordered task plan state. */
  class FakePlanBackend : public backend::PlanBackend {
  public:
    UpdatedPlan update(const std::vector<std::string>& plan) override;
    std::vector<std::string> current() const;

  private:
    std::vector<std::string> plan_{};
  };

  // ---------------------------------------------------------------------------
  // Defaults and assembly
  // ---------------------------------------------------------------------------

  /** Built-in deterministic site map used by `createFakeToolBackends`. */
  inline const rt::OrderedMap<std::string, backend::BrowserPage> FAKE_BROWSER_PAGES{
    {"https://example.com/index.html",
     backend::BrowserPage{
       .url = "https://example.com/index.html",
       .title = "Example",
       .content = "Welcome to the example site.\nFind content on this page.\n",
     }},
  };

  /** Built-in deterministic search index. */
  inline const std::vector<FakeBrowserSearchEntry> FAKE_BROWSER_SEARCH{
    FakeBrowserSearchEntry{
      .query = "example",
      .results =
        {
          backend::BrowserSearchResult{
            .title = "Example",
            .url = "https://example.com/index.html",
            .snippet = "Welcome to the example site.",
          },
        },
    },
  };

  struct FakeToolBackendsOptions {
    std::optional<rt::OrderedMap<std::string, std::string>> files{};
    std::optional<std::vector<std::string>> writeScope{};
    /** nullptr is undefined. */
    std::shared_ptr<FakeShell> shell{};
    std::optional<rt::OrderedMap<std::string, backend::BrowserPage>> browserPages{};
    std::optional<std::vector<FakeBrowserSearchEntry>> browserSearch{};
    std::optional<std::vector<backend::BrowserSearchResult>> browserSearchFallback{};
  };

  /** Assembles a complete deterministic ToolBackends with default fake data. */
  backend::ToolBackends createFakeToolBackends();
  backend::ToolBackends createFakeToolBackends(FakeToolBackendsOptions opts);
}
