#include "uos/http.hpp"

#include "uos/auth/relay.hpp"
#include "uos/config.hpp"
#include "uos/provider/cerebras_rate_limits.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <string>
#include <utility>
#include <vector>

namespace uos::http {
  namespace {
    const std::vector<std::string_view>& EXPOSED_RESPONSE_HEADERS() {
      static const std::vector<std::string_view> headers = [] {
        std::vector<std::string_view> names{
          "x-uos-warning",
          "x-uos-request-id",
          "x-uos-provider-request-id",
          "x-uos-upstream",
          "x-uos-attempted-providers",
          "x-uos-router-revision",
          "x-uos-git-sha",
          "x-uos-deployment-id",
          "x-uos-cache",
          "ETag",
          "Retry-After",
        };
        names.insert(names.end(), STANDARD_RATE_LIMIT_HEADERS.begin(), STANDARD_RATE_LIMIT_HEADERS.end());
        names.insert(names.end(), provider::cerebras_rate_limits::CEREBRAS_RATE_LIMIT_HEADERS.begin(),
                     provider::cerebras_rate_limits::CEREBRAS_RATE_LIMIT_HEADERS.end());
        names.push_back("x-codex-limit-name");
        names.push_back("x-codex-primary-used-percent");
        return names;
      }();
      return headers;
    }

    std::string joinNames(const std::vector<std::string_view>& names, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < names.size(); ++index) {
        if (index) out += separator;
        out += names[index];
      }
      return out;
    }

    std::optional<std::string> getRequestOrigin(const std::optional<rt::Request>& req) {
      const std::optional<std::string> headerOrigin = req ? req->headers().get("origin") : std::nullopt;
      if (headerOrigin) return auth::relay::parseTrustedAuthRelayOrigin(headerOrigin);
      try {
        const rt::URL url(req ? req->url() : std::string());
        return auth::relay::parseTrustedAuthRelayOrigin(url.searchParams().get("cors_origin"));
      } catch (...) {
        return std::nullopt;
      }
    }

    bool isAdminPath(const std::optional<rt::Request>& req) {
      if (!req) return false;
      try {
        const std::string path = rt::URL(req->url()).pathname();
        return path == "/admin" || path == "/admin.html" || path.starts_with("/admin/");
      } catch (...) {
        return false;
      }
    }
  }

  rt::Headers corsHeaders(std::optional<rt::Request> req) {
    const std::optional<std::string> requestOrigin = getRequestOrigin(req);
    const std::string& configuredOrigin = config::config.allowOrigin;
    // Mirrors the previous `canUseCredentials ? requestOrigin : configuredOrigin`:
    // a falsy requestOrigin (empty string) fell back to configuredOrigin there too.
    const std::optional<std::string> credentialsOrigin =
      rt::truthy(requestOrigin) && (configuredOrigin == "*" || configuredOrigin == *requestOrigin) ? requestOrigin : std::nullopt;
    rt::Headers headers;
    headers.append("Access-Control-Allow-Origin", credentialsOrigin.value_or(configuredOrigin));
    headers.append("Access-Control-Allow-Methods", "GET,POST,PUT,PATCH,DELETE,HEAD,OPTIONS");
    headers.append("Access-Control-Allow-Headers",
                   "Authorization,Content-Type,Idempotency-Key,If-None-Match,OpenAI-Beta,OpenAI-Organization,OpenAI-Project,X-GitHub-Owner,X-GitHub-Repo,X-GitHub-"
                   "Installation-Id,X-Ubiquity-Kernel-Token");
    // Allow browser clients to read quota state, gateway warnings, cache validators, and backoff hints.
    headers.append("Access-Control-Expose-Headers", joinNames(EXPOSED_RESPONSE_HEADERS(), ","));
    headers.append("Access-Control-Max-Age", "86400");
    if (rt::truthy(credentialsOrigin)) {
      headers.append("Access-Control-Allow-Credentials", "true");
      headers.append("Vary", "Origin");
    }
    return headers;
  }

  rt::Response withCors(rt::Response response, std::optional<rt::Request> req) {
    rt::Headers headers(response.headers());
    if (isAdminPath(req)) headers.set("Cache-Control", "no-store");
    headers.set("x-uos-git-sha", config::runtimeGitSha());
    headers.set("x-uos-deployment-id", config::runtimeDeploymentId());
    if (!headers.has("x-uos-request-id")) headers.set("x-uos-request-id", rt::crypto::randomUUID());
    for (const auto& [key, value] : corsHeaders(req).entries()) {
      if (rt::toLowerCase(key) != "vary") {
        headers.set(key, value);
        continue;
      }
      rt::OrderedMap<std::string, std::string> varyTokens;
      for (const std::string& token : rt::split(headers.get("Vary").value_or("") + "," + value, ",")) {
        const std::string normalized = rt::trim(token);
        if (!normalized.empty()) varyTokens.set(rt::toLowerCase(normalized), normalized);
      }
      std::string joined;
      bool firstToken = true;
      for (const std::string& token : varyTokens.values()) {
        if (!firstToken) joined += ", ";
        joined += token;
        firstToken = false;
      }
      headers.set("Vary", varyTokens.has(std::string("*")) ? std::string("*") : joined);
    }
    return rt::Response(rt::bodyInit(response.body()), rt::ResponseInit{.status = response.status(), .statusText = response.statusText(), .headers = headers});
  }

  rt::Response withoutBody(rt::Response response) {
    return rt::Response(nullptr, rt::ResponseInit{.status = response.status(), .statusText = response.statusText(), .headers = response.headers()});
  }

  rt::Response json(double status, const ::uos::json::value& body, rt::Headers extraHeaders) {
    rt::Headers headers(extraHeaders);
    // extraHeaders wins over the JSON default, matching the previous object spread.
    if (!headers.has("Content-Type")) headers.set("Content-Type", "application/json");
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), rt::ResponseInit{.status = status, .statusText = std::nullopt, .headers = headers});
  }

  rt::Response openaiError(double status, std::string message, std::optional<std::string> code, OpenaiErrorOptions options) {
    std::string type = rt::trim(options.type.value_or("invalid_request_error"));
    if (type.empty()) type = "invalid_request_error";
    ::uos::json::object error;
    rt::objectDefine(error, "message", ::uos::json::string(message));
    rt::objectDefine(error, "type", ::uos::json::string(type));
    if (code) rt::objectDefine(error, "code", ::uos::json::string(*code));
    if (options.param) rt::objectSet(error, "param", *options.param ? ::uos::json::value(::uos::json::string(**options.param)) : ::uos::json::value(nullptr));
    ::uos::json::object body;
    rt::objectDefine(body, "error", std::move(error));
    return json(status, ::uos::json::value(std::move(body)), options.headers.value_or(rt::Headers()));
  }

  rt::Response notFound() {
    ::uos::json::object error;
    rt::objectDefine(error, "message", ::uos::json::string("Not found"));
    rt::objectDefine(error, "type", ::uos::json::string("not_found"));
    rt::objectDefine(error, "code", ::uos::json::string("not_found"));
    ::uos::json::object body;
    rt::objectDefine(body, "error", std::move(error));
    return json(404, ::uos::json::value(std::move(body)));
  }

  std::optional<std::string> getBearerToken(const rt::Request& req) {
    const std::optional<std::string> value = req.headers().get("Authorization");
    if (!rt::truthy(value)) return std::nullopt;
    // Equivalent to /^Bearer\s+(.+)$/i but linear: the original had overlapping
    // `\s+`/`.+` quantifiers, so an attacker-supplied value such as
    // "Bearer <N spaces><M chars>\u2028" forced O(N*M) backtracking on every request.
    // Because `\s+` is greedy, the captured token always starts with a
    // non-whitespace character, which makes `\S` an exact, ambiguity-free prefix.
    static const rt::Regex BEARER("^Bearer\\s+(\\S[^\\n\\r\\u2028\\u2029]*)$", "i");
    const std::optional<rt::RegexMatch> match = BEARER.exec(*value);
    const std::string token = match && (*match)[1] ? rt::trim(*(*match)[1]) : std::string();
    return token.empty() ? std::nullopt : std::optional<std::string>(token);
  }
}
