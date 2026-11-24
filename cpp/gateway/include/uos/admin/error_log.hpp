#pragma once
// Port of src/admin/error-log.ts.
//
// A stored error-log row is returned as the value KV holds (`rt::KvValue`): the TS returns the
// stored object itself, so its member order (the writer's input order, `version` last) and any
// undeclared member reach the admin JSON unchanged. `AdminErrorLogRecord` is the typed record.
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::admin::error_log {
  inline const rt::KvKey ADMIN_ERROR_LOG_PREFIX{"uos_ai", "admin_error_log", "v1"};
  inline constexpr double ADMIN_ERROR_LOG_TTL_MS = 7 * 24 * 60 * 60'000;
  inline constexpr double ADMIN_ERROR_BUCKET_MS = 15 * 60'000;

  struct AdminErrorLogRecord {
    double version = 1;
    std::string request_id;
    std::string route;
    double status;
    std::string provider;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    std::optional<bool> stream;
    std::string terminal_type;
    std::string failure_kind;
    std::string delivery_outcome;  // "delivered" | "interrupted" | "unobserved"
    double created_at_ms;
    double latency_ms;
    std::optional<std::string> git_sha;
    std::optional<std::string> deno_revision;
    friend bool operator==(const AdminErrorLogRecord&, const AdminErrorLogRecord&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminErrorLogRecord& value);
  AdminErrorLogRecord tag_invoke(const json::value_to_tag<AdminErrorLogRecord>&, const json::value& value);

  // `Omit<AdminErrorLogRecord, "version" | "terminal_type" | "failure_kind"> & Readonly<{ terminal_type: string | null;
  // failure_kind: string | null }>`: members in the order the terminal logger writes them, which is the stored order.
  struct AdminErrorInput {
    std::string request_id;
    std::string route;
    double status;
    std::string provider;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    std::optional<bool> stream;
    std::optional<std::string> terminal_type;
    std::optional<std::string> failure_kind;
    std::string delivery_outcome;  // "delivered" | "interrupted" | "unobserved"
    double created_at_ms;
    double latency_ms;
    std::optional<std::string> git_sha;
    std::optional<std::string> deno_revision;
    /** Set only by a handler that verified authentication. Not part of the stored row. */
    bool authenticated = false;
  };

  // `kvOverride?: Kv | null`: the overload without it is undefined (getKv); a null pointer is null.
  [[nodiscard]] rt::Task<void> recordAdminError(const AdminErrorInput& input);
  [[nodiscard]] rt::Task<void> recordAdminError(const AdminErrorInput& input, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `value: unknown`
  bool isAdminErrorLogRecord(const rt::KvValue& value);

  // `{ path: string; bytes: number | null }`
  struct AdminLogFile {
    std::string path;
    std::optional<double> bytes;
    friend bool operator==(const AdminLogFile&, const AdminLogFile&) = default;
  };

  /** Stat-only observability for host text logs; content is never read. */
  struct AdminLogFileSizes {
    std::string scope;  // "host_text_logs"
    std::string platform;
    std::vector<AdminLogFile> files;
    std::optional<double> total_bytes;
    bool warning;
    friend bool operator==(const AdminLogFileSizes&, const AdminLogFileSizes&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminLogFile& value);
  AdminLogFile tag_invoke(const json::value_to_tag<AdminLogFile>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminLogFileSizes& value);
  AdminLogFileSizes tag_invoke(const json::value_to_tag<AdminLogFileSizes>&, const json::value& value);

  // `{ bucket_start_at_ms: number; count: number }`
  struct AdminErrorBucket {
    double bucket_start_at_ms;
    double count;
    friend bool operator==(const AdminErrorBucket&, const AdminErrorBucket&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminErrorBucket& value);
  AdminErrorBucket tag_invoke(const json::value_to_tag<AdminErrorBucket>&, const json::value& value);

  /** The bounded history scan: exactly one list over the error-log prefix. */
  // `data` holds the stored records themselves (each accepted by isAdminErrorLogRecord).
  struct AdminErrorHistoryScan {
    std::vector<rt::KvValue> data;
    std::vector<AdminErrorBucket> five_xx_buckets;
  };

  /** The full admin payload: the bounded scan plus the independent retention and host-text-log scopes. */
  struct AdminErrorHistory {
    std::vector<rt::KvValue> data;
    std::vector<AdminErrorBucket> five_xx_buckets;
    sentinel::replay_retention_schema::SentinelReplayRetentionStatus retention;
    AdminLogFileSizes log_files;
  };

  /**
   * Scan the bounded error history once. This intentionally performs exactly ONE
   * `kv.list` so a page render cannot turn into an unbounded storage walk; the
   * capture-retention status is composed by the HTTP handler instead of being
   * collected here.
   */
  // `limit = DEFAULT_LIMIT`, `kvOverride?: Kv | null` (as recordAdminError's).
  [[nodiscard]] rt::Task<AdminErrorHistoryScan> listAdminErrorHistory();
  [[nodiscard]] rt::Task<AdminErrorHistoryScan> listAdminErrorHistory(double limit);
  [[nodiscard]] rt::Task<AdminErrorHistoryScan> listAdminErrorHistory(double limit, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  [[nodiscard]] rt::Task<rt::Response> handleAdminErrors(rt::Request req);
}
