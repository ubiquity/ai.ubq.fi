// Port of src/agent-messages.ts.
#include "uos/agent_messages.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"

#include <utility>
#include <vector>

namespace uos::agent_messages {
  namespace {
    constexpr double DEFAULT_LIMIT = 50;
    constexpr double MAX_LIMIT = 200;
    constexpr double MAX_AGENT_ID_LENGTH = 120;
    constexpr double MAX_CHANNEL_LENGTH = 120;
    constexpr double MAX_KIND_LENGTH = 120;
    constexpr double MAX_BODY_LENGTH = 8000;
    constexpr double MAX_METADATA_LENGTH = 8000;

    struct AgentMessageRecord {
      std::string id;
      std::string owner;
      std::string repo;
      std::string state_id;
      std::string agent_id;
      std::optional<std::string> channel;
      std::optional<std::string> kind;
      std::string body;
      // `Record<string, unknown> | null`; utils.isRecord also admits an array, which is stored as given.
      std::optional<json::value> metadata;
      double created_at_ms;
    };

    json::object agentMessageRecordJson(const AgentMessageRecord& record) {
      json::object value;
      value["id"] = record.id;
      value["owner"] = record.owner;
      value["repo"] = record.repo;
      value["state_id"] = record.state_id;
      value["agent_id"] = record.agent_id;
      value["channel"] = record.channel ? json::value(*record.channel) : json::value(nullptr);
      value["kind"] = record.kind ? json::value(*record.kind) : json::value(nullptr);
      value["body"] = record.body;
      value["metadata"] = record.metadata ? *record.metadata : json::value(nullptr);
      value["created_at_ms"] = rt::makeNumber(record.created_at_ms);
      return value;
    }

    bool hasLineBreak(const std::string& value) {
      static const rt::Regex LINE_BREAK("[\\r\\n]", "");
      return LINE_BREAK.test(value);
    }

    // `value: unknown`: nullptr is undefined. std::nullopt is null.
    std::optional<std::string> normalizeAgentId(const json::value* value) {
      const std::optional<std::string> raw = utils::getString(value);
      if (!rt::truthy(raw)) return std::nullopt;
      const std::string trimmed = rt::trim(*raw);
      if (trimmed.empty()) return std::nullopt;
      if (static_cast<double>(rt::length(trimmed)) > MAX_AGENT_ID_LENGTH) return std::nullopt;
      if (hasLineBreak(trimmed)) return std::nullopt;
      return trimmed;
    }

    // `value: unknown`: nullptr is undefined. std::nullopt is null.
    std::optional<std::string> normalizeOptionalTag(const json::value* value, double maxLength) {
      if (rt::isNullish(value)) return std::nullopt;
      const std::optional<std::string> raw = utils::getString(value);
      if (!rt::truthy(raw)) return std::nullopt;
      const std::string trimmed = rt::trim(*raw);
      if (trimmed.empty()) return std::nullopt;
      if (static_cast<double>(rt::length(trimmed)) > maxLength) return std::nullopt;
      if (hasLineBreak(trimmed)) return std::nullopt;
      return trimmed;
    }

    // A search parameter (`string | null`).
    std::optional<std::string> normalizeOptionalTag(const std::optional<std::string>& value, double maxLength) {
      if (!value) return std::nullopt;
      const json::value text(*value);
      return normalizeOptionalTag(&text, maxLength);
    }

    // `value: unknown`: nullptr is undefined. std::nullopt is null.
    std::optional<std::string> normalizeBody(const json::value* value) {
      const std::optional<std::string> raw = utils::getString(value);
      if (!raw) return std::nullopt;
      const std::string trimmed = rt::trim(*raw);
      if (trimmed.empty()) return std::nullopt;
      if (static_cast<double>(rt::length(*raw)) > MAX_BODY_LENGTH) return std::nullopt;
      return raw;
    }

    // `{ ok: true; value: Record<string, unknown> | null } | { ok: false }`
    struct NormalizedMetadata {
      bool ok;
      std::optional<json::value> value{};
    };

    // `value: unknown`: nullptr is undefined.
    NormalizedMetadata normalizeMetadata(const json::value* value) {
      if (rt::isNullish(value)) return {.ok = true, .value = std::nullopt};
      if (!utils::isRecord(value)) return {.ok = false};
      try {
        const std::string encoded = rt::jsonStringify(*value);
        if (static_cast<double>(rt::length(encoded)) > MAX_METADATA_LENGTH) return {.ok = false};
      } catch (const rt::JsError&) {
        return {.ok = false};
      }
      return {.ok = true, .value = *value};
    }

    // std::nullopt is null.
    std::optional<double> parsePositiveInt(const std::optional<std::string>& value) {
      if (!rt::truthy(value)) return std::nullopt;
      const double parsed = rt::parseInt(*value, 10);
      if (!rt::Number::isFinite(parsed)) return std::nullopt;
      if (parsed <= 0) return std::nullopt;
      return rt::Math::trunc(parsed);
    }

    struct AgentMessagesListQuery {
      std::optional<double> since;
      double limit;
      double scanLimit;
      std::string cursor;
      std::optional<std::string> channel;
      std::optional<std::string> agentId;
    };

    /** Parses the bounded list parameters; the scan over-fetches only when a filter can discard entries. */
    AgentMessagesListQuery agentMessagesListQuery(rt::URL& url) {
      const double limit = rt::Math::min(rt::Math::max(parsePositiveInt(url.searchParams().get("limit")).value_or(DEFAULT_LIMIT), 1.0), MAX_LIMIT);
      std::optional<std::string> channel = normalizeOptionalTag(url.searchParams().get("channel"), MAX_CHANNEL_LENGTH);
      std::optional<std::string> agentId = normalizeOptionalTag(url.searchParams().get("agent_id"), MAX_AGENT_ID_LENGTH);
      const std::optional<std::string> cursor = url.searchParams().get("cursor");
      return {
        .since = parsePositiveInt(url.searchParams().get("since")),
        .limit = limit,
        .scanLimit = rt::truthy(channel) || rt::truthy(agentId) ? rt::Math::min(limit * 2, MAX_LIMIT) : limit,
        .cursor = cursor ? rt::trim(*cursor) : std::string(),
        .channel = std::move(channel),
        .agentId = std::move(agentId),
      };
    }

    // `a !== b` with `b` a string.
    bool strictNotEquals(const json::value* left, const std::string& right) { return !(left && left->is_string() && left->as_string() == right); }

    /** Whether one stored message satisfies every list filter. */
    // `message` is the stored entry value as JavaScript reads its members (rt::storedJsonView).
    bool messageMatchesListQuery(const json::value& message, const AgentMessagesListQuery& query) {
      if (query.since && rt::toNumber(rt::get(message, "created_at_ms")) < *query.since) return false;
      if (rt::truthy(query.agentId) && strictNotEquals(rt::get(message, "agent_id"), *query.agentId)) return false;
      if (rt::truthy(query.channel) && strictNotEquals(rt::get(message, "channel"), *query.channel)) return false;
      return true;
    }

    rt::Task<auth::index::AuthenticateClientResult> authenticate(const AgentMessagesDeps& deps, rt::Request req) {
      if (deps.authenticateClient) return deps.authenticateClient(std::move(req));
      return auth::index::authenticateClient(std::move(req));
    }
  }

  rt::Task<rt::Response> handleAgentMessagesPost(rt::Request req) {
    auto task = handleAgentMessagesPost(std::move(req), AgentMessagesDeps());
    rt::Response response = co_await std::move(task);
    co_return response;
  }

  rt::Task<rt::Response> handleAgentMessagesPost(rt::Request req, AgentMessagesDeps deps) {
    auto authentication = authenticate(deps, req);
    const auth::index::AuthenticateClientResult authResult = co_await std::move(authentication);
    if (const auto* refusal = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) co_return refusal->response;
    const auto* github = std::get_if<auth::index::ClientAuthMethodGithubToken>(&std::get<auth::index::AuthenticateClientResultOk>(authResult).method);
    if (!github) {
      co_return http::openaiError(403, "GitHub token auth required", "forbidden");
    }

    std::shared_ptr<rt::Kv> kv;
    if (deps.kv) {
      kv = *deps.kv;
    } else {
      rt::Promise<std::shared_ptr<rt::Kv>> kvPromise = kv::getKv();
      kv = co_await kvPromise.wait();
    }
    if (!kv) co_return http::openaiError(503, "Deno KV unavailable", "server_error");

    const json::value raw = co_await request::readJsonBody(req);
    if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");

    const std::optional<std::string> agentId = normalizeAgentId(rt::get(raw, "agent_id"));
    if (!rt::truthy(agentId)) co_return http::openaiError(400, "agent_id must be a non-empty string (<=120 chars)", "invalid_request_error");

    const std::optional<std::string> body = normalizeBody(rt::get(raw, "body"));
    if (!rt::truthy(body)) co_return http::openaiError(400, "body must be a non-empty string (<=8000 chars)", "invalid_request_error");

    const std::optional<std::string> channel = normalizeOptionalTag(rt::get(raw, "channel"), MAX_CHANNEL_LENGTH);
    if (!rt::isNullish(rt::get(raw, "channel")) && !rt::truthy(channel)) {
      co_return http::openaiError(400, "channel must be a string (<=120 chars)", "invalid_request_error");
    }

    const std::optional<std::string> kind = normalizeOptionalTag(rt::get(raw, "kind"), MAX_KIND_LENGTH);
    if (!rt::isNullish(rt::get(raw, "kind")) && !rt::truthy(kind)) {
      co_return http::openaiError(400, "kind must be a string (<=120 chars)", "invalid_request_error");
    }

    const NormalizedMetadata metadataResult = normalizeMetadata(rt::get(raw, "metadata"));
    if (!metadataResult.ok) {
      co_return http::openaiError(400, "metadata must be a JSON object (<=8000 chars)", "invalid_request_error");
    }

    const double createdAtMs = deps.now ? deps.now() : rt::dateNow();
    const std::string id = deps.uuid ? deps.uuid() : rt::crypto::randomUUID();
    const AgentMessageRecord record{
      .id = id,
      .owner = github->owner,
      .repo = github->repo,
      .state_id = github->state_id,
      .agent_id = *agentId,
      .channel = channel,
      .kind = kind,
      .body = *body,
      .metadata = metadataResult.value,
      .created_at_ms = createdAtMs,
    };
    const json::object recordJson = agentMessageRecordJson(record);

    const rt::KvKey key{"agent_messages", record.owner, record.repo, record.state_id, createdAtMs, id};
    const rt::KvValue value{json::value(recordJson)};
    co_await kv->set(key, value);

    json::object payload;
    payload["message"] = recordJson;
    co_return http::json(200, json::value(std::move(payload)), rt::Headers{{"Cache-Control", "no-store"}});
  }

  rt::Task<rt::Response> handleAgentMessagesList(rt::Request req) {
    auto task = handleAgentMessagesList(std::move(req), AgentMessagesDeps());
    rt::Response response = co_await std::move(task);
    co_return response;
  }

  rt::Task<rt::Response> handleAgentMessagesList(rt::Request req, AgentMessagesDeps deps) {
    auto authentication = authenticate(deps, req);
    const auth::index::AuthenticateClientResult authResult = co_await std::move(authentication);
    if (const auto* refusal = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) co_return refusal->response;
    const auto* github = std::get_if<auth::index::ClientAuthMethodGithubToken>(&std::get<auth::index::AuthenticateClientResultOk>(authResult).method);
    if (!github) {
      co_return http::openaiError(403, "GitHub token auth required", "forbidden");
    }

    std::shared_ptr<rt::Kv> kv;
    if (deps.kv) {
      kv = *deps.kv;
    } else {
      rt::Promise<std::shared_ptr<rt::Kv>> kvPromise = kv::getKv();
      kv = co_await kvPromise.wait();
    }
    if (!kv) co_return http::openaiError(503, "Deno KV unavailable", "server_error");

    rt::URL url(req.url());
    const AgentMessagesListQuery query = agentMessagesListQuery(url);

    const rt::KvKey prefix{"agent_messages", github->owner, github->repo, github->state_id};

    rt::KvListOptions options;
    options.limit = query.scanLimit;
    if (!query.cursor.empty()) {
      options.cursor = query.cursor;
    }

    rt::KvListSelector selector;
    selector.prefix = prefix;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(std::move(selector), std::move(options));
    json::array messages;

    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      if (!messageMatchesListQuery(rt::storedJsonView(entry->value), query)) continue;
      // The stored value as JSON.stringify writes it (an undefined value is written as null).
      const std::optional<std::string> encoded = rt::kvJsonStringify(entry->value);
      messages.push_back(encoded ? rt::jsonParse(*encoded) : json::value(nullptr));
      if (static_cast<double>(messages.size()) >= query.limit) {
        co_await iterator->return_();
        break;
      }
    }

    json::value nextSince = nullptr;
    if (!messages.empty()) {
      const json::value* createdAtMs = rt::get(messages.back(), "created_at_ms");
      if (!rt::isNullish(createdAtMs)) nextSince = *createdAtMs;
    }
    const std::string cursor = iterator->cursor();
    const std::optional<std::string> nextCursor = !cursor.empty() && rt::length(cursor) > 0 ? std::optional(cursor) : std::nullopt;

    json::object payload;
    payload["messages"] = std::move(messages);
    payload["next_since"] = nextSince;
    payload["next_cursor"] = nextCursor ? json::value(*nextCursor) : json::value(nullptr);
    payload["has_more"] = rt::truthy(nextCursor);
    co_return http::json(200, json::value(std::move(payload)), rt::Headers{{"Cache-Control", "no-store"}});
  }
}
