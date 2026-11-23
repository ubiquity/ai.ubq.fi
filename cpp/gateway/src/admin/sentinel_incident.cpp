#include "uos/admin/sentinel_incident.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace uos::admin::sentinel_incident {
  namespace {
    namespace incident_outbox = sentinel::incident_outbox;

    const rt::Regex& INDEX_CURSOR() {
      static const rt::Regex pattern("^[A-Za-z0-9_-]+={0,2}$", "");
      return pattern;
    }

    // std::nullopt is null.
    std::optional<double> validPageLimit(const std::optional<std::string>& value) {
      static const rt::Regex DIGITS("^\\d+$", "");
      if (!value) return incident_outbox::SENTINEL_INCIDENT_INDEX_DEFAULT_PAGE_LIMIT;
      if (!DIGITS.test(*value)) return std::nullopt;
      const double parsed = rt::stringToNumber(*value);
      return rt::Number::isSafeInteger(parsed) && parsed >= 1 && parsed <= incident_outbox::SENTINEL_INCIDENT_INDEX_MAX_PAGE_LIMIT ? std::optional<double>(parsed)
                                                                                                                                     : std::nullopt;
    }

    bool validCursor(const std::optional<std::string>& value) {
      return !value || (rt::length(*value) > 0 && rt::length(*value) <= 2'048 && INDEX_CURSOR().test(*value));
    }

    /**
     * Fixed canonical gateway identity for wire provenance. The frozen consumer
     * contract requires an absolute HTTP(S) URL here, while the internal index
     * keeps only the safe endpoint classification (a recognized path or "other")
     * and the private encrypted capture retains the detailed evidence. This
     * projection is an explicit fixed allowlist: a known path maps to its exact
     * gateway URL, and any unknown path is represented at the gateway origin.
     * The origin is a deliberate constant — stored row values, request URLs,
     * Host and forwarded headers can never influence the emitted URL, and
     * arbitrary stored data is never concatenated.
     */
    std::string wireProvenanceEndpoint(const std::string& endpoint) {
      if (endpoint == "/v1/responses") return "https://ai.ubq.fi/v1/responses";
      if (endpoint == "/v1/chat/completions") return "https://ai.ubq.fi/v1/chat/completions";
      return "https://ai.ubq.fi/";
    }

    /**
     * Fixed literal wire namespace for the restricted evidence artifact ref. The
     * frozen consumer only accepts opaque restricted storage refs
     * (`artifact://<namespace>/<incident_id>/<capture_id>`), never the internal
     * `capture:<id>` form. The namespace is a constant — no request URL, Host or
     * stored value can influence it, and no arbitrary scheme or external
     * reference is ever emitted. The capture id is sliced from the internal ref
     * only after the index row reader has validated the exact `capture:` prefix;
     * the incident id is the validated frozen provider-UUID from the same row.
     */
    constexpr std::string_view WIRE_EVIDENCE_REF_NAMESPACE = "sentinel";
    constexpr std::string_view WIRE_EVIDENCE_REF_CAPTURE_PREFIX = "capture:";

    json::value wireEvidenceRef(const incident_outbox::SentinelIncidentIndexEvidenceRef& evidence, const std::string& incidentId) {
      if (!rt::startsWith(evidence.ref, WIRE_EVIDENCE_REF_CAPTURE_PREFIX)) {
        // The row reader validates `capture:<id>` refs before this projection; an
        // unexpected ref is an internal invariant violation and fails closed.
        throw rt::JsError("Sentinel index evidence ref is not a validated capture ref");
      }
      const std::string captureId = rt::slice(evidence.ref, static_cast<double>(rt::length(WIRE_EVIDENCE_REF_CAPTURE_PREFIX)));
      json::object ref;
      ref["ref"] = "artifact://" + std::string(WIRE_EVIDENCE_REF_NAMESPACE) + "/" + incidentId + "/" + captureId;
      ref["digest"] = evidence.digest ? json::value(*evidence.digest) : json::value(nullptr);
      return ref;
    }

    json::value numberOrNull(const std::optional<double>& value) { return value ? json::value(*value) : json::value(nullptr); }

    /** Exact snake_case wire row; internal keys/metadata are never exposed. */
    json::value rowToWire(const incident_outbox::SentinelIncidentIndexRow& row) {
      json::array sample;
      for (const std::string& line : row.context.sample) sample.emplace_back(line);
      json::object context;
      context["message"] = row.context.message;
      context["location"] = nullptr;
      context["sample"] = std::move(sample);
      json::object provenance;
      provenance["endpoint"] = wireProvenanceEndpoint(row.provenance.endpoint);
      provenance["captured_at_ms"] = row.provenance.captured_at_ms;
      provenance["captured_by"] = nullptr;
      json::object wire;
      wire["incident_id"] = row.incident_id;
      wire["fingerprint"] = row.fingerprint;
      wire["severity"] = row.severity;
      wire["first_seen_at_ms"] = row.first_seen_at_ms;
      wire["last_seen_at_ms"] = row.last_seen_at_ms;
      wire["count"] = row.count;
      wire["failing_revision"] = row.failing_revision ? json::value(*row.failing_revision) : json::value(nullptr);
      wire["error_type"] = row.error_type;
      wire["context"] = std::move(context);
      wire["provenance"] = std::move(provenance);
      wire["evidence_ref"] = row.evidence_ref ? wireEvidenceRef(*row.evidence_ref, row.incident_id) : json::value(nullptr);
      wire["evidence_expires_at_ms"] = numberOrNull(row.evidence_expires_at_ms);
      return rt::canonicalNumbers(json::value(std::move(wire)));
    }

    rt::Response unavailable() {
      return http::openaiError(503, "Sentinel incident index is unavailable", std::string("sentinel_incidents_unavailable"));
    }
  }

  rt::Task<rt::Response> handleAdminSentinelIncidents(rt::Request req) {
    auto handling = handleAdminSentinelIncidents(std::move(req), SentinelIncidentAdminDependencies{});
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<rt::Response> handleAdminSentinelIncidents(rt::Request req, SentinelIncidentAdminDependencies dependencies) {
    const rt::URL url(req.url());
    const rt::URLSearchParams& params = url.searchParams();
    const std::optional<double> limit = validPageLimit(params.get("limit"));
    const std::optional<std::string> cursor = params.get("cursor");
    const std::optional<std::string> rawIncidentId = params.get("incident_id");
    const std::optional<std::string> incidentId = rawIncidentId;
    // Frozen single-value semantics: a repeated query key is never resolved by
    // arbitrary first-value coercion; explicit empty values are malformed, not
    // silently defaulted or trimmed (optional means the key is absent).
    const std::vector<std::string> keys = params.keys();
    const bool unknownKeys =
      std::any_of(keys.begin(), keys.end(), [](const std::string& key) { return key != "limit" && key != "cursor" && key != "incident_id"; });
    const bool duplicatedKeys = std::any_of(keys.begin(), keys.end(), [&params](const std::string& key) { return params.getAll(key).size() != 1; });
    if (!limit || !validCursor(cursor) || unknownKeys || duplicatedKeys || (incidentId && !incident_outbox::isSentinelIncidentId(rt::KvValue(*incidentId)))) {
      co_return http::openaiError(400, "limit must be an integer from 1 to 100, cursor, and incident_id must be valid, and unknown query keys are rejected",
                                  std::string("invalid_request_error"));
    }
    std::optional<rt::Response> response;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> pending = dependencies.getKv ? dependencies.getKv() : kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
      if (!kv) {
        response = unavailable();
      } else {
        incident_outbox::SentinelIncidentIndexListOptions options;
        options.incidentId = incidentId;
        options.limit = *limit;
        // `validCursor` above already rejects an empty cursor, so `cursor` is
        // either null or a non-empty string here.
        options.cursor = cursor;
        auto listing = dependencies.listSentinelIncidentIndexRows ? dependencies.listSentinelIncidentIndexRows(*kv, std::move(options))
                                                                  : incident_outbox::listSentinelIncidentIndexRows(*kv, std::move(options));
        const incident_outbox::SentinelIncidentIndexPage page = co_await std::move(listing);
        json::array data;
        for (const incident_outbox::SentinelIncidentIndexRow& row : page.rows) data.push_back(rowToWire(row));
        json::object coverage;
        coverage["status"] = "complete";
        json::object body;
        body["data"] = std::move(data);
        // The index reader already normalizes an empty iterator cursor to null.
        body["cursor"] = page.cursor ? json::value(*page.cursor) : json::value(nullptr);
        // Every successful page read is complete; incomplete is reserved for a
        // genuine source gap and never for ordinary pagination continuation.
        body["coverage"] = std::move(coverage);
        response = http::json(200, json::value(std::move(body)), rt::Headers{{"Cache-Control", "no-store"}});
      }
    } catch (...) {
      response = unavailable();
    }
    co_return *response;
  }
}
