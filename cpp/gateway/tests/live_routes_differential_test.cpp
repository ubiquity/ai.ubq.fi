// Differential cases for src/live/routes.ts (no direct TS tests): every expected value was
// recorded from Bun 1.4.0 running the TypeScript module.
#include "uos/live/routes.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"
#include <cstdio>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::live::routes;

  const char* const VECTORS = R"VEC([["POST","/v1/live",{"kind":"call"}],["GET","/v1/live",null],["post","/v1/live",null],["POST","/v1/live/",null],["POST","/v1/live/abc",null],["GET","/v1/live/abc",{"kind":"sideband","callId":"abc"}],["GET","/v1/live/rtc_123-ABC",{"kind":"sideband","callId":"rtc_123-ABC"}],["GET","/v1/live/a/b",null],["GET","/v1/live/",null],["GET","/v1/live/.",null],["GET","/v1/live/..",null],["GET","/v1/live/%2E",null],["GET","/v1/live/%2e%2E",null],["GET","/v1/live/%2F",{"kind":"sideband","callId":"/"}],["GET","/v1/live/a%20b",{"kind":"sideband","callId":"a b"}],["GET","/v1/live/%E2%82%AC",{"kind":"sideband","callId":"€"}],["GET","/v1/live/%",null],["GET","/v1/live/%zz",null],["GET","/v1/live/%C3",null],["GET","/v1/live/%ED%A0%80",null],["GET","/v1/live/line\nbreak",{"kind":"sideband","callId":"line\nbreak"}],["GET","/v1/live/x\n",{"kind":"sideband","callId":"x\n"}],["GET","/V1/live/x",null],["GET","/v1/livex",null],["HEAD","/v1/live/x",null],["GET","/v1/live/😀",{"kind":"sideband","callId":"😀"}],["DELETE","/v1/live/x",null],["GET","x/v1/live/x",null],["GET","/v1/live/x?y",{"kind":"sideband","callId":"x?y"}],["GET","/v1/live/%00",{"kind":"sideband","callId":"\u0000"}]])VEC";

  json::value toJson(const std::optional<LiveRoute>& route) {
    if (!route) return nullptr;
    json::object out;
    if (std::holds_alternative<LiveRouteCall>(*route)) {
      out["kind"] = json::string(LiveRouteCall::kind);
    } else {
      out["kind"] = json::string(LiveRouteSideband::kind);
      out["callId"] = std::get<LiveRouteSideband>(*route).callId;
    }
    return out;
  }
}

TEST("differential: liveRouteForRequest matches Bun") {
  const json::value vectors = rt::jsonParse(VECTORS);
  for (const json::value& row : vectors.as_array()) {
    const json::array& vector = row.as_array();
    const std::string method(vector[0].as_string());
    const std::string path(vector[1].as_string());
    const json::value actual = toJson(liveRouteForRequest(method, path));
    if (!rt::deepEqual(actual, vector[2])) {
      EXPECT_EQ(actual, vector[2]);
      std::fprintf(stderr, "      %s %s\n", method.c_str(), rt::jsonQuote(path).c_str());
    }
  }
}
