// Differential cases for src/runtime/http-early-sse.ts (its bun tests need serveHttp from
// src/runtime/http.ts and src/responses-native-headers.ts): the expected values were recorded
// from Bun 1.4.0 running the TypeScript module on the same calls.
#include "uos/runtime/http_early_sse.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/test.hpp"
#include <memory>

namespace {
  using namespace uos::runtime::http_early_sse;
  namespace rt = uos::rt;

  EarlySseHeaders contentType(std::string value) { return EarlySseHeaders{.headers = rt::Headers{{"content-type", std::move(value)}}}; }
}

TEST("registration, commit, idempotent success and removal") {
  const rt::Request request("http://localhost/x");
  const rt::Request other("http://localhost/x");
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{}), false);
  auto calls = std::make_shared<std::vector<std::string>>();
  auto answer = std::make_shared<bool>(false);
  const std::function<void()> unregister = registerEarlySseHeaders(request, [calls, answer](const EarlySseHeaders& value) {
    calls->push_back(value.headers.get("content-type").value_or("none"));
    return *answer;
  });
  EXPECT_EQ(tryCommitEarlySseHeaders(request, contentType("a")), false);
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  *answer = true;
  EXPECT_EQ(tryCommitEarlySseHeaders(request, contentType("b")), true);
  EXPECT_EQ(tryCommitEarlySseHeaders(request, contentType("c")), true);
  EXPECT_EQ(*calls, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(earlySseHeadersCommitted(request), true);
  EXPECT_EQ(earlySseHeadersCommitted(other), false);
  unregister();
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  unregister();
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
}

TEST("a throwing commit is contained, and a later registration replaces the entry") {
  const rt::Request request("http://localhost/x");
  const std::function<void()> throwing = registerEarlySseHeaders(request, [](const EarlySseHeaders&) -> bool { throw rt::JsError("boom"); });
  EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{}), false);
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  const std::function<void()> replaced = registerEarlySseHeaders(request, [](const EarlySseHeaders&) { return true; });
  EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{}), true);
  // The first registration's remover deletes whatever entry the request has now.
  throwing();
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  replaced();
}

TEST("a commit that re-registers or unregisters its request still reports its own entry") {
  const rt::Request request("http://localhost/x");
  auto inner = std::make_shared<std::function<void()>>();
  const std::function<void()> outer = registerEarlySseHeaders(request, [request, inner](const EarlySseHeaders&) {
    *inner = registerEarlySseHeaders(request, [](const EarlySseHeaders&) { return false; });
    return true;
  });
  EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{}), true);
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  outer();
  auto selfRemove = std::make_shared<std::function<void()>>();
  *selfRemove = registerEarlySseHeaders(request, [selfRemove](const EarlySseHeaders&) {
    (*selfRemove)();
    return true;
  });
  EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{}), true);
  EXPECT_EQ(earlySseHeadersCommitted(request), false);
  *selfRemove = nullptr;
}
