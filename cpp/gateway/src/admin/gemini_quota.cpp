#include "uos/admin/gemini_quota.hpp"

#include "uos/http.hpp"
#include "uos/openai.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"

#include <array>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uos::admin::gemini_quota {
  namespace {
    constexpr std::size_t MAX_QUOTA_FILE_BYTES = 64 * 1024;
    constexpr std::size_t MAX_BUCKETS = 64;

    struct RefreshGate {
      std::optional<rt::Promise<Snapshot>> pending{};
      double token = 0;
    };

    RefreshGate& refreshGate() {
      static RefreshGate gate;
      return gate;
    }

    std::optional<std::string> shortLabel(const json::value* value) {
      const auto text = utils::getString(value);
      if (!text) return std::nullopt;
      const std::string trimmed = rt::trim(*text);
      if (trimmed.empty() || rt::length(trimmed) > 96) return std::nullopt;
      for (const std::uint32_t code : rt::codePoints(trimmed)) if (code < 0x20 || code == 0x7f || code == '@') return std::nullopt;
      return trimmed;
    }

    std::optional<double> fraction(const json::value* value) {
      if (!value || !rt::isNumber(*value)) return std::nullopt;
      const double number = rt::toNumber(*value);
      return rt::Number::isFinite(number) && number >= 0 && number <= 1 ? std::optional(number) : std::nullopt;
    }

    std::optional<double> nonnegative(const json::value* value) {
      if (!value || !rt::isNumber(*value)) return std::nullopt;
      const double number = rt::toNumber(*value);
      return rt::Number::isFinite(number) && number >= 0 && number <= 9'007'199'254'740'991.0 ? std::optional(number) : std::nullopt;
    }

    std::optional<std::string> resetTime(const json::value* value) {
      const auto raw = utils::getString(value);
      if (!raw) return std::nullopt;
      const double timestamp = rt::Date::parse(*raw);
      if (!rt::Number::isFinite(timestamp) || timestamp < 0) return std::nullopt;
      const rt::Date date(timestamp);
      std::string normalized = date.toISOString();
      return normalized.size() == 24 ? std::optional(std::string(std::move(normalized))) : std::nullopt;
    }

    Bucket bucket(const json::object& raw) {
      Bucket result;
      const auto remainingFraction = fraction(rt::get(raw, "remaining_fraction"));
      const auto remainingAmount = nonnegative(rt::get(raw, "remaining_amount"));
      if (remainingFraction && !remainingAmount) result.remaining_fraction = remainingFraction;
      if (remainingAmount && !remainingFraction) result.remaining_amount = remainingAmount;
      if (const auto reset = resetTime(rt::get(raw, "reset_time"))) result.reset_time = reset;
      else if (const auto seconds = nonnegative(rt::get(raw, "reset_in_seconds"))) result.reset_in_seconds = seconds;
      if (const auto* disabled = rt::get(raw, "disabled"); disabled && disabled->is_bool()) result.disabled = disabled->as_bool();
      return result;
    }

    Snapshot unavailable(std::string status) {
      return Snapshot{.observed_at_ms = 0, .status = std::move(status), .plan_tier = std::nullopt, .quota = {}};
    }

    std::optional<Snapshot> decodeSnapshot(const json::value& value, double nowMs) {
      if (!value.is_object()) return std::nullopt;
      const auto& object = value.as_object();
      const auto version = rt::getNumber(object, "version");
      const auto observed = nonnegative(rt::get(object, "observed_at_ms"));
      const auto status = rt::getString(object, "status");
      if (!version || *version != 1 || !observed || *observed > nowMs || !status) return std::nullopt;
      if (*status != "observed" && *status != "signed_out" && *status != "invalid_event") return std::nullopt;
      Snapshot snapshot{.observed_at_ms = *observed, .status = std::string(*status)};
      if (*status != "observed") return snapshot;
      const auto* quota = rt::getObject(object, "quota");
      if (!quota || quota->size() > MAX_BUCKETS) return std::nullopt;
      if (const auto plan = shortLabel(rt::get(object, "plan_tier")); plan) snapshot.plan_tier = plan;
      if (rt::hasOwn(object, "plan_tier") && !rt::get(object, "plan_tier")->is_null() && !snapshot.plan_tier) return std::nullopt;
      for (const auto& [id, raw] : *quota) {
        const json::value idValue(id);
        if (shortLabel(&idValue) != std::optional(id) || !raw.is_object()) return std::nullopt;
        snapshot.quota.set(id, bucket(raw.as_object()));
      }
      return snapshot;
    }

    json::value bucketJson(const Bucket& value) {
      json::object result;
      if (value.remaining_fraction) result["remaining_fraction"] = rt::makeNumber(*value.remaining_fraction);
      if (value.remaining_amount) result["remaining_amount"] = rt::makeNumber(*value.remaining_amount);
      if (value.reset_time) result["reset_time"] = *value.reset_time;
      if (value.reset_in_seconds) result["reset_in_seconds"] = rt::makeNumber(*value.reset_in_seconds);
      if (value.disabled) result["disabled"] = *value.disabled;
      return result;
    }

    json::value storedSnapshotJson(const Snapshot& snapshot) {
      json::object result{{"version", rt::makeNumber(1.0)}, {"observed_at_ms", rt::makeNumber(snapshot.observed_at_ms)}, {"status", snapshot.status}};
      if (snapshot.status == "observed") {
        result["plan_tier"] = snapshot.plan_tier ? json::value(*snapshot.plan_tier) : json::value(nullptr);
        json::object quota;
        for (const auto& [id, value] : snapshot.quota) quota[id] = bucketJson(value);
        result["quota"] = std::move(quota);
      }
      return result;
    }

    void requireDirectory(std::string_view directory) {
      const std::filesystem::path stateDirectory(directory);
      if (!stateDirectory.is_absolute() || stateDirectory.filename() != ".data") {
        throw rt::JsError("Gemini quota requires an absolute gateway state directory.");
      }
      const auto info = rt::fs::lstatSync(directory);
      if (!info.isDirectory() || info.isSymbolicLink() || info.uid != rt::process::getuid() || (info.mode & 022) != 0) {
        throw rt::JsError("Unsafe Gemini quota state directory.");
      }
    }

    std::string filePath(const std::string& directory) { return rt::path::join(directory, std::string(GEMINI_QUOTA_FILE)); }

    rt::Task<Snapshot> readSnapshotInternal(std::string directory, double nowMs) {
      requireDirectory(directory);
      int fd = -1;
      try {
        fd = rt::fs::openSync(filePath(directory), rt::fs::constants::O_RDONLY_ | rt::fs::constants::O_NOFOLLOW_ | O_NONBLOCK);
      } catch (const rt::fs::ErrnoError& error) {
        if (error.code == "ENOENT") co_return unavailable("missing");
        co_return unavailable("unavailable");
      }
      const auto close = rt::scopeExit([fd] { rt::fs::closeSync(fd); });
      const auto info = rt::fs::fstatSync(fd);
      if (!info.isFile() || info.nlink > 1 || info.uid != rt::process::getuid() || (info.mode & 07777) != 0600 ||
          info.size < 0 || static_cast<std::size_t>(info.size) > MAX_QUOTA_FILE_BYTES) {
        co_return unavailable("unavailable");
      }
      json::value value;
      try {
        value = rt::jsonParse(rt::fs::readFileSync(fd, "utf8"));
      } catch (...) {
        co_return unavailable("unavailable");
      }
      const auto snapshot = decodeSnapshot(value, nowMs);
      co_return snapshot.value_or(unavailable("unavailable"));
    }

    rt::Task<void> writeSnapshot(std::string directory, Snapshot snapshot) {
      requireDirectory(directory);
      const std::string path = filePath(directory);
      const std::string temporary = path + ".tmp-" + rt::crypto::randomUUID();
      const std::string body = rt::jsonStringify(storedSnapshotJson(snapshot)) + "\n";
      rt::fs::writeFileSync(temporary, body, {.flag = "wx", .mode = 0600});
      auto cleanup = rt::scopeExit([temporary] {
        if (rt::fs::existsSync(temporary)) {
          try { rt::fs::rmSync(temporary, {.force = true}); } catch (...) {}
        }
      });
      rt::fs::renameSync(temporary, path);
      cleanup.release();
      co_await rt::microtask();
    }

    rt::Task<Snapshot> refreshNow(std::string directory, GeminiQuotaOptions options, double nowMs) {
      const rt::AbortSignal deadline = rt::AbortSignal::timeout(GEMINI_QUOTA_REFRESH_DEADLINE_MS);
      const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any({*options.signal, deadline}) : deadline;
      provider::gemini::GeminiCliOptions command{
        .model = "gemini-3.8-flash-low", .prompt = "/usage", .signal = signal, .mode = "", .disableSlashCommands = false, .streamJson = false};
      auto task = provider::gemini::runGeminiCli(command, options.runner);
      auto turn = co_await std::move(task);
      auto snapshot = projectUsageCommand(turn.rawResult, nowMs);
      if (!snapshot) co_return unavailable("invalid_event");
      auto writing = writeSnapshot(directory, *snapshot);
      co_await std::move(writing);
      co_return *snapshot;
    }
  }

  std::optional<Snapshot> projectUsageCommand(const json::value& result, double observedAtMs) {
    if (!result.is_object()) return std::nullopt;
    const auto* command = rt::getObject(result, "command");
    if (!command || rt::getString(*command, "name") != std::optional<std::string_view>("usage")) return std::nullopt;
    const auto* data = rt::getObject(*command, "data");
    const auto* groups = data ? rt::getArray(*data, "groups") : nullptr;
    if (!groups) return std::nullopt;
    Snapshot snapshot{.observed_at_ms = observedAtMs, .status = "observed", .plan_tier = std::string("Antigravity subscription")};
    for (const auto& group : *groups) {
      const auto* buckets = rt::getArray(group, "buckets");
      if (!buckets) continue;
      for (const auto& value : *buckets) {
        const auto* raw = value.is_object() ? &value.as_object() : nullptr;
        if (!raw || snapshot.quota.size() >= MAX_BUCKETS) return std::nullopt;
        const auto id = shortLabel(rt::get(*raw, "id"));
        if (!id || snapshot.quota.has(*id)) continue;
        snapshot.quota.set(*id, bucket(*raw));
      }
    }
    return snapshot;
  }

  json::value snapshotJson(const Snapshot& snapshot, double nowMs) {
    json::object result{{"version", rt::makeNumber(1.0)}, {"observed_at_ms", rt::makeNumber(snapshot.observed_at_ms)}, {"status", snapshot.status},
      {"refresh_supported", true}};
    if (snapshot.status == "observed") {
      result["plan_tier"] = snapshot.plan_tier ? json::value(*snapshot.plan_tier) : json::value(nullptr);
      json::object quota;
      for (const auto& [id, value] : snapshot.quota) quota[id] = bucketJson(value);
      result["quota"] = std::move(quota);
    }
    if (snapshot.observed_at_ms > 0) result["age_ms"] = rt::makeNumber(rt::Math::max(0.0, nowMs - snapshot.observed_at_ms));
    if (snapshot.status == "observed") result["stale"] = nowMs - snapshot.observed_at_ms >= GEMINI_QUOTA_STALE_AFTER_MS;
    return result;
  }

  rt::Task<Snapshot> readSnapshot(std::string directory, double nowMs) { return readSnapshotInternal(std::move(directory), nowMs); }

  rt::Task<Snapshot> refreshSnapshot(std::string directory, GeminiQuotaOptions options, double nowMs) {
    RefreshGate& gate = refreshGate();
    if (gate.pending) {
      const auto pending = *gate.pending;
      co_return co_await pending.wait();
    }
    auto reading = readSnapshotInternal(directory, nowMs);
    auto current = co_await std::move(reading);
    if (!options.force_refresh && current.observed_at_ms > 0 && nowMs - current.observed_at_ms < GEMINI_QUOTA_REFRESH_FRESH_MS) {
      co_return current;
    }
    const double token = ++gate.token;
    auto refreshing = refreshNow(directory, options, nowMs);
    gate.pending = rt::Promise<Snapshot>(std::move(refreshing));
    const auto pending = *gate.pending;
    auto cleanup = rt::scopeExit([token] {
      if (refreshGate().token == token) refreshGate().pending.reset();
    });
    co_return co_await pending.wait();
  }

  rt::Task<rt::Response> handleAdminGeminiQuota() {
    if (!provider::gemini::geminiCliConfigured())
      co_return http::openaiError(404, "Gemini subscription controls are not enabled on this gateway.", "gemini_disabled");
    const double now = rt::dateNow();
    auto task = readSnapshot(rt::path::resolve(rt::process::cwd(), ".data"), now);
    const auto snapshot = co_await std::move(task);
    co_return http::json(200, snapshotJson(snapshot, now), rt::Headers{{"Cache-Control", "no-store"}});
  }

  rt::Task<rt::Response> handleAdminGeminiQuotaRefresh(rt::Request req) {
    static_cast<void>(req);
    return handleAdminGeminiQuotaRefresh(std::move(req), GeminiQuotaOptions{.force_refresh = true});
  }

  rt::Task<rt::Response> handleAdminGeminiQuotaRefresh(rt::Request req, GeminiQuotaOptions options) {
    if (!options.runner && !provider::gemini::geminiCliConfigured())
      co_return http::openaiError(404, "Gemini subscription controls are not enabled on this gateway.", "gemini_disabled");
    options.signal = openai::downstreamSignalFor(req);
    const double now = rt::dateNow();
    try {
      auto task = refreshSnapshot(rt::path::resolve(rt::process::cwd(), ".data"), options, now);
      const auto snapshot = co_await std::move(task);
      co_return http::json(200, snapshotJson(snapshot, rt::dateNow()), rt::Headers{{"Cache-Control", "no-store"}});
    } catch (const rt::JsError& error) {
      if (error.name == "AbortError") co_return http::openaiError(499, "Request was cancelled.", "request_cancelled");
      if (error.name == "TimeoutError") co_return http::openaiError(504, "Gemini quota refresh exceeded the gateway deadline.", "gateway_timeout");
      upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Antigravity quota refresh failed:", std::current_exception());
    } catch (...) {
      upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Antigravity quota refresh failed:", std::current_exception());
    }
    co_return http::openaiError(503, "Antigravity quota refresh is unavailable.", "gemini_quota_refresh_failed");
  }
}
