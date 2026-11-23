// Port of src/admin/api-key-audit.ts.
#include "uos/admin/api_key_audit.hpp"
#include "uos/config.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

namespace uos::admin::api_key_audit {
  namespace {
    namespace audit = authority::native_api_key_audit;
    rt::Task<ApiKeyChangePage> list(std::shared_ptr<rt::Kv> kv, ListApiKeyChangeEventsInput input) {
      const auto oldestRetainedAtMs = rt::dateNow() - API_KEY_CHANGE_RETENTION_MS;
      rt::KvListSelector selector;
      selector.prefix = API_KEY_CHANGE_PREFIX;
      rt::KvListOptions options;
      options.limit = input.limit;
      if (rt::truthy(input.cursor)) options.cursor = input.cursor;
      auto iterator = kv->list(selector, options);
      std::vector<json::value> events;
      double scanned = 0;
      while (auto entry = co_await iterator->next()) {
        scanned += 1;
        if (audit::isRetainedApiKeyChangeEvent(entry->value, oldestRetainedAtMs)) {
          const auto encoded = rt::kvJsonStringify(entry->value);
          events.push_back(encoded ? rt::jsonParse(*encoded) : json::value(nullptr));
        }
        if (scanned >= input.limit) { co_await iterator->return_(); break; }
      }
      const auto cursor = iterator->cursor();
      co_return ApiKeyChangePage{events, scanned < input.limit || cursor.empty() ? std::nullopt : std::optional(cursor)};
    }
    rt::Task<rt::Response> handle(rt::Request req, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      std::shared_ptr<rt::Kv> kv;
      if (kvOverride) kv = *kvOverride;
      else { auto pending = kv::getKv(); kv = co_await pending.wait(); }
      if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot load key change history", "server_error");
      rt::URL url(req.url());
      const auto limit = audit::parseApiKeyChangeLimit(url.searchParams().get("limit"));
      if (!limit) co_return http::openaiError(400, "limit must be a positive integer", "invalid_request_error");
      const auto rawCursor = url.searchParams().get("cursor");
      const auto cursor = rawCursor ? std::optional(rt::trim(*rawCursor)) : std::nullopt;
      if (cursor && (cursor->empty() || rt::length(*cursor) > 1024)) co_return http::openaiError(400, "cursor is invalid", "invalid_request_error");
      try {
        const ListApiKeyChangeEventsInput input{*limit, cursor};
        auto task = listApiKeyChangeEvents(kv, input);
        rt::Promise<ApiKeyChangePage> pending(std::move(task));
        const auto page = co_await pending.wait();
        json::array data;
        for (const auto& event : page.events) data.push_back(event);
        json::object body{{"object", "list"}, {"retention_days", rt::makeNumber(API_KEY_CHANGE_RETENTION_DAYS)}, {"data", std::move(data)}, {"next_cursor", page.next_cursor ? json::value(*page.next_cursor) : json::value(nullptr)}};
        co_return http::json(200, body, rt::Headers{{"Cache-Control", "no-store"}});
      } catch (const rt::TypeError&) { co_return http::openaiError(400, "cursor is invalid", "invalid_request_error"); }
      catch (...) {
        rt::console::error("[ai.ubq.fi] Failed to load API key change history:", rt::console::Value(std::current_exception()));
        co_return http::openaiError(500, "Failed to load key change history", "server_error");
      }
    }
  }
  ApiKeyChangeEvent buildApiKeyChangeEvent(const ApiKeyChangeEventInput& input) {
    const audit::ApiKeyAuditSources sources{[] { return rt::crypto::randomUUID(); }, [] { return rt::dateNow(); }, [] { return config::runtimeGitSha(); }};
    return audit::buildApiKeyChangeEvent(input, sources);
  }
  rt::KvKey apiKeyChangeEventKey(const ApiKeyChangeEvent& event) { return audit::apiKeyChangeEventKey(event); }
  rt::AtomicOperation& appendApiKeyChangeEvent(rt::AtomicOperation& atomic, const ApiKeyChangeEvent& event) {
    const auto key = audit::apiKeyChangeEventKey(event);
    const rt::KvValue value(json::value_from(event));
    return atomic.set(key, value, rt::KvSetOptions{.expireIn = API_KEY_CHANGE_RETENTION_MS});
  }
  rt::Task<ApiKeyChangePage> listApiKeyChangeEvents(std::shared_ptr<rt::Kv> kv, const ListApiKeyChangeEventsInput& input) { return list(std::move(kv), input); }
  rt::Task<rt::Response> handleAdminApiKeyChanges(rt::Request req) { return handle(std::move(req), std::nullopt); }
  rt::Task<rt::Response> handleAdminApiKeyChanges(rt::Request req, std::shared_ptr<rt::Kv> kvOverride) { return handle(std::move(req), std::move(kvOverride)); }
}
