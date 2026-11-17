#include "uos/sentinel/replay_admin.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_capture.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace uos::sentinel::replay_admin {
  namespace {
    using replay_model::SentinelReplayCaptureStatusRow;

    // `nonNegativeInteger(value: string | null, fallback)`: std::nullopt is null.
    std::optional<double> nonNegativeInteger(const std::optional<std::string>& value, double fallback) {
      if (!value || value->empty()) return fallback;
      const double parsed = rt::stringToNumber(*value);
      return rt::Number::isSafeInteger(parsed) && parsed >= 0 ? std::optional<double>(parsed) : std::nullopt;
    }

    // `/^[A-Za-z0-9_-]+={0,2}$/`
    const rt::Regex& CURSOR() {
      static const rt::Regex pattern("^[A-Za-z0-9_-]+={0,2}$", "");
      return pattern;
    }

    bool validCursor(const std::optional<std::string>& value) { return !value || value->empty() || (rt::length(*value) <= 2'048 && CURSOR().test(*value)); }

    /** One shared invalid-input response for every replay-capture lookup mode. */
    rt::Response invalidReplayExportRequest() {
      return http::openaiError(
        400,
        "after_ms and before_ms must define a valid interval, limit must be one, cursor must be valid, and at most one of incident_id or request_id may be selected",
        std::string("invalid_request_error"));
    }

    // JSON.stringify of one key part of a status row's `manifest_key`.
    json::value keyPartJson(const rt::KvKeyPart& part) {
      if (part.isString()) return json::value(json::string(part.string()));
      if (part.isNumber()) return rt::Number::isFinite(part.number()) ? rt::makeNumber(part.number()) : json::value(nullptr);
      if (part.isBool()) return json::value(part.boolean());
      if (part.isBigInt()) throw rt::TypeError("JSON.stringify cannot serialize BigInt.");
      json::object bytes;
      const rt::KvBytes& data = part.bytes();
      for (std::size_t index = 0; index < data.size(); ++index) bytes[std::to_string(index)] = rt::makeNumber(static_cast<std::int64_t>(data[index]));
      return json::value(std::move(bytes));
    }

    // JSON.stringify of the capture status row the request-id lookup answers with.
    json::value statusRowJson(const SentinelReplayCaptureStatusRow& row) {
      json::object out;
      out["version"] = rt::makeNumber(row.version);
      out["request_id"] = json::string(row.request_id);
      out["status"] = json::string(row.status);
      out["reason"] = row.reason ? json::value(json::string(*row.reason)) : json::value(nullptr);
      out["captured_at_ms"] = rt::makeNumber(row.captured_at_ms);
      if (row.manifest_key) {
        json::array key;
        for (const rt::KvKeyPart& part : *row.manifest_key) key.push_back(keyPartJson(part));
        out["manifest_key"] = std::move(key);
      } else {
        out["manifest_key"] = nullptr;
      }
      out["fingerprint"] = row.fingerprint ? json::value(json::string(*row.fingerprint)) : json::value(nullptr);
      out["expires_at_ms"] = row.expires_at_ms ? rt::makeNumber(*row.expires_at_ms) : json::value(nullptr);
      return json::value(std::move(out));
    }

    rt::Headers noStore() { return rt::Headers{{"Cache-Control", "no-store"}}; }

    // `url.searchParams.get(name)?.trim() ?? ""`, then "" as null.
    std::optional<std::string> trimmedParameter(const rt::URLSearchParams& params, const std::string& name) {
      const std::optional<std::string> raw = params.get(name);
      const std::string trimmed = raw ? rt::trim(*raw) : std::string();
      return trimmed.empty() ? std::nullopt : std::optional<std::string>(trimmed);
    }

    // `handleAdminSentinelReplayCaptures(req, dependencies = {})`
    rt::Task<rt::Response> handleCaptures(rt::Request req, SentinelReplayAdminDependencies dependencies) {
      const rt::URL url(req.url());
      const rt::URLSearchParams& params = url.searchParams();
      // A missing parameter and one that trims to "" both mean "absent" here: `??`
      // would forward the empty value, so the emptiness is spelled out.
      const std::optional<std::string> incidentId = trimmedParameter(params, "incident_id");
      const std::optional<std::string> requestId = trimmedParameter(params, "request_id");
      double afterMs = 0;
      double beforeMs = 0;
      double limit = 0;
      std::optional<std::string> cursor;
      if (requestId) {
        // A request-id lookup is a point lookup: it neither needs nor consumes a
        // time interval, so its unused listing parameters are fixed to safe values.
        // The request-id shape and the mutually exclusive mode selection are still
        // validated, and every listing mode parses its parameters exactly as before.
        if (!replay_capture::isSentinelReplayRequestId(rt::KvValue(*requestId)) || incidentId) co_return invalidReplayExportRequest();
        afterMs = 0;
        beforeMs = 0;
        limit = replay_capture::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT;
        cursor = std::nullopt;
      } else {
        const std::optional<double> parsedAfterMs = nonNegativeInteger(params.get("after_ms"), 0);
        const std::optional<double> parsedBeforeMs = nonNegativeInteger(params.get("before_ms"), -1);
        const std::optional<double> parsedLimit = nonNegativeInteger(params.get("limit"), replay_capture::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT);
        const std::optional<std::string> parsedCursor = params.get("cursor");
        if (!parsedAfterMs || !parsedBeforeMs || *parsedBeforeMs < *parsedAfterMs || !parsedLimit ||
            *parsedLimit != replay_capture::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT || !validCursor(parsedCursor) ||
            (incidentId && !incident_outbox::isSentinelIncidentId(rt::KvValue(*incidentId)))) {
          co_return invalidReplayExportRequest();
        }
        afterMs = *parsedAfterMs;
        beforeMs = *parsedBeforeMs;
        limit = *parsedLimit;
        cursor = parsedCursor;
      }
      // An empty cursor is absent as well, so it must never be forwarded upstream.
      const std::optional<std::string> requestCursor = !cursor || cursor->empty() ? std::nullopt : cursor;
      std::optional<rt::Response> response;
      try {
        rt::Promise<std::shared_ptr<rt::Kv>> pending = dependencies.getKv ? dependencies.getKv() : ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
        if (!kv) {
          response = http::openaiError(503, "Sentinel replay storage is unavailable", std::string("sentinel_replay_storage_unavailable"));
        } else if (requestId) {
          // A request-id lookup always answers with the capture status for that
          // request, so a disabled, failed or expired capture is distinguishable
          // from a request that never produced one.
          auto listing = dependencies.listEncryptedSentinelReplaysByRequestId ? dependencies.listEncryptedSentinelReplaysByRequestId(*kv, *requestId)
                                                                              : replay_capture::listEncryptedSentinelReplaysByRequestId(*kv, *requestId);
          const replay_read::EncryptedSentinelRequestReplays result = co_await std::move(listing);
          json::object body;
          body["data"] = json::value_from(result.captures);
          body["cursor"] = nullptr;
          body["capture_status"] = statusRowJson(result.status);
          response = http::json(200, json::value(std::move(body)), noStore());
        } else {
          std::optional<replay_read::EncryptedSentinelReplayPage> page;
          if (incidentId) {
            replay_read::ListEncryptedSentinelIncidentReplaysOptions options;
            options.incidentId = *incidentId;
            options.limit = limit;
            options.cursor = requestCursor;
            auto listing = dependencies.listEncryptedSentinelIncidentReplays ? dependencies.listEncryptedSentinelIncidentReplays(*kv, std::move(options))
                                                                             : replay_capture::listEncryptedSentinelIncidentReplays(*kv, std::move(options));
            page = co_await std::move(listing);
          } else {
            replay_read::ListEncryptedSentinelReplaysOptions options{.afterMs = afterMs, .beforeMs = beforeMs};
            options.limit = limit;
            options.cursor = requestCursor;
            auto listing = dependencies.listEncryptedSentinelReplays ? dependencies.listEncryptedSentinelReplays(*kv, std::move(options))
                                                                     : replay_capture::listEncryptedSentinelReplays(*kv, std::move(options));
            page = co_await std::move(listing);
          }
          json::object body;
          body["data"] = json::value_from(page->captures);
          body["cursor"] = page->cursor.empty() ? json::value(nullptr) : json::value(json::string(page->cursor));
          response = http::json(200, json::value(std::move(body)), noStore());
        }
      } catch (...) {
        response = http::openaiError(503, "Sentinel replay export failed", std::string("sentinel_replay_export_failed"));
      }
      co_return *response;
    }
  }

  rt::Task<rt::Response> handleAdminSentinelReplayCaptures(rt::Request req) { return handleCaptures(std::move(req), SentinelReplayAdminDependencies{}); }

  rt::Task<rt::Response> handleAdminSentinelReplayCaptures(rt::Request req, const SentinelReplayAdminDependencies& dependencies) {
    return handleCaptures(std::move(req), dependencies);
  }
}
