#pragma once
/**
 * Canonical tool backend contracts and workspace boundaries (plan m04).
 *
 * The router (`router.ts`) owns tool semantics; actual I/O lives behind these
 * injected interfaces so harnesses stay deterministic and testable offline:
 *
 * - {@link WorkspaceBackend}: the disposable workspace (read/list/write/patch/
 *   shell) with write-scope enforcement (m02 task manifests).
 * - {@link BrowserBackend}: browser search/open/find (a real browser daemon is
 *   replaceable; the deterministic offline fake lives in `fakes.ts`).
 * - {@link PlanBackend}: in-memory task plan state (a per-run scratch store in
 *   real harnesses).
 *
 * Path boundaries are enforced here: {@link normalizeWorkspacePath} rejects
 * absolute paths and `..` traversal, and backends resolve every path below
 * their workspace root. Backends throw `ToolExecutionError` (from
 * `result.ts`) with the canonical error codes; the router converts them to
 * {@link ToolResult} envelopes and never lets backend failures escape as
 * exceptions.
 */
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::harmony::tools::backend {
  /** Result of one shell execution. */
  struct ShellExecResult {
    double exit_code;
    std::string stdout;
    std::string stderr;
    bool timed_out;
    friend bool operator==(const ShellExecResult&, const ShellExecResult&) = default;
  };

  /** Workspace-relative file operations and shell execution. */
  class WorkspaceBackend {
  public:
    // Readonly<{ old: string; new: string; add: boolean }>
    struct Patch {
      std::string old;
      std::string new_;
      bool add;
    };
    // Readonly<{ applied: true; detail: string }>
    struct AppliedPatch {
      bool applied;
      std::string detail;
    };
    // Readonly<{ timeoutMs: number; signal?: AbortSignal }>
    struct ExecShellOptions {
      double timeoutMs;
      std::optional<rt::AbortSignal> signal{};
    };

    virtual ~WorkspaceBackend() = default;
    /** Stable label for error messages, e.g. `fixture-workspace` or `fake-workspace`. */
    virtual std::string label() const = 0;
    /** Read a file. Throws ToolExecutionError `not_found` when it does not exist. */
    virtual std::string read(const std::string& rel) = 0;
    /** Sorted workspace-relative paths of the files under a directory. */
    virtual std::vector<std::string> listFiles(const std::string& rel) = 0;
    /** True when the path is inside the workspace and matches the write scope. */
    virtual bool isAllowedWrite(const std::string& rel) = 0;
    // Human-readable write scope description, e.g. `**, !protected/**`.
    virtual std::string describeWriteScope() = 0;
    /** Write a file. Throws ToolExecutionError `write_scope` when out of scope. */
    virtual void write(const std::string& rel, const std::string& content) = 0;
    /**
     * Apply a patch (replace the unique occurrence of `old` with `new`, or
     * create the file when `add` is true). Throws ToolExecutionError
     * `write_scope` or `patch_failed`.
     */
    virtual AppliedPatch applyPatch(const std::string& rel, const Patch& patch) = 0;
    /** Run a command with `sh -c` inside the workspace. */
    [[nodiscard]] virtual rt::Task<ShellExecResult> execShell(std::string command, ExecShellOptions opts) = 0;
  };

  /** One deterministic web search result. */
  struct BrowserSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
    friend bool operator==(const BrowserSearchResult&, const BrowserSearchResult&) = default;
  };

  /** One opened page. */
  struct BrowserPage {
    std::string url;
    std::string title;
    std::string content;
    friend bool operator==(const BrowserPage&, const BrowserPage&) = default;
  };

  /** One text match inside the current page. */
  struct BrowserLineMatch {
    double line;
    std::string text;
    friend bool operator==(const BrowserLineMatch&, const BrowserLineMatch&) = default;
  };

  /** Browser state and navigation; backends own the current-page state. */
  class BrowserBackend {
  public:
    // Readonly<{ url: string; matches: readonly BrowserLineMatch[] }>
    struct CurrentPageMatches {
      std::string url;
      std::vector<BrowserLineMatch> matches;
    };

    virtual ~BrowserBackend() = default;
    /** Search the web. Returns an empty list when nothing matches. */
    virtual std::vector<BrowserSearchResult> search(const std::string& query) = 0;
    /** Open a page (making it the current page). Throws `not_found` when unknown. */
    virtual BrowserPage open(const std::string& url) = 0;
    /** Search the current page. Throws `invalid_args` when no page is open. */
    virtual CurrentPageMatches findOnCurrentPage(const std::string& query) = 0;
  };

  /** Task plan state. */
  class PlanBackend {
  public:
    // Readonly<{ items: number }>
    struct UpdatedPlan {
      double items;
    };

    virtual ~PlanBackend() = default;
    /** Replace the plan; returns the number of stored items. */
    virtual UpdatedPlan update(const std::vector<std::string>& plan) = 0;
  };

  /** The injected dependencies for one canonical tool dispatch. */
  struct ToolBackends {
    std::shared_ptr<WorkspaceBackend> workspace;
    /** Absent when the harness runs without browser support. */
    std::shared_ptr<BrowserBackend> browser{};
    /** Absent when the harness runs without plan tracking. */
    std::shared_ptr<PlanBackend> plan{};
  };

  /**
   * Normalizes a workspace-relative path and rejects traversal.
   *
   * Returns the normalized relative path ("" for the workspace root, e.g. "."),
   * or null when the path is empty, absolute, or contains a `..` segment.
   */
  std::optional<std::string> normalizeWorkspacePath(std::string_view rel);

  /**
   * Glob matcher with the benchmark fixture semantics: `*` (one segment),
   * `**` (across segments), `?` (single character).
   */
  bool globMatch(std::string_view pattern, std::string_view path);

  // JSON members in declaration order; reading is strict (rt::TypeError naming the member).
  void tag_invoke(const json::value_from_tag&, json::value& out, const ShellExecResult& value);
  ShellExecResult tag_invoke(const json::value_to_tag<ShellExecResult>&, const json::value& value);
}
