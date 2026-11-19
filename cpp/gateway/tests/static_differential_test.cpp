// Differential cases for src/static.ts beyond tests/static-assets.test.ts: homepage content negotiation over
// Accept headers that exercise quality parsing, wildcards and tie-breaks, and the exact status, headers and body
// bytes (SHA-256) of every asset kind. Recorded from Bun 1.4.0 with a clean environment (not a deploy, no
// UOS_AI_TOKEN) and no KV, which this test reproduces through the config object before the module first loads.
// The admin HTML payload uses the current native UI length and SHA-256, with the recorded Bun status and headers.
// Links: tests/static_differential_test.cpp src/static.cpp src/http.cpp src/kv.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/auth/relay.cpp src/provider/cerebras_rate_limits.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/static.hpp"
#include "runtime/paths.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>

#include "tests/static_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;

  const std::string ROOT = rt::path::resolve(rt::path::dirname(__FILE__), "../../..");

  const json::value& vectors() {
    static const json::value parsed = [] {
      std::string text;
      for (const char* part : STATIC_VECTOR_PARTS) text += part;
      return rt::jsonParse(text);
    }();
    return parsed;
  }

  rt::Task<json::value> record(std::optional<rt::Response> response) {
    if (!response) co_return json::value(nullptr);
    json::array headers;
    for (const auto& [name, value] : response->headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    auto reading = response->arrayBuffer();
    const rt::Bytes bytes = co_await std::move(reading);
    json::object out;
    out["status"] = response->status();
    out["headers"] = std::move(headers);
    out["bytes"] = bytes.size();
    out["sha256"] = rt::toHex(rt::crypto::subtle::digest("SHA-256", bytes));
    co_return rt::canonicalNumbers(json::value(std::move(out)));
  }

  void expectSame(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(rt::canonicalNumbers(actual), rt::canonicalNumbers(expected))) {
      rt::test::report(file, line, label + ": " + rt::jsonStringify(actual) + " != " + rt::jsonStringify(expected), false);
    }
  }
}

ASYNC_TEST("homepage negotiation and asset responses match Bun") {
  uos::runtime::paths::setResourceRoot(ROOT);
  const json::value& data = vectors();
  // The recording environment: not a deploy, no client tokens, no KV.
  uos::config::config.isDeploy = rt::get(data, "isDeploy")->as_bool();
  uos::config::config.authTokens.clear();
  EXPECT_EQ(*rt::getNumber(data, "authTokens"), 0);
  uos::kv::setKvForTest(nullptr);

  for (const json::value& item : rt::get(data, "root")->as_array()) {
    rt::RequestInit init;
    std::string url = "https://ai.ubq.fi/";
    std::string label;
    if (const json::value* path = rt::get(item, "path")) {
      url = "https://ai.ubq.fi" + std::string(path->as_string());
      init.headers = rt::Headers{{"Accept", "application/json"}};
      label = "path " + std::string(path->as_string());
    } else if (const json::value* accept = rt::get(item, "accept"); accept && accept->is_string()) {
      init.headers = rt::Headers{{"Accept", std::string(accept->as_string())}};
      label = "accept " + rt::jsonStringify(*accept);
    } else {
      label = "no accept";
    }
    const rt::Request request(url, std::move(init));
    auto handling = uos::static_::handleRoot(request);
    const rt::Response response = co_await std::move(handling);
    auto recording = record(response);
    const json::value actual = co_await std::move(recording);
    expectSame(actual, *rt::get(item, "response"), label, __FILE__, __LINE__);
  }

  for (const json::value& item : rt::get(data, "assets")->as_array()) {
    const std::string path(rt::get(item, "path")->as_string());
    EXPECT_EQ(uos::static_::hasStaticAsset(path), rt::get(item, "has")->as_bool());
    auto serving = uos::static_::handleStaticAsset(path);
    const std::optional<rt::Response> response = co_await std::move(serving);
    auto recording = record(response);
    const json::value actual = co_await std::move(recording);
    expectSame(actual, *rt::get(item, "response"), "asset " + path, __FILE__, __LINE__);
  }
  uos::kv::setKvForTest(nullptr);
}
