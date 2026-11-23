// Differential tests for src/http.ts (its bun tests need unported modules: the
// handler, images.ts, static.ts and the HTTP server). Expected values were
// recorded by running the TS module under Bun 1.4.0 on the same inputs; the
// x-uos-git-sha and x-uos-deployment-id values are compared with this build's own
// runtimeGitSha()/runtimeDeploymentId(), which Bun reported as "unknown" too.
// Links: src/http.cpp src/auth/relay.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp
//        src/release.cpp
#include "uos/http.hpp"

#include "uos/config.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace http = uos::http;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(
#include "http_differential_vectors.inc"
    );
    return parsed;
  }
  const json::value& expected(std::string_view key) { return *rt::get(vectors(), key); }

  json::value entriesOf(const rt::Headers& headers) {
    json::array entries;
    for (const auto& [name, value] : headers.entries()) entries.push_back(json::array{json::string(name), json::string(value)});
    return entries;
  }

  rt::Request request(std::string url, std::vector<rt::Headers::Entry> headers = {}) {
    rt::RequestInit init;
    init.headers = rt::Headers(headers);
    return rt::Request(url, std::move(init));
  }

  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(json::string(*value)) : json::value(nullptr); }

  // `describe(response)` of the Bun script.
  rt::Task<json::value> describe(rt::Response response) {
    json::object out;
    out["status"] = response.status();
    out["statusText"] = response.statusText();
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) {
      if (name == "x-uos-request-id" || name == "x-uos-git-sha" || name == "x-uos-deployment-id") continue;
      headers.push_back(json::array{json::string(name), json::string(value)});
    }
    out["headers"] = std::move(headers);
    out["gitSha"] = orNull(response.headers().get("x-uos-git-sha"));
    out["deploymentId"] = orNull(response.headers().get("x-uos-deployment-id"));
    out["hasRequestId"] = response.headers().has("x-uos-request-id");
    if (response.body()) {
      const std::string body = co_await response.text();
      out["body"] = body;
    } else {
      out["body"] = nullptr;
    }
    co_return json::value(std::move(out));
  }

  // The recorded description with this build's git sha and deployment id.
  json::value withRuntimeIdentity(json::value recorded) {
    json::object& object = recorded.as_object();
    if (!object.at("gitSha").is_null()) object["gitSha"] = uos::config::runtimeGitSha();
    if (!object.at("deploymentId").is_null()) object["deploymentId"] = uos::config::runtimeDeploymentId();
    return recorded;
  }

  bool same(const json::value& actual, const json::value& wanted) {
    const bool equal = rt::deepEqual(actual, wanted);
    if (!equal) std::fprintf(stderr, "    actual:   %s\n    expected: %s\n", rt::jsonStringify(actual).c_str(), rt::jsonStringify(wanted).c_str());
    return equal;
  }

  rt::Task<void> expectDescribed(rt::Response response, const char* key) {
    const json::value actual = co_await describe(response);
    EXPECT(same(actual, withRuntimeIdentity(expected(key))));
  }
}

TEST("STANDARD_RATE_LIMIT_HEADERS matches the TS constant") {
  json::array names;
  for (std::string_view name : http::STANDARD_RATE_LIMIT_HEADERS) names.push_back(json::string(name));
  EXPECT(same(json::value(names), expected("standard")));
}

TEST("corsHeaders matches Bun for request origins, cors_origin queries and the configured origin") {
  EXPECT(same(entriesOf(http::corsHeaders()), expected("corsNone")));
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/v1/models", {{"origin", "https://ai-ubq-fi.deno.dev"}}))), expected("corsTrustedHeader")));
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/v1/models", {{"origin", "https://evil.example"}}))), expected("corsUntrustedHeader")));
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/v1/models?cors_origin=https://ai.ubq.fi"))), expected("corsQuery")));
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/v1/models?cors_origin=http://ai.ubq.fi"))), expected("corsQueryUntrusted")));
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/v1/models?cors_origin=https://ai.ubq.fi", {{"origin", ""}}))), expected("corsEmptyHeaderOrigin")));
  const std::string saved = uos::config::config.allowOrigin;
  uos::config::config.allowOrigin = "https://ai.ubq.fi";
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/x", {{"origin", "https://ai.ubq.fi"}}))), expected("corsConfiguredMatch")));
  EXPECT(same(entriesOf(http::corsHeaders(request("https://ai.ubq.fi/x", {{"origin", "https://ai-ubq-fi.deno.dev"}}))), expected("corsConfiguredMismatch")));
  uos::config::config.allowOrigin = saved;
}

ASYNC_TEST("withCors matches Bun for admin paths, request ids and Vary merging") {
  const rt::Response base(rt::BodyInit(std::string("payload")),
                          rt::ResponseInit{.status = 201, .statusText = "Created", .headers = rt::Headers{{"Vary", "Accept-Encoding, origin"}, {"x-uos-request-id", "fixed-id"}, {"X-Extra", "1"}}});
  const rt::Response trusted = http::withCors(base, request("https://ai.ubq.fi/admin/keys", {{"origin", "https://ai.ubq.fi"}}));
  co_await expectDescribed(trusted, "withCorsTrusted");
  const rt::Response fixedId(rt::BodyInit(std::string("x")), rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"x-uos-request-id", "fixed-id"}}});
  EXPECT(same(orNull(http::withCors(fixedId).headers().get("x-uos-request-id")), expected("withCorsTrustedRequestId")));
  const rt::Response noContent(nullptr, rt::ResponseInit{.status = 204, .statusText = std::nullopt, .headers = std::nullopt});
  const rt::Response plain = http::withCors(noContent);
  EXPECT(plain.headers().get("x-uos-request-id").has_value());
  EXPECT_EQ(plain.headers().get("x-uos-request-id")->size(), std::size_t{36});
  co_await expectDescribed(plain, "withCorsPlain");
  const rt::Response a(rt::BodyInit(std::string("a")));
  EXPECT(same(orNull(http::withCors(a, request("https://ai.ubq.fi/admin.html")).headers().get("cache-control")), expected("withCorsAdminHtml")));
  EXPECT(same(orNull(http::withCors(a, request("https://ai.ubq.fi/admin")).headers().get("cache-control")), expected("withCorsAdminRoot")));
  EXPECT(same(orNull(http::withCors(a, request("https://ai.ubq.fi/administrator")).headers().get("cache-control")), expected("withCorsNotAdmin")));
  const rt::Response varyStar(rt::BodyInit(std::string("v")), rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"Vary", "*"}}});
  const rt::Response star = http::withCors(varyStar, request("https://ai.ubq.fi/x", {{"origin", "https://ai.ubq.fi"}}));
  co_await expectDescribed(star, "withCorsVaryStar");
  const rt::Response varyDupes(rt::BodyInit(std::string("v")),
                               rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"Vary", " ORIGIN ,, accept , Origin"}}});
  const rt::Response dupes = http::withCors(varyDupes, request("https://ai.ubq.fi/x", {{"origin", "https://ai.ubq.fi"}}));
  co_await expectDescribed(dupes, "withCorsVaryDupes");
}

ASYNC_TEST("withoutBody, json, openaiError and notFound match Bun") {
  const rt::Response gone(rt::BodyInit(std::string("gone")), rt::ResponseInit{.status = 404, .statusText = "Nope", .headers = rt::Headers{{"x-a", "1"}}});
  const rt::Response withoutBody = http::withoutBody(gone);
  co_await expectDescribed(withoutBody, "withoutBody");
  const rt::Response json1 = http::json(200, rt::jsonParse(R"({"a":1,"b":[true,null,"x"]})"));
  co_await expectDescribed(json1, "json1");
  const rt::Response json2 = http::json(418, json::value("teapot"), rt::Headers{{"content-type", "text/plain"}, {"x-b", "2"}});
  co_await expectDescribed(json2, "json2");
  json::object third;
  third["n"] = 1.5e300;
  third["s"] = "\xE2\x80\xA8";
  const rt::Response json3 = http::json(200, json::value(third), rt::Headers{{"X-C", "3"}});
  co_await expectDescribed(json3, "json3");

  const rt::Response err1 = http::openaiError(400, "Bad thing");
  co_await expectDescribed(err1, "err1");
  http::OpenaiErrorOptions rateLimited;
  rateLimited.type = "  rate_limit_error  ";
  rateLimited.param = std::optional<std::string>(std::nullopt);
  rateLimited.headers = rt::Headers{{"Retry-After", "5"}};
  const rt::Response err2 = http::openaiError(429, "Slow down", std::string("rate_limited"), std::move(rateLimited));
  co_await expectDescribed(err2, "err2");
  http::OpenaiErrorOptions missing;
  missing.param = std::optional<std::string>("input");
  const rt::Response err3 = http::openaiError(400, "Missing", std::string("missing_param"), std::move(missing));
  co_await expectDescribed(err3, "err3");
  // `{ param: undefined }`: an own property whose value is undefined.
  http::OpenaiErrorOptions undefinedParam;
  undefinedParam.param = std::optional<std::string>(std::nullopt);
  const rt::Response err4 = http::openaiError(400, "Undefined param", std::string("x"), std::move(undefinedParam));
  co_await expectDescribed(err4, "err4");
  http::OpenaiErrorOptions blankType;
  blankType.type = "   ";
  const rt::Response err5 = http::openaiError(500, "Blank type", std::nullopt, std::move(blankType));
  co_await expectDescribed(err5, "err5");
  http::OpenaiErrorOptions emptyType;
  emptyType.type = "";
  const rt::Response err6 = http::openaiError(500, "Empty type", std::string("c"), std::move(emptyType));
  co_await expectDescribed(err6, "err6");
  const rt::Response notFound = http::notFound();
  co_await expectDescribed(notFound, "notFound");
}

TEST("getBearerToken matches Bun") {
  for (const json::value& row : expected("bearer").as_array()) {
    const json::value& input = row.as_array().at(0);
    const json::value& output = row.as_array().at(1);
    const rt::Request req = input.is_null() ? request("https://ai.ubq.fi/") : request("https://ai.ubq.fi/", {{"Authorization", std::string(input.get_string())}});
    EXPECT(same(orNull(http::getBearerToken(req)), output));
  }
}
