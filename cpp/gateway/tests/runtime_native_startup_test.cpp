// Links the sources of tests/serve_test.cpp but tests/helpers/counting_kv.cpp.
#include "uos/kv.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/serve.hpp"

namespace rt = uos::rt;

ASYNC_TEST("the native listener requires an initialized store and leaves default state untouched") {
  uos::kv::resetKvForTest();
  const auto reset = rt::scopeExit([] { uos::kv::resetKvForTest(); });
  ASSERT(!rt::fs::existsSync(".data"));
  const uos::serve::ServeStartupOptions options{"127.0.0.1", 0};
  auto starting = uos::serve::importMetaMain(options);
  EXPECT_REJECTS_MESSAGE(std::move(starting), rt::JsError, "Native KV must be initialized before starting the gateway");
  EXPECT(!rt::fs::existsSync(".data"));
}
